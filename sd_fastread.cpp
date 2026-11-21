/*
 * sd_fastread.cpp — 见 sd_fastread.h 顶部的完整说明
 *
 * 重要实现细节（踩错就会读到错位数据）
 * ------------------------------------
 * SD 卡在 SPI 模式下，每个数据块是:
 *     [数据令牌 0xFE] [512 字节数据] [2 字节 CRC16]
 * 所以**每读一个扇区必须吃满 514 字节**，否则下一段的令牌会对不齐，
 * 表现为"数据整体错位、解码器找不到帧头"。
 * 本实现统一用 514 字节的中转缓冲: 先读进临时缓冲(含 2 字节 CRC 一起吃掉),
 * 再把 512 字节拷到目标 buffer。多一次 memcpy(16KB 约 1ms), 换来绝不越界、
 * 绝不把 CRC 写进用户缓冲。
 */
#include "sd_fastread.h"

#include "config.h"

#include <SPI.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SD 命令
// ---------------------------------------------------------------------------
enum SdCmd {
  CMD0_GO_IDLE = 0,
  CMD8_SEND_IF_COND = 8,
  CMD17_READ_SINGLE = 17,
  CMD18_READ_MULTI = 18,
  CMD12_STOP_TRANSMISSION = 12,
  CMD55_APP_CMD = 55,
  CMD59_CRC_ON_OFF = 59,
  ACMD41_SD_SEND_OP_COND = 41,
};

// CRC7（多项式 x^7+x^3+1）。
// ★ 每条命令都必须发真 CRC7 ★ 不能图省事送 0x01: SD.begin() 已经用
//   CMD59(arg=1) 打开了卡的命令 CRC 校验, 送错 CRC 的命令会被卡直接丢弃
//   (表现为"命令毫无响应")。CRC7 只算 5 个字节, 每条命令一次, 开销可忽略。
static uint8_t crc7(const uint8_t *d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int j = 0; j < 8; j++) {
      crc <<= 1;
      if ((b & 0x80) ^ (crc & 0x80)) {
        crc ^= 0x09;
      }
      b <<= 1;
    }
  }
  return (uint8_t)(crc & 0x7F);
}

// SPI 单例：必须和 SD 库 / TFT 用同一个实例（见 display.cpp 的总线共用说明）
static SPIClass *s_spi = nullptr;

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
SdFastFile::~SdFastFile() {
  close();
}

void SdFastFile::close() {
  if (clusterMap_) {
    free(clusterMap_);
    clusterMap_ = nullptr;
  }
  if (secBuf_) {
    free(secBuf_);
    secBuf_ = nullptr;
  }
  clusterCount_ = 0;
  fileSize_ = 0;
  pos_ = 0;
  open_ = false;
}

void SdFastFile::csLow() {
  digitalWrite(PIN_SD_CS, LOW);
}

void SdFastFile::csHigh() {
  digitalWrite(PIN_SD_CS, HIGH);
}

// 发一条命令并读 R1 响应（返回 0xFF 表示没收到）。
//
// ★ 每条命令都发真 CRC7 ★
//   为什么: SD.begin() 在初始化时用 CMD59(arg=1) **打开**了卡的 CRC 校验
//   (sd_diskio.cpp:572)。开启后, 卡会校验每条命令的 CRC7, 错的直接丢弃 ——
//   表现为"命令没有任何响应"。所以这里不能图省事送 0x01。
//   CRC7 只算 5 个字节, 开销可以忽略(每条命令一次, 不是每个字节一次)。
uint8_t SdFastFile::cmd(uint8_t c, uint32_t arg) {
  uint8_t pkt[6];
  pkt[0] = (uint8_t)(c | 0x40);
  pkt[1] = (uint8_t)(arg >> 24);
  pkt[2] = (uint8_t)(arg >> 16);
  pkt[3] = (uint8_t)(arg >> 8);
  pkt[4] = (uint8_t)arg;
  pkt[5] = (uint8_t)((crc7(pkt, 5) << 1) | 0x01);   // ★ 总是真 CRC7 ★

  // ★ 硬化(上机拿到假 R1=0x20 之后加的)★
  //   0x20 = "address error", 而我们读的是 LBA 0 —— 那**不可能**是真地址错,
  //   所以它八成是上一次事务(库路径的 CMD18 等)留在线上的残余数据字节被当成了 R1。
  //   三条对策:
  //     ① 命令前"排空": CS 高打 16 个时钟(残余的多扇区读会就此结束), 再 CS 低打 10 个;
  //     ② 轮询从 16 次加到 128 次(卡慢也能等到);
  //     ③ 命令最多重发 3 次 —— 库自己的 sdCommand 也是靠重试才成功的
  //        (那两条 delay(100) 就是证据), 说明这张卡确实需要重试。
  //   命令后读到的前 4 个原始字节记进 dbgRaw_, 失败时会打进 err_ 便于一眼定性。
  dbgRawN_ = 0;
  dbgLastR1_ = 0xFF;
  for (int attempt = 0; attempt < 3; attempt++) {
    csHigh();
    for (int i = 0; i < 16; i++) {
      s_spi->transfer(0xFF);   // 排空残余数据流(CS 高, 卡不响应)
    }
    csLow();
    for (int i = 0; i < 10; i++) {
      s_spi->transfer(0xFF);
    }

    s_spi->writeBytes(pkt, 6);
    uint8_t r = 0xFF;
    for (int i = 0; i < 128; i++) {
      r = s_spi->transfer(0xFF);
      if (attempt == 0 && dbgRawN_ < 4) {
        dbgRaw_[dbgRawN_++] = r;   // 诊断: 命令后卡吐出的原始字节序列
      }
      if (!(r & 0x80)) {
        break;
      }
    }
    dbgLastR1_ = r;
    if (r == 0x00) {
      return r;                    // ★ 正常: 保持 CS 低, 调用方接着读数据
    }
    csHigh();                      // 失败: 抬 CS, 歇一下再重试
    delayMicroseconds(200);
  }
  return dbgLastR1_;               // 三次都不行 -> 调用方回落
}

// 静态：把 SPI 实例准备好（只需一次）
// 返回 false 表示拿不到总线实例 —— 调用方必须回落。
static bool s_ready = false;
static bool s_crcOffOk = false;

bool SdFastFile::rawInit(uint32_t hz) {
  // 由调用方通过一个全局 setter 注入 SPI 实例；这里用弱符号方式获取，
  // 避免 sd_fastread 依赖 display.h（那会引入循环包含）。
  if (!s_spi) {
    snprintf(err_, sizeof(err_), "SPI 实例未注入");
    return false;
  }
  if (!s_ready) {
    pinMode(PIN_SD_CS, OUTPUT);
    csHigh();
    s_ready = true;
  }

  s_spi->beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  actualHz_ = hz;

  // ★ 这里【故意不发 CMD0】★ —— 这是上一版把音频彻底搞坏的原因
  //
  // CMD0(GO_IDLE_STATE) 是把卡**打回 idle(未初始化)状态**的复位命令。
  // 我们的卡是 SD.begin() 已经初始化好的；再发 CMD0 会让它回到"需要重新走
  // 完整的 ACMD41/CMD58 流程"的状态，而我们并没有走那套流程 ——
  // 结果之后每一条 CMD18 都超时，音频一个字都读不出来。
  //
  // 正确做法: **完全不动卡的状态**。SD.begin() 留下的状态正是我们要用的
  // (已初始化、块寻址、512 字节块)。我们只是换一种方式去读它。
  //
  // CRC 校验同理: SD.begin() 已经用 CMD59(arg=1) 打开了卡的 CRC 校验，我们
  // 不去关它(关它也会改变卡的状态)，而是在 cmd() 里为**每条命令都发真 CRC7**。
  // 读回的数据不校验 CRC16 —— 那是纯校验开销，数据本身照常拿到。
  csLow();
  for (int i = 0; i < 40; i++) {
    s_spi->transfer(0xFF);   // 只有时钟, 不发命令
  }
  csHigh();

  // ★ 清掉可能残留的多扇区读状态 ★
  //   库路径的 sdReadSectors() 用 CMD18, 正常结束会发 CMD12; 但如果上一次是被
  //   异常路径中断的, 卡可能还停在"正在往外吐数据"的状态 —— 这时我们发的任何命令
  //   都会被它当成数据流的一部分(上机那个 0x20 假 R1 很可能就是这么来的)。
  //   CMD12 在空闲状态下返回 0x04(illegal command), 无害; 真卡在数据流里就把它停掉。
  (void)cmd(CMD12_STOP_TRANSMISSION, 0);
  csHigh();
  for (int i = 0; i < 8; i++) {
    s_spi->transfer(0xFF);
  }
  s_spi->endTransaction();
  return true;
}

// ---------------------------------------------------------------------------
// 扇区读
// ---------------------------------------------------------------------------

// 读 nsec 个连续扇区（nsec>=1）。整趟 CS 保持低，一条 CMD18 读完 —— 这就是
// #05b 那条 1629 KB/s 的路径。
//
// 对外的 rawRead() 带重试: 一次瞬时失败(令牌超时/命令无响应)不应该把整首歌
// 切回 39 KB/s 的慢路径。每次尝试都会自己 CMD12 + 抬 CS, 所以重试是安全的;
// 失败时 dst 内容未定义, 但调用方(rawRead 返回 false)不会用它。
bool SdFastFile::rawRead(uint32_t lba, uint32_t nsec, uint8_t *dst) {
  if (!nsec) {
    return true;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    if (rawReadOnce(lba, nsec, dst)) {
      return true;
    }
    delayMicroseconds(300);   // 给卡一点恢复时间再试
  }
  return false;
}

bool SdFastFile::rawReadOnce(uint32_t lba, uint32_t nsec, uint8_t *dst) {
  if (!nsec) {
    return true;
  }
  s_spi->beginTransaction(SPISettings(actualHz_, MSBFIRST, SPI_MODE0));
  csLow();
  for (int i = 0; i < 10; i++) {
    s_spi->transfer(0xFF);
  }

  const bool multi = (nsec > 1);
  const uint8_t theCmd = multi ? CMD18_READ_MULTI : CMD17_READ_SINGLE;
  uint8_t r = cmd(theCmd, lba);
  if (r != 0x00) {
    // 命令本身被拒(重试 3 次都没拿到 0x00) -> 不会有数据流, 直接抬 CS 收尾。
    // err_ 里带上: 命令号 + LBA + R1 + 命令后卡吐出的前 4 个原始字节。
    //   上机判读: 若 raw 全是 0xFF -> 卡根本没应答(时序/选中问题);
    //             若 raw 是 0x20 之类但 LBA=0 -> 那是残余数据被当成 R1;
    //             若 raw 确实是 0x20 且 LBA 很大 -> 才是真地址错(寻址模式/LBA 越界)。
    csHigh();
    s_spi->endTransaction();
    snprintf(err_, sizeof(err_), "读命令失败 CMD%u LBA=%lu R1=0x%02X raw=%02X %02X %02X %02X",
             (unsigned)theCmd, (unsigned long)lba, r, dbgRaw_[0], dbgRaw_[1], dbgRaw_[2], dbgRaw_[3]);
    return false;
  }

  // 中转缓冲：514 = 512 数据 + 2 字节 CRC
  //   ★ 这里**不能**多留 1 字节给令牌 ★
  //   令牌 0xFE 已经被上面的等待循环读走了(见 :208-219), 线上紧接着只剩
  //   [512 数据][2 CRC] = 514 字节。缓冲区如果写成 1+512+2=515, 下面
  //   transferBytes(..., sizeof(stage)) 就会多读 1 个字节 —— 那个字节是
  //   **下一段的第一个数据字节**, 于是下次等令牌时把数据当令牌 -> tok != 0xFE
  //   -> 整趟多扇区读失败。单扇区读看不出来(读完就抬 CS), 所以
  //   parseVolume/parseFat 能过, 而 buildClusterMap 里那条 FAT 多扇区读必失败
  //   -> open() 返回 false -> "FASTREAD 已启用" 永远不打印。这就是它的真身。
  static uint8_t stage[512 + 2];
  for (uint32_t s = 0; s < nsec; s++) {
    // 等数据令牌 0xFE
    uint32_t t0 = micros();
    uint8_t tok = 0xFF;
    while (tok == 0xFF) {
      tok = s_spi->transfer(0xFF);
      if ((uint32_t)(micros() - t0) > 250000u) {   // 250ms 超时
        if (multi) {
          cmd(CMD12_STOP_TRANSMISSION, 0);
        }
        csHigh();
        s_spi->endTransaction();
        snprintf(err_, sizeof(err_), "等令牌超时(sector %lu, LBA=%lu)", (unsigned long)s, (unsigned long)lba);
        return false;
      }
    }
    if (tok != 0xFE) {
      if (multi) {
        cmd(CMD12_STOP_TRANSMISSION, 0);
      }
      csHigh();
      s_spi->endTransaction();
      snprintf(err_, sizeof(err_), "令牌错 0x%02X", tok);
      return false;
    }
    // 一次搬 514 字节（512 数据 + 2 字节 CRC），保证下一次令牌对齐。
    // ★ 上面的等待循环已经把令牌 0xFE 读走了 ★，所以这 514 字节从**第 1 个数据
    //   字节**开始 —— 拷贝必须从 stage[0] 起。
    //   （原实现写的是 stage + 1：等于丢掉第 1 个数据字节、把 CRC 当数据，
    //     整段数据错位 1 字节 -> 解码器永远找不到帧同步字 -> 一声不出。）
    s_spi->transferBytes(nullptr, stage, sizeof(stage));
    memcpy(dst + (size_t)s * 512, stage, 512);
  }

  if (multi) {
    cmd(CMD12_STOP_TRANSMISSION, 0);
    // STOP 之后卡需要几个时钟才能释放总线
    s_spi->transfer(0xFF);
  }
  csHigh();
  s_spi->endTransaction();
  return true;
}

// ---------------------------------------------------------------------------
// FAT32 解析
// ---------------------------------------------------------------------------

// 由调用方（audio_player.cpp）注入共享的 SPI 实例
void sdFastSetSpi(SPIClass *spi) {
  s_spi = spi;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool isFat32(const uint8_t *bpb) {
  // FAT32 判定：FATSz16 == 0 且 FATSz32 != 0
  return rd16(bpb + 0x16) == 0 && rd32(bpb + 0x24) != 0;
}

bool SdFastFile::parseVolume() {
  uint8_t *mbr = secBuf_;
  if (!rawRead(0, 1, mbr)) {
    return false;
  }
  // 分区表在 0x1BE 起，4 项 x 16 字节；取第一个非空分区的起始 LBA
  for (int i = 0; i < 4; i++) {
    const uint8_t *e = mbr + 0x1BE + i * 16;
    const uint8_t type = e[4];
    if (type == 0x00) {
      continue;
    }
    const uint32_t lba = rd32(e + 8);
    if (lba) {
      partitionLba_ = lba;
      return true;
    }
  }
  // 没有分区表 -> 可能是"超级软盘"格式（直接是 BPB）
  partitionLba_ = 0;
  return true;
}

bool SdFastFile::parseFat() {
  uint8_t *bpb = secBuf_;
  uint32_t lba = partitionLba_;

  // 先按分区起始读 BPB；如果那里不是 FAT32 而 LBA 0 是，就用 LBA 0
  if (!rawRead(lba, 1, bpb)) {
    return false;
  }
  if (!isFat32(bpb) && lba != 0) {
    if (!rawRead(0, 1, bpb)) {
      return false;
    }
    if (isFat32(bpb)) {
      partitionLba_ = 0;
    }
  }
  if (!isFat32(bpb)) {
    snprintf(err_, sizeof(err_), "不是 FAT32 (FATSz16=%u FATSz32=%lu)", (unsigned)rd16(bpb + 0x16),
             (unsigned long)rd32(bpb + 0x24));
    return false;
  }

  bytesPerSec_ = rd16(bpb + 0x0B);
  secPerClus_ = bpb[0x0D];
  reservedSec_ = rd16(bpb + 0x0E);
  numFats_ = bpb[0x10];
  fatSizeSec_ = rd32(bpb + 0x24);
  rootClus_ = rd32(bpb + 0x2C);

  if (bytesPerSec_ != 512 || secPerClus_ == 0 || numFats_ == 0 || fatSizeSec_ == 0 || rootClus_ < 2 ||
      fatSizeSec_ > 65536u) {   // ★ fatSizeSec_ 直接来自 BPB: 不加上限, 损坏的 BPB 会让
                                //   `fatSizeSec_ * 512` 溢出/申请天量内存 -> 回落(安全)
    snprintf(err_, sizeof(err_), "BPB 参数不合理 bps=%u spc=%u nf=%u fatsz=%lu root=%lu", (unsigned)bytesPerSec_,
             (unsigned)secPerClus_, (unsigned)numFats_, (unsigned long)fatSizeSec_, (unsigned long)rootClus_);
    return false;
  }
  clusBytes_ = (uint32_t)bytesPerSec_ * secPerClus_;
  return true;
}

// UTF-16LE 的一个码元 -> UTF-8，返回写入字节数（0 表示结束符）
static int u16ToUtf8(uint16_t u, char *out) {
  if (u < 0x80) {
    out[0] = (char)u;
    return 1;
  }
  if (u < 0x800) {
    out[0] = (char)(0xC0 | (u >> 6));
    out[1] = (char)(0x80 | (u & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (u >> 12));
  out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
  out[2] = (char)(0x80 | (u & 0x3F));
  return 3;
}

// 短名校验和：卡用它把 LFN 项和对应的 8.3 目录项关联起来。
// 算法: 每个字节先循环右移一位, 再累加（和值本身就是校验和）。
static uint8_t lfnChecksum(const uint8_t *name11) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) {
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0x00) | (sum >> 1));
    sum = (uint8_t)(sum + name11[i]);
  }
  return sum;
}

// 在目录里找文件。两种匹配都支持:
//   ① 8.3 短名（ASCII 名）
//   ② 长文件名（LFN）—— ★ 必须支持 ★ 用户的歌名是
//      "《Weight of the World》-内置试听音乐.flac"，纯中文长名，没有可用的 8.3 名。
//
// LFN 的坑（三个都踩过）：
//   1. 在磁盘上 LFN 项是**按 seq 从大到小**排列的（最后一段在前），必须按 seq
//      把每段放回正确位置，不能简单前插/后接。
//   2. 内容是 **UTF-16LE**，不是 ASCII。直接取低字节会把中文全变成 '?'，
//      于是中文文件名永远匹配不上 —— 必须真的做 UTF-16 -> UTF-8 转换。
//   3. 每段 13 个字符，但有 1 个是 0x0000 结束符，只应写入到结束符之前。
uint32_t SdFastFile::dirLookup(uint32_t dirClus, const char *name83, const char *longName, bool *found) {
  *found = false;
  char    lfn[260];
  int     lfnLen = 0;
  uint8_t lfnSum = 0;      // 期望的校验和
  bool    hasLfn = false;
  lfn[0] = 0;

  for (uint32_t c = 0; c < 1024; c++) {   // 上限保护
    const uint32_t lba = partitionLba_ + (dirClus - 2) * secPerClus_ + reservedSec_ + numFats_ * fatSizeSec_;
    for (uint32_t s = 0; s < secPerClus_; s++) {
      if (!rawRead(lba + s, 1, secBuf_)) {
        return 0;
      }
      for (int e = 0; e < 16; e++) {
        const uint8_t *de = secBuf_ + e * 32;
        if (de[0] == 0x00) {
          return 0;   // 目录结束
        }
        if (de[0] == 0xE5) {
          hasLfn = false;
          lfnLen = 0;
          continue;   // 已删除
        }
        const uint8_t attr = de[11];
        if (attr == 0x0F) {
          // ---- LFN 项 ----
          const uint8_t seq = de[0];
          const uint8_t idx = (uint8_t)(seq & 0x3F);   // 1..20
          if (seq & 0x40) {
            // 最后一段: 从这里开始记一个新的名字
            lfnLen = 0;
            lfn[0] = 0;
            hasLfn = true;
          }
          if (!hasLfn || idx == 0) {
            continue;
          }
          const int charPos = (int)(idx - 1) * 13;
          if (charPos >= (int)sizeof(lfn) - 4) {
            hasLfn = false;
            continue;
          }
          lfnSum = de[13];   // 每条 LFN 项都带同一个校验和, 取最后一条的即可
          // 13 个 UTF-16 码元，分三段: 偏移 1(5个) / 14(6个) / 28(2个)
          const int offs[3] = {1, 14, 28};
          const int cnts[3] = {5, 6, 2};
          int w = charPos;
          for (int k = 0; k < 3; k++) {
            for (int i = 0; i < cnts[k]; i++) {
              const uint16_t u = (uint16_t)(de[offs[k] + i * 2] | (de[offs[k] + i * 2 + 1] << 8));
              if (u == 0x0000 || u == 0xFFFF) {
                continue;   // 结束符/填充，不写入
              }
              char tmp[4];
              const int n = u16ToUtf8(u, tmp);
              if (w + n < (int)sizeof(lfn) - 1) {
                memcpy(lfn + w, tmp, (size_t)n);
                w += n;
              }
            }
          }
          if (w > lfnLen) {
            lfnLen = w;
          }
          lfn[lfnLen] = 0;
          continue;
        }
        // ---- 普通目录项（8.3）----
        bool match = false;
        // ① 短名
        if (name83[0] && memcmp(de, name83, 11) == 0) {
          match = true;
        }
        // ② 长名：必须校验和一致，才认为这条 LFN 属于这个目录项
        if (!match && hasLfn && longName && longName[0] && lfnChecksum(de) == lfnSum && strcmp(lfn, longName) == 0) {
          match = true;
        }
        if (match) {
          const uint32_t clus = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
          dirFileSize_ = rd32(de + 28);   // ★ 顺带取出文件大小, buildClusterMap 要用
          *found = true;
          return clus;
        }
        hasLfn = false;
        lfnLen = 0;
        lfn[0] = 0;
      }
    }
    break;   // 只支持单簇目录（够用；否则回落）
  }
  return 0;
}

// 把路径拆成 8.3 名。只支持"ABCDEFGH.EXT"这种；太长的直接失败（调用方回落）。
static bool toName83(const char *name, char out[11]) {
  memset(out, ' ', 11);
  const char *dot = strrchr(name, '.');
  size_t baseLen = dot ? (size_t)(dot - name) : strlen(name);
  if (baseLen == 0 || baseLen > 8) {
    return false;
  }
  for (size_t i = 0; i < baseLen; i++) {
    char ch = name[i];
    if (ch >= 'a' && ch <= 'z') {
      ch = (char)(ch - 'a' + 'A');
    }
    if (ch < 0x20 || ch > 0x7E) {
      return false;   // 非 ASCII / 小写变体名（如 "~1"）-> 回落
    }
    out[i] = ch;
  }
  if (dot) {
    const char *ext = dot + 1;
    const size_t n = strlen(ext);
    if (n > 3) {
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      char ch = ext[i];
      if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
      }
      out[8 + i] = ch;
    }
  }
  return true;
}

bool SdFastFile::buildClusterMap() {
  if (!secBuf_) {
    secBuf_ = (uint8_t *)ps_malloc(512 + 4);
    if (!secBuf_) {
      secBuf_ = (uint8_t *)malloc(512 + 4);
    }
    if (!secBuf_) {
      snprintf(err_, sizeof(err_), "扇区缓冲分配失败");
      return false;
    }
  }

  // 逐级走目录：/music/<name>
  uint32_t clus = rootClus_;
  bool found = false;
  if (pathIsNested_) {
    char dir83[11];
    // 目录名也尽量试 8.3；失败不算错误 —— 交给 LFN 匹配
    if (!toName83(subDir_, dir83)) {
      memset(dir83, 0, sizeof(dir83));
    }
    clus = dirLookup(clus, dir83, subDir_, &found);
    if (!found) {
      snprintf(err_, sizeof(err_), "目录 %s 没找到, 回落", subDir_);
      return false;
    }
  }

  char file83[11];
  if (!toName83(baseName_, file83)) {
    // 纯中文长名（如 "《Weight of the World》-内置试听音乐.flac"）没有 8.3 名,
    // 这时靠 LFN 匹配。传一个不可能命中的短名即可。
    memset(file83, 0, sizeof(file83));
  }
  const uint32_t fclus = dirLookup(clus, file83, baseName_, &found);
  if (!found) {
    snprintf(err_, sizeof(err_), "文件 %s 没找到(短名/LFN 都没匹配), 回落", baseName_);
    return false;
  }

  // dirLookup() 每次调用都会更新 dirFileSize_，上面两次调用（先目录后文件）
  // 之后它保存的就是**文件**的大小 —— 正是我们需要的。这里再确认一次。
  fileSize_ = dirFileSize_;
  firstClus_ = fclus;
  if (fileSize_ == 0 || firstClus_ < 2) {
    snprintf(err_, sizeof(err_), "文件大小/首簇异常 size=%lu clus=%lu", (unsigned long)fileSize_,
             (unsigned long)firstClus_);
    return false;
  }

  clusterCount_ = (fileSize_ + clusBytes_ - 1) / clusBytes_;
  clusterMap_ = (uint32_t *)ps_malloc(sizeof(uint32_t) * clusterCount_);
  if (!clusterMap_) {
    clusterMap_ = (uint32_t *)malloc(sizeof(uint32_t) * clusterCount_);
  }
  if (!clusterMap_) {
    snprintf(err_, sizeof(err_), "簇链表分配失败(%lu 项)", (unsigned long)clusterCount_);
    return false;
  }

  // 把整条 FAT 读进 PSRAM 再走链 —— 比每步 seek 读一个 FAT 扇区快得多
  const uint32_t fatBytes = fatSizeSec_ * 512;
  uint8_t *fat = (uint8_t *)ps_malloc(fatBytes);
  if (!fat) {
    fat = (uint8_t *)malloc(fatBytes);
  }
  if (!fat) {
    snprintf(err_, sizeof(err_), "FAT 缓冲分配失败(%lu 字节)", (unsigned long)fatBytes);
    return false;
  }
  const uint32_t fatStart = partitionLba_ + reservedSec_;
  // 一趟读完整个 FAT（1.9GB 卷 / 4KB 簇约 3900 个扇区 ≈ 1.9MB, 一次性读比每步 seek 快得多。
  //   fatSizeSec_ 已在 parseFat() 里做过 65536 上限校验, 这里的乘法不会溢出。）
  if (!rawRead(fatStart, fatSizeSec_, fat)) {
    free(fat);
    return false;
  }

  uint32_t c2 = firstClus_;
  for (uint32_t i = 0; i < clusterCount_; i++) {
    if (c2 < 2 || c2 >= (fatBytes / 4)) {
      snprintf(err_, sizeof(err_), "簇链越界(第 %lu 项 = %lu)", (unsigned long)i, (unsigned long)c2);
      free(fat);
      return false;
    }
    clusterMap_[i] = partitionLba_ + (c2 - 2) * secPerClus_ + reservedSec_ + numFats_ * fatSizeSec_;
    const uint32_t next = rd32(fat + c2 * 4) & 0x0FFFFFFFu;
    if (i + 1 < clusterCount_) {
      if (next >= 0x0FFFFFF8u) {
        snprintf(err_, sizeof(err_), "簇链提前结束(第 %lu 项)", (unsigned long)i);
        free(fat);
        return false;
      }
      c2 = next;
    }
  }
  free(fat);
  return true;
}

bool SdFastFile::open(const char *path) {
  close();
  err_[0] = 0;
  if (!path || path[0] != '/') {
    snprintf(err_, sizeof(err_), "路径必须以 / 开头");
    return false;
  }
  if (!s_spi) {
    snprintf(err_, sizeof(err_), "SPI 实例未注入");
    return false;
  }

  // 拆目录/文件名。/music/xx.flac -> dir=music, base=xx.flac
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) {
    snprintf(err_, sizeof(err_), "路径没有文件名");
    return false;
  }
  snprintf(baseName_, sizeof(baseName_), "%s", slash + 1);
  if (slash != path) {
    // 取最后一级目录名
    const char *prev = slash;
    while (prev > path && prev[-1] != '/') {
      prev--;
    }
    const size_t n = (size_t)(slash - prev);
    if (n == 0 || n >= sizeof(subDir_)) {
      snprintf(err_, sizeof(err_), "目录名长度异常");
      return false;
    }
    memcpy(subDir_, prev, n);
    subDir_[n] = 0;
    pathIsNested_ = true;
  } else {
    pathIsNested_ = false;
  }

  if (!rawInit(SPI_FREQ_SD)) {
    return false;
  }
  // ★★★ 必须在 parseVolume() 之前分配 ★★★
  //   之前这一句在 buildClusterMap() 里(它排在 parseVolume/parseFat 之后),
  //   于是 parseVolume() 用 secBuf_(=nullptr) 去接 rawRead() 的数据 ->
  //   memcpy(nullptr, ...) -> StoreProhibited 异常 -> 一开音频就重启。
  //   这就是"音频完全播不出来"的直接原因之一。
  if (!secBuf_) {
    secBuf_ = (uint8_t *)ps_malloc(512 + 4);
    if (!secBuf_) {
      secBuf_ = (uint8_t *)malloc(512 + 4);
    }
    if (!secBuf_) {
      snprintf(err_, sizeof(err_), "扇区缓冲分配失败");
      return false;
    }
  }
  if (!parseVolume()) {
    return false;
  }
  if (!parseFat()) {
    return false;
  }
  if (!buildClusterMap()) {
    return false;
  }

  pos_ = 0;
  open_ = true;
  return true;
}

// ---------------------------------------------------------------------------
// 顺序读
// ---------------------------------------------------------------------------
size_t SdFastFile::read(uint8_t *dst, size_t len) {
  if (!open_ || !dst || !len) {
    return 0;
  }
  size_t done = 0;
  while (done < len && pos_ < fileSize_) {
    const uint32_t ci = pos_ / clusBytes_;
    const uint32_t inClus = pos_ % clusBytes_;
    if (ci >= clusterCount_) {
      break;
    }
    uint32_t thisLen = clusBytes_ - inClus;
    if (thisLen > (len - done)) {
      thisLen = (uint32_t)(len - done);
    }
    if (thisLen > (fileSize_ - pos_)) {
      thisLen = fileSize_ - pos_;
    }
    const uint32_t lba = clusterMap_[ci] + inClus / 512;
    const uint32_t off = inClus % 512;
    const uint32_t nsec = (off + thisLen + 511) / 512;

    // ★ 对齐判断只看"是否从扇区边界开始、是否整扇区结束" ★
    //   之前这里多写了一个 `nsec > 1`, 结果是**单扇区对齐读会掉进临时缓冲路径** ——
    //   而"每组数据的最后一块"经常正好是单扇区, 于是白白多读一次、多拷一次。
    //   修掉之后, 只有真正不对齐(文件起始不在扇区边界)的情况才走中转。
    if (off == 0 && (thisLen % 512) == 0) {
      // 整扇区对齐 -> 直接读进 dst（绝对主路径, 也是 #05b 实测 1629 KB/s 的路径）
      if (!rawRead(lba, nsec, dst + done)) {
        break;
      }
    } else {
      // 不对齐：先用临时缓冲读整段，再拷需要的部分
      static uint8_t tmp[4096 + 512];
      const uint32_t need = nsec * 512;
      if (need > sizeof(tmp)) {
        break;   // 单次不对齐读不超过 4096（本模块只被大块顺序读使用, 不会到这里）
      }
      if (!rawRead(lba, nsec, tmp)) {
        break;
      }
      memcpy(dst + done, tmp + off, thisLen);
    }
    pos_ += thisLen;
    done += thisLen;
  }
  return done;
}
