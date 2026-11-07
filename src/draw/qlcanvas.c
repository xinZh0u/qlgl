/**
 * @file    qlcanvas.c
 * @brief   画布原语实现。所有写入都先过裁剪，越界直接丢弃。
 */
#include <string.h>

#include <qlgl/qlgl.h>

/* ------------------------------ 像素读写 ------------------------------ */

ql_color_t ql_canvas_read_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y)
{
    if (canvas == NULL || x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return 0;

    const uint8_t *row = canvas->buffer + (uint32_t)y * canvas->stride;

    switch (canvas->format) {
    case QL_COLOR_RGB565: {
        uint16_t px;
        memcpy(&px, row + (uint32_t)x * 2u, 2);
        uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
        uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
        return QL_COLOR_RGB(r, g, b);
    }
    case QL_COLOR_RGB888:
        return QL_COLOR_RGB(row[x * 3 + 0], row[x * 3 + 1], row[x * 3 + 2]);
    case QL_COLOR_ARGB8888: {
        uint32_t px;
        memcpy(&px, row + (uint32_t)x * 4u, 4);
        return px & 0x00FFFFFFu;
    }
    default: {
        uint8_t v = row[x];
        return QL_COLOR_RGB(v, v, v);
    }
    }
}

void ql_canvas_write_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y,
                           ql_color_t color, uint8_t alpha)
{
    if (canvas == NULL || alpha == 0)
        return;
    if (x < canvas->clip.x || y < canvas->clip.y ||
        x >= ql_rect_right(canvas->clip) || y >= ql_rect_bottom(canvas->clip))
        return;
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return;

    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);

    if (alpha != 255) {
        ql_color_t old = ql_canvas_read_pixel(canvas, x, y);
        uint32_t a = alpha;
        uint32_t inv = 255u - a;
        r = (uint8_t)(((uint32_t)r * a + (uint32_t)QL_COLOR_GET_R(old) * inv) / 255u);
        g = (uint8_t)(((uint32_t)g * a + (uint32_t)QL_COLOR_GET_G(old) * inv) / 255u);
        b = (uint8_t)(((uint32_t)b * a + (uint32_t)QL_COLOR_GET_B(old) * inv) / 255u);
    }

    uint8_t *row = canvas->buffer + (uint32_t)y * canvas->stride;

    switch (canvas->format) {
    case QL_COLOR_RGB565: {
        uint16_t px = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        memcpy(row + (uint32_t)x * 2u, &px, 2);
        break;
    }
    case QL_COLOR_RGB888:
        row[x * 3 + 0] = r;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = b;
        break;
    case QL_COLOR_ARGB8888: {
        uint32_t px = 0xFF000000u | QL_COLOR_RGB(r, g, b);
        memcpy(row + (uint32_t)x * 4u, &px, 4);
        break;
    }
    default:
        row[x] = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                            (uint32_t)b * 29u) >> 8);
        break;
    }
}

ql_color_t ql_canvas_quantize(QLCanvas *canvas, ql_color_t color)
{
    if (canvas == NULL)
        return color;

    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);

    switch (canvas->format) {
    case QL_COLOR_RGB565:
        r = (uint8_t)((r >> 3) << 3);
        g = (uint8_t)((g >> 2) << 2);
        b = (uint8_t)((b >> 3) << 3);
        return QL_COLOR_RGB(r, g, b);
    case QL_COLOR_L8: {
        uint8_t v = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                               (uint32_t)b * 29u) >> 8);
        return QL_COLOR_RGB(v, v, v);
    }
    default:
        return color;
    }
}

/* ------------------------------ 画布构造 ------------------------------ */

ql_err_t ql_canvas_from_disp_clip(QLCanvas *canvas, QLDispDriver *driver, QLRect clip)
{
    if (canvas == NULL || driver == NULL || driver->framebuffer == NULL)
        return QL_ERR_INVAL;

    memset(canvas, 0, sizeof(*canvas));
    canvas->buffer = (uint8_t *)driver->framebuffer;
    canvas->stride = driver->stride;
    canvas->width = driver->width;
    canvas->height = driver->height;
    canvas->format = driver->format;
    canvas->clip = ql_rect_intersect(clip, ql_rect_make(0, 0, driver->width, driver->height));
    return QL_OK;
}

ql_err_t ql_canvas_from_disp(QLCanvas *canvas, QLDispDriver *driver)
{
    if (driver == NULL)
        return QL_ERR_INVAL;
    return ql_canvas_from_disp_clip(canvas, driver,
                                    ql_rect_make(0, 0, driver->width, driver->height));
}

void ql_canvas_set_clip(QLCanvas *canvas, QLRect clip)
{
    if (canvas == NULL)
        return;
    canvas->clip = ql_rect_intersect(clip, ql_rect_make(0, 0, canvas->width, canvas->height));
}

QLRect ql_canvas_clip(QLCanvas *canvas)
{
    return (canvas != NULL) ? canvas->clip : ql_rect_make(0, 0, 0, 0);
}

/* ------------------------------ 原语 ------------------------------ */

void ql_canvas_clear(QLCanvas *canvas, ql_color_t color)
{
    if (canvas == NULL)
        return;
    ql_canvas_fill_rect(canvas, canvas->clip, color);
}

void ql_canvas_fill_rect_alpha(QLCanvas *canvas, QLRect rect, ql_color_t color, uint8_t alpha)
{
    if (canvas == NULL || alpha == 0)
        return;

    QLRect area = ql_rect_intersect(rect, canvas->clip);
    if (ql_rect_is_empty(area))
        return;

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            if (alpha == 255) {
                /* 全不透明时跳过裁剪复检，直接按行写 */
                ql_canvas_write_pixel(canvas, x, y, color, 255);
            } else {
                ql_canvas_write_pixel(canvas, x, y, color, alpha);
            }
        }
    }
}

void ql_canvas_fill_rect(QLCanvas *canvas, QLRect rect, ql_color_t color)
{
    ql_canvas_fill_rect_alpha(canvas, rect, color, 255);
}

void ql_canvas_fill_round_rect_alpha(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                     ql_color_t color, uint8_t alpha)
{
    if (canvas == NULL || alpha == 0)
        return;

    QLRect area = ql_rect_intersect(rect, canvas->clip);
    if (ql_rect_is_empty(area))
        return;

    if (radius <= 0) {
        ql_canvas_fill_rect_alpha(canvas, area, color, alpha);
        return;
    }

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            if (ql_rect_contains_round(rect, x, y, radius))
                ql_canvas_write_pixel(canvas, x, y, color, alpha);
        }
    }
}

void ql_canvas_fill_round_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                               ql_color_t color)
{
    ql_canvas_fill_round_rect_alpha(canvas, rect, radius, color, 255);
}

void ql_canvas_stroke_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                           ql_coord_t thickness, ql_color_t color)
{
    if (canvas == NULL || thickness <= 0 || ql_rect_is_empty(rect))
        return;

    if (thickness > rect.w / 2)
        thickness = rect.w / 2;
    if (thickness > rect.h / 2)
        thickness = rect.h / 2;
    if (thickness <= 0)
        return;

    if (radius <= 0) {
        /* 直边快速路径：四条矩形 */
        ql_canvas_fill_rect(canvas, ql_rect_make(rect.x, rect.y, rect.w, thickness), color);
        ql_canvas_fill_rect(canvas,
                            ql_rect_make(rect.x, ql_rect_bottom(rect) - thickness, rect.w, thickness),
                            color);
        ql_canvas_fill_rect(canvas,
                            ql_rect_make(rect.x, rect.y + thickness, thickness,
                                         rect.h - 2 * thickness),
                            color);
        ql_canvas_fill_rect(canvas,
                            ql_rect_make(ql_rect_right(rect) - thickness, rect.y + thickness,
                                         thickness, rect.h - 2 * thickness),
                            color);
        return;
    }

    /* 圆角路径：环 = 外圆角矩形 - 内圆角矩形 */
    QLRect outer = rect;
    QLRect inner = ql_rect_make(rect.x + thickness, rect.y + thickness,
                               rect.w - 2 * thickness, rect.h - 2 * thickness);
    if (ql_rect_is_empty(inner))
        return;

    QLRect area = ql_rect_intersect(outer, canvas->clip);
    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            if (!ql_rect_contains_round(outer, x, y, radius))
                continue;
            if (ql_rect_contains_round(inner, x, y, radius - thickness))
                continue;
            ql_canvas_write_pixel(canvas, x, y, color, 255);
        }
    }
}

void ql_canvas_draw_hline(QLCanvas *canvas, ql_coord_t x1, ql_coord_t x2, ql_coord_t y,
                          ql_coord_t thickness, ql_color_t color)
{
    if (x2 < x1) {
        ql_coord_t t = x1;
        x1 = x2;
        x2 = t;
    }
    ql_canvas_fill_rect(canvas, ql_rect_make(x1, y, x2 - x1 + 1, thickness), color);
}

void ql_canvas_draw_vline(QLCanvas *canvas, ql_coord_t x, ql_coord_t y1, ql_coord_t y2,
                          ql_coord_t thickness, ql_color_t color)
{
    if (y2 < y1) {
        ql_coord_t t = y1;
        y1 = y2;
        y2 = t;
    }
    ql_canvas_fill_rect(canvas, ql_rect_make(x, y1, thickness, y2 - y1 + 1), color);
}

void ql_canvas_draw_line(QLCanvas *canvas, ql_coord_t x1, ql_coord_t y1,
                         ql_coord_t x2, ql_coord_t y2, ql_coord_t thickness,
                         ql_color_t color)
{
    /* Bresenham：只保证 1 像素线，thickness>1 时按方形笔刷加粗 */
    ql_coord_t dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    ql_coord_t dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    ql_coord_t sx = (x1 < x2) ? 1 : -1;
    ql_coord_t sy = (y1 < y2) ? 1 : -1;
    ql_coord_t err = dx - dy;
    ql_coord_t half = thickness > 0 ? thickness / 2 : 0;

    for (;;) {
        if (thickness <= 1)
            ql_canvas_write_pixel(canvas, x1, y1, color, 255);
        else
            ql_canvas_fill_rect(canvas,
                                ql_rect_make(x1 - half, y1 - half, thickness, thickness),
                                color);

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

/* ------------------------------ 文本 ------------------------------ */

void ql_canvas_draw_glyph(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color)
{
    if (canvas == NULL || font == NULL || glyph == NULL)
        return;
    if (glyph->box_w == 0 || glyph->box_h == 0)
        return;

    ql_coord_t origin_x = pen_x + glyph->ofs_x;
    ql_coord_t origin_y = baseline_y + glyph->ofs_y;

    for (uint32_t gy = 0; gy < glyph->box_h; gy++) {
        ql_coord_t y = origin_y + (ql_coord_t)gy;
        for (uint32_t gx = 0; gx < glyph->box_w; gx++) {
            uint8_t alpha = ql_font_glyph_alpha(font, glyph, (int32_t)gx, (int32_t)gy);
            if (alpha == 0)
                continue;
            ql_canvas_write_pixel(canvas, origin_x + (ql_coord_t)gx, y, color, alpha);
        }
    }
}

void ql_canvas_draw_text(QLCanvas *canvas, const QLFont *font, const char *text,
                         int32_t len, QLRect box, ql_color_t color, QLTextAlign align)
{
    if (canvas == NULL || font == NULL || text == NULL || ql_rect_is_empty(box))
        return;

    /* 先限制在 box 内，避免文字画到控件外面 */
    QLRect saved_clip = canvas->clip;
    ql_canvas_set_clip(canvas, ql_rect_intersect(canvas->clip, box));

    ql_coord_t baseline = box.y + font->ascent;
    int32_t i = 0;
    int32_t line_start = 0;

    for (;;) {
        int at_end = (len < 0) ? (text[i] == '\0') : (i >= len) || (text[i] == '\0');
        int at_break = !at_end && text[i] == '\n';

        if (at_end || at_break) {
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
                    ql_canvas_draw_glyph(canvas, font, glyph, pen_x, baseline, color);
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

    canvas->clip = saved_clip;
}
