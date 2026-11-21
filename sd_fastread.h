/*
 * sd_fastread.h — 原始多扇区读（绕过 FATFS / 逐扇区循环 / CRC16 校验）
 *
 * ===========================================================================
 * 为什么需要它（实测数据，2026-10-02）
 * ===========================================================================
 * 同一张卡、同一条 SPI、同一个文件，吞吐差了 41 倍：
 *
 *   #05b  一条 CMD18 连续读 16KB（CS 全程拉低，不校验 CRC）  -> 1629 KB/s  ★
 *   [sdbg] 库路径单扇区读                                   ->  314 KB/s
 *   [audiotime] 音频路径 64KB（FATFS 走 128 次单扇区）      ->   39 KB/s
 *
 * 播放 FLAC 需要 205 KB/s，而实际只有 39 KB/s —— 所以音频是"慢放 0.32 倍 +
 * 断续"，不是解码问题（解码器只用到实时的 13.5%）。
 *
 * 慢在哪（读库源码得到的两层开销，sd_diskio.cpp）：
 *   1. ff_sd_read 每次只读 1 个扇区 -> sdReadSector 有 3 次重试循环
 *   2. sdReadSectors 虽然是 CMD18，但**每个扇区都单独调一次 sdReadBytes**，
 *      每次都要重新等数据令牌 0xFE            (:302-318)
 *   （注: 一度以为"每扇区算一遍 CRC16"是主因 —— 实测它在总耗时里只占 ~52µs(3%),
 *     量级不对, 已排除。真正的固定成本见下。）
 *
 * ★ 真正的大头（复核者定量出来，2026-10-02）★
 *   T ≈ N_calls × 104.6ms + N_sectors × 0.27ms —— 成本按 **disk_read() 调用次数**算。
 *   那 104.6ms 是 sd_diskio.cpp:205/211 的两个 `delay(100)`：sdCommand() 第一次
 *   拿不到响应(或 CRC 错)时会 deselect -> delay(100) -> reselect 重发, 重发就成功。
 *   log_w 在 ARDUHAL_LOG_LEVEL=0 下被编译掉, 所以从来没日志暴露过。
 *   拟合: read288(1 次)=104.9ms / 4KB 一簇(1 次)=106.8ms / 跨界(2 次)=206.5ms /
 *         124KB(32 次)=3285ms —— 全部对上。
 *   => 本模块(**零 disk_read**, 直接 CMD18) 绕开的就是这 104.6ms/call。
 *
 * 本模块把这三层全部去掉：一条 CMD18 读一整块、CS 全程保持低、读数据时不
 * 校验 CRC16。
 *
 * ★ 绝不动卡的状态 ★（这是上一版把音频彻底搞坏的教训）
 *   SD.begin() 已经把卡初始化好了（已识别容量、块寻址、512 字节块），并且用
 *   CMD59(arg=1) 打开了**命令** CRC 校验。所以本模块:
 *     - 不发 CMD0（那会把卡打回 idle 未初始化状态 -> 之后所有 CMD18 都超时）
 *     - 不发 CMD59（不改变卡的状态）
 *     - 每条命令都发**真 CRC7**（校验已开, 送 0x01 会被卡丢弃）
 *   即：对卡只做"读扇区"这一件事。
 *
 * ===========================================================================
 * 用法
 * ===========================================================================
 *   SdFastFile ff;
 *   if (ff.open("/music/xx.flac")) {          // 解析 MBR/BPB/FAT/目录项 + 读出簇链
 *     ff.read(buf, len);                      // 顺序读, 自动跨簇
 *     ff.close();
 *   }
 * 失败时（任何一步不满足）open() 返回 false，调用方**必须回落原有路径**。
 *
 * ===========================================================================
 * 约束
 * ===========================================================================
 * - 只支持 FAT32（FAT16 的簇链是 16 位, 逻辑不同 —— 本卡实测是 FAT 1872MiB,
 *   按 FAT32 处理）。open() 会检查 BPB 里的 FATSz32/根簇, 不满足就返回 false。
 * - 只读，不做写。不缓存 FAT，全部一次性读进 PSRAM。
 * - **调用者必须持有 sd_mutex**：本模块直接操作 SPI, 不自己加锁。
 * - 只支持"从 0 开始的顺序读"（音频/正文都是这种模式）。seek 需重新 open。
 */
// ★ 用经典 include guard 而不是 #pragma once ★
//   原因: 这个头会被两条不同路径包含 —— sketch 里的 audio_player.cpp 用
//   "sd_fastread.h"(相对路径), 而库的 Audio.h 也用 "sd_fastread.h" 但解析到的
//   是另一个路径。GCC 的 #pragma once 是**按文件路径**判重的, 两个路径会被当成
//   两个文件 -> "redefinition of class SdFastFile"。
//   include guard 按宏判重, 不受路径影响, 永远可靠。
#ifndef BOOKREADER_SD_FASTREAD_H
#define BOOKREADER_SD_FASTREAD_H

#include <Arduino.h>
#include <stdint.h>

// ===========================================================================
// 【总开关】SD_FASTREAD_ENABLE —— 默认 0(关闭)
// ===========================================================================
// 为什么默认关:
//   2026-10-02 用 sd_diskio.cpp 里的 `delay(100)` -> `delay(f == 0 ? 1 : 100)`
//   治好之后, **库路径本身**已经跑到 >1000 KB/s(实测 [audiotime] avg 1680ms -> 13ms,
//   [afeed] rt 149 -> 993/994, inbuf=655350 满)。FLAC 只需要 205 KB/s ⇒ 富余 4~5 倍,
//   这条快路径已是锦上添花, 不必再投入。
//   上机结论(2026-10-03): 快路径握手始终不成功, 诊断行是
//       FASTREAD 未启用(回落库路径): 读命令失败 CMD17 LBA=32 R1=0x20 raw=FF 20 00 00
//   `raw=FF ...` 说明卡在 128 次轮询内**根本没应答**, 后面几个字节是流上的数据;
//   音频的收益 100% 来自上面那两行 delay 补丁, 与本模块无关。
//
//   默认关掉还能**回收 Flash**(整个模块的代码+字符串会被 --gc-sections 丢掉) ——
//   在 91% 占用的固件上很有价值。
//
// 想恢复: 把下面的 0 改成 1 重编即可, 代码与诊断完整保留。
//   恢复后留意 [audiolib/info] "FASTREAD 已启用 / 未启用(回落库路径): <原因>"。
#ifndef SD_FASTREAD_ENABLE
#define SD_FASTREAD_ENABLE 0
#endif

class SPIClass;

// 注入共享的 SPI 实例。必须和 SD 库 / TFT 用同一个（见 display.cpp 的说明）。
// 由 audio_player.cpp 在初始化时调一次。
void sdFastSetSpi(SPIClass *spi);

class SdFastFile {
 public:
  SdFastFile() = default;
  ~SdFastFile();

  // 解析路径并建立簇链。成功返回 true。失败会把 err_ 填上原因。
  bool open(const char *path);

  // 从当前读位置顺序读出最多 len 字节，返回实际读到的字节数。
  size_t read(uint8_t *dst, size_t len);

  // 当前读位置 / 文件大小
  uint32_t position() const { return pos_; }
  uint32_t size() const { return fileSize_; }

  bool isOpen() const { return open_; }
  const char *err() const { return err_; }

  // 打开后有效：本次实际用的有效 SPI 频率(Hz)
  uint32_t actualHz() const { return actualHz_; }

  void close();

 private:
  bool     parseVolume();      // MBR -> 分区起始扇区
  bool     parseFat();         // BPB -> 扇区/簇/FAT 参数
  bool     buildClusterMap();  // 走目录项 + 读整条 FAT，建 LBA 表
  uint32_t dirLookup(uint32_t dirClus, const char *name8_3, const char *longName, bool *found);

  // --- 原始 SPI 事务（调用者持 sd_mutex）---
  bool     rawInit(uint32_t hz);
  bool     rawRead(uint32_t lba, uint32_t nsec, uint8_t *dst);   // 带重试(2 次)
  bool     rawReadOnce(uint32_t lba, uint32_t nsec, uint8_t *dst);
  uint8_t  cmd(uint8_t c, uint32_t arg);
  void     csLow();
  void     csHigh();

  char       err_[64] = {0};
  bool       open_ = false;

  // 卷参数
  uint32_t partitionLba_ = 0;
  uint16_t bytesPerSec_ = 512;
  uint8_t  secPerClus_ = 1;
  uint32_t reservedSec_ = 0;
  uint8_t  numFats_ = 1;
  uint32_t fatSizeSec_ = 0;
  uint32_t rootClus_ = 0;
  uint32_t clusBytes_ = 4096;

  // 文件
  uint32_t fileSize_ = 0;
  uint32_t firstClus_ = 0;
  uint32_t pos_ = 0;

  // 簇链：clusterMap_[i] = 文件第 i 个簇的绝对 LBA（已含分区偏移）
  uint32_t *clusterMap_ = nullptr;
  uint32_t  clusterCount_ = 0;

  // 单扇区对齐缓冲（处理非 512 对齐的读与尾部）
  uint8_t *secBuf_ = nullptr;

  // 路径拆分结果
  char     baseName_[128] = {0};
  char     subDir_[64] = {0};
  bool     pathIsNested_ = false;

  // dirLookup() 顺带带出来的文件大小
  uint32_t dirFileSize_ = 0;

  // 【诊断】最近一次 cmd() 收到的原始字节(前 4 个)与最后一个 R1。
  //   用途: 区分"真 R1"和"上一次事务的残余数据被当成 R1" —— 上机实测拿到的
  //   0x20(= address error)在读 LBA 0 时**不可能**是真错误, 所以它八成是残余字节。
  uint8_t  dbgRaw_[4] = {0};
  uint8_t  dbgRawN_ = 0;
  uint8_t  dbgLastR1_ = 0xFF;

  uint32_t actualHz_ = 0;
};

#endif  // BOOKREADER_SD_FASTREAD_H
