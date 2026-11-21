/*
 * state_store.h — "记住上次看的书和页码"(存档在 Flash/NVS, 不碰 SD, 不占 sd_mutex)
 *
 * ★ 为什么需要 ★
 *   现在重启板子就回到主菜单: 书没了、页码也没了, 每次都要重新找书、重新翻页。
 *   把"上次打开的书 + 页码 + 上次界面"存进 NVS, 下次开机主菜单里就能一键「继续阅读」。
 *
 * ★ 设计要点 ★
 *   1. **只写 Flash(NVS), 完全不碰 SD/SPI**: 所以不需要 sd_mutex, 也不会和 SD 抢总线。
 *      (SD 可能没插, 而 NVS 分区一定在。)
 *   2. **防抖**: `stateStoreNoteBook()` 只标脏, 真正落盘由 `stateStoreTick()` 在
 *      "最后一次变更后 STATE_STORE_DEBOUNCE_MS"才做 —— 翻页时不会每页写一次 Flash。
 *      另外还有"两次写入至少间隔 STATE_STORE_MIN_WRITE_GAP_MS"与"内容没变就不写"两道闸,
 *      把 Flash 磨损降到最低。
 *   3. **版本号**: 存档带 magic + version; 版本不符或数据损坏 -> 视为"没有存档"
 *      (调用方走主菜单正常流程), 绝不因为存档问题卡住/崩溃。
 *   4. 存档内容 = 书路径(SD 上的 .txt/.epub **原始路径**) + 页码 + epub 标志 + 上次界面。
 *      ★ EPUB 的页码是**缓存文本**的页码, 而缓存路径是 .epub 路径的纯函数
 *        (`epubCachePath()`: /cache/<书名>.txt), 缓存一旦生成就会被复用 ——
 *        所以"存 .epub 原始路径 + 页码"在重新打开时能对上。
 *        **绝不能存 /cache/xxx.txt**: 那个文件不在书列表里, 用户删/换卡后就对不上了。
 *
 * 用法(接线清单见交付报告):
 *   启动:   stateStoreBegin();
 *   变更:   stateStoreNoteBook(path, isEpub, page);   // 翻页/开书/跳章后
 *   主循环: stateStoreTick();                          // 每秒都会被调到, 到点才写
 *   回菜单: stateStoreFlush();                         // 可选, 立刻落盘
 */
#pragma once

#include <Arduino.h>

#define STATE_STORE_VERSION 1

// 书路径上限(与 bookreader.ino 里的路径缓冲一致; 超过会被截断并拒绝保存)
#define STATE_STORE_PATH_MAX 120

// 防抖时间: 最后一次变更后多久才真正写 Flash
#ifndef STATE_STORE_DEBOUNCE_MS
#define STATE_STORE_DEBOUNCE_MS 2500
#endif
// 两次写入之间的最小间隔(硬下限, 保护 Flash)
#ifndef STATE_STORE_MIN_WRITE_GAP_MS
#define STATE_STORE_MIN_WRITE_GAP_MS 1500
#endif

// 读出来的存档内容
struct BookState {
  bool valid = false;                        // 有可用存档
  uint8_t version = 0;                       // 存档格式版本
  char bookPath[STATE_STORE_PATH_MAX] = {0}; // 书在 SD 上的原始路径(.txt/.epub)
  uint32_t page = 0;                         // 页码(0 起)
  uint32_t pageOffset = 0;                   // 该页在正文里的起始字节偏移(校验用, 可为 0)
  bool isEpub = false;
  uint8_t screen = 0;                        // 上次所在界面(UiScreen 的数值, 仅供参考)
};

// 初始化 NVS。失败返回 false: 存档功能整体降级为"不可用", 其余功能不受影响。
bool stateStoreBegin();

// 存档功能是否可用(不插 SD 也能用, 因为它只碰 NVS)
bool stateStoreAvailable();

// 读取存档。返回 false = 没有存档 / 版本不符 / 数据损坏(都会被当作"没有"处理)。
bool stateStoreLoad(BookState *out);

// 记录"当前正在看哪本书的第几页"(只标脏, 不立刻写 Flash)
// pageOffset = g_book.pageStartOffset(); 主程序在恢复时拿它校验"版面有没有变"
// (EPUB 缓存被重建、或两本同名书共用一个缓存时, 页码可能对不上 —— 校验不过就回第 1 页)
void stateStoreNoteBook(const char *path, bool isEpub, uint32_t page, uint32_t pageOffset = 0);

// 记录"当前界面"(只标脏; 仅作信息, 不用于开机自动跳转)
void stateStoreNoteScreen(uint8_t screen);

// 主循环里周期性调用: 到点(防抖到期 + 已变更)才真正写一次 Flash
void stateStoreTick();

// 立刻落盘(仍受"最小写入间隔"和"内容没变不写"两道闸约束)。
// 用在"回到主菜单""打开书之前"这种明确的存档点。
void stateStoreFlush();

// 清除存档(例如用户删掉了那本书)
void stateStoreClear();

// 存过东西没有(不需要读 NVS, 用的是内存里的镜像)
bool stateStoreHasSaved();
