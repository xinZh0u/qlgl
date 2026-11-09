/**
 * @file    qlfont.h
 * @brief   字体：字形表、字形查找、UTF-8 解码、文本度量。
 *
 * 两种字体共用同一套接口：
 *   1. **静态位图字体**（`ql_font_mono_12/16`，MCU 用）：字形表与位图都在 .rodata；
 *   2. **动态矢量字体**（M2，FreeType，见 `qlfont_ft.h`）：字形按需光栅化后缓存，
 *      `glyphs` / `glyph_bitmaps` 由后端持有并可在运行期增长。
 *
 * 字形按 codepoint 升序排列（动态字体插入时也保持有序），查找用二分；
 * 缺字用 font->fallback 兜底。
 *
 * 重要：`ql_font_find_glyph` 返回的指针在下一次查找前有效
 * （动态字体插入字形时可能 realloc）。绘制路径都是"取一个、画一个"，满足该约束。
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

typedef struct QLFontBackend QLFontBackend;

typedef struct QLFont {
    const char     *name;
    uint8_t         bpp;          /* 1 / 4 / 8 */
    int16_t         line_height;  /* ascent + descent */
    int16_t         ascent;       /* 基线到顶部 */
    int16_t         descent;      /* 基线到底部 */
    uint32_t        fallback;     /* 缺字时改用的 codepoint；0 = 不画只前进 */
    uint16_t        glyph_count;
    const QLGlyph  *glyphs;
    const uint8_t  *bitmap;       /* 静态字体：所有字形共用一块位图 */
    /*
     * --- 以下字段只对动态字体有意义；静态字体保持全 0/NULL ---
     * glyph_bitmaps[i] 是 glyphs[i] 自己的位图（行优先、每行 glyphs[i].box_w 字节）。
     * 有它就说明字形位图不是从 bitmap + data_offset 算出来的。
     */
    const uint8_t *const *glyph_bitmaps;
    const QLFontBackend *backend;
    void           *impl;         /* 后端私有数据（FreeType 句柄/缓存） */
    uint16_t        glyph_capacity;
    uint16_t        pixel_size;   /* 该字体标称的字号（动态字体用） */
} QLFont;

/* 动态字体后端：只负责"给我某个 codepoint 的字形"和"销毁" */
struct QLFontBackend {
    /* 取字形，必要时光栅化 + 插入（保持 glyphs 按 codepoint 有序）；失败返回 NULL */
    const QLGlyph *(*ensure_glyph)(QLFont *font, uint32_t codepoint);
    void (*destroy)(QLFont *font);
};

/* 内置字体（由 tools/qlfontgen 生成） */
extern const QLFont ql_font_mono_12;
extern const QLFont ql_font_mono_16;

/* 查找字形；找不到时返回 fallback 字形，再找不到返回 NULL */
const QLGlyph *ql_font_find_glyph(const QLFont *font, uint32_t codepoint);

/* 单个字形的位图行字节数 */
uint32_t ql_font_glyph_stride(const QLFont *font, const QLGlyph *glyph);

/* 取字形某一行的位图起始指针（每行按 bpp 打包）；越界返回 NULL */
const uint8_t *ql_font_glyph_row(const QLFont *font, const QLGlyph *glyph, int32_t y);

/* 取字形某个像素的覆盖率（0..255） */
uint8_t ql_font_glyph_alpha(const QLFont *font, const QLGlyph *glyph, int32_t x, int32_t y);

/* 动态字体？(1 = 由后端托管，内存需要 ql_font_destroy) */
int ql_font_is_dynamic(const QLFont *font);

/* 释放动态字体；传入静态字体或 NULL 时无操作 */
void ql_font_destroy(QLFont *font);

/*
 * 按字号取一个"默认字体"：配置了矢量字体就返回对应的动态字体（带缓存），
 * 否则回落到最接近的内置位图字体。qllabel / qlui / 绘制层共用。
 * 返回的字体由内部持有，调用方不要释放。
 */
const QLFont *ql_font_default(int pixel_size);

/* 内部接线：矢量字体后端把"按字号造字体"的工厂挂进来（qlgl_init 之后由 FT 后端调用） */
void ql_font_set_default_factory(QLFont *(*factory)(int pixel_size));

/*
 * 等宽字体工厂（可选）。没设时 `ql_font_mono(size)` 回落到 `ql_font_default` ——
 * 内建位图字体本身就是等宽的，所以"没接矢量字体"时两级完全一样。
 * 接了 FreeType 之后，`font-family: monospace` 才真的换字体。
 */
void ql_font_set_mono_factory(QLFont *(*factory)(int pixel_size));

/* 按字号取等宽字体（见 ql_font_set_mono_factory） */
const QLFont *ql_font_mono(int pixel_size);
/* 释放默认字体缓存里所有动态字体（qlgl_shutdown 调用） */
void ql_font_reset_defaults(void);

/* UTF-8 解码：成功返回字节数（1..4），非法字节返回 1 且 *codepoint = 0xFFFD */
int32_t ql_utf8_decode(const char *text, uint32_t *codepoint);

/* UTF-8 编码：返回写入字节数（最多 4），buf 至少 4 字节 */
int32_t ql_utf8_encode(uint32_t codepoint, char *buf);

/* 统计文本的像素尺寸；len < 0 表示以 '\0' 结尾。支持 '\n' 换行 */
void ql_font_measure_text(const QLFont *font, const char *text, int32_t len,
                          ql_coord_t *out_w, ql_coord_t *out_h);

/* 单行宽度（不含换行处理） */
ql_coord_t ql_font_line_width(const QLFont *font, const char *text, int32_t len);

/*
 * 同上，另外把 `letter_spacing` 加在**每个字符之后**（CSS 的算法：
 * 末字符后面那一份也算进宽度，所以居中/右对齐会随之偏移半个字距左右）。
 */
ql_coord_t ql_font_line_width_ls(const QLFont *font, const char *text, int32_t len,
                                 ql_coord_t letter_spacing);

/*
 * 再加两样（B3 的 `word-spacing` / `tab-size`）：
 *   - `word_spacing` 只加在**空格之后**（CSS 的 word-spacing 就是这么算的）；
 *   - `tab_size` != 0 时，'\t' 按 `tab_size` 个空格计宽（而不是当作 0 宽字形）。
 * 两样都为零时与 `ql_font_line_width_ls` 逐像素一致。
 */
ql_coord_t ql_font_line_width_ls2(const QLFont *font, const char *text, int32_t len,
                                  ql_coord_t letter_spacing, ql_coord_t word_spacing,
                                  uint8_t tab_size);

/* 行数：'\n' 分隔，空串算 1 行 */
uint32_t ql_font_line_count(const char *text, int32_t len);

#endif /* QLGL_FONT_H */
