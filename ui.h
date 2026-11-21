/*
 * ui.h — 界面状态与渲染
 *
 * 界面状态机(第一阶段只有阅读界面, 菜单/播放界面留给第二阶段):
 *   UI_READING : 正文 + 底部状态栏
 *   UI_TOC     : 章节目录
 *   UI_MESSAGE : 居中提示(开书失败、字库缺失等)
 */
#pragma once

#include <Arduino.h>

#include "display.h"
#include "font.h"
#include "input.h"  // BatteryInfo
#include "txt_reader.h"

enum UiScreen : uint8_t {
  UI_BOOT = 0,
  UI_READING,
  UI_TOC,
  UI_MESSAGE,
  UI_MUSIC,      // 选曲列表
  UI_MENU,       // 主菜单: 看小说 / 听音乐
  UI_BOOKS,      // 选书列表
  UI_PLAYER,     // 播放界面: 曲名 + 进度 + 音量
};

void uiInit();

// 设置提示信息(UI_MESSAGE 界面显示)
void uiSetMessage(const char *title, const char *detail);
void uiShowMessage();

// 界面切换
void uiShowReading();
void uiShowToc();

// ---------------------------------------------------------------------------
// 选曲列表(UI_MUSIC)
//
// 原来只能听"开机自动播放的第一首", 没法自己挑曲子。这里做一个列表:
// 旋钮滚动、按键选定。选中项高亮, 正在播放的那首前面标一个符号。
// ---------------------------------------------------------------------------
void uiShowMusic(void);
void uiMusicMove(int delta);        // 上/下移动选择(会做边界钳制)
int uiMusicSelected(void);          // 当前选中的曲目序号
void uiMusicSetCurrent(int index);  // 告诉界面"正在播放的是第几首"(用于标记)
// 由主程序注册: 返回曲目总数 / 第 i 首的名字。这样 ui 不用直接依赖 audio_player。
void uiSetTrackProvider(int (*countFn)(void), const char *(*titleFn)(int));

// ---------------------------------------------------------------------------
// 主菜单(UI_MENU) + 选书列表(UI_BOOKS)
//
// 为什么需要: 原来启动成功就直接进正文, 启动失败就卡在启动界面 —— 而音乐面板
// 的入口只在"阅读界面"分支里处理。结果是**启动没成功时, 音乐 UI 根本触发不了**。
// 现在改成: 启动完成后先进主菜单, 由用户选"看小说"还是"听音乐"。
// 启动失败时也能进主菜单, 所以音乐始终可用。
// ---------------------------------------------------------------------------
void uiShowMenu(void);
void uiMenuMove(int delta);
int uiMenuSelected(void);

// ---------------------------------------------------------------------------
// 主菜单第 3 项: "继续阅读"(上次看的书 + 页码)
//
// 为什么要有: 用户重启板子后书和页码都没了, 每次要重新找书、重新翻页。
// 存档由 state_store.*(NVS, 不碰 SD)负责, 这里只负责"显示 + 让用户选到它"。
//
//   菜单项顺序固定:  0=看小说  1=听音乐  2=继续阅读(UI_MENU_ITEM_CONTINUE)
//   没有存档时该项画成灰色, uiMenuContinueAvailable() 返回 false,
//   主程序据此决定"按下不响应"或提示"还没有阅读记录" —— 界面状态不会错位。
// ---------------------------------------------------------------------------
#define UI_MENU_ITEM_COUNT 3
#define UI_MENU_ITEM_CONTINUE 2

// 告诉界面"上次看到哪本书的哪一页"(bookPath = SD 上的 /xxx.txt 或 /xxx.epub;
// 传 nullptr/"" 表示没有存档)。页码是 0 起的内部页号, 显示时会 +1。
void uiSetContinueBook(const char *bookPath, uint32_t page);
bool uiMenuContinueAvailable(void);

void uiShowBooks(void);
void uiBooksMove(int delta);
int uiBooksSelected(void);
// 主程序注册: 返回书籍总数 / 第 i 本的书名。标题为 nullptr 时显示占位。
void uiSetBookProvider(int (*countFn)(void), const char *(*titleFn)(int));

// ---------------------------------------------------------------------------
// 播放界面(UI_PLAYER): 曲名 + 播放进度条 + 音量条
//
// 你说需要"能调音量、能看到在放什么、还要有进度" —— 这个界面就是干这个的。
// 数据来自 audio_player 的 AudioStatus(progress/elapsed/total/volume)。
// 界面本身不持有音频状态, 每次绘制时现取, 所以进度会跟着刷新。
// ---------------------------------------------------------------------------
void uiShowPlayer(void);

// 主程序注册"取当前页插图路径"的回调(返回 true 表示当前页是插图, 一页一张)
void uiSetPageImageProvider(bool (*fn)(char *, size_t));

// ---------------------------------------------------------------------------
// 启动进度界面
//
// 为什么要单独做: 原来启动过程只在**失败时**才在屏幕上显示信息, 成功了反而看不到
// 走到了哪一步 —— 没有串口时就完全抓瞎。这里改成每个阶段实时上图。
//
// 用法:
//   uiBootBegin();                        // 开始
//   uiBootStep(i, "挂载 SD 卡", BOOT_RUN); // 把第 i 步标成"进行中"并刷屏
//   uiBootStep(i, nullptr, BOOT_OK);       // 标成成功
//   uiBootStep(i, "原因...", BOOT_FAIL);   // 标成失败, 原因会显示在下面
// ---------------------------------------------------------------------------
enum BootState : uint8_t {
  BOOT_PEND = 0,  // 还没做
  BOOT_RUN,       // 进行中
  BOOT_OK,        // 成功
  BOOT_FAIL,      // 失败
};

#define BOOT_MAX_STEPS 8
void uiBootBegin(void);
// text 为 nullptr 表示沿用上一步设置的文字; detail 只在 FAIL 时显示
void uiBootStep(int index, const char *text, BootState state, const char *detail = nullptr);

// 把当前状态画进画布 + 刷屏。这是唯一会刷屏的地方。
void uiRender();

// 通知"这一页的字形需要预取"(翻页 / 打开书 / 跳章节后调用)。
// 下一次 uiRender() 会在进 sd_mutex 之前把整页字形一趟顺序读进来。
// 单次字形随机读实测 286~320ms, 预取后绘制阶段不再碰 SD。
void uiRequestGlyphPrefetch(void);

// ---------------------------------------------------------------------------
// 只重画并只刷新底部状态栏那一条(240x20)
//
// ★ 为什么要有这个 ★
//   uiRender() 是"整屏重绘 + 整帧 153600 字节全传", 40MHz 下约 30.7ms,
//   这段时间主循环完全卡住。而"音乐进度每秒跳一下""电量每 5 秒变一次"
//   只改动了状态栏 —— 用整帧去刷它是极大的浪费。
//   这个函数只刷 240x20 = 9600 字节(约 1.9ms), 快 16 倍, 手感立刻回来。
//
// 只在"界面上除了状态栏都没变"的场合使用(例如播放中的秒跳、电量更新)。
// 界面内容变化仍然用 uiRender()。
void uiRenderStatusBarOnly(void);

UiScreen uiScreen();

// ---- 阅读相关 ----
void uiAttachBook(TxtReader *book);
void uiSetFontReady(bool ready);
void uiSetBattery(const BatteryInfo &b);

// 章节目录里的选中项
void uiTocMove(int delta);
int uiTocSelected();
void uiTocReset();

// 字库自检页: 标题 + 若干诊断行 + 一行"测试汉字"。
// 字库有问题时用它代替一行文字提示 —— 能一眼区分"字库没加载"和"加载了但渲染坏"。
// lines 是若干 UTF-8 诊断行(最多 6 行); sample 是测试汉字(可为 nullptr)
void uiShowSelfTest(const char *title, const char *const *lines, int lineCount, const char *sample);

// 画一个居中的进度提示(扫描大文件时用), 立即刷屏
void uiShowProgress(const char *what, uint8_t percent);

// 状态栏右侧的小字提示(例如"扫描中…")
void uiSetStatusHint(const char *hint);
