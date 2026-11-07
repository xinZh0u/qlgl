/**
 * @file    qlfont_ft.h
 * @brief   FreeType 矢量字体后端（可选，PC/MPU 用；MCU 不编译）。
 *
 * 与内置位图字体的差别：
 *   - 缺字才光栅化，字形缓存在 `QLFont` 里（`glyphs` / `glyph_bitmaps`）；
 *   - 8bpp 灰度位图（FreeType 的 FT_RENDER_MODE_NORMAL），每行 box_w 字节；
 *   - 支持 TTC（Noto Sans CJK 是 .ttc），face_index < 0 时自动挑简体中文字面。
 *
 * 用法：
 *     if (ql_font_ft_available()) {
 *         QLFont *f = ql_font_ft_load("/usr/share/fonts/.../NotoSansCJK-Regular.ttc", -1, 16);
 *         ql_label_set_font(label, f);
 *     }
 */
#ifndef QLGL_FONT_FT_H
#define QLGL_FONT_FT_H

#include "qlgl.h"

#if defined(QLGL_ENABLE_FREETYPE)

/*
 * 加载一个矢量字体。
 *   path        字体文件（.ttf/.otf/.ttc）
 *   face_index  TTC 里的字面下标；传 -1 自动挑选（优先 family 名里带 SC 的简体中文字面）
 *   pixel_size  字号（像素，非 pt）
 * 返回的 QLFont 需要 ql_font_destroy 释放；失败返回 NULL 并打错误日志。
 */
QLFont *ql_font_ft_load(const char *path, int face_index, int pixel_size);

/* 后端是否可用（总是 1，保留给上层做编译期分支） */
int ql_font_ft_available(void);

/*
 * 指定"默认矢量字体"，之后 ql_font_default(size) 会按字号创建缓存的动态字体，
 * qllabel / qlui 的 font-size 也会用上它。
 * 返回 0 表示加载失败（此时仍然回落到内置位图字体）。
 */
int ql_font_set_default_file(const char *path, int face_index);

/* 当前默认字体文件；没设置返回 NULL */
const char *ql_font_default_file(void);

/* 字形缓存上限（超过后新字形不再生成，日志告警一次） */
void ql_font_ft_set_cache_limit(uint32_t glyphs);

#endif /* QLGL_ENABLE_FREETYPE */

#endif /* QLGL_FONT_FT_H */
