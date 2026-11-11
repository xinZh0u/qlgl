/**
 * @file    qldraw2d.c
 * @brief   绘制接口的公共排版逻辑 + CPU（QLCanvas）后端适配。
 */
#include <string.h>

#include <qlgl/qlgl.h>

#include "qllayer.h"

void ql_draw2d_draw_line(QLDraw2D *self, ql_coord_t x1, ql_coord_t y1, ql_coord_t x2,
                         ql_coord_t y2, ql_coord_t thickness, ql_color_t color)
{
    if (self == NULL || self->fill_rect == NULL)
        return;

    /* 与 ql_canvas_draw_line 完全相同的 Bresenham 参数，步进里只调用 fill_rect */
    ql_coord_t dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    ql_coord_t dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    ql_coord_t sx = (x1 < x2) ? 1 : -1;
    ql_coord_t sy = (y1 < y2) ? 1 : -1;
    ql_coord_t err = dx - dy;
    ql_coord_t t = (thickness > 0) ? thickness : 1;
    ql_coord_t half = t / 2;

    for (;;) {
        self->fill_rect(self, ql_rect_make(x1 - half, y1 - half, t, t), color, 255);

        if (x1 == x2 && y1 == y2)
            break;

        ql_coord_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/* ------------------------------ 折行 ------------------------------ */

/* 是否 CJK：U+2E80 以上的表意/假名/谚文等，浏览器允许在它们之间任意断行 */
static int is_cjk(uint32_t cp)
{
    return cp >= 0x2E80u;
}

static int is_space_cp(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\r';
}

/*
 * 从 [pos, end) 里挑一个断点，返回**下一行的起点**（等于 end 表示这一行吃到最后）。
 * max_w <= 0 表示不折行。
 */
static int32_t wrap_next(const QLFont *font, const char *t, int32_t pos, int32_t end,
                         ql_coord_t max_w, const QLTextWrapOpt *opt)
{
    if (max_w <= 0)
        return end;

    const ql_coord_t ls = (opt != NULL) ? opt->letter_spacing : 0;
    const int break_any = (opt != NULL) ? (opt->break_any != 0) : 0;

    ql_coord_t w = 0;
    int32_t last_break = -1;      /* 候选断点（空格之后 / CJK 边界） */
    int32_t i = pos;

    while (i < end) {
        uint32_t cp = 0;
        int32_t used = ql_utf8_decode(t + i, &cp);
        if (used <= 0)
            break;

        const QLGlyph *g = ql_font_find_glyph(font, cp);
        const ql_coord_t adv = (g != NULL) ? g->adv_w : 0;

        /*
         * 宽度里**必须算上空格**：不然后面接的词会被当成"还放得下"，
         * 一行实际宽 120px 却以为只有 90px（这个 bug 第一次实现时就有）。
         * 行尾空格在断行后由 trim_line_end() 去掉，不影响视觉。
         */
        if (w + adv + ls > max_w && i > pos) {
            if (last_break > pos)
                return last_break;
            /* 没有可用的软断点：硬断在当前字符前（超长单词/长串数字） */
            return i;
        }
        w += adv + ls;

        if (is_space_cp(cp)) {
            /* 空格之后是断点；跳过连续空白，下一行不以前导空格开头 */
            int32_t j = i + used;
            while (j < end) {
                uint32_t c2 = 0;
                int32_t u2 = ql_utf8_decode(t + j, &c2);
                if (u2 <= 0 || !is_space_cp(c2))
                    break;
                j += u2;
            }
            last_break = j;
        } else if ((break_any || is_cjk(cp)) && i + used < end) {
            last_break = i + used;   /* CJK 字之间（或 break-all 时任意字符之间）可以断 */
        }

        i += used;
    }
    return end;
}

/* 去掉行尾空白（'\n' 之前、折行点之前的尾随空格不该占宽度） */
static int32_t trim_line_end(const char *t, int32_t start, int32_t end)
{
    while (end > start) {
        /* 只看 ASCII 空白与常见 Unicode 空白 */
        unsigned char c = (unsigned char)t[end - 1];
        if (c == ' ' || c == '\t' || c == '\r')
            end--;
        else
            break;
    }
    return end;
}

int ql_text_wrap(const QLFont *font, const char *text, ql_coord_t max_w,
                 QLTextLine *out, int max_lines)
{
    return ql_text_wrap_ex(font, text, max_w, out, max_lines, 0);
}

int ql_text_wrap_opt(const QLFont *font, const char *text, ql_coord_t max_w,
                     const QLTextWrapOpt *opt, QLTextLine *out, int max_lines,
                     int keep_trailing)
{
    if (font == NULL || text == NULL)
        return 0;

    const int32_t total = (int32_t)strlen(text);
    const ql_coord_t indent = (opt != NULL) ? opt->indent : 0;
    int32_t pos = 0;
    int n = 0;

    for (;;) {
        /* 当前显式行（到 '\n' 或结尾） */
        int32_t hard = pos;
        while (hard < total && text[hard] != '\n')
            hard++;

        /* 这一行要拆成几个折行 */
        int32_t p = pos;
        for (;;) {
            /*
             * 首行缩进只作用在**整段的第一行**（n == 0）：可用的宽度少一截。
             * 缩进本身由绘制那边把笔位右移，两边必须用同一个判据。
             */
            ql_coord_t avail = (n == 0 && indent > 0) ? (max_w - indent) : max_w;
            /* 缩进比盒子还宽时第一行至少留一个字符的位置（与浏览器一致） */
            if (max_w > 0 && avail < 1)
                avail = 1;
            int32_t next = wrap_next(font, text, p, hard, avail, opt);
            /*
             * 软折行点上的空格不占宽（被断行消费掉了）；但一行的最后一段
             * 在 `keep_trailing` 时保留尾随空格 —— 输入框靠它把光标顶过去。
             */
            int32_t line_end = (!keep_trailing || next < hard)
                                   ? trim_line_end(text, p, next)
                                   : next;
            if (out != NULL && n < max_lines) {
                out[n].start = text + p;
                out[n].len = line_end - p;
            }
            n++;
            if (next >= hard)
                break;
            p = next;
        }

        if (hard >= total)
            break;
        pos = hard + 1;               /* 跳过 '\n' */
        if (pos > total)
            break;
    }
    return n;
}

int ql_text_wrap_ex(const QLFont *font, const char *text, ql_coord_t max_w,
                    QLTextLine *out, int max_lines, int keep_trailing)
{
    return ql_text_wrap_opt(font, text, max_w, NULL, out, max_lines, keep_trailing);
}

/* 省略号用 U+2026；字体里没有（内置位图字体就没有）时退回三个点 */
#define QL_ELLIPSIS_CP 0x2026u

static int ellipsis_is_dots(const QLFont *font)
{
    return ql_font_find_glyph(font, QL_ELLIPSIS_CP) == NULL;
}

/*
 * 画一条装饰线的各种样式。
 *
 * 全部用 `fill_rect` 拼出来（不引入新的后端入口），所以 CPU / GL 天然一致：
 *   solid  : 一条 th 高的矩形
 *   double : 两条 th 高、间隔 th
 *   dotted : th 见方的点，间隔 th
 *   dashed : 段 3th、缝 2th（与 border-style 的图案比例一致）
 */
static void deco_line(QLDraw2D *self, ql_coord_t x, ql_coord_t y, ql_coord_t w,
                      ql_coord_t th, uint8_t style, ql_color_t color, uint8_t alpha)
{
    if (w <= 0 || th <= 0)
        return;
    if (style == 1) {          /* double */
        const ql_coord_t t2 = (th > 1) ? (th / 2) : 1;
        self->fill_rect(self, ql_rect_make(x, y, w, t2), color, alpha);
        self->fill_rect(self, ql_rect_make(x, y + t2 + t2, w, t2), color, alpha);
        return;
    }
    if (style == 2 || style == 3) {   /* dotted / dashed */
        const ql_coord_t dash = (style == 2) ? th : 3 * th;
        const ql_coord_t gap = (style == 2) ? th : 2 * th;
        for (ql_coord_t p = 0; p < w; p += dash + gap) {
            ql_coord_t d = w - p;
            if (d > dash)
                d = dash;
            self->fill_rect(self, ql_rect_make(x + p, y, d, th), color, alpha);
        }
        return;
    }
    self->fill_rect(self, ql_rect_make(x, y, w, th), color, alpha);
}

static ql_coord_t ellipsis_width(const QLFont *font, ql_coord_t letter_spacing)
{
    if (ellipsis_is_dots(font)) {
        const QLGlyph *g = ql_font_find_glyph(font, '.');
        const ql_coord_t adv = (g != NULL) ? g->adv_w : 0;
        return 3 * (adv + letter_spacing);
    }
    const QLGlyph *g = ql_font_find_glyph(font, QL_ELLIPSIS_CP);
    return ((g != NULL) ? g->adv_w : 0) + letter_spacing;
}

static void emit_ellipsis(QLDraw2D *self, const QLFont *font, ql_coord_t *pen_x,
                          ql_coord_t baseline, const QLTextStyle *ts,
                          ql_draw2d_glyph_fn emit_glyph)
{
    if (ellipsis_is_dots(font)) {
        const QLGlyph *g = ql_font_find_glyph(font, '.');
        if (g == NULL)
            return;
        for (int i = 0; i < 3; i++) {
            if (g->box_w > 0)
                emit_glyph(self, font, g, *pen_x, baseline, ts);
            *pen_x += g->adv_w + ts->letter_spacing;
        }
        return;
    }
    const QLGlyph *g = ql_font_find_glyph(font, QL_ELLIPSIS_CP);
    if (g != NULL && g->box_w > 0)
        emit_glyph(self, font, g, *pen_x, baseline, ts);
    if (g != NULL)
        *pen_x += g->adv_w + ts->letter_spacing;
}

void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                           QLRect box, const QLTextStyle *ts,
                           ql_draw2d_glyph_fn emit_glyph)
{
    if (self == NULL || font == NULL || text == NULL || ts == NULL ||
        emit_glyph == NULL)
        return;
    if (ql_rect_is_empty(box))
        return;

    /*
     * 行高与半行距（CSS inline 布局）：行盒高 L，字形区 ascent + descent，
     * 多出来的部分上下各分一半，于是第一行基线 = box.y + 半行距 + ascent。
     * L == ascent + descent 时半行距为 0 —— 这就是 `line-height: normal`
     * 以及旧版本的行为，所以默认路径逐像素不变。
     */
    const ql_coord_t lead_total = (ql_coord_t)(font->ascent + font->descent);
    ql_coord_t line_step = font->line_height;
    ql_coord_t half_lead = 0;
    if (ts->line_h > 0) {
        line_step = ts->line_h;
        half_lead = (ql_coord_t)(ts->line_h - lead_total) / 2;
    }

    /* `len < 0` 时按 NUL 结尾；有显式长度时截一段临时缓冲交给折行函数 */
    char buf[256];
    const char *src = text;
    if (len >= 0) {
        int32_t n = (len < (int32_t)sizeof(buf)) ? len : (int32_t)sizeof(buf) - 1;
        memcpy(buf, text, (size_t)n);
        buf[n] = '\0';
        src = buf;
    }

    QLTextWrapOpt opt;
    opt.letter_spacing = ts->letter_spacing;
    opt.word_spacing = ts->word_spacing;
    opt.indent = ts->indent;
    opt.break_any = ts->break_any;
    opt.tab_size = ts->tab_size;

    QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
    const int max_lines = (int)(sizeof(lines) / sizeof(lines[0]));
    int count = ql_text_wrap_opt(font, src, ts->wrap ? box.w : 0, &opt, lines,
                                 max_lines, ts->keep_trailing);
    if (count > max_lines)
        count = max_lines;      /* 超出的行不画（布局统计不受此限） */

    /* `baseline_dy` 只挪基线，不影响行盒（与浏览器一样：行高不会因上标而变） */
    ql_coord_t baseline = box.y + half_lead + font->ascent + ts->baseline_dy;
    for (int li = 0; li < count; li++) {
        /*
         * 双向与整形都在 `ql_text_shape()` 里（B8）：它按"字体 + 方向"切段，
         * 段落 RTL 时把各段按从右往左摆，返回的已经是**视觉顺序**的字形序列。
         * 所以这里照旧从左往右累加笔位即可，不需要自己重排文本。
         */
        const char *lt = lines[li].start;
        int32_t llen = lines[li].len;
        /* 首行缩进：只影响第一行的起点（折行那边同时按缩进减了可用宽度） */
        const ql_coord_t x_indent = (li == 0) ? ts->indent : 0;
        ql_coord_t line_w = ql_font_line_width_ls2(font, lt, llen, ts->letter_spacing,
                                                   ts->word_spacing, ts->tab_size);

        /*
         * `text-overflow: ellipsis`：一行比盒子宽时，末尾一个省略号。
         * 只在**明确要省略**且盒子有宽度时做，否则维持"溢出被裁掉"的老行为。
         */
        int draw_ellipsis = 0;
        if (ts->ellipsis && box.w > 0 && line_w > box.w - x_indent) {
            const ql_coord_t ell_w = ellipsis_width(font, ts->letter_spacing);
            const ql_coord_t cap = box.w - x_indent - ell_w;
            int32_t k = 0;
            ql_coord_t acc = 0;
            while (k < llen) {
                uint32_t cp = 0;
                int32_t used = ql_utf8_decode(lt + k, &cp);
                if (used <= 0)
                    break;
                ql_coord_t adv;
                if (cp == (uint32_t)'\t' && ts->tab_size != 0) {
                    const QLGlyph *sp = ql_font_find_glyph(font, ' ');
                    adv = ((sp != NULL) ? sp->adv_w : 0) * (ql_coord_t)ts->tab_size +
                          ts->letter_spacing;
                } else {
                    const QLGlyph *g = ql_font_find_glyph(font, cp);
                    adv = ((g != NULL) ? g->adv_w : 0) + ts->letter_spacing;
                    if (cp == (uint32_t)' ')
                        adv += ts->word_spacing;
                }
                if (acc + adv > cap)
                    break;
                acc += adv;
                k += used;
            }
            llen = k;
            line_w = acc;
            draw_ellipsis = (cap > 0);
        }

        ql_coord_t pen_x = box.x + x_indent;
        if (ts->align == QL_TEXT_ALIGN_CENTER)
            pen_x = box.x + x_indent + (box.w - x_indent - line_w) / 2;
        else if (ts->align == QL_TEXT_ALIGN_RIGHT)
            pen_x = box.x + box.w - line_w;

        /*
         * `text-align: justify`：把剩余空间平摊到字形间隙。
         * 规则与 CSS 一致：**最后一行**不拉伸（否则一段短末行会被拉得很难看），
         * 只有一行时也就等于左对齐；`text-overflow: ellipsis` 的行不拉伸。
         */
        ql_coord_t justify_extra = 0;
        if (ts->align == QL_TEXT_ALIGN_JUSTIFY && !draw_ellipsis && li + 1 < count &&
            box.w > 0) {
            const ql_coord_t slack = box.w - x_indent - line_w;
            if (slack > 0)
                justify_extra = slack;
        }

        const ql_coord_t line_x0 = pen_x;

        /*
         * 逐字形发射改成**先整形再画**（B8）：连字/字距/阿拉伯文连写都是
         * 上下文相关的，逐码位取字形在原理上做不到。
         *
         * 内建整形（没有 HarfBuzz）给出的位置与旧的逐码位循环完全一致，
         * 所以"没装 HarfBuzz"时像素行为不变。
         */
        {
            QLTextStyle tsg = *ts;
            QLShapeOpt sopt;
            sopt.letter_spacing = ts->letter_spacing;
            sopt.word_spacing = ts->word_spacing;
            sopt.tab_size = ts->tab_size;
            sopt.ligatures = ts->ligatures;
            sopt.kerning = ts->kerning;
            sopt.small_caps = ts->small_caps;
            sopt.tabular_nums = ts->tabular_nums;
            sopt.par_rtl = ts->rtl;

            /*
             * 分块整形：一块最多 QL_TEXT_SHAPE_MAX 个字形。跨块的连字会断
             * （一块 256 个字形远多于一行能显示的字数），换来的是零动态分配。
             */
            static QLShapeGlyph shaped[QL_TEXT_SHAPE_MAX];
            int32_t off = 0;
            while (off < llen) {
                int32_t end = off + (int32_t)QL_TEXT_SHAPE_MAX;
                if (end > llen)
                    end = llen;
                /* 块边界必须落在 UTF-8 字符边界上 */
                while (end > off && end < llen && ((unsigned char)lt[end] & 0xC0u) == 0x80u)
                    end--;

                ql_coord_t chunk_w = 0;
                int32_t nsh = ql_text_shape(font, lt + off, end - off, &sopt, shaped,
                                            (int32_t)QL_TEXT_SHAPE_MAX, &chunk_w);
                if (nsh > (int32_t)QL_TEXT_SHAPE_MAX)
                    nsh = (int32_t)QL_TEXT_SHAPE_MAX;

                ql_coord_t pen = pen_x;
                /* 两端对齐的每字形补偿（块内按"块内字形数 - 1"平摊；跨块时
                 * 用整行的总字形数近似，误差最多 1px 级，肉眼不可见） */
                ql_coord_t jstep = 0;
                if (justify_extra > 0 && nsh > 1) {
                    int32_t total = 0;
                    for (int32_t q2 = 0; q2 < llen; q2++) {
                        unsigned char ch = (unsigned char)lt[q2];
                        if ((ch & 0xC0u) != 0x80u)
                            total++;
                    }
                    if (total > 1)
                        jstep = justify_extra / (ql_coord_t)(total - 1);
                }
                for (int32_t k = 0; k < nsh; k++) {
                    const QLShapeGlyph *sg = &shaped[k];
                    pen += (ql_coord_t)sg->dx;
                    if (sg->glyph != NULL && sg->glyph->box_w > 0) {
                        /* 真字面已经有粗/斜体了，就不要再合成一遍 */
                        const QLFont *gf = (sg->font != NULL) ? sg->font : font;
                        tsg.bold = (uint8_t)((ts->bold && !gf->is_bold) ? 1 : 0);
                        tsg.italic = (uint8_t)((ts->italic && !gf->is_italic) ? 1 : 0);
                        emit_glyph(self, gf, sg->glyph, pen, baseline, &tsg);
                    }
                    pen += (ql_coord_t)sg->adv;
                    pen += jstep;
                }
                if (jstep != 0)
                    pen -= jstep;       /* 最后一个字形后面不该有间隙 */
                if (!draw_ellipsis)
                    pen_x = pen;        /* 省略号路径自己会摆到截断点 */
                else
                    pen_x = pen;
                (void)chunk_w;
                off = end;
            }
        }
        if (draw_ellipsis)
            emit_ellipsis(self, font, &pen_x, baseline, ts, emit_glyph);

        /*
         * 装饰线在这里画：本函数知道每行的起止与基线，两个后端就不用各写一遍。
         * 空行（llen == 0）不画，否则折行产生的空行会拖一条线。
         */
        if (ts->deco != 0 && llen > 0 && self->fill_rect != NULL) {
            const ql_coord_t x0 = line_x0;
            const ql_coord_t w = line_w;
            const uint8_t dstyle = (uint8_t)(ts->deco_style & QL_UI_DECO_LINE_MASK);
            const ql_coord_t th =
                (ts->deco_thickness > 0) ? (ql_coord_t)ts->deco_thickness : 1;
            const ql_color_t dcol =
                (ts->deco_alpha > 0) ? ts->deco_color : ts->color;
            const uint8_t dalpha = (ts->deco_alpha > 0) ? ts->deco_alpha : 255;

            /* 一条线（按 style 展开成实线 / 双线 / 点线 / 虚线） */
            if ((ts->deco & QL_TEXT_DECO_UNDERLINE) != 0) {
                /* 基线下方 2px + `text-underline-offset`：再低会碰到下一行的字 */
                const ql_coord_t y = baseline + 2 + (ql_coord_t)ts->deco_offset;
                deco_line(self, x0, y, w, th, dstyle, dcol, dalpha);
            }
            if ((ts->deco & QL_TEXT_DECO_LINE_THROUGH) != 0) {
                /* 大约在 x-height 的一半处（ascent 的一半偏上一点） */
                const ql_coord_t y = baseline - font->ascent / 2;
                deco_line(self, x0, y, w, th, dstyle, dcol, dalpha);
            }
            if ((ts->deco & QL_TEXT_DECO_OVERLINE) != 0) {
                /* 上划线画在 ascent 顶上方 1px */
                const ql_coord_t y = baseline - font->ascent - (ql_coord_t)ts->deco_offset;
                deco_line(self, x0, y, w, th, dstyle, dcol, dalpha);
            }
        }

        baseline += line_step;
    }
}

/* ------------------------------ CPU 后端 ------------------------------ */

typedef struct {
    QLCanvas *canvas;
    QLRect    saved_clip;

#if QL_FEAT_CSS_FILTER_LAYER
    /* ----- 离屏图层（M9.3）：一块小栈，深度上限 QL_CFG_UI_LAYER_MAX_DEPTH ----- */
    struct {
        int             stage;      /* 0 空闲、1 黑底那一遍、2 白底那一遍 */
        QLRect          dev;        /* 图层在设备像素里的范围 */
        QLDraw2DLayerFx fx;
        QLCanvas        saved;      /* 进入图层前的画布状态（缓冲 / 裁剪 / 遮罩） */
        uint8_t        *matte[2];   /* [0] 黑底、[1] 白底，都是紧凑 RGB888 */
        uint8_t        *rgba;       /* 第二遍末尾反解出来的预乘 RGBA */
        size_t          matte_bytes;
        ql_coord_t      saved_off_x;
        ql_coord_t      saved_off_y;
        ql_coord_t      saved_mask_cx, saved_mask_cy, saved_mask_r, saved_mask_ry;
        ql_coord_t      saved_scale;
        uint8_t         saved_alpha;
    } layer[QL_CFG_UI_LAYER_MAX_DEPTH];
    int layer_depth;
#endif
} QLDraw2DCanvas;

static QLDraw2DCanvas s_cpu;

/*
 * 缩放只发生在后端入口这一处：上层（qlui / 控件 / 自定义绘制）给的全是 CSS px。
 * 矩形按"边"缩放（ql_scale_rect），相邻矩形不会在接缝处裂开。
 */
/* 绕中心等比缩放一个矩形（按"边"缩放，相邻矩形不会裂缝） */
static inline int elem_is_identity(const QLDraw2D *self)
{
    return self->elem_x_fx == QL_SCALE_ONE && self->elem_y_fx == QL_SCALE_ONE;
}

/* 圆角/描边这类"长度"用 min(sx, sy)：非等比时按小的来，宁可方一点也别越界 */
static inline ql_coord_t elem_abs(ql_coord_t v)
{
    return (v < 0) ? (ql_coord_t)-v : v;
}

static inline ql_coord_t elem_len_fx(const QLDraw2D *self)
{
    /* 取**绝对值**里小的那个：负缩放（镜像）时长度不能被符号污染 */
    const ql_coord_t ax = elem_abs(self->elem_x_fx);
    const ql_coord_t ay = elem_abs(self->elem_y_fx);
    return (ax < ay) ? ax : ay;
}

static inline QLRect elem_rect(QLDraw2D *self, QLRect r)
{
    if (elem_is_identity(self))
        return r;
    ql_coord_t fx = self->elem_x_fx, fy = self->elem_y_fx;
    ql_coord_t x0 = self->elem_px +
                    (ql_coord_t)((int64_t)(r.x - self->elem_px) * fx / QL_SCALE_ONE);
    ql_coord_t x1 = self->elem_px +
                    (ql_coord_t)((int64_t)(ql_rect_right(r) - self->elem_px) * fx /
                                 QL_SCALE_ONE);
    ql_coord_t y0 = self->elem_py +
                    (ql_coord_t)((int64_t)(r.y - self->elem_py) * fy / QL_SCALE_ONE);
    ql_coord_t y1 = self->elem_py +
                    (ql_coord_t)((int64_t)(ql_rect_bottom(r) - self->elem_py) * fy /
                                 QL_SCALE_ONE);
    /* 负缩放（镜像）会让两端交换顺序：先归一化成合法矩形 */
    if (x1 < x0) {
        const ql_coord_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        const ql_coord_t t = y0;
        y0 = y1;
        y1 = t;
    }
    return ql_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static inline QLRect sx(QLDraw2D *self, QLRect r)
{
    r = elem_rect(self, r);
    if (self->scale_fx == QL_SCALE_ONE)
        return r;
    return ql_scale_rect(r, self->scale_fx);
}

static inline ql_coord_t sl(QLDraw2D *self, ql_coord_t v)
{
    if (!elem_is_identity(self))
        v = (ql_coord_t)((int64_t)v * elem_len_fx(self) / QL_SCALE_ONE);
    if (self->scale_fx == QL_SCALE_ONE)
        return v;
    return ql_scale_len(v, self->scale_fx);
}

/*
 * CSS px 的边 → 设备像素 26.6（含绘制期平移）。
 * 先拼成"CSS 26.6"再加偏移，最后乘缩放，避免两次取整。
 */
static inline ql_coord_t dev_edge_fx(QLDraw2D *self, ql_coord_t px, ql_coord_t off_fx)
{
    int64_t v = (int64_t)px * (int64_t)QL_FX_ONE + (int64_t)off_fx;     /* CSS 26.6 */
    return (ql_coord_t)((v * (int64_t)self->scale_fx) >> QL_FX_SHIFT); /* 设备 26.6 */
}

/* CSS px 矩形 → 设备像素 26.6 矩形（按边算，接缝不裂） */
static inline QLRectFx sxf(QLDraw2D *self, QLRect r)
{
    ql_coord_t x0 = dev_edge_fx(self, r.x, self->offset_x_fx);
    ql_coord_t y0 = dev_edge_fx(self, r.y, self->offset_y_fx);
    ql_coord_t x1 = dev_edge_fx(self, ql_rect_right(r), self->offset_x_fx);
    ql_coord_t y1 = dev_edge_fx(self, ql_rect_bottom(r), self->offset_y_fx);
    return ql_rectfx_make(x0, y0, x1 - x0, y1 - y0);
}

static inline int has_offset(QLDraw2D *self)
{
    return self->offset_x_fx != 0 || self->offset_y_fx != 0;
}

/* 把绘制期平移的**整数部分**折进矩形（文本/渐变/阴影这类整像素路径用） */
static QLRect shift_rect_rounded(QLDraw2D *self, QLRect r)
{
    if (!has_offset(self))
        return r;
    r.x += (ql_coord_t)(self->offset_x_fx >> QL_FX_SHIFT);
    r.y += (ql_coord_t)(self->offset_y_fx >> QL_FX_SHIFT);
    return r;
}

/*
 * CSS px 矩形 → 设备像素整数矩形：`elem_rect` 处理 transform: scale，
 * `sxf` 处理**绘制期平移**（transform: translate）与 dpr。
 *
 * 顺序不能反、也不能重复：`sx()` 里已经含一次 elem_rect，早先写成
 * `sx(self, elem_rect(self, rect))` 是**缩放算了两遍**，而且两份都没算平移 ——
 * 表现就是"带 transform 的毛玻璃糊的是它没移动之前那块地方"。
 */
static QLRect dev_rect_of(QLDraw2D *self, QLRect r)
{
    return ql_rect_from_fx(sxf(self, elem_rect(self, r)));
}

/* 毛玻璃：把设备像素区域里已有的内容就地模糊（CPU 侧的"背后"就是画布本身） */
static void cpu_backdrop_blur(QLDraw2D *self, QLRect rect, int sigma, uint8_t sat)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (ctx->canvas == NULL)
        return;

    QLRect dev = dev_rect_of(self, rect);
    ql_coord_t fx = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
    int r = (int)(((int64_t)sigma * fx) / QL_SCALE_ONE);
    if (r < 1)
        r = 1;
    (void)ql_canvas_backdrop_blur(ctx->canvas, dev, r, sat);
}

/*
 * 带圆角裁剪 + 折射：模糊只写进圆角矩形内，四个缺口保持原样（与 GPU 同一语义）。
 * 采样会外扩一圈（模糊核 + 折射），所以带 transform 的元素也必须传"现在"的位置 ——
 * 位置错了，外扩采到的就是它没动之前那块背景。
 */
static void cpu_backdrop_blur_round(QLDraw2D *self, QLRect rect, int sigma,
                                    ql_coord_t corner, uint8_t sat, int refract)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (ctx->canvas == NULL)
        return;

    QLRect dev = dev_rect_of(self, rect);
    ql_coord_t fx = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
    int r = 0;
    if (sigma > 0) {
        r = (int)(((int64_t)sigma * fx) / QL_SCALE_ONE);
        if (r < 1)
            r = 1;
    }
    const ql_coord_t cdev =
        (ql_coord_t)(((int64_t)corner * fx) / QL_SCALE_ONE);
    const int fdev = (int)(((int64_t)refract * fx) / QL_SCALE_ONE);
    (void)ql_canvas_backdrop_blur_round(ctx->canvas, dev, r, cdev, sat,
                                        fdev > 0 ? fdev : 0);
}

static void cpu_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t rx,
                         ql_coord_t ry);
static void cpu_set_mask_polygon(QLDraw2D *self, const ql_coord_t *xs,
                                 const ql_coord_t *ys, int n);
static void cpu_set_cm(QLDraw2D *self, const int32_t m[12], int on);

static void cpu_set_clip(QLDraw2D *self, QLRect clip)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_set_clip(ctx->canvas, sx(self, clip));
}

/* 椭圆/圆遮罩：CSS px → 设备像素（按"边"缩放，与矩形一致；半径按长度缩放） */
static void cpu_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t rx,
                         ql_coord_t ry)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (rx < 0 || ry < 0) {
        ql_canvas_set_mask(ctx->canvas, 0, 0, -1, -1);
        return;
    }
    if (self->scale_fx == QL_SCALE_ONE) {
        ql_canvas_set_mask(ctx->canvas, cx, cy, rx, ry);
        return;
    }
    QLRect pt = ql_scale_rect(ql_rect_make(cx, cy, 0, 0), self->scale_fx);
    ql_canvas_set_mask(ctx->canvas, pt.x, pt.y, ql_scale_len(rx, self->scale_fx),
                       ql_scale_len(ry, self->scale_fx));
}

/* 多边形遮罩：CSS px → 设备像素（点坐标与矩形一样按边缩放） */
static void cpu_set_mask_polygon(QLDraw2D *self, const ql_coord_t *xs,
                                 const ql_coord_t *ys, int n)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (xs == NULL || ys == NULL || n < 3) {
        ql_canvas_set_mask_polygon(ctx->canvas, NULL, NULL, 0);
        return;
    }
    float px[QL_CFG_UI_CLIP_PTS_MAX], py[QL_CFG_UI_CLIP_PTS_MAX];
    const ql_coord_t fx = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
    for (int i = 0; i < n; i++) {
        px[i] = (float)ql_scale_len(xs[i], fx);
        py[i] = (float)ql_scale_len(ys[i], fx);
    }
    ql_canvas_set_mask_polygon(ctx->canvas, px, py, n);
}

/* 颜色矩阵：直接交给画布（逐像素生效，见 ql_canvas_set_color_matrix） */
static void cpu_set_cm(QLDraw2D *self, const int32_t m[12], int on)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_set_color_matrix(ctx->canvas, m, on);
}

static void cpu_fill_rect(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    if (!has_offset(self)) {
        ql_canvas_fill_rect_alpha(ctx->canvas, sx(self, rect), color, alpha);
        return;
    }
    ql_canvas_fill_rect_fx(ctx->canvas, sxf(self, rect), color, alpha);
}

static void cpu_fill_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    if (!has_offset(self)) {
        ql_canvas_fill_round_rect_alpha(ctx->canvas, sx(self, rect), sl(self, radius), color,
                                        alpha);
        return;
    }
    ql_canvas_fill_round_rect_fx(ctx->canvas, sxf(self, rect),
                                 ql_fx_of(sl(self, radius)), color, alpha);
}

static void cpu_fill_round_rect_gradient(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                         ql_color_t from, ql_color_t to, uint8_t dir,
                                         uint8_t a1, uint8_t a2)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    a1 = ql_draw2d_eff_alpha(self, a1);
    a2 = ql_draw2d_eff_alpha(self, a2);
    if (a1 == 0 && a2 == 0)
        return;
    /* 渐变走的是逐行整像素填充：偏移只取整数部分（亚像素对渐变几乎看不出） */
    ql_canvas_fill_round_rect_gradient(ctx->canvas,
                                       sx(self, shift_rect_rounded(self, rect)),
                                       sl(self, radius), from, to, (QLGradientDir)dir,
                                       a1, a2);
}

static void cpu_fill_shadow(QLDraw2D *self, QLRect inner, ql_coord_t radius,
                            ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                            ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    ql_canvas_fill_shadow(ctx->canvas, sx(self, shift_rect_rounded(self, inner)),
                          sl(self, radius), sl(self, blur), sl(self, dx), sl(self, dy),
                          color, alpha);
}

static void cpu_fill_inset_shadow(QLDraw2D *self, QLRect inner, ql_coord_t radius,
                                  ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                                  ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    ql_canvas_fill_inset_shadow(ctx->canvas, sx(self, shift_rect_rounded(self, inner)),
                                sl(self, radius), sl(self, blur), sl(self, dx),
                                sl(self, dy), color, alpha);
}

/* 四角圆角：偏移/缩放时退回"最大半径"的单半径路径（见 QLRender 阅读指南） */
static void cpu_fill_round_rect4(QLDraw2D *self, QLRect rect, const ql_coord_t r4[4],
                                 ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    ql_coord_t maxr = 0;
    for (int i = 0; i < 4; i++)
        if (r4[i] > maxr)
            maxr = r4[i];
    if (!has_offset(self)) {
        ql_coord_t sr[4];
        for (int i = 0; i < 4; i++)
            sr[i] = sl(self, r4[i]);
        ql_canvas_fill_round_rect4_alpha(ctx->canvas, sx(self, rect), sr, color, alpha);
        return;
    }
    ql_canvas_fill_round_rect_fx(ctx->canvas, sxf(self, rect), ql_fx_of(sl(self, maxr)),
                                 color, alpha);
}

static void cpu_fill_round_rect_gradient4(QLDraw2D *self, QLRect rect,
                                          const ql_coord_t r4[4], ql_color_t from,
                                          ql_color_t to, uint8_t dir, uint8_t a1,
                                          uint8_t a2)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    a1 = ql_draw2d_eff_alpha(self, a1);
    a2 = ql_draw2d_eff_alpha(self, a2);
    if (a1 == 0 && a2 == 0)
        return;
    ql_coord_t maxr = 0;
    for (int i = 0; i < 4; i++)
        if (r4[i] > maxr)
            maxr = r4[i];
    /*
     * 渐变内核现在只接受单半径；四角不同时先用最大半径铺满，再把四个
     * "缺角"用小矩形补成背景色做不到（我们没有底色），所以这里直接
     * 退到单半径 —— 视觉上圆角偏大但不会露底。文档里如实写。
     */
    ql_canvas_fill_round_rect_gradient(ctx->canvas,
                                       sx(self, shift_rect_rounded(self, rect)),
                                       sl(self, maxr), from, to, (QLGradientDir)dir,
                                       a1, a2);
}

static void cpu_stroke_round_rect4(QLDraw2D *self, QLRect rect, const ql_coord_t r4[4],
                                   ql_coord_t thickness, ql_color_t color)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    const uint8_t alpha = ql_draw2d_eff_alpha(self, 255);
    if (alpha == 0)
        return;
    ql_coord_t maxr = 0;
    for (int i = 0; i < 4; i++)
        if (r4[i] > maxr)
            maxr = r4[i];
    if (!has_offset(self)) {
        ql_coord_t sr[4];
        for (int i = 0; i < 4; i++)
            sr[i] = sl(self, r4[i]);
        ql_canvas_stroke_rect4_alpha(ctx->canvas, sx(self, rect), sr, sl(self, thickness),
                                     color, alpha);
        return;
    }
    ql_canvas_stroke_rect_fx_alpha(ctx->canvas, sxf(self, rect),
                                   ql_fx_of(sl(self, maxr)), ql_fx_of(sl(self, thickness)),
                                   color, alpha);
}

static void cpu_stroke_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                  ql_coord_t thickness, ql_color_t color)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    const uint8_t alpha = ql_draw2d_eff_alpha(self, 255);
    if (alpha == 0)
        return;
    if (!has_offset(self)) {
        ql_canvas_stroke_rect_alpha(ctx->canvas, sx(self, rect), sl(self, radius),
                                    sl(self, thickness), color, alpha);
        return;
    }
    ql_canvas_stroke_rect_fx_alpha(ctx->canvas, sxf(self, rect),
                                   ql_fx_of(sl(self, radius)),
                                   ql_fx_of(sl(self, thickness)), color, alpha);
}

static void cpu_emit_glyph(QLDraw2D *self, const QLFont *font, const QLGlyph *glyph,
                           ql_coord_t pen_x, ql_coord_t baseline_y,
                           const QLTextStyle *ts)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    /*
     * 只有"不是等比正缩放"时才需要逐字形做水平 affine（压扁 / 镜像）。
     * 等比时字形已经换过字号，这里保持恒等 —— 那条路必须逐像素不变。
     */
    const uint8_t alpha = ql_draw2d_eff_alpha(self, 255);
    if (alpha == 0)
        return;
    if (!self->text_geo) {
        ql_canvas_draw_glyph_alpha(ctx->canvas, font, glyph, pen_x, baseline_y, ts->color,
                                   ts->italic != 0, ts->bold != 0, alpha);
        return;
    }
    ql_canvas_draw_glyph_xf_alpha(ctx->canvas, font, glyph, pen_x, baseline_y, ts->color,
                                  ts->italic != 0, ts->bold != 0,
                                  self->elem_x_fx, self->elem_y_fx,
                                  self->text_px, self->text_py, alpha);
}

/*
 * 缩放后的字号：DPR>1 时要用同样字族的设备像素字号重新取一次字体，
 * 否则字形会被拉伸（糊）。只有动态字体（有 backend）能真正按字号重取；
 * 位图字体在 MCU 上不缩放——那是位图字体的固有限制，不是这里偷懒。
 */
const QLFont *ql_draw2d_device_font(const QLFont *font, ql_coord_t scale_fx)
{
    if (font == NULL || font->backend == NULL || scale_fx == QL_SCALE_ONE ||
        font->pixel_size == 0)
        return font;

    ql_coord_t px = ql_scale_len((ql_coord_t)font->pixel_size, scale_fx);
    if (px <= 0)
        return font;

    const QLFont *dev = ql_font_default((int)px);
    return (dev != NULL && dev->backend != NULL) ? dev : font;
}

static void cpu_draw_text(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                          QLRect box, const QLTextStyle *ts)
{
    /*
     * 字形按"元素缩放 × DPR"重取字号：transform: scale() 在浏览器里会让文字
     * 一起变大（而且清晰）。只缩放位置而不换字号会让字又小又错位。
     */
    if (elem_len_fx(self) == 0)
        return;         /* 压到零宽：字一个都不画，否则会挤成一条墨线 */

    /*
     * 两条完全不同的路：
     *
     *  · **等比正缩放**（sx == sy > 0）：换一个更大的设备字号，再按盒子缩放。
     *    这样放大时字是重新光栅化的、清晰。
     *  · **非等比 / 镜像**（scaleX、负值）：字形的**高度必须不变**，
     *    只做几何变换。这时候绝不能让字号跟着 min(sx,sy) 缩小 —— 那会把
     *    `scaleX(0.5)` 画成"字小了一半"，而不是"压扁"。
     *    具体做法：按原始字号排版，emit 每个字形时把 x 过一遍 affine。
     */
    const ql_coord_t font_fx =
        (ql_coord_t)((int64_t)self->scale_fx * elem_len_fx(self) / QL_SCALE_ONE);
    const int uniform_pos = (self->elem_x_fx > 0 &&
                             self->elem_x_fx == self->elem_y_fx);
    const QLFont *scaled = ql_draw2d_device_font(font, font_fx);
    /*
     * 只有"等比 + 正 + 真的拿到了另一个字号的字体"才算清晰路径。
     * 位图字体（MCU 默认）根本没有别的字号可换，只能靠几何变换缩放。
     */
    const int crisp = uniform_pos && scaled != font;
    const QLFont *dev = crisp ? scaled : font;
    self->text_geo = (!crisp && (self->elem_x_fx != QL_SCALE_ONE ||
                                 self->elem_y_fx != QL_SCALE_ONE));
    self->text_px = (self->scale_fx != QL_SCALE_ONE)
                        ? ql_scale_len(self->elem_px, self->scale_fx)
                        : self->elem_px;
    self->text_py = (self->scale_fx != QL_SCALE_ONE)
                        ? ql_scale_len(self->elem_py, self->scale_fx)
                        : self->elem_py;
    /* 文本取偏移的整数部分：亚像素平移会让字形糊掉 */
    /*
     * 行高与装饰线厚度都是 CSS px，到后端要按同样的比例放大（否则 HiDPI 下
     * 1px 的下划线会变成半根）。这里只换算，不改语义。
     */
    QLTextStyle local = *ts;
    /*
     * 行高是**垂直**方向的长度，只能按垂直轴缩放。
     *
     * 之前按 font_fx（= min(sx, sy)）缩：`scaleX(0.5)` 的假翻转时 sy 明明是 1，
     * 行高却跟着减半 → 半行距 (line_h − ascent − descent) / 2 变小 → 基线相对
     * 盒子顶部整体上移，看起来就是"文字在翻转时上下跑"。
     */
    const ql_coord_t vert_fx =
        (crisp) ? (ql_coord_t)((int64_t)self->scale_fx * self->elem_y_fx / QL_SCALE_ONE)
                : self->scale_fx;
    if (local.line_h > 0)
        local.line_h = ql_scale_len(local.line_h, vert_fx);

    QLRect tb = crisp ? sx(self, shift_rect_rounded(self, box))
                      : ql_scale_rect(shift_rect_rounded(self, box), self->scale_fx);
    ql_draw2d_text_layout(self, dev, text, len, tb, &local, cpu_emit_glyph);
}

#if QL_FEAT_CSS_FILTER_LAYER
/* ------------------------------ 离屏图层（CPU） ------------------------------ */

/*
 * CPU 侧的离屏图层 = **两遍哑光反解**。
 *
 * 为什么不像 GPU 那样直接渲染到一张 RGBA 缓冲？因为 CPU 光栅的二十多条混合路径
 * （整行快路径、SSE2 块、字形位图、图片搬运……）全都写成"只在 RGB 上混、alpha
 * 恒置 0xFF"，要各自改成累积 alpha，改动面大且极易漏。反过来"同一棵子树画两次
 * （黑底一遍、白底一遍）再解方程"只用一条公式：
 *
 *   黑底：B = C*a          白底：W = C*a + 255*(1-a)
 *   ⇒  a = 255-(W-B)，而预乘色就是 B。
 *
 * 代价是这棵子树画两遍（滤镜本来就只用在局部），换来的是**零风险**：
 * 所有既有图元的混合语义一个字都不用改。GPU 那侧有真的 RGBA 帧缓冲，只画一遍。
 */
static QLRect cpu_layer_dev_rect(QLDraw2D *self, QLRect bounds)
{
    const QLRectFx f = sxf(self, elem_rect(self, bounds));
    const ql_coord_t x0 = (ql_coord_t)(f.x >> QL_FX_SHIFT);
    const ql_coord_t y0 = (ql_coord_t)(f.y >> QL_FX_SHIFT);
    const ql_coord_t x1 = (ql_coord_t)((f.x + f.w + QL_FX_ONE - 1) >> QL_FX_SHIFT);
    const ql_coord_t y1 = (ql_coord_t)((f.y + f.h + QL_FX_ONE - 1) >> QL_FX_SHIFT);
    if (x1 <= x0 || y1 <= y0)
        return ql_rect_make(0, 0, 0, 0);
    ql_coord_t w = x1 - x0;
    ql_coord_t h = y1 - y0;
    /* 上限：一块哑光缓冲 3 B/px。设错了也不该把内存吃光（约 12 MB 封顶） */
    if ((int64_t)w * (int64_t)h > 4096 * 1024) {
        w = 4096;
        h = 1024;
    }
    return ql_rect_make(x0, y0, w, h);
}

/* 把画布切到某块哑光缓冲，并把 `dev` 的左上角挪到 (0,0) */
static void cpu_layer_attach(QLDraw2D *self, uint8_t *buf, QLRect dev)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    QLCanvas *c = ctx->canvas;
    c->buffer = buf;
    c->stride = (uint32_t)dev.w * 3u;
    c->width = dev.w;
    c->height = dev.h;
    c->format = QL_COLOR_RGB888;
    c->clip = ql_rect_make(0, 0, dev.w, dev.h);
    c->mask_r = -1;      /* 图层的裁剪留到合成阶段（圆角与 mask 一起做） */
    c->mask_ry = -1;

    /*
     * 平移：让 dev 的左上角落在 (0,0)。
     * 设备位置 = (px + off/F) * scale ⇒ 减掉 dev.x 个设备像素就是
     * `off -= dev.x * F * F / scale`（偏移是 CSS 26.6）。
     */
    const ql_coord_t scale = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
    self->offset_x_fx -= (ql_coord_t)(((int64_t)dev.x * QL_FX_ONE * QL_FX_ONE) / scale);
    self->offset_y_fx -= (ql_coord_t)(((int64_t)dev.y * QL_FX_ONE * QL_FX_ONE) / scale);
}

/* 在栈的第 i 层开一个新图层（黑底第一遍） */
static int cpu_layer_open(QLDraw2D *self, int i, QLRect bounds,
                          const QLDraw2DLayerFx *fx)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (fx == NULL)
        return 0;
    const QLRect dev = cpu_layer_dev_rect(self, bounds);
    if (ql_rect_is_empty(dev))
        return 0;
    const size_t bytes = (size_t)dev.w * (size_t)dev.h * 3u;
    uint8_t *m0 = (uint8_t *)ql_mem_alloc(bytes);
    if (m0 == NULL)
        return 0;

    ctx->layer[i].stage = 0;
    ctx->layer[i].matte[0] = m0;
    ctx->layer[i].matte[1] = NULL;
    ctx->layer[i].rgba = NULL;
    ctx->layer[i].matte_bytes = bytes;
    ctx->layer[i].dev = dev;
    ctx->layer[i].fx = *fx;
    ctx->layer[i].saved = *ctx->canvas;
    ctx->layer[i].saved_off_x = self->offset_x_fx;
    ctx->layer[i].saved_off_y = self->offset_y_fx;

    cpu_layer_attach(self, m0, dev);
    ql_canvas_clear(ctx->canvas, 0x000000);
    ctx->layer[i].stage = 1;
    return 1;
}

/* 恢复进入图层前的画布与平移 */
static void cpu_layer_restore(QLDraw2D *self, int i)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    *ctx->canvas = ctx->layer[i].saved;
    self->offset_x_fx = ctx->layer[i].saved_off_x;
    self->offset_y_fx = ctx->layer[i].saved_off_y;
}

static int cpu_layer_begin(QLDraw2D *self, QLRect bounds, const QLDraw2DLayerFx *fx)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (ctx == NULL || ctx->canvas == NULL)
        return 0;
    /* 栈顶正处于"两遍之间"→ 这是第二遍（白底） */
    if (ctx->layer_depth > 0 && ctx->layer[ctx->layer_depth - 1].stage == 2) {
        const int i = ctx->layer_depth - 1;
        if (ctx->layer[i].matte[1] == NULL) {
            uint8_t *m1 = (uint8_t *)ql_mem_alloc(ctx->layer[i].matte_bytes);
            if (m1 == NULL)
                return 0;
            ctx->layer[i].matte[1] = m1;
        }
        cpu_layer_attach(self, ctx->layer[i].matte[1], ctx->layer[i].dev);
        ql_canvas_clear(ctx->canvas, 0xFFFFFF);
        ctx->layer[i].stage = 3;
        return 1;
    }
    if (ctx->layer_depth >= QL_CFG_UI_LAYER_MAX_DEPTH)
        return 0;
    if (!cpu_layer_open(self, ctx->layer_depth, bounds, fx))
        return 0;
    ctx->layer_depth++;
    return 1;
}

static int cpu_layer_end(QLDraw2D *self)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (ctx == NULL || ctx->canvas == NULL || ctx->layer_depth <= 0)
        return 0;
    const int i = ctx->layer_depth - 1;
    if (ctx->layer[i].stage == 1) {
        /* 黑底那一遍画完了：请调用方把同一棵子树再画一遍 */
        cpu_layer_restore(self, i);
        ctx->layer[i].stage = 2;
        return 1;
    }
    if (ctx->layer[i].stage != 3)
        return 0;

    QLRect dev = ctx->layer[i].dev;
    const QLDraw2DLayerFx fx = ctx->layer[i].fx;
    cpu_layer_restore(self, i);

    QLCanvas *dst = ctx->canvas;
    const size_t npx = (size_t)dev.w * (size_t)dev.h;
    uint8_t *rgba = (uint8_t *)ql_mem_alloc(npx * 4u);
    if (rgba != NULL) {
        ql_layer_reconstruct(ctx->layer[i].matte[0], ctx->layer[i].matte[1], dev.w,
                             dev.h, rgba);
        if (fx.blur > 0) {
            ql_coord_t scale = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
            int r = (int)(((int64_t)fx.blur * scale) / QL_SCALE_ONE);
            if (r < 1)
                r = 1;
            ql_layer_blur(rgba, dev.w, dev.h, r);
        }
        ql_layer_apply_color_ops(rgba, dev.w, dev.h, &fx);
        if (fx.has_drop)
            ql_layer_drop_shadow(rgba, dev.w, dev.h, &fx);
        ql_layer_composite(dst, rgba, dev.w, dev.h,
                           ql_rect_make(dev.x, dev.y, dev.w, dev.h), &fx,
                           (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE);
        ql_mem_free(rgba);
    }

    ql_mem_free(ctx->layer[i].matte[0]);
    ql_mem_free(ctx->layer[i].matte[1]);
    ctx->layer[i].matte[0] = NULL;
    ctx->layer[i].matte[1] = NULL;
    ctx->layer[i].stage = 0;
    ctx->layer_depth = i;
    return 0;
}
#endif /* QL_FEAT_CSS_FILTER_LAYER */

static void cpu_blit_image(QLDraw2D *self, const QLImage *img, QLRect dst,
                           const QLRect *src, ql_coord_t radius, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    alpha = ql_draw2d_eff_alpha(self, alpha);
    if (alpha == 0)
        return;
    /* dst 走与其它图元完全相同的坐标变换（offset / DPR / elem scale） */
    ql_canvas_blit_image(ctx->canvas, img,
                         ql_rect_from_fx(sxf(self, shift_rect_rounded(self, dst))), src,
                         sl(self, radius), alpha);
}

ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas)
{
    if (out == NULL || canvas == NULL)
        return QL_ERR_INVAL;

    s_cpu.canvas = canvas;
    s_cpu.saved_clip = canvas->clip;

    out->set_clip = cpu_set_clip;
    out->set_mask = cpu_set_mask;
    out->set_mask_polygon = cpu_set_mask_polygon;
    out->set_cm = cpu_set_cm;
    out->backdrop_blur = cpu_backdrop_blur;
    out->backdrop_blur_round = cpu_backdrop_blur_round;
#if QL_FEAT_CSS_FILTER_LAYER
    out->layer_begin = cpu_layer_begin;
    out->layer_end = cpu_layer_end;
#endif
    out->fill_rect = cpu_fill_rect;
    out->fill_round_rect = cpu_fill_round_rect;
    out->fill_round_rect_gradient = cpu_fill_round_rect_gradient;
    out->fill_shadow = cpu_fill_shadow;
    out->stroke_round_rect = cpu_stroke_round_rect;
    out->stroke_round_rect4 = cpu_stroke_round_rect4;
    out->fill_round_rect4 = cpu_fill_round_rect4;
    out->fill_round_rect_gradient4 = cpu_fill_round_rect_gradient4;
    out->fill_inset_shadow = cpu_fill_inset_shadow;
    out->draw_line = ql_draw2d_draw_line;
    out->draw_text = cpu_draw_text;
    out->blit_image = cpu_blit_image;
    out->flush = NULL;
    out->scale_fx = (canvas->scale_fx > 0) ? canvas->scale_fx : QL_SCALE_ONE;
    out->offset_x_fx = 0;
    out->offset_y_fx = 0;
    out->mask_r = -1;
    out->mask_ry = -1;
    out->mask_pn = 0;
    out->cm_on = 0;
    out->alpha = 255;          /* 默认不透明 */
    out->elem_x_fx = QL_SCALE_ONE;
    out->text_geo = 0;
    out->elem_y_fx = QL_SCALE_ONE;
    out->elem_px = 0;
    out->elem_py = 0;
    out->user = &s_cpu;
    return QL_OK;
}
