/*
 * sd_mutex.h — 共用 SPI 总线 + SD 卡访问互斥
 *
 * 【为什么需要这把锁】
 * 你的屏幕和 MicroSD 共用 MOSI(GPIO11)/SCK(GPIO12), 靠各自 CS 区分。
 * arduino-esp32 的 SPI 库在 beginTransaction() 里已经有总线锁, 所以"单个 SPI 事务"
 * 不会被撕裂; 但下面这种情况它管不了:
 *
 *   UI 任务:  [推第 1~800 字节] [推第 801~1600 字节] ... 一屏要几百个事务
 *   音频任务:                   ^^^ 中间插进来读了一个 SD 扇区
 *
 * 对 ST7789 来说, 一帧图像推到一半被插队 => 花屏/撕裂。所以我们需要一把
 * "覆盖整个逻辑操作"的高层锁, 粒度是:
 *   - 刷一整屏(或一条)像素
 *   - 读音频/文本数据块、开目录、跳转
 *
 * 【锁序约定, 必须遵守, 否则死锁】
 *   1. 本锁是递归锁, 同一任务内可以嵌套(例如 UI 拿锁后再调用 TFT 底层事务)。
 *   2. 任何"长时间"操作(整屏刷新、大块读)必须在锁内完成, 不允许中途释放。
 *   3. 音频任务(优先级更高)拿不到锁时会让出 CPU, UI 任务继承其优先级,
 *      所以不会出现"音频等 UI 整整一屏"的优先级反转卡顿。
 */
#pragma once

#include <Arduino.h>

// 创建互斥锁。必须在 setup() 里、任何任务启动之前调用一次。
void sdLockInit(void);

// 获取/释放总线锁。timeoutMs 为 0 表示只尝试一次, 不等待。
bool sdLockTake(uint32_t timeoutMs = 3000);
void sdLockGive(void);

// RAII 守卫: 作用域结束自动释放, 异常/提前 return 也不漏
class SdLockGuard {
public:
  explicit SdLockGuard(uint32_t timeoutMs = 3000) {
    locked_ = sdLockTake(timeoutMs);
  }
  ~SdLockGuard() {
    if (locked_) {
      sdLockGive();
    }
  }
  // 是否成功拿到锁。没拿到时不要访问 SPI/SD, 直接走失败分支
  bool ok() const {
    return locked_;
  }
  explicit operator bool() const {
    return locked_;
  }

private:
  bool locked_;
  SdLockGuard(const SdLockGuard &);
  SdLockGuard &operator=(const SdLockGuard &);
};

// 便捷宏: 锁超时时统一打印, 方便定位"谁在占着总线"
#define SD_LOCK_OR_FAIL(timeoutMs, failExpr) \
  SdLockGuard _sdg(timeoutMs);               \
  if (!_sdg.ok()) {                          \
    log_e("[sd] lock timeout %ums at %s:%d", (unsigned)(timeoutMs), __FILE__, __LINE__); \
    return failExpr;                         \
  }
