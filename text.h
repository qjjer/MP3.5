/*
 * text.h — 文本解码与中文排版
 *
 * 只做三件与硬件无关的事, 方便单独测试:
 *   1. 把 UTF-8 / GBK 字节解码成 Unicode 码点
 *   2. 按屏幕列数自动折行(中英混排 + 标点避头尾)
 *   3. 分页: 从文件任意位置起, 连续吐出 N 行文本
 *
 * 注意: 本模块只管定位与切分, 不碰 SD 卡 —— 读文件由调用方在锁内完成。
 */
#pragma once

#include <Arduino.h>
#include <FS.h>

// 一次排版最多处理的字符数(一行 9 个汉字 + 安全余量, 英文换行时更宽)
#define TEXT_LINE_MAX 192

// 图片页标记字符 '@'。EPUB 正文里的插图会被替换成 "@<十进制索引>@" 这样一整行,
// 分页器让它单独占一行, 阅读界面看到这种行就改成渲染图片。
// 为什么用 '@' 而不是 \f: 文本整理阶段会把行首的 \f 当空白去掉, 标记会被拆散。
#define TEXT_MARKER_CHAR '@' 

// 解码结果
struct Decoded {
  uint32_t cp;  // Unicode 码点; 0 表示到达缓冲区末尾(需要更多数据)
  uint8_t len;  // 本字符占用的字节数
};

// 解码一个字符。返回 cp=0 且 len>0 时表示"数据不足, 需要再读 len 个字节"。
Decoded decodeUtf8(const uint8_t *s, size_t n);
Decoded decodeGbk(const uint8_t *s, size_t n);

// UTF-8 BOM 检测: 是 BOM 返回 3, 否则返回 0
size_t utf8BomLen(const uint8_t *s, size_t n);

// ---------------------------------------------------------------------------
// 字符属性与换行判定
// ---------------------------------------------------------------------------
bool isCjk(uint32_t cp);            // 汉字/日韩文字(全角, 占一格)
bool canBreakAfter(uint32_t cp);    // 可以作为行尾的最后一个字符(英文/数字)
bool canBreakBefore(uint32_t cp);   // 可以作为行首的第一个字符
bool isNoLineStart(uint32_t cp);    // 避头尾: 不能出现在行首(。，）」等)

// ---------------------------------------------------------------------------
// 分页器
// ---------------------------------------------------------------------------
class Paginator {
public:
  // cols: 每行字符数(汉字按 1 计), rows: 每页行数
  Paginator(uint8_t cols, uint8_t rows) : cols_(cols), rows_(rows) {
    // 实例是每页新建的, 这里清掉可能残留的悬挂字节计数, 保证 trailingEndOffset()
    // 在"一行都没排"时就是准确的页面起点。
    hangBytes_ = 0;
  }

  // 从 file 的当前位置开始排一页。
  //   out        : 输出缓冲, 每行一个以 '\0' 结尾的字符串
  //   maxLines   : out 能放几行(通常 = rows)
  //   maxChars   : out 每个字符串缓冲的容量(必须 >= TEXT_LINE_MAX)
  // 返回实际排版出的行数(0 = 文件结束)。
  // 结束后 file 的位置正好停在"尚未被排入的字节"处, 供下一页继续。
  int paginate(File &file, char *out, int maxLines, int maxChars, bool gbk);

  // 把上面 paginate 读文件需要拿总线锁, 这里允许注入一个"读数据"回调,
  // 便于在锁内一次性读块(默认实现直接调 file.read)。
  typedef size_t (*ReadFn)(void *ctx, uint8_t *buf, size_t len);
  void setReader(ReadFn fn, void *ctx) {
    readFn_ = fn;
    readCtx_ = ctx;
  }

  // 缓冲区里已经读进来但还没排进页面的字节数。
  // 注意: 这里**不包含**悬挂标点待退回的字节, 所以调用方不要直接用
  // file.position() - unconsumed() 当"下一页起点", 请用 trailingEndOffset()。
  size_t unconsumed() const {
    return bufLen_ - bufPos_;
  }

  // 真正的"排版结果结束位置": 已经排出去的最后一个字符之后的文件偏移。
  // = file.position() - unconsumed() - 待退回的悬挂标点字节
  //
  // 为什么需要这个: 悬挂标点虽然写进了本页最后一行, 但它的字节已经退还给下一行。
  // 如果调用方按 file.position() - unconsumed() 算下一页起点, 就会把这个标点
  // 也算成"已排版", 于是下一页从标点之后开始, 该标点被重复输出一次。
  uint32_t trailingEndOffset(File &file) const {
    const uint32_t pos = (uint32_t)file.position();
    const size_t pending = (bufLen_ - bufPos_) + hangBytes_;
    return (pos >= pending) ? (pos - pending) : 0;
  }

private:
  uint8_t cols_;
  uint8_t rows_;
  ReadFn readFn_ = nullptr;
  void *readCtx_ = nullptr;

  // 内部字节缓冲: 避免逐字节读 SD
  uint8_t buf_[512];
  size_t bufLen_ = 0;
  size_t bufPos_ = 0;
  bool skipLeadingSpaces_ = false;  // 上一行在空格处软换行 -> 本行行首空格丢掉
  size_t hangBytes_ = 0;            // 上一行悬挂标点占的字节, 必须退还给下一行

  // 回退用的绝对位置追踪。
  // 为什么不能直接用 bufPos_: fill() 每次都会把"已消费"的字节从缓冲区里挤出去
  // 并把 bufPos_ 归零, 所以排版一行期间若跨了多次 fill(), bufPos_ 就不再代表
  // "这一行消费了多少字节"。用文件绝对位置才可靠。
  struct FileMark {
    uint32_t pos;    // file 的读写指针
    size_t unread;   // 缓冲区里还没消费的字节数
  };
  FileMark lineMark_ = {0, 0};  // 本行第一个字节的绝对位置
  size_t lineExtra_ = 0;        // 本行期间被 fill() 挤出去的消费字节数

  size_t fill(File &file, size_t need);  // 尽量保证缓冲区里至少有 need 字节
  // 排一行: 把结果写进 line(容量 maxChars), 返回写入的字符数, 0 = 文件结束
  int layoutLine(File &file, char *line, int maxChars, bool gbk);
  // 缓冲区里第一个未消费字节的绝对文件位置
  uint32_t absUnreadPos(File &file) const;
  // 从缓冲区里退掉 rewind 个字节(调用方保证不越过本行起点)
  void rewindBy(File &file, size_t rewind);
};
