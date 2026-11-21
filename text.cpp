/*
 * text.cpp — UTF-8/GBK 解码 + 中文自动折行实现
 *
 * 关于 GBK: arduino-esp32 3.x 的核心已经去掉了 GBK<->Unicode 转换表(2.0.x 有
 * _GBK2UNICODE)。本项目不内嵌约 4 万字节的表, 所以 decodeGbk() 遇到双字节 GBK
 * 字符时无法还原成 Unicode。=> TXT 必须是 UTF-8。
 *
 * 【排版算法要点】
 * 一行只多解码 1 个字符(cols+1), 用来判断"断在哪里"。多出来的字符按字节回退到
 * 输入缓冲区, 这样换行符、空格都不会被吞掉, 空行也能被正确当成一行输出。
 */
#include "text.h"
#include "config.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------
Decoded decodeUtf8(const uint8_t *s, size_t n) {
  Decoded d = {0, 0};
  if (n == 0) {
    return d;
  }
  const uint8_t b = s[0];

  if (b < 0x80) {
    d.cp = b;
    d.len = 1;
    return d;
  }

  uint8_t need;
  uint32_t cp;
  if ((b & 0xE0) == 0xC0) {
    need = 2;
    cp = b & 0x1F;
  } else if ((b & 0xF0) == 0xE0) {
    need = 3;
    cp = b & 0x0F;
  } else if ((b & 0xF8) == 0xF0) {
    need = 4;
    cp = b & 0x07;
  } else {
    // 0x80~0xBF 是孤立续字节, 0xF8~0xFF 非法 -> 当坏字节跳过
    d.cp = 0xFFFD;
    d.len = 1;
    return d;
  }

  if (n < need) {
    d.len = need;  // 数据不足, 告诉调用方还差几个字节
    return d;
  }

  for (uint8_t i = 1; i < need; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      d.cp = 0xFFFD;  // 非法续字节
      d.len = 1;
      return d;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  d.cp = cp;
  d.len = need;
  return d;
}

Decoded decodeGbk(const uint8_t *s, size_t n) {
  Decoded d = {0, 0};
  if (n == 0) {
    return d;
  }
  const uint8_t b = s[0];
  if (b < 0x80) {
    d.cp = b;
    d.len = 1;
    return d;
  }
  if (n < 2) {
    d.len = 2;
    return d;
  }
  // 双字节 GBK 没有转换表无法还原成 Unicode
  d.cp = 0xFFFD;
  d.len = 2;
  return d;
}

size_t utf8BomLen(const uint8_t *s, size_t n) {
  if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
    return 3;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 字符属性
// ---------------------------------------------------------------------------
bool isCjk(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF)      // 部首扩展 / 康熙部首 / CJK 统一表意
         || (cp >= 0xF900 && cp <= 0xFAFF)   // 兼容汉字
         || (cp >= 0x3000 && cp <= 0x303F)   // CJK 标点
         || (cp >= 0xFF00 && cp <= 0xFFEF);  // 全角字符
}

bool isNoLineStart(uint32_t cp) {
  switch (cp) {
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0x3001:  // 、
    case 0xFF09:  // ）
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x2026:  // …
    case 0x201D:  // ”
    case 0x2019:  // ’
    case 0x00B7:  // ·
    case 0x30FB:  // ・
      return true;
    default:
      return false;
  }
}

static bool isNoLineEnd(uint32_t cp) {
  switch (cp) {
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x201C:  // “
    case 0x2018:  // ‘
      return true;
    default:
      return false;
  }
}

static inline bool isLatinWord(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '\'' || cp == '-' ||
         cp == '_';
}

bool canBreakAfter(uint32_t cp) {
  if (cp == 0xFFFD || isNoLineEnd(cp)) {
    return false;
  }
  if (cp == 0x3000) {
    return false;  // 全角空格是段首缩进, 必须跟后面的字粘在一起
  }
  return !isLatinWord(cp);  // 拉丁词中间不断
}

bool canBreakBefore(uint32_t cp) {
  if (cp == 0xFFFD || isNoLineStart(cp)) {
    return false;
  }
  if (cp == 0x3000) {
    return false;  // 不允许全角空格出现在行首(缩进只在段首有意义)
  }
  if (isCjk(cp)) {
    return true;
  }
  if (cp < 0x80) {
    return cp == ' ';  // ASCII 中只有空格允许做行首(西文靠空格断词)
  }
  return true;
}

// ---------------------------------------------------------------------------
// Paginator
// ---------------------------------------------------------------------------
uint32_t Paginator::absUnreadPos(File &file) const {
  // 缓冲区里第一个未消费字节的绝对位置 = 文件读写指针 - 缓冲区剩余未消费字节
  const uint32_t pos = (uint32_t)file.position();
  const size_t unread = bufLen_ - bufPos_;
  return (pos >= unread) ? (pos - unread) : 0;
}

void Paginator::rewindBy(File &file, size_t rewind) {
  if (rewind == 0) {
    return;
  }
  // 本行第一个字节的绝对位置
  const uint32_t lineStart = (lineMark_.pos >= lineMark_.unread) ? (lineMark_.pos - lineMark_.unread) : 0;
  const uint32_t cur = absUnreadPos(file);
  if (cur <= lineStart) {
    return;
  }
  uint32_t target = cur - (uint32_t)rewind;
  if (target < lineStart) {
    target = lineStart;
  }

  // 缓冲区里现存数据的绝对起点
  const uint32_t bufDataStart = (uint32_t)file.position() - (uint32_t)bufLen_;
  if (target >= bufDataStart) {
    // 目标还在缓冲区里, 直接移动读指针
    bufPos_ = (size_t)(target - bufDataStart);
    return;
  }

  // 目标已经被 fill() 挤出去了: 重新 seek 到目标位置并丢弃缓冲区
  file.seek(target);
  bufLen_ = 0;
  bufPos_ = 0;
}

size_t Paginator::fill(File &file, size_t need) {
  // 把未消费的数据挪到缓冲区头部
  if (bufPos_ > 0) {
    if (bufPos_ < bufLen_) {
      memmove(buf_, buf_ + bufPos_, bufLen_ - bufPos_);
      bufLen_ -= bufPos_;
    } else {
      bufLen_ = 0;
    }
    // 被挤出去的这些字节是"本行已经消费掉"的, 记下来供回退时换算绝对位置
    lineExtra_ += bufPos_;
    bufPos_ = 0;
  }

  while ((bufLen_ - bufPos_) < need && bufLen_ < sizeof(buf_)) {
    const size_t space = sizeof(buf_) - bufLen_;
    size_t got;
    if (readFn_) {
      got = readFn_(readCtx_, buf_ + bufLen_, space);
    } else {
      got = file.read(buf_ + bufLen_, space);
    }
    if (got == 0) {
      break;
    }
    bufLen_ += got;
  }
  return bufLen_ - bufPos_;
}

int Paginator::layoutLine(File &file, char *line, int maxChars, bool gbk) {
  // 记下本行起点的绝对位置, 供回退时换算(不能用 bufPos_, 原因见 text.h 注释)
  lineExtra_ = 0;
  lineMark_.pos = (uint32_t)file.position();
  lineMark_.unread = bufLen_ - bufPos_;

  if (skipLeadingSpaces_) {
    // 上一行是"软换行"(在空格处自动折行)留下的行首空格, 丢掉
    skipLeadingSpaces_ = false;
    for (;;) {
      const size_t avail = fill(file, 4);
      if (avail == 0) {
        break;
      }
      const uint8_t *p = buf_ + bufPos_;
      const Decoded d = decodeUtf8(p, avail);
      if (d.cp == ' ' || d.cp == 0x3000) {
        bufPos_ += d.len;
        continue;
      }
      break;
    }
    // 跳过的空格也算本行开头, 更新标记
    lineMark_.pos = (uint32_t)file.position();
    lineMark_.unread = bufLen_ - bufPos_;
    lineExtra_ = 0;
  }

  // 只需要 cols+1 个字符就能决定断点(多出来的一个用于"标点避头尾"判断)
  const int want = cols_ + 1;
  uint32_t cps[COLS + 2];
  uint8_t charLen[COLS + 2];
  int n = 0;
  bool hitNewline = false;

  // ★ 重要: 只有"真正作为一个字符收进 cps[]"时才推进 bufPos_ 并记录 charLen。
  // 换行符/控制字符/坏字节推进了 bufPos_ 却不增加 n, 如果按 n 去回退就会多退,
  // 导致下一行开头少字。
  while (n < want) {
    size_t avail = fill(file, 4);
    if (avail == 0) {
      break;  // 文件结束
    }
    const uint8_t *p = buf_ + bufPos_;
    Decoded d = gbk ? decodeGbk(p, avail) : decodeUtf8(p, avail);

    if (d.cp == 0 && d.len > 0) {
      // 多字节字符被缓冲区边界切断, 再读一次
      avail = fill(file, d.len);
      if (avail < d.len) {
        break;  // 文件真的结束了
      }
      p = buf_ + bufPos_;
      d = gbk ? decodeGbk(p, avail) : decodeUtf8(p, avail);
    }

    if (d.cp == '\n') {
      bufPos_ += d.len;  // 消费换行符, 它不算一个字符
      hitNewline = true;
      break;
    }
    if (d.cp == '\r') {
      bufPos_ += d.len;
      continue;
    }
    if (d.cp == 0xFFFD) {
      bufPos_ += d.len;  // 坏字节 / 无法解码的 GBK 双字节, 丢弃
      continue;
    }
    if (d.cp == '\t') {
      cps[n] = ' ';
      charLen[n] = (uint8_t)d.len;
      bufPos_ += d.len;
      n++;
      continue;
    }
    // 图片页标记(\f<索引>\f): 当普通字符收下, 这样它会单独占一行传给界面
    if (d.cp == TEXT_MARKER_CHAR) {
      cps[n] = d.cp;
      charLen[n] = (uint8_t)d.len;
      bufPos_ += d.len;
      n++;
      continue;
    }
    if (d.cp < 0x20) {
      bufPos_ += d.len;  // 其它不可见控制字符直接丢掉
      continue;
    }

    cps[n] = d.cp;
    charLen[n] = (uint8_t)d.len;
    bufPos_ += d.len;
    n++;
  }

  if (n == 0) {
    if (hitNewline) {
      line[0] = '\0';
      return 0;  // 空行也算一行
    }
    return -1;  // 文件结束
  }

  // 图片标记 "@<索引>@": 让它单独占一行(把标记之后的字符全部退回)
  if (cps[0] == (uint32_t)TEXT_MARKER_CHAR) {
    int takeM = 1;
    while (takeM < n && cps[takeM] != (uint32_t)TEXT_MARKER_CHAR) {
      takeM++;
    }
    if (takeM < n) {
      takeM++;  // 连结尾的标记一起收
    }
    int outM = 0;
    for (int i = 0; i < takeM && outM < maxChars - 1; i++) {
      line[outM++] = (char)cps[i];  // 标记字符与索引都是 ASCII, 直接写
    }
    line[outM] = 0;
    size_t rewind = 0;
    for (int i = takeM; i < n; i++) {
      rewind += charLen[i];
    }
    rewindBy(file, rewind);
    return takeM;
  }

  // ---- 决定断点 ----
  // k = 断点位置, 语义是"这一行收 cps[0..k-1], 剩下的都退回给下一行"。
  // 判定条件对应: cps[k-1] 作为行尾合适 且 cps[k] 作为行首合适。
  int take = n;
  if (n > cols_) {
    int k = -1;
    for (int i = cols_; i >= 1; i--) {
      if (canBreakAfter(cps[i - 1]) && canBreakBefore(cps[i])) {
        k = i;
        break;
      }
    }
    if (k > 0) {
      take = k;
      // 如果断点前面紧跟着一个空格(西文在空格后断行), 把这个空格也退回下一行,
      // 并标记下一行跳过行首空格 —— 否则行尾会多一个看不见的空格。
      if (cps[take - 1] == ' ') {
        take--;
        k = take;
        skipLeadingSpaces_ = true;
      }

      // ---- 标点悬挂(中文排版的关键) ----
      // 形如 "。" "，" "）" 的禁则标点不能出现在行首。如果按上面的断点它恰好会被
      // 推到下一行行首, 就把它留在本行行尾。悬挂的字节写进了本行, 但必须累加到
      // hangBytes_ 里退还给下一行。
      int hung = 0;
      while (k < n && hung < 2 && isNoLineStart(cps[k])) {
        take++;
        hangBytes_ += charLen[k];
        k++;
        hung++;
      }
    } else {
      // 整行找不到合法断点(例如一整行全是连续省略号这类禁则标点串):
      // 在列宽处硬断。此时**不做悬挂**(硬断已占满整行, 再悬挂会溢出行宽),
      // 但如果下一行行首正好是禁则标点, 就少收一个字 —— 让那个标点留在本行行尾,
      // 避免"标点出现在行首"。
      take = cols_;
      if (take > 1 && take < n && isNoLineStart(cps[take])) {
        take--;
      }
    }
  }

  // ---- 把 take 个字符写成 UTF-8 ----
  int out = 0;
  for (int i = 0; i < take && out < maxChars - 4; i++) {
    const uint32_t cp = cps[i];
    if (cp < 0x80) {
      line[out++] = (char)cp;
    } else if (cp < 0x800) {
      line[out++] = (char)(0xC0 | (cp >> 6));
      line[out++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      line[out++] = (char)(0xE0 | (cp >> 12));
      line[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      line[out++] = (char)(0x80 | (cp & 0x3F));
    } else {
      line[out++] = (char)(0xF0 | (cp >> 18));
      line[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      line[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      line[out++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  line[out] = '\0';

  // ---- 把多解码的字符退回缓冲区 ----
  // 两种情况需要回退:
  //   1) 上一行悬挂的标点字节(记在 hangBytes_ 里), 必须还给本行开头
  //   2) 本行多解码的字符(缓冲里 take 之后的), 按字节退回
  // 回退量算完必须把 hangBytes_ 清零, 否则会"泄漏"到下一行的回退量里, 导致重复输出。
  if (take < n || hangBytes_ > 0) {
    size_t rewind = hangBytes_;
    for (int i = take; i < n; i++) {
      rewind += charLen[i];
    }
    hangBytes_ = 0;
    rewindBy(file, rewind);
  }

  return take;
}

int Paginator::paginate(File &file, char *out, int maxLines, int maxChars, bool gbk) {
  int lines = 0;
  while (lines < maxLines) {
    char *dst = out + (size_t)lines * maxChars;
    const int got = layoutLine(file, dst, maxChars, gbk);
    if (got < 0) {
      return lines;  // 文件结束
    }

    // ★ 插图标记独占一页 ★ (2026-10-03 修)
    //
    // 症状: 一本漫画 epub 前面有 21 张插画, 设备上却只显示前 3 张;
    //       日志里同一页出现了 @0@ @1@ 两个标记, 而界面**一页只画一张图** ——
    //       于是同一页里第 2 个之后的标记全被丢弃。
    //
    // 修法: 读到标记行就**立刻结束本页**, 让每个标记独占一页。
    //       这样"一页一张图"的界面语义与缓存结构一致, 一张都不会丢。
    if (dst[0] == (char)TEXT_MARKER_CHAR) {
      lines++;
      return lines;
    }

    lines++;  // got == 0 表示空行, 同样占一行
  }
  return lines;
}
