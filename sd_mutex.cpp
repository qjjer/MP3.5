/*
 * sd_mutex.cpp — 共用 SPI 总线 + SD 卡访问互斥的实现
 */
#include "sd_mutex.h"
#include "config.h"

static SemaphoreHandle_t s_busMutex = nullptr;

void sdLockInit(void) {
  if (s_busMutex == nullptr) {
    // 递归锁: 允许同一任务嵌套获取(UI 拿锁 -> TFT 底层事务 -> 再拿)
    s_busMutex = xSemaphoreCreateRecursiveMutex();
    if (s_busMutex == nullptr) {
      log_e("[sd] 无法创建总线互斥锁, 内存不足!");
    }
  }
}

bool sdLockTake(uint32_t timeoutMs) {
  if (s_busMutex == nullptr) {
    // 还没初始化就容错处理: 不阻塞, 也不锁(单任务启动阶段是安全的)
    return true;
  }
  TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTakeRecursive(s_busMutex, ticks) == pdTRUE;
}

void sdLockGive(void) {
  if (s_busMutex != nullptr) {
    xSemaphoreGiveRecursive(s_busMutex);
  }
}
