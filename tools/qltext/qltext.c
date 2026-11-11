/**
 * @file    qltext.c
 * @brief   构建期宿主工具：HTML + CSS → 静态 C 表 / .qlui blob（M3.5 第 4 步）。
 *
 * 关键设计：**HTML/CSS 的语义只在 C 侧实现一次**（`src/ui/qlui_parse.c`）。
 * 这个工具就是"把 `ql_ui_load_text()` 的产物落盘"，所以不存在
 * Python 与 C 两份解析器互相漂移的可能 —— 这也是第 4 步的全部内容。
 *
 * 用法：
 *     qltext cc   --html a.html --css b.css --name foo --out DIR
 *                  → DIR/ui_foo.c/.h/_blob.c（可直接进目标机构建）
 *     qltext blob --html a.html --css b.css --out out.qlui [--name foo]
 *                  → 直接产出 blob（不需要编译器参与）
 *     qltext check --html a.html --css b.css [--name foo]
 *                  → 只解析并打印规模（CI 里当语法检查用）
 *
 * 产出的 doc 与旧 `qlui.py` **逐字节相同**（`test_golden` 用提交在
 * `test/golden` 里的基准守着）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define ql_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define ql_mkdir(p) mkdir((p), 0777)
#endif

#include <qlgl/qlgl.h>
#include <qlgl/qluitext.h>

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1u);
    if (p == NULL) {
        fprintf(stderr, "qltext: 内存不足（%zu 字节）\n", n);
        exit(1);
    }
    return p;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "qltext: 打不开 \"%s\"\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)xmalloc((size_t)sz + 1u);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        fprintf(stderr, "qltext: 读 \"%s\" 时短读\n", path);
        free(buf);
        return NULL;
    }
    /* 去掉 BOM：构建期工具不该因为编辑器加了 BOM 就报"第一个字符不认识" */
    size_t skip = 0;
    if (got >= 3u && (unsigned char)buf[0] == 0xEFu &&
        (unsigned char)buf[1] == 0xBBu && (unsigned char)buf[2] == 0xBFu)
        skip = 3u;
    if (skip > 0) memmove(buf, buf + skip, got - skip + 1u);
    buf[got - skip] = '\0';
    return buf;
}

/* 与 qlui.py 的 `re.sub(r"[^\w]", "_", name)` 对齐 */
static void sanitize_ident(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] != '\0' && i + 1u < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                 (c >= 'a' && c <= 'z') || c == '_';
        out[i] = ok ? (char)c : '_';
    }
    out[i] = '\0';
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return (s != NULL) ? s + 1 : path;
}

/* 本构建实际编进来的解码器（用于错误信息，别让用户猜） */
static const char *qltext_decode_formats(void)
{
#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE
    return "BMP/QOI/PNG";
#elif QL_FEAT_IMG_BMP && QL_FEAT_IMG_QOI
    return "BMP/QOI";
#elif QL_FEAT_IMG_BMP
    return "BMP";
#elif QL_FEAT_IMG_QOI
    return "QOI";
#else
    return "只有构建期已转好的 RAW";
#endif
}

/* ------------------------------------------------------------------ */
/* 枚举 → 名字（让生成出来的 C 可读，而不是一堆魔数）                  */
/* ------------------------------------------------------------------ */

/* 顺序必须与 include/qlgl/qlui.h 的 QLUITag 一致 */
static const char *const TAG_NAMES[] = {
    "QL_UI_TAG_DIV", "QL_UI_TAG_SECTION", "QL_UI_TAG_P", "QL_UI_TAG_SPAN",
    "QL_UI_TAG_B", "QL_UI_TAG_I", "QL_UI_TAG_BUTTON", "QL_UI_TAG_H1",
    "QL_UI_TAG_H2", "QL_UI_TAG_H3", "QL_UI_TAG_H4", "QL_UI_TAG_H5",
    "QL_UI_TAG_H6", "QL_UI_TAG_HEADER", "QL_UI_TAG_FOOTER", "QL_UI_TAG_NAV",
    "QL_UI_TAG_MAIN", "QL_UI_TAG_ARTICLE", "QL_UI_TAG_ASIDE",
    "QL_UI_TAG_FIGURE", "QL_UI_TAG_FIGCAPTION", "QL_UI_TAG_BLOCKQUOTE",
    "QL_UI_TAG_UL", "QL_UI_TAG_OL", "QL_UI_TAG_LI", "QL_UI_TAG_HR",
    "QL_UI_TAG_BR", "QL_UI_TAG_MARK", "QL_UI_TAG_EM", "QL_UI_TAG_STRONG",
    "QL_UI_TAG_U", "QL_UI_TAG_S", "QL_UI_TAG_INPUT", "QL_UI_TAG_SELECT",
    "QL_UI_TAG_OPTION",
    "QL_UI_TAG_HTML", "QL_UI_TAG_BODY", "QL_UI_TAG_DL", "QL_UI_TAG_DT",
    "QL_UI_TAG_DD", "QL_UI_TAG_MENU", "QL_UI_TAG_ADDRESS",
    "QL_UI_TAG_HGROUP", "QL_UI_TAG_SEARCH", "QL_UI_TAG_A", "QL_UI_TAG_ABBR",
    "QL_UI_TAG_CODE", "QL_UI_TAG_KBD", "QL_UI_TAG_SAMP", "QL_UI_TAG_VAR",
    "QL_UI_TAG_Q", "QL_UI_TAG_DFN", "QL_UI_TAG_CITE", "QL_UI_TAG_DATA",
    "QL_UI_TAG_TIME", "QL_UI_TAG_SMALL", "QL_UI_TAG_BDI", "QL_UI_TAG_BDO",
    "QL_UI_TAG_WBR", "QL_UI_TAG_DEL", "QL_UI_TAG_INS", "QL_UI_TAG_FORM",
    "QL_UI_TAG_LABEL", "QL_UI_TAG_FIELDSET", "QL_UI_TAG_LEGEND",
    "QL_UI_TAG_OPTGROUP",
    "QL_UI_TAG_BASE", "QL_UI_TAG_HEAD", "QL_UI_TAG_LINK", "QL_UI_TAG_META",
    "QL_UI_TAG_STYLE", "QL_UI_TAG_TITLE", "QL_UI_TAG_OUTPUT",
    "QL_UI_TAG_DATALIST", "QL_UI_TAG_PROGRESS", "QL_UI_TAG_METER",
    "QL_UI_TAG_DETAILS", "QL_UI_TAG_SUMMARY", "QL_UI_TAG_DIALOG", "QL_UI_TAG_IMG",
    "QL_UI_TAG_PICTURE", "QL_UI_TAG_SOURCE", "QL_UI_TAG_MAP", "QL_UI_TAG_AREA",
    "QL_UI_TAG_IFRAME", "QL_UI_TAG_EMBED", "QL_UI_TAG_OBJECT", "QL_UI_TAG_AUDIO",
    "QL_UI_TAG_VIDEO", "QL_UI_TAG_TRACK", "QL_UI_TAG_CANVAS", "QL_UI_TAG_SCRIPT",
    "QL_UI_TAG_NOSCRIPT", "QL_UI_TAG_SLOT", "QL_UI_TAG_TEMPLATE", "QL_UI_TAG_SVG",
    "QL_UI_TAG_MATH",
    "QL_UI_TAG_PRE", "QL_UI_TAG_SUB", "QL_UI_TAG_SUP",
    "QL_UI_TAG_TEXTAREA",
    "QL_UI_TAG_TABLE", "QL_UI_TAG_CAPTION", "QL_UI_TAG_THEAD",
    "QL_UI_TAG_TBODY", "QL_UI_TAG_TFOOT", "QL_UI_TAG_TR", "QL_UI_TAG_TD",
    "QL_UI_TAG_TH", "QL_UI_TAG_COLGROUP", "QL_UI_TAG_COL",
};

static const char *const DISPLAY_NAMES[] = {
    "QL_UI_DISPLAY_BLOCK", "QL_UI_DISPLAY_INLINE",
    "QL_UI_DISPLAY_INLINE_BLOCK", "QL_UI_DISPLAY_NONE",
    "QL_UI_DISPLAY_FLEX", "QL_UI_DISPLAY_INLINE_FLEX",
    "QL_UI_DISPLAY_GRID", "QL_UI_DISPLAY_INLINE_GRID",
    "QL_UI_DISPLAY_TABLE", "QL_UI_DISPLAY_TABLE_ROW", "QL_UI_DISPLAY_TABLE_CELL",
    "QL_UI_DISPLAY_TABLE_CAPTION", "QL_UI_DISPLAY_TABLE_ROW_GROUP",
    "QL_UI_DISPLAY_TABLE_COLUMN", "QL_UI_DISPLAY_TABLE_COLUMN_GROUP",
};

static const char *const FLEX_DIR_NAMES[] = {
    "QL_UI_FLEX_ROW", "QL_UI_FLEX_ROW_REVERSE",
    "QL_UI_FLEX_COLUMN", "QL_UI_FLEX_COLUMN_REVERSE",
};

static const char *const JUSTIFY_NAMES[] = {
    "QL_UI_JUSTIFY_START", "QL_UI_JUSTIFY_CENTER", "QL_UI_JUSTIFY_END",
    "QL_UI_JUSTIFY_SPACE_BETWEEN", "QL_UI_JUSTIFY_SPACE_AROUND",
    "QL_UI_JUSTIFY_SPACE_EVENLY",
};

static const char *const ALIGN_NAMES[] = {
    "QL_UI_ALIGN_STRETCH", "QL_UI_ALIGN_START",
    "QL_UI_ALIGN_CENTER", "QL_UI_ALIGN_END",
};

static const char *const ALIGN_CONTENT_NAMES[] = {
    "QL_UI_ALIGN_CONTENT_STRETCH", "QL_UI_ALIGN_CONTENT_START",
    "QL_UI_ALIGN_CONTENT_CENTER",  "QL_UI_ALIGN_CONTENT_END",
    "QL_UI_ALIGN_CONTENT_SPACE_BETWEEN", "QL_UI_ALIGN_CONTENT_SPACE_AROUND",
    "QL_UI_ALIGN_CONTENT_SPACE_EVENLY",
};

static const char *const CURSOR_NAMES[] = {
    "QL_UI_CURSOR_AUTO", "QL_UI_CURSOR_DEFAULT", "QL_UI_CURSOR_POINTER",
    "QL_UI_CURSOR_TEXT", "QL_UI_CURSOR_MOVE",    "QL_UI_CURSOR_NOT_ALLOWED",
    "QL_UI_CURSOR_HELP", "QL_UI_CURSOR_PROGRESS",
};

static const char *const BORDER_STYLE_NAMES[] = {
    "QL_UI_BORDER_SOLID", "QL_UI_BORDER_DASHED", "QL_UI_BORDER_DOTTED",
    "QL_UI_BORDER_NONE",
};

static const char *const POSITION_NAMES[] = {
    "QL_UI_POSITION_STATIC", "QL_UI_POSITION_RELATIVE",
    "QL_UI_POSITION_ABSOLUTE", "QL_UI_POSITION_FIXED",
};

static const char *const OVERFLOW_NAMES[] = {
    "QL_UI_OVERFLOW_VISIBLE", "QL_UI_OVERFLOW_HIDDEN", "QL_UI_OVERFLOW_AUTO",
};

static const char *const UNIT_NAMES[] = {
    "QL_UI_UNIT_AUTO", "QL_UI_UNIT_PX",   "QL_UI_UNIT_PERCENT",
    "QL_UI_UNIT_EM",   "QL_UI_UNIT_REM",  "QL_UI_UNIT_VW",
    "QL_UI_UNIT_VH",   "QL_UI_UNIT_VMIN", "QL_UI_UNIT_VMAX",
    "QL_UI_UNIT_CALC",
};

static const char *const EASE_NAMES[] = {
    "QL_EASE_LINEAR", "QL_EASE_IN", "QL_EASE_OUT", "QL_EASE_IN_OUT",
    "QL_EASE_CSS", "QL_EASE_STEPS",
};

static const char *const TEXT_ALIGN_NAMES[] = {
    "QL_TEXT_ALIGN_LEFT", "QL_TEXT_ALIGN_CENTER", "QL_TEXT_ALIGN_RIGHT",
};

static const char *const ANIM_DIR_NAMES[] = {
    "QL_UI_ANIM_NORMAL", "QL_UI_ANIM_REVERSE",
    "QL_UI_ANIM_ALTERNATE", "QL_UI_ANIM_ALTERNATE_REVERSE",
};

static const char *const AREA_SHAPE_NAMES[] = {
    "QL_UI_AREA_RECT", "QL_UI_AREA_CIRCLE", "QL_UI_AREA_POLY",
    "QL_UI_AREA_DEFAULT",
};

static const char *const ANIM_FILL_NAMES[] = {
    "QL_UI_ANIM_FILL_NONE", "QL_UI_ANIM_FILL_FORWARDS",
    "QL_UI_ANIM_FILL_BACKWARDS", "QL_UI_ANIM_FILL_BOTH",
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/*
 * 越界只可能来自"生成器和头文件里的枚举不一致"，属于真 bug；
 * 但生成工具没必要为此中断构建——数字写法语义完全相同，只是不好看。
 * 所以退化成数字并**明确告警**。
 */
static const char *enum_name(const char *const *tab, size_t n, unsigned v,
                             const char *what)
{
    static char buf[48];
    if (v < n) return tab[v];
    fprintf(stderr, "qltext: 警告：%s 的值 %u 超出已知范围（0..%zu），按数字写出\n",
            what, v, n - 1u);
    snprintf(buf, sizeof(buf), "(%s)%u", what, v);
    return buf;
}

static const char *ease_name(unsigned v)
{
    return enum_name(EASE_NAMES, COUNT_OF(EASE_NAMES), v, "QLEasing");
}

/* ------------------------------------------------------------------ */
/* 生成 ui_xxx.c                                                       */
/* ------------------------------------------------------------------ */

/* 非 ASCII 字节一律三位八进制转义：定长，不会吃掉后面字符，也不触发 -Wconversion */
static void fput_c_escaped(FILE *f, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)data[i];
        if (b == '"')       fputs("\\\"", f);
        else if (b == '\\') fputs("\\\\", f);
        else if (b >= 0x20u && b < 0x7Fu) fputc((int)b, f);
        else fprintf(f, "\\%03o", (unsigned)b);
    }
}

/*
 * 下标：0xFFFF 是"没有"，写成 0xFFFF 比写成 65535 好认（这是整个格式的哨兵值）。
 * first 为真时是行首，逗号位置随之变化。
 */
static void idx_ofs(FILE *f, uint16_t v, int first)
{
    const char *sep = first ? "" : ", ";
    if (v == 0xFFFFu) fprintf(f, "%s0xFFFF", sep);
    else fprintf(f, "%s%5u", sep, (unsigned)v);
}

static void fput_edges(FILE *f, QLUIEdges e)
{
    fprintf(f, "{ %d, %d, %d, %d }", (int)e.top, (int)e.right, (int)e.bottom,
            (int)e.left);
}

static void fput_length(FILE *f, QLUILength l)
{
    fprintf(f, "{ %d, %s }", (int)l.value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), l.unit, "QLUIUnit"));
}

static void fput_shadow(FILE *f, const QLUIShadow *sh)
{
    fprintf(f,
            "{ .dx = %d, .dy = %d, .blur = %u, .alpha = %u, .inset = %u,"
            " .spread = %d, .color = 0x%06X }",
            (int)sh->dx, (int)sh->dy, (unsigned)sh->blur, (unsigned)sh->alpha,
            (unsigned)sh->inset, (int)sh->spread, (unsigned)(sh->color & 0xFFFFFFu));
}

static void fput_style(FILE *f, const QLUIStyle *s)
{
    char bs_name[4][48];
    for (int i = 0; i < 4; i++)
        snprintf(bs_name[i], sizeof(bs_name[i]), "%s",
                 enum_name(BORDER_STYLE_NAMES, COUNT_OF(BORDER_STYLE_NAMES),
                           s->border_style4[i], "QLUIBorderStyle"));
    fprintf(f, "    { .bg_color = 0x%06X, .text_color = 0x%06X,\n",
            (unsigned)(s->bg_color & 0xFFFFFFu), (unsigned)(s->text_color & 0xFFFFFFu));
    fprintf(f, "      .bg_color2 = 0x%06X,\n", (unsigned)(s->bg_color2 & 0xFFFFFFu));
    fprintf(f, "      .border_color4 = { 0x%06X, 0x%06X, 0x%06X, 0x%06X },\n",
            (unsigned)(s->border_color4[0] & 0xFFFFFFu),
            (unsigned)(s->border_color4[1] & 0xFFFFFFu),
            (unsigned)(s->border_color4[2] & 0xFFFFFFu),
            (unsigned)(s->border_color4[3] & 0xFFFFFFu));
    fputs("      .shadow = { ", f);
    fput_shadow(f, &s->shadow[0]);
    fputs(",\n                  ", f);
    fput_shadow(f, &s->shadow[1]);
    fputs(" },\n      .text_shadow = ", f);
    fput_shadow(f, &s->text_shadow);
    fputs(",\n", f);
    fput_length(f, s->width);
    fputs(", .height = ", f);
    fput_length(f, s->height);
    fputs(",\n      .margin = ", f);
    fput_edges(f, s->margin);
    fputs(", .padding = ", f);
    fput_edges(f, s->padding);
    fprintf(f,
            ",\n"
            "      .display = %s, .bg_alpha = %u,\n"
            "      .border_w = { %u, %u, %u, %u },\n"
            "      .border_style4 = { %s, %s, %s, %s },\n"
            "      .radius = { %d, %d, %d, %d },\n"
            "      .text_align = %s, .font_id = %u,"
            " .box_sizing = %u,\n"
            "      .bg_grad_dir = %u,\n"
            "      .flex_direction = %s, .justify_content = %s,\n"
            "      .align_items = %s, .flex_wrap = %u, .gap_row = %d,"
            " .gap_col = %d, .gap_units = 0x%02X,\n"
            "      .flex_grow = %u, .flex_shrink = %u, .flex_basis = %d,"
            " .flex_basis_pct = %u,\n"
            "      .position = %s, .overflow = %s, .z_index = %d,\n"
            "      .trans_ms = %u, .trans_ease = %s, .trans_props = 0x%02X,\n"
            "      .trans_delay_ms = %u, .trans_steps = %u, .trans_bez_on = %u,\n"
            "      .trans_bez = { %d, %d, %d, %d },\n"
            "      .backdrop_blur = %u, .line_h = %d, .white_space = %u,"
            " .font_flags = 0x%02X,\n"
            "      .clip_kind = %u, .clip_r = %u, .opacity = %u,\n"
            "      .margin_unit = 0x%04X, .padding_unit = 0x%04X,\n"
            "      .aspect_w = %u, .aspect_h = %u,\n"
            "      .letter_spacing = %d, .word_spacing = %d, .tab_size = %u,"
            " .text_indent = %d, .text_transform = %u,"
            " .text_overflow = %u, .word_break = %u,\n"
            "      .deco_style = 0x%02X, .deco_thickness = %d, .deco_offset = %d,"
            " .deco_alpha = %u, .deco_color = 0x%06X,\n"
            "      .tf_x = %d, .tf_y = %d, .sc_x = %d, .sc_y = %d,\n"
            "      .visibility = %u, .pointer_events = %u,\n"
            "      .align_self = %s, .order = %d,\n"
            "      .overflow_x = %s, .overflow_y = %s,\n"
            "      .align_content = %s, .cursor = %s,\n"
            "      .user_select_none = %u, .scroll_smooth = %u,\n"
            "      .outline_color = 0x%06X, .outline_width = %d,"
            " .outline_offset = %d, .outline_style = %s,\n"
            "      .image_rendering = %u, .scrollbar_width = %u,"
            " .scrollbar_alpha = %u, .scrollbar_color = 0x%06X,\n"
            "      .object_position = { { %d, %s }, { %d, %s } },\n"
            "      .bg_size_mode = %u, .bg_repeat = %u,\n"
            "      .bg_size = { { %d, %s }, { %d, %s } },\n"
            "      .bg_pos = { { %d, %s }, { %d, %s } },\n"
            "      .anim = ",
            enum_name(DISPLAY_NAMES, COUNT_OF(DISPLAY_NAMES), s->display,
                      "QLUIDisplay"),
            (unsigned)s->bg_alpha,
            (unsigned)s->border_w[0], (unsigned)s->border_w[1],
            (unsigned)s->border_w[2], (unsigned)s->border_w[3],
            bs_name[0], bs_name[1], bs_name[2], bs_name[3],
            (int)s->radius[0], (int)s->radius[1], (int)s->radius[2], (int)s->radius[3],
            enum_name(TEXT_ALIGN_NAMES, COUNT_OF(TEXT_ALIGN_NAMES), s->text_align,
                      "QLTextAlign"),
            (unsigned)s->font_id, (unsigned)s->box_sizing,
            (unsigned)s->bg_grad_dir,
            enum_name(FLEX_DIR_NAMES, COUNT_OF(FLEX_DIR_NAMES), s->flex_direction,
                      "QLUIDirection"),
            enum_name(JUSTIFY_NAMES, COUNT_OF(JUSTIFY_NAMES), s->justify_content,
                      "QLUIJustify"),
            enum_name(ALIGN_NAMES, COUNT_OF(ALIGN_NAMES), s->align_items,
                      "QLUIAlign"),
            (unsigned)s->flex_wrap, (int)s->gap_row, (int)s->gap_col,
            (unsigned)s->gap_units,
            (unsigned)s->flex_grow, (unsigned)s->flex_shrink, (int)s->flex_basis,
            (unsigned)s->flex_basis_pct,
            enum_name(POSITION_NAMES, COUNT_OF(POSITION_NAMES), s->position,
                      "QLUIPosition"),
            enum_name(OVERFLOW_NAMES, COUNT_OF(OVERFLOW_NAMES), s->overflow,
                      "QLUIOverflow"),
            (int)s->z_index,
            (unsigned)s->trans_ms, ease_name(s->trans_ease),
            (unsigned)s->trans_props,
            (unsigned)s->trans_delay_ms, (unsigned)s->trans_steps,
            (unsigned)s->trans_bez_on,
            (int)s->trans_bez[0], (int)s->trans_bez[1], (int)s->trans_bez[2],
            (int)s->trans_bez[3],
            (unsigned)s->backdrop_blur, (int)s->line_h,
            (unsigned)s->white_space, (unsigned)s->font_flags,
            (unsigned)s->clip_kind, (unsigned)s->clip_r, (unsigned)s->opacity,
            (unsigned)s->margin_unit, (unsigned)s->padding_unit,
            (unsigned)s->aspect_w, (unsigned)s->aspect_h,
            (int)s->letter_spacing, (int)s->word_spacing, (unsigned)s->tab_size,
            (int)s->text_indent,
            (unsigned)s->deco_style, (int)s->deco_thickness, (int)s->deco_offset,
            (unsigned)s->deco_alpha, (unsigned)(s->deco_color & 0xFFFFFFu),
            (unsigned)s->text_transform, (unsigned)s->text_overflow,
            (unsigned)s->word_break,
            (int)s->tf_x, (int)s->tf_y, (int)s->sc_x, (int)s->sc_y,
            (unsigned)s->visibility, (unsigned)s->pointer_events,
            (s->align_self == QL_UI_ALIGN_SELF_AUTO)
                ? "QL_UI_ALIGN_SELF_AUTO"
                : enum_name(ALIGN_NAMES, COUNT_OF(ALIGN_NAMES), s->align_self,
                            "QLUIAlign"),
            (int)s->order,
            enum_name(OVERFLOW_NAMES, COUNT_OF(OVERFLOW_NAMES), s->overflow_x,
                      "QLUIOverflow"),
            enum_name(OVERFLOW_NAMES, COUNT_OF(OVERFLOW_NAMES), s->overflow_y,
                      "QLUIOverflow"),
            enum_name(ALIGN_CONTENT_NAMES, COUNT_OF(ALIGN_CONTENT_NAMES),
                      s->align_content, "QLUIAlignContent"),
            enum_name(CURSOR_NAMES, COUNT_OF(CURSOR_NAMES), s->cursor,
                      "QLUICursor"),
            (unsigned)s->user_select_none, (unsigned)s->scroll_smooth,
            (unsigned)(s->outline_color & 0xFFFFFFu), (int)s->outline_width,
            (int)s->outline_offset,
            enum_name(BORDER_STYLE_NAMES, COUNT_OF(BORDER_STYLE_NAMES),
                      s->outline_style, "QLUIBorderStyle"),
            (unsigned)s->image_rendering, (unsigned)s->scrollbar_width,
            (unsigned)s->scrollbar_alpha, (unsigned)(s->scrollbar_color & 0xFFFFFFu),
            (int)s->object_position[0].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->object_position[0].unit,
                      "QLUIUnit"),
            (int)s->object_position[1].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->object_position[1].unit,
                      "QLUIUnit"),
            (unsigned)s->bg_size_mode, (unsigned)s->bg_repeat,
            (int)s->bg_size[0].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->bg_size[0].unit, "QLUIUnit"),
            (int)s->bg_size[1].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->bg_size[1].unit, "QLUIUnit"),
            (int)s->bg_pos[0].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->bg_pos[0].unit, "QLUIUnit"),
            (int)s->bg_pos[1].value,
            enum_name(UNIT_NAMES, COUNT_OF(UNIT_NAMES), s->bg_pos[1].unit, "QLUIUnit"));
    /*
     * 资源名在**文本池**里的下标：doc 里已经是最终值，直接原样写出来。
     * （老代码这里写死成 `(uint16_t)0`，只有在"资源名恰好是池里第 0 条"时才对 ——
     *  bg 与 border 两张图同时存在时立刻暴露。）
     */
    if (s->anim == 0xFFFFu) fputs("0xFFFF", f);
    else fprintf(f, "0x%04X", (unsigned)s->anim);

    /*
     * 资源名 / 网格模板都是**池下标**，原样打出来。
     * （老代码把 bg_image 写成 `(uint16_t)0`，只有在"资源名恰好是文本池第 0 条"
     *  时才对；bg 与 border 两张图同时存在时立刻错位。）
     */
    fputs(",\n      .bg_image = ", f);
    if (s->bg_image == QL_UI_NO_IMAGE)
        fputs("QL_UI_NO_IMAGE", f);
    else
        fprintf(f, "%u", (unsigned)s->bg_image);

    fputs(", .border_image = ", f);
    if (s->border_image == QL_UI_NO_BORDER_IMAGE)
        fputs("QL_UI_NO_BORDER_IMAGE", f);
    else
        fprintf(f, "%u", (unsigned)s->border_image);
    fprintf(f,
            ", .bi_slice_pct = %u, .bi_width_pct = %u, .bi_repeat = %u,\n"
            "      .bi_slice = { %d, %d, %d, %d },"
            " .bi_width = { %d, %d, %d, %d },\n",
            (unsigned)s->bi_slice_pct, (unsigned)s->bi_width_pct,
            (unsigned)s->bi_repeat, (int)s->bi_slice[0], (int)s->bi_slice[1],
            (int)s->bi_slice[2], (int)s->bi_slice[3], (int)s->bi_width[0],
            (int)s->bi_width[1], (int)s->bi_width[2], (int)s->bi_width[3]);

    fputs("      .grid = ", f);
    if (s->grid == QL_UI_NO_GRID)
        fputs("QL_UI_NO_GRID", f);
    else
        fprintf(f, "%u", (unsigned)s->grid);
    fprintf(f,
            ", .grid_col = %d, .grid_row = %d, .grid_col_span = %u,"
            " .grid_row_span = %u,\n"
            "      .grid_flow = %u, .justify_items = %u, .justify_self = %u,"
            " .grid_auto_row = %d, .grid_auto_col = %d,\n",
            (int)s->grid_col, (int)s->grid_row, (unsigned)s->grid_col_span,
            (unsigned)s->grid_row_span, (unsigned)s->grid_flow,
            (unsigned)s->justify_items, (unsigned)s->justify_self,
            (int)s->grid_auto_row, (int)s->grid_auto_col);

    /*
     * 离屏图层的一组字段（M9.3）。`mask_image` 必须显式写 QL_UI_NO_MASK：
     * 不写的话静态初始化是 0，而 0 是"文本池第 0 条"这个合法下标 ——
     * 每个样式都会变成"带遮罩"，整棵树都去走离屏图层。
     */
    fputs("      .xform = ", f);
    if (s->xform < 0)
        fputs("QL_UI_NO_XFORM", f);
    else
        fprintf(f, "%d", (int)s->xform);
    fprintf(f, ", .origin = { { %d, %u, 0 }, { %d, %u, 0 } },\n",
            (int)s->origin[0].value, (unsigned)s->origin[0].unit,
            (int)s->origin[1].value, (unsigned)s->origin[1].unit);

    fputs("      .filter_n = ", f);
    fprintf(f, "%u", (unsigned)s->filter_n);
    fputs(", .filter_op = { ", f);
    for (int i = 0; i < QL_CFG_UI_FILTER_MAX_OPS; i++)
        fprintf(f, "%s0x%04X", (i == 0) ? "" : ", ", (unsigned)s->filter_op[i]);
    fprintf(f,
            " },\n"
            "      .ds_dx = %d, .ds_dy = %d, .ds_blur = %u, .ds_a = %u,"
            " .ds_color = 0x%06Xu,\n"
            "      .mix_blend = %u, .mask_image = ",
            (int)s->ds_dx, (int)s->ds_dy, (unsigned)s->ds_blur, (unsigned)s->ds_a,
            (unsigned)(s->ds_color & 0xFFFFFFu), (unsigned)s->mix_blend);
    if (s->mask_image == QL_UI_NO_MASK)
        fputs("QL_UI_NO_MASK", f);
    else
        fprintf(f, "%u", (unsigned)s->mask_image);

    fputs(",\n      .inset = { ", f);
    fput_length(f, s->inset[0]);
    fputs(", ", f);
    fput_length(f, s->inset[1]);
    fputs(", ", f);
    fput_length(f, s->inset[2]);
    fputs(", ", f);
    fput_length(f, s->inset[3]);
    /* B7 表格（blob v31） */
    fprintf(f,
            " },\n      .table_layout = %u, .border_collapse = %u, .valign = %u,"
            " .table_spacing_x = %d, .table_spacing_y = %d",
            (unsigned)s->table_layout, (unsigned)s->border_collapse,
            (unsigned)s->valign, (int)s->table_spacing_x, (int)s->table_spacing_y);

    /* 文本栈与双向（blob v33） */
    fprintf(f,
            ",\n      .direction = %u, .unicode_bidi = %u,"
            " .ligatures = %u, .kerning = %u",
            (unsigned)s->direction, (unsigned)s->unicode_bidi,
            (unsigned)s->ligatures, (unsigned)s->kerning);

    /* 多栏（blob v32） */
    fprintf(f,
            ",\n      .column_count = %u, .column_rule_w = %u,"
            " .column_rule_style = %u, .column_rule_color = 0x%06Xu,\n"
            "      .column_width = ",
            (unsigned)s->column_count, (unsigned)s->column_rule_w,
            (unsigned)s->column_rule_style, (unsigned)(s->column_rule_color & 0xFFFFFFu));
    fput_length(f, s->column_width);

    /*
     * clip-path 形状 + 字体变体 + 椭圆圆角（blob v34）。
     * `clip_kind` / `clip_r` 上面已经输出过（沿用老的输出位置），这里只补新增字段。
     */
    fputs(",\n      .clip_pts = ", f);
    if (s->clip_pts == QL_UI_NO_CLIP_PTS || s->clip_pt_n == 0)
        fputs("QL_UI_NO_CLIP_PTS", f);
    else
        fprintf(f, "%u", (unsigned)s->clip_pts);
    fprintf(f, ", .clip_pt_n = %u, .variant_flags = %u,\n      .clip_box = { ",
            (unsigned)s->clip_pt_n, (unsigned)s->variant_flags);
    for (int i = 0; i < 4; i++) {
        if (i != 0)
            fputs(", ", f);
        fput_length(f, s->clip_box[i]);
    }
    fprintf(f, " },\n      .radius_v = { %d, %d, %d, %d }",
            (int)s->radius_v[0], (int)s->radius_v[1], (int)s->radius_v[2],
            (int)s->radius_v[3]);

    fputs(" },\n", f);
}

/* 一次性建出 out_dir（老 qlui.py 用 os.makedirs，这里对齐它的行为） */
static void ensure_dir(const char *dir)
{
    char buf[1024];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, dir, n + 1u);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char save = buf[i];
            buf[i] = '\0';
            if (ql_mkdir(buf) != 0) { /* 已存在也走这条路，不必区分 */ }
            buf[i] = save;
        }
    }
}

static FILE *open_out(const char *out_dir, const char *ident, const char *ext)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s%s", out_dir, ident, ext);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "qltext: 写不了 \"%s\"\n", path);
        exit(1);
    }
    return f;
}


typedef struct {
    const char *html;
    const char *css;
    const char *name;
    const char *out;
    const char *root;          /* --root：图片资源根目录（给了才嵌入资源） */
    const char *pixel_format;  /* --pixel-format rgb565|rgb888|rgba8888|a8 */
    const char *platform;      /* --platform pc|mcu：只求降级报告，不产出文件 */
    const char *format;        /* --format raw（目前只支持 raw） */
    int         scale;         /* --scale N：构建期整数倍缩小（面积平均） */
} Args;

/* ------------------------------------------------------------------ */
/* 图片资源：构建期转码（--root）                                       */
/* ------------------------------------------------------------------ */

#define QL_ASSET_MAX   32
#define QL_ASSET_PATH  512

typedef struct {
    uint16_t text_index;      /* doc 文本池下标：资源名（<img src> 的值） */
    uint32_t off, len;        /* 像素字节在合并后的 image_data 里的位置 */
    uint16_t w, h;
} Asset;

typedef struct {
    Asset    item[QL_ASSET_MAX];
    int      count;
    uint8_t *data;            /* 所有资源的像素（拼在一起） */
    size_t   data_len, data_cap;
    uint8_t  pixel_format;
    int      scale;
} Assets;

static const char *pixel_format_name(uint8_t f)
{
    switch (f) {
    case QL_IMAGE_RGB565:   return "QL_IMAGE_RGB565";
    case QL_IMAGE_RGB888:   return "QL_IMAGE_RGB888";
    case QL_IMAGE_RGBA8888: return "QL_IMAGE_RGBA8888";
    default:                return "QL_IMAGE_A8";
    }
}

static int parse_pixel_format(const char *s)
{
    if (s == NULL || strcmp(s, "rgb565") == 0)
        return QL_IMAGE_RGB565;
    if (strcmp(s, "rgb888") == 0)
        return QL_IMAGE_RGB888;
    if (strcmp(s, "rgba8888") == 0)
        return QL_IMAGE_RGBA8888;
    if (strcmp(s, "a8") == 0)
        return QL_IMAGE_A8;
    return -1;
}

/* 整数倍面积平均缩小（构建期做，运行时就不用再缩） */
static uint8_t *downscale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride,
                          uint8_t format, int factor, uint16_t *ow, uint16_t *oh,
                          uint32_t *olen)
{
    if (factor <= 1)
        return NULL;
    const uint32_t bpp = ql_image_format_bpp(format);
    const uint32_t nw = w / (uint32_t)factor, nh = h / (uint32_t)factor;
    if (nw == 0 || nh == 0 || bpp == 0)
        return NULL;
    const size_t nlen = (size_t)nw * nh * bpp;
    uint8_t *out = (uint8_t *)calloc(1, nlen);
    if (out == NULL)
        return NULL;

    for (uint32_t y = 0; y < nh; y++) {
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t acc[4] = { 0, 0, 0, 0 };
            for (int dy = 0; dy < factor; dy++) {
                const uint8_t *row =
                    src + (size_t)(y * (uint32_t)factor + (uint32_t)dy) * stride;
                for (int dx = 0; dx < factor; dx++) {
                    const uint8_t *p =
                        row + (size_t)(x * (uint32_t)factor + (uint32_t)dx) * bpp;
                    if (format == QL_IMAGE_RGB565) {
                        uint16_t v;
                        memcpy(&v, p, 2);
                        acc[0] += (v >> 11) & 0x1Fu;
                        acc[1] += (v >> 5) & 0x3Fu;
                        acc[2] += v & 0x1Fu;
                    } else if (format == QL_IMAGE_RGB888) {
                        acc[0] += p[0];
                        acc[1] += p[1];
                        acc[2] += p[2];
                    } else if (format == QL_IMAGE_RGBA8888) {
                        acc[0] += p[0];
                        acc[1] += p[1];
                        acc[2] += p[2];
                        acc[3] += p[3];
                    } else {
                        acc[0] += p[0];
                    }
                }
            }
            const uint32_t d = (uint32_t)(factor * factor);
            uint8_t *o = out + ((size_t)y * nw + x) * bpp;
            if (format == QL_IMAGE_RGB565) {
                const uint16_t v = (uint16_t)(((acc[0] / d) << 11) |
                                              ((acc[1] / d) << 5) | (acc[2] / d));
                memcpy(o, &v, 2);
            } else if (format == QL_IMAGE_RGB888) {
                o[0] = (uint8_t)(acc[0] / d);
                o[1] = (uint8_t)(acc[1] / d);
                o[2] = (uint8_t)(acc[2] / d);
            } else if (format == QL_IMAGE_RGBA8888) {
                o[0] = (uint8_t)(acc[0] / d);
                o[1] = (uint8_t)(acc[1] / d);
                o[2] = (uint8_t)(acc[2] / d);
                o[3] = (uint8_t)(acc[3] / d);
            } else {
                o[0] = (uint8_t)(acc[0] / d);
            }
        }
    }
    *ow = (uint16_t)nw;
    *oh = (uint16_t)nh;
    *olen = (uint32_t)nlen;
    return out;
}

/*
 * 扫描 doc 里的 `<img src>`，按名字把文件解码成目标像素格式并拼进 Assets。
 * 任何一步失败都返回 0（调用方打印原因并中止构建）—— 构建期失败远好过运行时。
 */
static int collect_assets(const QLUIDoc *doc, const Args *a, Assets *out)
{
    memset(out, 0, sizeof(*out));
    out->scale = (a->scale > 0) ? a->scale : 1;
    const int pf = parse_pixel_format(a->pixel_format);
    if (pf < 0) {
        fprintf(stderr, "qltext: 未知 --pixel-format \"%s\"（rgb565/rgb888/rgba8888/a8）\n",
                a->pixel_format);
        return 0;
    }
    out->pixel_format = (uint8_t)pf;
    if (a->format != NULL && strcmp(a->format, "raw") != 0) {
        fprintf(stderr, "qltext: 目前只支持 --format raw（qoi 编码器待做）\n");
        return 0;
    }

    for (uint16_t i = 0; i < doc->node_count; i++) {
        if (doc->nodes[i].tag != QL_UI_TAG_IMG)
            continue;
        const uint16_t ti = doc->nodes[i].text;
        if (ti == 0xFFFFu)
            continue;
        const char *name = doc->text_pool + doc->text_offsets[ti];

        int dup = 0;
        for (int k = 0; k < out->count; k++)
            if (out->item[k].text_index == ti)
                dup = 1;
        if (dup)
            continue;                      /* 同一张图多处使用：只嵌一份 */
        if (out->count >= QL_ASSET_MAX) {
            fprintf(stderr, "qltext: 图片资源超过 %d 张（QL_ASSET_MAX）\n", QL_ASSET_MAX);
            return 0;
        }

        char path[QL_ASSET_PATH];
        snprintf(path, sizeof(path), "%s/%s", a->root, name);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            fprintf(stderr, "qltext: 找不到图片资源 \"%s\"（相对 --root 找的）\n", path);
            return 0;
        }
        fseek(f, 0, SEEK_END);
        const long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsz <= 0 || (size_t)fsz > QL_CFG_IMAGE_MAX_BYTES) {
            fclose(f);
            fprintf(stderr, "qltext: \"%s\" 大小非法或超过 QL_CFG_IMAGE_MAX_BYTES\n", path);
            return 0;
        }
        uint8_t *bytes = (uint8_t *)malloc((size_t)fsz);
        if (bytes == NULL || fread(bytes, 1, (size_t)fsz, f) != (size_t)fsz) {
            free(bytes);
            fclose(f);
            fprintf(stderr, "qltext: 读不了 \"%s\"\n", path);
            return 0;
        }
        fclose(f);

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = bytes;
        req.len = (size_t)fsz;
        req.src_format = QL_IMAGE_SRC_AUTO;      /* 按魔数判断，不看扩展名 */
        req.out_format = out->pixel_format;
        QLImage img;
        const int rc = ql_image_decode(&req, &img);
        free(bytes);
        if (rc != QL_OK) {
            fprintf(stderr,
                    "qltext: 解不了 \"%s\"（%s）：本构建的解码器只支持 %s；"
                    "JPEG/GIF 请先用其它工具转成 PNG/BMP/QOI\n",
                    path, (rc == QL_ERR_UNSUPPORTED) ? "格式不支持" : "数据非法",
                    qltext_decode_formats());
            return 0;
        }

        uint16_t ow = img.w, oh = img.h;
        uint32_t olen = img.len;
        uint8_t *scaled = NULL;
        if (out->scale > 1) {
            scaled = downscale(img.px, img.w, img.h, img.stride, img.format, out->scale,
                               &ow, &oh, &olen);
            if (scaled == NULL) {
                fprintf(stderr, "qltext: \"%s\" 无法按 %d 倍缩小（尺寸太小）\n", path,
                        out->scale);
                if (img.owned)
                    ql_mem_free((void *)img.px);
                return 0;
            }
        }

        if (out->data_len + olen > out->data_cap) {
            size_t cap = (out->data_cap == 0) ? 4096u : out->data_cap;
            while (cap < out->data_len + olen)
                cap *= 2u;
            uint8_t *nd = (uint8_t *)realloc(out->data, cap);
            if (nd == NULL) {
                free(scaled);
                if (img.owned)
                    ql_mem_free((void *)img.px);
                return 0;
            }
            out->data = nd;
            out->data_cap = cap;
        }

        Asset *as = &out->item[out->count++];
        memset(as, 0, sizeof(*as));
        as->text_index = ti;
        as->off = (uint32_t)out->data_len;
        as->len = olen;
        as->w = ow;
        as->h = oh;
        memcpy(out->data + out->data_len, scaled != NULL ? scaled : img.px, olen);
        out->data_len += olen;
        free(scaled);
        if (img.owned)
            ql_mem_free((void *)img.px);
    }
    return 1;
}

static void write_source(const QLUIDoc *doc, const char *ident, const char *out_dir,
                         const Assets *assets)
{
    FILE *f = open_out(out_dir, ident, ".c");
    size_t pool_len = ql_ui_doc_text_pool_size(doc);

    fprintf(f,
            "/* 本文件由 tools/qltext/qltext.c 生成，请勿手工修改。 */\n"
            "/* 源：%s */\n"
            "\n"
            "#include \"%s.h\"\n"
            "\n",
            doc->name != NULL ? doc->name : "", ident);

    /* ---- 节点 ---- */
    /* 注：doc 里不存 class/id，所以注释只给数组下标含义（旧 qlui.py 能写 <div#root>） */
    fprintf(f,
            "static const QLUINode %s_nodes[] = {\n"
            "    /* tag                     parent first  next   style  text   hover"
            "  active focus  disabled checked placeholder ph-text flags cspan rspan */\n",
            ident);
    for (uint16_t i = 0; i < doc->node_count; i++) {
        const QLUINode *n = &doc->nodes[i];
        fprintf(f, "    { %-22s, ",
                enum_name(TAG_NAMES, COUNT_OF(TAG_NAMES), n->tag, "QLUITag"));
        idx_ofs(f, n->parent, 1);
        idx_ofs(f, n->first_child, 0);
        idx_ofs(f, n->next_sibling, 0);
        idx_ofs(f, n->style, 0);
        idx_ofs(f, n->text, 0);
        idx_ofs(f, n->style_hover, 0);
        idx_ofs(f, n->style_active, 0);
        idx_ofs(f, n->style_focus, 0);
        idx_ofs(f, n->style_disabled, 0);
        idx_ofs(f, n->style_checked, 0);
        idx_ofs(f, n->style_placeholder, 0);
        idx_ofs(f, n->placeholder, 0);
        fprintf(f, ", 0x%02X, %u, %u },\n", (unsigned)n->flags, (unsigned)n->colspan,
                (unsigned)n->rowspan);
    }
    fputs("};\n\n", f);

    /* ---- 字号表 ---- */
    fprintf(f, "static const uint16_t %s_font_sizes[] = {", ident);
    for (uint16_t i = 0; i < doc->font_size_count; i++)
        fprintf(f, "%s%u", (i == 0) ? " " : ", ", (unsigned)doc->font_sizes[i]);
    fputs(doc->font_size_count > 0 ? " };\n\n" : " 0 };\n\n", f);

    /* ---- 样式表 ---- */
    fprintf(f, "static const QLUIStyle %s_styles[] = {\n", ident);
    for (uint16_t i = 0; i < doc->style_count; i++)
        fput_style(f, &doc->styles[i]);
    fputs("};\n\n", f);

    /* ---- 文本池 ---- */
    if (doc->text_count > 0 && doc->text_offsets != NULL) {
        fprintf(f, "static const char %s_text[] = \"", ident);
        fput_c_escaped(f, doc->text_pool, pool_len);
        fputs("\";\n", f);
        fprintf(f, "static const uint16_t %s_text_ofs[] = {", ident);
        for (uint16_t i = 0; i < doc->text_count; i++)
            fprintf(f, "%s%u", (i == 0) ? " " : ", ",
                    (unsigned)doc->text_offsets[i]);
        fputs(" };\n\n", f);
    } else {
        fprintf(f,
                "static const char %s_text[] = \"\";\n"
                "static const uint16_t %s_text_ofs[] = { 0 };\n\n",
                ident, ident);
    }

    /* ---- 关键帧 ---- */
    if (doc->keyframe_count > 0) {
        fprintf(f,
                "static const QLUIKeyframe %s_keyframes[] = {\n"
                "    /* offset  ease                  props bg_color bg_color2 text_c"
                " border_c  tf_x tf_y   scx  scy radius  clip alpha */\n",
                ident);
        for (uint16_t i = 0; i < doc->keyframe_count; i++) {
            const QLUIKeyframe *k = &doc->keyframes[i];
            const char *ease = (k->ease == QL_UI_KF_EASE_INHERIT)
                                   ? "QL_UI_KF_EASE_INHERIT"
                                   : ease_name(k->ease);
            fprintf(f,
                    "    { %4d, %-21s, 0x%02X, 0x%06X, 0x%06X, 0x%06X, 0x%06X,"
                    " %4d, %4d, %5d, %5d, %5d, %5d, %3d, 0,"
                    " { %5d, %5d, %5d, %5d }, %d, 0 },\n",
                    (int)k->offset, ease, (unsigned)k->props,
                    (unsigned)(k->bg_color & 0xFFFFFFu),
                    (unsigned)(k->bg_color2 & 0xFFFFFFu),
                    (unsigned)(k->text_color & 0xFFFFFFu),
                    (unsigned)(k->border_color & 0xFFFFFFu),
                    (int)k->tf_x, (int)k->tf_y, (int)k->sc_x, (int)k->sc_y,
                    (int)k->radius, (int)k->clip_r, (int)k->bg_alpha,
                    (int)k->bez[0], (int)k->bez[1], (int)k->bez[2], (int)k->bez[3],
                    (int)k->bez_on);
        }
        fputs("};\n\n", f);
    } else {
        fprintf(f,
                "static const QLUIKeyframe %s_keyframes[1] = {"
                " { 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 0, 0,"
                " { 0, 0, 0, 0 }, 0, 0 } };\n\n",
                ident);
    }

    /* ---- 动画 ---- */
    if (doc->anim_count > 0) {
        fprintf(f,
                "static const QLUIAnimation %s_anims[] = {\n"
                "    /* first count dur_ms ease        iterations direction"
                "                    fill                     delay */\n",
                ident);
        for (uint16_t i = 0; i < doc->anim_count; i++) {
            const QLUIAnimation *a = &doc->anims[i];
            fprintf(f,
                    "    { %5u, %5u, %6u, %-11s, %10u, %-26s, %-22s,"
                    " %5u, %3u, %d, { %5d, %5d, %5d, %5d }, 0 },\n",
                    (unsigned)a->first_frame, (unsigned)a->frame_count,
                    (unsigned)a->dur_ms, ease_name(a->ease),
                    (unsigned)a->iterations,
                    enum_name(ANIM_DIR_NAMES, COUNT_OF(ANIM_DIR_NAMES),
                              a->direction, "QLUIAnimDirection"),
                    enum_name(ANIM_FILL_NAMES, COUNT_OF(ANIM_FILL_NAMES), a->fill,
                              "QLUIAnimFill"),
                    (unsigned)a->delay_ms, (unsigned)a->steps, (int)a->bez_on,
                    (int)a->bez[0], (int)a->bez[1], (int)a->bez[2], (int)a->bez[3]);
        }
        fputs("};\n\n", f);
    } else {
        fprintf(f,
                "static const QLUIAnimation %s_anims[1] = {"
                " { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, { 0, 0, 0, 0 }, 0 } };\n\n",
                ident);
    }

    /* ---- 图片资源（只有 --root 才会走到这里） ---- */
    if (assets != NULL && assets->count > 0) {
        fprintf(f, "/* <img src> 的资源：构建期已解码并转成目标像素格式（RAW） */\n");
        fprintf(f, "static const uint8_t %s_img_data[%u] = {\n", ident,
                (unsigned)assets->data_len);
        for (size_t i = 0; i < assets->data_len; i++) {
            if (i % 16u == 0)
                fprintf(f, "    ");
            fprintf(f, "0x%02X,", (unsigned)assets->data[i]);
            if (i % 16u == 15u || i + 1u == assets->data_len)
                fprintf(f, "\n");
            else
                fprintf(f, " ");
        }
        fprintf(f, "};\n\n");
        fprintf(f, "static const QLUIImageSrc %s_images[%d] = {\n", ident,
                assets->count);
        for (int i = 0; i < assets->count; i++) {
            const Asset *as = &assets->item[i];
            fprintf(f,
                    "    { .name = %u, .off = %u, .len = %u, .w = %u, .h = %u, "
                    ".format = QL_IMAGE_SRC_RAW, .pixel_format = %s },\n",
                    (unsigned)as->text_index, (unsigned)as->off, (unsigned)as->len,
                    (unsigned)as->w, (unsigned)as->h,
                    pixel_format_name(assets->pixel_format));
        }
        fprintf(f, "};\n\n");
    }

    /* ---- 图像热区：<map>/<area> 在构建期就绑定到 <img> ---- */
    if (doc->area_count > 0) {
        fprintf(f, "/* <area> 热区：坐标是图片像素（原点 = 图片左上角） */\n");
        fprintf(f, "static const int16_t %s_area_coords[%u] = {", ident,
                (unsigned)doc->area_coord_count);
        for (uint16_t i = 0; i < doc->area_coord_count; i++) {
            if (i % 12u == 0)
                fprintf(f, "\n    ");
            fprintf(f, "%5d,", (int)doc->area_coords[i]);
        }
        fputs("\n};\n\n", f);

        fprintf(f, "static const QLUIArea %s_areas[%u] = {\n", ident,
                (unsigned)doc->area_count);
        for (uint16_t i = 0; i < doc->area_count; i++) {
            const QLUIArea *ar = &doc->areas[i];
            fprintf(f,
                    "    { .img = %u, .coord_off = %u, .href = %u, .alt = %u,"
                    " .shape = %s, .ncoord = %u },\n",
                    (unsigned)ar->img, (unsigned)ar->coord_off, (unsigned)ar->href,
                    (unsigned)ar->alt,
                    enum_name(AREA_SHAPE_NAMES, COUNT_OF(AREA_SHAPE_NAMES), ar->shape,
                              "QLUIAreaShape"),
                    (unsigned)ar->ncoord);
        }
        fputs("};\n\n", f);
    }

    /* ---- calc() 字节码池（有才输出） ---- */
    if (doc->calc_op_count > 0 && doc->calc_ops != NULL) {
        fprintf(f,
                "/* calc()/min()/max()/clamp() 的后缀字节码（unit 对 TERM 是单位，"
                "对 MIN/MAX/CLAMP 是参数个数） */\n");
        fprintf(f, "static const QLUICalcOp %s_calc_ops[%u] = {\n", ident,
                (unsigned)doc->calc_op_count);
        for (uint16_t i = 0; i < doc->calc_op_count; i++)
            fprintf(f, "    { %6d, %u, %3u },\n", (int)doc->calc_ops[i].value,
                    (unsigned)doc->calc_ops[i].op, (unsigned)doc->calc_ops[i].unit);
        fputs("};\n\n", f);
    }

    /* ---- Grid 三段池（有才输出） ---- */
    if (doc->grid_count > 0 && doc->grids != NULL) {
        fprintf(f, "/* grid 模板：col_first/row_first 是 %s_grid_tracks 的下标 */\n", ident);
        fprintf(f, "static const QLUIGrid %s_grids[%u] = {\n", ident,
                (unsigned)doc->grid_count);
        for (uint16_t i = 0; i < doc->grid_count; i++) {
            const QLUIGrid *gg = &doc->grids[i];
            fprintf(f, "    { %u, %u, %u, %u, %u, %u, %u, %u },\n",
                    (unsigned)gg->col_first, (unsigned)gg->col_count,
                    (unsigned)gg->row_first, (unsigned)gg->row_count,
                    (unsigned)gg->area_first, (unsigned)gg->area_count,
                    (unsigned)gg->col_hint, (unsigned)gg->row_hint);
        }
        fputs("};\n\n", f);
    }
    if (doc->grid_track_count > 0 && doc->grid_tracks != NULL) {
        fprintf(f, "/* grid 轨道：[min_kind, max_kind, min_val, max_val] */\n");
        fprintf(f, "static const QLUIGridTrack %s_grid_tracks[%u] = {\n", ident,
                (unsigned)doc->grid_track_count);
        for (uint16_t i = 0; i < doc->grid_track_count; i++) {
            const QLUIGridTrack *t = &doc->grid_tracks[i];
            fprintf(f, "    { %u, %u, %6d, %6d },\n", (unsigned)t->min_kind,
                    (unsigned)t->max_kind, (int)t->min_val, (int)t->max_val);
        }
        fputs("};\n\n", f);
    }
    if (doc->grid_area_count > 0 && doc->grid_areas != NULL) {
        fprintf(f, "/* grid 具名区域（行列号 1-based） */\n");
        fprintf(f, "static const QLUIGridArea %s_grid_areas[%u] = {\n", ident,
                (unsigned)doc->grid_area_count);
        for (uint16_t i = 0; i < doc->grid_area_count; i++) {
            const QLUIGridArea *a2 = &doc->grid_areas[i];
            fprintf(f, "    { %u, %d, %d, %u, %u },\n", (unsigned)a2->name,
                    (int)a2->col, (int)a2->row, (unsigned)a2->col_span,
                    (unsigned)a2->row_span);
        }
        fputs("};\n\n", f);
    }

    /* ---- transform 矩阵池（有才输出，M9.4） ---- */
    if (doc->xform_count > 0 && doc->xforms != NULL) {
        fprintf(f, "/* transform 矩阵（26.6 定点；a..d 无量纲、e/f 是 px） */\n");
        fprintf(f, "static const QLUIXform %s_xforms[%u] = {\n", ident,
                (unsigned)doc->xform_count);
        for (uint16_t i = 0; i < doc->xform_count; i++) {
            const QLUIXform *q = &doc->xforms[i];
            fprintf(f, "    { %d, %d, %d, %d, %d, %d },\n", (int)q->a, (int)q->b,
                    (int)q->c, (int)q->d, (int)q->e, (int)q->f);
        }
        fputs("};\n\n", f);
    }

    /* ---- clip-path 多边形顶点（有才输出，v34） ---- */
    if (doc->clip_pt_count > 0 && doc->clip_pts != NULL) {
        fprintf(f, "/* clip-path polygon 顶点（x 相对盒宽、y 相对盒高；千分比或 px） */\n");
        fprintf(f, "static const QLClipPt %s_clip_pts[%u] = {\n", ident,
                (unsigned)doc->clip_pt_count);
        for (uint16_t i = 0; i < doc->clip_pt_count; i++) {
            const QLClipPt *pt = &doc->clip_pts[i];
            fprintf(f, "    { { %d, %u, 0 }, { %d, %u, 0 } },\n", (int)pt->x.value,
                    (unsigned)pt->x.unit, (int)pt->y.value, (unsigned)pt->y.unit);
        }
        fputs("};\n\n", f);
    }

    /* ---- 文本栈（B8）：@font-face 与 font-family 列表（有才输出） ---- */
    if (doc->font_face_count > 0 && doc->font_faces != NULL) {
        fprintf(f, "/* @font-face 声明（family / src 都是文本池下标） */\n");
        fprintf(f, "static const QLUIFontFace %s_font_faces[%u] = {\n", ident,
                (unsigned)doc->font_face_count);
        for (uint16_t i = 0; i < doc->font_face_count; i++) {
            const QLUIFontFace *ff = &doc->font_faces[i];
            fprintf(f, "    { %u, %u, %u, %u },\n", (unsigned)ff->family,
                    (unsigned)ff->src, (unsigned)ff->weight, (unsigned)ff->italic);
        }
        fputs("};\n\n", f);
    }
    if (doc->font_list_count > 0 && doc->font_lists != NULL) {
        fprintf(f, "/* font-family 列表：names[first .. first+count) 是文本池下标 */\n");
        fprintf(f, "static const QLUIFontList %s_font_lists[%u] = {\n", ident,
                (unsigned)doc->font_list_count);
        for (uint16_t i = 0; i < doc->font_list_count; i++)
            fprintf(f, "    { %u, %u },\n", (unsigned)doc->font_lists[i].first,
                    (unsigned)doc->font_lists[i].count);
        fputs("};\n", f);
        if (doc->font_name_count > 0 && doc->font_names != NULL) {
            fprintf(f, "static const uint16_t %s_font_names[%u] = {", ident,
                    (unsigned)doc->font_name_count);
            for (uint16_t i = 0; i < doc->font_name_count; i++)
                fprintf(f, " %u,", (unsigned)doc->font_names[i]);
            fputs(" };\n", f);
        }
        fputs("\n", f);
    }

    /* ---- @media 断点（有才输出） ---- */
    if (doc->media_count > 0 && doc->media != NULL) {
        fprintf(f, "/* @media 断点区间（视口宽，闭区间；hi = 0xFFFF 表示无上界） */\n");
        fprintf(f, "static const QLUIMedia %s_media[%u] = {\n", ident,
                (unsigned)doc->media_count);
        for (uint16_t i = 0; i < doc->media_count; i++)
            fprintf(f, "    { %u, %u },\n", (unsigned)doc->media[i].lo,
                    (unsigned)doc->media[i].hi);
        fputs("};\n\n", f);

        /* 池的长度：最后一个有变体的节点的偏移 + 一个块（media_count*4） */
        uint32_t pool_n = 0;
        if (doc->node_media != NULL) {
            for (uint16_t i = 0; i < doc->node_count; i++)
                if (doc->node_media[i] != 0xFFFFu) {
                    const uint32_t end =
                        (uint32_t)doc->node_media[i] + (uint32_t)doc->media_count * 4u;
                    if (end > pool_n)
                        pool_n = end;
                }
        }
        fprintf(f, "static const uint16_t %s_node_media[%u] = {", ident,
                (unsigned)doc->node_count);
        for (uint16_t i = 0; i < doc->node_count; i++) {
            if (i % 12u == 0)
                fprintf(f, "\n    ");
            fprintf(f, "%5u,", (unsigned)doc->node_media[i]);
        }
        fputs("\n};\n\n", f);

        fprintf(f, "static const uint16_t %s_media_styles[%u] = {", ident,
                (unsigned)pool_n);
        for (uint32_t i = 0; i < pool_n; i++) {
            if (i % 12u == 0)
                fprintf(f, "\n    ");
            fprintf(f, "%5u,", (unsigned)doc->media_styles[i]);
        }
        fputs("\n};\n\n", f);
    }

    /* ---- doc ---- */
    fprintf(f,
            "const QLUIDoc %s_doc = {\n"
            "    .name         = \"%s\",\n"
            "    .node_count   = (uint16_t)(sizeof(%s_nodes) / sizeof(%s_nodes[0])),\n"
            "    .style_count  = (uint16_t)(sizeof(%s_styles) / sizeof(%s_styles[0])),\n"
            "    .text_count   = (uint16_t)(sizeof(%s_text_ofs) / sizeof(%s_text_ofs[0])),\n"
            "    .font_size_count = (uint16_t)(sizeof(%s_font_sizes) / sizeof(%s_font_sizes[0])),\n"
            "    .anim_count   = %u,\n"
            "    .keyframe_count = %u,\n"
            "    .font_sizes   = %s_font_sizes,\n"
            "    .anims        = %s_anims,\n"
            "    .keyframes    = %s_keyframes,\n"
            "    .nodes        = %s_nodes,\n"
            "    .styles       = %s_styles,\n"
            "    .text_pool    = %s_text,\n"
            "    .text_offsets = %s_text_ofs,\n",
            ident, doc->name != NULL ? doc->name : "",
            ident, ident,   /* .node_count  : sizeof(%s_nodes) x2 */
            ident, ident,   /* .style_count : sizeof(%s_styles) x2 */
            ident, ident,   /* .text_count  : sizeof(%s_text_ofs) x2 */
            ident, ident,   /* font_size_cnt: sizeof(%s_font_sizes) x2 */
            (unsigned)doc->anim_count, (unsigned)doc->keyframe_count,
            ident, ident, ident, ident, ident, ident, ident);

    /* @media 三段：没有断点时一个字都不多 */
    if (doc->media_count > 0 && doc->media != NULL) {
        fprintf(f,
                "    .media_count   = %u,\n"
                "    .media         = %s_media,\n"
                "    .node_media    = %s_node_media,\n"
                "    .media_styles  = %s_media_styles,\n",
                (unsigned)doc->media_count, ident, ident, ident);
    }

    /* transform 矩阵池：没有就一个字都不多 */
    if (doc->xform_count > 0 && doc->xforms != NULL) {
        fprintf(f,
                "    .xform_count = %u,\n"
                "    .xforms      = %s_xforms,\n",
                (unsigned)doc->xform_count, ident);
    }

    if (doc->clip_pt_count > 0 && doc->clip_pts != NULL) {
        fprintf(f,
                "    .clip_pt_count = %u,\n"
                "    .clip_pts      = %s_clip_pts,\n",
                (unsigned)doc->clip_pt_count, ident);
    }

    /* 文本栈：没有 @font-face / 家族列表时一个字都不多 */
    if (doc->font_face_count > 0 && doc->font_faces != NULL) {
        fprintf(f,
                "    .font_face_count  = %u,\n"
                "    .font_faces       = %s_font_faces,\n",
                (unsigned)doc->font_face_count, ident);
    }
    if (doc->font_list_count > 0 && doc->font_lists != NULL) {
        fprintf(f,
                "    .font_list_count  = %u,\n"
                "    .font_name_count  = %u,\n"
                "    .font_lists       = %s_font_lists,\n"
                "    .font_names       = %s_font_names,\n",
                (unsigned)doc->font_list_count, (unsigned)doc->font_name_count, ident,
                ident);
    }

    /* Grid 三段池：没有 grid 时一个字都不多 */
    if (doc->grid_count > 0 && doc->grids != NULL) {
        fprintf(f,
                "    .grid_count       = %u,\n"
                "    .grid_track_count = %u,\n"
                "    .grid_area_count  = %u,\n"
                "    .grids            = %s_grids,\n"
                "    .grid_tracks      = %s_grid_tracks,\n"
                "    .grid_areas       = %s_grid_areas,\n",
                (unsigned)doc->grid_count, (unsigned)doc->grid_track_count,
                (unsigned)doc->grid_area_count, ident, ident, ident);
    }

    /* calc() 池：没用 calc 时一个字都不多 */
    if (doc->calc_op_count > 0 && doc->calc_ops != NULL) {
        fprintf(f,
                "    .calc_op_count = %u,\n"
                "    .calc_ops      = %s_calc_ops,\n",
                (unsigned)doc->calc_op_count, ident);
    }

    /* 热区接在 doc 字面量里：没有 <area> 时一个字都不多 */
    if (doc->area_count > 0) {
        fprintf(f,
                "    .area_count    = %u,\n"
                "    .area_coord_count = %u,\n"
                "    .areas         = %s_areas,\n"
                "    .area_coords   = %s_area_coords,\n",
                (unsigned)doc->area_count, (unsigned)doc->area_coord_count, ident, ident);
    }

    /* 图片资源接在 doc 字面量里：没有资源时一个字都不多 */
    if (assets != NULL && assets->count > 0) {
        fprintf(f,
                "    .image_count   = %d,\n"
                "    .image_data_size = %u,\n"
                "    .images        = %s_images,\n"
                "    .image_data    = %s_img_data,\n",
                assets->count, (unsigned)assets->data_len, ident, ident);
    }
    fprintf(f, "};\n");
    fclose(f);
}

static void write_header(const char *ident, const char *out_dir)
{
    FILE *f = open_out(out_dir, ident, ".h");

    char guard[256];
    size_t gi = 0;
    for (const char *p = ident; *p != '\0' && gi + 16u < sizeof(guard); p++)
        guard[gi++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 'a' + 'A' : *p);
    guard[gi] = '\0';

    fprintf(f,
            "/* 本文件由 tools/qltext/qltext.c 生成，请勿手工修改。 */\n"
            "#ifndef QLUI_%s_H\n"
            "#define QLUI_%s_H\n"
            "\n"
            "#include <qlgl/qlgl.h>\n"
            "\n"
            "extern const QLUIDoc %s_doc;\n"
            "\n"
            "#endif /* QLUI_%s_H */\n",
            guard, guard, ident, guard);
    fclose(f);
}

/*
 * 给宿主工具 qlblob 用的访问器。
 * blob 的序列化格式只在 C 侧实现一次（src/ui/qlui_blob.c），这里只负责
 * "把生成的 doc 交给工具"，不重复实现二进制布局。
 */
static void write_blob_helper(const char *ident, const char *out_dir)
{
    FILE *f = open_out(out_dir, ident, "_blob.c");
    fprintf(f,
            "/* 本文件由 tools/qltext/qltext.c 生成，请勿手工修改。 */\n"
            "/* 仅供宿主工具 tools/qlblob 使用，不参与目标机构建。 */\n"
            "#include \"%s.h\"\n"
            "\n"
            "const QLUIDoc *qlui_blob_doc(void);\n"
            "\n"
            "const QLUIDoc *qlui_blob_doc(void)\n"
            "{\n"
            "    return &%s_doc;\n"
            "}\n",
            ident, ident);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* 命令行                                                              */
/* ------------------------------------------------------------------ */

/* ---------------------- --platform：构建期降级报告 ---------------------- */

/*
 * 同一份 HTML/CSS 在不同目标平台上的能力集不同（PC/移动全开，MCU 裁掉大件）。
 * 被裁掉的部分在本库里**构建期报错**，所以"能不能在目标上编过"必须提前知道 ——
 * 这就是 `qltext check --platform mcu` 的用途：不产出任何文件，只报告
 * "这份界面用到了哪些在目标平台上会报错/不生效的东西"，并给出对应的开关名。
 *
 * 代价 = 0：这张表只在宿主工具里（目标机根本不链接 qltext）。
 */
typedef struct {
    const char *label;       /* 人话描述 */
    const char *macro;       /* 哪个 QL_FEAT_CSS_*（=0 时不可用） */
    int (*hit)(const QLUIStyle *s);
} PlatformProbe;

static int hit_grid(const QLUIStyle *s)
{
    return s->grid != QL_UI_NO_GRID || s->display == QL_UI_DISPLAY_GRID ||
           s->display == QL_UI_DISPLAY_INLINE_GRID;
}
static int hit_border_image(const QLUIStyle *s)
{
    return s->border_image != QL_UI_NO_IMAGE;
}
static int hit_table(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_TABLE || s->display == QL_UI_DISPLAY_TABLE_ROW ||
           s->display == QL_UI_DISPLAY_TABLE_CELL ||
           s->display == QL_UI_DISPLAY_TABLE_ROW_GROUP ||
           s->display == QL_UI_DISPLAY_TABLE_CAPTION ||
           s->display == QL_UI_DISPLAY_TABLE_COLUMN ||
           s->display == QL_UI_DISPLAY_TABLE_COLUMN_GROUP ||
           s->table_layout != 0 || s->border_collapse != 0;
}
static int hit_columns(const QLUIStyle *s)
{
    return s->column_count != 0 || s->column_width.unit != QL_UI_UNIT_AUTO ||
           s->column_rule_w != 0;
}
static int hit_filter_layer(const QLUIStyle *s)
{
    return s->filter_n > 0 || s->mix_blend != 0 || s->mask_image != QL_UI_NO_MASK ||
           s->xform >= 0 || s->backdrop_blur != 0;
}
static int hit_font_stack(const QLUIStyle *s)
{
    /* 家族列表下标 0 是"默认（空）"，> 0 说明真的写了 font-family: 名字 */
    return s->font_family > 1;
}
static int hit_text_shape(const QLUIStyle *s)
{
    return s->ligatures == 0 || s->kerning == 0 || s->variant_flags != 0;
}
static int hit_bidi(const QLUIStyle *s)
{
    return s->direction != 0 || s->unicode_bidi != 0;
}
static int hit_clip_shape(const QLUIStyle *s)
{
    return s->clip_kind == QL_UI_CLIP_POLYGON || s->clip_kind == QL_UI_CLIP_ELLIPSE ||
           s->clip_kind == QL_UI_CLIP_INSET;
}

static const PlatformProbe k_probes[] = {
    { "Grid 布局 / border-image 九宫格", "QL_FEAT_CSS_GRID", hit_grid },
    { "border-image", "QL_FEAT_CSS_BORDER_IMAGE", hit_border_image },
    { "表格（display: table 全家）", "QL_FEAT_CSS_TABLE", hit_table },
    { "多栏（column-*）", "QL_FEAT_CSS_COLUMNS", hit_columns },
    { "滤镜 / 混合模式 / mask / 旋转（离屏图层）", "QL_FEAT_CSS_FILTER_LAYER",
      hit_filter_layer },
    { "@font-face / font-family 家族列表", "QL_FEAT_CSS_FONT_STACK", hit_font_stack },
    { "连字/字距/small-caps/tabular-nums（整形）", "QL_FEAT_CSS_TEXT_SHAPE",
      hit_text_shape },
    { "direction / RTL（双向）", "QL_FEAT_CSS_BIDI", hit_bidi },
    { "clip-path 的 ellipse/inset/polygon", "QL_FEAT_CSS_FILTER_LAYER", hit_clip_shape },
};

/*
 * 平台预设：**只列"关掉"的开关**，其余按开处理。
 * 数值与 test/mcu/build.sh 的默认值、以及文档里各能力包的默认值保持一致。
 */
static int platform_has(const char *platform, const char *macro)
{
    if (platform == NULL)
        return 1;
    if (strcmp(platform, "pc") == 0 || strcmp(platform, "mobile") == 0)
        return 1;                      /* PC/移动：全开 */
    if (strcmp(platform, "mcu") == 0) {
        /* MCU 默认裁掉所有"大件"（见 test/mcu/build.sh） */
        return !(strcmp(macro, "QL_FEAT_CSS_FILTER_LAYER") == 0 ||
                 strcmp(macro, "QL_FEAT_CSS_TABLE") == 0 ||
                 strcmp(macro, "QL_FEAT_CSS_COLUMNS") == 0 ||
                 strcmp(macro, "QL_FEAT_CSS_FONT_STACK") == 0 ||
                 strcmp(macro, "QL_FEAT_CSS_TEXT_SHAPE") == 0 ||
                 strcmp(macro, "QL_FEAT_CSS_BIDI") == 0);
    }
    return 1;
}

/* 返回"发现的降级项个数"；0 = 目标平台完全支持这份界面 */
static int report_platform(const QLUIStyle *styles, uint16_t n, const char *platform)
{
    int total = 0;
    printf("qltext: --platform %s 降级报告\n", platform);
    for (size_t i = 0; i < sizeof(k_probes) / sizeof(k_probes[0]); i++) {
        const PlatformProbe *p = &k_probes[i];
        if (platform_has(platform, p->macro))
            continue;                  /* 目标上有这个能力，不用报 */
        int hits = 0;
        for (uint16_t k = 0; k < n; k++)
            if (p->hit(&styles[k]))
                hits++;
        if (hits == 0)
            continue;
        total += hits;
        printf("  [裁掉] %-42s %3d 处 → %s=0 时构建期报错\n", p->label, hits,
               p->macro);
    }
    if (total == 0)
        printf("  ✔ 这份界面在 %s 上不需要裁任何特性\n", platform);
    return total;
}

static void usage(void)
{
    fprintf(stderr,
            "用法：\n"
            "  qltext cc    --html A.html --css B.css --name foo --out DIR\n"
            "               [--root DIR] [--pixel-format rgb565|rgb888|rgba8888|a8]\n"
            "               [--scale N]   构建期把 <img src> 的图片转成目标像素格式并嵌入\n"
            "  qltext blob  --html A.html --css B.css --out out.qlui [--name foo] [--root DIR]\n"
            "  qltext check --html A.html --css B.css [--name foo] [--platform pc|mcu]\n"
            "      --platform 额外给出“构建期降级报告”（目标平台上有哪些特性会被裁掉）\n");
}


static int parse_args(int argc, char **argv, Args *a)
{
    memset(a, 0, sizeof(*a));
    for (int i = 2; i < argc; i++) {
        const char *k = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (v == NULL) {
            fprintf(stderr, "qltext: %s 缺少参数\n", k);
            return 0;
        }
        if (strcmp(k, "--html") == 0)      { a->html = v; i++; }
        else if (strcmp(k, "--css") == 0)  { a->css = v; i++; }
        else if (strcmp(k, "--name") == 0) { a->name = v; i++; }
        else if (strcmp(k, "--out") == 0)  { a->out = v; i++; }
        else if (strcmp(k, "--root") == 0) { a->root = v; i++; }
        else if (strcmp(k, "--pixel-format") == 0) { a->pixel_format = v; i++; }
        else if (strcmp(k, "--format") == 0) { a->format = v; i++; }
        else if (strcmp(k, "--scale") == 0) { a->scale = atoi(v); i++; }
        else if (strcmp(k, "--platform") == 0) { a->platform = v; i++; }
        else {
            fprintf(stderr, "qltext: 未知参数 %s\n", k);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *mode = argv[1];
    if (strcmp(mode, "-h") == 0 || strcmp(mode, "--help") == 0 ||
        strcmp(mode, "help") == 0) {
        usage();
        return 0;
    }
    int is_cc = (strcmp(mode, "cc") == 0);
    int is_blob = (strcmp(mode, "blob") == 0);
    int is_check = (strcmp(mode, "check") == 0);
    if (!is_cc && !is_blob && !is_check) {
        fprintf(stderr, "qltext: 未知子命令 \"%s\"\n", mode);
        usage();
        return 2;
    }

    /*
     * 宿主工具跑在开发机上，所以这里**直接装 libjpeg 后端**：构建期能吃 JPEG，
     * 产物仍是运行时零解码的 RAW。没有 libjpeg 的构建里这行是空操作，
     * 于是 "qltext: 解不了 …（格式不支持）" 会明确告诉你去转格式。
     */
#if QL_FEAT_IMG_LIBJPEG
    ql_image_set_platform_backend(ql_image_backend_libjpeg());
#endif

    Args a;
    if (!parse_args(argc, argv, &a)) return 2;
    if (a.html == NULL || a.css == NULL) {
        fprintf(stderr, "qltext: 必须同时给 --html 与 --css\n");
        return 2;
    }
    if ((is_cc || is_blob) && a.out == NULL) {
        fprintf(stderr, "qltext: %s 需要 --out\n", mode);
        return 2;
    }

    char *html = read_file(a.html);
    char *css = read_file(a.css);
    if (html == NULL || css == NULL) {
        free(html);
        free(css);
        return 1;
    }

    /* 默认名字取 html 的文件名（不含扩展名），与旧 qlui.py 一致 */
    char name_buf[256];
    const char *raw_name = a.name;
    if (raw_name == NULL) {
        const char *b = base_name(a.html);
        size_t n = 0;
        while (b[n] != '\0' && b[n] != '.' && n + 1u < sizeof(name_buf)) {
            name_buf[n] = b[n];
            n++;
        }
        name_buf[n] = '\0';
        raw_name = name_buf;
    }

    /* doc->name 用"净化后、不带 ui_ 前缀"的名字（blob 逐字节比对要求） */
    char sname[200];
    char ident[256];
    sanitize_ident(raw_name, sname, sizeof(sname));
    if (strncmp(sname, "ui_", 3) == 0) {
        memcpy(ident, sname, strlen(sname) + 1u);
    } else {
        snprintf(ident, sizeof(ident), "ui_%s", sname);
    }
    ql_ui_text_set_name(sname);

    QLUIDoc *doc = ql_ui_load_text(html, css);
    free(html);
    free(css);
    if (doc == NULL) {
        fprintf(stderr, "qltext: %s + %s 解析失败：%s\n", a.html, a.css,
                ql_ui_text_last_error());
        ql_ui_text_set_name(NULL);
        return 1;
    }

    int rc = 0;
    size_t pool = ql_ui_doc_text_pool_size(doc);

    /* --root：把 <img src> 指向的图片在构建期解码，按目标像素格式嵌入 */
    Assets assets;
    memset(&assets, 0, sizeof(assets));
    if (a.root != NULL) {
        if (!collect_assets(doc, &a, &assets)) {
            free(assets.data);
            ql_ui_doc_free(doc);
            ql_ui_text_set_name(NULL);
            return 1;
        }
        if (assets.count > 0)
            printf("qltext: 嵌入 %d 张图片（%u 字节，%s）\n", assets.count,
                   (unsigned)assets.data_len,
                   pixel_format_name(assets.pixel_format) + 9 /* 跳过 "QL_IMAGE_" */);
    }

    if (is_check) {
        printf("qltext: %s 可以编译（%u 节点，%u 样式，%zu 字节文本）\n", sname,
               (unsigned)doc->node_count, (unsigned)doc->style_count, pool);
        if (a.platform != NULL) {
            const int bad = report_platform(doc->styles, doc->style_count, a.platform);
            if (bad > 0) {
                /*
                 * 有降级项时以退出码 3 结束：CI 可以用它做"这份界面能不能上 MCU"
                 * 的门槛，而 0/1/2 仍然分别表示成功/解析失败/参数错误。
                 */
                ql_ui_doc_free(doc);
                ql_ui_text_set_name(NULL);
                return 3;
            }
        }
    } else if (is_blob) {
        /*
         * blob 也要带上资源：把 doc 拷一份接上 images（QLUIDoc 是纯数据，
         * 拷结构体是安全的），编码器会把资源段一起写出。
         */
        QLUIDoc withdoc = *doc;
        QLUIImageSrc srcs[QL_ASSET_MAX];
        if (assets.count > 0) {
            for (int k = 0; k < assets.count; k++) {
                memset(&srcs[k], 0, sizeof(srcs[k]));
                srcs[k].name = assets.item[k].text_index;
                srcs[k].off = assets.item[k].off;
                srcs[k].len = assets.item[k].len;
                srcs[k].w = assets.item[k].w;
                srcs[k].h = assets.item[k].h;
                srcs[k].format = QL_IMAGE_SRC_RAW;
                srcs[k].pixel_format = assets.pixel_format;
            }
            withdoc.images = srcs;
            withdoc.image_count = (uint16_t)assets.count;
            withdoc.image_data = assets.data;
            withdoc.image_data_size = (uint32_t)assets.data_len;
        }
        if (ql_ui_doc_save(&withdoc, a.out) != QL_OK) {
            fprintf(stderr, "qltext: 写不了 \"%s\"\n", a.out);
            rc = 1;
        } else {
            printf("qltext: %s -> %s（%u 节点，%u 样式，%zu 字节文本）\n", sname,
                   a.out, (unsigned)doc->node_count, (unsigned)doc->style_count,
                   pool);
        }
    } else {
        ensure_dir(a.out);
        write_source(doc, ident, a.out, &assets);
        write_header(ident, a.out);
        write_blob_helper(ident, a.out);
        printf("qltext: %s -> %s/%s.c（%u 节点，%u 样式，%zu 字节文本）\n", sname,
               a.out, ident, (unsigned)doc->node_count,
               (unsigned)doc->style_count, pool);
    }

    ql_ui_doc_free(doc);
    free(assets.data);
    ql_ui_text_set_name(NULL);
    return rc;
}
