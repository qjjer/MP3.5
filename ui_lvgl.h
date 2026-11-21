/*
 * ui_lvgl.h — LVGL 版界面(第 1 阶段: 中文显示 + 可旋转选择的列表)
 *
 * ★ 对主程序(.ino)只暴露 3 个函数, 接线越少越好 ★
 *
 *   setup() 末尾(约 630 行, 在 uiShowMenu()/uiRender() 之后):
 *       #if UI_BACKEND_LVGL
 *         uiLvglBegin();          // 失败会自动退回旧 UI(返回 false 时不接管屏幕)
 *       #endif
 *
 *   loop() 开头(约 1106 行, 在 encoderTakeDelta()/buttonTakeEvent() 之前!):
 *       #if UI_BACKEND_LVGL
 *         if (uiLvglActive()) {    // LVGL 接管时, 旧的事件消费必须被跳过
 *           uiLvglLoop();          // 内部: lv_timer_handler() + 每秒一行 [lvgl]
 *           delay(4);
 *           return;
 *         }
 *       #endif
 *
 * ★ 为什么 loop 里必须放在最前面 ★
 *   LVGL 的 encoder indev 是**主动消费** encoderTakeDelta()/buttonTakeEvent() 的。
 *   如果旧 UI 的 loop 先跑, 事件会被它吃掉, LVGL 这边就什么都收不到。
 *
 * ★ 关掉 LVGL(=0)时 ★
 *   下面全部退化成空实现, 所以就算上面那些 #if 忘了删, 编译和运行也不会有任何变化。
 */
#pragma once

#include "config.h"

#if UI_BACKEND_LVGL

#include <lvgl.h>

// 中文点阵字体(桥接 DotFont)。LVGL 的"默认字体"仍是内置 Montserrat,
// 所以界面上每个要显示中文的对象都要把这个字体设上去(或设在父对象上让子对象继承)。
const lv_font_t *uiLvglFont(void);

// 初始化 LVGL + 显示/输入驱动 + 建第 1 阶段的示例界面。
// 返回 false: LVGL 没起来(画布缺失/内存不足), 调用方应继续用旧 UI。
bool uiLvglBegin(void);

// LVGL 是否已经接管屏幕
bool uiLvglActive(void);

// 主循环里调用(建议 4~5ms 一次)
void uiLvglLoop(void);

#else  /* UI_BACKEND_LVGL == 0 */

static inline bool uiLvglBegin(void) { return false; }
static inline bool uiLvglActive(void) { return false; }
static inline void uiLvglLoop(void) {}

#endif /* UI_BACKEND_LVGL */
