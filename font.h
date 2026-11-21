/*
 * font.h - 24x24 bitmap font loader + Unicode glyph rendering
 *
 * 字库文件格式 .f24 (由电脑端 tools/build_font.py 生成, 放在 SD 卡 /fonts/gbk24.f24)
 *
 *   偏移     长度    内容
 *   0x00     4       magic "F24A"
 *   0x04     1       version = 1
 *   0x05     1       bpp: 1 = 1bit 黑白, 4 = 4bit 灰度(抗锯齿)
 *   0x06     1       glyph_w = 24
 *   0x07     1       glyph_h = 24
 *   0x08     4       生成时间戳(仅参考)
 *   0x0C     4       count = N (小端 uint32)
 *   0x10     16      保留, 填 0
 *   0x20     N*2     索引表: 按 Unicode 码点升序的 uint16[N]
 *   0x20+2N  N*B     字形位图, 第 i 个字形 = 索引表第 i 项对应的字, 行优先
 *
 *   位图布局(行优先, 每行从高位到低位):
 *     bpp=1: 每行 3 字节 (ceil(24/8)), 一个字形 72 字节。位=1 -> 前景色
 *     bpp=4: 每行 12 字节 (ceil(24/2)), 一个字形 288 字节。高 4 位是左边像素
 *
 *   索引表按 Unicode 升序排列, 设备端直接二分查找, 因此不需要 GBK->Unicode
 *   转换表。索引表约 21000*2 = 42KB, 可以整表放进 PSRAM。
 */
#pragma once

#include <Arduino.h>
#include <FS.h>

#include "display.h"  // Canvas16

#define FONT_MAGIC       "F24A"
#define FONT_HEADER_SIZE 32

// 字形查询结果
struct GlyphMeta {
  bool found;
  uint16_t glyphId;
  uint32_t fileOffset;  // 位图在字库文件里的绝对偏移
};

class DotFont {
public:
  DotFont();

  // 打开字库文件并把索引表读进内存。失败时 lastError() 里有原因。
  bool begin(FS &fs, const char *path);

  // 当前已加载字库的路径(状态栏/串口显示用)
  const char *loadedPath() const {
    return path_;
  }

  // 扫描 dir 目录, 找第一个 .f24 字库文件写到 out。
  // 这样 /fonts/ 下放什么名字都能认, 不必改回 gbk24.f24。
  // 找到返回 true; 目录不存在或没有 .f24 返回 false。
  static bool findFontFile(FS &fs, const char *dir, char *out, size_t outLen);

  // 释放索引表和缓存
  void end();

  bool ready() const {
    return ready_;
  }
  const char *lastError() const {
    return err_;
  }
  uint16_t glyphCount() const {
    return count_;
  }
  uint8_t bpp() const {
    return bpp_;
  }
  uint8_t glyphW() const {
    return gw_;
  }
  uint8_t glyphH() const {
    return gh_;
  }

  // Unicode 码点 -> 字形编号(二分查找)。false 表示字库里没有这个字。
  bool lookup(uint32_t cp, GlyphMeta *out) const;

  // 把一个字形画到画布 (x, y)。
  // 返回 true 表示画了字形; false 会画一个空心方框占位, 缺字一眼可见。
  bool drawGlyph(Canvas16 &canvas, int16_t x, int16_t y, uint32_t cp, uint16_t fg, uint16_t bg);

  // 画 UTF-8 字符串, 返回用掉的像素宽度
  int16_t drawUtf8(Canvas16 &canvas, int16_t x, int16_t y, const char *utf8, uint16_t fg, uint16_t bg);

  // 缩小绘制: 把 24x24 字形隔行隔列采样成 12x12, 用于状态栏这种小字。
  // 返回用掉的宽度(ASCII 6px, 汉字 12px)。
  int16_t drawUtf8Small(Canvas16 &canvas, int16_t x, int16_t y, const char *utf8, uint16_t fg, uint16_t bg);

  // 量一下字符串画出来有多宽(不绘制)
  int16_t measureUtf8(const char *utf8) const;

  // ---------------------------------------------------------------------------
  // 整页字形预取 —— 把"随机小读"变成"一趟顺序读"
  //
  // ★ 为什么需要它(实测数据) ★
  //   每次字形未命中 = 一次 f.seek() + read(288 字节), 实测约 **286~320 毫秒**。
  //   原因不是 288 字节难读, 而是字库 6.8MB、字形在文件里随机分布,
  //   f.seek() 要沿着 FAT 链一簇簇跳过去。
  //   一页正文有 40~88 个不同汉字, 若逐个按需读 = 十几秒。
  //
  // ★ 做法 ★
  //   把这一页所有字符先收集起来, 按"在字库里的位置(id 升序)"排序,
  //   再一趟顺序读下来(相邻字形合并成一次连续读)。
  //   随机 seek 变成单调顺序读, 磁盘利用率天差地别。
  //
  // utf8 可以传多段(例如整页每一行各传一次), 用 count 指定段数。
  // 返回实际预取的字形个数。
  // ---------------------------------------------------------------------------
  int prefetchUtf8(const char *const *utf8, int count);
  int prefetchUtf8(const char *utf8) {
    return prefetchUtf8(&utf8, 1);
  }

  // ---------------------------------------------------------------------------
  // LVGL 桥接(只读) —— 取一个码点的**原始字形位图**, 供 lv_font_t 回调使用
  //
  // ★ 为什么需要它 ★
  //   LVGL 的 lv_font_t 要的是"原始位图指针 + 位深 + 包围盒", 而现有 drawGlyph()
  //   只能把字形画到 Canvas16 上, 拿不到位图。所以这里开一个**只读出口**, 把已经存在
  //   的 private loadGlyph() 的结果原样暴露出来 —— 不新增任何读卡路径, 不改变现有行为。
  //
  // 返回 nullptr  = 字库里没有这个字(或字库没就绪) -> 调用方自己跳过
  // *outBpp       = 位深: 1 / 2(PSRAM 压缩档) / 4(文件原始档)
  // *outGw/*outGh = 位图宽高(像素), 即字库头里的 gw_/gh_(24x24)
  //
  // 位图布局(与 LVGL 8 的期望**完全一致**, 不需要任何转换):
  //   行优先, 每行 (gw*bpp + 7) / 8 字节; LVGL 侧 width_bit = box_w*bpp 且行间不补齐,
  //   gw=24 时 1/2/4bpp 分别是 3/6/12 字节/行, 都刚好整字节。
  //   像素顺序: 左侧像素在每字节的高位(4bpp 高半字节 = 左边像素), 与 LVGL 相同。
  //
  // 指针有效期: 直到下一次调用本函数(缓存槽会被复用)。LVGL 取到位图后当帧就用掉,
  //             所以这个有效期足够; 但**不要跨次保存**。
  // ---------------------------------------------------------------------------
  const uint8_t *glyphBitmap(uint32_t cp, uint8_t *outBpp, uint8_t *outGw, uint8_t *outGh);

  // ---------------------------------------------------------------------------
  // ★ 字库位图进 PSRAM: 一次性把整份字形搬进内存, 之后**零 seek、零 SD 读** ★
  //
  // 为什么必须这么做(实测): 一次字形未命中 = f.seek() + read(288B) ≈ 286~320ms。
  // "整页预取"只能把 60 次读降到 55 次(字形在 23459 个里随机分布, 合并率极低),
  // 治不了本。唯一治本的办法是让绘制时**根本不需要碰 SD**。
  //
  // 内存: 字库位图 6.44MB(4bpp) 装不进 PSRAM, 所以在加载时降位深:
  //   4bpp -> 2bpp: 144 B/字形, 23459 个 = 3.38 MB  (4 级灰度, 视觉几乎无差)
  //   4bpp -> 1bpp:  72 B/字形, 23459 个 = 1.69 MB  (无抗锯齿, 兜底档)
  // 两档都是 100% 覆盖(不是"前 N 个字形"——实测按码点取前缀在 4MB 时只覆盖
  // 真实正文的 79.4%, 一页会漏 22 个字 -> 22×300ms, 等于没修)。
  //
  // 预算不足时自动降档; 再不够就只装能装下的部分(其余回落按需读), 功能不退化。
  // 返回 true 表示已经装进 PSRAM(之后 loadGlyph 不再碰 SD)。
  // ---------------------------------------------------------------------------
  bool loadBitmapsToPsram();
  uint32_t psramGlyphCount() const {
    return psramGlyphs_;
  }
  size_t psramBytes() const {
    return psramBytes_;
  }
  uint8_t psramBpp() const {
    return psramBpp_;
  }

  // ---------------------------------------------------------------------------
  // ★★ 常用字"瘦身子集": 整份进 PSRAM, 生僻字自动回落主字库 ★★
  //
  // ★ 为什么需要它(全部是这块板子的实测数字) ★
  //   SD 顺序读 **38 KB/s**(读 4KB=106ms、读 128KB=3285ms, 吞吐与块大小无关)。
  //   于是"把全量字库搬进内存"这条路彻底不成立:
  //       全量 4bpp 6.44MB -> PSRAM 装不下(8MB 已用约 1MB)
  //       全量 2bpp 3.38MB -> 能装下但要 **89 秒**
  //   而瘦身子集 2bpp×(GB2312 全集 ∪ 语料) 只要 **0.94MB / 约 25 秒**。
  //
  // ★ 两段式布局 ★
  //   第 1 段(快): 子集里的常用字 —— 直接给 PSRAM 指针, **零锁、零 SD、零 memcpy**
  //   第 2 段(兜底): 子集里没有的生僻字 —— 回落本月已打开的主字库文件按需读 SD
  //                  (慢, 但每页最多几个字, 而且**绝不缺字**)
  //
  // ★ 为什么不用"稀疏布局"(count 仍是 23459, 缺的字形填 0) ★
  //   稀疏 = 位图区必须留满 23459 个槽: 2bpp 也要 3.38MB, 正好是我们要避免的东西;
  //   而且空字形会渲染成"看起来像缺字的空白", 用户无法区分"字库没有"和"这字本来就是空的"。
  //   紧凑子集 + 回落主字库: 文件小 3.4 倍, 生僻字还能正常显示。
  //
  // 子集文件格式与主字库完全一致(.f24), 只是 count 更小、索引仍是码点升序。
  // 生成脚本: tools/make_subset_font.py
  // ---------------------------------------------------------------------------
  bool loadSubsetToPsram(FS &fs, const char *path);
  uint32_t subsetGlyphCount() const {
    return subGlyphs_;
  }
  uint8_t subsetBpp() const {
    return subBpp_;
  }
  uint32_t subsetBytes() const {
    return subBytes_;
  }
  const char *subsetPath() const {
    return subPath_;
  }
  // 诊断: 子集命中次数(命中 = 这一次字形访问完全没碰 SD)
  uint32_t statSubsetHit() const;

  // 缓存命中统计, 串口调试用
  uint32_t statHit() const {
    return hit_;
  }
  uint32_t statMiss() const {
    return miss_;
  }

private:
  FS *fs_ = nullptr;
  char path_[96] = {0};   // 已加载字库的路径
  // mutable: lookup() 是 const 方法, 但索引表没常驻内存时它需要 seek/read 文件
  mutable File file_;
  bool ready_ = false;
  char err_[64] = {0};

  uint8_t bpp_ = 1;
  uint8_t gw_ = 0, gh_ = 0;
  uint16_t count_ = 0;
  uint32_t bitmapBase_ = 0;
  uint16_t bytesPerGlyph_ = 0;

  uint16_t *index_ = nullptr;  // PSRAM 中的索引表(升序 Unicode)
  bool indexInRam_ = false;

  // 直接映射的字形位图缓存
  struct CacheSlot {
    uint32_t cp;  // 0xFFFFFFFF = 空
    uint16_t glyphId;
    uint8_t rows;
    bool valid;
  };
  CacheSlot *cache_ = nullptr;
  uint8_t *cacheData_ = nullptr;  // GLYPH_CACHE_SLOTS * bytesPerGlyph_

  uint32_t hit_ = 0, miss_ = 0;

  // ---- PSRAM 字形位图(见 loadBitmapsToPsram 的说明) ----
  uint8_t *bitmapPsram_ = nullptr;      // 压缩后的整份位图
  uint32_t psramGlyphs_ = 0;            // 真正装进去的字形数(从 glyphId 0 起连续)
  size_t psramBytes_ = 0;
  uint8_t psramBpp_ = 0;                // 0 = 没装; 1 = 1bpp; 2 = 2bpp
  uint16_t psramBytesPerGlyph_ = 0;
  // loadGlyph() 本次返回的位图是多少 bpp(2 = PSRAM 压缩档, 其余 = 文件原始位深)
  uint8_t lastBmpBpp_ = 0;

  // ---- 常用字子集(整份在内存: 要么是嵌入 Flash, 要么是 PSRAM) ----
  const uint16_t *subIndex_ = nullptr;  // 升序码点表(count 个 uint16)
  const uint8_t *subBitmap_ = nullptr;  // 紧凑位图: 第 i 个字形在 i*subBytesPerGlyph_
  bool subInFlash_ = false;             // true = 指向嵌入数组(Flash 内存映射), 不能 free
  uint32_t subGlyphs_ = 0;              // 子集字形数
  uint32_t subBytes_ = 0;               // subBitmap_ 占用字节
  uint8_t subBpp_ = 0;                  // 1 / 2 / 4
  uint16_t subBytesPerGlyph_ = 0;
  char subPath_[96] = {0};
  // 把编进固件的那份子集字库(emb_font_subset.c)接上 —— 最高优先级来源。
  // 零 SD、零 PSRAM 拷贝: 位图指针直接指向 Flash 内存映射区。
  bool attachEmbeddedSubset();
  // 码点 -> 子集内位图偏移。命中返回 true, 并写出偏移。
  bool subsetFind(uint32_t cp, uint32_t *outOff) const;

  bool loadIndex();
  // 取字形位图(PSRAM 直取 / 缓存命中 / 从 SD 读)。返回的指针在对应缓冲被复用前有效。
  const uint8_t *loadGlyph(File &f, uint32_t cp, uint16_t glyphId);
  void blit1bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  void blit4bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  void blit1bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  // 4bpp 的 12x12 缩小绘制。小字路径必须用它 —— 光标只前进 FONT_W/2,
  // 若误用全尺寸的 blit4bpp 会导致每个字重叠一半(4bit 字库才踩得到)
  void blit4bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  // PSRAM 里的 2bpp 档专用(4 级灰度, 见 loadBitmapsToPsram)
  void blit2bpp(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  void blit2bppHalf(Canvas16 &canvas, int16_t x, int16_t y, const uint8_t *bmp, uint16_t fg, uint16_t bg);
  void drawMissingBox(Canvas16 &canvas, int16_t x, int16_t y, uint16_t color);
};