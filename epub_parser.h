/*
 * epub_parser.h - EPUB 解析（ZIP 解压 + XHTML 取文本 + 插图提取）
 *
 * 【设计决定：为什么要"解压成 txt 缓存"，而不是直接流式分页】
 *   本项目第一步的排版引擎(text.cpp 的 Paginator)已经用 38 万字真实小说验证过
 *   字符一个不丢。EPUB 直接流式喂给它需要改动核心的缓冲区/回退逻辑(那是踩过
 *   三个真 bug 才修好的地方), 风险太高。
 *   所以这里走"先解压成纯文本缓存文件, 之后完全复用 TXT 阅读器"的路线:
 *     - 首次打开一本 epub: 解压正文 -> /cache/<名字>.txt (跳过 17MB 图片, 很快)
 *     - 之后再打开: 直接读缓存, 秒开
 *   代价是 SD 卡上多一份纯文本(约几百 KB), 换来的是排版逻辑零改动、零风险。
 *
 * 【解压用什么】
 *   ESP32 的 ROM 里自带 miniz(esp_rom/include/miniz.h), 提供 tinfl_decompress,
 *   不需要额外库。这里只做"读 ZIP 中央目录 + 按需解压单个条目"。
 *
 * 【已验证的 EPUB 结构(用真实文件确认过)】
 *   mimetype(不压缩) / META-INF/container.xml -> content.opf / item/xhtml/*.xhtml
 *   正文页用 <p> 分段、缩进是全角空格(不是 CSS)、<br> 空行、无 ruby 注音。
 */
#pragma once

#include <Arduino.h>
#include <FS.h>

#define EPUB_MAX_SPINE   256   // spine 条目上限
#define EPUB_MAX_MANIFEST 512  // manifest 条目上限
#define EPUB_MAX_IMAGES  256   // 书籍里图片数上限(超出部分只保留前 N 张)
#define EPUB_IMG_NAME_LEN 128  // zip 内图片路径最大长度

// ★ EPUB 缓存版本号 ★
//   为什么需要(2026-10-03 教训): 正文提取逻辑改过好几版, 而旧缓存会被**无条件复用** ——
//   于是出现"代码已经修好了, 界面还是老样子", 我们为此白查了好几轮(甚至怀疑过卡和硬件)。
//
//   做法: 版本号写进**缓存文件名**(/cache/<书名>.v1.txt), 而不是文件内容里。
//   ★ 曾经把它写在缓存第一行, 结果阅读器把 "EPUBCACHE" 当正文 -> 第 1 页被占掉 ->
//     分页全乱 -> 症状是"插图只显示前几张 + 正文被吞"。正文文件必须纯净。★
//
//   以后只要改了提取逻辑(markImagesInXhtml / epubXhtmlToText / 标记编号规则),
//   把这个数字加一 = 旧缓存自动失效, 不需要用户手动删 /cache。
#define EPUB_CACHE_VER 1

// 解析进度回调：phase 是阶段名，cur/total 是进度
typedef void (*EpubProgressFn)(void *ctx, const char *phase, int cur, int total);

class EpubParser {
public:
  EpubParser();

  // 打开 epub 并解析 container.xml / content.opf（只读少量字节，很快）
  bool open(FS &fs, const char *path);
  void close();
  bool isOpen() const {
    return file_;
  }
  const char *err() const {
    return err_;
  }

  int spineCount() const {
    return spineCount_;
  }
  // 估算的纯文本大小(用于显示进度百分比)
  uint32_t estimatedTextBytes() const {
    return estTextBytes_;
  }

  // 把 spine 里的正文全部解压并提取为纯文本，写入 outPath。
  // 每个 spine 项之间插入章节分隔标记（"\n\n"），保持段落结构。
  bool extractTextTo(const char *outPath, EpubProgressFn fn = nullptr, void *ctx = nullptr);

  // 解压指定 spine 项为一个图片文件（用于插图页显示）。
  // outPath 为空则只查询信息。返回解压后的字节数，0 表示不是图片或失败。
  uint32_t extractImage(int spineIndex, const char *outPath);

  // 提取过程中发现的图片（正文里的 <img> 和整页 <svg><image>）
  int imageCount() const {
    return imageCount_;
  }
  // 第 index 张图片解压到 outPath；返回写出的字节数
  uint32_t extractImageByIndex(int index, const char *outPath);
  // 第 index 张图片的扩展名（".jpg" / ".png"），用于拼缓存文件名
  const char *imageExt(int index);

  // ★ 一次性把全部插图导出到 dir/img_NN.jpg|png ★
  //   - dir 不存在会自动创建
  //   - 命名规则只留这一处, 免得"导出"和"查找"两边各写一份 %02d 而改岔
  //   - 内部会先 ensureImageList(): 复用正文缓存时清单本来是空的
  //   - 返回成功导出的张数; fn 用于进度(约定 cur = 百分比, total = 100)
  int extractAllImages(const char *dir, EpubProgressFn fn = nullptr, void *ctx = nullptr);

  // ★ 让"只解析、不提取正文"这条路径也能拿到图片清单 ★
  //   图片清单原本只在 extractTextTo() 里由 markImagesInXhtml() 顺带登记,
  //   所以复用正文缓存时 imageCount() 恒为 0。这个函数按**和提取正文完全相同的
  //   顺序**重扫一遍各章 XHTML(不产出文本), 保证编号和缓存里的 @N@ 标记一致。
  //   返回扫描后的图片张数(调用之后 imageCount() 就是有效值)。
  int ensureImageList();

private:
  FS *fs_ = nullptr;
  File file_;
  bool ready_ = false;
  bool imagesScanned_ = false;   // 已经扫过图片清单(没有图的书不必反复扫)
  char err_[160] = {0};

  char opfPath_[128] = {0};
  char opfDir_[96] = {0};   // opf 所在目录(用来拼相对路径)

  struct Entry {
    uint32_t localOffset;   // 本地头偏移
    uint32_t compSize;
    uint32_t rawSize;
    uint16_t method;        // 0=store 8=deflate
    uint16_t nameLen;       // 名字长度(用于算数据起始)
    uint16_t nameExtra;     // 本地头里 extra 字段长度
  };

  // manifest: id -> href 的映射(存散列 + 偏移)
  struct ManifestItem {
    uint32_t idHash;
    char href[128];
  };
  ManifestItem *manifest_ = nullptr;
  int manifestCount_ = 0;

  // spine: 按顺序存 href
  char (*spine_)[128] = nullptr;
  int spineCount_ = 0;
  uint32_t estTextBytes_ = 0;

  char (*images_)[EPUB_IMG_NAME_LEN] = nullptr;
  uint16_t *imageSpine_ = nullptr;
  int imageCount_ = 0;

  bool findEntry(const char *name, Entry *out);
  bool readEntryAll(const Entry &e, uint8_t **bufOut, uint32_t *lenOut, bool preferPsram);
  bool parseContainer();
  bool parseOpf();
  // 遇到新图片时记录并返回其索引(已存在则返回原索引)
  int noteImage(const char *zipPath);
  // 在原始 XHTML 上把图片引用替换成标记(必须在剥标签之前调用)
  uint32_t markImagesInXhtml(const char *xhtml, uint32_t len, const char *xhtmlPath, char *out, uint32_t outCap);
  static uint32_t hashName(const char *s);
};

// 把 XHTML 转成纯文本(去标签/处理 <br> 与 </p> 换行)
// 返回写入 out 的字节数; outCap 不足时截断并返回已写长度。
uint32_t epubXhtmlToText(const char *xhtml, uint32_t len, char *out, uint32_t outCap);

// 数一数 dir 里已经导出了多少张 img_NN.*（判断"要不要补导出"用，不解析 epub）。
// 用于"正文缓存可以复用, 但插图必须确认已经导出"这条路径。
int epubCountExportedImages(FS &fs, const char *dir);
