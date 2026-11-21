/*
 * sd_bench.cpp — SD 通路自检 v3（一次性诊断）
 *
 * 和前两版的区别
 * --------------
 * v1/v2 由 `displayInit()` 末尾触发(display.cpp:422)。上机日志里一条 [sdbench] 都没有,
 *   而源码里那处调用是存在的 —— 也就是说"触发点"这件事本身不可靠。
 * v3 把触发点搬到**驱动内部**: `sd_diskio.cpp` 的 `ff_sd_ioctl()` 在卡初始化完成、
 *   且没有 SPI 事务在跑的时候, 通过**弱符号**回调这里的 `sdBenchRun()`。
 *   即使 sketch 侧 display.cpp 的钩子丢了/被别的队友改掉, 自检照样会跑。
 *
 * 同时 v3 **只做"不碰 FATFS"的原始测量**: 不调 SD.begin/end/open。原因是它现在跑在
 *   `f_mount()` 过程中, 重入 FATFS 会破坏文件系统状态。FATFS 层的数字(512B=105ms、
 *   4KB=106ms、128KB=3.28s)上一版已经拿到了, 不需要重测。
 *
 * v3 要回答的核心问题(问题②/④):
 *   #02 SPI 裸测(CS 高, 完全不碰卡): 512B/4096B 各多少 us + 分频寄存器
 *   #03 同一裸测在 1/4/10/25/40MHz 下各多少 us + 各档分频寄存器
 *       -> 若"请求频率变了, 耗时/分频都不变" => SPIClass/HAL 根本没把频率写进去
 *       -> 若耗时随频率变化           => SPI 侧没问题, 慢在 SD 协议/卡侧
 *   #04 自制 CMD17 读 0 号扇区(带真 CRC7): 逐阶段 csClk/wait/cmd/token/data + 校验 0x55AA
 *   #05 自制 CMD18 x32 扇区(整趟 CS 低) -> 真实可达吞吐
 *   #06/#07 自制 FAT 走查 + CMD18 直读字库前 32KB -> 修复原型的实际吞吐
 *
 * 另外驱动里会打 [sdbg] 行: 把**真实数据读**分成 tok/dat/oth 三段, 并带上请求频率与
 *   实际分频寄存器。和这里的原始数字对照, 就能把"每 512B 约 105ms"钉到具体某一段。
 *
 * CRC7 说明: 库里初始化时用 CMD59 打开了卡的 CRC 校验(sd_diskio.cpp:542), 所以自制命令
 *   必须发真 CRC7(CMD17 arg0 -> 0x55)。这里的位运算实现已和库里的 m_CRC7Table 逐包核对一致。
 */
#include "sd_bench.h"

#include "config.h"
#include "display.h"

#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <strings.h>

#include "sd_mutex.h"

// 短行 + flush + delay(30): USB CDC 上 flush 不代表对端已收, 必须留排空时间
#define SBLOG(fmt, ...)                                  \
  do {                                                   \
    Serial.printf("[sdbench] " fmt "\n", ##__VA_ARGS__); \
    Serial.flush();                                      \
    delay(30);                                           \
  } while (0)

static SPIClass *s_bus = nullptr;
static bool s_ran = false;
static bool s_sdhc = false;
static uint8_t *s_buf = nullptr;   // 128KB
static char s_fontPath[192] = {0}; // #07 走查出来的字库完整路径, 供 #09 复用

// ---------------------------------------------------------------------------
// 原始 SPI 层
// ---------------------------------------------------------------------------
static inline void sbCsLow() {
  digitalWrite(PIN_SD_CS, LOW);
}
static inline void sbCsHigh() {
  digitalWrite(PIN_SD_CS, HIGH);
}

static uint8_t sbCrc7(const uint8_t *d, int n) {
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

static uint8_t sbCmd(uint8_t cmd, uint32_t arg) {
  uint8_t pkt[6] = {(uint8_t)(cmd | 0x40), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8),
                    (uint8_t)arg, 0};
  pkt[5] = (uint8_t)((sbCrc7(pkt, 5) << 1) | 0x01);
  s_bus->transferBytes(pkt, nullptr, 6);
  uint8_t r = 0xFF;
  for (int i = 0; i < 32; i++) {
    r = s_bus->transfer(0xFF);
    if (!(r & 0x80)) {
      break;
    }
  }
  return r;
}

struct RawTiming {
  uint32_t csClk, wait, cmd, token, data, tail, total;
};

static bool sbReadTimed(uint32_t lba, uint16_t n, uint8_t *dst, RawTiming *tm) {
  RawTiming t = {};
  const uint32_t addr = s_sdhc ? lba : (lba << 9);
  uint32_t a = micros();
  sbCsLow();
  for (int i = 0; i < 10; i++) {
    s_bus->transfer(0xFF);
  }
  uint32_t b = micros();

  uint8_t r = 0x00;
  do {
    r = s_bus->transfer(0xFF);
  } while (r == 0x00 && (uint32_t)(micros() - b) < 200000);
  uint32_t c = micros();

  const bool cmdOk = (sbCmd(n == 1 ? 17 : 18, addr) == 0x00);
  uint32_t d = micros();

  bool ok = false;
  if (cmdOk) {
    uint8_t tok = 0xFF;
    const uint32_t tk0 = micros();
    while ((uint32_t)(micros() - tk0) < 200000) {
      tok = s_bus->transfer(0xFF);
      if (tok != 0xFF) {
        break;
      }
    }
    uint32_t e = micros();
    if (tok == 0xFE) {
      for (uint16_t i = 0; i < n; i++) {
        s_bus->transferBytes(nullptr, dst + (size_t)i * 512, 512);
        s_bus->transfer(0xFF);
        s_bus->transfer(0xFF);
      }
      ok = true;
    }
    uint32_t f = micros();
    if (n > 1 && ok) {
      sbCmd(12, 0);
      s_bus->transfer(0xFF);
    }
    uint32_t g = micros();
    sbCsHigh();
    s_bus->transfer(0xFF);
    uint32_t h = micros();
    t.csClk = b - a;
    t.wait = c - b;
    t.cmd = d - c;
    t.token = e - d;
    t.data = f - e;
    t.tail = (g - f) + (h - g);
    t.total = h - a;
  } else {
    sbCsHigh();
    s_bus->transfer(0xFF);
    t.csClk = b - a;
    t.wait = c - b;
    t.cmd = d - c;
    t.total = micros() - a;
  }
  if (tm) {
    *tm = t;
  }
  return ok;
}

static bool sbRawAt(uint32_t hz, uint32_t lba, uint16_t n, uint8_t *dst, RawTiming *tm) {
  s_bus->beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  const bool ok = sbReadTimed(lba, n, dst, tm);
  s_bus->endTransaction();
  return ok;
}

// SPI 裸测: CS 保持高, 完全不碰卡, 只让外设空转 -> 直接量"位时钟"
static void sbSpiPure(uint32_t hz, uint32_t *divOut, uint32_t *b512Us, uint32_t *b4096Us) {
  s_bus->beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  if (divOut) {
    *divOut = s_bus->getClockDivider();
  }
  uint32_t dt512 = 0, dt4096 = 0;
  if (b512Us) {
    const uint32_t t0 = micros();
    s_bus->transferBytes(nullptr, s_buf, 512);
    dt512 = micros() - t0;
  }
  if (b4096Us) {
    const uint32_t t0 = micros();
    s_bus->transferBytes(nullptr, s_buf, 4096);
    dt4096 = micros() - t0;
  }
  s_bus->endTransaction();
  if (b512Us) {
    *b512Us = dt512;
  }
  if (b4096Us) {
    *b4096Us = dt4096;
  }
}

static uint32_t kbPerSec(uint32_t bytes, uint32_t us) {
  return us ? (uint32_t)(((uint64_t)bytes * 1000) / us) : 0;
}

// ---------------------------------------------------------------------------
// 自制只读 FAT 走查(纯原始读, 不用 FATFS)
// ---------------------------------------------------------------------------
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct {
  uint16_t bps;
  uint8_t spc;
  uint16_t rsvd;
  uint8_t nfat;
  uint16_t rootEnt;
  uint32_t fatSectors;
  uint32_t rootClus;
  uint32_t rootDirStart;
  uint32_t firstDataSector;
  uint32_t clusterBytes;
  bool isFat32;
  bool valid;
} s_fat = {};
static uint8_t s_fatSec[512];
static uint32_t s_fatSecLba = 0xFFFFFFFFu;

static bool sbParseFat(void) {
  RawTiming tm;
  if (!sbRawAt(SPI_FREQ_SD, 0, 1, s_buf, &tm)) {
    return false;
  }
  const uint8_t *b = s_buf;
  if (!(b[510] == 0x55 && b[511] == 0xAA)) {
    return false;
  }
  s_fat.bps = rd16(b + 11);
  s_fat.spc = b[13];
  s_fat.rsvd = rd16(b + 14);
  s_fat.nfat = b[16];
  s_fat.rootEnt = rd16(b + 17);
  s_fat.fatSectors = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
  s_fat.rootClus = rd32(b + 44);
  if (s_fat.bps != 512 || s_fat.spc == 0 || s_fat.nfat == 0 || s_fat.fatSectors == 0) {
    return false;
  }
  s_fat.isFat32 = (s_fat.rootEnt == 0);
  s_fat.clusterBytes = (uint32_t)s_fat.bps * s_fat.spc;
  const uint32_t rootDirSectors = ((uint32_t)s_fat.rootEnt * 32 + s_fat.bps - 1) / s_fat.bps;
  s_fat.rootDirStart = s_fat.rsvd + (uint32_t)s_fat.nfat * s_fat.fatSectors;
  s_fat.firstDataSector = s_fat.rootDirStart + (s_fat.isFat32 ? 0 : rootDirSectors);
  s_fat.valid = true;
  return true;
}

static uint32_t sbNextCluster(uint32_t clus) {
  const uint32_t entSize = s_fat.isFat32 ? 4 : 2;
  const uint32_t off = clus * entSize;
  const uint32_t lba = s_fat.rsvd + off / s_fat.bps;
  const uint32_t inSec = off % s_fat.bps;
  if (lba != s_fatSecLba) {
    RawTiming tm;
    if (!sbRawAt(SPI_FREQ_SD, lba, 1, s_fatSec, &tm)) {
      return 0xFFFFFFFFu;
    }
    s_fatSecLba = lba;
  }
  if (s_fat.isFat32) {
    return rd32(s_fatSec + inSec) & 0x0FFFFFFFu;
  }
  return rd16(s_fatSec + inSec);
}

struct Run {
  uint32_t lba;
  uint32_t count;
};
static uint32_t sbChainRuns(uint32_t firstClus, Run *runs, uint32_t maxRuns, uint32_t *outClusters) {
  uint32_t n = 0, nClus = 0;
  uint32_t clus = firstClus;
  while (clus >= 2 && clus != 0xFFFFFFFFu && nClus < 300000u) {
    const uint32_t lba = s_fat.firstDataSector + (clus - 2) * s_fat.spc;
    if (n && runs[n - 1].lba + runs[n - 1].count == lba) {
      runs[n - 1].count += s_fat.spc;
    } else if (n < maxRuns) {
      runs[n].lba = lba;
      runs[n].count = s_fat.spc;
      n++;
    } else {
      break;
    }
    nClus++;
    const uint32_t next = sbNextCluster(clus);
    if (s_fat.isFat32 ? (next >= 0x0FFFFFF8u) : (next >= 0xFFF8u)) {
      break;
    }
    clus = next;
  }
  if (outClusters) {
    *outClusters = nClus;
  }
  return n;
}

static bool sbScanDir(const uint8_t *data, uint32_t len, const char *want, bool byExt, uint32_t *outClus,
                      uint32_t *outSize, char *outName, size_t outNameLen) {
  char lfn[300];
  int lfnParts = 0;
  for (uint32_t p = 0; p + 32 <= len; p += 32) {
    const uint8_t *e = data + p;
    if (e[0] == 0x00) {
      break;
    }
    if (e[0] == 0xE5) {
      lfnParts = 0;
      continue;
    }
    if ((e[11] & 0x0F) == 0x0F) {
      if (lfnParts < 16) {
        static const uint8_t offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        char *dst = lfn + (size_t)lfnParts * 13;
        for (int i = 0; i < 13; i++) {
          const uint16_t c = (uint16_t)(e[offs[i]] | (e[offs[i] + 1] << 8));
          dst[i] = (c < 128) ? (char)c : '?';
        }
        lfnParts++;
      }
      continue;
    }
    char name[300];
    if (lfnParts > 0) {
      int k = 0;
      for (int i = lfnParts - 1; i >= 0 && k < (int)sizeof(name) - 1; i--) {
        for (int j = 0; j < 13 && k < (int)sizeof(name) - 1; j++) {
          const char c = lfn[(size_t)i * 13 + j];
          if (c == 0) {
            break;
          }
          name[k++] = c;
        }
      }
      name[k] = 0;
    } else {
      int k = 0;
      for (int i = 0; i < 8; i++) {
        if (e[i] != ' ') {
          name[k++] = (char)e[i];
        }
      }
      name[k++] = '.';
      for (int i = 8; i < 11; i++) {
        if (e[i] != ' ') {
          name[k++] = (char)e[i];
        }
      }
      name[k] = 0;
    }
    lfnParts = 0;
    const size_t wl = strlen(want);
    const size_t nl = strlen(name);
    const bool hit = byExt ? (nl > wl && strcasecmp(name + nl - wl, want) == 0) : (strcasecmp(name, want) == 0);
    if (hit) {
      *outClus = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
      *outSize = rd32(e + 28);
      if (outName && outNameLen) {
        snprintf(outName, outNameLen, "%s", name);
      }
      return true;
    }
  }
  return false;
}

static bool sbFindInDir(uint32_t dirClus, bool isRoot, const char *want, bool byExt, uint32_t *outClus,
                        uint32_t *outSize, char *outName, size_t outNameLen) {
  Run runs[64];
  uint32_t nClus = 0, nRuns = 0;
  if (isRoot && !s_fat.isFat32) {
    runs[0].lba = s_fat.rootDirStart;
    runs[0].count = (uint32_t)((s_fat.rootEnt * 32 + 511) / 512);
    nRuns = 1;
  } else {
    nRuns = sbChainRuns(dirClus, runs, 64, &nClus);
  }
  for (uint32_t r = 0; r < nRuns; r++) {
    uint32_t left = runs[r].count;
    uint32_t lba = runs[r].lba;
    while (left) {
      const uint32_t n = (left > 32) ? 32 : left;
      RawTiming tm;
      if (!sbRawAt(SPI_FREQ_SD, lba, (uint16_t)n, s_buf, &tm)) {
        return false;
      }
      if (sbScanDir(s_buf, n * 512, want, byExt, outClus, outSize, outName, outNameLen)) {
        return true;
      }
      lba += n;
      left -= n;
    }
  }
  return false;
}

static uint32_t sbRawReadFile(uint32_t firstClus, uint32_t maxBytes, uint8_t *dst, uint32_t *outRuns,
                             uint32_t *outCalls) {
  const uint32_t maxRuns = 4096;
  Run *runs = (Run *)ps_malloc(sizeof(Run) * maxRuns);
  if (!runs) {
    runs = (Run *)malloc(sizeof(Run) * maxRuns);
  }
  if (!runs) {
    return 0;
  }
  uint32_t nClus = 0;
  const uint32_t nRuns = sbChainRuns(firstClus, runs, maxRuns, &nClus);
  uint32_t total = 0, calls = 0;
  bool failed = false;
  for (uint32_t r = 0; r < nRuns && total < maxBytes && !failed; r++) {
    uint32_t lba = runs[r].lba;
    uint32_t left = runs[r].count;
    while (left && total < maxBytes) {
      uint32_t n = left;
      const uint32_t canTake = (maxBytes - total) / 512;
      if (n > canTake) {
        n = canTake;
      }
      if (n > 128) {
        n = 128;
      }
      if (n == 0) {
        break;
      }
      RawTiming tm;
      if (!sbRawAt(SPI_FREQ_SD, lba, (uint16_t)n, dst + total, &tm)) {
        failed = true;
        break;
      }
      calls++;
      total += n * 512;
      lba += n;
      left -= n;
    }
  }
  free(runs);
  if (outRuns) {
    *outRuns = nRuns;
  }
  if (outCalls) {
    *outCalls = calls;
  }
  return total;
}

// ---------------------------------------------------------------------------
// 主入口: sdBenchRunEarly() —— 在 SD 卡挂载成功之后调用一次(纯 sketch 侧, 不依赖核心库补丁)
// ---------------------------------------------------------------------------
bool sdBenchHasRun(void) {
  return s_ran;
}

void sdBenchRun(void) {   // 旧名字(display.cpp 的老调用点)保留为别名
  sdBenchRunEarly();
}

void sdBenchRunEarly(void) {
  if (s_ran) {
    return;
  }
  Serial.println();

  // 卡还没挂载: 只报一句, 不消耗"只跑一次"的标记(挂载成功后可以再调一次)
  if (SD.cardType() == CARD_NONE) {
    SBLOG("#00 忽略: SD 卡还没挂载(请在 SD.begin 成功之后再调 sdBenchRunEarly)");
    return;
  }

  // 独占共享 SPI 总线(挂载方通常正持着这把锁; 它是递归锁, 同一任务可重入)
  SdLockGuard lock(SD_LOCK_TIMEOUT_MS);
  if (!lock.ok()) {
    SBLOG("#00 !! 等 sd_mutex 超时, 自检放弃");
    return;
  }

  s_bus = displaySpiInstance();
  if (!s_bus) {
    SBLOG("#00 !! displaySpiInstance() null");
    return;
  }
  s_buf = (uint8_t *)ps_malloc(128 * 1024);
  if (!s_buf) {
    s_buf = (uint8_t *)malloc(128 * 1024);
  }
  if (!s_buf) {
    SBLOG("#00 !! buf alloc fail");
    return;
  }
  s_ran = true;

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);

  const uint8_t cardType = (uint8_t)SD.cardType();
  const uint32_t sectors = (uint32_t)SD.numSectors();
  const uint32_t ssz = (uint32_t)SD.sectorSize();
  s_sdhc = (cardType == CARD_SDHC) || (sectors > 0x400000);
  SBLOG("#01 card=%u sec=%lu ssz=%lu sdhc=%d req=%luHz", (unsigned)cardType, (unsigned long)sectors,
        (unsigned long)ssz, s_sdhc ? 1 : 0, (unsigned long)SPI_FREQ_SD);

  // #02 SPI 裸测(CS 高, 不碰卡)
  uint32_t div25 = 0, spi512 = 0, spi4096 = 0;
  sbSpiPure(SPI_FREQ_SD, &div25, &spi512, &spi4096);
  SBLOG("#02 spi_pure b512=%luus b4096=%luus div=%lu (CS高,与卡无关)", (unsigned long)spi512,
        (unsigned long)spi4096, (unsigned long)div25);

  // #03 同一裸测在不同请求频率下: 耗时和分频是否跟着变
  {
    const uint32_t freqs[5] = {1000000, 4000000, 10000000, 25000000, 40000000};
    uint32_t us[5] = {0}, dv[5] = {0};
    for (int i = 0; i < 5; i++) {
      uint32_t d = 0, u = 0;
      sbSpiPure(freqs[i], &d, &u, nullptr);
      us[i] = u;
      dv[i] = d;
    }
    SBLOG("#03 spi_pure_scan512B: 1M=%luus(d%lu) 4M=%luus(d%lu) 10M=%luus(d%lu) 25M=%luus(d%lu) 40M=%luus(d%lu)",
          (unsigned long)us[0], (unsigned long)dv[0], (unsigned long)us[1], (unsigned long)dv[1],
          (unsigned long)us[2], (unsigned long)dv[2], (unsigned long)us[3], (unsigned long)dv[3],
          (unsigned long)us[4], (unsigned long)dv[4]);
  }

  // #04 自制 CMD17 读 0 号扇区: 逐阶段 + 数据校验
  {
    RawTiming t = {};
    const bool ok = sbRawAt(SPI_FREQ_SD, 0, 1, s_buf, &t);
    SBLOG("#04 raw17 ok=%d sig=%02X%02X csClk=%lu wait=%lu cmd=%lu token=%lu data=%lu tail=%lu tot=%lu",
          ok ? 1 : 0, s_buf[510], s_buf[511], (unsigned long)t.csClk, (unsigned long)t.wait, (unsigned long)t.cmd,
          (unsigned long)t.token, (unsigned long)t.data, (unsigned long)t.tail, (unsigned long)t.total);
  }

  // #05 自制 CMD18: x8 扇区(4KB, 与 FATFS 一次簇读同尺寸) 和 x32 扇区(16KB)
  {
    RawTiming t8 = {}, t32 = {};
    const bool o8 = sbRawAt(SPI_FREQ_SD, 4500, 8, s_buf, &t8);
    const bool o32 = sbRawAt(SPI_FREQ_SD, 5000, 32, s_buf, &t32);
    SBLOG("#05 raw18x8 ok=%d tot=%luus %luKBps csClk=%lu wait=%lu cmd=%lu token=%lu data=%lu tail=%lu", o8 ? 1 : 0,
          (unsigned long)t8.total, (unsigned long)kbPerSec(8 * 512, t8.total), (unsigned long)t8.csClk,
          (unsigned long)t8.wait, (unsigned long)t8.cmd, (unsigned long)t8.token, (unsigned long)t8.data,
          (unsigned long)t8.tail);
    SBLOG("#05b raw18x32 ok=%d tot=%luus %luKBps data=%luus", o32 ? 1 : 0, (unsigned long)t32.total,
          (unsigned long)kbPerSec(32 * 512, t32.total), (unsigned long)t32.data);
  }

  // #06/#07 修复原型: 自制 FAT 走查 + 原始 CMD18 直读字库文件前 32KB
  if (sbParseFat()) {
    SBLOG("#06 bpb bps=%u spc=%u clus=%lu fat32=%d rootClus=%lu", (unsigned)s_fat.bps, (unsigned)s_fat.spc,
          (unsigned long)s_fat.clusterBytes, s_fat.isFat32 ? 1 : 0, (unsigned long)s_fat.rootClus);
    uint32_t dirClus = 0, dirSize = 0;
    char dirName[64] = {0};
    uint32_t fClus = 0, fSize = 0;
    char fName[128] = {0};
    const uint32_t rootClus = s_fat.isFat32 ? s_fat.rootClus : 0;
    if (sbFindInDir(rootClus, true, "fonts", false, &dirClus, &dirSize, dirName, sizeof(dirName)) &&
        sbFindInDir(dirClus, false, ".f24", true, &fClus, &fSize, fName, sizeof(fName))) {
      const uint32_t K = 32 * 1024;
      uint8_t *big = (uint8_t *)ps_malloc(K);
      if (!big) {
        big = (uint8_t *)malloc(K);
      }
      if (big) {
        const uint32_t t0 = micros();
        uint32_t runs = 0, calls = 0;
        const uint32_t got = sbRawReadFile(fClus, K, big, &runs, &calls);
        const uint32_t dt = micros() - t0;
        SBLOG("#07 raw_font %s size=%lu got=%luB %luus %luKBps runs=%lu calls=%lu", fName, (unsigned long)fSize,
              (unsigned long)got, (unsigned long)dt, (unsigned long)kbPerSec(got, dt), (unsigned long)runs,
              (unsigned long)calls);
        // #08 同一个文件、同样 4KB: 走库路径(FATFS) 做直接对比 —— 这就是"修复前后"的对照
        char fpath[160];
        snprintf(fpath, sizeof(fpath), "/fonts/%s", fName);
        // 供后面的 #09 使用(fName 在 #07 的块作用域里, 出了块就没了)
        snprintf(s_fontPath, sizeof(s_fontPath), "%s", fpath);
        File ff = SD.open(fpath, FILE_READ);
        if (ff) {
          const uint32_t tf = micros();
          const size_t gotF = ff.read(big, 4096);
          const uint32_t dtf = micros() - tf;
          ff.close();
          SBLOG("#08 fatfs4KB %luB %luus %luKBps (同一文件, 库路径)", (unsigned long)gotF, (unsigned long)dtf,
                (unsigned long)kbPerSec((uint32_t)gotF, dtf));
        } else {
          SBLOG("#08 fatfs4KB: 打不开 %s", fpath);
        }
        free(big);
      }
    } else {
      SBLOG("#07 raw_font: /fonts/*.f24 没找到(目录走查失败)");
    }
  } else {
    SBLOG("#06 bpb 解析失败(自制 CMD17 没读到合法引导扇区)");
  }

  // ---------------------------------------------------------------------------
  // ★ #09 判决性测量: 用【库路径】连续读一个大文件 ★
  //
  // 为什么必须有这一项: 现在有两组互相矛盾的实测数字 ——
  //   (a) 本自检的裸 CMD17/CMD18: 512B = 1.6ms, 16KB 连续读 = 1629 KB/s
  //   (b) 上面 [sdbg] 抓到的真实库路径读: tot=1626us/tok=1334/dat=240  -> 314 KB/s
  //   (c) **但** font.cpp 的 sdSequentialProbe() 一直报告 4KB = 106ms (38 KB/s)
  // 同样的卡、同样的驱动、同一个操作, (a)(b) 与 (c) 差了 65 倍。
  //
  // "38 KB/s" 是**所有架构决策的依据**: 它导致了"字库必须瘦身/塞进 Flash"这条路线。
  // 如果它其实是个测量错误, 那我们白白烧了 1.1MB Flash, 而且本来可以直接
  // 把整份 6.4MB 字库读进 PSRAM 按需用。
  //
  // 所以这里用**完全相同的库调用方式**去读一个已知大小的大文件, 一次定胜负:
  //   256KB 用 1~3 秒读完  -> 真实吞吐 >= 100KB/s, (c) 是错的
  //   256KB 用 80 秒读完   -> (c) 是对的, 38KB/s 是硬约束, 嵌入 Flash 必须保留
  //
  // 选文件: 用上面 #07 走查出来的字库路径(6.4MB, 最能说明问题)。
  //   s_fontPath 在 #07 成功时被填上("/fonts/xxx.f24"); 走查失败则为空, 那就跳过。
  {
    const char *gp = s_fontPath;
    if (gp[0]) {      const uint32_t K = 256 * 1024;
      uint8_t *big = (uint8_t *)ps_malloc(K);
      if (!big) {
        big = (uint8_t *)malloc(K);
      }
      if (big) {
        File gf = SD.open(gp, FILE_READ);
        if (gf) {
          const uint32_t t0 = micros();
          const size_t got = gf.read(big, K);
          const uint32_t dt = micros() - t0;
          gf.close();
          SBLOG("#09 bigread %s want=%luB got=%luB %luus %luKBps", gp, (unsigned long)K, (unsigned long)got,
                (unsigned long)dt, (unsigned long)kbPerSec((uint32_t)got, dt));
          // 逐块读: 每块 64KB, 看块之间是否稳定(排除"第一块慢"或"某一块卡住")
          File g2 = SD.open(gp, FILE_READ);
          if (g2) {
            uint32_t tot = 0;
            int okN = 0;
            for (int i = 0; i < 4; i++) {
              const uint32_t tb = micros();
              const size_t gb = g2.read(big, 64 * 1024);
              const uint32_t db = micros() - tb;
              tot += db;
              if (gb > 0) {
                okN++;
              }
              SBLOG("#09b blk%d %luB %luus %luKBps", i, (unsigned long)gb, (unsigned long)db,
                    (unsigned long)kbPerSec((uint32_t)gb, db));
            }
            g2.close();
            SBLOG("#09c 4x64KB total=%luus avg=%luKBps", (unsigned long)tot,
                  (unsigned long)kbPerSec(4 * 64 * 1024, tot));
          }
        } else {
          SBLOG("#09 bigread: 打不开 %s", gp);
        }
        free(big);
      } else {
        SBLOG("#09 bigread: 256KB 缓冲分配失败");
      }
    } else {
      SBLOG("#09 bigread: 没有可读的大文件, 跳过");
    }
  }

  // ---------------------------------------------------------------------------
  // ★ #10 音频路径测量: 直接读那个 FLAC 文件 ★
  //
  // 为什么单独测它: 播 FLAC 时实测 [afeed] busy=1679ms/次、rt=135(只有实时的 13.5%)、
  // 播放速度 0.32 倍(76 秒墙上时间只推进 24 秒音频)。库每个 loop() 读一次
  // min(writeSpace, 65535) 字节, 所以要么"读 64KB 花了 1.67 秒"(SD 读慢),
  // 要么"读很快, 时间花在解码"(CPU 问题)。
  // 这里用【库路径】读 /music 下最大的那个文件, 给出一手吞吐数字。
  // ---------------------------------------------------------------------------
  if (sbParseFat()) {
    const uint32_t rootClus = s_fat.isFat32 ? s_fat.rootClus : 0;
    uint32_t mClus = 0, mSize = 0;
    char mName[128] = {0};
    if (sbFindInDir(rootClus, true, "music", false, &mClus, &mSize, mName, sizeof(mName))) {
      // 在 /music 里找 .flac(按扩展名)
      uint32_t fClus = 0, fSize = 0;
      char fName2[128] = {0};
      const bool found = sbFindInDir(mClus, false, ".flac", true, &fClus, &fSize, fName2, sizeof(fName2));
      SBLOG("#10 music dir clus=%lu size=%lu <- %s", (unsigned long)mClus, (unsigned long)mSize, mName);
      if (found) {
        char mp[192];
        snprintf(mp, sizeof(mp), "/music/%s", fName2);
        const uint32_t K = 256 * 1024;
        uint8_t *mb = (uint8_t *)ps_malloc(K);
        if (!mb) {
          mb = (uint8_t *)malloc(K);
        }
        if (mb) {
          File mf = SD.open(mp, FILE_READ);
          if (mf) {
            const uint32_t t0 = micros();
            const size_t got = mf.read(mb, K);
            const uint32_t dt = micros() - t0;
            mf.close();
            SBLOG("#10 flac256K %s want=%luB got=%luB %luus %luKBps", fName2, (unsigned long)K,
                  (unsigned long)got, (unsigned long)dt, (unsigned long)kbPerSec((uint32_t)got, dt));
          } else {
            SBLOG("#10 flac: 打不开 %s", mp);
          }
          free(mb);
        } else {
          SBLOG("#10 flac: 256KB 缓冲分配失败");
        }
      } else {
        SBLOG("#10 /music 里没找到 .flac(走查失败)");
      }
    } else {
      SBLOG("#10 没有 /music 目录");
    }
  }

  // ---------------------------------------------------------------------------
  // ★ #11 读块大小扫描 ★ —— 这次定版最需要的一个数
  //
  // 已知两个互相矛盾的数(同一个文件、同一条库路径):
  //   字库路径读 512 字节/次  -> [sdbg] tot=1626us  -> **314 KB/s**
  //   音频路径读 64KB/次      -> [afeed] busy=1679ms/次, 每循环一块 -> **39 KB/s**
  // 唯一的结构差别就是**一次读多少字节**。所以直接扫一遍块大小, 看吞吐怎么变:
  //   吞吐随块变大而**下降**  -> 大块读有额外代价(缓存/簇链/重试), 修法=让上层小块读
  //   吞吐**基本不变**        -> 与块大小无关, 那是通路本身的性质
  // 这一项能直接决定音频该怎么改。
  // ---------------------------------------------------------------------------
  if (s_fontPath[0]) {
    static const size_t KS[5] = {512, 4096, 16384, 65536, 262144};
    uint8_t *kbuf = (uint8_t *)ps_malloc(262144);
    if (!kbuf) {
      kbuf = (uint8_t *)malloc(262144);
    }
    if (kbuf) {
      for (int i = 0; i < 5; i++) {
        File f2 = SD.open(s_fontPath, FILE_READ);
        if (!f2) {
          SBLOG("#11 chunk%luKB: 打不开", (unsigned long)(KS[i] / 1024));
          continue;
        }
        // 连读 4 次取总时间, 摊掉单次抖动
        const uint32_t t0 = micros();
        size_t tot = 0;
        for (int k = 0; k < 4; k++) {
          tot += f2.read(kbuf, KS[i]);
        }
        const uint32_t dt = micros() - t0;
        f2.close();
        SBLOG("#11 chunk=%luB x4 got=%luB %luus %luKBps", (unsigned long)KS[i], (unsigned long)tot,
              (unsigned long)dt, (unsigned long)kbPerSec((uint32_t)tot, dt));
      }
      free(kbuf);
    } else {
      SBLOG("#11 chunk: 256KB 缓冲分配失败");
    }
  }

  SBLOG("#99 end");
  Serial.println();
  Serial.flush();
}
