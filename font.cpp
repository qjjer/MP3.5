/*
 * font.cpp - 24x24 点阵字库加载与渲染实现
 *
 * 性能要点:
 *   1. 索引表(约 42KB)整表读进 PSRAM, 查找纯内存二分, 不碰 SD。
 *   2. 字形位图按需读 + 直接映射缓存(默认 256 槽), 一页小说文字里常用字重复
 *      率高, 命中率通常 70% 以上。
 *   3. 渲染直接写 canvas 的像素数组, 不走 drawPixel 虚函数, 快 3~5 倍。
 */
#include "font.h"
#include "config.h"
#include "font_builtin.h"   // 内置 5x7 ASCII 点阵(FONT5X7)
#include "sd_mutex.h"

#include <string.h>

// 诊断直接走 Serial。log_i/log_e 受 Arduino 的 Core Debug Level 控制,
// 默认 0 时什么都不打印, 排障时看不到真实数字。见本文件 fontPerfTick()。
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                    \
  do {                                       \
    Serial.printf(fmt "\n", ##__VA_ARGS__);    \
    Serial.flush();                          \
  } while (0)
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include "esp32-hal-psram.h"  // ps_malloc
#endif

// ---------------------------------------------------------------------------
// 编进固件的"常用字子集"字库(tools/embed_font.py 生成)
//
// ★ 为什么这是最优来源 ★
//   实测 SD 顺序读只有 38 KB/s, 从卡上读 0.94MB 子集要 25 秒、读全量 6.49MB 要 169 秒。
//   而这份数组是 **const**, 落在 Flash(rodata), 由内存映射直接访问 ——
//   取字形就是取一个指针, 零 SD、零 PSRAM 拷贝、零等待。
//
// ★ 它和 SD 路径的关系 ★
//   嵌入子集 = 最高优先级(常用字)
//   没命中   = 回落 SD 主字库按需读(生僻字, 很慢但很少发生, 且**绝不缺字**)
//   如果哪天不想占这 1.1MB Flash: 把 FONT_EMBEDDED_ENABLE 设 0,
//   再把 tools/out/subset24.f24 放到 SD 卡 /fonts/ 下, 行为等价(只是要等 25 秒读进 PSRAM)。
// ---------------------------------------------------------------------------
#if FONT_EMBEDDED_ENABLE
extern "C" {
extern const uint8_t emb_font_subset[];
extern const unsigned int emb_font_subset_len;
extern const uint32_t emb_font_subset_crc32;
}
#endif

// 子集命中次数(文件内静态: fontPerfTick() 是自由函数, 访问不到 DotFont 的私有成员)
static uint32_t s_subHit = 0;

// ---------------------------------------------------------------------------
// ★ 对齐安全的 16 位读 ★
//
// 为什么必须这样读: 嵌入数组是 `const uint8_t emb_font_subset[]`, 而 char 数组的
// 对齐要求只有 1 —— 实测链接后它落在 **0x3C14FFFB(奇数地址)**。
// 于是 `(const uint16_t *)(数组 + 32)` 就是**未对齐指针**, 在 Xtensa 上解引用是
// 未定义行为(轻则变慢, 重则异常)。用 memcpy 读 2 字节: 编译器会自己选安全指令,
// 对数对齐的数据一样是一条 L16UI, 零额外开销。
// (同时我也给生成器加了 aligned(4), 双保险 —— 两边任何一边单独失效都不会出事。)
// ---------------------------------------------------------------------------
static inline uint16_t subIdxAt(const uint16_t *p, uint32_t i) {
  uint16_t v = 0;
  memcpy(&v, (const uint8_t *)p + (size_t)i * 2, 2);
  return v;
}

// CRC-32/ISO-HDLC: 与生成端的 Python binascii.crc32() **完全同一个算法**
// (poly 0xEDB88320 反射, init 0xFFFFFFFF, 结果再异或 0xFFFFFFFF)。
// 用查表版: 1.1MB 只需 ~10ms, 逐位版要 8 倍时间, 启动时白等就不值了。
static uint32_t s_crcTable[256];
static bool s_crcTableReady = false;

static void crc32BuildTable(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    s_crcTable[i] = c;
  }
  s_crcTableReady = true;
}

static uint32_t crc32IsoHdlc(const uint8_t *p, uint32_t n) {
  if (!s_crcTableReady) {
    crc32BuildTable();
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) {
    crc = (crc >> 8) ^ s_crcTable[(crc ^ p[i]) & 0xFFu];
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// 构造/释放
// ---------------------------------------------------------------------------
DotFont::DotFont() {}

void DotFont::end() {
  if (file_) {
    file_.close();
  }
  if (index_) {
    free(index_);
    index_ = nullptr;
  }
  if (cache_) {
    free(cache_);
    cache_ = nullptr;
  }
  if (cacheData_) {
    free(cacheData_);
    cacheData_ = nullptr;
  }
  if (bitmapPsram_) {
    free(bitmapPsram_);
    bitmapPsram_ = nullptr;
  }
  // ★ 子集指针可能是"嵌入 Flash 的内存映射地址", 那种情况下**绝不能 free** ★
  if (subIndex_ && !subInFlash_) {
    free((void *)subIndex_);
  }
  if (subBitmap_ && !subInFlash_) {
    free((void *)subBitmap_);
  }
  subIndex_ = nullptr;
  subBitmap_ = nullptr;
  subInFlash_ = false;
  subGlyphs_ = 0;
  subBytes_ = 0;
  subBpp_ = 0;
  subBytesPerGlyph_ = 0;
  subPath_[0] = 0;
  s_subHit = 0;
  psramGlyphs_ = 0;
  psramBytes_ = 0;
  psramBpp_ = 0;
  psramBytesPerGlyph_ = 0;
  lastBmpBpp_ = 0;
  ready_ = false;
  indexInRam_ = false;
}

static void *allocPreferPsram(size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  void *p = ps_malloc(n);
  if (p) {
    return p;
  }
#endif
  return malloc(n);
}

// 前向声明: 定义在 loadIndex() 之后(见"字库位图进 PSRAM"一节)
static void sdSequentialProbe(File &f, uint32_t offset);

// ---------------------------------------------------------------------------
// 打开字库
// ---------------------------------------------------------------------------
bool DotFont::begin(FS &fs, const char *path) {
  end();
  fs_ = &fs;
  err_[0] = 0;
  snprintf(path_, sizeof(path_), "%s", path);

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    snprintf(err_, sizeof(err_), "等 SD 总线锁超时");
    return false;
  }

  file_ = fs.open(path, FILE_READ);
  if (!file_) {
    file_ = fs.open(path, "r");
  }
  if (!file_) {
    snprintf(err_, sizeof(err_), "cannot open font %s", path);
    return false;
  }

  uint8_t hdr[FONT_HEADER_SIZE];
  if (file_.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
    snprintf(err_, sizeof(err_), "字库文件太短, 读不到文件头");
    return false;
  }
  if (memcmp(hdr, FONT_MAGIC, 4) != 0) {
    snprintf(err_, sizeof(err_), "bad magic: %02X%02X%02X%02X (want F24A)", hdr[0], hdr[1], hdr[2], hdr[3]);
    return false;
  }

  const uint8_t ver = hdr[4];
  bpp_ = hdr[5];
  gw_ = hdr[6];
  gh_ = hdr[7];
  memcpy(&count_, &hdr[0x0C], 2);

  if (ver != 1) {
    snprintf(err_, sizeof(err_), "font ver %u unsupported (need 1)", ver);
    return false;
  }
  if (bpp_ != 1 && bpp_ != 4) {
    snprintf(err_, sizeof(err_), "bpp=%u unsupported (1 or 4)", bpp_);
    return false;
  }
  if (gw_ != FONT_W || gh_ != FONT_H) {
    snprintf(err_, sizeof(err_), "font %ux%u != config %dx%d", gw_, gh_, FONT_W, FONT_H);
    return false;
  }
  if (count_ == 0) {
    snprintf(err_, sizeof(err_), "字库索引为 0 个字形");
    return false;
  }

  const uint16_t bytesPerRow = (bpp_ == 1) ? ((gw_ + 7) / 8) : ((gw_ + 1) / 2);
  bytesPerGlyph_ = (uint16_t)bytesPerRow * gh_;
  bitmapBase_ = FONT_HEADER_SIZE + (uint32_t)count_ * 2;

  const uint32_t need = bitmapBase_ + (uint32_t)count_ * bytesPerGlyph_;
  const uint32_t have = file_.size();
  if (have < need) {
    snprintf(err_, sizeof(err_), "font incomplete: need %u, have %u", (unsigned)need, (unsigned)have);
    return false;
  }

  if (!loadIndex()) {
    return false;
  }

  cache_ = (CacheSlot *)allocPreferPsram(sizeof(CacheSlot) * GLYPH_CACHE_SLOTS);
  cacheData_ = (uint8_t *)allocPreferPsram((size_t)GLYPH_CACHE_SLOTS * bytesPerGlyph_);
  if (!cache_ || !cacheData_) {
    if (cache_) {
      free(cache_);
      cache_ = nullptr;
    }
    if (cacheData_) {
      free(cacheData_);
      cacheData_ = nullptr;
    }
    snprintf(err_, sizeof(err_), "字形缓存内存不足");
    return false;
  }
  for (int i = 0; i < GLYPH_CACHE_SLOTS; i++) {
    cache_[i].cp = 0xFFFFFFFFu;
    cache_[i].valid = false;
  }

  // 注意: loadBitmapsToPsram() 不在这里调用!
  //   它的第一句就是 `if (!ready_ || ...) return false;` —— 放在 ready_=true 之前
  //   只会静默返回(而且会把"只跑一次"的静态标志置位, 反而挡住后面那次真正该跑的)。
  //   真正的调用点在下面 ready_ = true 之后。
  ready_ = true;
  log_i("[font] ready: %u glyphs, %ux%u, %ubpp, %u B/glyph, index=%s", (unsigned)count_, (unsigned)gw_,
        (unsigned)gh_, (unsigned)bpp_, (unsigned)bytesPerGlyph_, indexInRam_ ? "PSRAM" : "SD");

  // ---- 诊断: SD 通路本身有多快(把"随机 seek 慢"和"顺序读慢"分开) ----
  Serial.printf("[font] PSRAM 当前空闲 %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
  sdSequentialProbe(file_, bitmapBase_);

  // ---- 治本之一: 常用字"瘦身子集"(最高优先级) ----
  //   优先级: ① 嵌入 Flash 的子集(零 SD/零 PSRAM/零等待)
  //           ② SD 卡上的子集文件(要读 25 秒, 但之后同样零 SD 读)
  //           ③ 原来的全量搬运/按需读(最慢, 只是保底)
  //   为什么 ① 优先: 这张卡 38 KB/s, 从卡上读 0.94MB 要 25 秒;
  //   而编进固件的那份是 Flash 内存映射, 取字形就是取指针。
  bool subsetOk = false;
#if FONT_EMBEDDED_ENABLE
  subsetOk = attachEmbeddedSubset();
#endif
#if FONT_SUBSET_ENABLE
  if (!subsetOk && FONT_SUBSET_PATH[0]) {
    subsetOk = loadSubsetToPsram(fs, FONT_SUBSET_PATH);
  }
#endif
  if (!subsetOk) {
    BOOTLOG("[font] 既没有嵌入子集, 也没读到 SD 子集 -> 退回原方案(全量搬运/按需读)");
    // ---- 治本之三: 整份字形位图搬进 PSRAM -> 之后绘制零 seek、零 SD 读 ----
    loadBitmapsToPsram();
  }
  return true;
}

bool DotFont::loadIndex() {
  index_ = (uint16_t *)allocPreferPsram((size_t)count_ * 2);
  if (!index_) {
    snprintf(err_, sizeof(err_), "index alloc failed: %u", (unsigned)(count_ * 2));
    return false;
  }

#if FONT_INDEX_CACHE
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    snprintf(err_, sizeof(err_), "读索引表等锁超时");
    free(index_);
    index_ = nullptr;
    return false;
  }
  if (!file_.seek(FONT_HEADER_SIZE)) {
    snprintf(err_, sizeof(err_), "seek 索引表失败");
    free(index_);
    index_ = nullptr;
    return false;
  }
  // 分块读, 避免单次 read 过大时某些 FAT 实现返回短读
  uint8_t *dst = (uint8_t *)index_;
  size_t left = (size_t)count_ * 2;
  while (left > 0) {
    const size_t want = left > 2048 ? 2048 : left;
    const size_t got = file_.read(dst, want);
    if (got == 0) {
      snprintf(err_, sizeof(err_), "index read broke, %u left", (unsigned)left);
      free(index_);
      index_ = nullptr;
      return false;
    }
    dst += got;
    left -= got;
  }
  indexInRam_ = true;
#else
  indexInRam_ = false;
#endif
  return true;
}

// ---------------------------------------------------------------------------
// ★ 字库位图进 PSRAM ★ (完整说明见 font.h)
//
// 实测依据: 一次字形未命中 = f.seek() + read(288B) ≈ 286~320ms, 贵在 seek。
// "整页预取"只能把 60 次随机读降到 55 次(合并率极低), 所以这里直接一次性把
// 整份位图搬进 PSRAM, 绘制时零 seek、零 SD 读、零锁。
//
// 为什么是"降位深"而不是"按码点取前 N 个": 实测(真实字库+真实正文)
//   4MB(14563 个字形, 码点<=U+7DD7) 只覆盖正文的 79.40% -> 一页漏 22 个字
//   -> 22 × 300ms, 比不改还糟。而降位深没有任何覆盖损失:
//   4bpp->2bpp 144B/字形 × 23459 = 3.38MB(4 级灰度, 视觉几乎无差)
//   4bpp->1bpp  72B/字形 × 23459 = 1.69MB(兜底档)
// ---------------------------------------------------------------------------
#ifndef FONT_PSRAM_BITMAP
#define FONT_PSRAM_BITMAP 1
#endif
// 给音频缓冲(655KB, 在 [boot] 6/6 才分配)/EPUB 图片(峰值 ~1.2MB)/正文等留的余量。
// 字库在 5/6 加载, 音频在 6/6 —— 必须"预留", 不能把当前空闲 PSRAM 全吃掉。
#ifndef FONT_PSRAM_RESERVE
#define FONT_PSRAM_RESERVE (2300 * 1024)
#endif
#ifndef FONT_PSRAM_CHUNK
#define FONT_PSRAM_CHUNK (96 * 1024)
#endif

// 顺序读基准: 区分"SD 通路本身慢"和"随机 seek 慢"。
// 先读 4KB 试探 —— 万一 SD 通路病态到只有 1KB/s, 也不会把启动卡死几十秒。
static void sdSequentialProbe(File &f, uint32_t offset) {
  const size_t first = 4096;
  const size_t rest = 124 * 1024;
  uint8_t *buf = (uint8_t *)allocPreferPsram(first + rest);
  if (!buf) {
    BOOTLOG("[fperf] SD顺序读基准: PSRAM 缓冲分配失败, 跳过");
    return;
  }
  uint32_t t0 = micros();
  const bool ok = f.seek(offset) && ((size_t)f.read(buf, first) == first);
  const uint32_t dt1 = micros() - t0;
  const uint32_t kbps = dt1 ? (uint32_t)((uint64_t)first * 1000 / dt1) : 0;
  BOOTLOG("[fperf] SD顺序读基准: 4KB = %lu ms (%lu KB/s)%s", (unsigned long)(dt1 / 1000), (unsigned long)kbps,
          ok ? "" : "  <-- 读失败");
  // ★ 这里原本是 `if (ok && kbps >= 100)` —— 一个 100 KB/s 的门限 ★
  //   它的本意是"太慢就不测了, 别拖时间", 但实际上把**最关键的一个数据点**挡掉了:
  //   实测 4KB 只有 38 KB/s(106ms), 于是 +124KB 那一档直接被跳过, 日志里永远
  //   看不到"读 124KB 要多久" —— 而正是这一档能一眼区分两种完全不同的病因:
  //       读 124KB 也只要 ~100ms  -> 耗时是【每次调用的固定开销】, 加大块就能提速
  //       读 124KB 要 124/0.038   -> 耗时与字节数成正比, 那是通路本身慢
  //   现在无条件测第二档(多花的时间有上限: 即使 4 KB/s 也只是 31 秒, 且下面有判断)。
  t0 = micros();
  const bool ok2 = ((size_t)f.read(buf, rest) == rest);
  const uint32_t dt2 = micros() - t0;
  BOOTLOG("[fperf] SD顺序读基准: +124KB = %lu ms (%lu KB/s)%s", (unsigned long)(dt2 / 1000),
          (unsigned long)(dt2 ? ((uint64_t)rest * 1000 / dt2) : 0), ok2 ? "" : "  <-- 读失败");
  // 小结: 两次读的耗时比值说明病因
  if (dt1 && dt2) {
    BOOTLOG("[fperf] 判读: 4KB=%luus, 124KB=%luus, 字节数差 31 倍而耗时差 %lu 倍 -> %s",
            (unsigned long)dt1, (unsigned long)dt2, (unsigned long)(dt2 / dt1),
            (dt2 / dt1 < 4) ? "固定开销主导(加大块能提速)" : "与字节数成正比(通路本身慢)");
  }
  free(buf);
}

// 位深转换(整块处理, 按字形顺序平铺)
static void convertGlyphBlock(const uint8_t *src, uint8_t *dst, uint32_t n, uint16_t srcBpg, uint8_t srcBpp,
                             uint8_t dstBpp, uint16_t gw, uint16_t gh) {
  const uint16_t dstStride = (dstBpp == 2) ? (uint16_t)((gw + 3) / 4) : (uint16_t)((gw + 7) / 8);
  const uint16_t dstBpg = (uint16_t)(dstStride * gh);
  const uint16_t srcStride4 = (uint16_t)((gw + 1) / 2);

  for (uint32_t k = 0; k < n; k++) {
    const uint8_t *s = src + (size_t)k * srcBpg;
    uint8_t *d = dst + (size_t)k * dstBpg;

    if (srcBpp == 1 && dstBpp == 1) {
      memcpy(d, s, dstBpg);
      continue;
    }
    if (dstBpp == 2) {
      // 4bpp 每字节 2 像素 -> 2bpp 每字节 4 像素: 一个输出字节吃两个源字节。
      // 一行 12 -> 6 字节, 行边界天然对齐, 所以可以按整块平铺, 不用分行。
      for (uint16_t i = 0; i < dstBpg; i++) {
        const uint8_t a = s[2 * i];
        const uint8_t b = s[2 * i + 1];
        d[i] = (uint8_t)((((a >> 6) & 0x03) << 6) | (((a >> 2) & 0x03) << 4) | (((b >> 6) & 0x03) << 2) |
                         ((b >> 2) & 0x03));
      }
      continue;
    }
    // 4bpp -> 1bpp: 阈值 8/15, 每行从高位到低位(与 build_font.py 的 1bpp 布局一致)
    for (uint16_t ry = 0; ry < gh; ry++) {
      const uint8_t *srow = s + (size_t)ry * srcStride4;
      uint8_t *drow = d + (size_t)ry * dstStride;
      for (uint16_t i = 0; i < dstStride; i++) {
        drow[i] = 0;
      }
      for (uint16_t px = 0; px < gw; px++) {
        const uint8_t byte = srow[px >> 1];
        const uint8_t v = (px & 1) ? (uint8_t)(byte & 0x0F) : (uint8_t)(byte >> 4);
        if (v >= 8) {
          drow[px >> 3] |= (uint8_t)(0x80 >> (px & 7));
        }
      }
    }
  }
}

bool DotFont::loadBitmapsToPsram() {
  // 只允许跑一次。
  // 为什么需要这个 guard: begin() 里历史上被加了两次调用(begin 尾部一次, 我接线时
  // 又在 ready_=true 前加了一次), 结果是同一趟 4MB 搬运要跑两遍、失败信息打两遍,
  // 白等一倍时间。用静态标志挡掉, 谁先调到谁生效。
  static bool s_tried = false;
  if (s_tried) {
    return bitmapPsram_ != nullptr;
  }
  s_tried = true;
#if !FONT_PSRAM_BITMAP
  return false;
#else
  if (!ready_ || !file_ || !index_ || !indexInRam_) {
    return false;
  }

  const size_t freePs = (size_t)ESP.getFreePsram();
  if (freePs < (size_t)FONT_PSRAM_RESERVE + 512 * 1024) {
    BOOTLOG("[font] PSRAM 不够, 跳过位图搬运: 空闲 %luKB < 预留 %luKB + 512KB",
            (unsigned long)(freePs / 1024), (unsigned long)(FONT_PSRAM_RESERVE / 1024));
    return false;
  }
  const size_t budget = freePs - (size_t)FONT_PSRAM_RESERVE;

  const uint16_t bpg2 = (uint16_t)(((gw_ + 3) / 4) * gh_);
  const uint16_t bpg1 = (uint16_t)(((gw_ + 7) / 8) * gh_);

  uint8_t dstBpp = 0;
  uint32_t take = count_;
  // 2bpp 的整块平铺要求 4bpp 的"每行字节数"是偶数(一行 2 源字节 -> 1 输出字节,
  // 行边界才对得上)。FONT_W=24 时为 12(偶数); 万一将来字宽变了, 自动走 1bpp。
  const bool can2bpp = (((gw_ + 1) / 2) % 2 == 0);
  if (bpp_ == 4 && can2bpp && (size_t)count_ * bpg2 <= budget) {
    dstBpp = 2;   // 首选: 4 级灰度
  } else if ((size_t)count_ * bpg1 <= budget) {
    dstBpp = 1;   // 退一档: 无抗锯齿, 但 100% 覆盖
  } else {
    // 只能装一部分: 按 glyphId 从 0 起的连续前缀(覆盖率会打折, 下面会告警)
    dstBpp = 1;
    take = (uint32_t)(budget / bpg1);
  }
  if (dstBpp == 0 || take == 0 || take > count_) {
    BOOTLOG("[font] PSRAM 预算只够 %lu 个字形, 放弃搬运", (unsigned long)take);
    return false;
  }

  const uint16_t bpg = (dstBpp == 2) ? bpg2 : bpg1;
  bitmapPsram_ = (uint8_t *)allocPreferPsram((size_t)take * bpg);
  if (!bitmapPsram_) {
    BOOTLOG("[font] PSRAM 分配 %luKB 失败, 保持按需读", (unsigned long)(((size_t)take * bpg) / 1024));
    return false;
  }

  const uint32_t chunkGlyphs = (uint32_t)(FONT_PSRAM_CHUNK / bytesPerGlyph_);
  uint8_t *tmp = (uint8_t *)allocPreferPsram((size_t)chunkGlyphs * bytesPerGlyph_);
  if (!tmp) {
    free(bitmapPsram_);
    bitmapPsram_ = nullptr;
    BOOTLOG("[font] 转换缓冲分配失败, 保持按需读");
    return false;
  }

  const uint32_t t0 = micros();
  bool ok = file_.seek(bitmapBase_);
  uint32_t g = 0;
  while (ok && g < take) {
    uint32_t n = take - g;
    if (n > chunkGlyphs) {
      n = chunkGlyphs;
    }
    const size_t want = (size_t)n * bytesPerGlyph_;
    const uint32_t tc0 = micros();
    if ((size_t)file_.read(tmp, want) != want) {
      ok = false;
      break;
    }
    if (g == 0) {
      // 健康检查: 用第一块推算整趟要多久, 太慢就放弃 ——
      // 位图搬运是"启动时一次付清", 但它不能把启动变成几分钟的假死。
      //
      // ★ 门限的演变(两次都被实测教育) ★
      //   第 1 版: `kbs < 100 || projectedMs > 12000` —— 首块 96KB 实测 19 KB/s, 直接判死。
      //   第 2 版: 我猜"耗时是每次调用的固定开销(读 4KB=106ms 和读 512B=105ms 一样)", 
      //            于是以为"加大块就能提速", 放宽到 90 秒。
      //   ★ 实测推翻了第 2 版 ★ (`[fperf] SD顺序读基准` 新增的那一档):
      //        读 4KB   = 106 ms   (38 KB/s)
      //        读 128KB = 3285 ms  (38 KB/s)     <- 字节数差 31 倍, 耗时也差 30 倍
      //     吞吐与块大小无关 ⇒ **这块卡就是 38 KB/s**, 加大块不能提速。
      //
      //   所以现在的判据是"按实测吞吐直说", 让用户在启动界面上看到真实代价:
      //     <= 45 秒  -> 搬(用户能接受)
      //     <= 300 秒 -> 搬(启动会很久, 但字库进 PSRAM 之后翻页是质的差别, 值得一次等待)
      //     >  300 秒 或 吞吐 < 4 KB/s -> 放弃(太病态, 别把启动变成假死)
      const uint32_t us = micros() - tc0;
      const uint32_t kbs = us ? (uint32_t)((uint64_t)want * 1000 / us) : 0;
      const uint32_t totalSrc = (uint32_t)((uint64_t)take * bytesPerGlyph_);
      const uint32_t projectedMs = kbs ? (uint32_t)((uint64_t)totalSrc / kbs) : 999999;
      if (kbs < 4 || projectedMs > 300000) {
        BOOTLOG("[font] !! SD 吞吐只有 %lu KB/s(推算整趟 %lu ms), 放弃搬运 —— 保持按需读",
                (unsigned long)kbs, (unsigned long)projectedMs);
        BOOTLOG("[font] !! 字库 6.4MB 装不进 PSRAM 的话, 每次翻页都要按需读字形(很慢)。");
        BOOTLOG("[font] !! 换一张读速正常的 SD 卡(应 1~3 MB/s)后, 字形会常驻内存, 翻页立刻变快。");
        ok = false;
        break;
      }
      BOOTLOG("[font] 位图搬运中: SD 吞吐 %lu KB/s, 预计整趟 %lu ms (%.1f 秒), %lu 个字形",
              (unsigned long)kbs, (unsigned long)projectedMs, projectedMs / 1000.0, (unsigned long)take);
    }
    convertGlyphBlock(tmp, bitmapPsram_ + (size_t)g * bpg, n, bytesPerGlyph_, bpp_, dstBpp, gw_, gh_);
    g += n;
    // 每块让一次 CPU: 否则 CPU1 的 idle 任务在这几秒里一次都跑不到,
    // 可能触发 Task WDT(IDLE1)。代价只有 1ms × 块数。
    delay(1);

    // 进度打印: 慢卡上整趟可能要几分钟, 没有进度用户会以为死机。
    // 每 10% 打一行(而且是 BOOTLOG, 走 Serial 且 flush, 一定看得见)。
    {
      static uint32_t nextPct = 0;
      const uint32_t pct = (uint32_t)((uint64_t)g * 100 / take);
      if (pct >= nextPct) {
        nextPct = pct + 10;
        BOOTLOG("[font] 位图搬运 %lu%% (%lu/%lu 字形)", (unsigned long)pct, (unsigned long)g,
                (unsigned long)take);
      }
    }
  }
  free(tmp);
  const uint32_t dt = micros() - t0;

  if (g == 0) {
    free(bitmapPsram_);
    bitmapPsram_ = nullptr;
    BOOTLOG("[font] !! 位图搬运失败(SD 读错误), 保持按需读");
    return false;
  }

  psramGlyphs_ = g;
  psramBytes_ = (size_t)g * bpg;
  psramBytesPerGlyph_ = bpg;
  psramBpp_ = dstBpp;

  BOOTLOG("[font] 位图进 PSRAM: %lu/%u 个字形, %ubpp, %luKB, 覆盖码点 <= U+%04X, 耗时 %lu ms, 剩余 PSRAM %luKB",
          (unsigned long)g, (unsigned)count_, (unsigned)dstBpp, (unsigned long)(psramBytes_ / 1024),
          (unsigned)index_[g - 1], (unsigned long)(dt / 1000), (unsigned long)(ESP.getFreePsram() / 1024));
  if (g < count_) {
    BOOTLOG("[font] !! 只装了 %lu/%u 个字形 -> 码点 > U+%04X 的字仍走 SD(每次约 300ms)",
            (unsigned long)g, (unsigned)count_, (unsigned)index_[g - 1]);
  }
  return true;
#endif
}

// ---------------------------------------------------------------------------
// 常用字"瘦身子集": 整份进 PSRAM, 生僻字回落主字库
//
// ★ 为什么必须瘦身(实测) ★
//   SD 顺序读 38 KB/s, 与块大小无关(4KB=106ms, 128KB=3285ms)。
//   全量 4bpp 6.44MB -> PSRAM 装不下; 全量 2bpp 3.38MB -> 装得下但要 89 秒。
//   子集 2bpp×(GB2312∪语料) 0.94MB -> **约 25 秒**, 之后常用字零 SD 读。
//
// ★ 两段式 ★
//   子集命中  -> subBitmap_ 里的指针(零锁/零 SD/零 memcpy)
//   子集未命中 -> 回落本月已打开的主字库文件(file_)按需读, 绝不缺字
//
// 文件格式与主字库完全一致(.f24): 头部 32 字节 + 升序 uint16 索引 + 紧凑位图。
// 生成脚本: tools/make_subset_font.py
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 把编进固件的那份子集字库接上(最高优先级来源)
//
// 零 SD、零 PSRAM 拷贝: subIndex_/subBitmap_ 直接指向 Flash 内存映射区。
// 上机自检: 头部自洽 + 索引严格升序 + CRC32 与生成时一致 —— 三者任一不对就拒绝,
// 宁可退回"慢但一定对"的 SD 路径, 也不要用一份坏数据画出满屏错字。
// ---------------------------------------------------------------------------
bool DotFont::attachEmbeddedSubset() {
#if !FONT_EMBEDDED_ENABLE
  return false;
#else
  const uint32_t len = (uint32_t)emb_font_subset_len;
  if (len < (uint32_t)FONT_HEADER_SIZE + 8) {
    BOOTLOG("[font] 嵌入子集: 长度 %lu 太小, 忽略", (unsigned long)len);
    return false;
  }
  const uint8_t *d = emb_font_subset;
  if (memcmp(d, FONT_MAGIC, 4) != 0) {
    BOOTLOG("[font] 嵌入子集: 魔数不是 F24A, 拒绝");
    return false;
  }
  const uint8_t abpp = d[5], agw = d[6], agh = d[7];
  uint16_t acount = 0;
  memcpy(&acount, &d[0x0C], 2);
  if (abpp != 1 && abpp != 2 && abpp != 4) {
    BOOTLOG("[font] 嵌入子集: bpp=%u 不支持", (unsigned)abpp);
    return false;
  }
  if (agw != gw_ || agh != gh_) {
    BOOTLOG("[font] 嵌入子集: 字形 %ux%u != 主字库 %ux%u", (unsigned)agw, (unsigned)agh, (unsigned)gw_,
            (unsigned)gh_);
    return false;
  }
  if (acount == 0) {
    BOOTLOG("[font] 嵌入子集: count=0, 拒绝");
    return false;
  }
  const uint16_t abpg = (uint16_t)((((uint32_t)agw * abpp + 7) / 8) * agh);
  const uint32_t need = FONT_HEADER_SIZE + (uint32_t)acount * 2 + (uint32_t)acount * abpg;
  if (len < need) {
    BOOTLOG("[font] 嵌入子集: 长度 %lu < 推算 %lu, 拒绝", (unsigned long)len, (unsigned long)need);
    return false;
  }

  // ---- CRC32 自检: 确认烧进 Flash 的数据和生成时逐字节一致 ----
  const uint32_t crc = crc32IsoHdlc(d, need);
  if (crc != (uint32_t)emb_font_subset_crc32) {
    BOOTLOG("[font] !! 嵌入子集 CRC32 不符: 算得 0x%08lX, 声明 0x%08lX -> 拒绝使用(回落 SD 路径)",
            (unsigned long)crc, (unsigned long)emb_font_subset_crc32);
    return false;
  }

  // ---- 索引必须严格升序(否则二分查找会静默查错字) ----
  const uint16_t *idx = (const uint16_t *)(const void *)(d + FONT_HEADER_SIZE);
  for (uint32_t i = 1; i < acount; i++) {
    if (subIdxAt(idx, i - 1) >= subIdxAt(idx, i)) {
      BOOTLOG("[font] !! 嵌入子集索引在 %lu 处不是升序(U+%04X -> U+%04X), 拒绝", (unsigned long)i,
              (unsigned)subIdxAt(idx, i - 1), (unsigned)subIdxAt(idx, i));
      return false;
    }
  }

  subIndex_ = idx;
  subBitmap_ = d + FONT_HEADER_SIZE + (uint32_t)acount * 2;
  subGlyphs_ = acount;
  subBytes_ = (uint32_t)acount * abpg;
  subBytesPerGlyph_ = abpg;
  subBpp_ = abpp;
  subInFlash_ = true;
  snprintf(subPath_, sizeof(subPath_), "(Flash 嵌入)");

  BOOTLOG("[font] ★ 嵌入 Flash 子集就绪: %lu 个字, %ubpp, %luKB, CRC32 0x%08lX 校验通过, "
          "码点 U+%04X..U+%04X",
          (unsigned long)subGlyphs_, (unsigned)subBpp_, (unsigned long)(subBytes_ / 1024),
          (unsigned long)crc, (unsigned)subIdxAt(idx, 0), (unsigned)subIdxAt(idx, acount - 1));
  BOOTLOG("[font] ★ 这些字的字形访问零 SD、零 PSRAM、零锁; 生僻字回落 SD 主字库(约 105ms/字)");
  return true;
#endif
}

bool DotFont::subsetFind(uint32_t cp, uint32_t *outOff) const {
  if (!subIndex_ || subGlyphs_ == 0 || cp > 0xFFFF) {
    return false;
  }
  const uint16_t target = (uint16_t)cp;
  uint32_t lo = 0;
  uint32_t hi = subGlyphs_ - 1;
  while (lo <= hi) {
    const uint32_t mid = lo + ((hi - lo) >> 1);
    const uint16_t v = subIdxAt(subIndex_, mid);
    if (v == target) {
      *outOff = mid * (uint32_t)subBytesPerGlyph_;
      return true;
    }
    if (v < target) {
      lo = mid + 1;
    } else {
      if (mid == 0) {
        break;
      }
      hi = mid - 1;
    }
  }
  return false;
}

bool DotFont::loadSubsetToPsram(FS &fs, const char *path) {
  snprintf(subPath_, sizeof(subPath_), "%s", path);

  File f;
  uint8_t hdr[FONT_HEADER_SIZE];
  uint16_t scount = 0;
  uint8_t sbpp = 0, sgw = 0, sgh = 0;
  uint16_t sbpg = 0;

  // ---- 1) 打开 + 校验头部(这一段持锁, 很短) ----
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      BOOTLOG("[font] 子集: 等 SD 锁超时");
      return false;
    }
    f = fs.open(path, FILE_READ);
    if (!f) {
      f = fs.open(path, "r");
    }
    if (!f) {
      BOOTLOG("[font] 子集: 打不开 %s -> 回落全量字库方案", path);
      return false;
    }
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr) || memcmp(hdr, FONT_MAGIC, 4) != 0) {
      BOOTLOG("[font] 子集: %s 头部不对(魔数不是 F24A), 拒绝", path);
      f.close();
      return false;
    }
    if (hdr[4] != 1) {
      BOOTLOG("[font] 子集: 版本 %u 不支持", (unsigned)hdr[4]);
      f.close();
      return false;
    }
    sbpp = hdr[5];
    sgw = hdr[6];
    sgh = hdr[7];
    memcpy(&scount, &hdr[0x0C], 2);   // 与主字库同一个字段位置
    if (sbpp != 1 && sbpp != 2 && sbpp != 4) {
      BOOTLOG("[font] 子集: bpp=%u 不支持(只认 1/2/4)", (unsigned)sbpp);
      f.close();
      return false;
    }
    if (sgw != gw_ || sgh != gh_) {
      BOOTLOG("[font] 子集: 字形尺寸 %ux%u != 主字库 %ux%u", (unsigned)sgw, (unsigned)sgh, (unsigned)gw_,
              (unsigned)gh_);
      f.close();
      return false;
    }
    if (scount == 0) {
      BOOTLOG("[font] 子集: count=0, 拒绝");
      f.close();
      return false;
    }
    const uint16_t stride = (uint16_t)(((uint32_t)sgw * sbpp + 7) / 8);
    sbpg = (uint16_t)(stride * sgh);
    const uint32_t need = FONT_HEADER_SIZE + (uint32_t)scount * 2 + (uint32_t)scount * sbpg;
    if ((uint32_t)f.size() < need) {
      BOOTLOG("[font] 子集: 文件不完整(需要 %lu, 实际 %lu)", (unsigned long)need, (unsigned long)f.size());
      f.close();
      return false;
    }
  }

  // ---- 2) 选目标位深: 优先用文件自带的(不转换最快), 预算不够就降档 ----
  const uint16_t stride2 = (uint16_t)((sgw + 3) / 4);
  const uint16_t stride1 = (uint16_t)((sgw + 7) / 8);
  const size_t freePs = (size_t)ESP.getFreePsram();
  const size_t budget = (freePs > (size_t)FONT_PSRAM_RESERVE + 256 * 1024) ? (freePs - (size_t)FONT_PSRAM_RESERVE) : 0;

  uint8_t dstBpp = sbpp;
  uint16_t dstBpg = sbpg;
  if (budget == 0 || (size_t)scount * dstBpg > budget) {
    // 装不下就降档: 4 -> 2 -> 1(降档会走一次 convertGlyphBlock)
    uint8_t alt = (sbpp == 4) ? 2 : 1;
    uint16_t altBpg = (uint16_t)(((alt == 2) ? stride2 : stride1) * sgh);
    if (alt == 2 && (size_t)scount * altBpg > budget) {
      alt = 1;
      altBpg = (uint16_t)(stride1 * sgh);
    }
    if ((size_t)scount * altBpg > budget) {
      BOOTLOG("[font] 子集: PSRAM 不够(需要 %luKB, 可用 %luKB) -> 回落全量方案",
              (unsigned long)((size_t)scount * altBpg / 1024), (unsigned long)(budget / 1024));
      f.close();
      return false;
    }
    BOOTLOG("[font] 子集: 预算不够, 位深 %u -> %u(会做一次转换)", (unsigned)sbpp, (unsigned)alt);
    dstBpp = alt;
    dstBpg = altBpg;
  }

  // ---- 3) 分配(索引 + 位图) ----
  uint16_t *sidx = (uint16_t *)allocPreferPsram((size_t)scount * 2);
  uint8_t *sbmp = (uint8_t *)allocPreferPsram((size_t)scount * dstBpg);
  if (!sidx || !sbmp) {
    free(sidx);
    free(sbmp);
    BOOTLOG("[font] 子集: PSRAM 分配失败(index %uB + bitmap %luKB)", (unsigned)(scount * 2),
            (unsigned long)((size_t)scount * dstBpg / 1024));
    f.close();
    return false;
  }
  subIndex_ = sidx;
  subBitmap_ = sbmp;

  const uint32_t t00 = micros();
  bool ok = true;

  // ---- 4) 读索引(分块) ----
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok() || !f.seek(FONT_HEADER_SIZE)) {
      ok = false;
    } else {
      uint8_t *dst = (uint8_t *)sidx;
      size_t left = (size_t)scount * 2;
      while (left > 0) {
        const size_t want = left > 2048 ? 2048 : left;
        const size_t got = f.read(dst, want);
        if (got == 0) {
          ok = false;
          break;
        }
        dst += got;
        left -= got;
      }
    }
  }

  // 索引必须严格升序 —— 否则二分查找会静默失效(宁可不用子集, 也不能查错字)
  if (ok) {
    for (uint32_t i = 1; i < scount; i++) {
      if (subIndex_[i - 1] >= subIndex_[i]) {
        BOOTLOG("[font] 子集: 索引在 i=%lu 处不是升序(U+%04X -> U+%04X), 拒绝使用",
                (unsigned long)i, (unsigned)subIndex_[i - 1], (unsigned)subIndex_[i]);
        ok = false;
        break;
      }
    }
  }

  // ---- 5) 读位图(分块, 每块自己拿一次锁; 块间让 CPU 一次, 避免 Task WDT) ----
  uint8_t *tmp = nullptr;
  uint32_t g2 = 0;
  if (ok) {
    const uint32_t chunkGlyphs = (uint32_t)(FONT_PSRAM_CHUNK / sbpg);
    tmp = (uint8_t *)allocPreferPsram((size_t)(chunkGlyphs ? chunkGlyphs : 1) * sbpg);
    if (!tmp) {
      BOOTLOG("[font] 子集: 转换缓冲分配失败");
      ok = false;
    }
    if (ok && !f.seek(FONT_HEADER_SIZE + (uint32_t)scount * 2)) {
      BOOTLOG("[font] 子集: seek 位图区失败");
      ok = false;
    }
    bool firstChunk = true;
    uint32_t nextPct = 0;
    while (ok && g2 < scount) {
      uint32_t n = scount - g2;
      if (n > chunkGlyphs) {
        n = chunkGlyphs;
      }
      const size_t want = (size_t)n * sbpg;
      const uint32_t tc0 = micros();
      {
        SdLockGuard g(SD_LOCK_TIMEOUT_MS);
        if (!g.ok() || (size_t)f.read(tmp, want) != want) {
          BOOTLOG("[font] 子集: 位图读取中断于 %lu/%u", (unsigned long)g2, (unsigned)scount);
          ok = false;
          break;
        }
      }
      const uint32_t us = micros() - tc0;
      if (firstChunk) {
        firstChunk = false;
        const uint32_t kbs = us ? (uint32_t)((uint64_t)want * 1000 / us) : 0;
        const uint32_t totalSrc = (uint32_t)((uint64_t)scount * sbpg);
        const uint32_t projectedMs = kbs ? (uint32_t)((uint64_t)totalSrc / kbs) : 999999;
        BOOTLOG("[font] 子集搬运中: SD 吞吐 %lu KB/s, 共 %luKB, 预计 %lu ms (%.1f 秒), %u 个常用字",
                (unsigned long)kbs, (unsigned long)(totalSrc / 1024), (unsigned long)projectedMs,
                projectedMs / 1000.0, (unsigned)scount);
      }
      convertGlyphBlock(tmp, sbmp + (size_t)g2 * dstBpg, n, sbpg, sbpp, dstBpp, sgw, sgh);
      g2 += n;
      delay(1);   // 让 idle 任务跑得到(96KB/块 -> 每块约 2.5 秒, 不 delay 会喂不到看门狗)
      {
        const uint32_t pct = (uint32_t)((uint64_t)g2 * 100 / scount);
        if (pct >= nextPct) {
          nextPct = pct + 10;
          BOOTLOG("[font] 子集搬运 %lu%% (%lu/%u 字形)", (unsigned long)pct, (unsigned long)g2, (unsigned)scount);
        }
      }
    }
  }

  if (tmp) {
    free(tmp);
  }
  f.close();

  if (!ok || g2 == 0) {
    free(sidx);
    free(sbmp);
    subIndex_ = nullptr;
    subBitmap_ = nullptr;
    subGlyphs_ = 0;
    subBytes_ = 0;
    subBpp_ = 0;
    BOOTLOG("[font] 子集加载失败 -> 回落全量字库方案");
    return false;
  }

  subGlyphs_ = g2;
  subBytes_ = (size_t)g2 * dstBpg;
  subBytesPerGlyph_ = dstBpg;
  subBpp_ = dstBpp;

  BOOTLOG("[font] ★ 子集字库就绪: %s, %lu 个常用字, %ubpp, %luKB PSRAM, 耗时 %lu ms, 剩余 PSRAM %luKB",
          path, (unsigned long)subGlyphs_, (unsigned)dstBpp, (unsigned long)(subBytes_ / 1024),
          (unsigned long)((micros() - t00) / 1000), (unsigned long)(ESP.getFreePsram() / 1024));
  BOOTLOG("[font] ★ 这些字的字形访问现在**零 SD 读**; 不在子集里的生僻字回落主字库按需读(约 105ms/字)");
  return true;
}

// ---------------------------------------------------------------------------
// 在目录里找第一个 .f24 字库文件
// ---------------------------------------------------------------------------
bool DotFont::findFontFile(FS &fs, const char *dir, char *out, size_t outLen) {
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    return false;
  }
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return false;
  }
  bool found = false;
  File f;
  while ((f = d.openNextFile())) {
    const bool isDir = f.isDirectory();
    char nm[80];
    strncpy(nm, f.name(), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    f.close();
    if (isDir) {
      continue;
    }
    const size_t l = strlen(nm);
    if (l > 4 && strcasecmp(nm + l - 4, ".f24") == 0) {
      snprintf(out, outLen, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, nm);
      found = true;
      break;
    }
  }
  d.close();
  return found;
}

// ---------------------------------------------------------------------------
// 查找: Unicode -> 字形编号(二分)
// ---------------------------------------------------------------------------
bool DotFont::lookup(uint32_t cp, GlyphMeta *out) const {
  out->found = false;
  out->glyphId = 0;
  out->fileOffset = 0;
  if (!ready_ || cp > 0xFFFF) {
    return false;  // 字库只收录 BMP
  }

  const uint16_t target = (uint16_t)cp;

  if (indexInRam_) {
    uint16_t lo = 0;
    uint16_t hi = (uint16_t)(count_ - 1);
    while (lo <= hi) {
      const uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
      const uint16_t v = index_[mid];
      if (v == target) {
        out->found = true;
        out->glyphId = mid;
        out->fileOffset = bitmapBase_ + (uint32_t)mid * bytesPerGlyph_;
        return true;
      }
      if (v < target) {
        if (mid == 0xFFFF) {
          break;
        }
        lo = (uint16_t)(mid + 1);
      } else {
        if (mid == 0) {
          break;
        }
        hi = (uint16_t)(mid - 1);
      }
    }
    return false;
  }

  // 退化路径: 索引表没进内存时直接对文件二分(慢, 每次一个 SD 随机读)
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok() || !file_) {
    return false;
  }
  uint16_t lo = 0;
  uint16_t hi = (uint16_t)(count_ - 1);
  while (lo <= hi) {
    const uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
    uint16_t v = 0;
    if (!file_.seek(FONT_HEADER_SIZE + (uint32_t)mid * 2) || file_.read((uint8_t *)&v, 2) != 2) {
      return false;
    }
    if (v == target) {
      out->found = true;
      out->glyphId = mid;
      out->fileOffset = bitmapBase_ + (uint32_t)mid * bytesPerGlyph_;
      return true;
    }
    if (v < target) {
      lo = (uint16_t)(mid + 1);
    } else {
      hi = (uint16_t)(mid - 1);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 【诊断】字形加载与像素绘制的耗时统计
//
// 背景: ui-perf 的 [perf] 显示"一次整屏渲染要 1.3~5.9 秒", 其中 flush 只占 7.5ms,
// 而字形只有 43 次查找(5 miss / 38 hit)。5 次 SD 读解释不了 1.5 秒, 所以必须
// 把时间拆开: 到底是「SD 读字形」贵, 还是「像素绘制(blend565)」贵。
// 每 2 秒打印一行, 用 BOOTLOG 保证可见。
// ---------------------------------------------------------------------------
static uint32_t s_dbgSdUs = 0, s_dbgSdN = 0, s_dbgBlitUs = 0, s_dbgBlitN = 0, s_dbgLookupUs = 0;
// ★ 把"等 sd_mutex"和"真正的 seek+read"分开 ★
// 之前 t0 取在拿锁之前, 报告里的"286ms/次"混着锁等待, 分不清是 SD 慢还是被别的任务挡住。
static uint32_t s_dbgSdWaitUs = 0, s_dbgSdIoUs = 0;
static uint32_t s_dbgLastMs = 0;

static void fontPerfTick(void) {
  const uint32_t now = millis();
  if (s_dbgLastMs == 0) {
    s_dbgLastMs = now ? now : 1;
    return;
  }
  if (now - s_dbgLastMs < 2000) {
    return;
  }
  s_dbgLastMs = now;
  static uint32_t subHitPrev = 0;
  const uint32_t subDelta = s_subHit - subHitPrev;
  subHitPrev = s_subHit;
  // 判读: 装了子集之后, "SD读字形"应该长期是 0~几次(只有生僻字), 而"子集命中"是大头。
  BOOTLOG("[fperf] SD读字形 %lu 次 %lu ms (其中等锁 %lu ms + IO %lu ms) | 像素绘制 %lu 次 %lu ms | 查找索引 %lu ms | 子集命中 %lu",
          (unsigned long)s_dbgSdN, (unsigned long)(s_dbgSdUs / 1000), (unsigned long)(s_dbgSdWaitUs / 1000),
          (unsigned long)(s_dbgSdIoUs / 1000), (unsigned long)s_dbgBlitN, (unsigned long)(s_dbgBlitUs / 1000),
          (unsigned long)s_dbgLookupUs / 1000, (unsigned long)subDelta);
  s_dbgSdUs = s_dbgSdN = s_dbgBlitUs = s_dbgBlitN = s_dbgLookupUs = 0;
  s_dbgSdWaitUs = s_dbgSdIoUs = 0;
}

uint32_t DotFont::statSubsetHit() const {
  return s_subHit;
}

// ---------------------------------------------------------------------------
// 取字形位图(带缓存)
// ---------------------------------------------------------------------------
const uint8_t *DotFont::loadGlyph(File &f, uint32_t cp, uint16_t glyphId) {
  // ---- 0) 常用字子集(PSRAM): 零锁、零 SD、零 memcpy ----
  //   实测: 子集(0.94MB, GB2312∪语料)覆盖正文 100% 的字符出现次数,
  //   所以正常翻页在这里就返回了, 下面那些 SD 路径一次都走不到。
  if (subIndex_) {
    uint32_t subOff = 0;
    if (subsetFind(cp, &subOff)) {
      hit_++;
      s_subHit++;
      lastBmpBpp_ = subBpp_;
      fontPerfTick();
      return subBitmap_ + subOff;
    }
  }

  // ---- 0.5) 全量字库的 PSRAM 档(只有没装子集时才可能命中) ----
  if (bitmapPsram_ && glyphId < psramGlyphs_) {
    hit_++;                     // 统计口径: hit = 没碰 SD
    lastBmpBpp_ = psramBpp_;    // 2 = 2bpp 压缩档, 1 = 1bpp 档
    fontPerfTick();
    return bitmapPsram_ + (size_t)glyphId * psramBytesPerGlyph_;
  }

  if (!cache_ || !cacheData_) {
    return nullptr;
  }

  const uint32_t slot = (cp * 2654435761u) % GLYPH_CACHE_SLOTS;
  CacheSlot &c = cache_[slot];
  uint8_t *data = cacheData_ + (size_t)slot * bytesPerGlyph_;

  if (c.valid && c.cp == cp) {
    hit_++;
    lastBmpBpp_ = bpp_;
    fontPerfTick();
    return data;  // 直接映射命中, 零拷贝
  }
  miss_++;

  {
    // SD 访问必须持锁。★ 等锁时间和真正的 IO 分开统计 ★
    // (之前 t0 取在拿锁之前, "286ms/次"里到底多少是等锁、多少是 seek 分不开)
    const uint32_t t0 = micros();
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    const uint32_t tGot = micros();
    if (!g.ok()) {
      return nullptr;
    }
    s_dbgSdWaitUs += (tGot - t0);
    if (!f.seek(bitmapBase_ + (uint32_t)glyphId * bytesPerGlyph_)) {
      c.valid = false;
      return nullptr;
    }
    if (f.read(data, bytesPerGlyph_) != bytesPerGlyph_) {
      c.valid = false;
      return nullptr;
    }
    s_dbgSdIoUs += (micros() - tGot);
    s_dbgSdUs += (micros() - t0);
    s_dbgSdN++;
  }

  c.cp = cp;
  c.glyphId = glyphId;
  c.rows = gh_;
  c.valid = true;
  lastBmpBpp_ = bpp_;
  fontPerfTick();
  return data;
}

// ---------------------------------------------------------------------------
// LVGL 桥接(只读): 把 loadGlyph() 的结果原样交出去
//
// 只做三件事: lookup -> loadGlyph -> 回填 bpp/宽高。
// 没有任何新的 SD 读路径 —— loadGlyph 内部该走 PSRAM 走 PSRAM, 该走缓存走缓存,
// 该按需读卡也已经有 sd_mutex 保护。所以这个接口对现有行为是零影响。
// ---------------------------------------------------------------------------
const uint8_t *DotFont::glyphBitmap(uint32_t cp, uint8_t *outBpp, uint8_t *outGw, uint8_t *outGh) {
  if (!ready_) {
    return nullptr;
  }
  GlyphMeta m;
  if (!lookup(cp, &m)) {
    return nullptr;  // 字库里没有这个字(ASCII 由调用方自己用内置点阵画)
  }
  const uint8_t *bmp = loadGlyph(file_, cp, m.glyphId);
  if (!bmp) {
    return nullptr;
  }
  if (outBpp) {
    *outBpp = lastBmpBpp_;
  }
  if (outGw) {
    *outGw = gw_;
  }
  if (outGh) {
    *outGh = gh_;
  }
  return bmp;
}

// ---------------------------------------------------------------------------
// 整页字形预取: 把"随机小读"变成"一趟顺序读"
//
// ★ 实测依据 ★
//   一次字形未命中 = f.seek() + read(288B), 实测 **286~320 毫秒**。
//   贵不在 288 字节, 而在 seek: 字库 6.8MB、字形在文件里随机分布,
//   f.seek() 要沿 FAT 链一簇簇跳。一页正文有 40~88 个不同汉字,
//   逐个按需读就是十几秒 —— 这就是"翻页卡死"的真正原因。
//
// ★ 做法 ★
//   1) 收集该页所有码点(去重)
//   2) 解析成 glyphId(每字形定长 bytesPerGlyph_, 所以 id 就是文件内顺序)
//   3) 按 id 升序排序
//   4) 把"间隔很近"的字形合并成一次连续读, 一趟读下来
//   5) 直接填进列缓存槽(和 loadGlyph 用同一个散列公式)
//
// 这样 40~88 次随机 seek 变成 1~几次顺序读。
// ---------------------------------------------------------------------------
#ifndef GLYPH_PREFETCH_MAX
#define GLYPH_PREFETCH_MAX 512        // 一页最多预取多少个不同字形(上限, 防越界)
#endif
#ifndef GLYPH_PREFETCH_MAX_GAP
#define GLYPH_PREFETCH_MAX_GAP 32     // 相邻字形 id 差距 <= 此值就合并进同一次读
#endif

int DotFont::prefetchUtf8(const char *const *utf8, int count) {
  if (!ready_ || !index_ || !indexInRam_ || !cache_ || !cacheData_ || !count) {
    return 0;
  }

  // ★ 字形位图已经整份在 PSRAM 里了 -> 预取变成纯粹的浪费 ★
  //   (实测: 预取用的是"按 id 排序 + 合并间隔<=32", 一页 60 个字形只能合并到 55 次读,
  //    每次读仍要 seek; 而现在绘制根本不需要读 SD, 所以直接跳过。)
  if (bitmapPsram_ && psramGlyphs_ >= count_) {
    BOOTLOG("[fprefetch] 跳过: 字形位图已全在 PSRAM(%lu 个), 无需预取", (unsigned long)psramGlyphs_);
    return 0;
  }

  uint32_t *cps = (uint32_t *)allocPreferPsram(sizeof(uint32_t) * GLYPH_PREFETCH_MAX);
  uint16_t *ids = (uint16_t *)allocPreferPsram(sizeof(uint16_t) * GLYPH_PREFETCH_MAX);
  if (!cps || !ids) {
    free(cps);
    free(ids);
    return 0;
  }

  // ---- 1) 收集码点(顺便去重) ----
  int n = 0;
  for (int s = 0; s < count && n < GLYPH_PREFETCH_MAX; s++) {
    const char *p = utf8[s];
    while (p && *p && n < GLYPH_PREFETCH_MAX) {
      const uint8_t c0 = (uint8_t)*p;
      uint32_t cp;
      int len;
      if (c0 < 0x80) {
        cp = c0;
        len = 1;
      } else if ((c0 & 0xE0) == 0xC0) {
        cp = c0 & 0x1F;
        len = 2;
      } else if ((c0 & 0xF0) == 0xE0) {
        cp = c0 & 0x0F;
        len = 3;
      } else if ((c0 & 0xF8) == 0xF0) {
        cp = c0 & 0x07;
        len = 4;
      } else {
        p++;   // 非法字节, 跳过
        continue;
      }
      for (int k = 1; k < len; k++) {
        if (((uint8_t)p[k] & 0xC0) != 0x80) {
          len = 0;   // 截断
          break;
        }
        cp = (cp << 6) | ((uint8_t)p[k] & 0x3F);
      }
      if (len == 0) {
        p++;
        continue;
      }
      // ASCII(0x20~0x7E)走内置点阵, 不占字库, 跳过
      if (cp >= 0x20 && cp < 0x7F) {
        p += len;
        continue;
      }
      // ★ 子集(PSRAM)已经覆盖的字形**不要预取** ★
      //   否则就白干了: 子集存在的意义就是"画的时候不碰 SD",
      //   而预取会把这些常用字又从头读一遍(一页 100 个字 = 100 次 SD 读)。
      //   预取从此只负责"生僻字"那一小撮, 正好把它该干的活干好。
      if (subIndex_) {
        uint32_t subOff = 0;
        if (subsetFind(cp, &subOff)) {
          p += len;
          continue;
        }
      }
      // 去重(线性扫 —— 最多几百个, 比排序再查快且不占额外内存)
      bool dup = false;
      for (int i = 0; i < n; i++) {
        if (cps[i] == cp) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        GlyphMeta m;
        if (lookup(cp, &m)) {   // 字库里没有的字不用预取(画的时候会画空心框)
          cps[n] = cp;
          ids[n] = m.glyphId;
          n++;
        }
      }
      p += len;
    }
  }

  if (n == 0) {
    free(cps);
    free(ids);
    return 0;
  }

  // ---- 2) 按 glyphId 升序排序(插入排序; n 很小, 且基本接近有序) ----
  for (int i = 1; i < n; i++) {
    const uint16_t ki = ids[i];
    const uint32_t ci = cps[i];
    int j = i - 1;
    while (j >= 0 && ids[j] > ki) {
      ids[j + 1] = ids[j];
      cps[j + 1] = cps[j];
      j--;
    }
    ids[j + 1] = ki;
    cps[j + 1] = ci;
  }

  // ---- 3) 合并相邻字形, 一趟读下来 ----
  const uint32_t t0 = micros();
  uint32_t reads = 0;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      free(cps);
      free(ids);
      return 0;
    }

    // 先把这些字形对应的缓存槽"作废", 免得读到一半时散列冲突留下旧的 valid 标记
    for (int i = 0; i < n; i++) {
      const uint32_t slot = (cps[i] * 2654435761u) % GLYPH_CACHE_SLOTS;
      cache_[slot].valid = false;
    }

    int i = 0;
    while (i < n) {
      // 从 i 开始, 把 id 连续的段合并成一次读
      int j = i;
      while (j + 1 < n && (uint32_t)(ids[j + 1] - ids[j]) <= GLYPH_PREFETCH_MAX_GAP) {
        j++;
      }
      const uint16_t idFirst = ids[i];
      const uint16_t idLast = ids[j];
      const uint32_t spanBytes = (uint32_t)(idLast - idFirst + 1) * bytesPerGlyph_;

      // 整段一次读进临时缓冲(段本身不大: 相邻字形, 通常几十个)
      uint8_t *tmp = (uint8_t *)allocPreferPsram(spanBytes);
      if (tmp) {
        const uint32_t fileOff = bitmapBase_ + (uint32_t)idFirst * bytesPerGlyph_;
        // 一次 seek + 一次连续 read: 这就是把"随机小读"变成"顺序读"的关键
        if (file_.seek(fileOff) && file_.read(tmp, spanBytes) == (int)spanBytes) {
          reads++;
          // 按顺序把每个字形填进各自的散列槽
          for (int k = i; k <= j; k++) {
            const uint32_t slot = (cps[k] * 2654435761u) % GLYPH_CACHE_SLOTS;
            const uint32_t rel = (uint32_t)(ids[k] - idFirst) * bytesPerGlyph_;
            memcpy(cacheData_ + (size_t)slot * bytesPerGlyph_, tmp + rel, bytesPerGlyph_);
            CacheSlot &c = cache_[slot];
            c.cp = cps[k];
            c.glyphId = ids[k];
            c.rows = gh_;
            c.valid = true;
          }
        }
        free(tmp);
      }
      i = j + 1;
    }
  }

  const uint32_t dt = micros() - t0;
  s_dbgSdUs += dt;
  s_dbgSdN += reads;
  BOOTLOG("[fprefetch] 整页预取 %d 个字形 -> %lu 次顺序读, 耗时 %lu ms", n, (unsigned long)reads,
          (unsigned long)(dt / 1000));
  fontPerfTick();

  free(cps);
  free(ids);
  return n;
}

// ---------------------------------------------------------------------------
// 位图 -> 画布输出
// ---------------------------------------------------------------------------
// 1bpp: 用查表把"一个字节 -> 8 个像素颜色"展开, 避免逐位判断
static uint16_t s_expand1[256][8];
static uint16_t s_expand1Key = 0xFFFF;

static void buildExpandTable(uint16_t fg, uint16_t bg) {
  for (int b = 0; b < 256; b++) {
    for (int i = 0; i < 8; i++) {
      s_expand1[b][i] = (b & (0x80 >> i)) ? fg : bg;
    }
  }
  s_expand1Key = fg;
}

static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  // a: 0..15 (0 = 全背景, 15 = 全前景)
  if (a == 0) {
    return bg;
  }
  if (a >= 15) {
    return fg;
  }
  const int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  const int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  const int r = br + ((fr - br) * a) / 15;
  const int g = bgc + ((fgc - bgc) * a) / 15;
  const int b = bb + ((fb - bb) * a) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void DotFont::blit1bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();   // 【诊断】像素绘制耗时
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 7) / 8;

  if (s_expand1Key != fg) {
    buildExpandTable(fg, bg);
  }

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry++) {
    const int16_t py = y + ry;
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx += 8) {
      const uint16_t *px = s_expand1[src[rx >> 3]];
      const int16_t px0 = x + rx;
      for (int i = 0; i < 8; i++) {
        const int16_t xx = px0 + i;
        if (xx >= 0 && xx < cw) {
          row[xx] = px[i];
        }
      }
    }
  }  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

// 12x12 缩小绘制: 隔行隔列采样
void DotFont::blit1bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();   // 【诊断】像素绘制耗时
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 7) / 8;

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry += 2) {
    const int16_t py = y + (ry >> 1);
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx += 2) {
      const bool on = (src[rx >> 3] & (0x80 >> (rx & 7))) != 0;
      const int16_t xx = x + (rx >> 1);
      if (xx >= 0 && xx < cw) {
        row[xx] = on ? fg : bg;
      }
    }
  }  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

// ---------------------------------------------------------------------------
// 2bpp 位图绘制(PSRAM 压缩档专用, 4 级灰度)
//
// 布局: 每行 (gw+3)/4 = 6 字节, 每字节 4 个像素, 高位在左(和 4bpp 的"高 4 位是
// 左边像素"一致); 灰度值 0..3 线性映射到 blend565 的 0..15 (×5)。
// ---------------------------------------------------------------------------
void DotFont::blit2bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 3) / 4;

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry++) {
    const int16_t py = y + ry;
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + (uint32_t)ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx++) {
      const uint8_t byte = src[rx >> 2];
      const uint8_t a2 = (uint8_t)((byte >> (6 - ((rx & 3) << 1))) & 0x03);
      const int16_t xx = x + rx;
      if (xx >= 0 && xx < cw) {
        row[xx] = blend565(fg, bg, (uint8_t)(a2 * 5));
      }
    }
  }
  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

// 12x12 缩小绘制(2bpp): 隔行隔列采样, 保留灰度
void DotFont::blit2bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 3) / 4;

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry += 2) {
    const int16_t py = y + (ry >> 1);
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + (uint32_t)ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx += 2) {
      const uint8_t byte = src[rx >> 2];
      const uint8_t a2 = (uint8_t)((byte >> (6 - ((rx & 3) << 1))) & 0x03);
      const int16_t xx = x + (rx >> 1);
      if (xx >= 0 && xx < cw) {
        row[xx] = blend565(fg, bg, (uint8_t)(a2 * 5));
      }
    }
  }
  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

void DotFont::blit4bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();   // 【诊断】像素绘制耗时
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 1) / 2;

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry++) {
    const int16_t py = y + ry;
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx++) {
      const uint8_t byte = src[rx >> 1];
      const uint8_t a = (rx & 1) ? (byte & 0x0F) : (byte >> 4);
      const int16_t xx = x + rx;
      if (xx >= 0 && xx < cw) {
        row[xx] = blend565(fg, bg, a);
      }
    }
  }  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

// 12x12 缩小绘制(4bpp 灰度): 隔行隔列采样, 保留抗锯齿
//
// ★ 这个函数是必须的 ★
//   原来的小字路径在 bpp==4 时错误地调用了 blit4bpp(全尺寸 24x24),
//   而光标只前进 FONT_W/2 = 12 像素 —— 于是每个字都重叠一半,
//   看起来就是"每个字只显示一半、还挤在一起"。用 4bit 字库才会踩到, 1bit 不会。
void DotFont::blit4bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg) {
  const uint32_t t_dbg0 = micros();   // 【诊断】像素绘制耗时
  const int16_t cw = canvas.width();
  const int16_t ch = canvas.height();
  const uint16_t stride = (gw_ + 1) / 2;

  uint16_t *fb = canvas.pixels();
  if (!fb) {
    return;
  }

  for (uint8_t ry = 0; ry < gh_; ry += 2) {
    const int16_t py = y + (ry >> 1);
    if (py < 0 || py >= ch) {
      continue;
    }
    uint16_t *row = fb + (size_t)py * cw;
    const uint8_t *src = bmp + ry * stride;
    for (uint8_t rx = 0; rx < gw_; rx += 2) {
      const uint8_t byte = src[rx >> 1];
      const uint8_t a = (rx & 1) ? (byte & 0x0F) : (byte >> 4);
      const int16_t xx = x + (rx >> 1);
      if (xx >= 0 && xx < cw) {
        row[xx] = blend565(fg, bg, a);
      }
    }
  }  s_dbgBlitUs += (micros() - t_dbg0);
  s_dbgBlitN++;
}

void DotFont::drawMissingBox(Canvas16 &canvas, int16_t x, int16_t y, uint16_t color) {
  // 缺字占位: 空心方框, 一眼能看出字库没这个字
  const int16_t w = gw_ - 6;
  const int16_t h = gh_ - 6;
  if (w <= 0 || h <= 0) {
    return;
  }
  const int16_t x0 = x + 3, y0 = y + 3;
  for (int16_t i = 0; i < w; i++) {
    canvas.setPixel(x0 + i, y0, color);
    canvas.setPixel(x0 + i, y0 + h - 1, color);
  }
  for (int16_t i = 0; i < h; i++) {
    canvas.setPixel(x0, y0 + i, color);
    canvas.setPixel(x0 + w - 1, y0 + i, color);
  }
}

// ---------------------------------------------------------------------------
// 对外渲染接口
// ---------------------------------------------------------------------------

// 内置 5x7 ASCII 点阵, 放大后绘制。
//
// ★ 为什么 ASCII 不走字库 ★
//   来自 CJK 字库(思源宋体/黑体)的 ASCII 字形只占 24x24 格子的左侧一小条,
//   而排版时 ASCII 的步进按"半宽"算(12 或 6 像素)。结果就是字形比步进宽一倍,
//   每个字母/数字都压掉一半 -> "英文数字全糊在一起"。
//   中文是方块字占满格子, 所以看起来正常, 只有 ASCII 出问题。
//   这里改用内置 5x7 点阵: 宽 5*scale、步进 6*scale, 天然匹配半宽语义。
static inline bool asciiGlyph(Canvas16 &canvas, int16_t x, int16_t y, uint32_t cp, uint16_t fg, uint16_t bg,
                              uint8_t scale) {
  if (cp < 32 || cp > 126) {
    return false;
  }
  const uint8_t *glyph = FONT5X7[cp - 32];
  for (uint8_t col = 0; col < 5; col++) {
    const uint8_t bits = pgm_read_byte(&glyph[col]);
    for (uint8_t row = 0; row < 7; row++) {
      const bool on = (bits >> row) & 1;
      for (uint8_t sy = 0; sy < scale; sy++) {
        for (uint8_t sx = 0; sx < scale; sx++) {
          canvas.setPixel(x + col * scale + sx, y + row * scale + sy, on ? fg : bg);
        }
      }
    }
  }
  return true;
}

bool DotFont::drawGlyph(Canvas16 &canvas, int16_t x, int16_t y, uint32_t cp, uint16_t fg, uint16_t bg) {
  if (cp == '\n' || cp == '\r') {
    return true;
  }

  // ASCII 用内置点阵, 不走字库(理由见上面 asciiGlyph 的注释)。
  // 步进是 12 像素, 所以取 scale=2 -> 字形 10x14, 正好在格子内。
  if (cp >= 32 && cp <= 126) {
    asciiGlyph(canvas, x, y + 5, cp, fg, bg, 2);   // 垂直居中: (24-14)/2 = 5
    return true;
  }

  GlyphMeta m;
  if (!ready_ || !lookup(cp, &m)) {
    drawMissingBox(canvas, x, y, fg);
    return false;
  }

  const uint8_t *bmp = loadGlyph(file_, cp, m.glyphId);
  if (!bmp) {
    drawMissingBox(canvas, x, y, fg);
    return false;
  }

  // 位深由 loadGlyph 决定: 2 = PSRAM 压缩档, 1/4 = 文件原始位深
  if (lastBmpBpp_ == 2) {
    blit2bpp(canvas, x, y, bmp, fg, bg);
  } else if (lastBmpBpp_ == 1) {
    blit1bpp(canvas, x, y, bmp, fg, bg);
  } else {
    blit4bpp(canvas, x, y, bmp, fg, bg);
  }
  return true;
}

// 解码一个 UTF-8 字符, 返回消耗的字节数(0 = 结束)
static uint8_t nextCp(const uint8_t *&p, uint32_t &cp) {
  const uint8_t b = *p;
  if (b == 0) {
    return 0;
  }
  uint8_t len;
  if (b < 0x80) {
    cp = b;
    len = 1;
  } else if ((b & 0xE0) == 0xC0) {
    cp = b & 0x1F;
    len = 2;
  } else if ((b & 0xF0) == 0xE0) {
    cp = b & 0x0F;
    len = 3;
  } else if ((b & 0xF8) == 0xF0) {
    cp = b & 0x07;
    len = 4;
  } else {
    p++;
    return 1;  // 跳过坏字节
  }
  for (uint8_t i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      p++;
      return 1;  // 非法序列, 只跳一个字节
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  p += len;
  return len;
}

int16_t DotFont::measureUtf8(const char *utf8) const {
  int16_t w = 0;
  const uint8_t *p = (const uint8_t *)utf8;
  while (*p) {
    uint32_t cp = 0;
    if (nextCp(p, cp) == 0) {
      break;
    }
    w += (cp < 0x80) ? (gw_ / 2) : gw_;
  }
  return w;
}

int16_t DotFont::drawUtf8(Canvas16 &canvas, int16_t x, int16_t y, const char *utf8, uint16_t fg, uint16_t bg) {
  int16_t cx = x;
  const uint8_t *p = (const uint8_t *)utf8;
  while (*p) {
    uint32_t cp = 0;
    if (nextCp(p, cp) == 0) {
      break;
    }
    drawGlyph(canvas, cx, y, cp, fg, bg);
    cx += (cp < 0x80) ? (gw_ / 2) : gw_;
  }
  return (int16_t)(cx - x);
}

int16_t DotFont::drawUtf8Small(Canvas16 &canvas, int16_t x, int16_t y, const char *utf8, uint16_t fg, uint16_t bg) {
  int16_t cx = x;
  const uint8_t *p = (const uint8_t *)utf8;
  while (*p) {
    uint32_t cp = 0;
    if (nextCp(p, cp) == 0) {
      break;
    }

    // ASCII 用内置点阵: 步进是 6 像素, 所以 scale=1 -> 字形 5x7, 正好占满格子
    if (cp >= 32 && cp <= 126) {
      asciiGlyph(canvas, cx, y + 3, cp, fg, bg, 1);   // 垂直居中: (12-7)/2 ≈ 3
      cx += (gw_ / 4);
      continue;
    }

    GlyphMeta m;
    if (ready_ && lookup(cp, &m)) {
      const uint8_t *bmp = loadGlyph(file_, cp, m.glyphId);
      if (bmp) {
        if (lastBmpBpp_ == 2) {
          blit2bppHalf(canvas, cx, y, bmp, fg, bg);   // 2bpp 档的小字(4 级灰度)
        } else if (lastBmpBpp_ == 1) {
          blit1bppHalf(canvas, cx, y, bmp, fg, bg);
        } else {
          blit4bppHalf(canvas, cx, y, bmp, fg, bg);   // 必须是 Half 版本: 光标只前进半格
        }
      }
    }
    cx += (gw_ / 2);
  }
  return (int16_t)(cx - x);
}