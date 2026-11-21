/*
 * state_store.cpp — 存档实现(见 state_store.h 的完整说明)
 *
 * 只用 Preferences(NVS), 不 include sd_mutex.h / 不碰 SD / 不拿任何锁。
 * NVS 自己管 Flash 磨损均衡, 我们又加了三道闸(防抖 / 最小间隔 / 内容没变不写),
 * 正常阅读节奏下每分钟最多几次写入。
 */
// ★ 先 config.h 再 state_store.h ★
//   config.h 里那节的宏必须先生效; 反过来的话 state_store.h 的 #ifndef 兜底值会先占位,
//   config.h 里的设置就不起作用了。
#include "config.h"

#include "state_store.h"

#include <Preferences.h>

// 诊断直接走 Serial: log_i/log_e 受 Arduino Core Debug Level 控制(默认 0 什么都不打)
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                       \
  do {                                          \
    Serial.printf(fmt "\n", ##__VA_ARGS__);     \
    Serial.flush();                             \
  } while (0)
#endif

#if STATE_STORE_ENABLE

// NVS 命名空间名(上限 15 字符)
static const char *STATE_NVS_NS = "bookstate";
static const char *STATE_NVS_KEY = "st";
static const uint16_t STATE_MAGIC = 0x4253;   // 'B''S'

// 落盘用的紧凑结构(定长, 一次 putBytes 写完)
struct StoredBlob {
  uint16_t magic;
  uint8_t version;
  uint8_t flags;      // bit0 = isEpub
  uint32_t page;
  uint32_t pageOffset;   // 该页起始字节偏移(校验用)
  uint8_t screen;
  char path[STATE_STORE_PATH_MAX];
};

static Preferences s_prefs;
static bool s_available = false;

// 内存镜像 = "想存的内容"。Note* 改它, Tick/Flush 负责落盘。
static BookState s_cur;
static bool s_haveCur = false;      // s_cur 是否有意义
static bool s_dirty = false;        // 与"上次落盘的内容"不同
static bool s_savedEver = false;    // 曾经成功落盘过(或用内存镜像确认过)
static uint32_t s_lastChangeMs = 0; // 最后一次变更时刻
static uint32_t s_lastWriteMs = 0;  // 最后一次成功写入时刻
static uint32_t s_writeCount = 0;   // 本次开机写了几次(诊断用)

static void fillBlob(const BookState &st, StoredBlob *b) {
  memset(b, 0, sizeof(*b));
  b->magic = STATE_MAGIC;
  b->version = STATE_STORE_VERSION;
  b->flags = st.isEpub ? 1 : 0;
  b->page = st.page;
  b->pageOffset = st.pageOffset;
  b->screen = st.screen;
  snprintf(b->path, sizeof(b->path), "%s", st.bookPath);
}

// 内容是否与上一次落盘的一致(避免无谓的 Flash 写)
static bool blobEquals(const StoredBlob &a, const StoredBlob &b) {
  return a.magic == b.magic && a.version == b.version && a.flags == b.flags && a.page == b.page &&
         a.pageOffset == b.pageOffset && a.screen == b.screen && strncmp(a.path, b.path, sizeof(a.path)) == 0;
}

static bool writeNow(const char *why) {
  if (!s_available || !s_haveCur) {
    return false;
  }
  const uint32_t now = millis();
  if (now - s_lastWriteMs < STATE_STORE_MIN_WRITE_GAP_MS && s_lastWriteMs != 0) {
    return false;   // 太频繁, 留给 Tick 稍后写
  }

  StoredBlob b;
  fillBlob(s_cur, &b);

  // 已经落盘的内容一样 -> 什么都不做(保护 Flash)
  StoredBlob old;
  const size_t got = s_prefs.getBytes(STATE_NVS_KEY, &old, sizeof(old));
  if (got == sizeof(old) && blobEquals(old, b)) {
    s_dirty = false;
    return true;
  }

  const uint32_t t0 = micros();
  const size_t n = s_prefs.putBytes(STATE_NVS_KEY, &b, sizeof(b));
  const uint32_t dt = micros() - t0;
  if (n != sizeof(b)) {
    BOOTLOG("[state] !! 存档写入失败(putBytes 返回 %u, 期望 %u)", (unsigned)n, (unsigned)sizeof(b));
    return false;
  }
  s_dirty = false;
  s_lastWriteMs = now;
  s_savedEver = true;
  s_writeCount++;
  BOOTLOG("[state] 存档(%s): %s 第 %lu 页, 界面=%u, 版本=%u, 写入 %lu us (本次开机第 %lu 次)", why,
          s_cur.bookPath, (unsigned long)s_cur.page, (unsigned)s_cur.screen, (unsigned)STATE_STORE_VERSION,
          (unsigned long)dt, (unsigned long)s_writeCount);
  return true;
}

bool stateStoreBegin() {
  if (s_available) {
    return true;
  }
  if (!s_prefs.begin(STATE_NVS_NS, false)) {
    BOOTLOG("[state] !! NVS 打不开(命名空间 %s), 存档功能关闭(其余功能不受影响)", STATE_NVS_NS);
    s_available = false;
    return false;
  }
  s_available = true;

  // 开机先把已有存档读进内存镜像, 这样后面"内容没变就不写"才判得准
  BookState st;
  if (stateStoreLoad(&st)) {
    s_cur = st;
    s_haveCur = true;
    s_savedEver = true;
  }
  BOOTLOG("[state] NVS 就绪(namespace=%s, 存档版本=%u)%s", STATE_NVS_NS, (unsigned)STATE_STORE_VERSION,
          s_savedEver ? "" : ", 当前没有存档");
  return true;
}

bool stateStoreAvailable() {
  return s_available;
}

bool stateStoreHasSaved() {
  return s_savedEver || s_haveCur;
}

bool stateStoreLoad(BookState *out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (!s_available) {
    return false;
  }

  StoredBlob b;
  const size_t n = s_prefs.getBytes(STATE_NVS_KEY, &b, sizeof(b));
  if (n == 0) {
    return false;   // 没有存档
  }
  if (n != sizeof(b)) {
    BOOTLOG("[state] 存档长度异常(%u != %u), 按损坏处理(已忽略)", (unsigned)n, (unsigned)sizeof(b));
    return false;
  }
  if (b.magic != STATE_MAGIC) {
    BOOTLOG("[state] 存档 magic 不对(0x%04X), 按损坏处理(已忽略)", (unsigned)b.magic);
    return false;
  }
  if (b.version != STATE_STORE_VERSION) {
    // 版本不符: 不猜、不迁移, 当作没有存档 —— 用户重新选一次书即可
    BOOTLOG("[state] 存档版本不符(存档=%u, 程序=%u), 已忽略", (unsigned)b.version, (unsigned)STATE_STORE_VERSION);
    return false;
  }

  // 第 1 层校验: 路径必须是可打印的、以 '/' 开头、且以 0 结尾
  b.path[sizeof(b.path) - 1] = 0;
  if (b.path[0] != '/') {
    BOOTLOG("[state] 存档路径不合法([]), 已忽略", b.path);
    return false;
  }
  for (size_t i = 0; b.path[i]; i++) {
    const uint8_t c = (uint8_t)b.path[i];
    if (c < 0x20 || c == 0x7F) {
      BOOTLOG("[state] 存档路径含控制字符, 已忽略");
      return false;
    }
  }

  snprintf(out->bookPath, sizeof(out->bookPath), "%s", b.path);
  out->page = b.page;
  out->pageOffset = b.pageOffset;
  out->isEpub = (b.flags & 1) != 0;
  out->screen = b.screen;
  out->version = b.version;
  out->valid = true;

  BOOTLOG("[state] 读档: %s(%s) 第 %lu 页, 上次界面=%u, 版本=%u", out->bookPath, out->isEpub ? "epub" : "txt",
          (unsigned long)out->page, (unsigned)out->screen, (unsigned)out->version);
  return true;
}

void stateStoreNoteBook(const char *path, bool isEpub, uint32_t page, uint32_t pageOffset) {
  if (!s_available) {
    return;
  }
  if (!path || path[0] != '/') {
    return;   // 没有合法路径 = 没有在看书, 不清存档(保留上一次的记录)
  }
  const size_t len = strlen(path);
  if (len >= STATE_STORE_PATH_MAX) {
    BOOTLOG("[state] 路径太长(%u 字节), 本页不记入存档: %s", (unsigned)len, path);
    return;
  }

  char cur[STATE_STORE_PATH_MAX];
  snprintf(cur, sizeof(cur), "%s", path);
  const bool same = s_haveCur && strncmp(s_cur.bookPath, cur, sizeof(s_cur.bookPath)) == 0 &&
                    s_cur.isEpub == isEpub && s_cur.page == page && s_cur.pageOffset == pageOffset;
  if (same) {
    return;   // 没变化, 不标脏 -> 也就不会写 Flash
  }
  const uint8_t prevScreen = s_haveCur ? s_cur.screen : 0;

  memset(&s_cur, 0, sizeof(s_cur));
  snprintf(s_cur.bookPath, sizeof(s_cur.bookPath), "%s", cur);
  s_cur.isEpub = isEpub;
  s_cur.page = page;
  s_cur.pageOffset = pageOffset;
  s_cur.version = STATE_STORE_VERSION;
  s_cur.valid = true;
  s_cur.screen = prevScreen;   // 换书/翻页不影响"上次界面"
  s_haveCur = true;
  s_dirty = true;
  s_lastChangeMs = millis();
}

void stateStoreNoteScreen(uint8_t screen) {
  if (!s_available || !s_haveCur) {
    return;   // 没有书就不记界面(避免存下"没有书"的状态)
  }
  if (s_cur.screen == screen) {
    return;
  }
  s_cur.screen = screen;
  s_dirty = true;
  s_lastChangeMs = millis();
}

void stateStoreTick() {
  if (!s_available || !s_dirty) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastChangeMs < STATE_STORE_DEBOUNCE_MS) {
    return;   // 还在防抖窗口里(翻页期间不写)
  }
  if (now - s_lastWriteMs < STATE_STORE_MIN_WRITE_GAP_MS && s_lastWriteMs != 0) {
    return;
  }
  writeNow("防抖");
}

void stateStoreFlush() {
  if (!s_available || !s_dirty) {
    return;
  }
  if (!writeNow("立即")) {
    // 没写成(太频繁/失败) -> 留在脏状态, 由 Tick 稍后补写
    return;
  }
}

void stateStoreClear() {
  if (!s_available) {
    return;
  }
  s_prefs.remove(STATE_NVS_KEY);
  memset(&s_cur, 0, sizeof(s_cur));
  s_haveCur = false;
  s_dirty = false;
  s_savedEver = false;
  BOOTLOG("[state] 存档已清除");
}

#else   // STATE_STORE_ENABLE == 0
// 关掉时给出一组空实现, 让上层接线代码一行都不用改
bool stateStoreBegin() {
  return false;
}
bool stateStoreAvailable() {
  return false;
}
bool stateStoreHasSaved() {
  return false;
}
bool stateStoreLoad(BookState *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
  }
  return false;
}
void stateStoreNoteBook(const char *, bool, uint32_t, uint32_t) {}
void stateStoreNoteScreen(uint8_t) {}
void stateStoreTick() {}
void stateStoreFlush() {}
void stateStoreClear() {}
#endif  // STATE_STORE_ENABLE
