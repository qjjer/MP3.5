/*
 * epub_parser.cpp - EPUB 解析实现
 *
 * ZIP 结构要点(只做读, 不做写):
 *   末尾 EOCD(0x06054b50) -> 中央目录偏移/条目数
 *   中央目录每条(0x02014b50) -> 名字/压缩方式/压缩前后大小/本地头偏移
 *   本地头(0x04034b50) -> 名字长度+extra 长度, 数据紧随其后
 * 压缩方式: 0 = 原样存储, 8 = deflate(用 ROM 里的 tinfl_decompress 解)
 */
#include "epub_parser.h"
#include "config.h"
#include "sd_mutex.h"

#include <string.h>
// ROM 里的 miniz(见与源码同目录的 miniz.h / esp_rom_caps.h / esp_rom_crc.h)。
// 这三个是从 ESP32-S3 的 esp_rom/include 复制过来的原版头文件, 放进 sketch 目录
// 是为了让编译器能找到它们(无需修改任何编译配置), 不是自己改写的代码。
#include "miniz.h"

// ---------------------------------------------------------------------------
// ★ 诊断必须"永远输出" ★
// 平台的 build.extra_flags 带的是 -DCORE_DEBUG_LEVEL=0, log_e/log_i **默认什么都
// 不打印**: 插图导出失败时串口一片安静, 只能看到"插图页显示不出来"。
// 一次性/启动路径上的诊断一律走 Serial。
// ---------------------------------------------------------------------------
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                       \
  do {                                          \
    Serial.printf(fmt "\n", ##__VA_ARGS__);     \
    Serial.flush();                             \
  } while (0)
#endif

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
uint32_t EpubParser::hashName(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h;
}

static void *psAlloc(size_t n) {
  void *p = ps_malloc(n);
  return p ? p : malloc(n);
}

// ---------------------------------------------------------------------------
// 提取正文用的临时缓冲大小。
// ★ 必须被"提取正文"和"只扫图片清单"两处共用 ★
//   它决定单个章节在 markImagesInXhtml() 里被截断的位置(那边循环条件里有
//   o + 24 < outCap) —— 两处给的大小不一样, 图片登记的范围就会不一样,
//   于是缓存里的 @N@ 和实际导出的图片错位/丢图。
// ---------------------------------------------------------------------------
static const uint32_t kEpubTextCap = 512 * 1024;

// spine 项是不是 XHTML 正文。
// ★ 也必须共用 ★: extractTextTo() 只对 XHTML 调 markImagesInXhtml()(也就是只对
//   XHTML 登记图片), 扫清单时必须用完全一样的判断, 否则编号会错位。
static bool isXhtmlPath(const char *p) {
  const size_t n = strlen(p);
  return (n >= 6 && strcasecmp(p + n - 6, ".xhtml") == 0) || (n >= 5 && strcasecmp(p + n - 5, ".html") == 0) ||
         (n >= 4 && strcasecmp(p + n - 4, ".htm") == 0);
}

static inline uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// XML 里的空白: 规范 S ::= #x20 | #x9 | #xD | #xA。
// ★ 必须包含 \n 和 \r ★: OPF/container.xml 常被格式化成
//     <item
//        id="x" href="y"/>
//   只认空格/制表符的话, 这种多行标签会被整条跳过。
static inline bool xmlIsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 标签名后面允许出现的字符(空白 / 标签结束 / 自闭合斜杠)。
// 用来把 "<rootfiles>" 和 "<rootfile>" 区分开。
static inline bool xmlIsNameEnd(char c) {
  return xmlIsSpace(c) || c == '>' || c == '/';
}

// 找标签真正的结束 '>'。属性值里可以合法地出现 '>'(例如 title="a>b"),
// 所以必须跳过引号内的内容; 找不到就退回"第一个 '>'"的老行为,
// 免得畸形输入把后面一大段正文全吃掉。
static const char *xmlFindTagEnd(const char *start, const char *end) {
  char quote = 0;
  for (const char *q = start; q < end; q++) {
    const char c = *q;
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return q;
    }
  }
  return (const char *)memchr(start, '>', end - start);
}

// 在 XML 文本里找 <tag ... attr="value" ...>，返回 value 起始指针并写入长度
//
// ★ 这里踩过一串坑, 每一个都会让"明明存在的属性"找不到或取错 ★
//
// 坑1: 标签名用 strncmp 前缀比较, 会把父节点当成目标节点。
//      container.xml 里是 <rootfiles><rootfile full-path=".."/></rootfiles>,
//      而 "rootfiles" 的前 8 个字符正是 "rootfile" —— 于是它匹配到**父节点
//      <rootfiles>**, gt 指向父节点的 '>', 然后在父节点内部找 full-path,
//      当然找不到。修法: 标签名后面必须紧跟空白/'>'/'/' 才算真正匹配。
//
// 坑2: 一旦匹配到标签就 return, 找不到属性时**不再继续扫描后面的标签**。
//      即使坑1修好了, 只要文档里先出现一个没有该属性的 <rootfile>, 也会
//      直接失败。修法: 属性没找到时继续往后找下一个同名标签。
//
// 坑3: end 参数是"搜索区间的末尾(不含)"。parseOpf() 以前把**标签结束符 '>' 的
//      位置**当 end 传进来, 于是下面这句 memchr 永远搜不到那个 '>' —— 属性
//      一律返回 nullptr, manifest/spine 全空, EPUB 直接打不开。修法: 先把
//      end 的语义统一成"不含", 调用方传 gt+1 (见 parseOpf)。
//
// 坑4: 只把 空格/制表 当分隔符 -> 多行标签失效; 属性名不做左边界检查 ->
//      "data-src" 里的 "src" 会被当成 src; 不跳过引号内的内容 ->
//      属性值里写着的假属性会把真属性顶掉。修法: xmlIsSpace + 左边界 + 引号状态。
//
// 注意 end 必须是"不含"边界: 本函数绝不读 end[0]。
static const char *xmlAttr(const char *start, const char *end, const char *tag, const char *attr, int *valLen) {
  const size_t tagLen = strlen(tag);
  const size_t aLen = strlen(attr);
  const char *p = start;
  while (p < end) {
    const char *lt = (const char *)memchr(p, '<', end - p);
    if (!lt) {
      return nullptr;
    }
    const char *q = lt + 1;
    // 标签名严格匹配: 名字后面必须紧跟分隔符, 否则 "<rootfiles>" 会被当成 "<rootfile"
    const bool nameOk = (q + tagLen < end) && (strncmp(q, tag, tagLen) == 0) && xmlIsNameEnd(q[tagLen]);
    if (!nameOk) {
      p = lt + 1;
      continue;   // 不是目标标签, 继续找下一个 '<'
    }

    const char *gt = xmlFindTagEnd(q, end);
    if (!gt) {
      return nullptr;   // 标签没闭合, 文件有问题
    }

    // 在这个标签内找 attr="；引号内的内容一律跳过(属性值里的假属性不算)
    const char *a = q;
    char inq = 0;
    while (a + aLen < gt) {
      const char c = *a;
      if (inq) {
        if (c == inq) {
          inq = 0;
        }
        a++;
        continue;
      }
      if (c == '"' || c == '\'') {
        inq = c;
        a++;
        continue;
      }
      if (strncmp(a, attr, aLen) == 0 && (a == q || xmlIsSpace(a[-1]))) {
        const char *eq = a + aLen;
        while (eq < gt && xmlIsSpace(*eq)) {
          eq++;
        }
        if (eq < gt && *eq == '=') {
          eq++;
          while (eq < gt && xmlIsSpace(*eq)) {
            eq++;
          }
          if (eq < gt && (*eq == '"' || *eq == '\'')) {
            const char quote = *eq++;
            const char *ve = (const char *)memchr(eq, quote, gt - eq);
            if (ve) {
              *valLen = (int)(ve - eq);
              return eq;
            }
          }
        }
      }
      a++;
    }

    // 本标签里没有该属性 —— 继续往后找下一个同名标签(不要在这里放弃)
    p = gt + 1;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
EpubParser::EpubParser() {}

void EpubParser::close() {
  if (file_) {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      file_.close();
    } else {
      file_.close();
    }
  }
  file_ = File();
  if (manifest_) {
    free(manifest_);
    manifest_ = nullptr;
  }
  if (spine_) {
    free(spine_);
    spine_ = nullptr;
  }
  if (images_) {
    free(images_);
    images_ = nullptr;
  }
  if (imageSpine_) {
    free(imageSpine_);
    imageSpine_ = nullptr;
  }
  imageCount_ = 0;
  manifestCount_ = 0;
  spineCount_ = 0;
  imagesScanned_ = false;   // 换书/重开时要能重新扫图片清单
  ready_ = false;
}

// 在中央目录里查一个条目
bool EpubParser::findEntry(const char *name, Entry *out) {
  const uint32_t fileSize = file_.size();
  if (fileSize < 22) {
    return false;
  }

  // 1) 末尾找 EOCD（注释最长 65535，我们从后往前扫 66KB）
  //    ★ 必须校验候选 EOCD ★: ZIP 注释里完全可能出现 PK\x05\x06 这四个字节,
  //      不校验的话会挑中注释里的假签名, 读出来的中央目录偏移是垃圾, 整本书打不开。
  //      校验方法: EOCD 自称的注释长度必须正好把文件收尾。
  uint8_t tail[1024];
  uint32_t scanLen = fileSize < 66560 ? fileSize : 66560;
  uint32_t eocdPos = 0;
  bool found = false;
  bool valid = false;
  for (uint32_t back = 0; back < scanLen;) {
    const uint32_t chunk = (scanLen - back) > sizeof(tail) ? sizeof(tail) : (scanLen - back);
    const uint32_t pos = fileSize - back - chunk;
    if (!file_.seek(pos) || file_.read(tail, chunk) != chunk) {
      return false;
    }
    for (int i = (int)chunk - 4; i >= 0; i--) {
      if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
        const uint32_t cand = pos + (uint32_t)i;
        uint8_t probe[22];
        bool candOk = false;
        if (file_.seek(cand) && file_.read(probe, 22) == 22) {
          const uint32_t commentLen = rd16(probe + 20);
          candOk = ((uint64_t)cand + 22u + commentLen == (uint64_t)fileSize);
        }
        if (!found) {
          eocdPos = cand;   // 兜底: 一个都不合法时沿用"最靠后"的那个
          found = true;
        }
        if (candOk) {
          eocdPos = cand;
          valid = true;
          break;
        }
      }
    }
    if (valid) {
      break;
    }
    back += chunk;
  }
  if (!found) {
    return false;
  }

  uint8_t e[22];
  if (!file_.seek(eocdPos) || file_.read(e, 22) != 22) {
    return false;
  }
  const uint16_t entryCount = rd16(e + 10);
  const uint32_t cdOffset = rd32(e + 16);

  // 2) 逐个读中央目录项
  if (!file_.seek(cdOffset)) {
    return false;
  }
  const size_t nameLen = strlen(name);
  uint8_t hdr[46];
  for (uint16_t i = 0; i < entryCount; i++) {
    if (file_.read(hdr, 46) != 46) {
      return false;
    }
    if (!(hdr[0] == 0x50 && hdr[1] == 0x4b && hdr[2] == 0x01 && hdr[3] == 0x02)) {
      return false;
    }
    const uint16_t nLen = rd16(hdr + 28);
    const uint16_t eLen = rd16(hdr + 30);
    const uint16_t cLen = rd16(hdr + 32);

    char nm[160];
    const uint16_t toRead = nLen < sizeof(nm) - 1 ? nLen : (uint16_t)(sizeof(nm) - 1);
    if (file_.read((uint8_t *)nm, toRead) != toRead) {
      return false;
    }
    nm[toRead] = 0;

    if (nLen == nameLen && strcmp(nm, name) == 0) {
      out->method = rd16(hdr + 10);
      out->compSize = rd32(hdr + 20);
      out->rawSize = rd32(hdr + 24);
      out->nameLen = nLen;
      out->localOffset = rd32(hdr + 42);
      out->nameExtra = 0;
      return true;
    }
    // 跳过剩余名字 + extra + comment
    const uint32_t skip = (uint32_t)(nLen - toRead) + eLen + cLen;
    if (skip && !file_.seek(file_.position() + skip)) {
      return false;
    }
  }
  return false;
}

// 读出一个条目并解压到新分配的缓冲
bool EpubParser::readEntryAll(const Entry &e, uint8_t **bufOut, uint32_t *lenOut, bool preferPsram) {
  *bufOut = nullptr;
  *lenOut = 0;
  if (e.rawSize == 0 || e.rawSize > (8u * 1024u * 1024u)) {
    return false;  // 太大或空，拒绝
  }

  // 本地头的 extra 长度可能与中央目录不同，必须重新读本地头
  uint8_t lh[30];
  if (!file_.seek(e.localOffset) || file_.read(lh, 30) != 30) {
    return false;
  }
  if (!(lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04)) {
    return false;
  }
  const uint16_t lNameLen = rd16(lh + 26);
  const uint16_t lExtraLen = rd16(lh + 28);
  const uint32_t dataPos = e.localOffset + 30 + lNameLen + lExtraLen;

  uint8_t *raw = (uint8_t *)(preferPsram ? psAlloc(e.rawSize) : malloc(e.rawSize));
  if (!raw) {
    return false;
  }

  if (e.method == 0) {
    // 原样存储
    if (e.rawSize != e.compSize) {
      free(raw);
      return false;
    }
    if (!file_.seek(dataPos) || file_.read(raw, e.rawSize) != e.rawSize) {
      free(raw);
      return false;
    }
  } else if (e.method == 8) {
    // deflate: 需要把压缩数据读进内存再解
    uint8_t *comp = (uint8_t *)psAlloc(e.compSize);
    if (!comp) {
      free(raw);
      return false;
    }
    if (!file_.seek(dataPos) || file_.read(comp, e.compSize) != e.compSize) {
      free(comp);
      free(raw);
      return false;
    }
    tinfl_decompressor *dec = (tinfl_decompressor *)psAlloc(sizeof(tinfl_decompressor));
    if (!dec) {
      free(comp);
      free(raw);
      return false;
    }
    tinfl_init(dec);
    size_t inLen = e.compSize;
    size_t outLen = e.rawSize;
    const tinfl_status st = tinfl_decompress(dec, comp, &inLen, raw, raw, &outLen,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(dec);
    free(comp);
    if (st != TINFL_STATUS_DONE) {
      log_e("[epub] deflate 解压失败: status=%d (rawSize=%u compSize=%u)", (int)st, (unsigned)e.rawSize,
            (unsigned)e.compSize);
      free(raw);
      return false;
    }
  } else {
    log_e("[epub] 不支持的压缩方式 %u (只支持 0=存储 8=deflate)", e.method);
    free(raw);
    return false;
  }

  *bufOut = raw;
  *lenOut = e.rawSize;
  return true;
}

// ---------------------------------------------------------------------------
bool EpubParser::open(FS &fs, const char *path) {
  close();
  fs_ = &fs;
  err_[0] = 0;

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    snprintf(err_, sizeof(err_), "打开 epub 时等 SD 锁超时");
    return false;
  }
  file_ = fs.open(path, FILE_READ);
  if (!file_) {
    snprintf(err_, sizeof(err_), "打不开 %s", path);
    return false;
  }

  // 先确认是 ZIP（前 4 字节 PK\x03\x04）
  uint8_t sig[4];
  if (!file_.seek(0) || file_.read(sig, 4) != 4 || sig[0] != 'P' || sig[1] != 'K') {
    snprintf(err_, sizeof(err_), "不是 EPUB(ZIP) 文件");
    return false;
  }

  if (!parseContainer()) {
    return false;
  }
  if (!parseOpf()) {
    return false;
  }

  ready_ = true;
  log_i("[epub] 解析成功: opf=%s, spine %d 项, manifest %d 项", opfPath_, spineCount_, manifestCount_);
  return true;
}

bool EpubParser::parseContainer() {
  Entry e;
  if (!findEntry("META-INF/container.xml", &e)) {
    snprintf(err_, sizeof(err_), "EPUB 里没有 META-INF/container.xml");
    return false;
  }
  uint8_t *buf = nullptr;
  uint32_t len = 0;
  if (!readEntryAll(e, &buf, &len, false)) {
    snprintf(err_, sizeof(err_), "解压 container.xml 失败");
    return false;
  }

  // ★ 诊断: 把设备端**实际解压出来的字节**打出来 ★
  //   host 端用 Python 验证过这个 container.xml 是标准的, 但设备端报
  //   "没找到 rootfile full-path" —— 所以必须看到真机上到底读到了什么。
  //   如果这里是乱码/空/长度不对, 说明问题在 ZIP 解压(tinfl)而不是 XML 解析。
  {
    Serial.printf("[epub] container.xml: 压缩方式=%u 压缩后=%u 原始=%u 解出=%u 字节\n", (unsigned)e.method,
                  (unsigned)e.compSize, (unsigned)e.rawSize, (unsigned)len);
    const uint32_t show = len < 320 ? len : 320;
    char line[100];
    int n = 0;
    for (uint32_t i = 0; i < show; i++) {
      const uint8_t c = buf[i];
      line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
      if (n >= 92 || i + 1 == show) {
        line[n] = 0;
        Serial.printf("[epub]   |%s\n", line);
        n = 0;
      }
    }
    Serial.flush();
  }

  int vlen = 0;
  const char *v = xmlAttr((const char *)buf, (const char *)buf + len, "rootfile", "full-path", &vlen);
  if (!v || vlen <= 0 || vlen >= (int)sizeof(opfPath_)) {
    free(buf);
    snprintf(err_, sizeof(err_), "container.xml 里没找到 rootfile full-path");
    return false;
  }
  memcpy(opfPath_, v, vlen);
  opfPath_[vlen] = 0;
  free(buf);

  // opf 所在目录(用于拼相对路径)
  const char *slash = strrchr(opfPath_, '/');
  if (slash) {
    const size_t dlen = (size_t)(slash - opfPath_);
    if (dlen < sizeof(opfDir_)) {
      memcpy(opfDir_, opfPath_, dlen);
      opfDir_[dlen] = 0;
    }
  } else {
    opfDir_[0] = 0;  // opf 在根目录
  }
  return true;
}

// 把 href 拼成 zip 内的完整路径
static void joinPath(const char *dir, const char *href, char *out, size_t outCap) {
  if (href[0] == '/') {
    snprintf(out, outCap, "%s", href + 1);
    return;
  }
  if (dir[0] == 0) {
    snprintf(out, outCap, "%s", href);
    return;
  }
  snprintf(out, outCap, "%s/%s", dir, href);
}

bool EpubParser::parseOpf() {
  Entry e;
  if (!findEntry(opfPath_, &e)) {
    snprintf(err_, sizeof(err_), "EPUB 里找不到 %s", opfPath_);
    return false;
  }
  uint8_t *buf = nullptr;
  uint32_t len = 0;
  if (!readEntryAll(e, &buf, &len, false)) {
    snprintf(err_, sizeof(err_), "解压 %s 失败", opfPath_);
    return false;
  }

  if (!manifest_) {
    manifest_ = (ManifestItem *)psAlloc(sizeof(ManifestItem) * EPUB_MAX_MANIFEST);
  }
  if (!spine_) {
    spine_ = (char(*)[128])psAlloc((size_t)128 * EPUB_MAX_SPINE);
  }
  if (!manifest_ || !spine_) {
    free(buf);
    snprintf(err_, sizeof(err_), "EPUB 索引内存不足");
    return false;
  }
  manifestCount_ = 0;
  spineCount_ = 0;

  const char *p = (const char *)buf;
  const char *end = p + len;

  // 1) 收集 manifest: id -> href
  const char *scan = p;
  while (manifestCount_ < EPUB_MAX_MANIFEST) {
    const char *lt = (const char *)memchr(scan, '<', end - scan);
    if (!lt) {
      break;
    }
    if (lt + 5 < end && strncmp(lt + 1, "item", 4) == 0 && xmlIsNameEnd(lt[5])) {
      const char *gt = (const char *)memchr(lt, '>', end - lt);
      if (!gt) {
        break;
      }
      int idLen = 0, hrefLen = 0;
      // ★ end 传 gt+1(不含 '>')★ 传 gt 的话 xmlAttr 内部的 memchr 看不到这个 '>',
      //   会一律返回 nullptr —— 这正是"OPF 解开了却一个 spine 都没有"的原因。
      const char *id = xmlAttr(lt, gt + 1, "item", "id", &idLen);
      const char *href = xmlAttr(lt, gt + 1, "item", "href", &hrefLen);
      if (id && href && idLen > 0 && idLen < 64 && hrefLen > 0 && hrefLen < 128) {
        ManifestItem &mi = manifest_[manifestCount_];
        char idBuf[64];
        memcpy(idBuf, id, idLen);
        idBuf[idLen] = 0;
        mi.idHash = hashName(idBuf);
        memcpy(mi.href, href, hrefLen);
        mi.href[hrefLen] = 0;
        manifestCount_++;
      }
      scan = gt + 1;
    } else {
      scan = lt + 1;
    }
  }

  // 2) 按 spine 顺序把 idref 换成 href
  scan = p;
  while (spineCount_ < EPUB_MAX_SPINE) {
    const char *lt = (const char *)memchr(scan, '<', end - scan);
    if (!lt) {
      break;
    }
    if (lt + 9 < end && strncmp(lt + 1, "itemref", 7) == 0 && xmlIsNameEnd(lt[8])) {
      const char *gt = (const char *)memchr(lt, '>', end - lt);
      if (!gt) {
        break;
      }
      int idLen = 0;
      const char *id = xmlAttr(lt, gt + 1, "itemref", "idref", &idLen);
      if (id && idLen > 0 && idLen < 64) {
        char idBuf[64];
        memcpy(idBuf, id, idLen);
        idBuf[idLen] = 0;
        const uint32_t h = hashName(idBuf);
        for (int i = 0; i < manifestCount_; i++) {
          if (manifest_[i].idHash == h) {
            joinPath(opfDir_, manifest_[i].href, spine_[spineCount_], 128);
            spineCount_++;
            break;
          }
        }
      }
      scan = gt + 1;
    } else {
      scan = lt + 1;
    }
  }

  // 3) 估算纯文本大小(只统计 xhtml 的 rawSize)
  estTextBytes_ = 0;
  for (int i = 0; i < spineCount_; i++) {
    Entry se;
    if (findEntry(spine_[i], &se)) {
      estTextBytes_ += se.rawSize;
    }
  }

  free(buf);
  if (spineCount_ == 0) {
    snprintf(err_, sizeof(err_), "content.opf 里没有可读的 spine 条目");
    return false;
  }
  return true;
}

// 在"原始 XHTML"上把图片引用替换成标记 "\n<FF><idx><FF>\n"。
// 必须在 epubXhtmlToText() 之前调用 —— 因为剥标签会把 <img> 一起丢掉,
// 之后就再也找不到图片引用了(这是踩过的坑)。
uint32_t EpubParser::markImagesInXhtml(const char *xhtml, uint32_t len, const char *xhtmlPath, char *out,
                                       uint32_t outCap) {
  uint32_t o = 0;
  uint32_t i = 0;
  int found = 0;
  while (i < len && o + 24 < outCap) {
    if (xhtml[i] != '<') {
      out[o++] = xhtml[i++];
      continue;
    }
    // 看是不是 <img 或 <image
    // 注意: 名字后面必须是分隔符, 否则 <image> 会被 "<img" 的 4 字符前缀抢走
    int kind = -1;
    if (i + 4 < len && strncasecmp(xhtml + i, "<img", 4) == 0 && xmlIsNameEnd(xhtml[i + 4])) {
      kind = 0;
    } else if (i + 6 < len && strncasecmp(xhtml + i, "<image", 6) == 0 && xmlIsNameEnd(xhtml[i + 6])) {
      kind = 1;
    }
    if (kind < 0) {
      out[o++] = xhtml[i++];
      continue;
    }
    const char *gtp = xmlFindTagEnd(xhtml + i, xhtml + len);
    if (!gtp) {
      out[o++] = xhtml[i++];
      continue;
    }
    const uint32_t gt = (uint32_t)(gtp - xhtml);
    const char *attr = (kind == 0) ? "src" : "xlink:href";
    const size_t alen = strlen(attr);

    // 取属性值: 跳过引号内的内容, 属性名要求左边界是空白。
    // 否则 <img data-src="a" src="b"> 会把 data-src 的值当成 src。
    char rel[EPUB_IMG_NAME_LEN] = {0};
    {
      char inq = 0;
      for (uint32_t q = i + 1; q + alen < gt; q++) {
        const char c = xhtml[q];
        if (inq) {
          if (c == inq) {
            inq = 0;
          }
          continue;
        }
        if (c == '"' || c == '\'') {
          inq = c;
          continue;
        }
        if (!xmlIsSpace(xhtml[q - 1])) {
          continue;   // 属性名必须紧跟空白, "data-src" 里的 src 不算
        }
        if (strncasecmp(xhtml + q, attr, alen) == 0) {
          uint32_t v = q + alen;
          while (v < gt && xmlIsSpace(xhtml[v])) {
            v++;
          }
          if (v < gt && xhtml[v] == '=') {
            v++;
            while (v < gt && xmlIsSpace(xhtml[v])) {
              v++;
            }
            if (v < gt && (xhtml[v] == '"' || xhtml[v] == '\'')) {
              const char qc = xhtml[v++];
              uint32_t rl = 0;
              while (v < gt && xhtml[v] != qc && rl < sizeof(rel) - 1) {
                rel[rl++] = xhtml[v++];
              }
              rel[rl] = 0;
            }
          }
          break;
        }
      }
    }

    if (rel[0]) {
      // 解析相对路径 -> zip 内路径
      char zipPath[EPUB_IMG_NAME_LEN];
      if (rel[0] == '/') {
        snprintf(zipPath, sizeof(zipPath), "%s", rel + 1);
      } else {
        char base[EPUB_IMG_NAME_LEN];
        snprintf(base, sizeof(base), "%s", xhtmlPath);
        char *sl = strrchr(base, '/');
        if (sl) {
          *sl = 0;
        } else {
          base[0] = 0;
        }
        char joined[EPUB_IMG_NAME_LEN * 2];
        snprintf(joined, sizeof(joined), "%s/%s", base, rel);

        // ★ 归一化 ./ 与 ../ ★
        // 旧实现只维护一个 depth 计数器, ".." 时**不把上一段从结果里删掉**,
        // 于是 "item/xhtml/../image/p047.jpg" 变成 "item/xhtml/image/p047.jpg";
        // 而且 depth 归零后不再插 '/' , 连 "OEBPSx.jpg" 这种拼接都会出现。
        // 真实书里 21/22 张插图用的正是 "../image/xxx.jpg", 全部因此找不到。
        // 现在按 ZIP 路径的语义做真正的分量栈: ".." 弹出最后一段(已在根就忽略)。
        char norm[EPUB_IMG_NAME_LEN];
        uint32_t normLen = 0;
        const char *seg = joined;
        while (*seg) {
          while (*seg == '/') {
            seg++;   // 跳过多余的分隔符
          }
          if (!*seg) {
            break;
          }
          const char *segEnd = seg;
          while (*segEnd && *segEnd != '/') {
            segEnd++;
          }
          const uint32_t segLen = (uint32_t)(segEnd - seg);
          if (segLen == 1 && seg[0] == '.') {
            // 当前目录: 忽略
          } else if (segLen == 2 && seg[0] == '.' && seg[1] == '.') {
            // 上一级: 删掉结果末尾的最后一个分量(含它前面的 '/')
            while (normLen > 0 && norm[normLen - 1] != '/') {
              normLen--;
            }
            if (normLen > 0) {
              normLen--;
            }
          } else {
            if (normLen > 0 && normLen < sizeof(norm) - 1) {
              norm[normLen++] = '/';
            }
            uint32_t room = (uint32_t)(sizeof(norm) - 1 - normLen);
            const uint32_t cp = segLen < room ? segLen : room;
            memcpy(norm + normLen, seg, cp);
            normLen += cp;
          }
          seg = segEnd;
        }
        norm[normLen] = 0;
        snprintf(zipPath, sizeof(zipPath), "%s", norm);
      }

      const int idx = noteImage(zipPath);
      if (idx >= 0) {
        // 标记必须用**可打印字符** @ 包起来, 不能用 \f:
        // 行整理会把行首的 \f 当空白去掉, 结果标记被拆散, 界面就认不出来了。
        const int w = snprintf(out + o, outCap - o, "\n@%d@\n", idx);
        if (w > 0) {
          o += (uint32_t)w;
          found++;
        }
      }
    }
    i = gt + 1;  // 跳过整个标签(不放回输出)
  }
  // 收尾
  if (o < outCap) {
    out[o] = 0;
  }
  if (found && outCap > 0) {
    log_i("[epub] 本页替换 %d 处图片引用", found);
  }
  return o;
}

int EpubParser::noteImage(const char *zipPath) {
  for (int i = 0; i < imageCount_; i++) {
    if (strcmp(images_[i], zipPath) == 0) {
      return i;
    }
  }
  if (imageCount_ >= EPUB_MAX_IMAGES) {
    return -1;
  }
  if (!images_) {
    images_ = (char(*)[EPUB_IMG_NAME_LEN])psAlloc((size_t)EPUB_IMG_NAME_LEN * EPUB_MAX_IMAGES);
    imageSpine_ = (uint16_t *)psAlloc(sizeof(uint16_t) * EPUB_MAX_IMAGES);
  }
  if (!images_ || !imageSpine_) {
    return -1;
  }
  snprintf(images_[imageCount_], EPUB_IMG_NAME_LEN, "%s", zipPath);
  imageSpine_[imageCount_] = 0xFFFF;
  return imageCount_++;
}

// ---------------------------------------------------------------------------
// XHTML -> 纯文本
// ---------------------------------------------------------------------------
uint32_t epubXhtmlToText(const char *xhtml, uint32_t len, char *out, uint32_t outCap) {
  uint32_t o = 0;
  if (outCap < 8) {
    return 0;
  }
  const char *p = xhtml;
  const char *end = xhtml + len;

  // 跳过 <head>...</head>
  const char *b = (const char *)memchr(p, '<', end - p);
  const char *body = nullptr;
  {
    const char *q = p;
    while (q < end) {
      const char *lt = (const char *)memchr(q, '<', end - q);
      if (!lt) {
        break;
      }
      if (lt + 5 < end && (strncmp(lt + 1, "body", 4) == 0) && xmlIsNameEnd(lt[5])) {
        const char *gt = (const char *)memchr(lt, '>', end - lt);
        if (gt) {
          body = gt + 1;
        }
        break;
      }
      q = lt + 1;
    }
  }
  p = body ? body : (b ? b : xhtml);

  while (p < end && o + 8 < outCap) {
    if (*p == '<') {
      // 注释整段丢掉。不处理的话 "<!-- <p>x</p> -->" 会把注释文字和 "-->" 混进正文。
      if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
        const char *ce = p + 4;
        const char *ceEnd = nullptr;
        while (ce + 2 < end) {
          if (ce[0] == '-' && ce[1] == '-') {
            // 注释结束标记是 "-->", 那个 '>' 也要一起吃掉
            ceEnd = (ce[2] == '>') ? ce + 3 : ce + 2;
            break;
          }
          ce++;
        }
        if (ceEnd) {
          p = ceEnd;
          continue;
        }
        break;   // 注释没闭合: 后面的内容不可信
      }
      const char *gt = xmlFindTagEnd(p, end);
      if (!gt) {
        break;
      }
      // 判断标签类型
      const bool closing = (p + 1 < gt && p[1] == '/');
      const char *nm = p + (closing ? 2 : 1);
      const size_t nmLen = (size_t)(gt - nm);
      // 纯标签名长度(到空白/'>'/'/'为止)。
      // ★ 必须先截出来 ★ 以前直接拿"整个标签内容"当前缀比较, 于是
      //   <pre> 命中 "p"、<link> 命中 "li"(多出空行), 而
      //   <style type="text/css"> 又因为带了属性而匹配不上 </style>,
      //   结果整段 CSS 被当正文吐出来。
      size_t nameLen = 0;
      while (nameLen < nmLen && !xmlIsSpace(nm[nameLen]) && nm[nameLen] != '>' && nm[nameLen] != '/') {
        nameLen++;
      }

      auto isTag = [&](const char *t) {
        const size_t l = strlen(t);
        return nameLen == l && strncasecmp(nm, t, l) == 0;
      };

      if (isTag("br") || isTag("p") || isTag("div") || isTag("h1") || isTag("h2") || isTag("h3") ||
          isTag("h4") || isTag("h5") || isTag("h6") || isTag("li") || isTag("tr")) {
        // 段落/换行 -> 换行符
        out[o++] = '\n';
      } else if (isTag("script") || isTag("style")) {
        // 跳过其内容
        if (!closing) {
          const char *close = nullptr;
          for (const char *q = gt + 1; q + 2 < end; q++) {
            if (q[0] == '<' && q[1] == '/' && (size_t)(end - (q + 2)) >= nameLen &&
                strncasecmp(q + 2, nm, nameLen) == 0) {
              close = xmlFindTagEnd(q + 2, end);
              if (close) {
                break;
              }
            }
          }
          if (close) {
            p = close + 1;
            continue;
          }
        }
      } else if (isTag("img")) {
        // 图片另起一页: 用换行隔开, 后续 epub 阅读器会识别出独立图片页
        out[o++] = '\n';
      }
      p = gt + 1;
      continue;
    }

    // 普通字节(含 UTF-8 多字节, 原样透传)
    if (*p == '\n' || *p == '\r') {
      p++;  // 源文件里的换行忽略, 结构由标签决定
      continue;
    }
    out[o++] = *p++;
  }

  // ---- 按行整理 ----
  // XHTML 源文件里 <p> 标签之间通常有换行+缩进(ASCII 空格), 剥离标签后会留下
  // 大量"只含空格的假空行", 一页能占掉好几行正文。
  // 注意: 中文段落缩进用的是**全角空格 U+3000**(E3 80 80), 那是内容, 必须保留;
  // 这里只清理 ASCII 空格(U+0020)和制表符。
  uint32_t w = 0, nl = 0;
  for (uint32_t i = 0; i < o;) {
    if (out[i] == '\n') {
      out[w++] = '\n';
      i++;
      nl++;
      continue;
    }
    // 取出一整行(到下一个换行符)
    uint32_t j = i;
    while (j < o && out[j] != '\n') {
      j++;
    }
    // 去掉行首的 ASCII 空格/制表符
    uint32_t ls = i;
    while (ls < j && (out[ls] == ' ' || out[ls] == '\t')) {
      ls++;
    }
    // 判断这一行是不是"只剩 ASCII 空白"
    bool blank = true;
    for (uint32_t k = ls; k < j; k++) {
      const uint8_t c = (uint8_t)out[k];
      // 全角空格(E3 80 80)算内容
      if (c != ' ' && c != '\t' && c != 0xEF && c != 0x80 && c != 0xE3) {
        blank = false;
        break;
      }
    }
    if (blank) {
      // 整行都是空白 -> 变成真正的空行(连续空行随后被压缩)
      out[w++] = '\n';
      nl++;
      i = j;
      continue;
    }
    for (uint32_t k = ls; k < j; k++) {
      out[w++] = out[k];
    }
    out[w++] = '\n';
    nl = 0;
    i = (j < o) ? j + 1 : j;
  }
  o = w;

  // 压缩连续空行(最多保留一个空行)
  w = 0;
  nl = 0;
  for (uint32_t i = 0; i < o; i++) {
    if (out[i] == '\n') {
      nl++;
      if (nl > 2) {
        continue;
      }
    } else {
      nl = 0;
    }
    out[w++] = out[i];
  }
  // 去掉开头的空行
  uint32_t s = 0;
  while (s < w && (out[s] == '\n')) {
    s++;
  }
  if (s) {
    memmove(out, out + s, w - s);
    w -= s;
  }
  return w;
}

// ---------------------------------------------------------------------------
bool EpubParser::extractTextTo(const char *outPath, EpubProgressFn fn, void *ctx) {
  if (!ready_) {
    return false;
  }
  const int total = spineCount_;

  // 1) 先解压所有需要处理的 spine 项，估算进度用压缩后大小累计
  uint32_t totalComp = 0;
  for (int i = 0; i < spineCount_; i++) {
    Entry e;
    if (findEntry(spine_[i], &e)) {
      totalComp += e.compSize;
    }
  }
  if (totalComp == 0) {
    totalComp = 1;
  }

  // 2) 打开输出文件
  File out;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      return false;
    }
    out = fs_->open(outPath, FILE_WRITE);
    if (!out) {
      log_e("[epub] 无法创建缓存文件 %s", outPath);
      return false;
    }
    // ★ 这里【绝对不能】往缓存里写版本标记 ★ (2026-10-03 踩过)
    //   我曾把 "EPUBCACHE v2" 写在缓存第一行当版本标记, 结果阅读器把它当**正文**:
    //     [pg] 首行=[EPUBCACHE]     <- 第 1 页被这行占掉 -> 分页全乱
    //   症状: 插图只显示前几张、正文被吞。
    //   正确做法: 版本号放在**缓存文件名**里(见 bookreader.ino 的 epubCachePath),
    //   正文文件保持纯净, 一个字都不多写。
  }

  // 3) 逐个 spine 项: 解压 -> 提取文本 -> 写缓存
  uint8_t *raw = nullptr;
  uint32_t rawLen = 0;
  char *text = nullptr;
  const uint32_t textCap = kEpubTextCap;  // 单个 xhtml 解压后最大按 512KB 处理
  text = (char *)psAlloc(textCap + 1);
  if (!text) {
    log_e("[epub] 文本缓冲分配失败");
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      out.close();
    }
    return false;
  }

  uint32_t doneComp = 0;
  int written = 0;
  int imgFound = 0;
  bool writeFailed = false;   // 任何一章写入不完整 -> 整份缓存不可信(见下面收尾处)
  for (int i2 = 0; i2 < spineCount_; i2++) {
    Entry e;
    if (!findEntry(spine_[i2], &e)) {
      log_w("[epub] 找不到条目 %s, 跳过", spine_[i2]);
      continue;
    }

    // 不是 xhtml 的条目(纯图片页)跳过
    // (按后缀判断, 且长度不足时不去读负数下标 —— 以前要求 nl>6, 名字只有
    //  6 个字符的 "a.html" 会被整章跳过)
    if (!isXhtmlPath(spine_[i2])) {
      doneComp += e.compSize;
      continue;
    }

    if (!readEntryAll(e, &raw, &rawLen, true)) {
      log_w("[epub] 解压 %s 失败, 跳过", spine_[i2]);
      doneComp += e.compSize;
      continue;
    }

    // ★ 顺序很重要 ★ 必须先在原始 XHTML 上把图片换成标记, 再剥标签。
    //   反过来的话 <img> 已经被剥掉, 就再也找不到图片引用了。
    const uint32_t markedLen = markImagesInXhtml((const char *)raw, rawLen, spine_[i2], text, textCap);
    const uint32_t tlen = epubXhtmlToText(text, markedLen, text, textCap);
    const uint32_t tlen2 = tlen;

    if (tlen2 > 0) {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (g.ok()) {
        // ★ 必须检查 write() 的返回值 ★
        //   原来这里是 `out.write(...)` 丢掉返回值 + `written += tlen2` 按"全写成功"计数。
        //   实测后果(2026-10-03, 30GB 卡): 正文缓存 362995 字节里出现**空洞** ——
        //   文件大小对, 但内容有大段旧的/零数据, 读出来就是成千上万行空行
        //   (`[pg] 首行=[]`), 表现成"翻页像卡住/页面空白、只有开头两页正常"。
        //   同一根因也造成插图: `插图写入不完整(622592/686936)`。
        //   现在: 校验每次写入的字节数, 不完整就整份重试一次, 再失败就报错 + 删掉
        //   半成品(避免下次被当成"有效缓存"直接复用)。
        uint32_t want = (uint32_t)tlen2 + ((written > 0) ? 2u : 0u);
        size_t got = 0;
        if (written > 0) {
          got += out.write((const uint8_t *)"\n\n", 2);
        }
        got += out.write((const uint8_t *)text, tlen2);
        if (got != want) {
          // ★ 不做"原地重写" ★
          //   一开始我想"重试一次说不定就好", 但那会**造成内容重复**:
          //   若第一次 "\n\n" 写成功、正文只写了一半, 重写会把 "\n\n" 再写一遍,
          //   正文也重复 —— 缓存里有重复段落, 页数/页码全乱。
          //   正确做法: 立刻判定"这份缓存不可信", 整份作废删掉, 下次重新解压。
          //   (整份重试是安全的: 那时文件是空的, 不存在重复问题。)
          BOOTLOG("[epub] !! 正文写入不完整(实写 %u / 应写 %u), 缓存作废", (unsigned)got, (unsigned)want);
          writeFailed = true;
          written += (int)got;   // 只按真实写入量累加
        } else {
          written += (int)tlen2;
        }
      }
    }
    free(raw);
    raw = nullptr;

    doneComp += e.compSize;
    if (fn) {
      fn(ctx, "正在解压 EPUB", (int)((uint64_t)doneComp * 100 / totalComp), 100);
    }
  nextEntry:;
  }

  free(text);
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      out.close();
    } else {
      out.close();
    }
  }

  // ★ 写坏了就把半成品删掉 ★
  //   否则下次打开会看到"缓存存在且够大" -> 直接复用坏文件 -> 永远修不好。
  //   删掉之后下次会重新解压, 用户只要把卡上腾出空间(或换卡)就能自愈。
  if (writeFailed) {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      fs_->remove(outPath);
    }
    BOOTLOG("[epub] !! 正文缓存写入失败, 已删除半成品 %s (下次打开会重新解压)", outPath);
    return false;
  }

  log_i("[epub] 文本提取完成: %d 字节, 登记图片 %d 张 -> %s", written, imageCount_, outPath);
  return written > 0;
}

uint32_t EpubParser::extractImage(int spineIndex, const char *outPath) {
  if (!ready_ || spineIndex < 0 || spineIndex >= spineCount_) {
    return 0;
  }
  // 目前只处理"整个 spine 项就是一个图片"的情况(封面等)
  Entry e;
  if (!findEntry(spine_[spineIndex], &e)) {
    return 0;
  }
  if (!outPath) {
    return e.rawSize;
  }
  uint8_t *raw = nullptr;
  uint32_t len = 0;
  if (!readEntryAll(e, &raw, &len, true)) {
    return 0;
  }
  uint32_t written = 0;
  bool opened = false;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok()) {
      File f = fs_->open(outPath, FILE_WRITE);
      if (f) {
        opened = true;
        written = f.write(raw, len);
        f.flush();
        f.close();
      }
    }
  }
  free(raw);
  // 原来这里**不报错就返回 0**, 调用方分不清"打不开"和"写失败"。
  // 实测(2026-10-03)出现过"插图写入不完整(622592/686936)" —— 卡写满/写入失败时
  // 必须留下明确记录, 否则只会表现成"某几页插图空白"。
  if (!opened) {
    BOOTLOG("[epub] !! 打不开插图输出文件(卡满/写保护?): %s", outPath);
    return 0;
  }
  if (written != len) {
    BOOTLOG("[epub] !! 插图写入不完整(%lu/%lu): %s", (unsigned long)written, (unsigned long)len, outPath);
  }
  return written;
}


uint32_t EpubParser::extractImageByIndex(int index, const char *outPath) {
  if (!ready_ || !images_ || index < 0 || index >= imageCount_ || !outPath) {
    return 0;
  }
  Entry e;
  uint8_t *raw = nullptr;
  uint32_t len = 0;

  // ★ 读 ZIP 这一整段也要持 sd_mutex ★
  //   这里以前只给"写缓存文件"那一下加了锁, 而找中央目录 + 解压(单张图可能上兆
  //   字节)是在锁外做的 —— 与 sd_mutex.h 里写的约定("大块读必须在锁内完成,
  //   不允许中途释放")以及 EpubParser::open() 的做法都不一致。
  //   现在插图导出会被连着调用几十次(21 张图 ≈ 25MB 读写), 必须一致。
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      BOOTLOG("[epub] !! 导出插图时等 SD 锁超时: %s", images_[index]);
      return 0;
    }
    if (!findEntry(images_[index], &e)) {
      BOOTLOG("[epub] !! 找不到图片条目: %s", images_[index]);
      return 0;
    }
    if (!readEntryAll(e, &raw, &len, true)) {
      BOOTLOG("[epub] !! 解压图片失败: %s (方式=%u 压缩=%lu 原始=%lu)", images_[index], (unsigned)e.method,
              (unsigned long)e.compSize, (unsigned long)e.rawSize);
      return 0;
    }
  }

  uint32_t written = 0;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      BOOTLOG("[epub] !! 写插图时等 SD 锁超时: %s", outPath);
      free(raw);
      return 0;
    }
    File f = fs_->open(outPath, FILE_WRITE);
    if (f) {
      written = f.write(raw, len);
      f.close();
    }
    // 只写了一半的图片比没写更糟: 留在卡上会被当成"已经导出过",
    // 下次打开就不补导出, 翻到那页只会看到"插图解码失败"。
    // 直接删掉 → 下次打开时"目录里张数不足"会重新导出。
    if (written != len) {
      BOOTLOG("[epub] !! 插图写入不完整(%lu/%lu), 删掉重来: %s", (unsigned long)written, (unsigned long)len, outPath);
      fs_->remove(outPath);
      written = 0;
    }
  }
  free(raw);
  return written;
}


// ---------------------------------------------------------------------------
// ★ 只解析(不提取正文)时, 也要能拿到图片清单 ★
//
// images_[] 原来**只在 extractTextTo() 里**被 markImagesInXhtml() 顺带登记
// (noteImage() 是唯一的登记入口, epub_parser.cpp:783/801)。所以"复用正文缓存、
// 只 open() 不提取正文"这条路径上 imageCount_ 恒为 0 —— 正文能看、缓存里的 @N@
// 标记都在, 却一张图都导不出来, 插图页永远是空的。
//
// 这里按**和 extractTextTo() 逐字相同**的方式重走一遍:
//   同样的 spine 顺序、同样的 XHTML 过滤(isXhtmlPath)、同样的输出缓冲大小
//   (kEpubTextCap)、同一个 markImagesInXhtml()。
// 输入和代码路径完全一致 ⇒ 登记顺序与当初写缓存标记时一模一样 ⇒ @N@ 不会错位。
// 只读 XHTML(不含 17MB 图片数据), 代价很小, 而且只在真的要导出插图时才做。
// ---------------------------------------------------------------------------
int EpubParser::ensureImageList() {
  if (!ready_) {
    return 0;
  }
  if (imageCount_ > 0 || imagesScanned_) {
    return imageCount_;   // 提取正文时已经登记过, 或这本书确实没图
  }

  // 和 extractTextTo() 用**同一块大小**的输出缓冲: 那边会被截断的章节, 这边
  // 也按同样方式截断, 登记范围才不会和缓存里的标记错位。
  char *scratch = (char *)psAlloc(kEpubTextCap + 1);
  if (!scratch) {
    BOOTLOG("[epub] !! 扫描图片清单: 分配 %lu 字节失败", (unsigned long)(kEpubTextCap + 1));
    return 0;   // 不置 imagesScanned_, 以后还能再试
  }

  int chapters = 0;
  bool aborted = false;
  for (int i = 0; i < spineCount_; i++) {
    if (!isXhtmlPath(spine_[i])) {
      continue;   // 和 extractTextTo() 一样: 非 XHTML 的 spine 项不参与登记
    }
    Entry e;
    uint8_t *raw = nullptr;
    uint32_t rawLen = 0;
    bool chapterOk = false;
    {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      chapterOk = g.ok() && findEntry(spine_[i], &e) && readEntryAll(e, &raw, &rawLen, true);
    }
    // ★ 任何一章读不出来都**整体作废**, 不能"跳过这章继续" ★
    //   少登记一章的图片, 后面所有章节的编号都会往前挪 —— 与缓存里的 @N@ 全部
    //   错位(会显示成别的图, 比不显示更糟)。宁可这次一张都不导出。
    if (!chapterOk) {
      free(raw);
      BOOTLOG("[epub] !! 扫描图片清单失败(第 %d 章 %s), 本次不导出插图", i, spine_[i]);
      aborted = true;
      break;
    }
    // 只要副作用(noteImage 登记), 输出丢进 scratch
    markImagesInXhtml((const char *)raw, rawLen, spine_[i], scratch, kEpubTextCap);
    free(raw);
    chapters++;
  }
  free(scratch);

  if (aborted) {
    // 清掉半份清单, 免得被当成有效结果(编号已经错位了)
    if (images_) {
      free(images_);
      images_ = nullptr;
    }
    if (imageSpine_) {
      free(imageSpine_);
      imageSpine_ = nullptr;
    }
    imageCount_ = 0;
    return 0;
  }

  imagesScanned_ = true;   // 只有完整扫完才记住, 避免把半份清单缓存下来
  BOOTLOG("[epub] 图片清单扫描完成: %d 章 -> %d 张图", chapters, imageCount_);
  return imageCount_;
}


// ---------------------------------------------------------------------------
// 一次性导出全部插图到 dir/img_NN.jpg|png
// ★ 命名规则只留这一处 ★ 以前"导出"(bookreader.ino 里 snprintf img_%02d)和
//   "查找"(currentPageImage 里拼 img_%02d + .jpg/.png)各写一份, 很容易改岔。
// ---------------------------------------------------------------------------
int EpubParser::extractAllImages(const char *dir, EpubProgressFn fn, void *ctx) {
  if (!ready_ || !fs_ || !dir || !dir[0]) {
    return 0;
  }
  // ★ 复用正文缓存时清单是空的, 先按缓存标记的顺序把它建起来 ★
  //   扫描要读各章 XHTML(几秒), 先给界面一句提示, 免得看起来像卡死。
  if (imageCount_ <= 0 && !imagesScanned_ && fn) {
    fn(ctx, "正在扫描插图", 0, 100);
  }
  if (ensureImageList() <= 0) {
    return 0;
  }

  // 目录不存在就建(调用方忘了 ensureDir 也不会 21 张全部失败)
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok() && !fs_->exists(dir)) {
      fs_->mkdir(dir);
    }
  }

  char path[192];
  int okCount = 0;
  for (int i = 0; i < imageCount_; i++) {
    snprintf(path, sizeof(path), "%s/img_%02d%s", dir, i, imageExt(i));
    if (extractImageByIndex(i, path) > 0) {
      okCount++;
    } else {
      BOOTLOG("[epub] !! 插图 %d/%d 导出失败 -> %s", i + 1, imageCount_, path);
    }
    if (fn) {
      // 约定和 extractTextTo 一致: cur 是百分比, total 恒为 100
      fn(ctx, "正在导出插图", (int)((int64_t)(i + 1) * 100 / imageCount_), 100);
    }
  }
  return okCount;
}


// 数一数 dir 里已经有多少张 img_NN.*(判断"要不要补导出"用, 不需要解析 epub)
int epubCountExportedImages(FS &fs, const char *dir) {
  if (!dir || !dir[0]) {
    return 0;
  }
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    return 0;
  }
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) {
    d.close();
    return 0;
  }
  int n = 0;
  File f;
  while ((f = d.openNextFile())) {
    const char *nm = f.name();
    if (nm) {
      // name() 在不同核心上可能返回全路径, 取最后一段再比
      const char *base = strrchr(nm, '/');
      base = base ? (base + 1) : nm;
      if (strncmp(base, "img_", 4) == 0) {
        n++;
      }
    }
    f.close();
  }
  d.close();
  return n;
}


const char *EpubParser::imageExt(int index) {
  if (!images_ || index < 0 || index >= imageCount_) {
    return ".jpg";
  }
  const char *dot = strrchr(images_[index], '.');
  if (dot && strcasecmp(dot, ".png") == 0) {
    return ".png";
  }
  return ".jpg";
}
