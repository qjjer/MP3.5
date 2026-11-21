/*
 * lv_conf.h — LVGL 8.3.11 配置(bookreader 专用精简档)
 *
 * ===========================================================================
 * ★ 这个文件必须能被 LVGL 库自己的源文件找到 ★
 * ===========================================================================
 * lvgl/src/lv_conf_internal.h 的查找顺序是:
 *     1) -DLV_CONF_PATH=...            (我们没有加这个编译参数)
 *     2) __has_include("lv_conf.h") -> 有就 #include "lv_conf.h"
 *     3) 否则 #include "../../lv_conf.h"  (= <Arduino>/libraries/lv_conf.h)
 *
 * 实测 arduino-cli 给**库**编译时也会带上 sketch 目录:
 *     -ID:\65383\Documents\dsh\bookreader
 * 所以放在 sketch 目录(本文件)就能被 __has_include 命中, 不需要往
 * Arduino/libraries/ 下塞任何东西。第 2 条命中的标志是**编译日志里不会出现**
 *     #pragma message("Possible failure to include lv_conf.h ...")
 * 每次改完 lv_conf.h 都该在编译日志里确认这一条。
 *
 * ===========================================================================
 * ★ 几个关键选择的理由(都是被这块板子的实测数字逼出来的) ★
 * ===========================================================================
 * 1) LV_MEM_CUSTOM = 1, 全部走 ps_malloc(PSRAM)
 *    内部 DRAM 只剩约 230KB, 而 LVGL 默认要在 .bss 里静态吃 48KB;
 *    8MB OPI PSRAM 还空着 ~7MB, 放那边对谁都好。
 *
 * 2) LV_TICK_CUSTOM = 1, 直接读 Arduino 的 millis()
 *    不用自己开定时器喂 lv_tick_inc(), 少一个可能忘记调用的地方。
 *
 * 3) 应用分区只剩 1.37MB -> 关掉所有用不到的控件/主题/解码器
 *    只留: label / btn / list / bar / roller / flex / 默认主题。
 *    中文字库**不在这里** —— 它走 font.cpp 的 DotFont 桥接(见 ui_lvgl.cpp),
 *    绝不能用 lv_font_conv 生成(几十 MB, Flash 放不下)。
 *
 * 4) LV_COLOR_16_SWAP = 0
 *    画布(display.cpp 的 Canvas16)存的是本机序 RGB565, 且 TFT_eSPI 已经
 *    setSwapBytes(true)。LVGL 也用本机序, 两边直接 memcpy, 不需要交换。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
/* ===========================================================================
 * ★★ 这一行绝对不能写尾注释 ★★  (踩过, 实测复现过)
 *
 *   arduino-cli 对"合并后的 .ino"会**额外跑一遍** -E -CC(保留注释的预处理,
 *   用来生成函数原型)。在 -CC 模式下 GCC 会把**尾注释当成宏替换表里的一个 token**,
 *   于是 LVGL 里这一处以井号井号拼接的地方(lv_color.h:235)
 *       typedef LV_CONCAT3(lv_color, LV_COLOR_DEPTH, _t) lv_color_t;
 *   就变成 "16 + 尾注释 ## _t", 直接报:
 *       error: pasting "..." and "_t" does not give a valid preprocessing token
 *
 *   ★ 为什么现象很隐蔽 ★
 *     只有 .ino 走 -E -CC; 纯 .cpp 文件(例如 lvgl_port.cpp)用同样的头文件完全正常。
 *     所以"LVGL 库编译通过、单独的 .cpp 也编译通过"都不能说明 .ino 一旦
 *     include 了 lvgl.h 会不会炸 —— 必须真的往 .ino 里加 include 试一次才知道。
 *   修法: 值写干净, 说明放上面。
 * =========================================================================== */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP   0       /* 本机序; display.cpp 已 setSwapBytes(true) */
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* 1 = 用自定义 malloc/free。全部走 PSRAM, 不占内部 DRAM。 */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp32-hal-psram.h>
#define LV_MEM_CUSTOM_ALLOC   ps_malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC ps_realloc

/* 渲染中间缓冲个数。默认 16, 每个约 1~2KB; 内部 RAM 紧张的话可以降, 先保持默认。 */
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*====================
   HAL SETTINGS
 *====================*/
/* 默认刷新周期。主循环每 4~5ms 调一次 lv_timer_handler(), 真正重绘按这个周期走。 */
#define LV_DISP_DEF_REFR_PERIOD  30      /* [ms] */
/* 输入设备读取周期。EC11 的事件由 input.cpp 的 1ms 任务入队, 这里 30ms 取一次。 */
#define LV_INDEV_DEF_READ_PERIOD 30      /* [ms] */

/* 心跳直接取 millis(), 不需要 lv_tick_inc() */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

#define LV_DPI_DEF 130

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/
#define LV_DRAW_COMPLEX 1           /* 圆角/阴影/蒙版; 关掉能再省一点 Flash */
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_DISP_ROT_MAX_BUF (10*1024)

/* GPU: 全部关掉(ESP32-S3 没有这些) */
#define LV_USE_GPU_ARM2D 0
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_RA6M3_G2D 0
#define LV_USE_GPU_SWM341_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0

/*-------------
 * Log / 断言
 * 全部关掉: 省 Flash, 也避免和我们的 BOOTLOG 抢串口
 *-------------*/
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL          0
#define LV_USE_ASSERT_MALLOC        0
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_USER_DATA 0

/*==================
 * FONT / TEXT
 *==================*/
/* 保留一个内置英文字体当 LV_FONT_DEFAULT(主题/默认样式要用)。
 * 中文一律走 ui_lvgl.cpp 里的自定义 lv_font_t(DotFont 桥接)。 */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0
#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
/* 缺字不画占位方框(占位框要额外代码; 缺字由我们自己的桥接决定) */
#define LV_USE_FONT_PLACEHOLDER 0

/*==================
 * TEXT SETTINGS
 *==================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*==================
 * WIDGETS   <- Flash 的主要开销在这里, 只留真正要用的
 *==================*/
#define LV_USE_ARC        0
#define LV_USE_ANIMIMG    0
#define LV_USE_BAR        1
#define LV_USE_BTN        1   /* lv_list 依赖它 */
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        0
#define LV_USE_LABEL      1   /* 一切文字都靠它 */
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE       0
#define LV_USE_ROLLER     1   /* 备用: 更像"旋钮选择器"的控件 */
#define LV_ROLLER_INF_PAGES 7
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SPAN       0
#define LV_USE_SPINNER    0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        0
#define LV_USE_LIST       1   /* 第 1 阶段的主控件: 可旋转选择的列表 */
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINBOX    0
#define LV_USE_CALENDAR   0
#define LV_USE_QRCODE     0
#define LV_USE_SNAPSHOT   0
#define LV_USE_GRIDNAV    0
#define LV_USE_FRAGMENT   0
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_MSGBOX     0

/* 布局: list 内部用 flex, 必须留; grid 关掉 */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/*==================
 * THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    /* 深色主题: 和 config.h 的 COL_BG(纯黑)一致, 夜间看着舒服 */
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO  0

/*==================
 * LAYOUTS / OTHERS
 *==================*/
#define LV_USE_OBJ_ID 0
#define LV_USE_OBJ_ID_BUILTIN 0

/* 文件系统: 一律不用 LVGL 的 FS(我们的 SD 访问全部走 sd_mutex + 自己的代码) */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_LITTLEFS 0

/* 图片解码器: 第 1 阶段一个都不用 */
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_RLOTTIE 0
#define LV_USE_FFMPEG 0

/*------------------
 * EXAMPLES / DEMOS
 * (arduino-cli 本来就不会编译 src/ 之外的东西, 这里显式关掉表个态)
 *-----------------*/
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif /* LV_CONF_H */
