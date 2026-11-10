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

/*
 * 文本栈（B8）靠 FreeType 打开字体文件：没有 FreeType 就不可能有 @font-face /
 * 家族列表。这个组合直接**在编译期报错**（而不是留到链接期报"未定义符号"）。
 */
#if QL_FEAT_CSS_FONT_STACK && !defined(QLGL_ENABLE_FREETYPE)
#error "QL_FEAT_CSS_FONT_STACK=1 需要 QLGL_ENABLE_FREETYPE=1（文本栈要用 FreeType 读字体文件）"
#endif

#if defined(QLGL_ENABLE_FREETYPE)

/*
 * 加载一个矢量字体。
 *   path        字体文件（.ttf/.otf/.ttc）
 *   face_index  TTC 里的字面下标；传 -1 自动挑选（优先 family 名里带 SC 的简体中文字面）
 *   pixel_size  字号（像素，非 pt）
 * 返回的 QLFont 需要 ql_font_destroy 释放；失败返回 NULL 并打错误日志。
 */
QLFont *ql_font_ft_load(const char *path, int face_index, int pixel_size);

/*
 * 从内存加载（字体被打进二进制、或从资源包解出来时用）。
 * 内存会被**拷贝**一份，调用方随后可以释放自己的缓冲。
 */
QLFont *ql_font_ft_load_mem(const void *data, long size, int face_index, int pixel_size);

/*
 * 取出动态字体背后的 FreeType face（整形后端建 hb_font 要用）。
 * 不是 FT 后端造的字体时返回 NULL。
 */
void *ql_font_ft_face(const QLFont *font);

/*
 * 字体文件里的**字形下标**（整形后端用它把 codepoint 映射到 gid）。
 * 字体里没有这个字时返回 0。
 */
uint32_t ql_font_ft_glyph_index(const QLFont *font, uint32_t codepoint);

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

/* ============================ 文本栈（B8） ============================ */
#if QL_FEAT_CSS_FONT_STACK

/*
 * 字面注册表：`@font-face` 在**运行期**落到这里，一个 (family, weight, italic)
 * 对应一个字体文件（路径或内存）。注册是幂等的：同 key 重复注册直接覆盖。
 *
 *   ql_font_family_register("DejaVu Sans", "/usr/share/fonts/.../DejaVuSans.ttf", 400, 0);
 *   ql_font_family_register_mem("MyFont", bytes, size, 700, 1);   // 内存字体（TTC 取第 0 字面）
 *
 * 返回 1 成功；0 = 表满 / 参数非法 / 字体读不出来（坏文件当场就报，不留到绘制时）。
 */
int ql_font_family_register(const char *family, const char *path, int weight, int italic);
int ql_font_family_register_mem(const char *family, const void *data, long size, int weight,
                                int italic);

/* 家族名 → family_id（1..n）；没注册过返回 0 */
int ql_font_family_id(const char *family);

/*
 * 按 (家族, 字重, 斜体, 字号) 取字体，内部按字号缓存。
 * 家族里没有精确字面时按"字重距离 + 斜体是否匹配"挑最接近的一张
 * （挑不到真斜体时会返回常规字面，由绘制层做合成斜体/加粗）。
 * 返回 NULL 表示这个家族一张字面都没有。
 */
const QLFont *ql_font_family_font(int family_id, int weight, int italic, int pixel_size);

/* 清空注册表（连同缓存字体一起释放）。qlgl_shutdown 会调。 */
void ql_font_family_reset(void);

#endif /* QL_FEAT_CSS_FONT_STACK */

#endif /* QLGL_ENABLE_FREETYPE */

#endif /* QLGL_FONT_FT_H */
