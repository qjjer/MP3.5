/*
 * image_view.h - 插图页渲染（JPEG 用 TJpg_Decoder，PNG 用 PNGdec）
 *
 * 屏幕是 240x320，而 EPUB 里的插画是 1442x2048 / 2048x1453 这种大图。
 * 全尺寸解码成 RGB565 要 8MB 内存 —— 所以**必须降采样**：
 *   TJpg_Decoder 的 setJpgScale(1/2/4/8) 能在解码阶段就缩小，1/8 时 2048 -> 256 像素，
 *   正好接近屏幕宽度，内存和速度都可接受。
 *
 * 解码器通过回调按块吐出 RGB565 像素，我们直接写进整屏帧缓冲(Canvas16)，
 * 然后由 displayFlush() 统一刷上去 —— 和其它所有绘制走同一条路。
 *
 * ★ 重要(2026-10 修复) ★
 *   解码**不走库的按文件名 API**（drawJpg(path)/getJpgSize(path)）：
 *   TJpg_Decoder 在 ESP32 上会把 '/' 开头的路径当成 SPIFFS/LittleFS 的文件
 *   （TJpg_Decoder.cpp:170-172 -> drawFsJpg()，默认 fs = SPIFFS），而本工程从没
 *   挂载过 SPIFFS，于是必然返回 JDR_INP —— 这就是"插图解码失败"的根因。
 *   现在的做法是：先用工程自己挂载的 SD 对象把图片读进 PSRAM，再调用库的
 *   **内存版** drawJpg(x, y, array, size) / getJpgSize(w, h, array, size)。
 */
#pragma once

#include <Arduino.h>

#include "display.h"

// 把一张图片缩放居中画到画布上（letterbox，保持宽高比）。
// path 是 SD 卡上的完整路径。返回 false 表示读文件或解码失败。
// 内部只在"读文件"那一步持 sd_mutex；解码在内存里做，不占 SPI 总线。
bool imageDrawToCanvas(Canvas16 *canvas, const char *path);

// 只读取图片尺寸，不解码像素（用于排版前估算，可选）
bool imageGetSize(const char *path, uint16_t *w, uint16_t *h);

// 根据后缀判断是不是我们支持的图片
bool imageIsSupported(const char *path);
