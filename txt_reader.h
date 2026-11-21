/*
 * txt_reader.h — TXT 分页与翻页
 *
 * 【核心难点与解法】
 * 分页器为了排版会把文件多读一点(内部有字节缓冲)。翻页时如果直接 seek 到下一页
 * 偏移, 会把"已读进缓冲但还没排进页面"的字节丢掉, 表现为吞字/重复。
 *
 * 解法: 每次排完一页, 用
 *     nextOffset = file.position() - paginator.unconsumed()
 * 把文件指针拉回到"这一页真正的结束位置"。分页器每页重建一次, 状态干净。
 *
 * 【章节跳转】
 * 打开文件时扫描一遍(最多 TXT_SCAN_LIMIT 字节, 分块读), 记录形如
 * "第X章/节/卷/回/篇" 的行偏移做成章节目录。5MB 文件一次扫描约 1~2 秒,
 * 期间屏幕上显示"正在扫描", 只在打开书的时候发生。
 */
#pragma once

#include <Arduino.h>
#include <FS.h>

#define TXT_MAX_CHAPTERS 512
#define TXT_SCAN_LIMIT   (8 * 1024 * 1024)  // 最多扫描 8MB 找章节
#define TXT_LINE_MAX     192                // 与 text.h 的 TEXT_LINE_MAX 一致

struct Chapter {
  char title[40];     // 章节标题(UTF-8, 截断显示)
  uint32_t offset;    // 该章第一个字节在文件里的偏移
  uint32_t pageHint;  // 该章大约是第几页(扫描时估算, 可能不准)
};

class TxtReader {
public:
  // 一本书最多缓存多少页(每页 CHARS_PER_PAGE 个 UTF-16 单元)
  // 缓存用来看"上一页"和快速回退, 200 页 * 9*11*2B 约 40KB, 放 PSRAM
  static const int CACHE_PAGES = 200;

  TxtReader();
  ~TxtReader();

  // 打开文件。fs 通常是 SD。失败时 err() 里有原因。
  // 注意: 本函数内部会拿 sd_mutex(扫描章节), 不要在持锁时调用。
  bool open(FS &fs, const char *path);

  void close();
  bool isOpen() const {
    return file_;
  }
  const char *err() const {
    return err_;
  }
  const char *name() const {
    return name_;
  }

  // 文件字节大小
  uint32_t fileSize() const {
    return fileSize_;
  }

  // 源文件是否被判定为 GBK(目前会明确报错, 不静默乱码)
  bool isGbk() const {
    return gbk_;
  }

  // 当前页纯文本的字节数(UTF-8)。用来区分"没读到字"和"读到了但没画出来"。
  uint32_t pageTextBytes() const {
    return pageTextBytes_;
  }

  // ---- 页操作 ----
  // 载入第 page 页(从 0 开始)的文本到 lastPage_, 返回实际行数(0 = 超出文件末尾)
  int loadPage(uint32_t page);

  // 当前页号(最近一次 loadPage 成功的页)
  uint32_t page() const {
    return page_;
  }

  // 总页数。只有完整扫过一遍才知道; 扫描过程中会更新, 未扫完时是估算值。
  uint32_t pageCount() const {
    return pageCount_;
  }
  bool pageCountKnown() const {
    return pageCountKnown_;
  }

  // 当前页的第 line 行文本(UTF-8)。越界返回 ""。
  const char *line(int line) const;

  // 当前页的行数
  int lineCount() const {
    return linesInPage_;
  }

  // 当前页起始字节偏移 / 当前页结束(下一页起始)字节偏移
  uint32_t pageStartOffset() const {
    return pageStart_;
  }
  uint32_t pageEndOffset() const {
    return pageEnd_;
  }

  // 供状态栏显示: 大概读了百分之几
  uint8_t percent() const;

  // 打开时扫描出来的章节
  int chapterCount() const {
    return chapterCount_;
  }
  const Chapter *chapter(int i) const {
    return (i >= 0 && i < chapterCount_) ? &chapters_[i] : nullptr;
  }
  // 当前页所在的章节序号(-1 = 没找到)
  int currentChapter() const;

  // 跳到指定章节, 成功后 *pageOut 是该章起始页号
  bool jumpToChapter(int idx, uint32_t *pageOut);

  // 用于显示"正在扫描"的回调(扫描期间周期性调用, 可用来刷界面)
  typedef void (*ScanProgressFn)(void *ctx, uint32_t bytesDone, uint32_t total);
  void setScanProgress(ScanProgressFn fn, void *ctx) {
    scanFn_ = fn;
    scanCtx_ = ctx;
  }

  // 是否停止扫描(用户按键打断)
  typedef bool (*AbortFn)(void *ctx);
  void setAbortCheck(AbortFn fn, void *ctx) {
    abortFn_ = fn;
    abortCtx_ = ctx;
  }

private:
  FS *fs_ = nullptr;
  File file_;
  bool gbk_ = false;  // 源文件是否 GBK(目前会明确报错)
  char err_[192] = {0};
  char name_[32] = {0};

  uint32_t fileSize_ = 0;
  uint32_t page_ = 0;
  // ★ page_ 指向的那一页是否**真的排过版**(即 pageLines_/linesInPage_/pageStart_/
  //   pageEnd_ 与 page_ 一致)。为什么必须有它: close()/open() 会把 page_ 归零,
  //   同时 linesInPage_ 也是 0, 而 loadPage() 里"已经在这一页"的快捷返回只看页号
  //   (page == page_), 于是打开书后的第一次 loadPage(0) 会命中它、静默返回 0 行,
  //   屏幕全白、且一条 [pg] 日志都不打。见 txt_reader.cpp 的 loadPage()。
  bool pageValid_ = false;
  uint32_t pageStart_ = 0;
  uint32_t pageEnd_ = 0;
  uint32_t pageCount_ = 1;
  bool pageCountKnown_ = false;

  int linesInPage_ = 0;
  uint32_t pageTextBytes_ = 0;   // 当前页纯文本字节数(自检用)
  char *pageLines_ = nullptr;  // ROWS * TXT_LINE_MAX 的字符块

  Chapter *chapters_ = nullptr;
  int chapterCount_ = 0;

  // ---- 稀疏页索引 ----
  // index_[i] = 第 (i * PAGE_INDEX_STEP) 页的起始字节偏移。
  // 顺序翻页时顺手填充, 于是"跳到第 N 页"最多只需重排 PAGE_INDEX_STEP 页。
  struct PageIndexEntry {
    uint32_t page;
    uint32_t offset;
  };
  PageIndexEntry *index_ = nullptr;
  int indexCount_ = 0;

  ScanProgressFn scanFn_ = nullptr;
  void *scanCtx_ = nullptr;
  AbortFn abortFn_ = nullptr;
  void *abortCtx_ = nullptr;

  bool detectEncoding();
  void scanChapters();
  uint32_t estimatePageCount() const;

  // 从指定字节偏移排一页到 pageLines_, 更新 pageStart_/pageEnd_/linesInPage_
  int layoutPageAt(uint32_t offset);
  void indexPut(uint32_t page, uint32_t offset);
  bool indexFindFloor(uint32_t page, uint32_t *offset) const;
  void indexReset();
};
