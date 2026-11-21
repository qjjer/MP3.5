/*
 * lvgl_port.cpp — LVGL 显示/输入驱动接线实现
 *
 * 只做三件事:
 *   1) 显示: LVGL 脏区域 -> displayCanvas() 画布 -> displayFlushRect()(内部持 sd_mutex)
 *   2) 输入: input.cpp 的 EC11 队列 -> LVGL encoder indev
 *   3) 心跳/统计: LV_TICK_CUSTOM=millis(), 每秒一行 [lvgl]
 *
 * 全部包在 #if UI_BACKEND_LVGL 里: 关掉时这个文件编译成空, 连 lvgl.h 都不 include,
 * arduino-cli 的依赖扫描就不会去编译 LVGL 库 —— 固件与加 LVGL 之前完全等价。
 */
#include "config.h"

#if UI_BACKEND_LVGL

#include "lvgl_port.h"

#include <lvgl.h>
#include <string.h>

#include "display.h"   // Canvas16 / displayCanvas / displayFlushRect
#include "font.h"      // DotFont(只为统计打印)
#include "input.h"     // encoderTakeDelta / buttonTakeEvent

// ---------------------------------------------------------------------------
// ★ 编译期自检: 确认 sketch 目录下的 lv_conf.h 真的被 LVGL 采纳了 ★
//
// 为什么需要这一段: lv_conf.h 是**被 LVGL 库自己的源文件**包含的, 而库文件编译时
// 用的是 __has_include("lv_conf.h") 去猜路径。一旦哪天路径变了(比如把 sketch 挪走),
// LVGL 会**静默**退回它自己的默认配置 —— 最典型的后果是 LV_MEM_CUSTOM 变回 0,
// 于是内部 DRAM 里凭空多出一个 48KB 的静态数组, 甚至直接链接失败。
// 这三条断言让"配置没生效"变成一个**编译期错误**, 而不是上机后才发现的怪事。
// ---------------------------------------------------------------------------
#if LV_MEM_CUSTOM != 1
#error "lv_conf.h 没被 LVGL 采纳: LV_MEM_CUSTOM 应为 1(全部内存走 PSRAM)。请检查 bookreader/lv_conf.h 是否在 sketch 目录下。"
#endif
#if LV_COLOR_DEPTH != 16
#error "lv_conf.h 没被 LVGL 采纳: LV_COLOR_DEPTH 应为 16(RGB565)。"
#endif
#if LV_TICK_CUSTOM != 1
#error "lv_conf.h 没被 LVGL 采纳: LV_TICK_CUSTOM 应为 1(用 millis() 当心跳)。"
#endif

extern DotFont g_font;

// ---------------------------------------------------------------------------
// 绘图缓冲
//
// 240 x 40 x 2B = 19200 字节。LVGL 的 partial 模式按这个高度切条渲染,
// 每次 flush 回调拿到的是"真正变了的那个区域"(通常是整条 240x40 里的一小块)。
// 放 PSRAM: 内部 DRAM 只剩约 230KB, 而画布本来就在 PSRAM 里(同一个memcpy 路径)。
// ---------------------------------------------------------------------------
#define LVGL_BUF_LINES 40

// 正转 = 焦点往下(和旧 UI 的 uiMusicMove(+1) 一致)。上机若方向反了改这里。
#define LVGL_ENCODER_INVERT 0

// 一次 indev 读取最多消化几档旋转(防止快速旋转时一次跳过头)
#define LVGL_ENC_MAX_STEP 5

static lv_disp_draw_buf_t s_drawBuf;
static lv_disp_drv_t s_dispDrv;
static lv_indev_drv_t s_indevDrv;
static lv_color_t *s_buf1 = nullptr;
static lv_disp_t *s_disp = nullptr;
static lv_indev_t *s_indev = nullptr;
static bool s_ready = false;
static bool s_invert = (LVGL_ENCODER_INVERT != 0);
static bool s_verboseFlush = false;

// ---- 统计(只在主循环任务里累加, 无需加锁) ----
static uint32_t s_flushN = 0;          // flush 回调次数
static uint32_t s_flushUs = 0;         // flush 总耗时(含等 sd_mutex)
static uint32_t s_flushUsMax = 0;
static uint32_t s_flushPx = 0;         // 累计推送像素数
static uint32_t s_flushSkip = 0;       // displayFlushRect 返回 false(等锁超时)的次数
static uint32_t s_handlerN = 0;        // lv_timer_handler 调用次数
static uint32_t s_handlerUsSum = 0;
static uint32_t s_handlerUsMax = 0;
static uint32_t s_reportMs = 0;
static uint32_t s_encSteps = 0;        // 消化掉的旋钮档数
static uint32_t s_clicks = 0;          // 上报给 LVGL 的按键次数

// ---------------------------------------------------------------------------
// flush: LVGL 脏区域 -> 画布 -> displayFlushRect()
//
// 为什么绕一道画布而不是直接 tft.pushColors():
//   display.cpp 的 tft 是 static 的, 而且是**唯一**被 sd_mutex 保护的刷屏出口。
//   自己再建一个 TFT_eSPI 实例推屏 = 绕过 sd_mutex, 会和音频读卡抢 SPI 总线 -> 花屏。
//   走画布这一条路的代价只是"每像素一次 memcpy", 换来的是锁语义完全不变。
// ---------------------------------------------------------------------------
static void lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t t0 = micros();

  Canvas16 *canvas = displayCanvas();
  if (!canvas) {
    lv_disp_flush_ready(drv);
    return;
  }

  int16_t x = area->x1;
  int16_t y = area->y1;
  int16_t w = (int16_t)(area->x2 - area->x1 + 1);
  int16_t h = (int16_t)(area->y2 - area->y1 + 1);

  // 防御性裁剪(LVGL 正常不会越界, 但越界一次就是内存踩踏)
  if (x < 0) { w = (int16_t)(w + x); x = 0; }
  if (y < 0) { h = (int16_t)(h + y); y = 0; }
  if (x + w > TFT_W) { w = (int16_t)(TFT_W - x); }
  if (y + h > TFT_H) { h = (int16_t)(TFT_H - y); }

  if (w > 0 && h > 0) {
    uint16_t *dst = canvas->pixels();
    const uint16_t *src = (const uint16_t *)color_p;
    for (int16_t row = 0; row < h; row++) {
      memcpy(dst + (size_t)(y + row) * TFT_W + x, src + (size_t)row * w, (size_t)w * 2);
    }
    // 只把这块脏区域推给屏幕。displayFlushRect 内部:
    //   整行宽 -> displayFlushDirtyRange(逐行差分, 没变的行一个字节都不发)
    //   非整行 -> 逐行 setAddrWindow + pushPixels
    // 两种情况都在内部持 sd_mutex, 和 SD 读卡/音频互斥。
    if (!displayFlushRect(x, y, w, h)) {
      s_flushSkip++;
    }
  }

  const uint32_t dt = micros() - t0;
  s_flushN++;
  s_flushUs += dt;
  s_flushPx += (uint32_t)w * (uint32_t)h;
  if (dt > s_flushUsMax) {
    s_flushUsMax = dt;
  }
  if (s_verboseFlush) {
    Serial.printf("[lvgl] flush area=(%d,%d %dx%d) %lu us\n", (int)x, (int)y, (int)w, (int)h, (unsigned long)dt);
  }

  lv_disp_flush_ready(drv);
}

// ---------------------------------------------------------------------------
// 输入: EC11 -> LVGL encoder indev
//
// 关键点(踩过的坑都写在这):
//   1) LVGL 的 encoder 是"按键边沿"驱动的: 必须**先上报一帧 PRESSED, 再上报一帧
//      RELEASED**, 它才会发 LV_EVENT_CLICKED。而 input.cpp 只在按下时给一次
//      BTN_SHORT 事件 —— 所以这里要做个小状态机兜住:
//          收到事件 -> 下一帧报 PRESSED -> 再下一帧报 RELEASED
//      读周期是 30ms(LV_INDEV_DEF_READ_PERIOD), 所以按一下大约 30~60ms 后生效。
//   2) 只报**一帧** PRESSED 是故意的: LVGL 会把"持续按住 >400ms"当成
//      "进入编辑模式"(lv_group_set_editing), 长按对我们没有意义, 还会把焦点
//      搞成编辑态(旋转变成 LEFT/RIGHT 而不是移动焦点)。一帧就松开, 天然避开。
//   3) 长按事件(BTN_LONG/BTN_REPEAT)先丢掉 —— 第 1 阶段只验证"旋转选择 + 按下确认",
//      长按留给后面的"返回/切界面"。
// ---------------------------------------------------------------------------
static void lvglEncoderReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;

  // ---- 旋转 ----
  static int32_t carry = 0;
  carry += (int32_t)encoderTakeDelta();   // input.cpp 已经做了无阻塞累计
  int32_t step = carry;
  if (step > LVGL_ENC_MAX_STEP) { step = LVGL_ENC_MAX_STEP; }
  if (step < -LVGL_ENC_MAX_STEP) { step = -LVGL_ENC_MAX_STEP; }
  carry -= step;
  if (s_invert) { step = -step; }
  data->enc_diff = (int16_t)step;
  if (step != 0) {
    s_encSteps += (uint32_t)(step < 0 ? -step : step);
  }

  // ---- 按键: 单帧 PRESSED + 下一帧 RELEASED ----
  static bool pressedPending = false;   // 收到过事件, 还没上报
  static bool releasePending = false;   // 已经上报过 PRESSED, 该上报 RELEASED 了

  const BtnEvent ev = buttonTakeEvent();
  if (ev == BTN_SHORT || ev == BTN_BOOT) {
    pressedPending = true;
  }
  // BTN_LONG / BTN_REPEAT 这里故意不处理(见上面注释第 3 条)

  if (releasePending) {
    data->state = LV_INDEV_STATE_RELEASED;
    releasePending = false;
  } else if (pressedPending) {
    data->state = LV_INDEV_STATE_PRESSED;
    pressedPending = false;
    releasePending = true;
    s_clicks++;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }

  data->key = LV_KEY_ENTER;   // encoder 模式下 LVGL 只用它来触发 CLICKED
}

// ---------------------------------------------------------------------------
// 每秒一行 [lvgl] —— 上机判读用
//
// flush=次数/总ms/最长ms: 最长那一笔就是"一帧里花在推屏上"的上限, 应该和
//   [perf] 里的局部推送数字对得上(整条 240x40 约 0.6ms, 满屏一次约 30ms)。
// sent=累计推送像素数: 只推脏区域的直接证据(对比 240*320*次数)。
// wait/lock: 字数太多会顶满一行, 超时次数只打非零值。
// ---------------------------------------------------------------------------
void lvglPortPrintStats(void) {
  if (!s_ready) {
    return;
  }
  const uint32_t now = millis();
  const uint32_t spanMs = (s_reportMs == 0) ? 0 : (now - s_reportMs);
  s_reportMs = now;

  Serial.printf("[lvgl] handler %lu 次 %lu ms(最长 %lu) | flush %lu 次 %lu.%lums(最长 %lu.%lums) sent=%lu px | enc=%lu click=%lu | heap=%luK psram=%luK\n",
                (unsigned long)s_handlerN, (unsigned long)(s_handlerUsSum / 1000), (unsigned long)(s_handlerUsMax / 1000),
                (unsigned long)s_flushN, (unsigned long)(s_flushUs / 1000), (unsigned long)((s_flushUs % 1000) / 100),
                (unsigned long)(s_flushUsMax / 1000), (unsigned long)((s_flushUsMax % 1000) / 100),
                (unsigned long)s_flushPx, (unsigned long)s_encSteps, (unsigned long)s_clicks,
                (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getFreePsram() / 1024));
  if (s_flushSkip) {
    Serial.printf("[lvgl] !! displayFlushRect 等 sd_mutex 超时 %lu 次(这些帧丢了, 下一帧会补)\n",
                  (unsigned long)s_flushSkip);
    s_flushSkip = 0;
  }
  (void)spanMs;

  s_handlerN = 0;
  s_handlerUsSum = 0;
  s_handlerUsMax = 0;
  s_flushN = 0;
  s_flushUs = 0;
  s_flushUsMax = 0;
  s_flushPx = 0;
  s_encSteps = 0;
  s_clicks = 0;
}

void lvglPortLoop(void) {
  if (!s_ready) {
    return;
  }

  const uint32_t t0 = micros();
  const uint32_t idleMs = lv_timer_handler();
  const uint32_t dt = micros() - t0;

  s_handlerN++;
  s_handlerUsSum += dt;
  if (dt > s_handlerUsMax) {
    s_handlerUsMax = dt;
  }

  const uint32_t now = millis();
  if (s_reportMs == 0) {
    s_reportMs = now;
  } else if (now - s_reportMs >= 1000) {
    lvglPortPrintStats();
  }
  // idleMs = 距离下一次该调用的毫秒数。主循环固定 4~5ms 调一次就够了(它比 idleMs 小),
  // 所以这里忽略返回值 —— 万一某次 handler 很慢, 下一轮循环会立刻再进来。
  (void)idleMs;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
static bool lvglAllocDrawBuf(void) {
  const size_t px = (size_t)TFT_W * LVGL_BUF_LINES;
  s_buf1 = (lv_color_t *)ps_malloc(px * sizeof(lv_color_t));
  if (!s_buf1) {
    s_buf1 = (lv_color_t *)malloc(px * sizeof(lv_color_t));   // PSRAM 不够时的兜底
  }
  if (!s_buf1) {
    return false;
  }
  memset(s_buf1, 0, px * sizeof(lv_color_t));
  lv_disp_draw_buf_init(&s_drawBuf, s_buf1, NULL, (uint32_t)px);
  return true;
}

bool lvglPortBegin(void) {
  if (s_ready) {
    return true;
  }

  // 画布必须已经存在(displayInit() 先跑)。没有画布就没有推屏出口。
  if (!displayCanvas()) {
    Serial.println(F("[lvgl] !! displayCanvas() 为空: 必须先 displayInit() 成功"));
    return false;
  }

  const uint32_t t0 = millis();
  lv_init();

  if (!lvglAllocDrawBuf()) {
    Serial.println(F("[lvgl] !! 绘图缓冲分配失败"));
    return false;
  }

  lv_disp_drv_init(&s_dispDrv);
  s_dispDrv.hor_res = TFT_W;
  s_dispDrv.ver_res = TFT_H;
  s_dispDrv.flush_cb = lvglFlushCb;
  s_dispDrv.draw_buf = &s_drawBuf;
  // 画布只有 16bpp 一份, LVGL 直接在里面合成, 不需要第二个缓冲
  s_dispDrv.direct_mode = 0;
  s_dispDrv.full_refresh = 0;
  s_disp = lv_disp_drv_register(&s_dispDrv);
  if (!s_disp) {
    Serial.println(F("[lvgl] !! lv_disp_drv_register 失败"));
    return false;
  }

  lv_indev_drv_init(&s_indevDrv);
  s_indevDrv.type = LV_INDEV_TYPE_ENCODER;
  s_indevDrv.read_cb = lvglEncoderReadCb;
  // 读周期不在这里设: LVGL 8.3 的 lv_indev_drv_t 没有 read_period 字段,
  // 周期由 lv_conf.h 的 LV_INDEV_DEF_READ_PERIOD(30ms)在 lv_indev_drv_register 里建定时器时决定。
  s_indev = lv_indev_drv_register(&s_indevDrv);
  if (!s_indev) {
    Serial.println(F("[lvgl] !! lv_indev_drv_register 失败"));
    return false;
  }

  s_ready = true;
  s_reportMs = millis();

  Serial.printf("[lvgl] begin ok: %dx%d color=%d buf=%ux%u(%u B, %s) disp=%p indev=%p, 耗时 %lu ms\n", (int)TFT_W,
                (int)TFT_H, (int)LV_COLOR_DEPTH, (unsigned)TFT_W, (unsigned)LVGL_BUF_LINES,
                (unsigned)((size_t)TFT_W * LVGL_BUF_LINES * 2), "PSRAM", (void *)s_disp, (void *)s_indev,
                (unsigned long)(millis() - t0));
  return true;
}

bool lvglPortReady(void) {
  return s_ready;
}

lv_group_t *lvglPortNewGroup(void) {
  if (!s_ready) {
    return nullptr;
  }
  lv_group_t *g = lv_group_create();
  if (g) {
    lv_group_set_wrap(g, true);   // 到最后一项再转 -> 回到第一项
  }
  return g;
}

void lvglPortAttachGroup(lv_group_t *g) {
  if (s_ready && s_indev && g) {
    lv_indev_set_group(s_indev, g);
  }
}

lv_indev_t *lvglPortIndev(void) {
  return s_indev;
}

void lvglPortInvertEncoder(bool invert) {
  s_invert = invert;
}

void lvglPortSetVerboseFlush(bool on) {
  s_verboseFlush = on;
}

#endif /* UI_BACKEND_LVGL */
