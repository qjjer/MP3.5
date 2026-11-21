/*
 * ui_lvgl.cpp — LVGL 版界面 + 中文字库桥接(第 1 阶段)
 *
 * ===========================================================================
 * ★ 一、中文字库是怎么接进 LVGL 的(这是本文件最核心的部分) ★
 * ===========================================================================
 * LVGL 要的字体是一个 lv_font_t, 里面两个回调:
 *     get_glyph_dsc()    -> 这个字的 度量(adv_w / box_w / box_h / ofs_x / ofs_y / bpp)
 *     get_glyph_bitmap() -> 这个字的 **原始位图指针**
 *
 * 我们**不生成**任何字库数组(lv_font_conv 生成 23459 个汉字要几十 MB, 应用分区只有
 * 1.37MB), 而是桥接到 SD 上的 /fonts/sourcehan24_aa.f24:
 *     DotFont::lookup(cp, &meta)        -> 码点是否存在
 *     DotFont::glyphBitmap(cp, &bpp…)   -> 位图(只读出口, 见 font.h)
 *
 * ★ 位图布局为什么可以零拷贝直接给 LVGL(实测核对过 lvgl 源码) ★
 *   LVGL(lv_draw_sw_letter.c: draw_letter_normal)是按
 *       width_bit = box_w * bpp          行与行之间**不补齐**
 *       letter_px = (*map_p & bitmask) >> (col_bit_max - col_bit)
 *   来读的, 也就是"行优先、每行 box_w*bpp 位、左侧像素在字节高位"。
 *   而 .f24 字库正是这个布局(高 4 位 = 左边像素), 并且 box_w=24 时
 *       1bpp -> 24 位 = 3 字节/行
 *       2bpp -> 48 位 = 6 字节/行   (PSRAM 压缩档)
 *       4bpp -> 96 位 = 12 字节/行
 *   三种都刚好整字节对齐, 所以**原样返回指针就行, 一个字节都不用转换**。
 *
 *   唯一需要转换的情况: 文件是 4bpp 但 PSRAM 压缩档是 2bpp —— 那也是布局一致、
 *   只是 bpp 不同。为了不给 LVGL 出难题(dsc 回调先于 bitmap 回调, bpp 必须提前知道),
 *   这里统一按 **4bpp** 上报, 若拿到的是 1/2bpp 就在本文件里展开成 4bpp(288 字节,
 *   每字一次, 可以忽略不计)。
 *
 * ★ 缺字怎么办 ★
 *   get_glyph_dsc 返回 false -> LVGL 不画(missing glyph), 不会崩。
 *   LV_USE_FONT_PLACEHOLDER 已关(省 Flash)。
 *
 * ===========================================================================
 * ★ 二、性能保障: 绘制时不要碰 SD ★
 * ===========================================================================
 * 这块板子的 SD 读慢得离谱(实测每次调用约 105ms, 与字节数几乎无关), 所以
 * "按需从 SD 读字形"在 LVGL 下同样致命。两条保障:
 *   1) 生产用法: 启动时调 DotFont::loadBitmapsToPsram() 把整份位图(1bpp/2bpp 压缩)
 *      搬进 PSRAM —— 之后 glyphBitmap() 是**纯内存**访问, 一次 SD 都不读。
 *   2) 没搬进 PSRAM 时: glyphBitmap() 仍走 DotFont 自己的 256 槽缓存 + sd_mutex,
 *      功能正确但慢(每个新字约 105ms)。所以串口那行 [lvgl] 字体会把
 *      "PSRAM 档 / 按需读卡" 状态打出来, 一眼能看出是不是走了慢路径。
 *
 * ===========================================================================
 * ★ 三、输入: 为什么用 LV_INDEV_TYPE_ENCODER ★
 * ===========================================================================
 *   EC11 就是标准的 encoder 语义, 配 lv_group 之后"旋转移动焦点、按下确认"
 *   是所有控件通用的, 不需要给每个控件写事件处理。参考项目那套是 XPT2046
 *   触摸屏(POINTER + 坐标 map), 我们没有触摸屏, 搬过来用不上。
 */
#include "config.h"

#if UI_BACKEND_LVGL

#include "ui_lvgl.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font.h"
#include "font_builtin.h"   // FONT5X7: ASCII 用内置点阵, 不走 SD 字库
#include "lvgl_port.h"

extern DotFont g_font;

// ===========================================================================
// 1) 中文字体桥接
// ===========================================================================

// 24x24 4bpp 一个字形 288 字节; 只在"拿到 1/2bpp 需要展开"时用到
static uint8_t s_glyph4[24 * 12];
// ASCII: 5x7 点阵放大 2 倍 = 10x14, bpp=8(一个像素一个字节的 alpha)
static uint8_t s_ascii8[10 * 14];

static uint8_t s_cjkW = 24;
static uint8_t s_cjkH = 24;

#define ASCII_BOX_W 10
#define ASCII_BOX_H 14
#define ASCII_ADV   192   /* 12px * 16 (LVGL 的 adv_w 是 1/16 像素) */

// 行高/基线: 与 config.h 的排版参数一致(LINE_H = 24+2 = 26)
static lv_coord_t s_lineHeight = FONT_H + LINE_GAP;   /* 26 */
static lv_coord_t s_baseLine = 2;                     /* 基线在行顶下方 24px */

// 把 1bpp / 2bpp 的字形展开成 4bpp(布局不变, 只是位深)
static void expandTo4bpp(const uint8_t *src, uint8_t srcBpp) {
  const uint16_t gw = s_cjkW;
  const uint16_t gh = s_cjkH;
  const uint16_t srcStride = (uint16_t)((gw * srcBpp + 7) / 8);
  const uint16_t dstStride = (uint16_t)((gw + 1) / 2);   /* 24 -> 12 */

  if (srcBpp == 1) {
    for (uint16_t y = 0; y < gh; y++) {
      const uint8_t *srow = src + (size_t)y * srcStride;
      uint8_t *drow = s_glyph4 + (size_t)y * dstStride;
      for (uint16_t x = 0; x < gw; x += 2) {
        const uint8_t a = (srow[x >> 3] & (uint8_t)(0x80 >> (x & 7))) ? 0x0F : 0x00;
        const uint8_t b = (srow[(x + 1) >> 3] & (uint8_t)(0x80 >> ((x + 1) & 7))) ? 0x0F : 0x00;
        drow[x >> 1] = (uint8_t)((a << 4) | b);
      }
    }
    return;
  }

  if (srcBpp == 2) {
    // 每字节 4 个像素 -> 每字节 2 个像素: 一个源字节出两个目标字节
    for (uint16_t y = 0; y < gh; y++) {
      const uint8_t *srow = src + (size_t)y * srcStride;
      uint8_t *drow = s_glyph4 + (size_t)y * dstStride;
      for (uint16_t i = 0; i < srcStride; i++) {
        const uint8_t v = srow[i];
        const uint8_t p0 = (uint8_t)((v >> 6) & 0x03);
        const uint8_t p1 = (uint8_t)((v >> 4) & 0x03);
        const uint8_t p2 = (uint8_t)((v >> 2) & 0x03);
        const uint8_t p3 = (uint8_t)(v & 0x03);
        // 2bit 的 0..3 -> 4bit 的 0..15(乘 5 和 5 是最均匀的四级灰度映射)
        drow[2 * i] = (uint8_t)((p0 * 5) << 4 | (p1 * 5));
        drow[2 * i + 1] = (uint8_t)((p2 * 5) << 4 | (p3 * 5));
      }
    }
    return;
  }

  // 已经是 4bpp: 直接拷(理论上走不到这里, 兜底)
  memcpy(s_glyph4, src, (size_t)dstStride * gh);
}

// 5x7 点阵 -> 10x14 的 8bpp alpha 位图
static void buildAsciiBitmap(uint32_t letter) {
  memset(s_ascii8, 0, sizeof(s_ascii8));
  if (letter < 32 || letter > 126) {
    return;
  }
  const uint8_t *glyph = FONT5X7[letter - 32];
  for (uint8_t col = 0; col < 5; col++) {
    const uint8_t bits = pgm_read_byte(&glyph[col]);
    for (uint8_t row = 0; row < 7; row++) {
      if (!((bits >> row) & 1)) {
        continue;
      }
      for (uint8_t dy = 0; dy < 2; dy++) {
        uint8_t *p = s_ascii8 + (size_t)(row * 2 + dy) * ASCII_BOX_W + col * 2;
        p[0] = 255;
        p[1] = 255;
      }
    }
  }
}

static bool cjkGetGlyphDsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t letter_next) {
  (void)font;
  (void)letter_next;

  dsc->is_placeholder = 0;

  if (letter < 32) {
    return false;   /* 控制字符(含 \n)不画 */
  }

  if (letter <= 126) {
    // ASCII 走内置 5x7 点阵(和旧 UI 完全一致的做法, 不碰 SD)
    dsc->adv_w = ASCII_ADV;
    dsc->box_w = ASCII_BOX_W;
    dsc->box_h = ASCII_BOX_H;
    dsc->ofs_x = 0;
    dsc->ofs_y = 5;   /* 让 14px 高的字落在 26px 行的垂直中部(与旧 UI 的 y+5 一致) */
    dsc->bpp = 8;
    return true;
  }

  if (!g_font.ready()) {
    return false;
  }
  GlyphMeta m;
  if (!g_font.lookup(letter, &m)) {
    return false;   /* 字库里没有这个字 -> LVGL 跳过 */
  }
  dsc->adv_w = (uint16_t)(s_cjkW * 16);
  dsc->box_w = s_cjkW;
  dsc->box_h = s_cjkH;
  dsc->ofs_x = 0;
  dsc->ofs_y = 0;
  dsc->bpp = 4;     /* 统一按 4bpp 上报; 实际位深在 bitmap 回调里对齐 */
  return true;
}

static const uint8_t *cjkGetGlyphBitmap(const lv_font_t *font, uint32_t letter) {
  (void)font;

  if (letter >= 32 && letter <= 126) {
    buildAsciiBitmap(letter);
    return s_ascii8;
  }

  uint8_t bpp = 0, gw = 0, gh = 0;
  const uint8_t *src = g_font.glyphBitmap(letter, &bpp, &gw, &gh);
  if (!src) {
    return nullptr;
  }
  if (bpp == 4 && gw == s_cjkW && gh == s_cjkH) {
    return src;   /* 布局与 LVGL 完全一致 -> 零拷贝 */
  }
  expandTo4bpp(src, bpp);
  return s_glyph4;
}

static lv_font_t s_fontCjk;

static void cjkFontInit(void) {
  s_cjkW = g_font.glyphW() ? g_font.glyphW() : 24;
  s_cjkH = g_font.glyphH() ? g_font.glyphH() : 24;

  memset(&s_fontCjk, 0, sizeof(s_fontCjk));
  s_fontCjk.get_glyph_dsc = cjkGetGlyphDsc;
  s_fontCjk.get_glyph_bitmap = cjkGetGlyphBitmap;
  s_fontCjk.line_height = s_lineHeight;
  s_fontCjk.base_line = s_baseLine;
  s_fontCjk.subpx = LV_FONT_SUBPX_NONE;
  s_fontCjk.underline_position = -2;
  s_fontCjk.underline_thickness = 2;
  s_fontCjk.dsc = nullptr;
  s_fontCjk.fallback = nullptr;

  Serial.printf("[lvgl] 中文字体桥接: %ux%u, 字库=%s, 字形数=%u, 子集=%lu 字, PSRAM字形=%lu, 文件位深=%u\n",
                (unsigned)s_cjkW, (unsigned)s_cjkH, g_font.ready() ? g_font.loadedPath() : "(未加载)",
                (unsigned)g_font.glyphCount(), (unsigned long)g_font.subsetGlyphCount(),
                (unsigned long)g_font.psramGlyphCount(), (unsigned)g_font.bpp());
  if (g_font.psramGlyphCount() > 0) {
    Serial.println(F("[lvgl] 全量字形位图已在 PSRAM -> 绘制阶段零 SD 读(最快路径)"));
  } else if (g_font.subsetGlyphCount() > 0) {
    Serial.printf("[lvgl] 常用字子集已就绪(%s, %lu 字, %ubpp) -> 常用字零 SD 读; "
                  "生僻字回落 SD 主字库(约 105ms/字, 很少发生)\n",
                  g_font.subsetPath(), (unsigned long)g_font.subsetGlyphCount(), (unsigned)g_font.subsetBpp());
  } else if (g_font.ready()) {
    Serial.println(F("[lvgl] !! 既没有子集也没有全量 PSRAM 档 -> 每个新字一次 SD 读(约 105ms), 翻页会卡。"
                     "请把 tools/out/subset24.f24 放到 SD 卡 /fonts/ 下(见 config.h 说明)"));
  }
}

const lv_font_t *uiLvglFont(void) {
  return &s_fontCjk;
}

// ===========================================================================
// 2) 第 1 阶段示例界面: 一屏中文 + 一个可旋转选择的列表
// ===========================================================================
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_group_t *s_group = nullptr;

static const char *const s_items[] = {
    "打开小说",
    "翻到下一页",
    "章节目录",
    "播放音乐",
    "显示插图",
    "调节音量",
    "关于本机",
};
#define ITEM_COUNT ((int)(sizeof(s_items) / sizeof(s_items[0])))

static void updateStatusFromItem(lv_obj_t *btn) {
  if (!s_status) {
    return;
  }
  const char *txt = lv_list_get_btn_text(s_list, btn);
  lv_label_set_text_fmt(s_status, "已选: %s", txt ? txt : "?");
}

static void onItemEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *item = lv_event_get_target(e);

  if (code == LV_EVENT_FOCUSED) {
    // 焦点移动时把该项滚进可视区(列表长了也不会跑到屏幕外)
    lv_obj_scroll_to_view(item, LV_ANIM_ON);
    updateStatusFromItem(item);
  } else if (code == LV_EVENT_CLICKED) {
    updateStatusFromItem(item);
    Serial.printf("[lvgl] 按下: %s\n", lv_list_get_btn_text(s_list, item));
  }
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &s_fontCjk, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

static void buildDemoScreen(void) {
  s_scr = lv_scr_act();
  lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  // ★ 中文字体设在屏幕对象上, 子对象会继承 —— 这样每个控件不用单独设字体 ★
  lv_obj_set_style_text_font(s_scr, &s_fontCjk, 0);
  lv_obj_set_style_text_color(s_scr, lv_color_white(), 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---- 标题(中文, 走 DotFont 桥接) ----
  lv_obj_t *title = makeLabel(s_scr, "LVGL 中文显示测试", lv_color_hex(0x05FF));
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  // ---- 可旋转选择的列表 ----
  s_list = lv_list_create(s_scr);
  lv_obj_set_size(s_list, TFT_W - 6, 214);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_text_font(s_list, &s_fontCjk, 0);
  lv_obj_set_style_bg_color(s_list, lv_color_hex(0x101010), 0);
  lv_obj_set_style_border_width(s_list, 1, 0);
  lv_obj_set_style_border_color(s_list, lv_color_hex(0x404040), 0);

  lv_obj_t *first = nullptr;
  for (int i = 0; i < ITEM_COUNT; i++) {
    lv_obj_t *btn = lv_list_add_btn(s_list, nullptr, s_items[i]);
    lv_obj_set_style_text_font(btn, &s_fontCjk, 0);
    lv_obj_add_event_cb(btn, onItemEvent, LV_EVENT_ALL, nullptr);
    lv_group_add_obj(s_group, btn);
    if (i == 0) {
      first = btn;
    }
  }

  // ---- 底部: 当前选中 + 操作提示 ----
  s_status = makeLabel(s_scr, "已选: -", lv_color_hex(0xE7E0));
  lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -36);

  lv_obj_t *hint = makeLabel(s_scr, "旋转=选择  按下=确定", lv_color_hex(0x8C71));
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

  if (first) {
    lv_group_focus_obj(first);
    updateStatusFromItem(first);
  }
}

// ===========================================================================
// 3) 对外接口
// ===========================================================================
static bool s_active = false;

bool uiLvglBegin(void) {
  if (s_active) {
    return true;
  }
  if (!lvglPortBegin()) {
    Serial.println(F("[lvgl] !! 端口初始化失败, 继续使用旧 UI"));
    return false;
  }

  cjkFontInit();

  s_group = lvglPortNewGroup();
  if (!s_group) {
    Serial.println(F("[lvgl] !! lv_group_create 失败(PSRAM 不够?)"));
    return false;
  }
  lvglPortAttachGroup(s_group);

  buildDemoScreen();

  // 首帧: 让 LVGL 立刻把整屏画出来, 不必等下一次 lv_timer_handler
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);

  s_active = true;
  Serial.println(F("[lvgl] 第 1 阶段界面已就绪: 中文标题 + 7 项可旋转列表"));
  return true;
}

bool uiLvglActive(void) {
  return s_active;
}

void uiLvglLoop(void) {
  lvglPortLoop();
}

#endif /* UI_BACKEND_LVGL */
