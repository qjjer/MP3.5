/*
 * display.h — 屏幕抽象层(统一画布接口)
 *
 * 设计要点:
 *   - 上层(字体/阅读器/UI)只认 Canvas16 这个画布接口, 不关心底层是哪个库。
 *   - 画布是 PSRAM 里的 16bit 全屏帧缓冲, 先在内存里画完整帧, 再一次性刷到屏幕。
 *     这样做有两个好处:
 *       1) 不会出现"画一半被音频读卡插队"的撕裂(配合 sd_mutex 的整帧锁);
 *       2) SPI 总线只被占用一次, 音频任务等待时间可预测。
 *   - Canvas16 暴露 pixels() 裸指针, 字体渲染直接写像素数组, 不走 drawPixel 虚函数。
 *
 * 【绝对不要】在持锁区间之外调用 tft 的画图 API: 那会绕过 sd_mutex, 直接和
 * SD 卡抢 SPI2 总线, 造成花屏。所有刷屏都走 displayFlush()。
 */
#pragma once

#include <Arduino.h>

// 画布接口: 一块 w*h 的 RGB565 像素数组
class Canvas16 {
public:
  Canvas16(uint16_t w, uint16_t h, uint16_t *buf) : w_(w), h_(h), buf_(buf) {}

  uint16_t width() const {
    return w_;
  }
  uint16_t height() const {
    return h_;
  }
  uint16_t *pixels() const {
    return buf_;
  }

  inline uint16_t getPixel(int16_t x, int16_t y) const {
    if (x < 0 || y < 0 || x >= (int16_t)w_ || y >= (int16_t)h_) {
      return 0;
    }
    return buf_[(uint32_t)y * w_ + x];
  }
  inline void setPixel(int16_t x, int16_t y, uint16_t c) {
    if (x < 0 || y < 0 || x >= (int16_t)w_ || y >= (int16_t)h_) {
      return;
    }
    buf_[(uint32_t)y * w_ + x] = c;
  }
  inline void fill(uint16_t c) {
    const uint32_t n = (uint32_t)w_ * h_;
    for (uint32_t i = 0; i < n; i++) {
      buf_[i] = c;
    }
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
  void hLine(int16_t x, int16_t y, int16_t w, uint16_t c);
  void vLine(int16_t x, int16_t y, int16_t h, uint16_t c);
  void rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);

private:
  uint16_t w_, h_;
  uint16_t *buf_;
};

// 初始化屏幕(含 SPI 总线初始化)与帧缓冲。失败返回 false。
bool displayInit(void);

// 拿全屏画布。返回 nullptr 表示帧缓冲分配失败。
Canvas16 *displayCanvas(void);

// 把画布内容刷到屏幕。内部会拿 sd_mutex 的整帧锁, 这是唯一允许的刷屏入口。
// 返回 false 表示拿锁超时(这种情况会跳过本帧, 不损坏屏幕内容)。
bool displayFlush(void);

// ---------------------------------------------------------------------------
// 局部刷新: 只把画布上的一块矩形推到屏幕
//
// ★ 为什么需要它 ★
//   整帧是 240x320x2 = 153600 字节, 40MHz 下要 30.7ms —— 这段 CPU 是死等的
//   (TFT_eSPI 的 pushImageDMA 内部也会 dmaWait, 所以开 DMA 也省不了)。
//   而"每秒刷新一次状态栏"这种需求只改了最底下 240x20 那一条:
//       整帧  153600 字节 -> 30.7ms
//       状态栏  9600 字节 ->  1.9ms     (快 16 倍)
//   于是音乐进度秒跳、电量变化都不再拖慢手感。
//
// 参数是屏幕坐标(会被裁剪到屏幕范围内)。x/y 为块左上角, w/h 为宽高。
// 同样持 sd_mutex, 和整帧刷新互斥。
//
// 实现说明: x==0 且 w==TFT_W(整行宽)时它会走 displayFlushDirtyRange() ——
// 也就是"只推真正变了的行", 而不是无条件推这一整块。这样状态栏每秒刷新时,
// 如果这一秒时间文字没变, 一个字节都不会发。
bool displayFlushRect(int16_t x, int16_t y, int16_t w, int16_t h);

// ---------------------------------------------------------------------------
// ★ 脏区域刷新(参考项目 LVGL 思路的等价实现)★
//
// 背景: 整帧 240x320x2 = 153600 字节, 40MHz 下光传输就 30.7ms, 而且这段
// 时间是 CPU 死等的(TFT_eSPI 非 DMA 路径每个 64 字节块都要自己写寄存器+等 SPI_USR)。
// 于是"转一格旋钮 -> 整帧重推"给人的感觉就是卡。
//
// 参考项目流畅的真正原因不是 LVGL, 而是它**只把变了的区域交给屏幕**:
//     tft.setAddrWindow(area->x1, area->y1, w, h);
//     tft.pushColors(..., w*h, true);
// 这里用同样的思路, 但不需要改任何绘制代码:
//   - 内部保留一块"影子缓冲", 内容是**上一次真正推到屏幕上的像素**;
//   - 刷屏时逐行比对 帧缓冲 vs 影子缓冲, 把连续变化的行合并成"行段", 每个行段只做
//     一次 setWindow + 一次连续推送(等价于 LVGL 的脏区域);
//   - 推完把该行段同步进影子缓冲。
// 正确性: 影子缓冲只在"这一行真的推出去了"之后才更新, 所以差分只会漏推
// (不会多推)。比 displayFlush() 更保守地少发字节, 绝不会把屏幕刷错。
//
// 返回 true = 完成(可能一个字节都没发, 那就是屏幕本来就对);
// 返回 false = 拿锁超时或没初始化(本帧丢弃, 屏幕内容不变, 下一帧会补上)。
bool displayFlushDirty(void);

// 只在 [y0, y1] 这个行区间内做差分 + 推送(状态栏/播放进度条这种已知小区域用,
// 省掉整屏比对的时间)。y0/y1 会被裁剪到屏幕内。
bool displayFlushDirtyRange(int16_t y0, int16_t y1);

// 让下一帧跳过差分、直接整帧推送一次。
// 用在"整屏内容必然全变"的场合(翻页、切界面), 这样就不必为必然全变的帧
// 白付一次逐行比对的开销。整帧推送永远是正确的, 所以调不调都安全。
void displayDirtyInvalidate(void);

// ---------------------------------------------------------------------------
// ★ 性能测量(task-6): 先量, 再改 ★
//
// 每秒打印一行 [perf], 内容:
//   - uiRender() 整屏渲染次数 / 平均与最大耗时
//   - uiRenderStatusBarOnly() 局部渲染次数 / 平均耗时
//   - 其中"绘制(含读 SD 字形)"和"刷屏"各占多少, 以及等 sd_mutex 的时间
//   - 整帧推送次数+耗时+实测带宽, 局部推送次数+耗时+实际发出的字节数
//   - 字形缓存命中/未命中次数(未命中 = 一次 SD 随机读)
// 只在有渲染活动时打印, 空闲时一行都不打。
// ---------------------------------------------------------------------------
void displayPerfNoteUi(uint32_t totalUs, uint32_t drawUs, uint32_t drawLockWaitUs, bool fullScreen,
                       uint32_t glyphHits, uint32_t glyphMisses, bool drawLockOk);
void displayPerfTick(void);

// 上一次刷屏的明细(供 ui.cpp 在"这一帧特别慢"时打一行分解, 见 ui.cpp 的 [perf-slow])
void displayLastFlushInfo(uint32_t *outUs, uint32_t *outBytes, bool *wasFull, uint32_t *outBands);

// 立即停止 DMA 并确保总线上没有未完成的 TFT 传输。
// 硬关机/重启前必须调用(否则可能打断 SD 写操作)。
void displayDrain(void);

// 诊断: 返回显示后端名字
const char *displayBackendName(void);

// 显示库实际使用的 SPI 总线实例。
// ★ SD 卡必须用同一个实例, 否则屏幕和 SD 会跑在两条不同的总线上, sd_mutex 就白加了。
// 注意: 必须在 displayInit() 成功之后调用(TFT_eSPI 在那时才知道自己用哪个端口)。
class SPIClass;
SPIClass *displaySpiInstance(void);


// 诊断探针(给 sd_bench 用, 不参与正常渲染):
//   displayBenchFullFrame(): 做一次真正的整帧刷屏(与 [perf] 的"整帧推送"同路径)
//   displaySpiReprime():     把共享 SPI 重新配置成 SD 的时序后立刻交还
void displayBenchFullFrame(void);
void displaySpiReprime(void);

// 注意: 没有"运行时改 SPI 频率"的接口。
// TFT_eSPI 的频率是编译期由 tft_setup.h 里的 SPI_FREQUENCY 决定的,
// 要调速度就改那个宏然后重新编译。
