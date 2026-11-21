/*
 * txt_reader.cpp — TXT 分页实现
 */
#include "txt_reader.h"
#include "config.h"
#include "text.h"
#include "sd_mutex.h"

#include <string.h>

// 关键诊断直接走 Serial。log_i/log_e 受 Arduino IDE 的 "Core Debug Level" 控制,
// 默认是 None —— 也就是默认什么都不打印, 排障时会完全瞎。这个宏保证一定输出。
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                    \
  do {                                       \
    Serial.printf(fmt "\n", ##__VA_ARGS__);    \
    Serial.flush();                          \
  } while (0)
#endif

// 稀疏页索引: 每 PAGE_INDEX_STEP 页记一个字节偏移, 让"跳到第 N 页"不用从头扫
#define PAGE_INDEX_STEP 16
#define INDEX_MAX       1024  // 可覆盖 16 * 1024 = 16384 页

namespace {

// 说明: 原来这里有个 readerRead() 回调, 在每次读块时再拿一次 sd_mutex。
// 但 layoutPageAt() 调用分页时**已经持有该锁**了, 所以那一层纯属多余,
// 还多一个可能出错的环节。现在 Paginator 直接 file.read(), 已删除。
// (Paginator 的 setReader() 接口本身保留, 它是通用能力, 目前无人使用。)

// 章节名判定: 行首 "第" 之后 1~8 个汉字内出现章节单位字
// 例: "第一章 风起" / "第3节" / "第二卷" 都算; 正文里出现的 "第一章" 因为不在
// 行首(通常伴随缩进全角空格)一般不会被误判, 但缩进的全角空格我们这里先跳过。
bool looksLikeChapterTitle(const char *line) {
  static const char *units[] = {"\xE7\xAB\xA0",  // 章
                                "\xE8\x8A\x82",  // 节
                                "\xE5\x8D\xB7",  // 卷
                                "\xE5\x9B\x9E",  // 回
                                "\xE7\xAF\x87",  // 篇
                                "\xE9\x83\xA8",  // 部
                                "\xE9\x9B\x86"}; // 集
  const uint8_t *p = (const uint8_t *)line;

  // 跳过行首空白(含全角空格 E3 80 80)
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  while (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) {
    p += 3;
  }

  static const char *DI = "\xE7\xAC\xAC";  // 第
  if (memcmp(p, DI, 3) != 0) {
    return false;
  }
  p += 3;

  // 第 之后最多看 8 个字符
  for (int i = 0; i < 8; i++) {
    // 非汉字(空格/数字/字母)允许出现, 继续往后看
    if (*p == 0) {
      return false;
    }
    if (*p < 0x80) {
      p++;
      continue;
    }
    if ((*p & 0xF0) == 0xE0) {
      for (int u = 0; u < 7; u++) {
        if (p[1] == (uint8_t)units[u][1] && p[2] == (uint8_t)units[u][2]) {
          return true;  // 命中 "第X章/节/卷/回/篇/部/集"
        }
      }
      p += 3;
    } else {
      p++;
    }
  }
  return false;
}

// 字节偏移 -> 是否 UTF-8 合法(用于编码嗅探)
bool looksLikeUtf8(const uint8_t *p, size_t n, size_t *badOut) {
  size_t i = 0, bad = 0;
  while (i < n) {
    const uint8_t b = p[i];
    if (b < 0x80) {
      i++;
      continue;
    }
    uint8_t need;
    if ((b & 0xE0) == 0xC0) {
      need = 2;
    } else if ((b & 0xF0) == 0xE0) {
      need = 3;
    } else if ((b & 0xF8) == 0xF0) {
      need = 4;
    } else {
      bad++;
      i++;
      continue;
    }
    if (i + need > n) {
      break;  // 末尾被截断, 不算错
    }
    bool ok = true;
    for (uint8_t k = 1; k < need; k++) {
      if ((p[i + k] & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      bad++;
      i++;
    } else {
      i += need;
    }
  }
  *badOut = bad;
  return bad * 100 < n;  // 坏字节 < 1% 就认为是 UTF-8
}

}  // namespace

// ---------------------------------------------------------------------------
TxtReader::TxtReader() {
  pageLines_ = (char *)ps_malloc((size_t)ROWS * TXT_LINE_MAX);
  if (!pageLines_) {
    pageLines_ = (char *)malloc((size_t)ROWS * TXT_LINE_MAX);
  }
  chapters_ = (Chapter *)ps_malloc(sizeof(Chapter) * TXT_MAX_CHAPTERS);
  if (!chapters_) {
    chapters_ = (Chapter *)malloc(sizeof(Chapter) * TXT_MAX_CHAPTERS);
  }
  index_ = (PageIndexEntry *)ps_malloc(sizeof(PageIndexEntry) * INDEX_MAX);
  if (!index_) {
    index_ = (PageIndexEntry *)malloc(sizeof(PageIndexEntry) * INDEX_MAX);
  }
}

TxtReader::~TxtReader() {
  close();
  if (pageLines_) {
    free(pageLines_);
    pageLines_ = nullptr;
  }
  if (chapters_) {
    free(chapters_);
    chapters_ = nullptr;
  }
  if (index_) {
    free(index_);
    index_ = nullptr;
  }
}

void TxtReader::close() {
  if (file_) {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      file_.close();
    } else {
      file_.close();  // 拿不到锁也得关, 否则文件句柄泄漏
    }
  }
  file_ = File();
  page_ = 0;
  pageValid_ = false;  // ★ 一页都没排过, 禁止 loadPage() 命中"已经在这一页"的快捷返回
  pageStart_ = pageEnd_ = 0;
  linesInPage_ = 0;
  pageTextBytes_ = 0;  // 避免上一本书的字节数残留在自检日志里
  chapterCount_ = 0;
  pageCount_ = 1;
  pageCountKnown_ = false;
  indexReset();
}

bool TxtReader::open(FS &fs, const char *path) {
  close();
  err_[0] = 0;
  fs_ = &fs;

  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      snprintf(err_, sizeof(err_), "打开文件时等 SD 锁超时");
      return false;
    }
    file_ = fs.open(path, FILE_READ);
    if (!file_) {
      snprintf(err_, sizeof(err_), "cannot open %s", path);
      return false;
    }
  }

  fileSize_ = file_.size();
  if (fileSize_ < 32) {
    snprintf(err_, sizeof(err_), "文件太小或不是文本");
    close();
    return false;
  }

  // 文件名(去掉路径)
  const char *slash = strrchr(path, '/');
  const char *base = slash ? (slash + 1) : path;
  strncpy(name_, base, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = 0;

  // 编码嗅探
  if (!detectEncoding()) {
    return false;
  }

  // 章节扫描
  scanChapters();

  pageStart_ = 0;
  page_ = 0;
  pageValid_ = false;  // ★ 打开后还没排过任何一页; 必须让第一次 loadPage(0) 真的去排版
  return true;
}

bool TxtReader::detectEncoding() {
  uint8_t head[4096];
  size_t got = 0;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      snprintf(err_, sizeof(err_), "读文件头等锁超时");
      return false;
    }
    if (!file_.seek(0)) {
      snprintf(err_, sizeof(err_), "seek 失败");
      return false;
    }
    got = file_.read(head, sizeof(head));
  }
  if (got == 0) {
    snprintf(err_, sizeof(err_), "文件读不出内容");
    return false;
  }

  size_t bom = utf8BomLen(head, got);
  if (bom == 3) {
    gbk_ = false;
    return true;
  }

  size_t bad = 0;
  if (looksLikeUtf8(head, got, &bad)) {
    gbk_ = false;
    return true;
  }

  // 不是 UTF-8 => 很可能是 GBK
  gbk_ = true;
  snprintf(err_,
           sizeof(err_),
           "这个 TXT 不是 UTF-8(疑似 GBK)。请用记事本另存为 UTF-8 后重试, "
           "或用电脑端 tools/convert_txt.py 批量转换");
  return false;
}

uint32_t TxtReader::estimatePageCount() const {
  // 每页最多 COLS*ROWS 个字符, 一个汉字 UTF-8 占 3 字节; 再加上换行和废字节,
  // 用 3 字节/字 估算是偏乐观(页数偏少)的, 只用于滚动条, 不参与正确性。
  const uint32_t bytesPerPage = (uint32_t)CHARS_PER_PAGE * 3;
  const uint32_t pc = (fileSize_ + bytesPerPage - 1) / bytesPerPage;
  return pc ? pc : 1;
}

void TxtReader::scanChapters() {
  chapterCount_ = 0;
  if (!chapters_) {
    return;
  }

  // ---------------------------------------------------------------------------
  // ★ 扫描块大小 8KB -> 64KB ★
  //
  // 设备实测 (2026-10-02): **每次 FATFS disk_read 调用固定约 106ms, 与这次读多少
  // 字节无关**。三个数据点互相印证:
  //        单扇区 288B   ≈ 105 ms      (1 次调用)
  //        4KB (1 簇)    = 106.8 ms    (1 次调用)
  //        +124KB        = 3285.7 ms   = 31 x 106 ms   (31 次调用)
  // 也就是"38.5 KB/s"并不是吞吐, 而是 106ms / 4KB 簇 的除法巧合。
  //
  // 而核心库 sd_diskio.cpp 的 ff_sd_read 是这样分流的:
  //        count > 1  -> sdReadSectors()   ← 一次 CMD18 连续读完
  //        count == 1 -> sdReadSector()    ← 单扇区, 固定开销全吃
  // FATFS 的 disk_read 每次要几个扇区, 取决于上层一次 file.read() 想要多少字节。
  // 所以块越大, count 越大, 越能走多扇区快路径, 且调用次数成比例下降。
  //
  // 8KB  -> 16 个扇区, 但仍是"1 次调用 16 扇区"还是"16 次调用"取决于库实现,
  //         实测这版库按 512B 粒度循环 -> 8KB 要 16 次调用 = 1.7 秒
  // 64KB -> 调用次数降到 1/8, 而 PSRAM 有 8MB, 这点内存不算什么
  //
  // 13.txt = 382KB: 按 8KB 分块 = 47 次 read x 16 = 752 次调用 ≈ 80 秒;
  //                 按 64KB 分块 = 6 次 read x 128 = 768 次 -> 但如果库真的走
  //                 sdReadSectors 多扇区路径, 就只剩 6 次调用 ≈ 0.6 秒。
  // 上机看 [scan] 那行日志的耗时即可判定走的是哪条路。
  // ---------------------------------------------------------------------------
  const size_t SCAN_BLK = 64 * 1024;
  uint8_t *blk = (uint8_t *)ps_malloc(SCAN_BLK);
  size_t blkSize = SCAN_BLK;
  if (!blk) {
    // PSRAM 不够就退 16KB, 再不行退 8KB(老行为), 保证功能不受影响
    blkSize = 16 * 1024;
    blk = (uint8_t *)malloc(blkSize);
  }
  if (!blk) {
    blkSize = 8192;
    blk = (uint8_t *)malloc(blkSize);
  }
  if (!blk) {
    return;
  }
  BOOTLOG("[scan] 章节扫描块 = %u KB", (unsigned)(blkSize / 1024));
  const uint32_t scanT0 = millis();

  const uint32_t total = fileSize_ < TXT_SCAN_LIMIT ? fileSize_ : TXT_SCAN_LIMIT;
  uint32_t pos = 0;
  uint32_t lineStart = 0;   // 当前行第一个字节的文件偏移
  uint32_t lineCount = 0;   // 已经数过的行数(用来估算页号)
  bool lineHasContent = false;
  char line[64];
  int lineLen = 0;

  while (pos < total) {
    if (abortFn_ && abortFn_(abortCtx_)) {
      break;
    }

    size_t got;
    {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (!g.ok()) {
        break;
      }
      if (!file_.seek(pos)) {
        break;
      }
      got = file_.read(blk, blkSize);
    }
    if (got == 0) {
      break;
    }

    for (size_t i = 0; i < got; i++) {
      const uint8_t c = blk[i];
      const uint32_t absPos = pos + i;

      if (c == '\n' || c == '\r') {
        if (c == '\r') {
          continue;  // 交给 '\n' 处理, 避免 CRLF 被数成两行
        }
        if (lineHasContent && lineLen > 0) {
          line[lineLen] = 0;
          if (looksLikeChapterTitle(line)) {
            if (chapterCount_ < TXT_MAX_CHAPTERS) {
              Chapter &ch = chapters_[chapterCount_];
              strncpy(ch.title, line, sizeof(ch.title) - 1);
              ch.title[sizeof(ch.title) - 1] = 0;
              ch.offset = lineStart;
              ch.pageHint = lineCount / ROWS;  // 粗略估算, 仅供 UI 参考
              chapterCount_++;
            }
          }
        }
        lineCount++;
        lineLen = 0;
        lineHasContent = false;
        lineStart = absPos + 1;
      } else {
        if (!lineHasContent) {
          lineStart = absPos;
          lineHasContent = true;
        }
        if (lineLen < (int)sizeof(line) - 1) {
          line[lineLen++] = (char)c;
        }
      }
    }

    pos += got;
    if (scanFn_) {
      scanFn_(scanCtx_, pos, total);
    }
  }

  free(blk);
  // 上机判定行: 这一次扫描花了多少 ms、扫了多少字节。
  // 判定标准(13.txt = 382KB):
  //   约 80 秒  -> 库按 512B 粒度循环, 每个扇区一次调用(老行为), 说明 64KB 块没走多扇区路径
  //   约 10 秒  -> 调用次数按块大小成比例下降, 但仍是单扇区路径
  //   1 秒以内  -> 真的走了 sdReadSectors 多扇区路径 ★ 这就是我们要的结果
  BOOTLOG("[scan] 章节扫描完成: %lu 字节 %lu 行, 块 %uKB, 耗时 %lu ms", (unsigned long)pos,
          (unsigned long)lineCount, (unsigned)(blkSize / 1024), (unsigned long)(millis() - scanT0));
  // ★ 这里算出来的 pageCount_ 只是**估算值**(按 3 字节/字估, 对全中文文本会明显偏小),
  //   所以绝不能把 pageCountKnown_ 置成 true —— 否则 loadPage() 开头的
  //   `if (pageCountKnown_ && page >= pageCount_)` 会把估算页数之后的**真实页面**
  //   全部当成越界返回 0: 13.txt 估算 1289 页而实际 2305 页, 用户翻到一半就再也翻不动。
  //   只有 loadPage() 真的排到文件末尾时, pageCount_ 才是精确值, 那时才置 true。
  pageCountKnown_ = false;
  pageCount_ = estimatePageCount();
}

// ---------------------------------------------------------------------------
// 页操作
// ---------------------------------------------------------------------------
int TxtReader::loadPage(uint32_t page) {
  // ★ 入口无条件打印一次状态 ★
  //   之前只在"提前返回"的分支加日志, 结果"第一页 0 行"时一条都没打印 ——
  //   因为走的是 `if (page == page_) return linesInPage_;` 这条**没有日志**的路径:
  //   open() 已经把 page_ 置 0、linesInPage_ 也还是 0, 于是第一次 loadPage(0)
  //   静默返回 0 行 —— 这就是"书打不开、屏幕全白"的根因。
  //   现在: 该快捷返回必须同时满足 pageValid_(真的排过版), 并且它也会打日志。
  {
    static uint32_t dbgCount = 0;
    if (dbgCount < 6) {
      dbgCount++;
      BOOTLOG("[pg] loadPage(%lu) 进入: file=%d pageLines=%p page_=%lu pageValid=%d linesInPage_=%d "
              "pageCountKnown=%d pageCount_=%lu fileSize=%lu",
              (unsigned long)page, file_ ? 1 : 0, (void *)pageLines_, (unsigned long)page_,
              pageValid_ ? 1 : 0, linesInPage_, pageCountKnown_ ? 1 : 0, (unsigned long)pageCount_,
              (unsigned long)fileSize_);
    }
  }

  if (!file_ || !pageLines_) {
    BOOTLOG("[pg] loadPage(%lu): 提前返回 -> file=%d pageLines=%p (分配失败?)", (unsigned long)page,
            file_ ? 1 : 0, (void *)pageLines_);
    return 0;
  }
  if (pageCountKnown_ && page >= pageCount_) {
    BOOTLOG("[pg] loadPage(%lu): 提前返回 -> pageCountKnown=1 且 pageCount=%lu", (unsigned long)page,
            (unsigned long)pageCount_);
    return 0;
  }

  uint32_t from = 0;
  uint32_t off = 0;

  if (page == page_ && pageValid_) {
    // ★ 只有"这一页真的排过版"才能复用 pageLines_; 否则 (pageValid_ == false)
    //   必须继续往下走, 真的去 layoutPageAt() 排一页。
    BOOTLOG("[pg] loadPage(%lu): 命中当前页缓存, 返回 %d 行", (unsigned long)page, linesInPage_);
    return linesInPage_;  // 已经在这一页
  }
  if (page == page_ + 1 && pageValid_ && pageEnd_ > 0) {
    // 最常见的顺序向后翻页: 直接用上一页算出来的下一页偏移
    // ★ from 的语义是"第一个还没排过的页号", 而 pageEnd_ 已经是第 page 页的起点,
    //   所以必须从 page_ + 1 开始。写成 from = page_ 会让下面的 while 先把第 page
    //   页排一遍, 末尾那次 layoutPageAt() 又往后多排一页 —— 于是 loadPage(1) 显示的
    //   是第 2 页, 每翻一页跳一页。
    from = page_ + 1;
    off = pageEnd_;
  } else if (page > 0 && indexFindFloor(page, &off)) {
    // 索引表里最接近且不超过目标页的锚点
    from = 0;
    for (int i = 0; i < indexCount_; i++) {
      if (index_[i].offset == off) {
        from = index_[i].page;
        break;
      }
    }
  } else {
    from = 0;
    off = 0;
    if (page < page_) {
      indexReset();  // 往回跳: 索引里大于目标页的锚点已经没用了
    }
  }

  // 从锚点逐页前进
  int guard = 0;
  while (from < page) {
    const int n = layoutPageAt(off);
    indexPut(from, off);
    if (n <= 0) {
      // 到文件末尾了: 把真实总页数记下来
      pageCountKnown_ = true;
      pageCount_ = from;
      return 0;
    }
    off = pageEnd_;
    from++;
    if (++guard > 4096) {
      break;  // 保险丝, 防止索引失效导致长时间卡住
    }
  }

  const int n = layoutPageAt(off);
  if (n > 0) {
    page_ = page;
    pageValid_ = true;  // ★ 内容与页号成对成立, 之后 loadPage(page) 才能安全命中缓存
    indexPut(page, off);
  }
  return n;
}

// 从指定字节偏移排一页; 成功后更新 pageStart_/pageEnd_/linesInPage_
int TxtReader::layoutPageAt(uint32_t offset) {
  if (!file_) {
    return 0;
  }
  // ★ 本函数会重写 pageLines_/linesInPage_/pageStart_/pageEnd_, 所以从这一刻起
  //   "pageLines_ 里装的是第 page_ 页"这个前提就不再成立了 —— 先失效, 由成功的
  //   调用方 (loadPage / jumpToChapter) 在拿到 n > 0 后重新置为有效。
  //   这样"排版失败返回 0"也不会留下一个自称有效的当前页。
  pageValid_ = false;
  if (offset >= fileSize_) {
    linesInPage_ = 0;
    pageStart_ = pageEnd_ = fileSize_;
    return 0;
  }

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    snprintf(err_, sizeof(err_), "分页时等 SD 锁超时");
    return 0;
  }
  if (!file_.seek(offset)) {
    BOOTLOG("[pg] !! seek(%lu) 失败", (unsigned long)offset);
    return 0;
  }
  BOOTLOG("[pg] layoutPageAt(offset=%lu): fileSize=%lu, seek 后 position=%lu", (unsigned long)offset,
          (unsigned long)fileSize_, (unsigned long)file_.position());

  Paginator pg(COLS, ROWS);
  // ★ 不走读取回调, 直接读文件 ★
  //   layoutPageAt() 这里已经持有 sd_mutex 了, 而 readerRead 回调又会在每次读块时
  //   再拿一次锁 —— 纯属多余的一层, 还多一个可能出错的环节。
  //   Paginator::fill() 在没有回调时直接走 file.read(), 路径更短更好排查。
  const int lines = pg.paginate(file_, pageLines_, ROWS, TXT_LINE_MAX, gbk_);
  BOOTLOG("[pg] paginate 返回 %d 行, position=%lu, unconsumed=%lu, trailingEnd=%lu", lines,
          (unsigned long)file_.position(), (unsigned long)pg.unconsumed(),
          (unsigned long)pg.trailingEndOffset(file_));
  if (lines > 0) {
    BOOTLOG("[pg] 首行=[%s]", pageLines_);
  }

  pageStart_ = offset;
  // 关键: 用 trailingEndOffset() 而不是 file.position() - unconsumed()。
  // 后者会把"悬挂标点待退回的字节"也当成已排版, 导致下一页重复输出那个标点。
  pageEnd_ = pg.trailingEndOffset(file_);
  if (pageEnd_ <= pageStart_) {
    pageEnd_ = pageStart_ + 1;  // 防止死循环
  }

  linesInPage_ = lines;
  // 统计本页纯文本字节数: 用来区分"没读到字"和"读到了但画不出来"
  // pageLines_ 是 ROWS * TXT_LINE_MAX 的平铺字符块, 每行占 TXT_LINE_MAX
  pageTextBytes_ = 0;
  for (int i = 0; i < lines; i++) {
    pageTextBytes_ += (uint32_t)strlen(pageLines_ + (size_t)i * TXT_LINE_MAX);
  }
  return lines;
}

void TxtReader::indexReset() {
  indexCount_ = 0;
}

void TxtReader::indexPut(uint32_t page, uint32_t offset) {
  if (!index_) {
    return;
  }
  // 索引是稀疏的: 只记录 PAGE_INDEX_STEP 整数倍页 (以及第 0 页)
  if (page != 0 && (page % PAGE_INDEX_STEP) != 0) {
    return;
  }
  for (int i = 0; i < indexCount_; i++) {
    if (index_[i].page == page) {
      index_[i].offset = offset;
      return;
    }
    if (index_[i].page > page) {
      if (indexCount_ >= INDEX_MAX) {
        return;
      }
      memmove(&index_[i + 1], &index_[i], sizeof(PageIndexEntry) * (indexCount_ - i));
      index_[i].page = page;
      index_[i].offset = offset;
      indexCount_++;
      return;
    }
  }
  if (indexCount_ < INDEX_MAX) {
    index_[indexCount_].page = page;
    index_[indexCount_].offset = offset;
    indexCount_++;
  }
}

bool TxtReader::indexFindFloor(uint32_t page, uint32_t *offset) const {
  if (!index_ || indexCount_ == 0) {
    return false;
  }
  int best = -1;
  for (int i = 0; i < indexCount_; i++) {
    if (index_[i].page <= page) {
      best = i;
    } else {
      break;
    }
  }
  if (best < 0) {
    return false;
  }
  *offset = index_[best].offset;
  return true;
}

const char *TxtReader::line(int line) const {
  if (!pageLines_ || line < 0 || line >= linesInPage_) {
    return "";
  }
  return pageLines_ + (size_t)line * TXT_LINE_MAX;
}

uint8_t TxtReader::percent() const {
  if (fileSize_ == 0) {
    return 0;
  }
  const uint32_t p = (uint32_t)((uint64_t)pageStart_ * 100 / fileSize_);
  return (uint8_t)(p > 100 ? 100 : p);
}

int TxtReader::currentChapter() const {
  int best = -1;
  for (int i = 0; i < chapterCount_; i++) {
    if (chapters_[i].offset <= pageStart_) {
      best = i;
    } else {
      break;
    }
  }
  return best;
}

bool TxtReader::jumpToChapter(int idx, uint32_t *pageOut) {
  if (idx < 0 || idx >= chapterCount_) {
    return false;
  }
  // 从这一章的字节偏移开始排一页, 页面内容就从章节标题开始
  const int n = layoutPageAt(chapters_[idx].offset);
  if (n <= 0) {
    return false;
  }
  page_ = chapters_[idx].pageHint;  // 估算页号, 仅供显示
  pageValid_ = true;                // ★ 上面 layoutPageAt 刚排好一页, 内容与 page_ 成对
  indexReset();
  indexPut(page_, chapters_[idx].offset);
  if (pageOut) {
    *pageOut = page_;
  }
  return true;
}
