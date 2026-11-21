/*
 * bus_init.h - 共享 SPI 总线的初始化（bookreader 与 diag_display 共用）
 *
 * 【为什么默认不自己调 SPI.begin() —— 这是踩过坑的结论】
 *
 * TFT_eSPI 的 TFT_eSPI::init() 内部**自己就会初始化总线**：
 *     TFT_eSPI.cpp:647  spi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1);
 * 而 ESP32-S3 的 TFT_eSPI_ESP32_S3.h 里有：
 *     #if (TFT_MISO == -1)  #define TFT_MISO TFT_MOSI  #endif
 * 也就是**没定义 TFT_MISO 时它会用 MOSI 的管脚号当 MISO**（本方案 SDO 悬空）。
 *
 * 于是出现过一个真实的崩溃：
 *   我们先用 (SCK=12, MISO=13, MOSI=11) 调 SPI.begin(),
 *   接着 TFT_eSPI 又用 (SCK=12, MISO=11, MOSI=11) 调一次 —— 两次对 MISO 的
 *   设定不一致。实测结果是 tft.init() 一进去就 LoadProhibited
 *   (EXCCAUSE 0x1d, EXCVADDR 0x10)。
 *
 * 而 SPIClass::begin() 是"已初始化就直接 return", 所以**谁先调谁说了算**。
 * 既然 TFT_eSPI 无论如何都会自己初始化, 就让它来 —— 这也是实测能点亮屏幕的路径。
 * 之后 SD 库用的还是同一个全局 SPIClass 对象, 总线共用关系不变。
 *
 * 如果哪天真需要自己控制引脚, 把下面 BUS_OUR_BEGIN 改成 1 再测。
 */
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "config.h"

// 0 = 由 TFT_eSPI 自己初始化总线(推荐, 实测可用)
// 1 = 我们先自己初始化(历史上有崩溃, 保留以便排查)
#define BUS_OUR_BEGIN 0   // 保持 0: 由 TFT_eSPI 初始化它自己要用的那条总线

static inline bool busBeginShared() {
#if BUS_OUR_BEGIN
  const bool ok = SPI.begin(PIN_TFT_SCK, PIN_SD_MISO, PIN_TFT_MOSI, -1);
  if (!ok) {
    log_e("[bus] SPI.begin(SCK=%d MISO=%d MOSI=%d) 失败, 放弃初始化屏幕", PIN_TFT_SCK, PIN_SD_MISO,
          PIN_TFT_MOSI);
    return false;
  }
  log_i("[bus] 由我们自己初始化: SCK=%d MISO=%d MOSI=%d", PIN_TFT_SCK, PIN_SD_MISO, PIN_TFT_MOSI);
#else
  // 什么都不做: 交给 TFT_eSPI::init() 里的 spi.begin(...) 初始化它自己要用的那条总线。
  // SD 卡随后会用 TFT_eSPI 的同一个实例(见 bookreader.ino 的 mountSd),
  // 所以无论 TFT_eSPI 走 SPI2 还是 SPI3, 两者都在同一条总线上。
  log_i("[bus] 总线交由 TFT_eSPI 初始化; SD 卡会跟随同一个实例");
#endif
  return true;
}
