/*
 * image_view.cpp - 插图页渲染实现
 *
 * ★ 根因: 库的"按文件名"版本会把 '/'-开头的路径送到 SPIFFS ★
 *   TJpg_Decoder 的 drawJpg(x, y, pFilename) / getJpgSize(w, h, pFilename) 在
 *   ESP32 上会**看首字符是不是 '/'** 来决定用哪个文件系统:
 *       TJpg_Decoder.cpp:168-172
 *         #if defined(ESP32)
 *         #if defined(TJPGD_LOAD_SD_LIBRARY)
 *             if (*pFilename == '/')
 *         #endif
 *             return drawFsJpg(x, y, pFilename);      // <- 送到 LittleFS/SPIFFS
 *         #endif
 *   而我们的插图路径是 "/cache/<书名>/img_NN.jpg" —— 首字符正好就是 '/',
 *   于是被送进 drawFsJpg(), 它默认的文件系统是 **SPIFFS**(TJpg_Decoder.h:91),
 *   本工程从来没有挂载过 SPIFFS, 所以:
 *       TJpg_Decoder.cpp:256  if (!fs.exists(pFilename)) { Serial.println("Jpeg file not found"); return JDR_INP; }
 *   立刻返回 JDR_INP。屏幕上的表现就是 ui.cpp:208-209 那句"插图解码失败"。
 *   getJpgSize(path) 走的是同一条分支(TJpg_Decoder.cpp:212-214), 所以连图多大
 *   都读不出来。库自带的 SD 示例用的是 drawSdJpg()/getSdJpgSize(), 正是为了绕开它。
 *
 * ★ 现在的做法: 读进 PSRAM, 用内存版 API 解码 ★
 *   第 1 步: 用**工程自己挂载的 SD 对象**(和显示库同一条 SPI 总线)把整张图读进
 *            PSRAM —— 这一步持 sd_mutex。
 *   第 2 步: 调用库里的内存版 getJpgSize(w, h, array, size) / drawJpg(x, y, array, size),
 *            完全不碰文件系统。好处:
 *             a) 绕开库对"全局 SD / SPIFFS"的一切假设, 用的一定是我们挂载的那张卡;
 *             b) 解码全程**不持 sd_mutex**(内存版不用总线), 不会再卡住音频和刷屏;
 *             c) 能顺便验证"文件真的存在、读到的字节数和文件大小一致", 失败原因打得清楚;
 *             d) PNG 一起修好 —— PNGdec 的 open() 会**无条件调用 pfnOpen 回调**
 *                (PNGdec.cpp:75), 原来传 NULL 必然崩; 内存版走 openRAM() 没这问题。
 *
 * 诊断一律用 BOOTLOG 而不是 log_e/log_i: 平台的 build.extra_flags 带的是
 * -DCORE_DEBUG_LEVEL=0, log_* 默认什么都不打印, 出问题时串口一片安静。
 */
#include "image_view.h"

#include <string.h>

#include "config.h"
#include "sd_mutex.h"
#include "esp32-hal-psram.h"  // ps_malloc

// TJpg_Decoder 默认不带 SD 支持; 这个宏只为了让它 include <SD.h>。
// 注意: 我们用的一律是**内存版** API, 不再走它"按文件名打开"的那条路(见文件头)。
#define TJPGD_LOAD_SD_LIBRARY
#include <TJpg_Decoder.h>
#include <PNGdec.h>
#include <SD.h>

static Canvas16 *s_target = nullptr;    // 当前正在绘制的画布
static int16_t s_offX = 0, s_offY = 0;  // 图片在画布上的居中偏移(缩放后坐标)

// 单张插图的大小上限。PSRAM 共 8MB, 帧缓冲/字库/音频缓冲已经占掉一块,
// 再大的图直接拒绝, 免得把 PSRAM 掏空导致后面全部失败。
#define IMG_MAX_FILE_BYTES (6u * 1024u * 1024u)

// ★ JPEG 输出的字节序 ★
// 画布(Canvas16)里存的是**本机序**的 RGB565 —— 和 config.h 的 COL_* 常量、
// 字体渲染写进去的值完全一致; 推屏时由 display.cpp 的 tft.setSwapBytes(true)
// 负责在发字节时交换。所以解码器必须输出本机序, 也就是 tjpgd 的 swap = 0:
//     tjpgd.c:944   swap=1 -> *d++ = (w << 8) | (w >> 8);   // 已交换(大端)
//     tjpgd.c:953   swap=0 -> *d++ = w;                     // 本机序 ✔
// 原来这里写 true, 等于和 tft.setSwapBytes(true) 叠了两次, 颜色会翻掉;
// 之前图根本没解出来, 所以这个症状一直没机会暴露。
// 万一上机发现图片偏色(像红蓝调换), 把下面这个宏改成 true 即可对调。
#define IMG_JPEG_SWAP_BYTES false

// 说明: 这里一律用 BOOTLOG 而不是 log_e/log_i。
// 平台的 build.extra_flags 带的是 -DCORE_DEBUG_LEVEL=0, 所以 log_e/log_i
// **默认什么都不打印** —— 插图解码失败时屏幕上只有一句"插图解码失败",
// 串口也一片安静, 根本无法定位。BOOTLOG 直接走 Serial, 永远可见。
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                       \
  do {                                          \
    Serial.printf(fmt "\n", ##__VA_ARGS__);     \
    Serial.flush();                             \
  } while (0)
#endif

bool imageIsSupported(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".png") == 0;
}

// ---------------------------------------------------------------------------
// 第 1 步: 把整个文件读进 PSRAM(唯一需要 SD 卡的一步)
// 成功返回缓冲(调用者 free), 失败返回 nullptr 并把原因写到 *outErr。
// ---------------------------------------------------------------------------
static uint8_t *readImageToPsram(const char *path, uint32_t *outSize, const char **outErr) {
  *outSize = 0;
  *outErr = "未知原因";

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    *outErr = "等 sd_mutex 超时(音频/刷屏占着总线)";
    return nullptr;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    *outErr = "SD.open 失败(文件不存在? 路径不对?)";
    return nullptr;
  }
  if (f.isDirectory()) {
    f.close();
    *outErr = "这是个目录, 不是图片文件";
    return nullptr;
  }

  const uint32_t size = (uint32_t)f.size();
  if (size < 16) {
    f.close();
    *outErr = "文件几乎是空的(插图导出时就没写成功?)";
    return nullptr;
  }
  if (size > IMG_MAX_FILE_BYTES) {
    f.close();
    *outErr = "文件超过单张上限(6MB), 拒绝分配";
    return nullptr;
  }

  uint8_t *buf = (uint8_t *)ps_malloc(size);
  if (!buf) {
    buf = (uint8_t *)malloc(size);  // PSRAM 失败时的兜底(多半也会失败, 但便宜)
  }
  if (!buf) {
    f.close();
    *outErr = "分配解码缓冲失败(PSRAM 不够或碎片化)";
    return nullptr;
  }

  uint32_t got = 0;
  while (got < size) {
    uint32_t want = size - got;
    if (want > 32768u) {
      want = 32768u;  // 分块读, 出问题时能看出读到哪一块断了
    }
    const int n = (int)f.read(buf + got, want);
    if (n <= 0) {
      break;
    }
    got += (uint32_t)n;
  }
  f.close();

  if (got != size) {
    free(buf);
    *outErr = "只读到部分字节(文件被截断? 卡读取错误?)";
    return nullptr;
  }
  *outSize = size;
  return buf;
}

// ---------------------------------------------------------------------------
// JPEG: 解码器按块回调, 我们把每块像素写进画布
// ---------------------------------------------------------------------------
static bool jpgBlockCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data) {
  if (!s_target || !data) {
    return false;
  }
  const int16_t cw = (int16_t)s_target->width();
  const int16_t ch = (int16_t)s_target->height();
  uint16_t *fb = s_target->pixels();
  if (!fb) {
    return false;
  }

  const int16_t px0 = s_offX + x;
  const int16_t py0 = s_offY + y;

  for (uint16_t row = 0; row < h; row++) {
    const int16_t py = py0 + row;
    if (py < 0 || py >= ch) {
      continue;
    }
    const uint16_t *src = data + (uint32_t)row * w;
    uint16_t *dst = fb + (size_t)py * cw;
    for (uint16_t col = 0; col < w; col++) {
      const int16_t px = px0 + col;
      if (px >= 0 && px < cw) {
        dst[px] = src[col];
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// PNG: 逐行回调, 转成 RGB565 后写进画布
// ---------------------------------------------------------------------------
static PNG png;  // 必须声明在回调之前(回调里要用它取行数据)

static uint16_t *s_pngLine = nullptr;  // 行缓冲: 按图宽在 PSRAM 里分配
static int s_pngLineW = 0;

static int pngDrawCallback(PNGDRAW *pDraw) {
  if (!s_target || !s_pngLine) {
    return 0;
  }
  const int16_t cw = (int16_t)s_target->width();
  const int16_t ch = (int16_t)s_target->height();
  const int16_t py = s_offY + (int16_t)pDraw->y;
  if (py < 0 || py >= ch) {
    return 1;  // 跳过这一行, 继续
  }

  const int w = pDraw->iWidth;
  if (w <= 0 || w > s_pngLineW) {
    return 0;
  }
  // iEndianness: 小端(和我们的帧缓冲一致); u32Bkgd = 白底(处理透明)
  png.getLineAsRGB565(pDraw, s_pngLine, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  uint16_t *fb = s_target->pixels();
  if (!fb) {
    return 0;
  }
  uint16_t *dst = fb + (size_t)py * cw;
  for (int col = 0; col < w; col++) {
    const int16_t px = s_offX + col;
    if (px >= 0 && px < cw) {
      dst[px] = s_pngLine[col];
    }
  }
  return 1;
}

// 把 JRESULT 翻译成人话, 串口日志里直接能看出失败在哪一类
static const char *jdrName(JRESULT r) {
  switch (r) {
    case JDR_OK:   return "成功";
    case JDR_INTR: return "解码被输出回调中断";
    case JDR_INP:  return "输入流错误(数据不是JPEG或已损坏)";
    case JDR_MEM1: return "工作区内存不足";
    case JDR_MEM2: return "输入缓冲不足";
    case JDR_PAR:  return "参数错误";
    case JDR_FMT1: return "JPEG 数据格式错误";
    case JDR_FMT2: return "JPEG 格式不支持";
    case JDR_FMT3: return "不是标准 JPEG";
    default:       return "未知错误";
  }
}

// ---------------------------------------------------------------------------
// 用内存版 API 解码 JPEG(不再需要 SD, 也不再需要任何文件系统)
// ---------------------------------------------------------------------------
static bool drawJpegMem(const uint8_t *buf, uint32_t size, uint16_t cw, uint16_t ch, const char *path) {
  if (size < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
    BOOTLOG("[img] !! 数据开头不是 JPEG(SOI FF D8), 实际 %02X %02X —— 文件写坏了? %s", buf[0], buf[1], path);
    return false;
  }

  // ★ 用库的全局实例 TJpgDec, 不要自己 new 一个局部对象 ★
  // 库里 jd_input()/jd_output() 是通过 `TJpgDec.thisPtr` 反查对象的, 而每个
  // TJpg_Decoder 的构造函数都会把 **TJpgDec.thisPtr** 改成自己。局部对象的
  // 构造/析构顺序会把这个指针指来指去(全局对象之间的静态初始化顺序还不确定),
  // 用它的全局实例才是库的用法, 也顺带避免 3.1KB 工作区放在栈上。
  TJpgDec.setCallback(jpgBlockCallback);
  TJpgDec.setSwapBytes(IMG_JPEG_SWAP_BYTES);

  uint16_t iw = 0, ih = 0;
  JRESULT r = TJpgDec.getJpgSize(&iw, &ih, buf, size);
  if (r != JDR_OK || iw == 0 || ih == 0) {
    BOOTLOG("[img] !! 读不出 JPEG 尺寸: getJpgSize(内存版)=%d(%s) 文件=%lu 字节 %s", (int)r, jdrName(r),
            (unsigned long)size, path);
    return false;
  }

  // 选**最小**的降采样档(1/2/4/8), 让缩完的图同时不超过画布的宽和高。
  // (原来的算法只看宽, 1442x2048 会选 1/4 -> 360 宽, 超出 240 只能被裁掉两边)
  uint8_t scale = 8;  // 8 倍是库的极限; 比这还大就只能居中裁一点
  static const uint8_t kScales[4] = {1, 2, 4, 8};
  for (uint8_t i = 0; i < 4; i++) {
    if ((uint32_t)(iw / kScales[i]) <= cw && (uint32_t)(ih / kScales[i]) <= ch) {
      scale = kScales[i];
      break;
    }
  }
  TJpgDec.setJpgScale(scale);

  const int32_t dw = (int32_t)iw / scale;
  const int32_t dh = (int32_t)ih / scale;
  s_offX = (int16_t)(((int32_t)cw - dw) / 2);  // 可能为负 -> 左右对称裁切
  s_offY = (int16_t)(((int32_t)ch - dh) / 2);

  const uint32_t t0 = millis();
  r = TJpgDec.drawJpg(0, 0, buf, size);
  const uint32_t ms = millis() - t0;

  if (r != JDR_OK) {
    BOOTLOG("[img] !! JPEG 解码失败: drawJpg(内存版)=%d(%s) 尺寸=%ux%u 文件=%lu 字节 用时=%lums %s", (int)r,
            jdrName(r), iw, ih, (unsigned long)size, (unsigned long)ms, path);
    return false;
  }
  BOOTLOG("[img] JPEG %ux%u 缩放1/%u -> %ldx%ld @(%d,%d), 文件 %lu 字节, 解码 %lums", iw, ih, (unsigned)scale,
          (long)dw, (long)dh, s_offX, s_offY, (unsigned long)size, (unsigned long)ms);
  return true;
}

// ---------------------------------------------------------------------------
// 用内存版 API 解码 PNG
// ---------------------------------------------------------------------------
static bool drawPngMem(uint8_t *buf, uint32_t size, uint16_t cw, uint16_t ch, const char *path) {
  if (size < 8 || buf[0] != 0x89 || buf[1] != 'P' || buf[2] != 'N' || buf[3] != 'G') {
    BOOTLOG("[img] !! 数据开头不是 PNG(89 50 4E 47), 实际 %02X %02X %02X %02X —— 文件写坏了? %s", buf[0], buf[1],
            buf[2], buf[3], path);
    return false;
  }
  // ★ 不能用 png.open(路径, NULL, ...): PNGdec.cpp:75 会无条件 (*pfnOpen)(...), 传 NULL 必崩
  if (png.openRAM(buf, (int)size, pngDrawCallback) != PNG_SUCCESS) {
    BOOTLOG("[img] !! PNG 头解析失败: %lu 字节 %s", (unsigned long)size, path);
    return false;
  }

  const int iw = png.getWidth();
  const int ih = png.getHeight();
  if (iw <= 0 || ih <= 0) {
    BOOTLOG("[img] !! PNG 尺寸非法: %dx%d %s", iw, ih, path);
    png.close();
    return false;
  }

  // PNGdec 不做降采样, 行缓冲必须按**图片实际宽度**分配。
  // (原来是写死的 static uint16_t line[600], 宽图会直接解码失败)
  s_pngLine = (uint16_t *)ps_malloc((size_t)iw * 2);
  if (!s_pngLine) {
    s_pngLine = (uint16_t *)malloc((size_t)iw * 2);
  }
  if (!s_pngLine) {
    BOOTLOG("[img] !! PNG 行缓冲分配失败(%d 像素), PSRAM 余 %u", iw, (unsigned)ESP.getFreePsram());
    png.close();
    return false;
  }
  s_pngLineW = iw;

  s_offX = (int16_t)(((int32_t)cw - iw) / 2);  // 可能为负 -> 居中裁切
  s_offY = (int16_t)(((int32_t)ch - ih) / 2);

  const uint32_t t0 = millis();
  const int r = png.decode(nullptr, 0);
  const uint32_t ms = millis() - t0;

  png.close();
  free(s_pngLine);
  s_pngLine = nullptr;
  s_pngLineW = 0;

  if (r != PNG_SUCCESS) {
    BOOTLOG("[img] !! PNG 解码失败: decode()=%d 尺寸=%dx%d 文件=%lu 字节 用时=%lums %s", r, iw, ih,
            (unsigned long)size, (unsigned long)ms, path);
    return false;
  }
  BOOTLOG("[img] PNG %dx%d -> 画布 %ux%u @(%d,%d), 文件 %lu 字节, 解码 %lums", iw, ih, cw, ch, s_offX, s_offY,
          (unsigned long)size, (unsigned long)ms);
  return true;
}

// ---------------------------------------------------------------------------
bool imageGetSize(const char *path, uint16_t *w, uint16_t *h) {
  if (!path || !w || !h) {
    return false;
  }
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  // 统一走"读进内存再解析"的路线: 走库的按文件名版本会踩 SPIFFS 那个坑(见文件头)。
  // 注: 当前工程里没有调用者, 只在排版前估算时可能用到。
  const char *err = nullptr;
  uint32_t size = 0;
  uint8_t *buf = readImageToPsram(path, &size, &err);
  if (!buf) {
    BOOTLOG("[img] !! imageGetSize 读文件失败(%s): %s", err, path);
    return false;
  }

  bool ok = false;
  if (strcasecmp(dot, ".png") == 0) {
    if (png.openRAM(buf, (int)size, pngDrawCallback) == PNG_SUCCESS) {
      *w = (uint16_t)png.getWidth();
      *h = (uint16_t)png.getHeight();
      ok = (*w > 0 && *h > 0);
      png.close();
    }
  } else {
    *w = 0;
    *h = 0;
    ok = (TJpgDec.getJpgSize(w, h, buf, size) == JDR_OK) && *w > 0 && *h > 0;
  }
  free(buf);
  return ok;
}

bool imageDrawToCanvas(Canvas16 *canvas, const char *path) {
  if (!canvas || !path) {
    return false;
  }
  const uint16_t cw = canvas->width();
  const uint16_t ch = canvas->height();
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }

  const bool isJpg = (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
  const bool isPng = (strcasecmp(dot, ".png") == 0);
  if (!isJpg && !isPng) {
    BOOTLOG("[img] !! 不支持的图片格式: %s", path);
    return false;
  }

  canvas->fill(COL_BG);
  s_target = canvas;
  s_offX = 0;
  s_offY = 0;

  const uint32_t t0 = millis();

  // ---- 1) 读进内存(只在这一步持 sd_mutex) ----
  const char *err = nullptr;
  uint32_t size = 0;
  uint8_t *buf = readImageToPsram(path, &size, &err);
  if (!buf) {
    BOOTLOG("[img] !! 读图失败(%s): %s", err, path);
    s_target = nullptr;
    return false;
  }
  const uint32_t tRead = millis() - t0;

  // ---- 2) 解码(内存里做, 不碰 SD 总线, 不再和音频/刷屏抢锁) ----
  bool ok = false;
  if (isJpg) {
    ok = drawJpegMem(buf, size, cw, ch, path);
  } else {
    ok = drawPngMem(buf, size, cw, ch, path);
  }
  free(buf);

  if (!ok) {
    BOOTLOG("[img] !! %s 渲染失败: 读 %lu 字节用 %lums, 总 %lums, PSRAM 余 %u", path, (unsigned long)size,
            (unsigned long)tRead, (unsigned long)(millis() - t0), (unsigned)ESP.getFreePsram());
  }

  s_target = nullptr;
  return ok;
}
