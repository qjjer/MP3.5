// ===========================================================================
//  tft_setup.h  --  bookreader 专用 TFT_eSPI 配置
//
//  机制: TFT_eSPI.h 里有
//        #if __has_include(<tft_setup.h>)
//          #include <tft_setup.h>
//          #define USER_SETUP_LOADED     // -> 阻止再加载全局 User_Setup.h
//        #endif
//  所以只要这个文件存在, 你库里的 User_Setup.h 就完全不会被用到, 那边一行都不用改。
//
//  ★★ 本文件的配置 = "已被证明能点亮屏幕" 的那份 (你的 diag_display 诊断固件) ★★
//     请先只烧这一版拿启动日志, 不要再改任何东西。
// ===========================================================================

#define USER_SETUP_INFO "bookreader ST7789 (proven-working config)"

// ---------------------------------------------------------------------------
// 驱动与分辨率
// ---------------------------------------------------------------------------
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_RGB_ORDER TFT_RGB   // 红蓝反了就改成 TFT_BGR

// ---------------------------------------------------------------------------
// 引脚
// ---------------------------------------------------------------------------
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

// ★★ TFT_MISO 必须定义成 SD 卡的 MISO(GPIO13) ★★
//
// 踩过的坑: 不定义它时, ESP32-S3 的 Processors/TFT_eSPI_ESP32_S3.h 里有
//     #if (TFT_MISO == -1)
//       #undef  TFT_MISO
//       #define TFT_MISO TFT_MOSI     // 拿 MOSI 的管脚号当 MISO!
//     #endif
// 于是 TFT_eSPI::init() 执行 spi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1) 时,
// 会把 SPI3 的 MISO 绑到 GPIO 11; 而 SD 卡的 MISO 是 GPIO 13 ——
// **卡往 13 发数据、SPI 却在 11 上采样, 结果卡插着也永远检测不到(cardType=NONE)**。
//
// 因为 SD 卡现在跟随屏幕用的同一条总线(SPI3), 所以这条总线的 MISO 必须是 GPIO13。
// 屏幕是只写的(SDO 悬空), MISO 绑到 13 对它没有任何影响。
#define TFT_MISO 13

// ★ 这里**故意不定义 USE_HSPI_PORT**。
//   你全局 User_Setup.h 里的 USE_HSPI_PORT 在本 sketch 中不生效(被 USER_SETUP_LOADED
//   整个挡掉), 所以不会再触发你说的那个启动报错。
//   如果这一版又出现启动报错, 请把串口内容发我 —— 那说明报错来自别处。
#define USE_HSPI_PORT
// ---------------------------------------------------------------------------
// 字体: 中文用 SD 点阵字库, 这里全部关掉省 Flash
// ---------------------------------------------------------------------------
#define LOAD_GLCD
// #define LOAD_FONT2
// #define LOAD_FONT4
// #define LOAD_FONT6
// #define LOAD_FONT7
// #define LOAD_FONT8
// #define LOAD_GFXFF
// #define SMOOTH_FONT

// ---------------------------------------------------------------------------
// SPI 速度
//
// ★ 这个值决定"一帧要多久", 直接决定界面跟不跟手 ★
//   TFT_eSPI 的频率是**编译期**写死的, 没有运行时接口, 所以只能在这里改。
//   一帧 = 240x320x2 = 153600 字节:
//       27MHz -> 约 45.5 ms   (原值, 一帧就占掉 45ms, 是"不跟手"的主要原因之一)
//       40MHz -> 约 30.7 ms
//       60MHz -> 约 20.5 ms   (ST7789 规格上限 62.5MHz, 但杜邦线可能不稳)
//   先取 40MHz 这一档(比原值高、又没到极限)。若出现花屏/闪屏/白屏,
//   退回 27000000; 若 40MHz 稳定且仍嫌慢, 可再试 60000000。
// ---------------------------------------------------------------------------
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
