/*
 * input.h - EC11 旋转编码器 + 按键 + 电池电压
 *
 * 单独放一个头文件而不是写在 .ino 里: Arduino 的预处理器会往 .ino 里自动插入
 * 函数原型, 位置在 include 之后、函数定义之前, 很容易把 .ino 里定义的类型
 * (比如下面的 enum) 变成"未声明的类型"。放进 .h 就彻底避开这个坑。
 */
#pragma once

#include <Arduino.h>

// 按键事件
enum BtnEvent : uint8_t {
  BTN_NONE = 0,
  BTN_SHORT,    // 短按(松开时触发)
  BTN_LONG,     // 长按(按住超过阈值时触发一次)
  BTN_REPEAT,   // 长按保持(之后每隔一段触发一次, 用于连发调节)
  BTN_BOOT,     // 开发板 GPIO0 上的 BOOT 键(免接线的备用按键)
};

// 长按判定与连发间隔(毫秒)
#define BTN_LONG_MS    800
#define BTN_REPEAT_MS  180

// 电池状态
struct BatteryInfo {
  uint16_t millivolts = 0;  // 电池电压(mV)
  uint8_t percent = 0;      // 0~100
  bool valid = false;
};

void inputInit(void);

// 轮询编码器(无阻塞, 建议 UI 循环里每 1~5ms 调一次)
void encoderPoll(void);
// 取走并清零旋转累计量: 正数 = 顺时针, 负数 = 逆时针, 单位是"档"
int32_t encoderTakeDelta(void);

// 轮询按键, 返回本次产生的事件(可在任意任务里高频调用)
BtnEvent buttonPoll(void);

// 按钮/编码器独立任务: 每 1ms 轮询一次, 把事件压进内部队列。
// 这样即使主循环在做耗时操作(刷屏、解压 EPUB), 按键也不会丢。
void inputStartTask(void);

// 从队列取一个按键事件(没有则返回 BTN_NONE)。主循环用这个, 不要直接调 buttonPoll。
BtnEvent buttonTakeEvent(void);

// 诊断用: 打印当前三个引脚的原始电平(排查接线问题)
void inputPrintPinStates(void);

void batteryInit(void);
BatteryInfo batteryRead(void);