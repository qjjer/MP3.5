/*
 * lvgl_port.h — LVGL 与这块板子之间的"接线层"(显示 + 输入 + 心跳)
 *
 * 分工:
 *   lvgl_port.cpp  —— 只负责把 LVGL 接到已存在的显示/输入设施上, 不含任何业务界面
 *   ui_lvgl.cpp    —— 界面 + 中文字体桥接(第 1 阶段的示例界面)
 *
 * ★ 为什么 flush 不自己驱动 TFT_eSPI ★
 *   display.cpp 里的 tft 对象是 static 的, 而且它和 SD 卡共用 SPI 总线 + 一把
 *   sd_mutex。自己再 new 一个 TFT_eSPI 实例去推屏, 就等于绕过了那把锁 ——
 *   音频读卡的同一时刻推屏, 屏上会花。所以这里**复用 display.cpp 已有的出口**:
 *     1) displayCanvas()  拿到那块 PSRAM 全屏画布(Canvas16)
 *     2) 把 LVGL 这一块脏区域的像素 memcpy 进画布
 *     3) displayFlushRect(x, y, w, h) —— 它内部持 sd_mutex, 且只把真正变了的行发出去
 *   LVGL 本来就只把**脏区域**交给 flush_cb, 所以"只推脏区域"这一条天然成立;
 *   再叠加上 display.cpp 的行差分(影子缓冲比对), 一个像素没变时一个字节都不发。
 *
 * ★ 关掉 LVGL 时(UI_BACKEND_LVGL=0) ★
 *   下面所有函数都退化成空实现, 主程序可以**无条件调用**, 接线代码两种模式通用。
 */
#pragma once

#include "config.h"

#if UI_BACKEND_LVGL

#include <lvgl.h>

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
// 初始化 LVGL + 显示驱动 + 输入驱动(EC11 旋钮/按键)。
// 前置条件: displayInit() 成功(画布必须已经存在)。
// 返回 false 表示初始化失败(画布没有 / 缓冲分配失败), 调用方应留在旧 UI。
bool lvglPortBegin(void);

bool lvglPortReady(void);

// 主循环里每次调一次。内部就是 lv_timer_handler() + 每秒一行 [lvgl] 统计。
// 建议调用周期 4~5ms(和现有 loop 的 delay(4) 一致): 再快没用, 再慢输入会有延迟感。
void lvglPortLoop(void);

// ---------------------------------------------------------------------------
// 输入: EC11 旋钮 + 按键 -> LVGL encoder 类型的 indev
//
// 用 LV_INDEV_TYPE_ENCODER(而不是自己解析事件)的理由:
//   - 旋钮天然就是 encoder: 正转 -> LV_KEY_NEXT, 反转 -> LV_KEY_PREV,
//     按下 -> LV_KEY_ENTER。配合 lv_group 就有了"焦点在控件间移动"的语义,
//     列表/按钮/滚轮全都自动支持, 不用给每个控件写事件处理。
//   - 参考项目那套是 XPT2046 触摸屏(INDEV_TYPE_POINTER + 坐标 map), 我们没有触摸,
//     搬过来也用不上。
//   - input.cpp 的 1ms 任务已经把事件排队了, 这里只是"取走并翻译成 LVGL 语义",
//     不新增任何轮询/中断。
// ---------------------------------------------------------------------------
// 建一个输入组并把默认 indev 绑上去。之后新控件用 lv_group_add_obj() 加进来。
lv_group_t *lvglPortNewGroup(void);
void lvglPortAttachGroup(lv_group_t *g);
lv_indev_t *lvglPortIndev(void);

// 正转 = 焦点往下(与旧 UI 的 uiMusicMove(+1) 方向一致)。
// 如果上机发现方向反了, 把这里改成 true 即可, 不用动别的代码。
void lvglPortInvertEncoder(bool invert);

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------
void lvglPortPrintStats(void);      // 立即打一行 [lvgl]
void lvglPortSetVerboseFlush(bool on);  // 每次 flush 打一行(排障用, 平时关)

#else  /* UI_BACKEND_LVGL == 0: 全部退化空实现, 主程序可以无条件调用 */

#include <stdint.h>

struct _lv_group_t;
struct _lv_indev_t;

static inline bool lvglPortBegin(void) { return false; }
static inline bool lvglPortReady(void) { return false; }
static inline void lvglPortLoop(void) {}
static inline struct _lv_group_t *lvglPortNewGroup(void) { return nullptr; }
static inline void lvglPortAttachGroup(struct _lv_group_t *) {}
static inline struct _lv_indev_t *lvglPortIndev(void) { return nullptr; }
static inline void lvglPortInvertEncoder(bool) {}
static inline void lvglPortPrintStats(void) {}
static inline void lvglPortSetVerboseFlush(bool) {}

#endif /* UI_BACKEND_LVGL */
