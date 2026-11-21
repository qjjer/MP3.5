/*
 * sd_bench.h — SD 通路自检（一次性诊断, 纯 sketch 侧, 不依赖核心库补丁）
 *
 * 背景: 设备上 FATFS 顺序读恒定 ~38.5 KB/s(每 512B 约 105ms)。PC 上同一张卡
 *       4KB 随机读只要 3ms —— 慢 40~1400 倍, 所以真因在设备侧。
 *
 * ★ 用法（推荐, 由 Lead 接线）★
 *   在 `bookreader.ino` 的 `mountSd()` 里, `SD.begin(...)` 成功之后加两行:
 *       #include "sd_bench.h"          // 文件顶部
 *       sdBenchRunEarly();             // SD.begin 成功、return true 之前
 *   它会跑一次原始 SPI 自检并打印 [sdbench] #01..#99, 然后立刻返回, 不影响启动。
 *   诊断完把那一行删掉即可(重复调用也只会跑一次)。
 *
 * 全部测量只用本文件里的 sb* 原始 SPI 代码(自制 CMD17/CMD18 + 真 CRC7),
 * **不需要修改/重编 ESP32 核心包的任何东西**。
 * 想彻底关掉: config.h 里把 SD_BENCH_ENABLE 改成 0(那时下面的函数体变成空操作)。
 */
#pragma once

#include <Arduino.h>

// ★ 推荐入口: 在 SD 卡挂载成功之后调用一次(见上方用法)。
//   卡没挂载时只打印一行提示并返回, 不消耗"只跑一次"的标记。
void sdBenchRunEarly(void);

// 旧名字(displayInit 里的老调用点), 等价于 sdBenchRunEarly()
void sdBenchRun(void);

// 是否已经跑过(供其它模块判断)
bool sdBenchHasRun(void);
