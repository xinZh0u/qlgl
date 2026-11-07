/**
 * @file    qlfont.h
 * @brief   位图字体：字形表、字形查找、UTF-8 解码、文本度量。
 *
 * v0.1 只支持位图字形（1/4/8 bpp），不做矢量、不做连字。
 * 字形按 codepoint 升序排列，查找用二分；缺字用 font->fallback 兜底。
 */
#ifndef QLGL_FONT_H
#define QLGL_FONT_H

#include "qlgl.h"
#include "qlgeom.h"

/* 一个字形：度量 + 位图偏移（位图统一放在 QLFont.bitmap 里） */
typedef struct {
    uint32_t codepoint;
    int16_t  adv_w;        /* 前进宽度（像素） */
    uint8_t  box_w;        /* 墨迹宽 */
    uint8_t  box_h;        /* 墨迹高 */
    int8_t   ofs_x;        /* 墨迹左边界相对笔位 */
    int8_t   ofs_y;        /* 墨迹上边界相对基线（负值表示基线之上） */
    uint16_t data_offset;  /* 相对 QLFont.bitmap 的字节偏移 */
} QLGlyph;

typedef struct QLFont {
    const char     *name;
    uint8_t         bpp;          /* 1 / 4 / 8 */
    int16_t         line_height;  /* ascent + descent */
    int16_t         ascent;       /* 基线到顶部 */
    int16_t         descent;      /* 基线到底部 */
    uint32_t        fallback;     /* 缺字时改用的 codepoint；0 = 不画只前进 */
    uint16_t        glyph_count;
    const QLGlyph  *glyphs;
    const uint8_t  *bitmap;
} QLFont;

/* 内置字体（由 tools/qlfontgen 生成） */
extern const QLFont ql_font_mono_12;
extern const QLFont ql_font_mono_16;

/* 查找字形；找不到时返回 fallback 字形，再找不到返回 NULL */
const QLGlyph *ql_font_find_glyph(const QLFont *font, uint32_t codepoint);

/* 单个字形的位图行字节数 */
uint32_t ql_font_glyph_stride(const QLFont *font, const QLGlyph *glyph);

/* 取字形某个像素的覆盖率（0..255） */
uint8_t ql_font_glyph_alpha(const QLFont *font, const QLGlyph *glyph, int32_t x, int32_t y);

/* UTF-8 解码：成功返回字节数（1..4），非法字节返回 1 且 *codepoint = 0xFFFD */
int32_t ql_utf8_decode(const char *text, uint32_t *codepoint);

/* UTF-8 编码：返回写入字节数（最多 4），buf 至少 4 字节 */
int32_t ql_utf8_encode(uint32_t codepoint, char *buf);

/* 统计文本的像素尺寸；len < 0 表示以 '\0' 结尾。支持 '\n' 换行 */
void ql_font_measure_text(const QLFont *font, const char *text, int32_t len,
                          ql_coord_t *out_w, ql_coord_t *out_h);

/* 单行宽度（不含换行处理） */
ql_coord_t ql_font_line_width(const QLFont *font, const char *text, int32_t len);

/* 行数：'\n' 分隔，空串算 1 行 */
uint32_t ql_font_line_count(const char *text, int32_t len);

#endif /* QLGL_FONT_H */
