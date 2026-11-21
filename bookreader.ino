/*
 * bookreader.ino - ESP32-S3 小说阅读器 (第一步: TXT + 中文字库渲染)
 *
 * 本阶段目标: 屏幕点亮、SD 卡挂载、24x24 字库加载、TXT 分页翻页。
 * 第二步接音频(双核 + 共用 SPI 锁), 第三步接 EPUB。
 *
 * ★ 关键设计: 屏幕和 SD 卡共用 SPI2 总线, 而 TFT_eSPI 推像素时是直接写 SPI
 *   寄存器(不走 SPIClass 的事务锁), 所以"刷屏"和"读卡"必须用 sd_mutex 串行化。
 *   规则: 任何访问 SD 或刷屏的代码都在 SdLockGuard 作用域内。
 *
 * 接线:
 *   ST7789 : MOSI=11 SCK=12 CS=10 DC=9 RST=8 BL=3.3V常亮
 *   MicroSD: MOSI=11 SCK=12 MISO=13 CS=14
 *   EC11   : A=1 B=2 KEY=3(另一端 GND)
 *   电池   : 正极 -> 10k -> GPIO6 -> 10k -> GND
 *
 * Arduino IDE 设置: 开发板 = "ESP32S3 Dev Module"
 *   PSRAM = OPI PSRAM, Flash Size = 16MB,
 *   Partition Scheme = 16M Flash (3MB APP/9.9MB FATFS), USB CDC On Boot = Enabled
 */
#include "config.h"

#include "audio_player.h"
#include "bus_init.h"
#include "display.h"
#include "epub_parser.h"
#include "font.h"
#include "image_view.h"
#include "input.h"
#include "sd_bench.h"
#include "sd_mutex.h"
#include "state_store.h"
#include "text.h"
#include "txt_reader.h"
#include "ui.h"

#include <SD.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// ★ 关键诊断必须"永远输出"★
// log_e()/log_i() 受 Arduino IDE 的 "Core Debug Level" 控制, 默认是 None,
// 也就是说默认情况下它们**什么都不打印**。启动失败的真正原因如果用 log_* 输出,
// 用户就只能看到"未就绪"三个字, 完全不知道卡在哪一步 —— 这是踩过的坑。
// 所以启动路径上的诊断一律用下面这个宏直接走 Serial。
// ---------------------------------------------------------------------------
#define BOOTLOG(fmt, ...)                 \
  do {                                    \
    Serial.printf(fmt "\n", ##__VA_ARGS__); \
    Serial.flush();                       \
  } while (0)

// 全局字库实例(ui.cpp 里 extern 使用)
DotFont g_font;

static TxtReader g_book;
static bool s_sdReady = false;
static bool s_fontReady = false;
static bool s_audioTried = false;
static const char *s_sdErr = nullptr;   // SD 失败的类别, 用于给出不同提示
static uint32_t s_fontSizeKB = 0;       // 字库文件实际大小(自检页显示)
static char s_epubImgDir[128] = {0};   // 当前 epub 的插图目录(没有则为空)
static uint32_t s_batNextMs = 0;

// 可重试的启动推进(定义在 setup 之后)
static bool bootTryAdvance();

// 打开一本书(txt/epub)并按需跳到指定页。定义在选书流程附近, 但「继续阅读」
// 在主菜单处理里先用到它 —— 所以这里必须先声明(否则 "was not declared in this scope")。
// ★ 默认参数只写在这里 ★ C++ 规定默认实参只能给一次, 定义处再写一遍会报
//   "default argument given for parameter" 之类的错误。
static bool openBookAtPath(const char *path, bool isEpub, uint32_t startPage = 0, uint32_t startOffset = 0);

// ---------------------------------------------------------------------------
// SD 卡挂载
// ---------------------------------------------------------------------------
static bool mountSd() {
  // 先把两边的 CS 都拉高, 保证初始化期间不会有设备误响应总线数据
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  // ★ 关键: SD 必须用"显示库实际使用的那条总线", 而不是全局 SPI 对象。
  //   如果 User_Setup.h / tft_setup.h 里定义了 USE_HSPI_PORT, TFT_eSPI 会自己建一个
  //   SPIClass(SPI3/HSPI) 驱动屏幕; 此时若 SD 还用全局 SPI(SPI2), 两者就在两条不同的
  //   总线上 —— sd_mutex 就挡不住"刷屏"和"读卡"互相干扰了。
  //   SD.begin() 支持传入 SPIClass&, 所以这里直接跟随显示库的实例。
  SPIClass *bus = displaySpiInstance();
  if (!bus) {
    BOOTLOG("[sd] 显示库还没初始化, 无法确定共用哪条 SPI 总线");
    return false;
  }
  Serial.printf("[sd] 使用与屏幕相同的 SPI 实例 (%s)\n",
                (bus == &SPI) ? "全局 SPI / FSPI(SPI2)" : "TFT_eSPI 私有实例 / HSPI(SPI3)");

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    BOOTLOG("[sd] 挂载时等锁超时");
    return false;
  }
  if (!SD.begin(PIN_SD_CS, *bus, SPI_FREQ_SD)) {
    // 区分"卡没插/接触不良"(cardType = NONE) 和"卡在但文件系统有问题"
    // 这两种情况的排查方向完全不同, 混在一起报错会让人白折腾
    const uint8_t ct = SD.cardType();
    if (ct == CARD_NONE) {
      BOOTLOG("[sd] 失败: 没有检测到 SD 卡 (cardType=NONE) -> 检查卡是否插紧/接线/供电");
      s_sdErr = "SD_ABSENT";
    } else {
      BOOTLOG("[sd] 失败: 卡已识别(cardType=%u)但挂载不上 -> 多半不是 FAT32, 请用电脑格式化为 FAT32", (unsigned)ct);
      s_sdErr = "SD_FS";
    }
    return false;
  }
  s_sdErr = nullptr;
  // SD 通路自检(见 sd_bench.cpp): 纯 sketch 侧原始 SPI 事务, 不依赖核心库。
  // 目的: 量出 #02 纯SPI 时钟 / #03 频率扫描对比分频寄存器 / #04 CMD17 逐阶段
  // / #05 CMD18 多扇区 / #07 自制FAT直读 / #08 库路径对照 —— 一次上机就能钉死
  // "每次 disk_read 固定 106ms" 到底是时钟没升上去, 还是每次调用的协议开销。
  // 卡没挂载时只打一行且不消耗"只跑一次"标记, 所以放这里安全。
  sdBenchRunEarly();
  BOOTLOG("[sd] 挂载成功, 容量 %llu MB", (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

// ---------------------------------------------------------------------------
// 找一本可读的 TXT
// ---------------------------------------------------------------------------
// 找第一本可读的书: 优先 .txt, 其次 .epub
// isEpub 输出是不是 epub(需要先解压成文本缓存)
static bool findFirstBook(char *out, size_t outLen, bool *isEpub) {
  static const char *dirs[] = {"/txt", "/books", "/"};
  if (isEpub) {
    *isEpub = false;
  }
  // 两轮: 第一轮找 txt, 第二轮找 epub(这样 txt 优先)
  for (int pass = 0; pass < 2; pass++) {
  for (int d = 0; d < 3; d++) {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      continue;
    }
    File dir = SD.open(dirs[d]);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      continue;
    }
    File f;
    while ((f = dir.openNextFile())) {
      const bool isDir = f.isDirectory();
      char nm[64];
      strncpy(nm, f.name(), sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      f.close();
      if (isDir) {
        continue;
      }
      const size_t l = strlen(nm);
      const char *want = (pass == 0) ? ".txt" : ".epub";
      const size_t wl = strlen(want);
      if (l > wl && strcasecmp(nm + l - wl, want) == 0) {
        snprintf(out, outLen, "%s/%s", strcmp(dirs[d], "/") == 0 ? "" : dirs[d], nm);
        if (isEpub) {
          *isEpub = (pass == 1);
        }
        dir.close();
        return true;
      }
    }
    dir.close();
  }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 书籍列表: 扫描 SD 卡上所有可读的书(.txt / .epub), 供主菜单里的"选书"使用
//
// 原来只找"第一本"就直接打开, 用户没法挑。卡里放多本书时必须能选。
// ---------------------------------------------------------------------------
#define BOOK_LIST_MAX 32
#define BOOK_PATH_LEN 96

static char (*s_bookList)[BOOK_PATH_LEN] = nullptr;
static bool *s_bookIsEpub = nullptr;
static int s_bookCount = 0;

static int bookCountCb(void) {
  return s_bookCount;
}

static const char *bookTitleCb(int index) {
  if (!s_bookList || index < 0 || index >= s_bookCount) {
    return "";
  }
  // 只显示文件名(去掉路径), 后缀由下面的图标/说明体现
  const char *p = s_bookList[index];
  const char *slash = strrchr(p, '/');
  return slash ? slash + 1 : p;
}

static const char *bookPathAt(int index) {
  if (!s_bookList || index < 0 || index >= s_bookCount) {
    return nullptr;
  }
  return s_bookList[index];
}

static bool bookIsEpubAt(int index) {
  if (!s_bookIsEpub || index < 0 || index >= s_bookCount) {
    return false;
  }
  return s_bookIsEpub[index];
}

// 扫描所有书。返回找到的本数。
static int scanBooks(void) {
  if (!s_bookList) {
    s_bookList = (char(*)[BOOK_PATH_LEN])malloc((size_t)BOOK_PATH_LEN * BOOK_LIST_MAX);
    s_bookIsEpub = (bool *)malloc(sizeof(bool) * BOOK_LIST_MAX);
  }
  if (!s_bookList || !s_bookIsEpub) {
    BOOTLOG("[book] 书籍列表内存分配失败");
    return 0;
  }
  s_bookCount = 0;

  static const char *dirs[] = {"/txt", "/books", "/"};
  // 先扫 .txt(优先显示), 再扫 .epub
  for (int pass = 0; pass < 2; pass++) {
    const char *want = (pass == 0) ? ".txt" : ".epub";
    const size_t wl = strlen(want);
    for (int d = 0; d < 3; d++) {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (!g.ok()) {
        continue;
      }
      File dir = SD.open(dirs[d]);
      if (!dir || !dir.isDirectory()) {
        if (dir) {
          dir.close();
        }
        continue;
      }
      File f;
      while ((f = dir.openNextFile())) {
        const bool isDir = f.isDirectory();
        char nm[64];
        strncpy(nm, f.name(), sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = 0;
        f.close();
        if (isDir) {
          continue;
        }
        const size_t l = strlen(nm);
        if (l <= wl || strcasecmp(nm + l - wl, want) != 0) {
          continue;
        }
        if (s_bookCount >= BOOK_LIST_MAX) {
          continue;
        }
        snprintf(s_bookList[s_bookCount], BOOK_PATH_LEN, "%s/%s",
                 strcmp(dirs[d], "/") == 0 ? "" : dirs[d], nm);
        s_bookIsEpub[s_bookCount] = (pass == 1);
        s_bookCount++;
      }
      dir.close();
    }
  }
  BOOTLOG("[book] 扫描到 %d 本书", s_bookCount);
  for (int i = 0; i < s_bookCount && i < 8; i++) {
    BOOTLOG("[book]   %d) %s", i + 1, s_bookList[i]);
  }
  return s_bookCount;
}

// ---------------------------------------------------------------------------
// 章节扫描进度: 扫描期间刷界面
// ---------------------------------------------------------------------------
static void onScanProgress(void *, uint32_t done, uint32_t total) {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs < 150) {
    return;
  }
  lastMs = now;
  const uint8_t pct = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
  uiShowProgress("正在扫描章节", pct);
}

static bool scanAborted(void *) {
  // 扫描中按一下按钮就中断扫描(直接开始读第一页)。
  // 注意: 只"看"队列不消费, 否则会把按键从主循环那里抢走。
  return false;   // 暂时不打断扫描, 避免和主循环抢事件; 扫描有进度条可看
}

// ---------------------------------------------------------------------------
// 打开书
// ---------------------------------------------------------------------------
static bool openBook(const char *path) {
  uiShowProgress("正在打开", 0);
  s_epubImgDir[0] = 0;   // 纯 txt 没有插图

  g_book.setScanProgress(onScanProgress, nullptr);
  g_book.setAbortCheck(scanAborted, nullptr);

  if (!g_book.open(SD, path)) {
    uiSetMessage("打不开这本书", g_book.err());
    uiShowMessage();
    uiRender();
    return false;
  }

  uiAttachBook(&g_book);
  uiSetStatusHint("");

  // ---- 打开之后立刻自检: 空白页到底是"没读到字"还是"画不出来" ----
  {
    // 文件头 16 字节: 用来确认编码(UTF-8 BOM = EF BB BF; UTF-16 LE = FF FE)
    uint8_t head[16] = {0};
    uint32_t fsize = 0;
    {
      SdLockGuard hg(SD_LOCK_TIMEOUT_MS);
      if (hg.ok()) {
        File f = SD.open(path, FILE_READ);
        if (f) {
          fsize = f.size();
          f.read(head, sizeof(head));
          f.close();
        }
      }
    }
    BOOTLOG("[book] 文件 %s", path);
    BOOTLOG("[book] 大小 %lu 字节, 头16字节: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned long)fsize, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], head[8],
            head[9], head[10], head[11], head[12], head[13], head[14], head[15]);
    if (head[0] == 0xFF && head[1] == 0xFE) {
      BOOTLOG("[book] !! 这是 UTF-16 LE 编码, 设备只支持 UTF-8 —— 请另存为 UTF-8");
    } else if (head[0] == 0xFE && head[1] == 0xFF) {
      BOOTLOG("[book] !! 这是 UTF-16 BE 编码, 设备只支持 UTF-8 —— 请另存为 UTF-8");
    } else if (head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
      BOOTLOG("[book] 带 UTF-8 BOM(正常)");
    } else {
      BOOTLOG("[book] 无 BOM, 按 UTF-8 解析");
    }
  }

  const int nlines = g_book.loadPage(0);
  BOOTLOG("[book] 编码=%s, 第一页 %d 行, 纯文本 %lu 字节", g_book.isGbk() ? "GBK" : "UTF-8", nlines,
          (unsigned long)g_book.pageTextBytes());
  if (nlines > 0) {
    BOOTLOG("[book] 第一页首行: [%s]", g_book.line(0));
    BOOTLOG("[book] 第一页次行: [%s]", g_book.line(1 < nlines ? 1 : 0));
  }

  if (nlines <= 0) {
    static char detail[200];
    snprintf(detail, sizeof(detail), "%s | 文件 %lu 字节。若是 UTF-16/GBK 请另存为 UTF-8 | 按一下旋钮重试",
             g_book.err()[0] ? g_book.err() : "第一页没有任何文字", (unsigned long)g_book.fileSize());
    uiSetMessage("这本书是空的", detail);
    uiShowMessage();
    uiRender();
    return false;
  }

  uiShowReading();
  uiRender();
  return true;
}

// ---------------------------------------------------------------------------
// 翻页
// ---------------------------------------------------------------------------
// ★ 当前正在看的书(供阅读存档用) ★
//   为什么需要它: stateStoreNoteBook() 要"书在 SD 上的**原始路径**"。
//   而 EPUB 打开后 g_book 打开的是**缓存文本**(/cache/xxx.txt), 用 g_book 的路径
//   会存错 —— 那个文件不在书列表里, 用户删卡/换卡后就对不上了。
//   所以在这里记住"用户点开的是哪本书"(原始路径 + 是否 epub)。
static char s_curBookPath[STATE_STORE_PATH_MAX] = {0};
static bool s_curBookIsEpub = false;

// 记一笔"现在看到第几页了"。只标脏, 真正落盘由 stateStoreTick() 防抖后做。
static void noteReadingProgress(void) {
  if (!g_book.isOpen() || s_curBookPath[0] == 0) {
    return;
  }
  stateStoreNoteBook(s_curBookPath, s_curBookIsEpub, g_book.page(), g_book.pageStartOffset());
}

static void turnPage(int32_t delta) {
  if (!g_book.isOpen() || delta == 0) {
    return;
  }
  const int32_t target = (int32_t)g_book.page() + delta;
  if (target < 0) {
    return;
  }
  if (g_book.loadPage((uint32_t)target) <= 0) {
    uiSetStatusHint("到底了");
    uiRender();
    return;
  }
  noteReadingProgress();   // 页码变了 -> 记进度(不写 Flash, 只是标脏)
  uiSetStatusHint("");
  uiRender();
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
// 音乐面板的开关状态(叠加在阅读界面上)
static bool s_musicPanel = false;

static void openToc() {
  if (g_book.chapterCount() <= 0) {
    uiSetStatusHint("无章节");
    uiRender();
    return;
  }
  uiShowToc();
  const int cur = g_book.currentChapter();
  for (int i = 0; i < cur; i++) {
    uiTocMove(1);  // 定位到当前章节
  }
  uiRender();
}

// ---- 选曲 ----
// ui 层不直接依赖 audio_player, 用两个回调把曲目信息喂过去
static int trackCountCb(void) {
  return audioTrackCount();
}
static const char *trackTitleCb(int index) {
  return audioTrackTitle(index);
}

static void openMusicPicker() {
  if (audioTrackCount() <= 0) {
    uiSetStatusHint("没找到音乐文件");
    uiRender();
    return;
  }
  // 把光标定位到正在播放的那首上, 方便就近换曲
  const AudioStatus st = audioGetStatus();
  uiMusicSetCurrent(st.trackIndex);
  uiShowMusic();
  if (st.trackIndex >= 0) {
    // uiShowMusic 之后再把选择移到当前曲目
    const int delta = st.trackIndex - uiMusicSelected();
    uiMusicMove(delta);
  }
  uiRender();
}

static void handleMusicInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    uiMusicMove((int)enc);
    uiRender();
    return;
  }
  if (btn == BTN_SHORT || btn == BTN_BOOT) {
    // 选中并立即播放, 然后进播放界面(能看到曲名/进度/音量)
    //
    // 注意: audioPlayIndex() 内部会**先做文件头预检**, 不支持的格式直接拒绝、
    // 且**完全不碰当前正在播放的那首**(见 audio_player.cpp 的 headerLooksPlayable)。
    // 被拒时不要改 uiMusicSetCurrent —— 界面上的"当前曲目"必须仍然指向真正在响的那首,
    // 否则用户会以为切歌成功了但没声音。
    const int sel = uiMusicSelected();
    const bool started = audioPlayIndex(sel);
    if (started) {
      uiMusicSetCurrent(sel);
      uiSetStatusHint("");
      BOOTLOG("[ui] 选曲: 第 %d 首已开始播放", sel + 1);
    } else {
      // 原因由 audio_player 放进 AudioStatus::notice, 播放界面底部会显示出来
      uiSetStatusHint("这首歌不能播");
      BOOTLOG("[ui] 选曲: 第 %d 首被拒绝(格式不支持), 保持原曲继续播放", sel + 1);
    }
    uiShowPlayer();
    uiRender();
  } else if (btn == BTN_LONG) {
    // 长按 = 返回: 有书在看就回正文, 否则回主菜单
    if (g_book.isOpen()) {
      uiShowReading();
    } else {
      uiShowMenu();
    }
    uiRender();
  }
}

// ---------------------------------------------------------------------------
// 播放界面: 旋转调音量, 按下暂停/继续, 长按选曲
//
// 这是你要的"能调音量 + 能看到在放什么 + 有进度"的界面。
// ---------------------------------------------------------------------------
static void handlePlayerInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    const int steps = (int)enc;
    for (int i = 0; i < abs(steps); i++) {
      audioVolumeUp(steps > 0 ? 1 : -1);
    }
    uiRender();
    return;
  }
  if (btn == BTN_SHORT || btn == BTN_BOOT) {
    audioTogglePause();
    uiRender();
  } else if (btn == BTN_LONG) {
    openMusicPicker();   // 长按进选曲
  }
}

// 回到主菜单: 必须先关书, 否则文件句柄和排版状态会留在原地, 再开另一本书就出问题。
// 返回 true 表示确实关掉了一本书。
static bool returnToMainMenu(void) {
  if (g_book.isOpen()) {
    g_book.close();
    BOOTLOG("[ui] 已关书, 返回主菜单");
  }
  uiAttachBook(nullptr);
  s_musicPanel = false;
  // ★ 回主菜单是一个明确的存档点: 立刻落盘 ★
  //   不然用户看完书直接关机, 防抖窗口里那次写就没发生 —— 进度白记了。
  stateStoreFlush();
  uiShowMenu();
  uiSetStatusHint("");
  uiRender();
  return true;
}

static void handleReadingInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    // 音乐面板打开时, 旋转调音量; 否则翻页
    if (s_musicPanel) {
      audioVolumeUp((int8_t)(enc > 0 ? 1 : -1) * (int8_t)abs((int)enc));
      uiRender();
    } else {
      turnPage(enc);
    }
  }
  if (btn == BTN_SHORT) {
    // 音乐面板打开时, 短按 = 播放/暂停; 否则进目录
    if (s_musicPanel) {
      audioTogglePause();
      uiRender();
    } else {
      openToc();
    }
  } else if (btn == BTN_BOOT) {
    // 开发板 BOOT 键(GPIO0): 直接呼出选曲列表, 不用接线也能换曲
    openMusicPicker();
  } else if (btn == BTN_REPEAT) {
    // ★ 长按不放 = 回主菜单 ★ (用户要求加的)
    //
    // 为什么用 BTN_REPEAT 而不是再加一个新按键或新时长:
    //   - BTN_LONG(按住 0.8s) 已经被"开关音乐面板"占了;
    //   - BTN_REPEAT 是"长按之后继续按住不放"每 180ms 连发的事件, 之前只用于
    //     音乐面板里连续减音量 —— 那个用途很小, 让位给"回主菜单"更值。
    //   - 直觉上"按越久退越彻底"也说得通: 短按进目录 / 长按开面板 / 长按不放退菜单。
    returnToMainMenu();
  } else if (btn == BTN_LONG) {
    // 长按 = 开/关音乐面板
    s_musicPanel = !s_musicPanel;
    if (s_musicPanel && audioTrackCount() > 0 && !audioGetStatus().loaded) {
      audioPlayIndex(0);
    }
    uiShowReading();
    uiSetStatusHint("");
    uiRender();
  }
}

static void handleTocInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    uiTocMove((int)enc);
    uiRender();
  }
  if (btn == BTN_SHORT) {
    uint32_t page = 0;
    if (g_book.jumpToChapter(uiTocSelected(), &page)) {
      uiShowReading();
      uiSetStatusHint("");
      uiRender();
    } else {
      uiSetStatusHint("跳转失败");
      uiRender();
    }
  } else if (btn == BTN_LONG) {
    uiShowReading();
    uiRender();
  } else if (btn == BTN_REPEAT) {
    // ★ 长按不放 = 回主菜单 ★
    //   和阅读界面保持一致 —— 否则进了目录就只能回到正文, 出不去。
    returnToMainMenu();
  }
}

// ---------------------------------------------------------------------------
// setup: 只做"必须最先做且不会失败"的部分
//
// 其余每一步都可能失败(SD 没插、字库没生成、没有 txt), 所以放到 bootTryAdvance()
// 里做, 并且**失败后可重试** —— 否则不插卡就永远起不来, 插了卡也不会继续。
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=== bookreader (ESP32-S3) ==="));
  // ★ 芯片指纹(板子身份) ★
  //   为什么打这个: 调试期间换过板子, 而"是不是同一块板"只能靠唯一标识判断 ——
  //   COM 口会变(COM3->COM4)、启动横幅完全一样, 光看日志分不出来。
  //   原因: 换板后**接线可能不一样**(尤其 SD 卡那几根), 那会表现成
  //   "卡突然检测不到"这种看起来像软件故障的现象, 极易误判。
  {
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("芯片 MAC: %02X:%02X:%02X:%02X:%02X:%02X  (板子身份指纹)\n", (uint8_t)(mac >> 40),
                  (uint8_t)(mac >> 32), (uint8_t)(mac >> 24), (uint8_t)(mac >> 16), (uint8_t)(mac >> 8),
                  (uint8_t)mac);
    Serial.printf("芯片型号: %s rev%d, %u MHz, Flash %u MB\n", ESP.getChipModel(), (int)ESP.getChipRevision(),
                  (unsigned)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  }
  Serial.printf("PSRAM: %u KB 空闲 / %u KB 总共\n", (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));
  if (ESP.getPsramSize() == 0) {
    Serial.println(F("!! PSRAM 没开: 工具菜单里把 PSRAM 设为 OPI PSRAM, 否则帧缓冲分配不了"));
  }

  // 1) 总线互斥锁必须最先创建
  Serial.println(F("[boot] 1/6 创建总线互斥锁"));
  Serial.flush();
  uiInit();          // 界面状态要先初始化(启动进度界面要用)
  uiBootBegin();
  uiBootStep(0, "初始化互斥锁", BOOT_RUN);
  sdLockInit();
  uiBootStep(0, nullptr, BOOT_OK);

  // 2) 输入
  Serial.println(F("[boot] 2/6 初始化输入(EC11/电池)"));
  Serial.flush();
  uiBootStep(1, "初始化输入设备", BOOT_RUN);
  inputInit();
  batteryInit();
  inputPrintPinStates();       // 接线排查用: 串口会打印 A/B/SW 的原始电平
  inputStartTask();            // 独立 1ms 任务, 保证刷屏/解压时按键也不丢
  uiBootStep(1, nullptr, BOOT_OK);

  // 3) 屏幕(同时建立共享 SPI 总线)
  Serial.println(F("[boot] 3/6 初始化屏幕"));
  Serial.flush();
  uiBootStep(2, "初始化屏幕", BOOT_RUN);
  if (!displayInit()) {
    Serial.println(F("!! 屏幕初始化失败"));
    uiBootStep(2, nullptr, BOOT_FAIL, "屏幕初始化失败, 检查接线与 tft_setup.h");
    return;
  }
  uiBootStep(2, nullptr, BOOT_OK);
  Serial.println(F("[boot] 3/6 屏幕初始化完成"));
  Serial.flush();

  uiSetPageImageProvider(currentPageImage);   // 让界面能显示插图页
  uiSetTrackProvider(trackCountCb, trackTitleCb);   // 让选曲界面能列出曲目
  uiSetBookProvider(bookCountCb, bookTitleCb);      // 让选书界面能列出书

  // 4~6) 交给可重试的启动推进器
  //
  // ★ 不论成功还是失败, 最后都进主菜单 ★
  //   原来成功就直接进正文、失败就停在启动界面, 而音乐面板的入口只在"阅读界面"
  //   分支里处理 —— 结果启动一旦失败, 音乐 UI 就完全触发不了。
  //   现在统一由主菜单分流, 音乐和小说都能自己选。
  if (!bootTryAdvance()) {
    BOOTLOG(" ");
    BOOTLOG("======== 启动未完成, 卡在上面最后一条 [xx] 步骤 ========");
    if (!s_sdReady) {
      BOOTLOG("  -> SD 卡没挂上: 上面的 [sd] 行说明了原因");
    } else if (!s_fontReady) {
      BOOTLOG("  -> 字库没加载: 把 build_font.py 生成的 .f24 放进卡里 %s/ 目录", FONT_DIR);
      BOOTLOG("     当前错误: %s", g_font.lastError());
    } else {
      BOOTLOG("  -> 卡和字库都正常, 是没找到书文件(.txt / .epub)");
    }
    BOOTLOG("  仍然可以进主菜单听音乐; 补好文件后回主菜单选\"看小说\"即可");
    BOOTLOG("======================================================");
  }

  // 扫描书列表(主菜单的"看小说"要用)
  if (s_sdReady) {
    scanBooks();
  }

  // ★ 阅读存档(NVS) ★ 放最后: 它只碰 Flash, 不依赖 SD/字库/音频, 所以前面任何一步
  //   失败都不影响它。开机先把已有存档读进内存镜像, 这样"内容没变就不写"才判得准。
  if (stateStoreBegin()) {
    BookState st;
    if (stateStoreLoad(&st)) {
      BOOTLOG("[state] 上次看到: %s 第 %lu 页 -> 主菜单「继续阅读」可一键回去", st.bookPath,
              (unsigned long)st.page);
    }
  }

  // 进主菜单, 由用户决定看小说还是听音乐
  uiShowMenu();
  uiRender();
  BOOTLOG("[boot] 已进入主菜单");
}

// ---------------------------------------------------------------------------
// EPUB: 首次打开时解压成纯文本缓存, 之后复用 TXT 阅读器
//
// 为什么这么做: 排版引擎(text.cpp)已经用 38 万字真实小说验证过字符不丢,
// 直接流式喂给它需要改动那套缓冲区/回退逻辑, 风险太高。解压成缓存只做一次,
// 之后打开是秒开, 换来的是排版逻辑零改动。
// ---------------------------------------------------------------------------
static void onEpubProgress(void *, const char *phase, int cur, int total) {
  uiShowProgress(phase, (uint8_t)(total > 0 ? cur : 0));
}

// 把 /a/b/书名.epub 映射成 /cache/书名.v2.txt
//
// ★ 版本号放在【文件名】里, 不放在文件内容里 ★ (2026-10-03 踩过的坑)
//   我一开始把 "EPUBCACHE v2" 写在缓存**第一行**当版本标记, 结果阅读器把它当正文:
//     [pg] 首行=[EPUBCACHE]     <- 第 1 页被这行占掉 -> 分页全乱
//   症状是"插图只显示前几张 + 正文被吞"。正文文件必须**纯净**, 一个字都不能多写。
//   改成文件名带版本号后:
//     提取逻辑一变 -> 版本号加一 -> 文件名变了 -> 旧缓存自然"找不到" -> 重新解压。
//   既不用手动删 /cache, 也不会污染正文。
static void epubCachePath(const char *epubPath, char *out, size_t outLen) {
  const char *slash = strrchr(epubPath, '/');
  const char *base = slash ? (slash + 1) : epubPath;
  char name[80];
  snprintf(name, sizeof(name), "%s", base);
  char *dot = strrchr(name, '.');
  if (dot) {
    *dot = 0;
  }
  // EPUB_CACHE_VER 只是个小整数(见 epub_parser.h), 放进文件名不占正文
  snprintf(out, outLen, "/cache/%s.v%d.txt", name, EPUB_CACHE_VER);
}

// /cache/<书名>/ 用来放这本书的插图
static void epubImageDir(const char *epubPath, char *out, size_t outLen) {
  char name[80];
  const char *slash = strrchr(epubPath, '/');
  const char *base = slash ? (slash + 1) : epubPath;
  snprintf(name, sizeof(name), "%s", base);
  char *dot = strrchr(name, '.');
  if (dot) {
    *dot = 0;
  }
  snprintf(out, outLen, "/cache/%s", name);
}

static void ensureDir(const char *dir) {
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    return;
  }
  if (!SD.exists(dir)) {
    SD.mkdir(dir);
  }
}

// 确保 /cache 目录存在
static void ensureCacheDir() {
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    return;
  }
  if (!SD.exists("/cache")) {
    SD.mkdir("/cache");
  }
}

// ★ EPUB 缓存版本标记的定义在 epub_parser.h 里(EPUB_CACHE_VER_TAG) ★
//   因为解压侧(epub_parser.cpp 的 extractTextTo)要写它、打开侧(这里)要校验它。

// ★ scanCacheMarkers: 列出正文缓存里实际存在的插图标记 ★//   用途: 插图文件导出了 21 张，但如果正文里没有对应的 @N@ 标记，图片页就是不出现。
//   它同时数一下"非法标记"，便于发现提取阶段的 bug。
//   上限列 40 个，避免长书刷屏。
static void scanCacheMarkers(const char *cachePath) {
  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    BOOTLOG("[epub] 标记扫描: 等 SD 锁超时");
    return;
  }
  File f = SD.open(cachePath, FILE_READ);
  if (!f) {
    BOOTLOG("[epub] 标记扫描: 打不开 %s", cachePath);
    return;
  }
  char line[128];
  char list[300] = {0};
  int found = 0, bad = 0, lines = 0;
  while (f.available()) {
    const int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n > 0 ? n : 0] = 0;
    lines++;
    int L = (int)strlen(line);
    while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == ' ')) {
      line[--L] = 0;
    }
    if (L >= 3 && line[0] == '@' && line[L - 1] == '@') {
      bool digits = true;
      for (int i = 1; i < L - 1; i++) {
        if (line[i] < '0' || line[i] > '9') {
          digits = false;
          break;
        }
      }
      if (digits) {
        found++;
        if (strlen(list) < sizeof(list) - 8) {
          if (list[0]) {
            strncat(list, ",", sizeof(list) - strlen(list) - 1);
          }
          strncat(list, line, sizeof(list) - strlen(list) - 1);
        }
      } else {
        bad++;
      }
    }
  }
  f.close();
  BOOTLOG("[epub] 标记扫描: 共 %d 行, 合法插图标记 %d 个, 非法 %d 个", lines, found, bad);
  BOOTLOG("[epub] 标记列表(最多这些): %s", list[0] ? list : "(一个都没有!)");
  if (found == 0) {
    BOOTLOG("[epub] !! 正文里没有任何插图标记 -> 插图页永远出不来(提取阶段丢标记了)");
  }
}

// 打开一本 epub: 需要时先解压, 然后交给 TXT 阅读器
static bool openEpub(const char *epubPath) {
  char cache[128];
  epubCachePath(epubPath, cache, sizeof(cache));

  BOOTLOG("[epub] 打开 %s -> 缓存 %s", epubPath, cache);

  bool needConvert = true;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (g.ok() && SD.exists(cache)) {
      File cf = SD.open(cache, FILE_READ);
      if (cf) {
        const uint32_t csize = (uint32_t)cf.size();
        needConvert = (csize < 64);   // 太小的缓存视为无效, 重新解压
        // 版本校验已经由**文件名**完成(epubCachePath 里带 .vN), 不需要读文件内容 ——
        // 这样正文文件保持纯净, 不会出现"版本标记被当正文"的问题。
        // ★ 检测"坏缓存": 字节数很大但几乎没有可见字符 ★
        //   为什么需要(2026-10-03 实测): 写入失败会留下**空洞**, 文件大小正常
        //   (362995 字节), 但内容大段是零/旧数据 —— 读出来是上千行空行,
        //   表现成"翻页一路空白、只有开头两页正常"。
        //   光看 csize 判不出来, 所以**抽样子节**看有没有可见字符:
        //   正常中文正文到处都有可打印字节; 空洞区则整段是 0x00 或 0xFF。
        if (!needConvert && csize >= 4096) {
          uint8_t probe[512];
          int good = 0, total = 0;
          const uint32_t offsets[4] = {csize / 5, csize / 2, (csize * 3) / 4, csize - 1024};
          for (int k = 0; k < 4; k++) {
            if (!cf.seek(offsets[k])) {
              break;
            }
            const int n = cf.read(probe, sizeof(probe));
            for (int i = 0; i < n; i++) {
              total++;
              const uint8_t c = probe[i];
              if (c >= 0x20 && c != 0x7F) {
                good++;
              }
            }
          }
          // 正常正文可见字节比例很高; 空洞几乎为 0
          if (total > 0 && good * 100 < total * 30) {
            BOOTLOG("[epub] !! 缓存内容异常(可见字符仅 %d%%, 疑似写入空洞) -> 删掉重新解压", good * 100 / total);
            needConvert = true;
          }
        }
        BOOTLOG("[epub] 发现已有缓存: %lu 字节, %s", (unsigned long)csize,
                needConvert ? "无效, 重新解压" : "直接使用");
        cf.close();
      }
    }
    // 坏缓存直接删掉, 否则下面"重新解压"会覆盖它但旧空洞可能残留
    if (needConvert && g.ok() && SD.exists(cache)) {
      SD.remove(cache);
      BOOTLOG("[epub] 已删除旧缓存 %s", cache);
    }
  }

  if (needConvert) {
    ensureCacheDir();
    uiShowProgress("正在解压 EPUB", 0);
    BOOTLOG("[epub] 首次打开, 开始解压(可能要一会儿)");

    EpubParser ep;
    if (!ep.open(SD, epubPath)) {
      BOOTLOG("[epub] !! 解析失败: %s", ep.err());
      static char detail[200];
      snprintf(detail, sizeof(detail), "%s | 按一下旋钮重试", ep.err());
      uiSetMessage("EPUB 解析失败", detail);
      uiShowMessage();
      uiRender();
      ep.close();
      return false;
    }
    BOOTLOG("[epub] 解析成功: spine %d 项, 图片 %d 张", ep.spineCount(), ep.imageCount());

    if (!ep.extractTextTo(cache, onEpubProgress, nullptr)) {
      BOOTLOG("[epub] !! 正文解压失败");
      uiSetMessage("EPUB 解压失败", "可能是加密或使用了不支持的压缩方式 | 按一下旋钮重试");
      uiShowMessage();
      uiRender();
      ep.close();
      return false;
    }

    // 检查解压出来的缓存到底有多大 —— 0 字节说明正文没提取出来
    {
      uint32_t csize = 0;
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (g.ok()) {
        File cf = SD.open(cache, FILE_READ);
        if (cf) {
          csize = cf.size();
          cf.close();
        }
      }
      BOOTLOG("[epub] 正文解压完成: 缓存 %lu 字节", (unsigned long)csize);
      if (csize < 64) {
        BOOTLOG("[epub] !! 缓存几乎没有内容, 说明正文提取失败");
        uiSetMessage("EPUB 里没提取到正文", "可能是纯图片书或结构特殊 | 按一下旋钮重试");
        uiShowMessage();
        uiRender();
        ep.close();
        return false;
      }

      // 标记扫描统一放在 openEpub() 结尾(两条路径都会跑), 这里不再重复调用
    }

    // 把插图也导出到缓存目录, 阅读时按标记直接显示
    const int nImg = ep.imageCount();
    if (nImg > 0) {
      char dir[128];
      epubImageDir(epubPath, dir, sizeof(dir));
      ensureDir(dir);
      snprintf(s_epubImgDir, sizeof(s_epubImgDir), "%s", dir);
      // 导出循环 + 进度 + 命名统一收进 extractAllImages()：
      //   命名规则必须和 currentPageImage() 完全一致(都是 img_%02d + imageExt(i)),
      //   以前这里是手写循环, 两边各写一套很容易漂移。现在只有一处实现。
      const int okCount = ep.extractAllImages(dir, onEpubProgress, nullptr);
      BOOTLOG("[epub] 插图导出 %d/%d 张 -> %s", okCount, nImg, dir);
    } else {
      s_epubImgDir[0] = 0;
    }
    ep.close();
    uiShowProgress("正在解压 EPUB", 100);
    BOOTLOG("[epub] 解压阶段结束");
  }

  // =========================================================================
  // ★ 插图必须与"正文是否需要转换"解耦 ★ （2026-10-03 修）
  //
  // 症状: 正文能看, 但翻到插图页永远不显示, 串口是
  //       `[img] 页内有图片标记 @0@ 但图片目录为空(不是 epub?)`
  //
  // 根因: s_epubImgDir 只在上面 `if (needConvert)` 里被赋值。而正文缓存一旦存在
  //       (第二次以后打开这本书, 或缓存是之前某次留下的), needConvert 就是 false,
  //       于是整块被跳过 -> 一张图都不导出、目录名也没登记 ->
  //       currentPageImage() 看到空目录直接 return false -> 插图页永远是空白。
  //
  // 修法: 正文照旧复用(省十几秒解压), 只单独确认一次"插图导出全了没有"。
  //       用 epubCountExportedImages() 数目录里已有的 img_NN.*, 不够就补导出 ——
  //       所以"导出到一半断电"的半截目录也能自愈, 而且是可重复执行的。
  // =========================================================================
  if (!needConvert) {
    char imgDir[128];
    epubImageDir(epubPath, imgDir, sizeof(imgDir));

    // ★ 判定依据改成"卡上实际有几张图", 不再依赖 imageCount() ★
    //
    // 2026-10-03 实测定位:
    //   [EPDBG] ep2.open=1 nImg=0 err=[]      <- 打开成功, 但"解析出 0 张图片"
    // 原因: 图片清单是在**提取正文**的过程中顺带建起来的 ——
    //   markImagesInXhtml() 遇到 <img> 才调 noteImage() 登记一张
    //   (epub_parser.cpp:760 / :783 / :801)。
    //   而复用正文缓存时**不跑提取正文**, 所以 imageCount() 永远是 0,
    //   旧写法 `if (nImg2 > 0)` 直接把 s_epubImgDir 清空 -> 插图页永远出不来。
    //
    // 现在改成: 先数卡上已有的 img_NN.*, 一张都没有就调 extractAllImages() 补导出,
    // 再数一次。**导出结果以"目录里实际有几个文件"为准**, 这条判据与解析器内
    // 部状态无关, 所以无论解析器怎么改都不会再被误导。
    int have = epubCountExportedImages(SD, imgDir);
    Serial.printf("[EPDBG] dir=%s 卡上已有=%d 张\n", imgDir, have);
    Serial.flush();

    if (have <= 0) {
      EpubParser ep2;   // 只读 container.xml + OPF
      const bool ep2ok = ep2.open(SD, epubPath);
      // ★ 用 ensureImageList() 而不是 imageCount() ★
      //   图片清单**不是** open() 时建好的 —— 它是在"提取正文"过程中顺带登记的
      //   (markImagesInXhtml -> noteImage)。所以复用缓存(不提取正文)时 imageCount()
      //   永远是 0。ensureImageList() 会按**和当初写缓存标记完全相同的顺序**重扫一遍
      //   各章 XHTML, 只为把清单建起来 —— 编号才能和缓存里的 @N@ 对上。
      const int nImg2 = ep2ok ? ep2.ensureImageList() : -1;
      Serial.printf("[EPDBG] ep2.open=%d nImg=%d err=[%s]\n", ep2ok ? 1 : 0, nImg2, ep2.err());
      Serial.flush();
      if (ep2ok) {
        BOOTLOG("[epub] 复用正文缓存, 卡上还没有插图 -> 补导出");
        const int okCount = ep2.extractAllImages(imgDir, onEpubProgress, nullptr);
        BOOTLOG("[epub] 插图导出 %d/%d 张 -> %s", okCount, nImg2, imgDir);
      } else {
        BOOTLOG("[epub] !! 复用缓存时打开 epub 失败, 插图无法补导出: %s", ep2.err());
      }
      ep2.close();   // 必须: 释放 manifest/spine/images 缓冲
      have = epubCountExportedImages(SD, imgDir);   // 以导出后的实际结果为准
      Serial.printf("[EPDBG] 导出后卡上=%d 张\n", have);
      Serial.flush();
    } else {
      BOOTLOG("[epub] 插图已在卡上: %s (%d 张), 跳过导出", imgDir, have);
    }

    // ★ 关键: 只要卡上真有图就登记目录 ★ 复用也要登记, 这正是插图页一直空白的根因。
    if (have > 0) {
      snprintf(s_epubImgDir, sizeof(s_epubImgDir), "%s", imgDir);
      BOOTLOG("[epub] 插图目录已登记: %s (%d 张)", s_epubImgDir, have);
    } else {
      s_epubImgDir[0] = 0;
      BOOTLOG("[epub] !! 插图一张都没导出成功, 插图页将显示空白");
    }
  }

  // ★ 诊断: 列出缓存里实际有哪些插图标记 ★
  //   位置很关键: 放在这里, 无论"刚解压完"还是"复用已有缓存"**都会跑**。
  //   (第一次我把它放在解压分支里, 结果复用缓存时根本不执行 —— 白等一轮。)
  scanCacheMarkers(cache);

  const bool ok = g_book.open(SD, cache);
  BOOTLOG("[epub] 打开缓存文本 %s: %s", ok ? "成功" : "失败", ok ? "" : g_book.err());
  return ok;
}

// ---------------------------------------------------------------------------
// 图片页: 查当前页里有没有 "\f<索引>\f" 标记
// 有就返回图片路径(写到 out), 没有返回 false
// ---------------------------------------------------------------------------
// "图片目录为空" 这条诊断的去重标志(见下面用处的说明)
static bool s_warnedNoImgDir = false;

static bool currentPageImage(char *out, size_t outLen) {
  const int n = g_book.lineCount();
  for (int i = 0; i < n; i++) {
    const char *ln = g_book.line(i);
    // 标记行形如 "@12@"
    if (!ln || ln[0] != (char)TEXT_MARKER_CHAR) {
      continue;
    }
    const int idx = atoi(ln + 1);
    if (idx < 0) {
      continue;
    }
    if (s_epubImgDir[0] == 0) {
      // 诊断: 说明这页确实是插图页, 但没有设过图片目录(例如重新开了 txt)
      //
      // ★ 去重 ★ currentPageImage() 是**每次渲染都会被调用**的, 而插图页会一直停在
      //   同一页上, 于是同一句话以每秒几次的频率刷满串口(实测刷了 278 行), 把真正
      //   有用的信息(epub 打开流程)全冲掉了。只在状态变化时打一次。
      //   用 s_epubImgDir 的内容当"去重键"放在调用方更麻烦, 这里直接用一个静态标志,
      //   并在成功拿到非空目录时复位 —— 见下面 s_epubImgDirOk 的复位点。
      if (!s_warnedNoImgDir) {
        s_warnedNoImgDir = true;
        BOOTLOG("[img] 页内有图片标记 @%d@ 但图片目录为空(不是 epub?) [只提示一次]", idx);
      }
      return false;
    }
    s_warnedNoImgDir = false;   // 目录可用了 -> 下次若再变空还会提示
    // 依次试 jpg / png
    bool tried[2] = {false, false};
    static const char *exts[2] = {".jpg", ".png"};
    for (int e = 0; e < 2; e++) {
      snprintf(out, outLen, "%s/img_%02d%s", s_epubImgDir, idx, exts[e]);
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (!g.ok()) {
        BOOTLOG("[img] 查图片存在性时等 SD 锁超时: %s", out);
        return false;
      }
      if (SD.exists(out)) {
        BOOTLOG("[img] 插图页: 标记 @%d@ -> %s", idx, out);
        return true;
      }
      tried[e] = true;
    }
    // 诊断: 标记存在但两个候选文件都不在。把目录里的实际文件名列出来(最多 8 个),
    //       一眼就能看出是"没导出"还是"命名对不上"。
    BOOTLOG("[img] !! 标记 @%d@ 对应的图片文件不存在, 期望其中之一:", idx);
    BOOTLOG("[img] !!   %s/img_%02d.jpg", s_epubImgDir, idx);
    BOOTLOG("[img] !!   %s/img_%02d.png", s_epubImgDir, idx);
    {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (g.ok()) {
        File d = SD.open(s_epubImgDir);
        if (d && d.isDirectory()) {
          int shown = 0;
          File f;
          while ((f = d.openNextFile()) && shown < 8) {
            BOOTLOG("[img] !!   目录里有: %s (%lu 字节)", f.name(), (unsigned long)f.size());
            f.close();
            shown++;
          }
          if (shown == 0) {
            BOOTLOG("[img] !!   目录是空的(说明插图没导出成功)");
          }
          d.close();
        } else {
          BOOTLOG("[img] !!   目录 %s 打不开(说明根本没建过)", s_epubImgDir);
        }
      }
    }
    (void)tried;
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 可重试的启动推进: 返回 true 表示已经进入阅读界面
//
// 每一步失败都会在屏幕上显示"具体原因 + 怎么修", 并且**不锁死**: 用户按一下
// 旋钮就会重新走一遍, 所以不插卡/忘放字库都能当场补上, 不用重新烧录。
// ---------------------------------------------------------------------------
// 主菜单: 看小说 / 听音乐 / 继续阅读
// ---------------------------------------------------------------------------
// 「继续阅读」: 从 NVS 存档恢复"上次那本书 + 那一页"。
// 注意: **不是开机自动跳转** —— 用户明确不要"被强行拉回去", 所以做成菜单里的一项,
//       由用户主动选。
static void continueLastReading(void) {
  BookState st;
  if (!stateStoreLoad(&st)) {
    uiSetStatusHint("没有存档");
    BOOTLOG("[state] 继续阅读: 没有可用存档");
    uiRender();
    return;
  }
  BOOTLOG("[state] 继续阅读: %s 第 %lu 页 (存档 offset=%lu)", st.bookPath, (unsigned long)st.page,
          (unsigned long)st.pageOffset);

  // 书可能被删了/改名的 -> 优雅降级, 不清存档(用户可能只是换了张卡)
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok() || !SD.exists(st.bookPath)) {
      uiSetStatusHint("上次的书找不到了");
      BOOTLOG("[state] !! 存档里的书不存在: %s", st.bookPath);
      uiRender();
      return;
    }
  }

  // 与选书流程共用同一套打开逻辑; pageOffset 交给它做版面校验
  //   (校验不过它会自己回第 1 页, 而不是跳到内容完全不对的地方)
  if (!openBookAtPath(st.bookPath, st.isEpub, st.page, st.pageOffset)) {
    uiSetStatusHint("打不开上次的书");
    uiRender();
    return;
  }
  uiSetStatusHint("");
  BOOTLOG("[state] 继续阅读完成: 实际停在第 %lu 页", (unsigned long)g_book.page());
}

static void handleMenuInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    uiMenuMove((int)enc);
    uiRender();
    return;
  }
  if (btn == BTN_SHORT || btn == BTN_BOOT) {
    const int sel = uiMenuSelected();
    if (sel == 0) {
      // 看小说 -> 选书
      if (s_bookCount <= 0) {
        scanBooks();   // 可能刚插上卡, 重扫一次
      }
      uiShowBooks();
      uiRender();
    } else if (sel == 1) {
      // 听音乐 -> 选曲
      openMusicPicker();
    } else {
      // ★ 继续阅读 ★
      //   注意这里是 else-if 链。原来是 `if (sel == 0) ... else ...` 两分支写法,
      //   加了第 3 项之后选到「继续阅读」会掉进音乐分支去放歌 —— 那个 bug 一并修掉。
      continueLastReading();
    }
  }
}

// ---------------------------------------------------------------------------
// 打开一本书(普通 txt 或 epub)并按需跳到指定页。
//
// 抽出来是因为有**两个**调用方:
//   ① 选书流程(handleBooksInput) —— 从头看
//   ② 「继续阅读」(continueLastReading) —— 从上一次的页码看
// 两条路必须共用同一套"打开 + 挂到界面 + 排版"逻辑, 否则很容易漂移成两套行为。
//
// startPage: 0 = 从头; >0 = 尝试跳到该页(靠 pageOffset 校验版面有没有变)。
// 成功返回 true, 此时界面已经在阅读界面。
static bool openBookAtPath(const char *path, bool isEpub, uint32_t startPage, uint32_t startOffset) {
  bool ok;
  if (isEpub) {
    ok = openEpub(path);
    if (ok) {
      uiAttachBook(&g_book);
      uiSetStatusHint("");
      const int n = g_book.loadPage(startPage);
      BOOTLOG("[epub] 打开后第 %lu 页 %d 行, 纯文本 %lu 字节", (unsigned long)startPage, n,
              (unsigned long)g_book.pageTextBytes());
      if (n <= 0) {
        ok = false;
      } else {
        // 版面校验: 存档的字节偏移和现在排出来的对不上 => 缓存被重建过, 回第 1 页
        if (startPage > 0 && startOffset != 0 && g_book.pageStartOffset() != startOffset) {
          BOOTLOG("[state] 版面变了(offset %lu != 存档 %lu) -> 回第 1 页", (unsigned long)g_book.pageStartOffset(),
                  (unsigned long)startOffset);
          g_book.loadPage(0);
        }
        uiShowReading();
        uiRender();
      }
    }
  } else {
    ok = openBook(path);   // openBook 内部切到阅读界面并停在第 1 页
  }
  if (!ok) {
    return false;
  }

  // ★ 记下"用户点开的是哪本书"(原始路径) ★
  //   必须在这里记, 不能从 g_book 反推 —— epub 打开的是缓存文本 /cache/xxx.txt,
  //   拿它当书路径存进 NVS, 用户删/换卡后就对不上了。
  snprintf(s_curBookPath, sizeof(s_curBookPath), "%s", path);
  s_curBookIsEpub = isEpub;

  // 非 epub 的跳页(openBook 只停在第 1 页)
  if (!isEpub && startPage > 0) {
    const int n = g_book.loadPage(startPage);
    if (n > 0) {
      if (startOffset != 0 && g_book.pageStartOffset() != startOffset) {
        BOOTLOG("[state] 版面变了(txt) -> 回第 1 页");
        g_book.loadPage(0);
      }
      uiRender();
    }
  }
  noteReadingProgress();
  return true;
}

// ---------------------------------------------------------------------------
// 选书: 选中后打开; 长按返回主菜单
// ---------------------------------------------------------------------------
static void handleBooksInput(int32_t enc, BtnEvent btn) {
  if (enc != 0) {
    uiBooksMove((int)enc);
    uiRender();
    return;
  }
  if (btn == BTN_LONG) {
    uiShowMenu();
    uiRender();
    return;
  }
  if (btn != BTN_SHORT && btn != BTN_BOOT) {
    return;
  }

  const int sel = uiBooksSelected();
  const char *path = bookPathAt(sel);
  if (!path) {
    uiSetStatusHint("没有这本书");
    uiRender();
    return;
  }
  const bool isEpub = bookIsEpubAt(sel);
  BOOTLOG("[book] 用户选择: %s (%s)", path, isEpub ? "epub" : "txt");

  // 打开逻辑统一收进 openBookAtPath()（「继续阅读」走的是同一个函数）
  if (!openBookAtPath(path, isEpub)) {
    // 打开失败: 回到选书列表, 由用户换一本(而不是卡在提示页)
    uiShowBooks();
    uiRender();
  }
}

// ---------------------------------------------------------------------------
// 启动失败后的"按一下重试"
//
// ★ 必须同时服务 UI_BOOT 和 UI_MESSAGE 两个界面 ★
//   启动失败时屏幕停在 UI_BOOT(启动进度清单), 而重试逻辑原先只挂在 UI_MESSAGE 上,
//   于是 UI_BOOT 的按键落进 switch 的 default 分支被直接丢弃 —— 表现就是
//   "按旋钮没有任何反应, 无法重试"。这是踩过的坑。
// ---------------------------------------------------------------------------
static void tryRetryBoot() {
  Serial.println(F("[boot] 用户请求重试启动流程"));
  Serial.flush();
  if (bootTryAdvance()) {
    Serial.println(F("[boot] 已就绪"));
  }
}

// ---------------------------------------------------------------------------
static bool bootTryAdvance() {
  // --- SD 卡 ---
  if (!s_sdReady) {
    Serial.println(F("[boot] 4/6 挂载 SD 卡"));
    Serial.flush();
    uiBootStep(3, "挂载 SD 卡", BOOT_RUN);
    s_sdReady = mountSd();
    if (!s_sdReady) {
      if (s_sdErr && strcmp(s_sdErr, "SD_FS") == 0) {
        uiBootStep(3, nullptr, BOOT_FAIL,
                   "卡已识别但挂载失败: 多半不是 FAT32。请用电脑重新格式化为 FAT32 再试。按一下旋钮重试");
      } else {
        uiBootStep(3, nullptr, BOOT_FAIL,
                   "没检测到 SD 卡: 1) 卡插紧了吗 2) MISO=GPIO13 CS=GPIO14 3) 模块 3.3V 供电 4) 换张卡试。按一下旋钮重试");
      }
      return false;
    }
    uiBootStep(3, nullptr, BOOT_OK);
    Serial.println(F("[boot] 4/6 SD 卡挂载成功"));
    Serial.flush();
  }

  // --- 中文字库 ---
  if (!s_fontReady) {
    Serial.println(F("[boot] 5/6 加载字库"));
    Serial.flush();
    uiBootStep(4, "加载中文字库", BOOT_RUN);

    // 先扫描 /fonts/ 下的 .f24, 这样字库文件叫什么都行(不用改名成 gbk24.f24)
    char fontPath[96] = {0};
    if (DotFont::findFontFile(SD, FONT_DIR, fontPath, sizeof(fontPath))) {
      BOOTLOG("[font] 在 %s 下找到字库: %s", FONT_DIR, fontPath);
    } else {
      snprintf(fontPath, sizeof(fontPath), "%s", FONT_PATH);
      BOOTLOG("[font] %s 下没有 .f24, 回退尝试 %s", FONT_DIR, fontPath);
    }

    s_fontSizeKB = 0;
    {
      SdLockGuard fg(SD_LOCK_TIMEOUT_MS);
      if (fg.ok()) {
        File ff = SD.open(fontPath, FILE_READ);
        if (ff) {
          s_fontSizeKB = (uint32_t)(ff.size() / 1024);
          ff.close();
        }
      }
    }
    s_fontReady = g_font.begin(SD, fontPath);
    uiSetFontReady(s_fontReady);
    if (!s_fontReady) {
      BOOTLOG("[font] 失败: %s", g_font.lastError());
      uiBootStep(4, nullptr, BOOT_FAIL, g_font.lastError());
      // 用自检页把关键信息直接画在屏幕上, 不依赖串口
      static char l1[48], l2[48], l3[48], l4[48], l5[48];
      snprintf(l1, sizeof(l1), "目录 %s", FONT_DIR);
      snprintf(l2, sizeof(l2), "文件 %s", fontPath);
      if (s_fontSizeKB > 0) {
        snprintf(l3, sizeof(l3), "大小 %lu KB", (unsigned long)s_fontSizeKB);
      } else {
        snprintf(l3, sizeof(l3), "文件打不开或不存在");
      }
      snprintf(l4, sizeof(l4), "原因 %s", g_font.lastError());
      snprintf(l5, sizeof(l5), "用 tools/build_font.py 生成 .f24 放入 %s/ 后按旋钮重试", FONT_DIR);
      const char *lines[5] = {l1, l2, l3, l4, l5};
      uiShowSelfTest("字库自检", lines, 5, "永");
      return false;
    }
    {
      static char okmsg[64];
      snprintf(okmsg, sizeof(okmsg), "字库 %u 字形", (unsigned)g_font.glyphCount());
      // 文件名 + 位深都显示出来。
      // bpp 特别关键: 1=黑白, 4=灰度抗锯齿 —— 渲染路径按它分流,
      // 出现"字挤在一起/只显示一半"时这是第一个要看的数字。
      const char *fslash = strrchr(fontPath, '/');
      static char fmsg[80];
      snprintf(fmsg, sizeof(fmsg), "%s  %ubpp", fslash ? fslash + 1 : fontPath, (unsigned)g_font.bpp());
      uiBootStep(4, okmsg, BOOT_OK, fmsg);
      BOOTLOG("[font] 实际使用: %s (%lu KB), %u 字形, %ux%u, %ubpp", fontPath,
              (unsigned long)s_fontSizeKB, (unsigned)g_font.glyphCount(), (unsigned)FONT_W, (unsigned)FONT_H,
              (unsigned)g_font.bpp());
    }
    Serial.println(F("[boot] 5/6 字库加载成功"));
    Serial.flush();
  }

  // --- 音频(失败不阻塞阅读) ---
  if (!s_audioTried) {
    s_audioTried = true;
    Serial.println(F("[boot] 6/6 初始化音频"));
    Serial.flush();
    uiBootStep(5, "初始化音频", BOOT_RUN);
    if (!audioInit()) {
      BOOTLOG("!! 音频初始化失败(不影响阅读)");
      uiBootStep(5, nullptr, BOOT_FAIL, "音频初始化失败(不影响阅读, 继续启动)");
    } else {
      const int n = audioScanTracks();
      if (n > 0) {
        // ★ 开机不自动播放 ★
        //   之前这里调了 audioPlayIndex(0), 结果是"一开机就响", 用户明确表示
        //   不想要这个效果。改成只扫描曲目、不上电就放; 想听就自己进主菜单选。
        //   (扫描仍然要做, 否则选曲列表是空的。)
        static char amsg[48];
        snprintf(amsg, sizeof(amsg), "音频 %d 首曲目(未播放)", n);
        uiBootStep(5, amsg, BOOT_OK);
        BOOTLOG("[audio] 扫描到 %d 首曲目, 按用户设置不自动播放", n);
        // ★ 启动喂数据任务(Core 0, 2ms 一次) ★
        //   必须在扫描之后、播放之前启动。它负责调库的 Audio::loop(),
        //   也就是"读文件 / 建解码器 / 解析文件头"的唯一入口。
        audioStartTask();
      } else {
        Serial.println(F("[audio] 没找到音乐(可放 /mp3 或 /music 目录)"));
        uiBootStep(5, "音频(无曲目)", BOOT_OK);
      }
    }
  }

  // --- 扫描书籍列表(不再自动打开第一本, 改由主菜单里自己选) ---
  BOOTLOG("[boot] 6/6 扫描书文件");
  uiBootStep(6, "扫描书文件", BOOT_RUN);
  const int nbooks = scanBooks();
  if (nbooks == 0) {
    uiBootStep(6, nullptr, BOOT_FAIL,
               "没找到书: 把 .txt(UTF-8) 或 .epub 放到 SD 卡根目录、/txt 或 /books 目录");
    return false;
  }
  {
    static char bmsg[40];
    snprintf(bmsg, sizeof(bmsg), "找到 %d 本书", nbooks);
    uiBootStep(6, bmsg, BOOT_OK);
  }
  uiBootStep(7, "就绪", BOOT_OK);
  return true;
}

// ---------------------------------------------------------------------------
// loop: 输入 + 渲染
//
// 本函数跑在 Arduino 的默认任务里(ESP32-S3 上 loopTask 钉在 Core 1)。
// 音频任务被 setAudioTaskCore(AUDIO_TASK_CORE=1) 钉在 Core 1... 两者会同核,
// 但音频任务优先级 2 高于 loopTask 的 1, 所以刷屏期间音频要读卡时会抢占我们,
// 不会因为渲染而卡住音频。UI 侧的 SPI/SD 访问一律走 sd_mutex。
// ---------------------------------------------------------------------------
void loop() {
  // ★ 阅读存档: 到点(防抖 2.5s 到期 + 内容有变)才真正写一次 NVS ★
  //   只碰 Flash, 不占 sd_mutex、不碰 SPI, 所以放在最前面很安全。
  stateStoreTick();

  // ---------------------------------------------------------------------------
  // ★ 一次性 SD 通路自检(临时诊断, 见 sd_bench.cpp) ★
  //
  // 为什么放在 loop 里而不是 mountSd() 里: 烧录后板子会自动重启, 而"启动的头 1~2 秒"
  // 正好是串口监视器还没接上的窗口 —— 启动阶段打的 [sdbench] 全被串口环形缓冲吃掉了
  // (连续三次上机都没抓到)。放在 loop 里, 监视器已就绪, 一定抓得到。
  //
  // 启动后 8 秒才跑: 等界面/音频都稳定, 且此时 SD 已挂载。
  // 卡未挂载时 sdBenchRunEarly() 只打一行、不消耗"只跑一次"标记, 所以是安全的。
  // 整个自检约 2~8 秒, 期间不刷屏 —— 音频缓冲有 655350 字节(约 3.2 秒),
  // 自检期间喂数据任务会短暂拿不到锁, 但如果它抢到了就会正常喂, 风险可控。
  // 自检跑完(s_ran 置位)后这个块永远不再进。
  //
  // 想关掉: 把这整块删掉, 或把 config.h 的 SD_BENCH_ENABLE 设 0。
  {
    static bool s_sdBenchTried = false;
    if (!s_sdBenchTried && millis() > 8000) {
      // 先把判决条件打出来 —— 否则"自检只打了触发/返回、中间一条 # 都没有"这种情况
      // 没法区分是卡没挂上、还是别的提前返回。cardType: 0=NONE 1=MMC 2=SD 3=SDHC
      const uint8_t ct = (uint8_t)SD.cardType();
      BOOTLOG("[sdbench] 触发前检查: cardType=%u (0=NONE) hasRun=%d", (unsigned)ct, sdBenchHasRun() ? 1 : 0);
      if (ct != CARD_NONE) {
        s_sdBenchTried = true;
        BOOTLOG("[sdbench] 触发 loop 侧自检(SD 已挂载)");
        sdBenchRunEarly();
        BOOTLOG("[sdbench] 自检返回");
      } else {
        s_sdBenchTried = true;
        BOOTLOG("[sdbench] SD 未挂载, 放弃自检");
      }
    }
  }

  // 编码器与按键都由 inputTask(1ms) 在后台采集, 这里只取结果。
  // 独立任务的好处: 主循环被刷屏/解压 EPUB 阻塞时, 按键不会被漏掉。
  const int32_t enc = encoderTakeDelta();
  const BtnEvent btn = buttonTakeEvent();

  // 注意: 音频喂数据**不在这里做**了。
  //   库的 Audio::loop() 由 Core 0 的专用任务(audioStartTask)每 2ms 调一次。
  //   以前放在主循环里, 会和整帧刷屏抢 sd_mutex, 造成"音频一顿一顿 + 界面卡"。
  //   audioLoop() 现在是空实现(保留仅为兼容历史调用点)。

  // 曲目放完了就自动下一首
  audioPollEndOfTrack();

  // 每 5 秒更新一次电量
  const uint32_t now = millis();
  bool batChanged = false;
  if (now >= s_batNextMs) {
    s_batNextMs = now + 5000;
    uiSetBattery(batteryRead());
    batChanged = true;
  }

  // 有音乐在放时, 每 1 秒刷新一次状态栏进度(不需要重排正文)
  static uint32_t s_clockNextMs = 0;
  bool clockTick = false;
  if (now >= s_clockNextMs) {
    s_clockNextMs = now + 1000;
    const AudioStatus as = audioGetStatus();
    clockTick = as.loaded && (as.playing || as.paused);
  }

  switch (uiScreen()) {
    case UI_READING:
      if (enc != 0 || btn != BTN_NONE) {
        handleReadingInput(enc, btn);
      } else if (batChanged) {
        uiRender();                    // 电量变化可能连带别的地方, 走整帧
      } else if (clockTick) {
        uiRenderStatusBarOnly();       // 只有音乐秒跳 -> 只刷状态栏(1.9ms vs 30.7ms)
      }
      break;

    case UI_TOC:
      // 目录里旋转=滚章节, 但同时也让音量键可用: 用长按切换
      if (enc != 0 || btn != BTN_NONE) {
        handleTocInput(enc, btn);
      } else if (clockTick) {
        uiRenderStatusBarOnly();
      }
      break;

    case UI_MUSIC:
      // 选曲列表: 旋转滚动, 短按选中播放, 长按返回
      if (enc != 0 || btn != BTN_NONE) {
        handleMusicInput(enc, btn);
      }
      break;

    case UI_MENU:
      // 主菜单: 旋转选"看小说/听音乐", 按下确定
      if (enc != 0 || btn != BTN_NONE) {
        handleMenuInput(enc, btn);
      } else if (clockTick) {
        uiRenderStatusBarOnly();       // 菜单内容没变, 只有状态栏的播放时间在跳
      }
      break;

    case UI_BOOKS:
      // 选书: 旋转滚动, 按下打开, 长按回主菜单
      if (enc != 0 || btn != BTN_NONE) {
        handleBooksInput(enc, btn);
      }
      break;

    case UI_PLAYER:
      // 播放界面: 旋转调音量, 按下暂停, 长按选曲。
      // 每秒刷新进度/时间 —— 但**只刷变化的那一条**(约 240x62, 几毫秒),
      // 不再整帧重推(153600 字节, 40MHz 下 30.7ms)。这就是"播放时界面卡"的修法。
      if (enc != 0 || btn != BTN_NONE) {
        handlePlayerInput(enc, btn);
      } else if (clockTick) {
        uiRenderStatusBarOnly();
      }
      break;

    case UI_MESSAGE:
    case UI_BOOT:
      // 启动没走完: 按一下旋钮就重试整条流程(SD/字库/找书), 不用重新烧录。
      // UI_BOOT 也必须处理 —— 启动失败时屏幕正是停在 UI_BOOT。
      if (btn != BTN_NONE) {
        tryRetryBoot();
      }
      break;

    default:
      break;
  }

  delay(4);  // 让出 CPU, 避免空转和看门狗
}