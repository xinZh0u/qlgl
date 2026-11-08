/**
 * @file    qldraw2d.c
 * @brief   绘制接口的公共排版逻辑 + CPU（QLCanvas）后端适配。
 */
#include <string.h>

#include <qlgl/qlgl.h>

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
                         ql_coord_t max_w)
{
    if (max_w <= 0)
        return end;

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
        if (w + adv > max_w && i > pos) {
            if (last_break > pos)
                return last_break;
            /* 没有可用的软断点：硬断在当前字符前（超长单词/长串数字） */
            return i;
        }
        w += adv;

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
        } else if (is_cjk(cp) && i + used < end) {
            last_break = i + used;   /* CJK 字之间可以断 */
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
    if (font == NULL || text == NULL)
        return 0;

    const int32_t total = (int32_t)strlen(text);
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
            int32_t next = wrap_next(font, text, p, hard, max_w);
            int32_t line_end = trim_line_end(text, p, next);
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

    QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
    const int max_lines = (int)(sizeof(lines) / sizeof(lines[0]));
    int count = ql_text_wrap(font, src, ts->wrap ? box.w : 0, lines, max_lines);
    if (count > max_lines)
        count = max_lines;      /* 超出的行不画（布局统计不受此限） */

    ql_coord_t baseline = box.y + half_lead + font->ascent;
    for (int li = 0; li < count; li++) {
        const char *lt = lines[li].start;
        const int32_t llen = lines[li].len;
        const ql_coord_t line_w = ql_font_line_width(font, lt, llen);

        ql_coord_t pen_x = box.x;
        if (ts->align == QL_TEXT_ALIGN_CENTER)
            pen_x = box.x + (box.w - line_w) / 2;
        else if (ts->align == QL_TEXT_ALIGN_RIGHT)
            pen_x = box.x + box.w - line_w;

        const ql_coord_t line_x0 = pen_x;

        int32_t j = 0;
        while (j < llen) {
            uint32_t cp = 0;
            int32_t used = ql_utf8_decode(lt + j, &cp);
            if (used <= 0)
                break;
            const QLGlyph *glyph = ql_font_find_glyph(font, cp);
            if (glyph != NULL && glyph->box_w > 0)
                emit_glyph(self, font, glyph, pen_x, baseline, ts);
            if (glyph != NULL)
                pen_x += glyph->adv_w;
            j += used;
        }

        /*
         * 装饰线在这里画：本函数知道每行的起止与基线，两个后端就不用各写一遍。
         * 空行（llen == 0）不画，否则折行产生的空行会拖一条线。
         */
        if (ts->deco != 0 && llen > 0 && self->fill_rect != NULL) {
            const ql_coord_t x0 = line_x0;
            const ql_coord_t w = line_w;
            if ((ts->deco & QL_TEXT_DECO_UNDERLINE) != 0) {
                /* 基线下方 2px：再低会碰到下一行的字，再高会压住下伸部 */
                QLRect r = ql_rect_make(x0, baseline + 2, w, 1);
                self->fill_rect(self, r, ts->color, 255);
            }
            if ((ts->deco & QL_TEXT_DECO_LINE_THROUGH) != 0) {
                /* 大约在 x-height 的一半处（ascent 的一半偏上一点） */
                QLRect r = ql_rect_make(x0, baseline - font->ascent / 2, w, 1);
                self->fill_rect(self, r, ts->color, 255);
            }
        }

        baseline += line_step;
    }
}

/* ------------------------------ CPU 后端 ------------------------------ */

typedef struct {
    QLCanvas *canvas;
    QLRect    saved_clip;
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
static inline ql_coord_t elem_len_fx(const QLDraw2D *self)
{
    return (self->elem_x_fx < self->elem_y_fx) ? self->elem_x_fx : self->elem_y_fx;
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

/* 毛玻璃：把设备像素区域里已有的内容就地模糊（CPU 侧的"背后"就是画布本身） */
static void cpu_backdrop_blur(QLDraw2D *self, QLRect rect, int sigma)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (ctx->canvas == NULL)
        return;

    QLRect dev = sx(self, elem_rect(self, rect));
    ql_coord_t fx = (self->scale_fx > 0) ? self->scale_fx : QL_SCALE_ONE;
    int r = (int)(((int64_t)sigma * fx) / QL_SCALE_ONE);
    if (r < 1)
        r = 1;
    (void)ql_canvas_backdrop_blur(ctx->canvas, dev, r);
}

static void cpu_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t r);

static void cpu_set_clip(QLDraw2D *self, QLRect clip)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_set_clip(ctx->canvas, sx(self, clip));
}

/* 圆遮罩：CSS px → 设备像素（按"边"缩放，与矩形一致；半径按长度缩放） */
static void cpu_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t r)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (r < 0) {
        ql_canvas_set_mask(ctx->canvas, 0, 0, -1);
        return;
    }
    if (self->scale_fx == QL_SCALE_ONE) {
        ql_canvas_set_mask(ctx->canvas, cx, cy, r);
        return;
    }
    QLRect pt = ql_scale_rect(ql_rect_make(cx, cy, 0, 0), self->scale_fx);
    ql_canvas_set_mask(ctx->canvas, pt.x, pt.y, ql_scale_len(r, self->scale_fx));
}

static void cpu_fill_rect(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
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
                                         uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    /* 渐变走的是逐行整像素填充：偏移只取整数部分（亚像素对渐变几乎看不出） */
    ql_canvas_fill_round_rect_gradient(ctx->canvas,
                                       sx(self, shift_rect_rounded(self, rect)),
                                       sl(self, radius), from, to, (QLGradientDir)dir,
                                       alpha);
}

static void cpu_fill_shadow(QLDraw2D *self, QLRect inner, ql_coord_t radius,
                            ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                            ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_fill_shadow(ctx->canvas, sx(self, shift_rect_rounded(self, inner)),
                          sl(self, radius), sl(self, blur), sl(self, dx), sl(self, dy),
                          color, alpha);
}

static void cpu_stroke_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                  ql_coord_t thickness, ql_color_t color)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    if (!has_offset(self)) {
        ql_canvas_stroke_rect(ctx->canvas, sx(self, rect), sl(self, radius),
                              sl(self, thickness), color);
        return;
    }
    ql_canvas_stroke_rect_fx(ctx->canvas, sxf(self, rect), ql_fx_of(sl(self, radius)),
                             ql_fx_of(sl(self, thickness)), color);
}

static void cpu_emit_glyph(QLDraw2D *self, const QLFont *font, const QLGlyph *glyph,
                           ql_coord_t pen_x, ql_coord_t baseline_y,
                           const QLTextStyle *ts)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_draw_glyph_ex(ctx->canvas, font, glyph, pen_x, baseline_y, ts->color,
                            ts->italic != 0, ts->bold != 0);
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
    const ql_coord_t font_fx =
        (ql_coord_t)((int64_t)self->scale_fx * elem_len_fx(self) / QL_SCALE_ONE);
    const QLFont *dev = ql_draw2d_device_font(font, font_fx);
    /* 文本取偏移的整数部分：亚像素平移会让字形糊掉 */
    /*
     * 行高与装饰线厚度都是 CSS px，到后端要按同样的比例放大（否则 HiDPI 下
     * 1px 的下划线会变成半根）。这里只换算，不改语义。
     */
    QLTextStyle local = *ts;
    if (local.line_h > 0)
        local.line_h = ql_scale_len(local.line_h, font_fx);
    ql_draw2d_text_layout(self, dev, text, len, sx(self, shift_rect_rounded(self, box)),
                          &local, cpu_emit_glyph);
}

static void cpu_blit_image(QLDraw2D *self, const QLImage *img, QLRect dst,
                           const QLRect *src, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    /* dst 走与其它图元完全相同的坐标变换（offset / DPR / elem scale） */
    ql_canvas_blit_image(ctx->canvas, img,
                         ql_rect_from_fx(sxf(self, shift_rect_rounded(self, dst))), src,
                         alpha);
}

ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas)
{
    if (out == NULL || canvas == NULL)
        return QL_ERR_INVAL;

    s_cpu.canvas = canvas;
    s_cpu.saved_clip = canvas->clip;

    out->set_clip = cpu_set_clip;
    out->set_mask = cpu_set_mask;
    out->backdrop_blur = cpu_backdrop_blur;
    out->fill_rect = cpu_fill_rect;
    out->fill_round_rect = cpu_fill_round_rect;
    out->fill_round_rect_gradient = cpu_fill_round_rect_gradient;
    out->fill_shadow = cpu_fill_shadow;
    out->stroke_round_rect = cpu_stroke_round_rect;
    out->draw_line = ql_draw2d_draw_line;
    out->draw_text = cpu_draw_text;
    out->blit_image = cpu_blit_image;
    out->flush = NULL;
    out->scale_fx = (canvas->scale_fx > 0) ? canvas->scale_fx : QL_SCALE_ONE;
    out->offset_x_fx = 0;
    out->offset_y_fx = 0;
    out->mask_r = -1;
    out->elem_x_fx = QL_SCALE_ONE;
    out->elem_y_fx = QL_SCALE_ONE;
    out->elem_px = 0;
    out->elem_py = 0;
    out->user = &s_cpu;
    return QL_OK;
}
