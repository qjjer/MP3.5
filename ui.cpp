/*
 * ui.cpp — 界面渲染
 *
 * 所有绘制都先画进 PSRAM 里的整屏画布, 最后统一由 displayFlush() 刷上去。
 * 画汉字需要从 SD 读字形, 所以绘制区间要持 sd_mutex; 刷屏由 displayFlush()
 * 自己持锁。两者是分开的两段临界区, 不会嵌套, 也不会让音频任务饿死。
 */
#include "ui.h"
#include "audio_player.h"
#include "font_builtin.h"
#include "image_view.h"
#include "config.h"
#include "sd_mutex.h"

#include <stdio.h>
#include <string.h>

// 字库(全局唯一实例, 由主程序 begin)
extern DotFont g_font;

static UiScreen s_screen = UI_BOOT;
static TxtReader *s_book = nullptr;

// ★ 是否需要"整页字形预取" ★
// 打开一本书、翻页、跳章节之后置 true; uiRender() 在进 sd_mutex 之前消费一次。
// 预取把该页所有字形按"字库内位置升序"一趟顺序读进来, 之后绘制阶段全部命中
// 缓存、不再碰 SD —— 这是"翻页卡死"的根治办法(单次随机读实测 286~320ms)。
static bool s_needPrefetch = false;

// 让主程序(翻页/开书/跳章)通知"这一页的字形需要预取"
void uiRequestGlyphPrefetch(void) {
  s_needPrefetch = true;
}
static bool s_fontReady = false;
static BatteryInfo s_bat;

static char s_msgTitle[32] = {0};
static char s_msgDetail[128] = {0};
static char s_hint[24] = {0};

static int s_tocSel = 0;
static int s_tocTop = 0;

// ---- 启动进度状态 ----
static char s_bootText[BOOT_MAX_STEPS][32];
static BootState s_bootState[BOOT_MAX_STEPS];
static int s_bootCount = 0;
static char s_bootDetail[128] = {0};

// 主程序注册的"取当前页图片路径"回调: 返回 true 表示这一页是插图
static bool (*s_pageImageFn)(char *out, size_t outLen) = nullptr;

void uiSetPageImageProvider(bool (*fn)(char *, size_t)) {
  s_pageImageFn = fn;
}

// ---------------------------------------------------------------------------
void uiInit() {
  s_screen = UI_BOOT;
}

void uiSetMessage(const char *title, const char *detail) {
  strncpy(s_msgTitle, title ? title : "", sizeof(s_msgTitle) - 1);
  s_msgTitle[sizeof(s_msgTitle) - 1] = 0;
  strncpy(s_msgDetail, detail ? detail : "", sizeof(s_msgDetail) - 1);
  s_msgDetail[sizeof(s_msgDetail) - 1] = 0;
}

void uiShowMessage() {
  s_screen = UI_MESSAGE;
}

void uiShowReading() {
  s_screen = UI_READING;
}

void uiShowToc() {
  s_screen = UI_TOC;
  s_tocSel = 0;
  s_tocTop = 0;
}

UiScreen uiScreen() {
  return s_screen;
}

void uiAttachBook(TxtReader *book) {
  s_book = book;
}

void uiSetFontReady(bool ready) {
  s_fontReady = ready;
}

void uiSetBattery(const BatteryInfo &b) {
  s_bat = b;
}

void uiSetStatusHint(const char *hint) {
  strncpy(s_hint, hint ? hint : "", sizeof(s_hint) - 1);
  s_hint[sizeof(s_hint) - 1] = 0;
}

void uiTocMove(int delta) {
  if (!s_book) {
    return;
  }
  const int n = s_book->chapterCount();
  if (n <= 0) {
    return;
  }
  s_tocSel += delta;
  if (s_tocSel < 0) {
    s_tocSel = 0;
  }
  if (s_tocSel > n - 1) {
    s_tocSel = n - 1;
  }
  const int visible = TOC_ROWS;
  if (s_tocSel < s_tocTop) {
    s_tocTop = s_tocSel;
  }
  if (s_tocSel >= s_tocTop + visible) {
    s_tocTop = s_tocSel - visible + 1;
  }
}

int uiTocSelected() {
  return s_tocSel;
}

void uiTocReset() {
  s_tocSel = 0;
  s_tocTop = 0;
}

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------
static void drawStatusBar(Canvas16 *c) {
  const int16_t y0 = TFT_H - STATUS_BAR_H;

  // 分隔线
  c->hLine(0, y0, TFT_W, COL_DIM);
  c->fillRect(0, y0 + 1, TFT_W, STATUS_BAR_H - 1, COL_BG);

  // 字库不可用时, 中文一个字都画不出来 -> 状态栏会全空。
  // 这时改用内置 ASCII 显示最关键的状态, 免得用户以为死机了。
  if (!s_fontReady) {
    char msg[32];
    if (s_bat.valid) {
      snprintf(msg, sizeof(msg), "NO FONT  BAT %u%%", (unsigned)s_bat.percent);
    } else {
      snprintf(msg, sizeof(msg), "NO FONT (put .f24 in /fonts)");
    }
    builtinDrawText(*c, 2, y0 + 2, msg, COL_ACCENT, COL_BG, 2);
    return;
  }

  // 电量(左)
  char buf[32];
  if (s_bat.valid) {
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_bat.percent);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y0 + 3, buf, COL_DIM, COL_BG);

  // 页码 + 百分比(中)
  // 翻页/跳章时自动请求预取: 用"本页首偏移"做变化检测, 不依赖主程序配合。
  // (主程序也可以直接调 uiRequestGlyphPrefetch())
  if (s_book && s_book->isOpen()) {
    static uint32_t s_lastPageOff = 0xFFFFFFFFu;
    const uint32_t offNow = s_book->pageStartOffset();
    if (offNow != s_lastPageOff) {
      s_lastPageOff = offNow;
      s_needPrefetch = true;   // 本帧先按需读, 下一帧开始就命中缓存
    }
  }

  if (s_book && s_book->isOpen()) {
    const int ch = s_book->currentChapter();
    const uint32_t pg = s_book->page() + 1;
    if (ch >= 0) {
      snprintf(buf, sizeof(buf), "%luP %u%%", (unsigned long)pg, (unsigned)s_book->percent());
    } else {
      snprintf(buf, sizeof(buf), "%luP %u%%", (unsigned long)pg, (unsigned)s_book->percent());
    }
  } else {
    buf[0] = 0;
  }
  if (buf[0]) {
    const int16_t w = g_font.measureUtf8(buf) / 2;  // 缩小字是半宽
    g_font.drawUtf8Small(*c, (TFT_W - w) / 2, y0 + 3, buf, COL_DIM, COL_BG);
  }

  // 右侧提示: 有音乐在放就优先显示播放状态, 否则显示普通提示
  {
    char hint[40] = {0};
    const AudioStatus as = audioGetStatus();
    if (as.loaded && (as.playing || as.paused)) {
      const uint32_t mm = as.elapsedSec / 60;
      const uint32_t ss = as.elapsedSec % 60;
      snprintf(hint, sizeof(hint), "%s %lu:%02lu V%u", as.paused ? "||" : ">", (unsigned long)mm, (unsigned long)ss,
               (unsigned)as.volume);
    } else if (s_hint[0]) {
      snprintf(hint, sizeof(hint), "%s", s_hint);
    }
    // ★ 阅读界面且什么都没显示时, 用这一格提示"怎么退回主菜单" ★
    //   为什么放这里: 阅读界面 11 行正文把屏幕占满了, 底部没有空行可以放提示;
    //   而状态栏右侧这一格原本就是空的(没音乐、没临时提示时)。
    //   只在阅读界面显示 —— 别的界面(目录/选书/播放)底部本来就有常驻提示文字。
    //   加这个是因为"回主菜单"以前根本没有入口, 用户会以为被困住了。
    if (!hint[0] && s_screen == UI_READING) {
      snprintf(hint, sizeof(hint), "长按不放=主菜单");
    }
    if (hint[0]) {
      const int16_t w = g_font.measureUtf8(hint) / 2;
      g_font.drawUtf8Small(*c, TFT_W - TEXT_MARGIN_X - w, y0 + 3, hint, COL_ACCENT, COL_BG);
    }
  }
}

static void drawReading(Canvas16 *c) {
  c->fill(COL_BG);

  int16_t y = TEXT_TOP_Y;

  if (!s_fontReady) {
    // 字库没加载成功: 用提示界面代替
    return;
  }

  // 插图页: 整页画图
  if (s_pageImageFn) {
    char imgPath[192];
    if (s_pageImageFn(imgPath, sizeof(imgPath))) {
      if (!imageDrawToCanvas(c, imgPath)) {
        g_font.drawUtf8(*c, TEXT_MARGIN_X, 120, "插图解码失败", COL_FG, COL_BG);
      }
      drawStatusBar(c);
      return;
    }
  }

  if (s_book && s_book->isOpen()) {
    // 章节标题(居中, 若有)
    const int ch = s_book->currentChapter();
    if (ch >= 0 && s_book->pageStartOffset() == s_book->chapter(ch)->offset) {
      const char *title = s_book->chapter(ch)->title;
      const int16_t tw = g_font.measureUtf8(title);
      g_font.drawUtf8(*c, (TFT_W - tw) / 2, y, title, COL_ACCENT, COL_BG);
      y += LINE_H + 2;
    }

    const int n = s_book->lineCount();
    const int rows = (n < ROWS) ? n : ROWS;

    // 注意: 整页字形预取**不在这里做** —— 它在 uiRender() 里、进这把锁之前完成
    // (见 uiRender 开头的 s_needPrefetch 段)。原因是 prefetchUtf8() 内部自己会
    // 拿 sd_mutex, 在这里再拿一次会嵌套持锁, 让音频任务白等。
    for (int i = 0; i < rows; i++) {
      g_font.drawUtf8(*c, TEXT_MARGIN_X, y, s_book->line(i), COL_FG, COL_BG);
      y += LINE_H;
    }
  }

  drawStatusBar(c);
}

static void drawToc(Canvas16 *c) {
  c->fill(COL_BG);

  // 标题
  const char *title = "目录";
  const int16_t tw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 4, title, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 4 + FONT_H + 2, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  const int n = s_book ? s_book->chapterCount() : 0;
  int16_t y = 4 + FONT_H + 6;

  if (n == 0) {
    g_font.drawUtf8(*c, TEXT_MARGIN_X, y, "没找到章节标记", COL_DIM, COL_BG);
    drawStatusBar(c);
    return;
  }

  for (int i = 0; i < TOC_ROWS; i++) {
    const int idx = s_tocTop + i;
    if (idx >= n) {
      break;
    }
    const bool sel = (idx == s_tocSel);
    if (sel) {
      c->fillRect(0, y - 1, TFT_W, FONT_H + 2, COL_ACCENT);
    }
    // 只显示标题的前若干字, 避免压到右边
    char line[20];
    strncpy(line, s_book->chapter(idx)->title, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 3, line, sel ? COL_BG : COL_FG, sel ? COL_ACCENT : COL_BG);
    y += FONT_H + 2;
  }

  // 底部操作提示(没串口也能看懂)。
  //   特意把"长按不放=主菜单"写出来: 这个操作以前没有, 用户会以为进了目录就出不去
  //   (只能回正文)。"长按"和"长按不放"是两个不同动作, 提示里必须区分清楚。
  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 4, "旋转=选择 按下=跳转", COL_DIM, COL_BG);
  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 4 + FONT_H, "长按=返回 长按不放=主菜单", COL_DIM, COL_BG);
  drawStatusBar(c);
}

// ---------------------------------------------------------------------------
// 选曲列表
// ---------------------------------------------------------------------------
static int s_musicSel = 0;
static int s_musicTop = 0;
static int s_musicCurrent = -1;   // 正在播放的那一首(用于标记)
static int (*s_trackCountFn)(void) = nullptr;
static const char *(*s_trackTitleFn)(int) = nullptr;

void uiSetTrackProvider(int (*countFn)(void), const char *(*titleFn)(int)) {
  s_trackCountFn = countFn;
  s_trackTitleFn = titleFn;
}

void uiShowMusic(void) {
  s_screen = UI_MUSIC;
  const int n = s_trackCountFn ? s_trackCountFn() : 0;
  if (s_musicSel >= n) {
    s_musicSel = n > 0 ? n - 1 : 0;
  }
  if (s_musicSel < 0) {
    s_musicSel = 0;
  }
}

void uiMusicMove(int delta) {
  const int n = s_trackCountFn ? s_trackCountFn() : 0;
  if (n <= 0) {
    return;
  }
  s_musicSel += delta;
  if (s_musicSel < 0) {
    s_musicSel = 0;
  }
  if (s_musicSel >= n) {
    s_musicSel = n - 1;
  }
}

int uiMusicSelected(void) {
  return s_musicSel;
}

void uiMusicSetCurrent(int index) {
  s_musicCurrent = index;
}

static void drawMusic(Canvas16 *c) {
  c->fill(COL_BG);

  const int n = s_trackCountFn ? s_trackCountFn() : 0;

  // 标题栏: 曲目数
  char title[24];
  snprintf(title, sizeof(title), "选曲 %d/%d", n > 0 ? s_musicSel + 1 : 0, n);
  const int16_t tw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 4, title, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 4 + FONT_H + 2, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  int16_t y = 4 + FONT_H + 6;

  if (n == 0) {
    g_font.drawUtf8(*c, TEXT_MARGIN_X, y, "没找到音乐文件", COL_DIM, COL_BG);
    drawStatusBar(c);
    return;
  }

  // 让选中项尽量保持在可见范围中间
  if (s_musicSel < s_musicTop) {
    s_musicTop = s_musicSel;
  }
  if (s_musicSel >= s_musicTop + TOC_ROWS) {
    s_musicTop = s_musicSel - TOC_ROWS + 1;
  }

  for (int i = 0; i < TOC_ROWS; i++) {
    const int idx = s_musicTop + i;
    if (idx >= n) {
      break;
    }
    const bool sel = (idx == s_musicSel);

    if (sel) {
      c->fillRect(0, y - 1, TFT_W, FONT_H + 2, COL_ACCENT);
    }

    // 正在播放的标一个 ">"(用 ASCII, 不依赖字库里的特殊符号)
    const bool playing = (idx == s_musicCurrent);
    const uint16_t fg = sel ? COL_BG : COL_FG;
    const uint16_t bg = sel ? COL_ACCENT : COL_BG;
    g_font.drawUtf8Small(*c, 2, y + 3, playing ? ">" : " ", fg, bg);

    char line[18];
    const char *t = s_trackTitleFn ? s_trackTitleFn(idx) : "";
    if (!t) {
      t = "?";
    }
    strncpy(line, t, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    g_font.drawUtf8Small(*c, 2 + FONT_W / 2 + 2, y + 3, line, fg, bg);
    y += FONT_H + 2;
  }

  drawStatusBar(c);
}

// ---------------------------------------------------------------------------
// 主菜单 + 选书列表
//
// 为什么需要: 原来启动成功就直接进正文, 启动失败就卡在启动界面 —— 而音乐面板的
// 入口只在"阅读界面"分支里处理。结果是**启动没成功时音乐 UI 根本触发不了**。
// 现在改成: 启动流程结束后(无论成功与否)都进主菜单, 由用户选看小说还是听音乐。
// ---------------------------------------------------------------------------
static int s_menuSel = 0;
static int s_bookSel = 0;
static int s_bookTop = 0;
static int (*s_bookCountFn)(void) = nullptr;
static const char *(*s_bookTitleFn)(int) = nullptr;

// ---- 主菜单第 3 项: "继续阅读"(上次看的书 + 页码, 存在 NVS 里) ----
// 没有存档时该项画成灰色, 由主程序判断 uiMenuContinueAvailable() 后决定要不要响应。
static bool s_contAvail = false;
static char s_contName[40] = {0};    // 书的显示名(只剩基名, 去掉扩展名)
static uint32_t s_contPage = 0;

void uiSetContinueBook(const char *bookPath, uint32_t page) {
  if (bookPath && bookPath[0] == '/') {
    const char *base = strrchr(bookPath, '/');
    base = base ? (base + 1) : bookPath;
    snprintf(s_contName, sizeof(s_contName), "%s", base);
    char *dot = strrchr(s_contName, '.');
    if (dot && dot != s_contName) {
      *dot = 0;   // 去掉 .txt/.epub
    }
    s_contPage = page;
    s_contAvail = true;
  } else {
    s_contName[0] = 0;
    s_contPage = 0;
    s_contAvail = false;
  }
}

bool uiMenuContinueAvailable(void) {
  return s_contAvail;
}

void uiSetBookProvider(int (*countFn)(void), const char *(*titleFn)(int)) {
  s_bookCountFn = countFn;
  s_bookTitleFn = titleFn;
}

void uiShowMenu(void) {
  s_screen = UI_MENU;
}

void uiMenuMove(int delta) {
  s_menuSel += delta;
  if (s_menuSel < 0) {
    s_menuSel = 0;
  }
  if (s_menuSel > UI_MENU_ITEM_CONTINUE) {
    s_menuSel = UI_MENU_ITEM_CONTINUE;
  }
}

int uiMenuSelected(void) {
  return s_menuSel;
}

void uiShowBooks(void) {
  s_screen = UI_BOOKS;
  const int n = s_bookCountFn ? s_bookCountFn() : 0;
  if (s_bookSel >= n) {
    s_bookSel = n > 0 ? n - 1 : 0;
  }
  if (s_bookSel < 0) {
    s_bookSel = 0;
  }
}

void uiBooksMove(int delta) {
  const int n = s_bookCountFn ? s_bookCountFn() : 0;
  if (n <= 0) {
    return;
  }
  s_bookSel += delta;
  if (s_bookSel < 0) {
    s_bookSel = 0;
  }
  if (s_bookSel >= n) {
    s_bookSel = n - 1;
  }
}

int uiBooksSelected(void) {
  return s_bookSel;
}

// 按可用宽度截断后绘制小字(用于"继续阅读"右边显示书名, 不压到屏幕外)
static void drawFittedSmall(Canvas16 *c, int16_t x, int16_t y, const char *s, int16_t maxW, uint16_t fg,
                            uint16_t bg) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", s ? s : "");
  while (buf[0] && (g_font.measureUtf8(buf) / 2) > maxW) {
    int len = (int)strlen(buf);
    while (len > 0 && ((uint8_t)buf[len - 1] & 0xC0) == 0x80) {
      len--;   // 退到 UTF-8 字符边界
    }
    if (len > 0) {
      len--;
    }
    buf[len] = 0;
  }
  if (buf[0]) {
    g_font.drawUtf8Small(*c, x, y, buf, fg, bg);
  }
}

static void drawMenu(Canvas16 *c) {
  c->fill(COL_BG);

  const char *title = "主菜单";
  const int16_t tw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 40, title, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 40 + FONT_H + 4, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  // 第 3 项 = 继续阅读(上次的书 + 页码, 存在 NVS 里)
  static const char *items[UI_MENU_ITEM_COUNT] = {"看小说", "听音乐", "继续阅读"};
  int16_t y = 40 + FONT_H + 8;
  for (int i = 0; i < UI_MENU_ITEM_COUNT; i++) {
    const bool sel = (i == s_menuSel);
    const bool cont = (i == UI_MENU_ITEM_CONTINUE);
    const bool enabled = (!cont || s_contAvail);   // 没有存档时"继续阅读"是灰的

    if (sel) {
      c->fillRect(0, y - 2, TFT_W, FONT_H + 6, enabled ? COL_ACCENT : COL_DIM);
    }
    const uint16_t fg = sel ? COL_BG : (enabled ? COL_FG : COL_DIM);
    const uint16_t bg = sel ? (enabled ? COL_ACCENT : COL_DIM) : COL_BG;
    g_font.drawUtf8(*c, TEXT_MARGIN_X + 6, y, items[i], fg, bg);

    if (cont) {
      // 右边用半高小字显示"书名 第N页"; 没存档时提示"没有阅读记录"
      const int16_t lx = TEXT_MARGIN_X + 6 + FONT_W * 4 + 8;
      const int16_t maxW = (int16_t)(TFT_W - lx - TEXT_MARGIN_X);
      if (s_contAvail) {
        char info[56];
        snprintf(info, sizeof(info), "%s 第%lu页", s_contName, (unsigned long)(s_contPage + 1));
        drawFittedSmall(c, lx, y + 6, info, maxW, sel ? COL_BG : COL_DIM, bg);
      } else {
        drawFittedSmall(c, lx, y + 6, "没有阅读记录", maxW, sel ? COL_BG : COL_DIM, bg);
      }
    }
    y += FONT_H + 8;
  }

  // 底部提示操作方式(没串口也能看懂)
  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, TFT_H - STATUS_BAR_H - FONT_H - 8, "旋转=选择  按下=确定", COL_DIM,
                       COL_BG);
  drawStatusBar(c);
}

static void drawBooks(Canvas16 *c) {
  c->fill(COL_BG);

  const int n = s_bookCountFn ? s_bookCountFn() : 0;
  char title[24];
  snprintf(title, sizeof(title), "选书 %d/%d", n > 0 ? s_bookSel + 1 : 0, n);
  const int16_t tw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 4, title, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 4 + FONT_H + 2, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  int16_t y = 4 + FONT_H + 6;

  if (n == 0) {
    g_font.drawUtf8(*c, TEXT_MARGIN_X, y, "卡里没找到书", COL_DIM, COL_BG);
    g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + FONT_H + 4, "放 .txt(UTF-8) 或 .epub", COL_DIM, COL_BG);
    drawStatusBar(c);
    return;
  }

  // 末尾留一行给"返回"
  const int visible = TOC_ROWS - 1;
  if (s_bookSel < s_bookTop) {
    s_bookTop = s_bookSel;
  }
  if (s_bookSel >= s_bookTop + visible) {
    s_bookTop = s_bookSel - visible + 1;
  }
  if (s_bookTop < 0) {
    s_bookTop = 0;
  }

  for (int i = 0; i < visible; i++) {
    const int idx = s_bookTop + i;
    if (idx >= n) {
      break;
    }
    const bool sel = (idx == s_bookSel);
    if (sel) {
      c->fillRect(0, y - 1, TFT_W, FONT_H + 2, COL_ACCENT);
    }
    char line[14];
    const char *t = s_bookTitleFn ? s_bookTitleFn(idx) : "";
    if (!t) {
      t = "?";
    }
    strncpy(line, t, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 3, line, sel ? COL_BG : COL_FG, sel ? COL_ACCENT : COL_BG);
    y += FONT_H + 2;
  }

  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 4, "长按=返回主菜单", COL_DIM, COL_BG);
  drawStatusBar(c);
}

// ---------------------------------------------------------------------------
// 播放界面: 曲名 + 进度条 + 音量条
//
// 每次绘制现取 AudioStatus, 所以进度会跟着主循环的定时刷新一起走。
// ---------------------------------------------------------------------------
void uiShowPlayer(void) {
  // 诊断: 进入播放界面时打一行, 用来和 [audio] 的事件对照先后次序。
  // (排查"在选曲界面能放、一进播放界面就没声"时, 靠这行确认到底是
  //  "切界面导致停播" 还是 "音频自己先停了/被切歌了"。)
  Serial.printf("[ui] 进入播放界面: 曲目=%d/%d 播放中=%d 已播=%lu秒\n", audioGetStatus().trackIndex + 1,
                audioGetStatus().trackCount, audioGetStatus().playing ? 1 : 0,
                (unsigned long)audioGetStatus().elapsedSec);
  Serial.flush();
  s_screen = UI_PLAYER;
}

// 画一条横向长条(进度/音量通用): 外框 + 内部填充
static void drawBar(Canvas16 *c, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fillCol) {
  c->fillRect(x, y, w, h, COL_DIM);                    // 外框(暗色)
  const int16_t inner = (int16_t)((w - 2) * (pct > 100 ? 100 : pct) / 100);
  if (inner > 0) {
    c->fillRect(x + 1, y + 1, inner, h - 2, fillCol);
  }
}

// 播放界面里"每秒都会变"的那部分: 进度条 + 时间 + 音量 + 状态栏。
//
// ★ 为什么要把这部分拆出来 ★
//   参考项目的 flush 回调只推 LVGL 交给它的"脏区域":
//       tft.setAddrWindow(area->x1, area->y1, w, h);
//       tft.pushColors(..., w * h, true);        // 只推这一块
//   而我们的 uiRender() 是"整屏重绘 + 整帧 240x320 全推"(153600 字节, 40MHz 下 30.7ms)。
//   播放界面每秒都要刷新进度/时间, 于是每秒卡 30ms —— 这就是"播放时界面卡"的根因。
//   把这个区域单独刷, 只推约 240x60, 耗时降到几毫秒。
static void drawPlayerDynamic(Canvas16 *c) {
  const AudioStatus as = audioGetStatus();
  const int16_t barW = TFT_W - 2 * TEXT_MARGIN_X;
  int16_t y = 8 + FONT_H + 22 + FONT_H + 18;

  // 进度条(先清底, 否则残留旧进度)
  c->fillRect(TEXT_MARGIN_X, y, barW, 12, COL_BG);
  uint8_t pct = 0;
  if (as.totalSec > 0) {
    const uint32_t el = as.elapsedSec > as.totalSec ? as.totalSec : as.elapsedSec;
    pct = (uint8_t)((uint64_t)el * 100 / as.totalSec);
  }
  drawBar(c, TEXT_MARGIN_X, y, barW, 12, pct, COL_ACCENT);

  // 时间
  char t1[16], t2[16];
  snprintf(t1, sizeof(t1), "%lu:%02lu", (unsigned long)(as.elapsedSec / 60), (unsigned long)(as.elapsedSec % 60));
  if (as.totalSec > 0) {
    snprintf(t2, sizeof(t2), "%lu:%02lu", (unsigned long)(as.totalSec / 60), (unsigned long)(as.totalSec % 60));
  } else {
    snprintf(t2, sizeof(t2), "--:--");
  }
  y += 15;
  c->fillRect(TEXT_MARGIN_X, y, 72, FONT_H, COL_BG);   // 清掉旧时间
  g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y, t1, COL_FG, COL_BG);
  {
    const int16_t w2 = g_font.measureUtf8(t2) / 2;
    c->fillRect(TFT_W - TEXT_MARGIN_X - 72, y, 72, FONT_H, COL_BG);
    g_font.drawUtf8Small(*c, TFT_W - TEXT_MARGIN_X - w2, y, t2, COL_FG, COL_BG);
  }

  // 音量条
  y += FONT_H + 12;
  drawBar(c, TEXT_MARGIN_X, y, barW, 10, (uint8_t)((uint32_t)as.volume * 100 / AUDIO_VOLUME_MAX), COL_FG);

  drawStatusBar(c);
}

static void drawPlayer(Canvas16 *c) {
  c->fill(COL_BG);

  const AudioStatus as = audioGetStatus();

  // ---- 顶部: 状态 + 曲目序号 ----
  char top[32];
  const char *state = as.paused ? "暂停" : (as.playing ? "播放中" : "停止");
  snprintf(top, sizeof(top), "%s  %d/%d", state, as.trackIndex + 1, as.trackCount);
  const int16_t tw = g_font.measureUtf8(top);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 8, top, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 8 + FONT_H + 4, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  // ---- 中部: 曲名(大字, 太长就截断) ----
  char title[14];
  strncpy(title, as.title[0] ? as.title : "(无标题)", sizeof(title) - 1);
  title[sizeof(title) - 1] = 0;
  const int16_t nw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - nw) / 2, 8 + FONT_H + 22, title, COL_FG, COL_BG);

  // ---- 变化区域(进度/时间/音量/状态栏) 交给它, 便于只刷这一条 ----
  drawPlayerDynamic(c);

  // ---- 底部: 操作提示 / 上一次选中的曲目为什么没播起来 ----
  //
  // ★ 为什么必须显示失败原因(排查"一进播放界面就没声"时加的)★
  //   按下"选中"时, audioPlayIndex() 会先把上一首 stop 掉(库 connecttoFS 的第一步),
  //   所以如果新选的这首库不支持, 声音是在**按下的那一刻**没的, 不是切界面导致的。
  //   若界面只显示"停止 / 00:00 / --:--", 用户根本没线索, 误判成"进了这个界面就没声"。
  //   audio_player.cpp 的预检会把原因放进 AudioStatus::notice, 这里原样画出来。
  {
    const int16_t hintY = TFT_H - STATUS_BAR_H - FONT_H - 8;
    if (as.notice[0]) {
      g_font.drawUtf8Small(*c, TEXT_MARGIN_X, hintY, as.notice, COL_ACCENT, COL_BG);
    } else {
      g_font.drawUtf8Small(*c, TEXT_MARGIN_X, hintY, "旋转=音量 按下=暂停 长按=选曲", COL_DIM, COL_BG);
    }
  }
}

static void drawMessage(Canvas16 *c) {
  c->fill(COL_BG);

  int16_t y = 90;
  if (s_msgTitle[0]) {
    const int16_t w = g_font.measureUtf8(s_msgTitle);
    g_font.drawUtf8(*c, (TFT_W - w) / 2, y, s_msgTitle, COL_ACCENT, COL_BG);
    y += FONT_H + 10;
  }
  if (s_msgDetail[0]) {
    // 详情按每行 9 个字符手动折行(中文)
    char line[10];
    const char *p = s_msgDetail;
    while (*p) {
      int k = 0;
      while (*p && k < 9) {
        // 按 UTF-8 字符边界切
        const uint8_t b = (uint8_t)*p;
        int len = 1;
        if ((b & 0xE0) == 0xC0) {
          len = 2;
        } else if ((b & 0xF0) == 0xE0) {
          len = 3;
        } else if ((b & 0xF8) == 0xF0) {
          len = 4;
        }
        if (k + len >= (int)sizeof(line)) {
          break;
        }
        for (int i = 0; i < len && *p; i++) {
          line[k++] = *p++;
        }
      }
      line[k] = 0;
      const int16_t w = g_font.measureUtf8(line);
      g_font.drawUtf8(*c, (TFT_W - w) / 2, y, line, COL_FG, COL_BG);
      y += FONT_H + 2;
      if (y > TFT_H - 30) {
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 启动进度
// ---------------------------------------------------------------------------
void uiBootBegin(void) {
  s_bootCount = 0;
  s_bootDetail[0] = 0;
  uiShowMessage();   // 复用提示界面的屏幕枚举
  s_screen = UI_BOOT;
}

void uiBootStep(int index, const char *text, BootState state, const char *detail) {
  if (index < 0 || index >= BOOT_MAX_STEPS) {
    return;
  }
  if (text) {
    snprintf(s_bootText[index], sizeof(s_bootText[index]), "%s", text);
  }
  s_bootState[index] = state;
  if (index + 1 > s_bootCount) {
    s_bootCount = index + 1;
  }
  if (detail) {
    snprintf(s_bootDetail, sizeof(s_bootDetail), "%s", detail);
  } else if (state == BOOT_RUN) {
    // 开始重试时先把上一次的失败原因清掉, 否则会一直挂在屏幕上
    s_bootDetail[0] = 0;
  }
  uiRender();
}

// 单独画启动界面: 每行一个阶段 + 状态符号
static void drawBoot(Canvas16 *c) {
  c->fill(COL_BG);

  // ---- 字库没就绪: 用内置 ASCII 画一份英文清单 ----
  // 这是必须的兜底: 没插 SD 卡时字库读不到, 而启动界面恰恰要报告这件事。
  if (!s_fontReady) {
    builtinDrawText(*c, (TFT_W - builtinTextWidth("BOOTING", 2)) / 2, 6, "BOOTING", COL_ACCENT, COL_BG, 2);
    c->hLine(TEXT_MARGIN_X, 26, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);
    int16_t by = 32;
    for (int i = 0; i < s_bootCount && by < TFT_H - STATUS_BAR_H - 26; i++) {
      const char *mark = "[  ]";
      uint16_t mcol = COL_DIM;
      if (s_bootState[i] == BOOT_OK) {
        mark = "[OK]";
        mcol = COL_ACCENT;
      } else if (s_bootState[i] == BOOT_RUN) {
        mark = "[..]";
        mcol = COL_FG;
      } else if (s_bootState[i] == BOOT_FAIL) {
        mark = "[XX]";
        mcol = COL_ACCENT;
      }
      builtinDrawText(*c, 2, by, mark, mcol, COL_BG, 2);
      // 阶段名是中文, 用内置字体画不了 -> 画对应数量的方块占位, 同时把序号标上
      char idx[8];
      snprintf(idx, sizeof(idx), "STEP %d", i + 1);
      builtinDrawText(*c, 46, by, idx, (s_bootState[i] == BOOT_PEND) ? COL_DIM : COL_FG, COL_BG, 2);
      by += 18;
    }
    if (s_bootDetail[0]) {
      c->hLine(TEXT_MARGIN_X, by, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);
      builtinDrawText(*c, 2, by + 4, "FAILED - see serial", COL_ACCENT, COL_BG, 2);
    }
    drawStatusBar(c);
    return;
  }

  const char *title = "启动中";
  const int16_t tw = g_font.measureUtf8(title);
  g_font.drawUtf8(*c, (TFT_W - tw) / 2, 6, title, COL_ACCENT, COL_BG);
  c->hLine(TEXT_MARGIN_X, 6 + FONT_H + 2, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);

  int16_t y = 6 + FONT_H + 8;
  for (int i = 0; i < s_bootCount; i++) {
    // 状态符号(用 ASCII, 不依赖字库里的特殊符号)
    const char *mark;
    uint16_t mcol;
    switch (s_bootState[i]) {
      case BOOT_OK:
        mark = "[OK]";
        mcol = COL_ACCENT;
        break;
      case BOOT_RUN:
        mark = "[..]";
        mcol = COL_FG;
        break;
      case BOOT_FAIL:
        mark = "[XX]";
        mcol = COL_ACCENT;
        break;
      default:
        mark = "[  ]";
        mcol = COL_DIM;
        break;
    }
    g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y + 4, mark, mcol, COL_BG);
    const uint16_t tcol = (s_bootState[i] == BOOT_PEND) ? COL_DIM : COL_FG;
    g_font.drawUtf8Small(*c, TEXT_MARGIN_X + 34, y + 4, s_bootText[i], tcol, COL_BG);
    y += FONT_H + 2;
    if (y > TFT_H - STATUS_BAR_H - 40) {
      break;
    }
  }

  // 失败原因(自动折行)
  if (s_bootDetail[0]) {
    int16_t dy = y + 6;
    c->hLine(TEXT_MARGIN_X, dy - 4, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);
    const char *p = s_bootDetail;
    while (*p && dy < TFT_H - STATUS_BAR_H - 14) {
      char line[14];
      int k = 0;
      while (*p && k < 11) {
        const uint8_t b = (uint8_t)*p;
        int len = 1;
        if ((b & 0xE0) == 0xC0) {
          len = 2;
        } else if ((b & 0xF0) == 0xE0) {
          len = 3;
        } else if ((b & 0xF8) == 0xF0) {
          len = 4;
        }
        if (k + len >= (int)sizeof(line)) {
          break;
        }
        for (int q = 0; q < len && *p; q++) {
          line[k++] = *p++;
        }
      }
      line[k] = 0;
      g_font.drawUtf8Small(*c, TEXT_MARGIN_X, dy, line, COL_FG, COL_BG);
      dy += FONT_H ;
    }
  }

  drawStatusBar(c);
}

// ---------------------------------------------------------------------------
// 字库自检页: 把关键信息直接画在屏幕上, 不依赖串口
// 字库完全没加载时由 drawBoot 的内置 ASCII 兜底; 这里负责"加载了但有问题"的情况。
// ---------------------------------------------------------------------------
void uiShowSelfTest(const char *title, const char *const *lines, int lineCount, const char *sample) {
  Canvas16 *c = displayCanvas();
  if (!c) {
    return;
  }

  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      return;
    }
    c->fill(COL_BG);

    int16_t y = 6;
    if (s_fontReady) {
      const int16_t tw = g_font.measureUtf8(title);
      g_font.drawUtf8(*c, (TFT_W - tw) / 2, y, title, COL_ACCENT, COL_BG);
      y += FONT_H + 4;
    } else {
      builtinDrawText(*c, 2, y + 2, "SELF TEST", COL_ACCENT, COL_BG, 2);
      y += 22;
    }
    c->hLine(TEXT_MARGIN_X, y, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);
    y += 4;

    for (int i = 0; i < lineCount && i < 6; i++) {
      const char *p = lines[i];
      while (*p && y < TFT_H - STATUS_BAR_H - 28) {
        char buf[14];
        int k = 0;
        while (*p && k < 11) {
          const uint8_t b = (uint8_t)*p;
          int len = 1;
          if ((b & 0xE0) == 0xC0) {
            len = 2;
          } else if ((b & 0xF0) == 0xE0) {
            len = 3;
          } else if ((b & 0xF8) == 0xF0) {
            len = 4;
          }
          if (k + len >= (int)sizeof(buf)) {
            break;
          }
          for (int q = 0; q < len && *p; q++) {
            buf[k++] = *p++;
          }
        }
        buf[k] = 0;
        if (s_fontReady) {
          g_font.drawUtf8Small(*c, TEXT_MARGIN_X, y, buf, COL_FG, COL_BG);
        } else {
          builtinDrawText(*c, 2, y + 2, buf, COL_FG, COL_BG, 2);
        }
        y += FONT_H;
      }
      y += 2;
    }

    if (sample && sample[0]) {
      const int16_t sy = TFT_H - STATUS_BAR_H - FONT_H - 6;
      c->hLine(TEXT_MARGIN_X, sy - 4, TFT_W - 2 * TEXT_MARGIN_X, COL_DIM);
      if (s_fontReady) {
        g_font.drawUtf8(*c, TEXT_MARGIN_X, sy, sample, COL_ACCENT, COL_BG);
        g_font.drawUtf8Small(*c, TEXT_MARGIN_X + FONT_W + 8, sy + 6, sample, COL_FG, COL_BG);
      } else {
        builtinDrawText(*c, 2, sy + 2, "FONT NOT LOADED", COL_ACCENT, COL_BG, 2);
      }
    }

    drawStatusBar(c);
  }

  displayFlush();
}

// ---------------------------------------------------------------------------
// 上一帧画的是哪个界面/哪一页 —— 用来判断"这一帧是不是必然整屏全变"
// ---------------------------------------------------------------------------
static int s_lastDrawnScreen = -1;
static uint32_t s_lastDrawnPage = 0xFFFFFFFFu;
static int s_lastDrawnChapter = -999;

void uiRender() {
  Canvas16 *c = displayCanvas();
  if (!c) {
    return;
  }

  const uint32_t t0 = micros();

  // 翻页 / 切界面时整屏内容必然全变: 直接让刷屏层整帧推送一次,
  // 省掉一遍 153600 字节的逐行比对(比对本身要读两遍 PSRAM, 也是钱)。
  {
    const bool reading = (s_screen == UI_READING) && s_book && s_book->isOpen();
    const uint32_t page = reading ? s_book->page() : 0xFFFFFFFFu;
    const int ch = reading ? s_book->currentChapter() : -999;
    if ((int)s_screen != s_lastDrawnScreen || page != s_lastDrawnPage || ch != s_lastDrawnChapter) {
      displayDirtyInvalidate();
      s_lastDrawnScreen = (int)s_screen;
      s_lastDrawnPage = page;
      s_lastDrawnChapter = ch;
    }
  }

  uint32_t drawUs = 0, lockWaitUs = 0;
  uint32_t glyphHits = 0, glyphMisses = 0;
  // ---------------------------------------------------------------------------
  // ★ 整页字形预取 (必须在进 sd_mutex 之前做) ★
  //
  // 实测: 一次字形未命中 = SD seek + read(288B) ≈ **286~320 毫秒**。
  //   贵不在 288 字节, 而在 seek —— 字库 6.8MB、字形在文件里随机分布,
  //   f.seek() 要沿 FAT 链一簇簇跳过去。一页正文常有 40~88 个不同汉字,
  //   逐个按需读就是十几秒, 这就是"翻页卡死"的真正原因。
  //
  // 做法: 把整页字符收集起来, 让 DotFont 按"字库内位置升序"一趟顺序读,
  //   40~88 次随机 seek 变成 1~几次顺序读。之后绘制阶段全部命中缓存、不碰 SD,
  //   顺带也让音频任务不必再等这把 sd_mutex。
  //
  // 为什么放在锁外面: prefetchUtf8() 内部自己会拿 sd_mutex。
  //   若在这里持着外层锁去调, 就会嵌套持锁, 让音频白等。
  // ---------------------------------------------------------------------------
  if (s_needPrefetch) {
    s_needPrefetch = false;
    if (s_screen == UI_READING && s_book && s_book->isOpen()) {
      const int pn = s_book->lineCount();
      const int prows = (pn < ROWS) ? pn : ROWS;
      const char *plines[ROWS];
      int plc = 0;
      for (int pi = 0; pi < prows; pi++) {
        const char *pln = s_book->line(pi);
        if (pln) {
          plines[plc++] = pln;
        }
      }
      if (plc > 0) {
        g_font.prefetchUtf8(plines, plc);   // 内部持锁, 一趟顺序读
      }
    }
  }

  bool lockOk = true;

  // 画字需要读 SD 字形, 这一段持锁
  {
    const uint32_t tw = micros();
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    const uint32_t tl = micros();
    lockWaitUs = tl - tw;
    lockOk = g.ok();
    if (!lockOk) {
      log_w("[ui] 绘制等 SD 锁超时, 跳过本帧");
    } else {
      // 字形缓存命中/未命中: 未命中 = 一次 SD 随机读(6.8MB 字库里的 288 字节)
      const uint32_t h0 = g_font.statHit();
      const uint32_t m0 = g_font.statMiss();

      switch (s_screen) {
        case UI_READING:
          drawReading(c);
          break;
        case UI_TOC:
          drawToc(c);
          break;
        case UI_BOOT:
          drawBoot(c);
          break;
        case UI_MUSIC:
          drawMusic(c);
          break;
        case UI_MENU:
          drawMenu(c);
          break;
        case UI_BOOKS:
          drawBooks(c);
          break;
        case UI_PLAYER:
          drawPlayer(c);
          break;
        case UI_MESSAGE:
        default:
          drawMessage(c);
          break;
      }

      glyphHits = g_font.statHit() - h0;
      glyphMisses = g_font.statMiss() - m0;
      drawUs = micros() - tl;
    }
  }

  // 刷屏: 走脏区域差分 —— 只把"和上一次已经推上去的像素不同"的行段发出去。
  // (displayFlush 是"无条件整帧 153600 字节", 这里换掉了)
  if (lockOk) {
    displayFlushDirty();
  }

  const uint32_t totalUs = micros() - t0;

  // 这一帧特别慢就单独打一行明细(不参与每秒平均), 这样"翻一页到底花在哪"一目了然:
  //   draw = 绘制(含从 SD 读字形)  miss = 这一帧发生了几次字形 SD 随机读
  //   flush = 推送(整帧 or 只推脏行段) + 实际发出多少字节
  // 限频 6 行/秒, 避免刷屏时串口本身把界面拖慢。
  {
    static uint32_t slowWindowMs = 0;
    static uint32_t slowInWindow = 0;
    const uint32_t nowMs = millis();
    if (nowMs - slowWindowMs >= 1000) {
      slowWindowMs = nowMs;
      slowInWindow = 0;
    }
    if (lockOk && totalUs > 40000 && slowInWindow < 6) {
      slowInWindow++;
      uint32_t flushUs = 0, flushBytes = 0, bands = 0;
      bool flushFull = false;
      displayLastFlushInfo(&flushUs, &flushBytes, &flushFull, &bands);
      Serial.printf("[perf-slow] total=%lu.%lums draw=%lu.%lums (miss=%lu hit=%lu) flush=%lu.%lums %s sent=%luKB bands=%lu lockWait=%lu.%lums\n",
                    (unsigned long)(totalUs / 1000), (unsigned long)((totalUs % 1000) / 100),
                    (unsigned long)(drawUs / 1000), (unsigned long)((drawUs % 1000) / 100), (unsigned long)glyphMisses,
                    (unsigned long)glyphHits, (unsigned long)(flushUs / 1000), (unsigned long)((flushUs % 1000) / 100),
                    flushFull ? "full" : "part", (unsigned long)(flushBytes / 1024), (unsigned long)bands,
                    (unsigned long)(lockWaitUs / 1000), (unsigned long)((lockWaitUs % 1000) / 100));
    }
  }

  displayPerfNoteUi(totalUs, drawUs, lockWaitUs, true, glyphHits, glyphMisses, lockOk);
  displayPerfTick();
}

// ---------------------------------------------------------------------------
// 只重画并只刷新底部状态栏(见 ui.h 的说明)
// ---------------------------------------------------------------------------
void uiRenderStatusBarOnly(void) {
  Canvas16 *c = displayCanvas();
  if (!c) {
    return;
  }

  const uint32_t t0 = micros();

  // 播放界面比较特殊: 每秒变的是"进度条+时间+音量", 不只是状态栏。
  // 把这块和状态栏一起重画, 然后只推这一整条(约 240x62), 而不是整帧。
  const int16_t dynTop = 8 + FONT_H + 22 + FONT_H + 18 - 4;

  uint32_t drawUs = 0, lockWaitUs = 0;
  uint32_t glyphHits = 0, glyphMisses = 0;
  bool lockOk = true;
  {
    const uint32_t tw = micros();
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    const uint32_t tl = micros();
    lockWaitUs = tl - tw;
    lockOk = g.ok();
    if (lockOk) {
      const uint32_t h0 = g_font.statHit();
      const uint32_t m0 = g_font.statMiss();
      if (s_screen == UI_PLAYER) {
        drawPlayerDynamic(c);
      } else {
        drawStatusBar(c);
      }
      glyphHits = g_font.statHit() - h0;
      glyphMisses = g_font.statMiss() - m0;
      drawUs = micros() - tl;
    }
  }

  if (lockOk) {
    // 只在这条带里做差分: 上面的内容这一步根本没重画, 不可能变
    if (s_screen == UI_PLAYER) {
      displayFlushDirtyRange(dynTop, (int16_t)(TFT_H - 1));
    } else {
      // 其余界面只有状态栏在变: 240x20 = 9600 字节, 约 2ms(整帧是 30.7ms);
      // 而且这一秒如果时间/电量没变, 差分结果是 0 字节, 一个字节都不发。
      displayFlushDirtyRange((int16_t)(TFT_H - STATUS_BAR_H), (int16_t)(TFT_H - 1));
    }
  }

  displayPerfNoteUi(micros() - t0, drawUs, lockWaitUs, false, glyphHits, glyphMisses, lockOk);
  displayPerfTick();
}

void uiShowProgress(const char *what, uint8_t percent) {
  Canvas16 *c = displayCanvas();
  if (!c) {
    return;
  }
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      return;
    }
    c->fill(COL_BG);

    const int16_t tw = g_font.measureUtf8(what);
    g_font.drawUtf8(*c, (TFT_W - tw) / 2, 130, what, COL_FG, COL_BG);

    // 进度条
    const int16_t bx = 30, bw = TFT_W - 60, by = 170, bh = 10;
    c->rect(bx, by, bw, bh, COL_DIM);
    const int16_t fill = (int16_t)((int32_t)(bw - 4) * percent / 100);
    if (fill > 0) {
      c->fillRect(bx + 2, by + 2, fill, bh - 4, COL_ACCENT);
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)percent);
    const int16_t pw = g_font.measureUtf8(buf) / 2;
    g_font.drawUtf8Small(*c, (TFT_W - pw) / 2, by + bh + 8, buf, COL_DIM, COL_BG);
  }
  displayFlush();
}
