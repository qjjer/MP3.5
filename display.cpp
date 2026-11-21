/*
 * display.cpp — ST7789 显示抽象层实现(支持 TFT_eSPI / Adafruit 两个后端)
 *
 * 【SPI 总线共用与初始化顺序 —— 本项目最容易踩的坑】
 *
 * TFT_eSPI 在 ESP32-S3 上:
 *   #define SPI_PORT 2
 *   volatile uint32_t* _spi_cmd = (volatile uint32_t*)(SPI_CMD_REG(SPI_PORT));
 * 它推像素时直接写 SPI2 的寄存器, 不走 SPIClass 的 beginTransaction(),
 * 所以 core 里那把总线锁保护不了 TFT 传输。SD 卡(SD 库)走的是 SPIClass,
 * 两者物理上是同一个 SPI2 外设 —— 由 sd_mutex 统一串行化, 别绕过去。
 *
 * 总线初始化只做一次, 由 displayInit() 里的第一个 SPI.begin() 完成:
 *   - 先 SPI.begin(SCK, MISO, MOSI, -1)  -> 抢在 TFT_eSPI 之前建立总线
 *   - 再 tft.init()                       -> TFT_eSPI 只会改自己的寄存器配置
 *   - 最后 SD.begin(CS, SPI, freq)        -> SPIClass::begin() 是幂等的,
 *                                            总线已存在时它直接 return, 不会重置引脚
 * 这样任何一方都不会"顺手重置"另一方的总线配置。
 */
#include "display.h"
#include "bus_init.h"
#include "font_builtin.h"
#include "config.h"
#include "sd_mutex.h"
#include "sd_bench.h"

#include <SPI.h>

#if DISPLAY_BACKEND == 1
#include <TFT_eSPI.h>
#elif DISPLAY_BACKEND == 2
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#else
#error "config.h: DISPLAY_BACKEND 只能是 1(TFT_eSPI) 或 2(Adafruit_ST7789)"
#endif

// ---------------------------------------------------------------------------
// 画布基础绘制(纯内存操作, 不碰 SPI, 不需要锁)
// ---------------------------------------------------------------------------
void Canvas16::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int16_t)w_) {
    w = w_ - x;
  }
  if (y + h > (int16_t)h_) {
    h = h_ - y;
  }
  if (w <= 0 || h <= 0) {
    return;
  }
  for (int16_t row = 0; row < h; row++) {
    uint16_t *p = buf_ + (uint32_t)(y + row) * w_ + x;
    for (int16_t i = 0; i < w; i++) {
      p[i] = c;
    }
  }
}

void Canvas16::hLine(int16_t x, int16_t y, int16_t w, uint16_t c) {
  fillRect(x, y, w, 1, c);
}

void Canvas16::vLine(int16_t x, int16_t y, int16_t h, uint16_t c) {
  fillRect(x, y, 1, h, c);
}

void Canvas16::rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  hLine(x, y, w, c);
  hLine(x, y + h - 1, w, c);
  vLine(x, y, h, c);
  vLine(x + w - 1, y, h, c);
}

// ---------------------------------------------------------------------------
// ★ 性能测量(task-6): 先量, 再改 ★
//
// 为什么必须先量: 之前已经试过 6 种"看起来应该有效"的办法(改喂数据频率、改任务核、
// 升 SPI 频率、局部刷状态栏、改输入任务核)全部无效 —— 说明"时间花在哪"一直没人测过。
// 这里把每一段都计时, 每秒汇总打印一行, 后面所有改动都以这些数字为依据。
// ---------------------------------------------------------------------------
struct PerfStats {
  uint32_t lastReportMs;
  // ---- 界面侧(ui.cpp 通过 displayPerfNoteUi 喂进来) ----
  uint32_t uiFullN, uiFullUs, uiFullMaxUs;   // 整屏渲染(uiRender)
  uint32_t uiPartN, uiPartUs, uiPartMaxUs;   // 局部渲染(uiRenderStatusBarOnly)
  uint32_t uiDrawUs;                         // 绘制部分累计(含读 SD 字形)
  uint32_t uiDrawLockWaitUs;                 // 绘制前等 sd_mutex 的累计时间
  uint32_t uiDrawLockTimeoutN;
  uint32_t glyphHit, glyphMiss;              // 字形缓存命中/未命中(未命中=一次 SD 随机读)
  // ---- 刷屏侧(display.cpp 自己统计) ----
  uint32_t fullN, fullUs, fullMaxUs, fullBytes;   // 整帧推送
  uint32_t partN, partUs, partMaxUs, partBytes;   // 局部推送
  uint32_t partBands, partNoopN;                  // 行段数 / 一个字节都没发的次数
  uint32_t diffUs;                                // 逐行比对本身花掉的时间
  uint32_t flushLockWaitUs;                       // 刷屏前等 sd_mutex 的累计时间
  uint32_t flushLockTimeoutN;
};
static PerfStats s_perf = {};

// 影子缓冲 = "上一次真正推到屏幕上的像素"(见 display.h 的脏区域刷新说明)。
// 只有它和帧缓冲都算对了, 差分才敢跳过某一行。
static uint16_t *s_shadow = nullptr;
static bool s_shadowValid = false;

void displayDirtyInvalidate(void) {
  s_shadowValid = false;
}

// 上一次刷屏的明细(给 ui.cpp 打 "[perf-slow] 这一帧为什么慢")
static uint32_t s_lastFlushUs = 0;
static uint32_t s_lastFlushBytes = 0;
static bool s_lastFlushFull = false;
static uint32_t s_lastFlushBands = 0;

static void perfNoteFlush(uint32_t us, uint32_t bytes, bool full, uint32_t bands) {
  s_lastFlushUs = us;
  s_lastFlushBytes = bytes;
  s_lastFlushFull = full;
  s_lastFlushBands = bands;
}

void displayLastFlushInfo(uint32_t *outUs, uint32_t *outBytes, bool *wasFull, uint32_t *outBands) {
  if (outUs) {
    *outUs = s_lastFlushUs;
  }
  if (outBytes) {
    *outBytes = s_lastFlushBytes;
  }
  if (wasFull) {
    *wasFull = s_lastFlushFull;
  }
  if (outBands) {
    *outBands = s_lastFlushBands;
  }
}

static void perfResetWindow(void) {
  const uint32_t keep = s_perf.lastReportMs;
  s_perf = PerfStats();
  s_perf.lastReportMs = keep;
}

// 微秒 -> 毫秒(带一位小数, 不依赖浮点 printf 的 %f 支持)
static uint32_t us2msX10(uint32_t us) {
  return (us + 50) / 100;   // 0.1ms 为单位
}

static uint32_t perCallX10(uint32_t total, uint32_t n) {
  if (n == 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)total * 10 + n / 2) / n);
}

// 每秒打印一次。由 uiRender/uiRenderStatusBarOnly 在**锁外**调用。
// 注意: 串口 115200 下这两行约 220 字节 ≈ 19ms/秒的发送时间, 是测量本身的成本。
// 不想让它干扰手感时把下面这个改成 0 重新编译即可(测量点仍然全部保留在计数里)。
#define PERF_LOG_ENABLE 1

void displayPerfTick(void) {
#if !PERF_LOG_ENABLE
  return;
#else
  const uint32_t now = millis();
  if (s_perf.lastReportMs == 0) {
    s_perf.lastReportMs = (now == 0) ? 1 : now;
    return;
  }
  const uint32_t dtMs = now - s_perf.lastReportMs;
  if (dtMs < 1000) {
    return;
  }
  const uint32_t dtSecX10 = dtMs / 100;   // 0.1s 为单位, 用来把"每秒"算准

  const PerfStats p = s_perf;   // 打印期间不要再累加
  perfResetWindow();

  const uint32_t uiN = p.uiFullN + p.uiPartN;
  const uint32_t missPerX10 = perCallX10(p.glyphMiss, uiN);
  const uint32_t missPerSecX10 = dtSecX10 ? (p.glyphMiss * 10 / dtSecX10) : 0;

  // ---- 第 1 行: 界面侧(渲染次数 / 绘制 / 等锁 / 字形) ----
  Serial.printf("[perf] %lu.%lus ui: full=%lu avg=%lu.%lu max=%lu.%lums | part=%lu avg=%lu.%lums | draw=%lu.%lums/r miss=%lu.%lu/r wait=%lu.%lums/r to=%lu | glyph hit=%lu miss=%lu %lu.%lu/s\n",
                (unsigned long)(dtMs / 1000), (unsigned long)((dtMs % 1000) / 100), (unsigned long)p.uiFullN,
                (unsigned long)(perCallX10(p.uiFullUs, p.uiFullN) / 10),
                (unsigned long)(perCallX10(p.uiFullUs, p.uiFullN) % 10), (unsigned long)(us2msX10(p.uiFullMaxUs) / 10),
                (unsigned long)(us2msX10(p.uiFullMaxUs) % 10), (unsigned long)p.uiPartN,
                (unsigned long)(perCallX10(p.uiPartUs, p.uiPartN) / 10),
                (unsigned long)(perCallX10(p.uiPartUs, p.uiPartN) % 10),
                (unsigned long)(perCallX10(p.uiDrawUs, uiN) / 10), (unsigned long)(perCallX10(p.uiDrawUs, uiN) % 10),
                (unsigned long)(missPerX10 / 10), (unsigned long)(missPerX10 % 10),
                (unsigned long)(perCallX10(p.uiDrawLockWaitUs, uiN) / 10),
                (unsigned long)(perCallX10(p.uiDrawLockWaitUs, uiN) % 10), (unsigned long)p.uiDrawLockTimeoutN,
                (unsigned long)p.glyphHit, (unsigned long)p.glyphMiss, (unsigned long)(missPerSecX10 / 10),
                (unsigned long)(missPerSecX10 % 10));

  // ---- 第 2 行: 刷屏侧(整帧/局部/实际发出的字节/比对/等锁) ----
  const uint32_t fullBwKBs =
      (p.fullUs && p.fullBytes) ? (uint32_t)((uint64_t)p.fullBytes * 1000 / p.fullUs) : 0;   // 字节/ms = KB/s
  const uint32_t flushN = p.fullN + p.partN;
  Serial.printf("[perf]      flush: full=%lu avg=%lu.%lums %luKB/s | part=%lu avg=%lu.%lums sent=%luKB bands=%lu noop=%lu | diff=%lu.%lums wait=%lu.%lums to=%lu\n",
                (unsigned long)p.fullN, (unsigned long)(perCallX10(p.fullUs, p.fullN) / 10),
                (unsigned long)(perCallX10(p.fullUs, p.fullN) % 10), (unsigned long)fullBwKBs, (unsigned long)p.partN,
                (unsigned long)(perCallX10(p.partUs, p.partN) / 10), (unsigned long)(perCallX10(p.partUs, p.partN) % 10),
                (unsigned long)(p.partBytes / 1024), (unsigned long)p.partBands, (unsigned long)p.partNoopN,
                (unsigned long)(perCallX10(p.diffUs, p.partN) / 10), (unsigned long)(perCallX10(p.diffUs, p.partN) % 10),
                (unsigned long)(perCallX10(p.flushLockWaitUs, flushN) / 10),
                (unsigned long)(perCallX10(p.flushLockWaitUs, flushN) % 10), (unsigned long)p.flushLockTimeoutN);
#endif
}

void displayPerfNoteUi(uint32_t totalUs, uint32_t drawUs, uint32_t drawLockWaitUs, bool fullScreen,
                       uint32_t glyphHits, uint32_t glyphMisses, bool drawLockOk) {
  if (fullScreen) {
    s_perf.uiFullN++;
    s_perf.uiFullUs += totalUs;
    if (totalUs > s_perf.uiFullMaxUs) {
      s_perf.uiFullMaxUs = totalUs;
    }
  } else {
    s_perf.uiPartN++;
    s_perf.uiPartUs += totalUs;
    if (totalUs > s_perf.uiPartMaxUs) {
      s_perf.uiPartMaxUs = totalUs;
    }
  }
  s_perf.uiDrawUs += drawUs;
  s_perf.uiDrawLockWaitUs += drawLockWaitUs;
  s_perf.glyphHit += glyphHits;
  s_perf.glyphMiss += glyphMisses;
  if (!drawLockOk) {
    s_perf.uiDrawLockTimeoutN++;
  }
}

// ---------------------------------------------------------------------------
// 后端实现
// ---------------------------------------------------------------------------
#if DISPLAY_BACKEND == 1
// ============================ TFT_eSPI =====================================
static TFT_eSPI tft;
static bool s_sameBus = false;   // TFT_eSPI 是否用的是全局 SPI 对象
static SPIClass *s_spiUsed = nullptr;  // TFT_eSPI 实际使用的 SPI 实例(给 SD 卡共用)
static uint16_t *s_frameBuf = nullptr;
static Canvas16 *s_canvas = nullptr;

const char *displayBackendName(void) {
  return "TFT_eSPI";
}

// ---------------------------------------------------------------------------
// 上电基准测量(task-6): 把"老架构每次交互都要做的那几件事"各量一次
//
// 全部用同一份 COL_BG 填充的帧缓冲推送, 所以屏幕上**看不到任何变化**,
// 不会干扰启动界面。数字直接打进串口, 作为后续所有优化的对比基准。
// ---------------------------------------------------------------------------
static void displayBenchmark(void) {
  const int REPS = 3;
  uint32_t t, avg, best;

  Serial.printf("[perf] ===== 上电基准(帧缓冲 %u 字节, SPI=%u Hz, DMA=%s) =====\n",
                (unsigned)((size_t)TFT_W * TFT_H * 2), (unsigned)SPI_FREQ_TFT, tft.DMA_Enabled ? "on" : "off");

  // 1) 全屏内存填充(纯 PSRAM 写, 不碰 SPI) —— 每次整屏重绘都要做一次
  s_canvas->fill(COL_BG);
  t = micros();
  s_canvas->fill(COL_BG);
  t = micros() - t;
  Serial.printf("[perf] 全屏内存填充 %ux%u = %lu us (%lu.%lu ms)\n", (int)TFT_W, (int)TFT_H, (unsigned long)t,
                (unsigned long)(t / 1000), (unsigned long)((t % 1000) / 100));

  // 2) 整帧推送 —— 这就是老代码**每次翻页/转一格旋钮**都要付的钱
  avg = 0;
  best = 0xFFFFFFFFu;
  for (int i = 0; i < REPS; i++) {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    tft.startWrite();
    t = micros();
    tft.pushImage(0, 0, TFT_W, TFT_H, s_frameBuf);
    t = micros() - t;
    tft.endWrite();
    avg += t;
    if (t < best) {
      best = t;
    }
  }
  avg /= REPS;
  Serial.printf("[perf] 整帧推送 %dx%d (%u 字节) = %lu us (%lu.%lu ms), 最好 %lu us, 有效带宽 %lu KB/s\n", (int)TFT_W,
                (int)TFT_H, (unsigned)((size_t)TFT_W * TFT_H * 2), (unsigned long)avg, (unsigned long)(avg / 1000),
                (unsigned long)((avg % 1000) / 100), (unsigned long)best,
                (unsigned long)(avg ? ((size_t)TFT_W * TFT_H * 2) * 1000 / avg : 0));

  // 3) 各种局部推送(老代码里 displayFlushRect 走的就是这个)
  {
    struct {
      const char *name;
      int16_t y;
      int16_t h;
    } cases[] = {
        {"状态栏 240x20", (int16_t)(TFT_H - 20), 20},
        {"选中行 240x30", 80, 30},
        {"正文区 240x286", 2, 286},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      avg = 0;
      best = 0xFFFFFFFFu;
      for (int r = 0; r < REPS; r++) {
        SdLockGuard g(SD_LOCK_TIMEOUT_MS);
        tft.startWrite();
        t = micros();
        tft.pushImage(0, cases[i].y, TFT_W, cases[i].h, s_frameBuf + (size_t)cases[i].y * TFT_W);
        t = micros() - t;
        tft.endWrite();
        avg += t;
        if (t < best) {
          best = t;
        }
      }
      avg /= REPS;
      Serial.printf("[perf] 局部推送 %s (%u 字节) = %lu us (%lu.%lu ms)\n", cases[i].name,
                    (unsigned)((size_t)TFT_W * cases[i].h * 2), (unsigned long)avg, (unsigned long)(avg / 1000),
                    (unsigned long)((avg % 1000) / 100));
    }
  }
  Serial.println("[perf] =====================================================");
}

bool displayInit(void) {
  // 1) 先把共享总线立起来(幂等: 后面谁再调 begin() 都会直接 return)
  //    必须检查返回值: 失败时 SPIClass 内部句柄无效, 再让 TFT 去驱动就会崩。
  if (!busBeginShared()) {
    return false;
  }

  // 2) 检查 TFT_eSPI 用的是不是我们的全局 SPI 对象(即同一个硬件外设)
  {
    SPIClass &spiUsed = TFT_eSPI::getSPIinstance();
    s_spiUsed = &spiUsed;
    s_sameBus = (&spiUsed == &SPI);
    if (!s_sameBus) {
      log_e("========================================================");
      log_e("!! 严重配置问题: TFT_eSPI 用的不是全局 SPI 对象(你定义了 USE_HSPI_PORT?)");
      log_e("!! 说明 User_Setup.h / tft_setup.h 里定义了 USE_HSPI_PORT 或 USE_FSPI_PORT");
      log_e("!! 后果: 屏幕走 SPI3, SD 卡走 SPI2 -> 两者不共用总线, sd_mutex 形同虚设");
      log_e("!! 而且 ESP32-S3 的 SPI3 与 OPI PSRAM 复用, 屏幕可能完全不亮");
      log_e("!! 修复: 把 USE_HSPI_PORT / USE_FSPI_PORT 两行都注释掉, 重新编译");
      log_e("!! (本程序仍然继续初始化, 以便你至少能在串口看到这条信息)");
      log_e("========================================================");
    }
  }

  // 3) ST7789 初始化
  //
  // 注意: 这里**不做**"必须用全局 SPI 对象"的硬断言。
  // 如果 User_Setup.h / tft_setup.h 里定义了 USE_HSPI_PORT, TFT_eSPI 会自己创建
  // SPIClass(SPI3)(见 Processors/TFT_eSPI_ESP32.c 开头的分支), 此时它和 SD 用的
  // 全局 SPI(SPI2) 不是同一个外设 —— 但 S3 上 SPI3 是和 OPI PSRAM 复用的,
  // 所以那种配置下屏幕/SD 并存不可靠。我们打印明确告警, 但仍然继续初始化,
  // 绝不因为"配置不合我意"就让屏幕保持全黑。
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setSwapBytes(true);  // 帧缓冲是小端 RGB565, 需要交换字节
  tft.fillScreen(COL_BG);

#if DISPLAY_USE_DMA
  // DMA 会自己往 SPI 总线挂设备, 必须放在 tft.init()/SPI.begin() 之后。
  // 注意: 开了 DMA 之后不要用 cpu 直接写的 spi 事务去碰 TFT。
  // 注意: initDMA() 失败时 TFT_eSPI 内部可能是 ESP_ERROR_CHECK -> abort 重启,
  // 那种情况根本走不到这里的返回值判断。所以 config.h 里默认把 DMA 关掉。
  // 打开 DMA 的前提是: 确认不会 abort(见 config.h 的说明)。
  log_w("[display] 尝试 initDMA()... 若屏幕亮一下即灭, 请把 DISPLAY_USE_DMA 改回 0");
  if (!tft.initDMA(false)) {
    log_w("[display] initDMA 返回失败, 回退到普通刷屏(功能不受影响)");
  } else {
    log_i("[display] DMA 已启用");
  }
#endif

  // 4) 帧缓冲: 优先 PSRAM
  const size_t need = (size_t)TFT_W * TFT_H * 2;
  s_frameBuf = (uint16_t *)ps_malloc(need);
  if (!s_frameBuf) {
    s_frameBuf = (uint16_t *)malloc(need);
  }
  if (!s_frameBuf) {
    log_e("[display] 帧缓冲分配失败, 需要 %u 字节。请确认已开启 PSRAM", (unsigned)need);
    return false;
  }
  s_canvas = new Canvas16(TFT_W, TFT_H, s_frameBuf);
  s_canvas->fill(COL_BG);

  // 5) 影子缓冲(脏区域刷新用): 分配失败就退化回"每次都整帧推", 功能不受影响
  s_shadow = (uint16_t *)ps_malloc(need);
  if (!s_shadow) {
    s_shadow = (uint16_t *)malloc(need);
  }
  if (!s_shadow) {
    Serial.println(F("[perf] !! 影子缓冲分配失败: 脏区域刷新退化为整帧推送(功能正常, 但不会更快)"));
  } else if (s_frameBuf) {
    memcpy(s_shadow, s_frameBuf, need);
    s_shadowValid = true;
  }

  // 探针: 确认 use_dma 真的生效
  log_i("[display] TFT_eSPI ready %dx%d, SPI=%u Hz, DMA=%s, 共用总线=%s", (int)TFT_W, (int)TFT_H,
        (unsigned)SPI_FREQ_TFT, tft.DMA_Enabled ? "on" : "off", s_sameBus ? "yes" : "NO(见上方告警)");

  // 6) 上电基准(见 displayBenchmark 说明): 只做一次, 之后不再干扰运行
  displayBenchmark();

#if SD_BENCH_ENABLE
  // 7) SD 通路自检(一次性, 见 sd_bench.cpp): 量出 SD 各层吞吐/延迟, 定位 19 KB/s 的真因。
  //    它自己会挂载 SD、跑完再卸载, 所以随后 bootTryAdvance() 里的 mountSd() 照常工作。
  sdBenchRun();
#endif
  return true;
}

Canvas16 *displayCanvas(void) {
  return s_canvas;
}

// 把一块行段推到屏幕, 并把这段同步进影子缓冲。
// 调用者必须已经持有 sd_mutex 且已 startWrite()。
// 返回 true 表示真的发了数据。
static bool pushBand(int16_t y, int16_t h) {
  if (h <= 0) {
    return false;
  }
  const uint16_t *src = s_frameBuf + (size_t)y * TFT_W;
#if DISPLAY_USE_DMA
  if (tft.DMA_Enabled) {
    tft.pushImageDMA(0, y, TFT_W, h, (uint16_t *)src);
  } else {
    tft.pushImage(0, y, TFT_W, h, (uint16_t *)src);
  }
#else
  // 一次 setWindow + 一次连续推送: 等价于参考项目的
  //   tft.setAddrWindow(area->x1, area->y1, w, h); tft.pushColors(..., w*h, true);
  tft.pushImage(0, y, TFT_W, h, (uint16_t *)src);
#endif
  if (s_shadow) {
    memcpy(s_shadow + (size_t)y * TFT_W, src, (size_t)h * TFT_W * 2);
  }
  return true;
}

// 整帧推送(并同步整块影子缓冲)
static bool pushWholeFrame(void) {
  tft.startWrite();
  bool sent;
#if DISPLAY_USE_DMA
  if (tft.DMA_Enabled) {
    tft.pushImageDMA(0, 0, TFT_W, TFT_H, s_frameBuf);
    sent = true;
  } else {
    sent = pushBand(0, TFT_H);
  }
#else
  sent = pushBand(0, TFT_H);
#endif
  tft.endWrite();
  if (s_shadow) {
    memcpy(s_shadow, s_frameBuf, (size_t)TFT_W * TFT_H * 2);
    s_shadowValid = true;
  }
  (void)sent;
  return true;
}

bool displayFlush(void) {
  if (!s_canvas) {
    return false;
  }

#if DISPLAY_STRIP_MODE
  // 条带模式: 只把内部 RAM 当搬运缓冲, 逐行拷过去(慢, 但内存紧张时能用)
  static uint16_t *strip = nullptr;
  if (!strip) {
    strip = (uint16_t *)malloc((size_t)TFT_W * 2);
    if (!strip) {
      return false;
    }
  }
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    return false;
  }
  const uint32_t t0s = micros();
  tft.startWrite();
  for (int y = 0; y < TFT_H; y++) {
    memcpy(strip, s_frameBuf + (size_t)y * TFT_W, (size_t)TFT_W * 2);
    tft.setAddrWindow(0, y, TFT_W, 1);
    tft.pushPixels(strip, TFT_W);
  }
  tft.endWrite();
  s_shadowValid = false;   // 逐行拷的路径不走差分, 影子缓冲作废
  perfNoteFlush(micros() - t0s, (uint32_t)TFT_W * TFT_H * 2, true, TFT_H);
  return true;
#else
  // 整帧模式: 一次锁 + 一次传输, 音频任务等待时间可预测
  const uint32_t t0 = micros();
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  const uint32_t tLock = micros();
  s_perf.flushLockWaitUs += (tLock - t0);
  if (!g.ok()) {
    log_w("[display] 刷屏等锁超时, 跳过本帧");
    s_perf.flushLockTimeoutN++;
    return false;
  }
  pushWholeFrame();
#if SD_SPI_REPRIME
  // ★ 可选的"总线复位"★
  //   TFT_eSPI 是直接写 SPI 寄存器推像素的, 它可能把外设留在 40MHz 的状态,
  //   而 SD 驱动以为自己已经把时钟配置成 25MHz 了(于是不再重配) -> SD 实际跑错时序
  //   -> 卡重试 -> 慢 100 倍。这一句把外设拉回 SD 期望的时序。
  //   是否真的有效, 看 [sdbench] 的 G 组(无刷屏 / 刷屏后 / 刷屏+重设 三个吞吐对比)。
  displaySpiReprime();
#endif
  const uint32_t dt = micros() - t0;
  s_perf.fullN++;
  s_perf.fullUs += dt;
  s_perf.fullBytes += (uint32_t)TFT_W * TFT_H * 2;
  if (dt > s_perf.fullMaxUs) {
    s_perf.fullMaxUs = dt;
  }
  perfNoteFlush(dt, (uint32_t)TFT_W * TFT_H * 2, true, 1);
  return true;
#endif
}

void displayDrain(void) {
  if (tft.DMA_Enabled) {
    tft.dmaWait();
  }
  // 等最后一个字节真正出门, 之后再断电才不会影响 SD 卡
  delayMicroseconds(200);
}

// ---------------------------------------------------------------------------
// 脏区域刷新: 逐行比对 帧缓冲 vs 影子缓冲, 只推变了的行段
//
// 这才是"整屏重绘 + 整帧推送"和参考项目"只推脏区域"的差距所在:
//   整帧 153600 字节 -> 40MHz 下 30.7ms, 而且是 CPU 死等;
//   只推一条状态栏(20 行, 9600 字节) -> 约 2ms;
//   一个像素都没变 -> 0ms(一个字节都不发)。
// ---------------------------------------------------------------------------
bool displayFlushDirtyRange(int16_t y0, int16_t y1) {
  if (!s_canvas) {
    return false;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (y1 > TFT_H - 1) {
    y1 = (int16_t)(TFT_H - 1);
  }
  if (y0 > y1) {
    return false;
  }

  const uint32_t t0 = micros();
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  const uint32_t tLock = micros();
  s_perf.flushLockWaitUs += (tLock - t0);
  if (!g.ok()) {
    s_perf.flushLockTimeoutN++;
    return false;
  }

  // 影子缓冲不可信(首次 / 分配失败 / 条带模式) -> 整帧推送一次, 之后就能差分了
  if (!s_shadow || !s_shadowValid) {
    pushWholeFrame();
    const uint32_t dt = micros() - t0;
    s_perf.fullN++;
    s_perf.fullUs += dt;
    s_perf.fullBytes += (uint32_t)TFT_W * TFT_H * 2;
    if (dt > s_perf.fullMaxUs) {
      s_perf.fullMaxUs = dt;
    }
    perfNoteFlush(dt, (uint32_t)TFT_W * TFT_H * 2, true, 1);
    return true;
  }

  const uint32_t rowBytes = (uint32_t)TFT_W * 2;
  uint32_t pushedBytes = 0;
  uint32_t bands = 0;
  uint32_t diffUs = 0;      // 逐行比对本身的开销(和推送开销分开记, 好判断值不值)
  bool started = false;

  int16_t y = y0;
  while (y <= y1) {
    const uint32_t td0 = micros();
    // 跳过没变的行
    while (y <= y1 && memcmp(s_frameBuf + (size_t)y * TFT_W, s_shadow + (size_t)y * TFT_W, rowBytes) == 0) {
      y++;
    }
    if (y > y1) {
      diffUs += micros() - td0;
      break;
    }
    const int16_t bandTop = y;
    // 吃掉连续变化的行, 合并成一条推送(ST7789 在这条里自己递增地址)
    while (y <= y1 && memcmp(s_frameBuf + (size_t)y * TFT_W, s_shadow + (size_t)y * TFT_W, rowBytes) != 0) {
      y++;
    }
    diffUs += micros() - td0;
    const int16_t bandH = (int16_t)(y - bandTop);
    if (!started) {
      tft.startWrite();
      started = true;
    }
    pushBand(bandTop, bandH);
    pushedBytes += (uint32_t)bandH * rowBytes;
    bands++;
  }
  if (started) {
    tft.endWrite();
  }

  const uint32_t dt = micros() - t0;
  s_perf.partN++;
  s_perf.partUs += dt;
  s_perf.partBytes += pushedBytes;
  s_perf.partBands += bands;
  s_perf.diffUs += diffUs;
  if (bands == 0) {
    s_perf.partNoopN++;
  }
  if (dt > s_perf.partMaxUs) {
    s_perf.partMaxUs = dt;
  }
  perfNoteFlush(dt, pushedBytes, false, bands);
  return true;
}

bool displayFlushDirty(void) {
  return displayFlushDirtyRange(0, (int16_t)(TFT_H - 1));
}

// ---------------------------------------------------------------------------
// 局部刷新: 只推画布上的一块矩形(保留原接口, 内部走脏区域引擎)
// ---------------------------------------------------------------------------
bool displayFlushRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!s_canvas || w <= 0 || h <= 0) {
    return false;
  }
  // 裁剪到屏幕范围
  if (x < 0) {
    w = (int16_t)(w + x);
    x = 0;
  }
  if (y < 0) {
    h = (int16_t)(h + y);
    y = 0;
  }
  if (x + w > TFT_W) {
    w = (int16_t)(TFT_W - x);
  }
  if (y + h > TFT_H) {
    h = (int16_t)(TFT_H - y);
  }
  if (w <= 0 || h <= 0) {
    return false;
  }

  // 整行宽(目前所有调用者都是): 交给脏区域引擎, 只推真正变了的行
  if (x == 0 && w == TFT_W) {
    return displayFlushDirtyRange(y, (int16_t)(y + h - 1));
  }

  // 非整行宽: 逐行推送, 并把这几行的对应列同步进影子缓冲
  const uint32_t t0 = micros();
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  const uint32_t tLock = micros();
  s_perf.flushLockWaitUs += (tLock - t0);
  if (!g.ok()) {
    s_perf.flushLockTimeoutN++;
    return false;
  }
  tft.startWrite();
  for (int16_t row = 0; row < h; row++) {
    const uint16_t *src = s_frameBuf + (size_t)(y + row) * TFT_W + x;
    tft.setAddrWindow(x, y + row, w, 1);
    tft.pushPixels((uint16_t *)src, (uint32_t)w);
    if (s_shadow) {
      memcpy(s_shadow + (size_t)(y + row) * TFT_W + x, src, (size_t)w * 2);
    }
  }
  tft.endWrite();
  const uint32_t dt = micros() - t0;
  s_perf.partN++;
  s_perf.partUs += dt;
  s_perf.partBytes += (uint32_t)w * h * 2;
  s_perf.partBands += h;
  perfNoteFlush(dt, (uint32_t)w * h * 2, false, (uint32_t)h);
  return true;
}

#elif DISPLAY_BACKEND == 2
// ========================= Adafruit_ST7789 =================================
static Adafruit_ST7789 *s_tft = nullptr;
static GFXcanvas16 *s_gfxCanvas = nullptr;
static Canvas16 *s_canvas = nullptr;

const char *displayBackendName(void) {
  return "Adafruit_ST7789";
}

bool displayInit(void) {
  // 同样的顺序: 先把共享总线立起来
  SPI.begin(PIN_TFT_SCK, PIN_SD_MISO, PIN_TFT_MOSI, -1);

  s_tft = new Adafruit_ST7789(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  if (!s_tft) {
    return false;
  }
  s_tft->init(TFT_W, TFT_H);  // ST7789 默认 SPI_MODE0
  s_tft->setSPISpeed(SPI_FREQ_TFT);
  s_tft->setRotation(TFT_ROTATION);
  s_tft->fillScreen(COL_BG);

  // GFXcanvas16 内部用 malloc; 开了 PSRAM 时 ESP32 的 malloc 会分配到 PSRAM
  s_gfxCanvas = new GFXcanvas16(TFT_W, TFT_H);
  if (!s_gfxCanvas || !s_gfxCanvas->getBuffer()) {
    log_e("[display] GFXcanvas16 分配失败(153600 字节)。请确认 PSRAM 已开启");
    return false;
  }
  s_canvas = new Canvas16(TFT_W, TFT_H, s_gfxCanvas->getBuffer());
  s_canvas->fill(COL_BG);

  log_i("[display] Adafruit_ST7789 就绪 %dx%d, SPI=%u Hz", TFT_W, TFT_H, (unsigned)SPI_FREQ_TFT);
  return true;
}

Canvas16 *displayCanvas(void) {
  return s_canvas;
}

bool displayFlush(void) {
  if (!s_canvas || !s_tft) {
    return false;
  }
  const uint32_t t0 = micros();
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  const uint32_t tLock = micros();
  s_perf.flushLockWaitUs += (tLock - t0);
  if (!g.ok()) {
    log_w("[display] 刷屏等锁超时, 跳过本帧");
    s_perf.flushLockTimeoutN++;
    return false;
  }
  s_tft->drawRGBBitmap(0, 0, s_canvas->pixels(), TFT_W, TFT_H);
  const uint32_t dt = micros() - t0;
  s_perf.fullN++;
  s_perf.fullUs += dt;
  s_perf.fullBytes += (uint32_t)TFT_W * TFT_H * 2;
  if (dt > s_perf.fullMaxUs) {
    s_perf.fullMaxUs = dt;
  }
  perfNoteFlush(dt, (uint32_t)TFT_W * TFT_H * 2, true, 1);
  return true;
}

// Adafruit 后端没有实现逐行差分(这个后端当前没在用), 直接整帧推送 ——
// 功能完全正确, 只是享受不到"只推脏区域"的加速。
bool displayFlushDirty(void) {
  return displayFlush();
}

bool displayFlushDirtyRange(int16_t y0, int16_t y1) {
  (void)y0;
  (void)y1;
  return displayFlush();
}

void displayDrain(void) {
  delayMicroseconds(200);
}
#endif


// ---------------------------------------------------------------------------
// 返回显示库实际使用的 SPI 实例, SD 卡要用同一个(见 display.h 的说明)
// ---------------------------------------------------------------------------
SPIClass *displaySpiInstance(void) {
#if DISPLAY_BACKEND == 1
  return s_spiUsed;      // 由 displayInit() 里通过 TFT_eSPI::getSPIinstance() 取得
#else
  return &SPI;           // Adafruit 后端: 我们显式传的就是全局 SPI
#endif
}

// ---------------------------------------------------------------------------
// 给 sd_bench 用的两个探针(只做诊断, 不参与正常渲染路径)
// ---------------------------------------------------------------------------

// 做一次**真正的整帧刷屏**, 走的路径和 [perf] 里"整帧推送"完全一样。
// 用途: 验证"刷屏是否会把共享 SPI/SD 的状态搞脏, 从而让下一次 SD 读变慢"。
// 调用者负责持 sd_mutex(自检里已经持了)。
void displayBenchFullFrame(void) {
#if DISPLAY_BACKEND == 1
  if (!s_frameBuf) {
    return;
  }
  tft.startWrite();
  tft.pushImage(0, 0, TFT_W, TFT_H, s_frameBuf);
  tft.endWrite();
#endif
}

// 把共享 SPI 总线**重新配置成 SD 的时序**(然后立刻交还)。
// 思路: TFT_eSPI 是直接写 SPI 寄存器推像素的(绕过了 SPIClass 的驱动状态机),
//       如果它把外设留在"40MHz / 其它模式", 而 SD 驱动又以为自己已经配好了,
//       那么 SD 的实际时序就是错的 -> 卡反复重试 -> 慢 100 倍。
//       这个函数就是把外设拉回 SD 期望的状态, 用来验证上面的猜测。
void displaySpiReprime(void) {
  SPIClass *sp = displaySpiInstance();
  if (!sp) {
    return;
  }
  sp->beginTransaction(SPISettings(SPI_FREQ_SD, MSBFIRST, SPI_MODE0));
  sp->endTransaction();
}


// ---------------------------------------------------------------------------
// 内置 5x7 ASCII 渲染(字库不可用时的兜底)
//
// 直接写帧缓冲, 不依赖任何字库, 所以"没插 SD 卡 / 字库缺失"时屏幕依然能显示
// 可读的英文提示 —— 否则启动排错信息本身就画不出来。
// ---------------------------------------------------------------------------
int16_t builtinTextWidth(const char *s, uint8_t scale) {
  if (!s) {
    return 0;
  }
  return (int16_t)(strlen(s) * 6 * scale);
}

int16_t builtinDrawText(Canvas16 &canvas, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg,
                        uint8_t scale) {
  if (!s) {
    return x;
  }
  if (scale < 1) {
    scale = 1;
  }
  int16_t cx = x;
  for (const char *p = s; *p; p++) {
    const uint8_t ch = (uint8_t)*p;
    const bool known = (ch >= 32 && ch <= 126);
    if (known) {
      const uint8_t *glyph = FONT5X7[ch - 32];
      for (uint8_t col = 0; col < 5; col++) {
        const uint8_t bits = pgm_read_byte(&glyph[col]);
        for (uint8_t row = 0; row < 7; row++) {
          const bool on = (bits >> row) & 1;
          for (uint8_t sy = 0; sy < scale; sy++) {
            for (uint8_t sx = 0; sx < scale; sx++) {
              canvas.setPixel(cx + col * scale + sx, y + row * scale + sy, on ? fg : bg);
            }
          }
        }
      }
    } else {
      // 不认识的字符(含中文): 画一个小实心方块, 明确表示"这里有个字符但画不出来"
      for (uint8_t sy = 0; sy < 5 * scale; sy++) {
        for (uint8_t sx = 0; sx < 5 * scale; sx++) {
          canvas.setPixel(cx + sx, y + 2 + sy, fg);
        }
      }
    }
    cx += 6 * scale;
  }
  return cx;
}
