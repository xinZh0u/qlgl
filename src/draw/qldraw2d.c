/**
 * @file    qldraw2d.c
 * @brief   绘制接口的公共排版逻辑 + CPU（QLCanvas）后端适配。
 */
#include <qlgl/qlgl.h>

void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                           QLRect box, ql_color_t color, QLTextAlign align,
                           ql_draw2d_glyph_fn emit_glyph)
{
    if (self == NULL || font == NULL || text == NULL || emit_glyph == NULL)
        return;
    if (ql_rect_is_empty(box))
        return;

    ql_coord_t baseline = box.y + font->ascent;
    int32_t i = 0;
    int32_t line_start = 0;

    for (;;) {
        int at_end = (len < 0) ? (text[i] == '\0')
                               : (i >= len || text[i] == '\0');
        if (at_end || text[i] == '\n') {
            int32_t line_len = i - line_start;
            ql_coord_t line_w = ql_font_line_width(font, text + line_start, line_len);

            ql_coord_t pen_x = box.x;
            if (align == QL_TEXT_ALIGN_CENTER)
                pen_x = box.x + (box.w - line_w) / 2;
            else if (align == QL_TEXT_ALIGN_RIGHT)
                pen_x = box.x + box.w - line_w;

            int32_t j = 0;
            while (j < line_len) {
                uint32_t cp = 0;
                int32_t used = ql_utf8_decode(text + line_start + j, &cp);
                if (used <= 0)
                    break;
                const QLGlyph *glyph = ql_font_find_glyph(font, cp);
                if (glyph != NULL && glyph->box_w > 0)
                    emit_glyph(self, font, glyph, pen_x, baseline, color);
                if (glyph != NULL)
                    pen_x += glyph->adv_w;
                j += used;
            }

            if (at_end)
                break;
            baseline += font->line_height;
            line_start = i + 1;
        }
        i++;
    }
}

/* ------------------------------ CPU 后端 ------------------------------ */

typedef struct {
    QLCanvas *canvas;
    QLRect    saved_clip;
} QLDraw2DCanvas;

static QLDraw2DCanvas s_cpu;

static void cpu_set_clip(QLDraw2D *self, QLRect clip)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_set_clip(ctx->canvas, clip);
}

static void cpu_fill_rect(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_fill_rect_alpha(ctx->canvas, rect, color, alpha);
}

static void cpu_fill_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                ql_color_t color, uint8_t alpha)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_fill_round_rect_alpha(ctx->canvas, rect, radius, color, alpha);
}

static void cpu_stroke_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                  ql_coord_t thickness, ql_color_t color)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_stroke_rect(ctx->canvas, rect, radius, thickness, color);
}

static void cpu_emit_glyph(QLDraw2D *self, const QLFont *font, const QLGlyph *glyph,
                           ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color)
{
    QLDraw2DCanvas *ctx = (QLDraw2DCanvas *)self->user;
    ql_canvas_draw_glyph(ctx->canvas, font, glyph, pen_x, baseline_y, color);
}

static void cpu_draw_text(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                          QLRect box, ql_color_t color, QLTextAlign align)
{
    ql_draw2d_text_layout(self, font, text, len, box, color, align, cpu_emit_glyph);
}

ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas)
{
    if (out == NULL || canvas == NULL)
        return QL_ERR_INVAL;

    s_cpu.canvas = canvas;
    s_cpu.saved_clip = canvas->clip;

    out->set_clip = cpu_set_clip;
    out->fill_rect = cpu_fill_rect;
    out->fill_round_rect = cpu_fill_round_rect;
    out->stroke_round_rect = cpu_stroke_round_rect;
    out->draw_text = cpu_draw_text;
    out->flush = NULL;
    out->user = &s_cpu;
    return QL_OK;
}
