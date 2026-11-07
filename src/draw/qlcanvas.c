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

/* ==========================================================================
 * 快路径：一次性算好裁剪边界，然后按"行"逐格式写入。
 *
 * 旧实现每个像素都走 ql_canvas_write_pixel：空指针检查 + 4 次裁剪比较 +
 * 越界检查 + 格式 switch + memcpy。1080p 全屏要 44 ms。
 * 现在把格式 switch 提到循环外，行内只做一次存储 → 1080p 降到个位 ms。
 * ========================================================================== */

/* 不做任何检查的像素写入（调用方保证已在 clip 与缓冲区内） */
static inline void put_px(QLCanvas *c, ql_coord_t x, ql_coord_t y,
                          ql_color_t color, uint8_t alpha)
{
    uint8_t *row = c->buffer + (uint32_t)y * c->stride;

    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);

    switch (c->format) {
    case QL_COLOR_RGB565: {
        uint16_t *p = (uint16_t *)(void *)(row + (uint32_t)x * 2u);
        if (alpha == 255) {
            *p = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        } else {
            uint16_t old = *p;
            uint32_t orr = (uint32_t)((old >> 11) & 0x1Fu) * 255u / 31u;
            uint32_t og = (uint32_t)((old >> 5) & 0x3Fu) * 255u / 63u;
            uint32_t ob = (uint32_t)(old & 0x1Fu) * 255u / 31u;
            uint32_t inv = 255u - alpha;
            r = (uint8_t)(((uint32_t)r * alpha + orr * inv) / 255u);
            g = (uint8_t)(((uint32_t)g * alpha + og * inv) / 255u);
            b = (uint8_t)(((uint32_t)b * alpha + ob * inv) / 255u);
            *p = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        }
        break;
    }
    case QL_COLOR_RGB888: {
        uint8_t *p = row + (uint32_t)x * 3u;
        if (alpha != 255) {
            uint32_t inv = 255u - alpha;
            r = (uint8_t)(((uint32_t)r * alpha + (uint32_t)p[0] * inv) / 255u);
            g = (uint8_t)(((uint32_t)g * alpha + (uint32_t)p[1] * inv) / 255u);
            b = (uint8_t)(((uint32_t)b * alpha + (uint32_t)p[2] * inv) / 255u);
        }
        p[0] = r;
        p[1] = g;
        p[2] = b;
        break;
    }
    case QL_COLOR_ARGB8888: {
        uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x * 4u);
        if (alpha != 255) {
            uint32_t old = *p;
            uint32_t inv = 255u - alpha;
            r = (uint8_t)(((uint32_t)r * alpha + (old >> 16 & 0xFFu) * inv) / 255u);
            g = (uint8_t)(((uint32_t)g * alpha + (old >> 8 & 0xFFu) * inv) / 255u);
            b = (uint8_t)(((uint32_t)b * alpha + (old & 0xFFu) * inv) / 255u);
        }
        *p = 0xFF000000u | QL_COLOR_RGB(r, g, b);
        break;
    }
    default: {
        uint8_t *p = row + (uint32_t)x;
        uint8_t lum = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                                 (uint32_t)b * 29u) >> 8);
        if (alpha != 255)
            lum = (uint8_t)(((uint32_t)lum * alpha + (uint32_t)(*p) * (255u - alpha)) / 255u);
        *p = lum;
        break;
    }
    }
}

/* 写一行 [x0, x1]（含端点），alpha 必须已判断 */
static void fill_row(QLCanvas *c, ql_coord_t x0, ql_coord_t x1, ql_coord_t y,
                     ql_color_t color, uint8_t alpha)
{
    if (x1 < x0)
        return;

    uint8_t *row = c->buffer + (uint32_t)y * c->stride;
    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);
    uint32_t n = (uint32_t)(x1 - x0 + 1);

    if (alpha == 255) {
        switch (c->format) {
        case QL_COLOR_RGB565: {
            uint16_t *p = (uint16_t *)(void *)(row + (uint32_t)x0 * 2u);
            uint16_t px = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
            for (uint32_t i = 0; i < n; i++)
                p[i] = px;
            return;
        }
        case QL_COLOR_RGB888: {
            uint8_t *p = row + (uint32_t)x0 * 3u;
            /*
             * RGB888 每像素 3 字节，逐字节写很慢。
             * 这里一次处理 4 个像素（12 字节 = 3 个 uint32），
             * 用 3 次 32 位存储代替 12 次字节存储。
             */
            uint32_t w0 = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) |
                          ((uint32_t)r << 24);
            uint32_t w1 = (uint32_t)g | ((uint32_t)b << 8) | ((uint32_t)r << 16) |
                          ((uint32_t)g << 24);
            uint32_t w2 = (uint32_t)b | ((uint32_t)r << 8) | ((uint32_t)g << 16) |
                          ((uint32_t)b << 24);

            uint32_t i = 0;
            for (; i + 4 <= n; i += 4) {
                uint8_t *q = p + i * 3u;
                memcpy(q + 0, &w0, 4);
                memcpy(q + 4, &w1, 4);
                memcpy(q + 8, &w2, 4);
            }
            for (; i < n; i++) {
                p[i * 3 + 0] = r;
                p[i * 3 + 1] = g;
                p[i * 3 + 2] = b;
            }
            return;
        }
        case QL_COLOR_ARGB8888: {
            uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x0 * 4u);
            uint32_t px = 0xFF000000u | QL_COLOR_RGB(r, g, b);
            for (uint32_t i = 0; i < n; i++)
                p[i] = px;
            return;
        }
        default: {
            uint8_t lum = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                                     (uint32_t)b * 29u) >> 8);
            memset(row + (uint32_t)x0, lum, n);
            return;
        }
        }
    }

    for (ql_coord_t x = x0; x <= x1; x++)
        put_px(c, x, y, color, alpha);
}

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

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++)
        fill_row(canvas, area.x, ql_rect_right(area) - 1, y, color, alpha);
}

void ql_canvas_fill_rect(QLCanvas *canvas, QLRect rect, ql_color_t color)
{
    ql_canvas_fill_rect_alpha(canvas, rect, color, 255);
}

/*
 * 求圆角矩形某一行的可见横向跨度。
 * 从两端向内扫，最多扫 radius 步（圆角之外必定命中），
 * 语义与 ql_rect_contains_round 完全一致（是扫描而不是换公式，
 * 保证与既有像素级测试、GPU 后端的边界一致）。
 */
static int round_row_span(QLRect rect, ql_coord_t y, ql_coord_t radius,
                          ql_coord_t *out_x0, ql_coord_t *out_x1)
{
    if (y < rect.y || y >= ql_rect_bottom(rect))
        return 0;

    ql_coord_t x0 = rect.x;
    ql_coord_t x1 = ql_rect_right(rect) - 1;

    ql_coord_t limit = (rect.w < rect.h ? rect.w : rect.h) / 2;
    ql_coord_t rad = (radius > limit) ? limit : radius;

    if (rad > 0) {
        while (x0 < x1 && !ql_rect_contains_round(rect, x0, y, rad))
            x0++;
        while (x1 > x0 && !ql_rect_contains_round(rect, x1, y, rad))
            x1--;
        if (!ql_rect_contains_round(rect, x0, y, rad))
            return 0;
    }

    *out_x0 = x0;
    *out_x1 = x1;
    return 1;
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
        ql_coord_t sx0 = 0, sx1 = 0;
        if (!round_row_span(rect, y, radius, &sx0, &sx1))
            continue;
        if (sx0 < area.x)
            sx0 = area.x;
        if (sx1 > ql_rect_right(area) - 1)
            sx1 = ql_rect_right(area) - 1;
        fill_row(canvas, sx0, sx1, y, color, alpha);
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

    /* 圆角路径：环 = 外圆角矩形 - 内圆角矩形，按行取两段 */
    QLRect outer = rect;
    QLRect inner = ql_rect_make(rect.x + thickness, rect.y + thickness,
                               rect.w - 2 * thickness, rect.h - 2 * thickness);
    if (ql_rect_is_empty(inner))
        return;

    QLRect area = ql_rect_intersect(outer, canvas->clip);
    ql_coord_t clip_x0 = area.x;
    ql_coord_t clip_x1 = ql_rect_right(area) - 1;

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        ql_coord_t ox0 = 0, ox1 = 0;
        if (!round_row_span(outer, y, radius, &ox0, &ox1))
            continue;

        ql_coord_t ix0 = 0, ix1 = 0;
        int has_inner = round_row_span(inner, y, radius - thickness, &ix0, &ix1);

        /* 左段 */
        ql_coord_t seg0 = ox0 > clip_x0 ? ox0 : clip_x0;
        ql_coord_t seg1 = (has_inner ? ix0 - 1 : ox1);
        if (seg1 > clip_x1) seg1 = clip_x1;
        if (seg1 >= seg0)
            fill_row(canvas, seg0, seg1, y, color, 255);

        /* 右段 */
        if (has_inner) {
            seg0 = ix1 + 1 > clip_x0 ? ix1 + 1 : clip_x0;
            seg1 = ox1 < clip_x1 ? ox1 : clip_x1;
            if (seg1 >= seg0)
                fill_row(canvas, seg0, seg1, y, color, 255);
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

    /* 可见范围一次算好：之后行内不再做任何裁剪/越界判断 */
    ql_coord_t gx0 = 0, gx1 = (ql_coord_t)glyph->box_w - 1;
    if (origin_x < canvas->clip.x)
        gx0 = canvas->clip.x - origin_x;
    if (origin_x + gx1 > ql_rect_right(canvas->clip) - 1)
        gx1 = ql_rect_right(canvas->clip) - 1 - origin_x;
    if (origin_x < 0)
        gx0 = (gx0 > -origin_x) ? gx0 : -origin_x;
    if (origin_x + gx1 > canvas->width - 1)
        gx1 = canvas->width - 1 - origin_x;
    if (gx1 < gx0)
        return;

    ql_coord_t gy0 = 0, gy1 = (ql_coord_t)glyph->box_h - 1;
    if (origin_y < canvas->clip.y)
        gy0 = canvas->clip.y - origin_y;
    if (origin_y + gy1 > ql_rect_bottom(canvas->clip) - 1)
        gy1 = ql_rect_bottom(canvas->clip) - 1 - origin_y;
    if (origin_y < 0)
        gy0 = (gy0 > -origin_y) ? gy0 : -origin_y;
    if (origin_y + gy1 > canvas->height - 1)
        gy1 = canvas->height - 1 - origin_y;
    if (gy1 < gy0)
        return;

    uint32_t stride = ql_font_glyph_stride(font, glyph);

    for (ql_coord_t gy = gy0; gy <= gy1; gy++) {
        const uint8_t *src = font->bitmap + glyph->data_offset + (uint32_t)gy * stride;
        ql_coord_t y = origin_y + gy;

        switch (font->bpp) {
        case 4:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 2u];
                uint8_t nib = (gx % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
                if (nib != 0)
                    put_px(canvas, origin_x + gx, y, color, (uint8_t)(nib * 17u));
            }
            break;
        case 1:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 8u];
                if (byte & (0x80u >> (gx % 8)))
                    put_px(canvas, origin_x + gx, y, color, 255);
            }
            break;
        default:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t a = src[gx];
                if (a != 0)
                    put_px(canvas, origin_x + gx, y, color, a);
            }
            break;
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
