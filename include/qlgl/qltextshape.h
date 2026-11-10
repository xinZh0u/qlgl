/**
 * @file    qltextshape.h
 * @brief   文本整形（B8）：把一段 UTF-8 变成"已定位的字形序列"。
 *
 * 两条实现：
 *   1. **HarfBuzz**（首选，PC/Mobile）：连字、字距、复杂文字（阿拉伯/印度语系）、
 *      数字特性全靠它。构建期找到头文件就直连，否则运行期 `dlopen`；
 *   2. **内建**（兜底）：逐码位查字形 + 逐字回退，没有连字与字距，
 *      行为与"整形关闭"时逐像素一致。
 *
 * 用法只有两个入口：
 *   - 量宽度：`ql_text_shape(font, text, len, &opt, NULL, 0, &w)`
 *   - 画一行：`ql_text_shape(font, text, len, &opt, out, cap, NULL)` 再逐个画
 *
 * 为什么是"一次性给出整行"而不是逐字问：连字/字距/阿拉伯文的字形变换都是
 * **上下文相关**的，逐字接口在原理上就做不到。
 */
#ifndef QLGL_TEXT_SHAPE_H
#define QLGL_TEXT_SHAPE_H

#include "qlgl.h"
#include "qlfont.h"

/*
 * 一次整形最多产出多少个字形。绘制层按这个大小**分块**整形（一块 256 个字形
 * ≈ 6 KB 静态缓冲），所以长行不会丢内容，只是跨块边界的连字可能断掉
 * （一块 256 个字形已经远超一行能显示的字数）。
 */
#ifndef QL_TEXT_SHAPE_MAX
#define QL_TEXT_SHAPE_MAX  256
#endif

/* 整形出来的一个字形 */
typedef struct {
    const QLFont  *font;     /* 真正提供这个字形的字体（逐字回退时会换） */
    const QLGlyph *glyph;    /* NULL = 这个码位哪儿都没有（只按 advance 前进） */
    uint16_t       cluster;  /* 源文本里这个字形对应的**第一个字节** */
    /*
     * 摆放量：`dx` 是相对当前笔位的水平微调（阿拉伯文的连写位移、组合标记），
     * `adv` 是画完之后笔位要前进多少（**已含**字间距/词间距）。
     * 字形序列本身就是**视觉顺序**（整形的责任），调用方从左往右累加即可。
     */
    int16_t        dx;
    int16_t        adv;
} QLShapeGlyph;

/* 整形选项（与 QLUIStyle 里的那几个属性一一对应） */
typedef struct {
    ql_coord_t letter_spacing;  /* 每个字符之后加（含最后一个，与 CSS 一致） */
    ql_coord_t word_spacing;    /* 只加在空格之后 */
    uint8_t    tab_size;        /* != 0 时 '\t' 按 N 个空格宽 */
    uint8_t    ligatures;       /* 0 = 关掉 `liga`/`clig`（font-variant-ligatures: none） */
    uint8_t    kerning;         /* 0 = 关掉 `kern`（font-feature-settings: "kern" 0） */
    /*
     * 段落基准方向（`direction: rtl`）。整形**自带双向处理**：
     *   - 按"字体 + 方向"切段，每段单独整形（HarfBuzz 会按方向给出视觉顺序）；
     *   - 段落是 RTL 时，把各段整体按从右往左排列（段内字形顺序不变）；
     *   - 没有 HarfBuzz 时用内建 UAX#9 子集先把整行排成视觉顺序。
     * 所以绘制方**不需要**再自己重排文本。
     */
    uint8_t    par_rtl;
    /*
     * OpenType 特性开关（B9）：TRUE 时给 HarfBuzz 挂上 `smcp` / `tnum`。
     * 内建整形没有特性表，见 `ql_text_shape_has_harfbuzz()`。
     */
    uint8_t    small_caps;
    uint8_t    tabular_nums;
} QLShapeOpt;

/*
 * 整形一行文本（不含换行处理，调用方先按行切开）。
 *
 *   out/cap  输出缓冲；`out == NULL` 时只算宽度（cap 忽略）
 *   out_width 返回整行宽度（含字距/字间距），可为 NULL
 * 返回写入的字形个数（>= 0）。
 */
int32_t ql_text_shape(const QLFont *font, const char *text, int32_t len,
                      const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                      ql_coord_t *out_width);

/* 当前实际生效的后端名："harfbuzz" / "builtin"（文档与测试用） */
const char *ql_text_shape_backend(void);

/* 运行期能力探测：HarfBuzz 是否可用（只查一次，结果缓存） */
int ql_text_shape_has_harfbuzz(void);

/*
 * 丢掉整形后端的字体缓存（字体表被重建 / qlgl_shutdown 时调用）。
 * `hb_font_t` 引用着 FreeType face，face 活着它才能用，所以要跟着生命周期走。
 */
void ql_text_shape_reset(void);

/*
 * 某个字体要销毁了：把挂在它上面的整形句柄放掉。
 * `hb_font_t` 引用着 FreeType face，face 一死它就悬空 —— 所以必须先放它。
 * `ql_font_destroy()` 会自动调，调用方不用管。
 */
void ql_text_shape_forget(const QLFont *font);

#endif /* QLGL_TEXT_SHAPE_H */
