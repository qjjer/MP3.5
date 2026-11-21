/*
 * config.h — 全局引脚定义与常量
 *
 * 目标硬件: ESP32-S3-N16R8 (16MB Flash + 8MB OPI PSRAM)
 * 屏幕 ST7789 240x320 SPI 与 MicroSD 共用 SPI 总线(见 sd_mutex.cpp 说明)
 */
#pragma once

// ===========================================================================
// 注: 库的调试日志(CORE_DEBUG_LEVEL)不能在这里开
// ===========================================================================
// 曾经想在这里 #define CORE_DEBUG_LEVEL 3 来打开 ESP32-audioI2S 的
// AUDIO_LOG_ERROR/WARN(它们被 Audio.h:631-644 用 CORE_DEBUG_LEVEL 卡着,
// 平台默认是 -DCORE_DEBUG_LEVEL=0, 所以真实错误一条都不打印)。
//
// 但**这在这里是无效的**: 库的 Audio.cpp 从不 include 本文件, 它编译时看不到
// 这个宏。正确做法是在库自己的编译单元里改(见音频库 Audio.cpp 顶部的说明)。
// 留着这个无效的 define 只会误导后来人, 所以删掉了。

#include <Arduino.h>

// ===========================================================================
// 0. UI 后端开关(两套 UI 并存, 编译期切换)
// ===========================================================================
// 0 = 现有自绘 UI(ui.cpp —— 唯一已被实机验证能跑的一套, 保底)
// 1 = LVGL 8.3.11(ui_lvgl.cpp + lvgl_port.cpp)
//
// ★ 为什么默认 0 ★
//   这一版是"能给用户用的那一版"。LVGL 是全新路径, 没上机验证过之前不能当默认,
//   出问题必须能一行改回来。改成 1 之后旧 UI 的代码**一行都不会少**(两条路径都编译),
//   只是主循环改走 LVGL 的分支(接线在 bookreader.ino 里, 由 Lead 落地)。
//
// ★ 关掉(=0)时 LVGL 连编译都不会 ★
//   lvgl_port.cpp / ui_lvgl.cpp 里所有 #include <lvgl.h> 都在 #if UI_BACKEND_LVGL 之内,
//   所以 arduino-cli 的依赖扫描看不到 lvgl, 那个库根本不会被编译 —— 这一版的固件
//   与加 LVGL 之前**逐字节等价**, Flash/时间都不变。
//
// 用 #ifndef 包住: 允许临时用 -DUI_BACKEND_LVGL=1 编译验证, 不必改文件。
#ifndef UI_BACKEND_LVGL
// ★ 默认必须是 0 ★
//   上面写了"没上机验证过之前不能当默认", 但这里一度被设成 1 且当时 LVGL 路径
//   编译不过 —— 结果整棵树编不出来, 用户会烧到一个坏固件。
//   所以固定为 0; 开发 LVGL 时用 -DUI_BACKEND_LVGL=1 编译, 上机验证通过后再改这里。
#define UI_BACKEND_LVGL 0
#endif

// ===========================================================================
// 1. 显示相关开关
// ===========================================================================
// 显示后端: 1 = TFT_eSPI     (推荐, 配 User_Setup.h, 支持 SPI DMA + Sprite)
//           2 = Adafruit_ST7789 + Adafruit_GFX (备选, 不用改库文件)
// 两个后端都提供 16bit 全屏画布, font/txt_reader 等代码完全不用改。
#define DISPLAY_BACKEND 1

// TFT_eSPI: 0 = 普通阻塞式刷屏(默认, 稳)
//           1 = 用 initDMA() 走 SPI DMA 刷屏(性能更好, 但见下方警告)
//
// ★ 为什么默认关掉 DMA ★
//   TFT_eSPI 的 initDMA() 内部会调 spi_bus_initialize(), 而总线此时已经被
//   TFT_eSPI::init() 里的 spi.begin() 初始化过了。该函数在 ESP_ERROR_CHECK()
//   保护下, 一旦返回错误就 **abort 重启** —— 现象是"屏幕亮一下然后灭"。
//   实测在本项目(S3 + 与 SD 共用总线)上就是这个结果, 所以默认关闭。
//   关掉之后刷屏走 pushImage(), 功能完全正常, 只是 CPU 占用高一些。
//   如果你想试 DMA: 改成 1, 若出现"亮一下即灭", 就是它, 改回 0。
#define DISPLAY_USE_DMA 0

// 0 = 整屏帧缓冲放 PSRAM, 一页一次 SPI 刷完(推荐, 无撕裂, 锁占用短)
// 1 = 横向条带缓冲放内部 RAM, 适合 PSRAM 不可用的板子(兼容路径)
#define DISPLAY_STRIP_MODE 0

// ===========================================================================
// 2. 引脚定义(严格按你的接线, 不要随意改)
// ===========================================================================
// ---- ST7789 (2 寸 240x320, SPI) ----
#define PIN_TFT_MOSI 11  // SDA, 与 SD 共用
#define PIN_TFT_SCK  12  // SCL, 与 SD 共用
#define PIN_TFT_CS   10
#define PIN_TFT_DC    9
#define PIN_TFT_RST   8
#define PIN_TFT_BL   -1  // -1 = 背光直接接 3.3V 常亮(你的接法)

// ---- MicroSD (SPI, 与屏幕共用 MOSI/SCK) ----
#define PIN_SD_MOSI 11  // 与 TFT 共用
#define PIN_SD_SCK  12  // 与 TFT 共用
#define PIN_SD_MISO 13  // 仅 SD 使用
#define PIN_SD_CS   14

// ---- PCM5102A (I2S, 独立外设, 不占 SPI 带宽) ----
#define PIN_I2S_BCK  17
#define PIN_I2S_LCK  16  // LRCK / WS
#define PIN_I2S_DIN  15

// ---- PCM5102A 的 XSMT(软静音)控制脚 ----
// ★ 没声音的头号原因 ★
//   PCM5102A 的 XSMT 脚**必须为高电平才出声**, 悬空/低电平时芯片一直静音。
//   市面上常见的红色 PCM5102A 模块上, XSMT 往往是悬空的 —— 这时解码在正常跑、
//   I2S 数据也在发, 但**一点声音都没有**, 极难从"播放已开始"判断出来。
//   两个办法(任选其一):
//     a) 直接把这个脚用线接到模块的 3.3V  (最简单, 推荐先用这个验证)
//     b) 在这里填一个空闲 GPIO, 由程序开机拉高(注意别和别的脚冲突)
//   注意: 模块上还有个 SCK 脚, 必须接 GND(让芯片从 BCK 自行恢复主时钟)。
//   若找不到 XSMT 丝印: 它通常就在 SCK/FMT/FLT/DEMP 那排焊盘里。
#define PIN_I2S_XSMT  -1   // -1 = 不由程序控制(默认, 请手动接 3.3V)
//
//   ★ 待机底噪(2026-10-03)★
//   XSMT 有效之后 PCM5102A 就一直在工作, 而 I2S 的 BCK/LRCK **从开机起一直在跑**
//   (库在 setPinout()->I2S_init() 里 i2s_channel_enable 之后就不管了), 于是
//   "不放歌、停在主菜单"时耳机里会有嘶嘶的电流声。
//   两条消除路线, 见下面 AUDIO_I2S_IDLE_MUTE 的说明:
//     ① 空闲 N 秒后调库的 I2Sstop()(= i2s_channel_disable)停掉时钟 —— 最彻底,
//        但**库把 I2Sstart()/I2Sstop() 声明在 private 区**(Audio.h:221 private ...
//        281-282 ... 293 才是 public), sketch 直接调用编不过, 需要先给库加 2 行
//        `public:`/`private:`。所以开关默认 0。
//     ② 空闲 N 秒后把 XSMT 拉低(芯片软静音)—— **不需要改库**, 但需要一根线:
//        把模块的 XSMT 从 3.3V 改接到 PIN_I2S_XSMT 指定的 GPIO。
//   ⇒ XSMT 仍然必须有效(接 3.3V 或由程序拉高), 否则**完全没有声音**。
//     两者分工: XSMT 管"能不能响", 空闲静音管"待机时有没有底噪"。

// 另外: 这块模块上通常还有这几个配置脚, 建议的接法
//   SCK -> GND      (必须; 让 PCM5102A 用内部 PLL 从 BCK 恢复主时钟)
//   FMT -> GND      (I2S 格式)
//   FLT -> GND      (普通延迟)
//   DEMP-> GND      (关闭去加重)

// ---- EC11 旋转编码器 ----
#define PIN_ENC_A     1
#define PIN_ENC_B     2
#define PIN_ENC_SW    3  // 按键, 另一端接 GND

// ---------------------------------------------------------------------------
// ★ 旋钮灵敏度: 转多少格才算"一步" ★
//
// 背景(用户实测反馈): "太灵敏了" —— 轻轻碰一下就跳好几行, 根本停不到想选的那一项。
//
// 原理: EC11 这类机械编码器每转过**一档(detent, 手感上"咔"一下)**所产生的
//       正交跳变次数, 因型号而异, 常见两种:
//         ① 每档 2 个跳变 (20 定位/20 脉冲)  -> 阈值 2 就是"一档一步"
//         ② 每档 4 个跳变 (20 定位/40 脉冲, 最常见) -> 阈值 4 才是"一档一步"
//       EC11 规格书里的 "脉冲数(Pulses)" 指的就是每转的完整方波周期数,
//       4 倍频解码后跳变数 = 脉冲数 x 4, 而定位数(Detents)通常 = 脉冲数。
//       => 每档跳变数 = 4 x 脉冲数 / 定位数。
//          20脉冲/20定位 => 4 跳变/档   ← 绝大多数 EC11 是这种
//          20脉冲/40定位 => 2 跳变/档
//
// 之前代码写死 ±2, 所以对"每档 4 跳变"的编码器来说**每档会发 2 步** —— 正好是
// 用户感觉到的"太灵敏"。现在改成可调, 调大 = 更迟钝(需要转更多才动一格),
// 调小 = 更灵敏。
//   4 = 一档一步(推荐起点)
//   2 = 灵敏度翻倍(原来写死的值)
//   8 = 两档一步(如果 4 还是嫌跳)
//
// 怎么定: 看串口里那一行 `[enc] 旋钮: 事件=N` (见下文 ENCODER_DEBUG)。
//   转一大格(咔一下), 事件数应该 = 1。若 = 2 就把这个值再加倍。
// ---------------------------------------------------------------------------
#define ENCODER_STEPS_PER_DETENT  4

// ★ 一档内的"锁定窗口"(ms) ★
// 机械编码器在定位点附近触点会抖动, 可能被状态表当成额外的合法跳变, 导致
// "转一格跳两格"。发完一步后, 在这个窗口内不再累计 —— 抖动被直接吃掉。
// 0 = 关闭。太大(>20ms)会让快速旋转丢步, 建议 3~10。
#define ENCODER_SETTLE_MS  6

// ENCODER_DEBUG: 1 = 每秒打一行旋钮事件计数, 用来标定上面那个阈值。
#define ENCODER_DEBUG  0

// ---- 开发板自带的 BOOT 键 ----
// ESP32-S3 开发板的 BOOT 键就接在 GPIO0 上, 已经焊好了。
// 连上它就有了一个**不用接线**的备用按键: 选曲、确认、重试都能用。
// 它没有内部上拉问题(外部已有), 但 S3 的 GPIO0 是 strapping 脚,
// 上电瞬间被按下会进下载模式 —— 所以只在运行期间使用, 不影响正常启动。
#define PIN_BTN_BOOT  0

// ---- 电池检测 (10k/10k 分压中点) ----
#define PIN_BAT_ADC   6
// 电池电压 = ADC 读数 * BAT_DIVIDER。10k/10k 分压 => 2.0
// 实测万用表电压后按需微调这个系数
#define BAT_DIVIDER   2.0f
#define BAT_FULL      4.15f  // 显示满格
#define BAT_EMPTY     3.30f  // 显示空格
// ADC 校准: 用曲线拟合, 比默认的 raw->mV 准很多
#define BAT_ADC_ATTEN ADC_11db

// ===========================================================================
// 3. 屏幕参数
// ===========================================================================
#define TFT_W 240
#define TFT_H 320
// ST7789 的可见区在 240x320 面板上通常有 0 偏移; 若图像整体偏移再改这两个值
#define TFT_X_OFFSET 0
#define TFT_Y_OFFSET 0
#define TFT_ROTATION 0  // 0/2 = 竖屏, 1/3 = 横屏

#define SPI_FREQ_TFT (40 * 1000 * 1000)  // ST7789 40MHz 稳
// SD 频率。
// 说明: 排障期间曾把它从 25MHz 降到 10MHz, 想验证"读到的数据有错导致解码失败"。
// 结论是**降频没有解决问题**(真正的病根是音频库的文件头门限 + Audio::loop 没被调用),
// 所以这里恢复 25MHz。低频率会让读字形/翻页/扫描全都变慢, 是纯粹的损失。
// 若以后出现读卡不稳(偶发挂载失败/花屏), 优先降到 10MHz 排除线路问题。
#define SPI_FREQ_SD  (25 * 1000 * 1000)

// ===========================================================================
// 4. 阅读区排版参数
// ===========================================================================
#define FONT_W         24  // 字库点阵宽(必须与字库文件一致)
#define FONT_H         24  // 字库点阵高
#define TEXT_MARGIN_X   6  // 左右页边距
#define TEXT_TOP_Y      8  // 正文首行基线框顶部
#define LINE_GAP        2  // 行间距
#define STATUS_BAR_H   20  // 底部状态栏高度(电量/页码/文件名)

#define LINE_H      (FONT_H + LINE_GAP)                                       // 26
#define TEXT_AREA_W (TFT_W - 2 * TEXT_MARGIN_X)                               // 228
#define COLS        (TEXT_AREA_W / FONT_W)                                    // 9 (228/24=9.5)
#define ROWS        ((TFT_H - TEXT_TOP_Y - STATUS_BAR_H) / LINE_H)            // 11
#define CHARS_PER_PAGE (COLS * ROWS)

// 章节目录一屏能显示几条
#define TOC_ROWS    ((TFT_H - (4 + FONT_H + 6) - STATUS_BAR_H) / (FONT_H + 2))

// ===========================================================================
// 5. 字库 (SD 卡上的 24x24 点阵字库, 由 tools/build_font.py 生成)
// ===========================================================================
// 字库路径: 程序会**先扫描 FONT_DIR 下任意 .f24 文件**(名字随你取),
// 一个都没有时才退回用下面的 FONT_PATH。
#define FONT_DIR       "/fonts"
#define FONT_PATH      "/fonts/gbk24.f24"   // 回退默认值
#define FONT_INDEX_CACHE 1  // 1 = 启动时把字形索引表读进 PSRAM(约 84KB, 强烈建议开)

// 字形位图缓存槽数(每个槽 24*24 灰度 4bit = 288B 或 1bit = 72B)
// 一页大约 100 个不同汉字, 256 槽足够挡住 80%+ 的重复读取
#define GLYPH_CACHE_SLOTS 256

// ---------------------------------------------------------------------------
// 5.1 常用字"瘦身子集"字库(★ 解决翻页慢的主力 ★)
// ---------------------------------------------------------------------------
// ★ 为什么需要它(实测) ★
//   这张卡顺序读只有 **38 KB/s**(读 4KB=106ms、128KB=3285ms, 与块大小无关)。
//   全量字库 sourcehan24_aa.f24 = 6.49MB:
//       4bpp 6.44MB -> PSRAM 装不下
//       2bpp 3.38MB -> 装得下, 但要读 **89 秒**
//   瘦身子集(2bpp × GB2312全集∪语料)只有 **0.94MB -> 约 25 秒**,
//   读进 PSRAM 之后: 常用字零 SD 读, 生僻字自动回落 FONT_PATH 上的全量字库按需读。
//
// ★ 要放哪些文件(生成脚本 bookreader/tools/make_subset_font.py) ★
//   SD 卡: /fonts/subset24.f24   (988 KB, 6769 字, 2bpp)   <- 常用字, 进 PSRAM
//          /fonts/sourcehan24_aa.f24 (6.49MB, 4bpp)        <- 生僻字回落用(必须保留)
//   只放子集=生僻字会缺; 只放全量=回到每次翻页读 SD 的老问题。
//
// 没有子集文件时: 打印一行提示, 然后原样退回"全量搬运/按需读"的老路径, 功能不退化。
#define FONT_SUBSET_ENABLE 1
#define FONT_SUBSET_PATH   "/fonts/subset24.f24"

// ---------------------------------------------------------------------------
// 5.2 ★ 把常用字子集**编进固件**(当前主用方案) ★
// ---------------------------------------------------------------------------
// bookreader/emb_font_subset.c 里是一份 1.14MB 的 4bpp 子集字库
// (3921 字形 = GB2312 一级 ∪ 两本语料的全部非 ASCII 字符, 由 tools/embed_font.py 生成)。
//
// ★ 它比"从 SD 读子集"好在哪(全是实测) ★
//   1) 零等待: SD 读 0.94MB 要 25 秒; Flash 内存映射是**直接取指针**。
//   2) 不依赖卡: 卡慢/换卡/卡拔了都不影响常用字。
//   3) 不占 PSRAM: PSRAM 留给音频缓冲(655KB)/帧缓冲/EPUB 图片(峰值 1.2MB)。
//   代价: 占 1.14MB Flash(4bpp)。若要把 Flash 省给 LVGL 后续阶段:
//         python tools/make_subset_font.py --bpp 2 ... -> 约 0.57MB(省 565KB),
//         代价是抗锯齿从 16 级降到 4 级(font.h 里对 2bpp 的评价是"视觉几乎无差")。
//
// 三者优先级: 嵌入(本开关) > SD 子集(FONT_SUBSET_PATH) > 全量搬运/按需读。
// 关掉这个开关时: font.cpp 不再引用 emb_font_subset, 但那个 .c 文件仍会被编译;
// 想彻底省 Flash 就把 emb_font_subset.c 从 sketch 目录移走(届时保持本开关为 0)。
#define FONT_EMBEDDED_ENABLE 1

// ===========================================================================
// 6. 任务 / 双核分工
// ===========================================================================
// ★ 这一节曾经配错, 是"界面不跟手"的根因 ★
//
// 事实: Arduino 的 loopTask(我们的主循环)在 ESP32-S3 上**钉在 Core 1**。
// 之前把 AUDIO_TASK_CORE 也设成 1, 于是音频任务(库内部优先级 2)和主循环
// (loopTask 优先级 1)同核, 音频任务**持续抢占**主循环 —— 播放时界面就卡。
//
// 库自己的源码就写明了正确做法(Audio.cpp:7878 原话):
//   "Recommendation: If the ARDUINO RUNNING CORE is 1,
//    the audio task should be core 0 or vice versa"
// 也就是**主循环和音频任务必须分到不同核**。
//
// 最终分工:
//   Core 1: 主循环(loopTask, 优先级 1) + 输入任务(优先级 2)
//           —— 输入任务优先级略高于主循环, 按键/旋转永远能及时响应,
//              而它的体力活只有"读两个 GPIO + 入队", 不会拖慢主循环。
//   Core 0: 音频任务(库内部优先级 2)独占, 解码/重采样不被打扰, 也就不爆音。
#define AUDIO_TASK_CORE    0      // 音频独占 Core 0(与主循环分开)
#define INPUT_TASK_CORE    1      // 输入跟着主循环, 略高于它的优先级
#define INPUT_TASK_PRIO    2
#define UI_TASK_CORE       1      // 主循环实际所在核(仅作记录/文档用)
#define UI_TASK_PRIO       1      // Arduino loopTask 的优先级

// UI 任务栈(读文件 + 排版 + 渲染都比较吃栈, 给足 8KB)
#define UI_TASK_STACK      8192

#define SD_LOCK_TIMEOUT_MS 3000   // 等 SPI/SD 锁的最长时间, 超时返回失败而不是死等

// ===========================================================================
// 7. 音频 (ESP32-audioI2S 3.4.x)
// ===========================================================================
#define AUDIO_MAX_TRACKS   256    // 播放列表上限
#define AUDIO_VOLUME_MAX   21     // 库的音量范围是 0~21
#define AUDIO_VOLUME_INIT  12
// ★ 采样率策略: 固定 44.1kHz (用户要求"把输出固定") ★
//   原来是 SR_ORIGIN(跟随源)。实测问题: 换成 48k/96k 音源会**静默走重采样器**,
//   而重采样是纯额外 CPU 开销 —— 本机 CPU 已经很紧(实测解码只有实时的 13.5%),
//   这种不确定性会表现成"某首歌特别卡"。
//   固定成 SR_44100 后行为确定:
//     44.1k 源 -> 不重采样直通(和原来完全一样)
//     48k/96k 源 -> 统一重采样到 44.1k
//   代价: 96k 音源降到 44.1k(听感基本无差); 收益: 不再有随机卡顿。
//   audio_player.cpp 里显式调用 setOutputSampleRate(Audio::SR_44100)。
// 前提: PCM5102A 的 SCK 接 GND(你的接法), 它从 BCK 自恢复主时钟。

// ===========================================================================
// 8. 颜色 (RGB565)
// ===========================================================================
#define COL_BG      0x0000  // 黑底
#define COL_FG      0xE7E0  // 接近纸白
#define COL_DIM     0x8C71  // 灰(状态栏)
#define COL_ACCENT  0x05FF  // 青蓝(当前选中项)

// ===========================================================================
// 9. SD 通路自检(build_sd 诊断固件用, 见 bookreader/sd_bench.cpp)
// ===========================================================================
// 背景: 实测 [font] 首块顺序读只有 19 KB/s(96KB 花 5 秒), 而 SPI 跑在 25MHz,
//       512 字节理论 164us, 实测 ~26ms —— 慢了约 100 倍。
//       上层的字形预取/缓存/刷屏优化都被这条通路卡死, 所以先用一次上机把
//       "哪一层慢"量出来。
//
// SD_BENCH_ENABLE: 1 = displayInit() 末尾跑一遍 SD 自检(占启动时间, 全量约 30~60 秒)。
//                  诊断完改回 0 即可(留着也只是每次启动多花点时间)。
// SD_BENCH_BUDGET_MS: 自检总预算, 超了自动跳过剩余项, 不会把启动变成假死。
// SD_SPI_REPRIME: 1 = 每次整帧刷屏后把共享 SPI 重新配置成 SD 的时序(见 display.cpp)。
//                  这是"刷屏污染 SPI/SD 状态"这条假设的修复开关。
//                  **先看 [sdbench] 的 G 组**: 若"刷屏+重设总线"明显快于"刷屏后",
//                  就把它改成 1 再烧一次; 若三者一样, 说明这条假设不成立, 保持 0。
#define SD_BENCH_ENABLE     1
#define SD_BENCH_BUDGET_MS  90000
#define SD_SPI_REPRIME      0

// ===========================================================================
// 9. 阅读存档(state_store.*) —— 记住上次看的书和页码
// ===========================================================================
// ★ 这一节是 ui-perf 加的, 只增不改: 上面的段落一行都没有动 ★
//
// STATE_STORE_ENABLE: 1 = 用 NVS 记住"上次打开的书 + 页码", 主菜单出现第 3 项
//                        "继续阅读"; 0 = 整个功能编译期关闭(state_store.cpp 给出
//                        一组空实现, 上层接线代码不用改)。
//                     走独立 flash 分区(nvs, 0x9000/0x5000), 不碰 SD/SPI,
//                     不需要 sd_mutex, 也不占 PSRAM。
// STATE_STORE_DEBOUNCE_MS: 最后一次变更后多久才真正写 Flash(防抖, 保护 Flash 磨损)。
// STATE_STORE_MIN_WRITE_GAP_MS: 两次写入之间的硬性最小间隔。
// 注意: 这两个值在 state_store.h 里用 #ifndef 包着, 也可以用 -D 覆盖。
#ifndef STATE_STORE_ENABLE
#define STATE_STORE_ENABLE 1
#endif
#ifndef STATE_STORE_DEBOUNCE_MS
#define STATE_STORE_DEBOUNCE_MS 2500
#endif
#ifndef STATE_STORE_MIN_WRITE_GAP_MS
#define STATE_STORE_MIN_WRITE_GAP_MS 1500
#endif

// ===========================================================================
// 10. 音频诊断开关（默认全关 = 串口安静；出问题时一条命令把证据打开）
// ===========================================================================
// 【背景】音频"一顿一顿/慢放"那件事排查了很久, 期间到处加了诊断输出。现在已修好
// —— 修法是 sd_diskio.cpp 里两行 `delay(f == 0 ? 1 : 100)`(每次 disk_read 少等
// 99ms, 实测 38 KB/s -> >1000 KB/s)。用户嫌串口刷屏吵, 所以下面默认全关。
//
// ── AUDIO_DIAG_LOG ────────────────────────────────────────────────────────
//   0(默认) = 安静, 但**留了安全网**: 只在真的异常时打一行 [afeed]
//             —— 判据是"解码实时率 rt < 900"(跟不上播放 = 会一顿一顿)或抢锁超时,
//             最快 5 秒一行; 一切正常时一行都不打。
//   1       = 详细: 每秒一行 [afeed](loop/s / to / wait / busy / rt / inbuf)
//             + 库里的 [audiotime](每次 SD 读的耗时)。
//
//   ★ 打开方式(二选一)★
//     ① 一条命令, 不改文件(推荐, sketch 和库同时生效):
//        arduino-cli compile -b "<board>" --build-path <dir> \
//          --build-property "compiler.cpp.extra_flags=-DAUDIO_DIAG_LOG=1" <sketch路径>
//     ② 最保险: 把下面的 0 改成 1, 重编。
//
//   ⚠️ 为什么库里也有一份: 库(D:\...\ESP32-audioI2S\src\Audio.h)看不到 sketch 的
//      config.h, 所以它在自己头文件里也定义了同名默认值 0(#ifndef 守卫)。
//      两边同名 + #ifndef ⇒ 改这里、或用 -D 覆盖, 都能同时作用于 sketch 和库。
//
// ── AUDIO_FASTREAD_ENABLE ─────────────────────────────────────────────────
//   快路径(原始多扇区读)总开关, **默认 0 = 关**。定义在 bookreader/sd_fastread.h
//   顶部(同名宏 + #ifndef 守卫, 可用 -DAUDIO_FASTREAD_ENABLE=1 覆盖)。
//   关闭原因: 音频的修复完全来自上面那两行 delay 补丁, 快路径始终没能让卡应答
//   (上机: `读命令失败 CMD17 LBA=32 R1=0x20 raw=FF 20 00 00` = 卡根本没应答)。
//   关掉回收 ~12 KB Flash / ~5 KB DRAM; 代码完整保留, 改回 1 即可恢复。
#ifndef AUDIO_DIAG_LOG
#define AUDIO_DIAG_LOG 0
#endif

// ── AUDIO_I2S_IDLE_MUTE ───────────────────────────────────────────────────
//   1 = 空闲 3 秒后调库的 I2Sstop() 停掉 I2S 时钟(消除待机底噪)。
//   **现在是 1(已启用)**: Lead 已授权给库加下面那 2 行"放开访问级别"的补丁
//   (只改访问级别, 函数体一律不碰, 零行为变化)。不加补丁的话 sketch 编不过:
//   `error: 'esp_err_t Audio::I2Sstart()' is private within this context`
//   —— 库把 `I2Sstart()/I2Sstop()` 声明在 `Audio.h:281-282`, 而 221 行起是 `private:`。
//   库侧补丁(已打; **还原 = 删掉 public:/private: 这两行**):
//       // Audio.h, `bool initializeDecoder();` 之后
//       public:   // 【本地补丁-I2SMUTE】让 sketch 能停 I2S 时钟消底噪
//         esp_err_t I2Sstart();
//         esp_err_t I2Sstop();
//       private:  // 恢复原访问级别
//   想临时关掉这条(回到"待机有底噪"的老行为): 加 -DAUDIO_I2S_IDLE_MUTE=0 重编。
//   兜底路线(本次未采用, 留作万一): 把模块 XSMT 从 3.3V 改接到一个 GPIO 并填进
//   PIN_I2S_XSMT, audio_player.cpp 会自动改为"空闲拉低 XSMT 软静音", 不需要改库。
#ifndef AUDIO_I2S_IDLE_MUTE
#define AUDIO_I2S_IDLE_MUTE 1
#endif
