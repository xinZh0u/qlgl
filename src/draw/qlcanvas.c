/**
 * @file    qlcanvas.c
 * @brief   画布原语实现。所有写入都先过裁剪，越界直接丢弃。
 */
#include <math.h>
#include <string.h>

#include <qlgl/qlgl.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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

/*
 * put_px 的行指针版本：调用方已算好行起点，省掉每像素一次
 * c->buffer + y*stride（阴影的环内循环会调用几十万次）。
 */
static inline void put_px_at(uint8_t *row, uint8_t format, ql_coord_t x,
                             ql_color_t color, uint8_t alpha)
{
    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);

    switch (format) {
    case QL_COLOR_RGB565: {
        uint16_t *p = (uint16_t *)(void *)(row + (uint32_t)x * 2u);
        uint16_t old = *p;
        uint32_t orr = (uint32_t)((old >> 11) & 0x1Fu) * 255u / 31u;
        uint32_t og = (uint32_t)((old >> 5) & 0x3Fu) * 255u / 63u;
        uint32_t ob = (uint32_t)(old & 0x1Fu) * 255u / 31u;
        uint32_t inv = 255u - alpha;
        r = (uint8_t)(((uint32_t)r * alpha + orr * inv) / 255u);
        g = (uint8_t)(((uint32_t)g * alpha + og * inv) / 255u);
        b = (uint8_t)(((uint32_t)b * alpha + ob * inv) / 255u);
        *p = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        break;
    }
    case QL_COLOR_RGB888: {
        uint8_t *p = row + (uint32_t)x * 3u;
        uint32_t inv = 255u - alpha;
        p[0] = (uint8_t)(((uint32_t)r * alpha + (uint32_t)p[0] * inv) / 255u);
        p[1] = (uint8_t)(((uint32_t)g * alpha + (uint32_t)p[1] * inv) / 255u);
        p[2] = (uint8_t)(((uint32_t)b * alpha + (uint32_t)p[2] * inv) / 255u);
        break;
    }
    case QL_COLOR_ARGB8888: {
        uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x * 4u);
        uint32_t old = *p;
        uint32_t inv = 255u - alpha;
        r = (uint8_t)(((uint32_t)r * alpha + (old >> 16 & 0xFFu) * inv) / 255u);
        g = (uint8_t)(((uint32_t)g * alpha + (old >> 8 & 0xFFu) * inv) / 255u);
        b = (uint8_t)(((uint32_t)b * alpha + (old & 0xFFu) * inv) / 255u);
        *p = 0xFF000000u | QL_COLOR_RGB(r, g, b);
        break;
    }
    default: {
        uint8_t *p = row + (uint32_t)x;
        uint8_t lum = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                                 (uint32_t)b * 29u) >> 8);
        *p = (uint8_t)(((uint32_t)lum * alpha + (uint32_t)(*p) * (255u - alpha)) / 255u);
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

    /*
     * 半透明路径：把行指针与格式 switch 提到循环外。
     *
     * 旧实现逐像素调 put_px()，每像素重算 c->buffer + y*stride 并再跳一次
     * switch —— 软阴影、渐变、圆角覆盖都是整行同 alpha，代价全砸在这上面。
     *
     * 混合式与 put_px 完全一致：out = (src*a + dst*(255-a)) / 255。
     * 中间和 < 255*255 = 65025，能用 16 位无符号装下，
     * 因此 /255 可以用与截断除法逐值相等的两个恒等式加速：
     *   标量   (x + (x>>8) + 1) >> 8
     *   SSE2   mulhi_epu16(x, 32897) >> 7
     * （0..65025 全域已验证与 x/255 逐值相同）
     */
    switch (c->format) {
    case QL_COLOR_ARGB8888: {
        uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x0 * 4u);
        const uint32_t inv = 255u - alpha;
        const uint32_t sr = (uint32_t)r * alpha;
        const uint32_t sg = (uint32_t)g * alpha;
        const uint32_t sb = (uint32_t)b * alpha;
        uint32_t i = 0;
#if defined(__SSE2__)
        {
            /* dst*inv + 源项(按通道预乘) 一次算完；A 通道最后统一置 0xFF */
            const __m128i invv = _mm_set1_epi16((short)inv);
            /* 车道序 = unpacklo 后的 [b, g, r, a] × 2；src 是 16 位预乘值，
               不能塞进 8 位槽位，必须逐车道写入 */
            const __m128i srcv = _mm_setr_epi16(
                (short)(uint16_t)sb, (short)(uint16_t)sg, (short)(uint16_t)sr, 0,
                (short)(uint16_t)sb, (short)(uint16_t)sg, (short)(uint16_t)sr, 0);
            const __m128i k255 = _mm_set1_epi16((short)32897);
            const __m128i opaque = _mm_set1_epi32((int)0xFF000000u);
            const __m128i zero = _mm_setzero_si128();
            for (; i + 4u <= n; i += 4u) {
                __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
                __m128i lo = _mm_unpacklo_epi8(v, zero);
                __m128i hi = _mm_unpackhi_epi8(v, zero);
                lo = _mm_add_epi16(_mm_mullo_epi16(lo, invv), srcv);
                hi = _mm_add_epi16(_mm_mullo_epi16(hi, invv), srcv);
                lo = _mm_srli_epi16(_mm_mulhi_epu16(lo, k255), 7);
                hi = _mm_srli_epi16(_mm_mulhi_epu16(hi, k255), 7);
                _mm_storeu_si128((__m128i *)(void *)(p + i),
                                 _mm_or_si128(_mm_packus_epi16(lo, hi), opaque));
            }
        }
#endif
        for (; i < n; i++) {
            uint32_t old = p[i];
            uint32_t dr = (sr + ((old >> 16) & 0xFFu) * inv) / 255u;
            uint32_t dg = (sg + ((old >> 8) & 0xFFu) * inv) / 255u;
            uint32_t db = (sb + (old & 0xFFu) * inv) / 255u;
            p[i] = 0xFF000000u | (dr << 16) | (dg << 8) | db;
        }
        return;
    }
    case QL_COLOR_RGB888: {
        uint8_t *p = row + (uint32_t)x0 * 3u;
        const uint32_t inv = 255u - alpha;
        const uint32_t sr = (uint32_t)r * alpha;
        const uint32_t sg = (uint32_t)g * alpha;
        const uint32_t sb = (uint32_t)b * alpha;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t tr = sr + (uint32_t)p[0] * inv;
            uint32_t tg = sg + (uint32_t)p[1] * inv;
            uint32_t tb = sb + (uint32_t)p[2] * inv;
            p[0] = (uint8_t)((tr + (tr >> 8) + 1u) >> 8);
            p[1] = (uint8_t)((tg + (tg >> 8) + 1u) >> 8);
            p[2] = (uint8_t)((tb + (tb >> 8) + 1u) >> 8);
            p += 3;
        }
        return;
    }
    case QL_COLOR_RGB565: {
        uint16_t *p = (uint16_t *)(void *)(row + (uint32_t)x0 * 2u);
        const uint32_t inv = 255u - alpha;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t old = p[i];
            uint32_t dr = ((uint32_t)r * alpha +
                           ((old >> 11) & 0x1Fu) * 255u / 31u * inv) / 255u;
            uint32_t dg = ((uint32_t)g * alpha +
                           ((old >> 5) & 0x3Fu) * 255u / 63u * inv) / 255u;
            uint32_t db = ((uint32_t)b * alpha +
                           (old & 0x1Fu) * 255u / 31u * inv) / 255u;
            p[i] = (uint16_t)(((dr & 0xF8u) << 8) | ((dg & 0xFCu) << 3) | (db >> 3));
        }
        return;
    }
    default: {
        uint8_t *p = row + (uint32_t)x0;
        const uint32_t inv = 255u - alpha;
        const uint32_t lum = ((uint32_t)r * 77u + (uint32_t)g * 150u +
                              (uint32_t)b * 29u) >> 8;
        const uint32_t sl = lum * alpha;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t t = sl + (uint32_t)p[i] * inv;
            p[i] = (uint8_t)((t + (t >> 8) + 1u) >> 8);
        }
        return;
    }
    }
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
 *
 * 原实现从两端向内**逐像素扫描** + 每次调 ql_rect_contains_round（两次乘法），
 * 半径 12 时每行约 24 次调用。这里改成解析求解：
 *
 *   判定条件 dx² + dy² <= rad²   （dx/dy 为到"圆角圆心矩形"的距离）
 *   固定 y ⇒ dy 已知 ⇒ dx_max = floor(sqrt(rad² - dy²))
 *   ⇒ 跨度 = [ix1 - dx_max, ix2 + dx_max] 与矩形边界求交
 *
 * 结果与原扫描**逐像素一致**（test_canvas 有像素级断言守着），
 * 但每行只剩一次 sqrtf 与两次比较。
 */
static inline int round_row_span(QLRect rect, ql_coord_t y, ql_coord_t radius,
                          ql_coord_t *out_x0, ql_coord_t *out_x1)
{
    if (y < rect.y || y >= ql_rect_bottom(rect))
        return 0;

    ql_coord_t x0 = rect.x;
    ql_coord_t x1 = ql_rect_right(rect) - 1;

    ql_coord_t limit = (rect.w < rect.h ? rect.w : rect.h) / 2;
    ql_coord_t rad = (radius > limit) ? limit : radius;
    if (rad < 0)
        rad = 0;
    if (rad == 0) {
        *out_x0 = x0;
        *out_x1 = x1;
        return 1;
    }

    const ql_coord_t ix1 = rect.x + rad;
    const ql_coord_t iy1 = rect.y + rad;
    const ql_coord_t ix2 = ql_rect_right(rect) - 1 - rad;
    const ql_coord_t iy2 = ql_rect_bottom(rect) - 1 - rad;

    ql_coord_t dy = 0;
    if (y < iy1)
        dy = iy1 - y;
    else if (y > iy2)
        dy = y - iy2;

    if (dy > rad)
        return 0;

    ql_coord_t dx_max;
    if (dy == 0) {
        dx_max = rad;
    } else {
        /* 向下取整的整数平方根，与 dx*dx + dy*dy <= rad*rad 的边界完全一致 */
        ql_coord_t rem = rad * rad - dy * dy;
        dx_max = (ql_coord_t)sqrtf((float)rem);
        while (dx_max * dx_max > rem)
            dx_max--;
        while ((dx_max + 1) * (dx_max + 1) <= rem)
            dx_max++;
    }

    ql_coord_t lo = ix1 - dx_max;
    ql_coord_t hi = ix2 + dx_max;
    if (lo > x0)
        x0 = lo;
    if (hi < x1)
        x1 = hi;
    if (x1 < x0)
        return 0;

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

/* ------------------------------ 渐变 / 阴影 ------------------------------ */

/* 两个颜色的定点插值：t 为 0..255 */
static ql_color_t lerp_color(ql_color_t a, ql_color_t b, uint32_t t)
{
    uint32_t inv = 255u - t;
    uint32_t r = ((uint32_t)QL_COLOR_GET_R(a) * inv + (uint32_t)QL_COLOR_GET_R(b) * t) / 255u;
    uint32_t g = ((uint32_t)QL_COLOR_GET_G(a) * inv + (uint32_t)QL_COLOR_GET_G(b) * t) / 255u;
    uint32_t bl = ((uint32_t)QL_COLOR_GET_B(a) * inv + (uint32_t)QL_COLOR_GET_B(b) * t) / 255u;
    return QL_COLOR_RGB(r, g, bl);
}

void ql_canvas_fill_round_rect_gradient(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                        ql_color_t from, ql_color_t to,
                                        QLGradientDir dir, uint8_t alpha)
{
    if (canvas == NULL || alpha == 0)
        return;

    QLRect area = ql_rect_intersect(rect, canvas->clip);
    if (ql_rect_is_empty(area))
        return;

    ql_coord_t span = (dir == QL_GRADIENT_HORIZONTAL) ? rect.w : rect.h;
    if (span <= 1) {
        /* 退化成一格：直接用起始色 */
        ql_canvas_fill_round_rect_alpha(canvas, rect, radius, from, alpha);
        return;
    }

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        ql_coord_t sx0 = rect.x;
        ql_coord_t sx1 = ql_rect_right(rect) - 1;

        if (radius > 0 && !round_row_span(rect, y, radius, &sx0, &sx1))
            continue;

        if (sx0 < area.x)
            sx0 = area.x;
        if (sx1 > ql_rect_right(area) - 1)
            sx1 = ql_rect_right(area) - 1;
        if (sx1 < sx0)
            continue;

        if (dir == QL_GRADIENT_VERTICAL) {
            /* 快路径：整行同一个颜色 */
            uint32_t t = (uint32_t)((y - rect.y) * 255) / (uint32_t)(span - 1);
            fill_row(canvas, sx0, sx1, y, lerp_color(from, to, t), alpha);
        } else {
            for (ql_coord_t x = sx0; x <= sx1; x++) {
                uint32_t t = (uint32_t)((x - rect.x) * 255) / (uint32_t)(span - 1);
                put_px(canvas, x, y, lerp_color(from, to, t), alpha);
            }
        }
    }
}

/*
 * 单像素混合写（行指针已给）。颜色分量与格式预先取好，
 * 供阴影角区那种"一次算覆盖率、写四个镜像位置"的循环使用。
 */
static inline void blend_px(uint8_t *row, ql_coord_t x, uint8_t fmt, uint32_t cr,
                            uint32_t cg, uint32_t cb, ql_color_t color, uint32_t a)
{
    if (fmt == QL_COLOR_ARGB8888) {
        uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x * 4u);
        uint32_t old = *p;
        uint32_t inv = 255u - a;
        uint32_t dr = (cr * a + ((old >> 16) & 0xFFu) * inv) / 255u;
        uint32_t dg = (cg * a + ((old >> 8) & 0xFFu) * inv) / 255u;
        uint32_t db = (cb * a + (old & 0xFFu) * inv) / 255u;
        *p = 0xFF000000u | (dr << 16) | (dg << 8) | db;
        return;
    }
    put_px_at(row, fmt, x, color, (uint8_t)a);
}

/*
 * 垂直填充一列 [y0, y1]，alpha 必须已判断。
 * 阴影的左右直边带用它：那里覆盖率只跟 x 有关，整列同色。
 */
static void fill_col(QLCanvas *c, ql_coord_t x, ql_coord_t y0, ql_coord_t y1,
                     ql_color_t color, uint8_t alpha)
{
    if (y1 < y0)
        return;

    if (c->format == QL_COLOR_ARGB8888) {
        /* 主格式直写：把格式判定与色分量提取提到循环外 */
        uint8_t *p = c->buffer + (uint32_t)y0 * c->stride + (uint32_t)x * 4u;
        const uint32_t inv = 255u - alpha;
        const uint32_t sr = (uint32_t)QL_COLOR_GET_R(color) * alpha;
        const uint32_t sg = (uint32_t)QL_COLOR_GET_G(color) * alpha;
        const uint32_t sb = (uint32_t)QL_COLOR_GET_B(color) * alpha;
        for (ql_coord_t y = y0; y <= y1; y++) {
            uint32_t old = *(uint32_t *)(void *)p;
            uint32_t dr = (sr + ((old >> 16) & 0xFFu) * inv) / 255u;
            uint32_t dg = (sg + ((old >> 8) & 0xFFu) * inv) / 255u;
            uint32_t db = (sb + (old & 0xFFu) * inv) / 255u;
            *(uint32_t *)(void *)p = 0xFF000000u | (dr << 16) | (dg << 8) | db;
            p += c->stride;
        }
        return;
    }

    for (ql_coord_t y = y0; y <= y1; y++) {
        uint8_t *row = c->buffer + (uint32_t)y * c->stride;
        put_px_at(row, c->format, x, color, alpha);
    }
}

/* 距离 → 覆盖率（0..255，定点），与 GPU 的 smoothstep 同式 */
static inline uint8_t shadow_cov_u8(float d, float blur)
{
    if (d <= 0.0f)
        return 255;
    if (blur <= 0.0f)
        return 0;
    float t = d / blur;
    if (t >= 1.0f)
        return 0;
    float cov = 1.0f - (t * t * (3.0f - 2.0f * t));
    uint32_t a = (uint32_t)(cov * 255.0f + 0.5f);
    return (uint8_t)(a > 255 ? 255 : a);
}

/*
 * 软阴影：SDF + smoothstep。
 *
 * 关键优化——按九宫格分解，避免对整个外框逐像素算距离：
 *
 *      ┌────┬──────────────┬────┐
 *      │ 角 │  上边（行同色）│ 角 │
 *      ├────┼──────────────┼────┤
 *      │左边│   实心（全覆盖）│右边│   ← 左右边与"列同色"
 *      │列同色              │列同色│
 *      ├────┼──────────────┼────┤
 *      │ 角 │  下边（行同色）│ 角 │
 *      └────┴──────────────┴────┘
 *
 * 上/下边带：距离只取决于 y → 整行一个 alpha（fill_row）。
 * 左/右边带：距离只取决于 x → 整列一个 alpha（fill_col）。
 * 只有四个圆角区（rad × blur 的小方块）需要逐像素 sqrt。
 *
 * 一张 143×68 的卡片、blur=10：逐像素次数从 ~4200 降到 ~400（约 10×）。
 * 圆角半径为 0 时四个角退化掉，全部走行/列填充。
 */
void ql_canvas_fill_shadow(QLCanvas *canvas, QLRect inner, ql_coord_t radius,
                           ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                           ql_color_t color, uint8_t alpha)
{
    if (canvas == NULL || alpha == 0 || ql_rect_is_empty(inner))
        return;
    if (blur < 0)
        blur = 0;

    /* 阴影轮廓 = inner 平移；绘制区域 = 轮廓外扩 blur（+1 给衰减留边） */
    const QLRect shaped = ql_rect_make(inner.x + dx, inner.y + dy, inner.w, inner.h);
    const ql_coord_t pad = blur + 1;
    const QLRect outer = ql_rect_make(shaped.x - pad, shaped.y - pad,
                                      shaped.w + 2 * pad, shaped.h + 2 * pad);

    QLRect area = ql_rect_intersect(outer, canvas->clip);
    if (ql_rect_is_empty(area))
        return;

    ql_coord_t limit = (shaped.w < shaped.h ? shaped.w : shaped.h) / 2;
    ql_coord_t rad = (radius > limit) ? limit : (radius < 0 ? 0 : radius);

    const float frad = (float)rad;
    const float fblur = (float)blur;

    const ql_coord_t sx0 = shaped.x, sx1 = ql_rect_right(shaped);      /* 半开 */
    const ql_coord_t sy0 = shaped.y, sy1 = ql_rect_bottom(shaped);
    const ql_coord_t ax0 = area.x, ax1 = ql_rect_right(area);
    const ql_coord_t ay0 = area.y, ay1 = ql_rect_bottom(area);

    /* ---- ① 实心区：距离 <= 0 ⇒ 全覆盖 ---- */
    {
        ql_coord_t fx0 = (sx0 > ax0) ? sx0 : ax0;
        ql_coord_t fx1 = (sx1 < ax1) ? sx1 : ax1;
        ql_coord_t fy0 = (sy0 > ay0) ? sy0 : ay0;
        ql_coord_t fy1 = (sy1 < ay1) ? sy1 : ay1;
        if (fx1 > fx0 && fy1 > fy0) {
            if (fx0 == sx0 && fx1 == sx1 && fy0 == sy0 && fy1 == sy1) {
                /* 常见情形：实心区整块可见，交给通用圆角填充（同一条按行跨度路径） */
                ql_canvas_fill_round_rect_alpha(canvas, shaped, rad, color, alpha);
            } else if (rad > 0) {
                /* 圆角区不可整块填：按行取跨度 */
                for (ql_coord_t y = fy0; y < fy1; y++) {
                    ql_coord_t rx0 = 0, rx1 = 0;
                    if (!round_row_span(shaped, y, rad, &rx0, &rx1))
                        continue;
                    if (rx0 < fx0) rx0 = fx0;
                    if (rx1 > fx1 - 1) rx1 = fx1 - 1;
                    if (rx1 >= rx0)
                        fill_row(canvas, rx0, rx1, y, color, alpha);
                }
            } else {
                for (ql_coord_t y = fy0; y < fy1; y++)
                    fill_row(canvas, fx0, fx1 - 1, y, color, alpha);
            }
        }
    }

    /* ---- ② 上 / 下边带：距离只取决于 y ⇒ 整行同色 ---- */
    /* 直边段 = 轮廓横向去掉两端的圆角 */
    ql_coord_t strip0 = sx0 + rad;
    ql_coord_t strip1 = sx1 - rad;

    for (int side = 0; side < 2; side++) {
        ql_coord_t y_begin = (side == 0) ? ay0 : sy1;
        ql_coord_t y_end   = (side == 0) ? ((sy0 < ay1) ? sy0 : ay1)
                                         : ((outer.y + outer.h < ay1) ? ql_rect_bottom(outer)
                                                                      : ay1);
        if (side == 0 && sy0 <= ay0)
            continue;      /* 上边带不在可见区域内 */
        if (side == 1 && sy1 >= ay1)
            continue;

        for (ql_coord_t y = y_begin; y < y_end; y++) {
            float dyv = (side == 0) ? ((float)sy0 - ((float)y + 0.5f))
                                    : (((float)y + 0.5f) - (float)sy1);
            uint8_t cov = shadow_cov_u8(dyv, fblur);
            if (cov == 0)
                continue;
            uint32_t a = (uint32_t)cov * (uint32_t)alpha / 255u;
            if (a == 0)
                continue;
            ql_coord_t x0 = (strip0 > ax0) ? strip0 : ax0;
            ql_coord_t x1 = (strip1 < ax1) ? strip1 : ax1;
            if (x1 > x0)
                fill_row(canvas, x0, x1 - 1, y, color, (uint8_t)a);
        }
    }

    /* ---- ③ 左 / 右边带：距离只取决于 x ⇒ 整列同色 ---- */
    ql_coord_t vstrip0 = sy0 + rad;
    ql_coord_t vstrip1 = sy1 - rad;

    for (int side = 0; side < 2; side++) {
        ql_coord_t x_begin = (side == 0) ? ax0 : sx1;
        ql_coord_t x_end   = (side == 0) ? ((sx0 < ax1) ? sx0 : ax1)
                                         : ((ql_rect_right(outer) < ax1)
                                                ? ql_rect_right(outer) : ax1);
        if (side == 0 && sx0 <= ax0)
            continue;
        if (side == 1 && sx1 >= ax1)
            continue;

        for (ql_coord_t x = x_begin; x < x_end; x++) {
            float dxv = (side == 0) ? ((float)sx0 - ((float)x + 0.5f))
                                    : (((float)x + 0.5f) - (float)sx1);
            uint8_t cov = shadow_cov_u8(dxv, fblur);
            if (cov == 0)
                continue;
            uint32_t a = (uint32_t)cov * (uint32_t)alpha / 255u;
            if (a == 0)
                continue;
            ql_coord_t y0 = (vstrip0 > ay0) ? vstrip0 : ay0;
            ql_coord_t y1 = (vstrip1 < ay1) ? vstrip1 : ay1;
            if (y1 > y0)
                fill_col(canvas, x, y0, y1 - 1, color, (uint8_t)a);
        }
    }

    /* ---- ④ 四个角区：唯一的逐像素 sqrt ---- */
    /*
     * fblur == 0 时角外的覆盖率恒为 0（硬边阴影），整块跳过。
     *
     * rad == 0 也必须走这里：此时上/下边带只覆盖 [sx0, sx1)、左/右边带只覆盖
     * [sy0, sy1)，四个 pad×pad 的方角没有任何区域负责，会留白。
     *
     * 角块的横向范围是 {qx > 0} = 距左右边 rad 以内再加外面 pad 的余量，
     * 即 [sx0-pad, sx0+rad) 与 [sx1-rad, sx1+pad)。右侧必须以 outer 的右边界
     * 为锚，否则会与直边带重叠（同一点混两次、偏黑）并漏掉最外侧 pad 条带。
     *
     * 四个角互为镜像：覆盖率只取决于"到圆角圆心的相对偏移 (qx, qy)"，
     * 所以每个 (qx, qy) 只算一次，然后写入四个镜像位置。
     * 角内 qx、qy 恒为正（最小的 i=j=span-1 时仍为 0.5），钳零分支可留作保险。
     */
    if (fblur > 0.0f) {
        const ql_coord_t span = rad + pad;
        const ql_coord_t lx = sx0 - pad;
        const ql_coord_t rx = ql_rect_right(outer) - span;
        const ql_coord_t ty = sy0 - pad;
        const ql_coord_t byy = ql_rect_bottom(outer) - span;
        const float inv_blur = 1.0f / fblur;
        const uint32_t cr = QL_COLOR_GET_R(color);
        const uint32_t cg = QL_COLOR_GET_G(color);
        const uint32_t cb = QL_COLOR_GET_B(color);
        const uint8_t fmt = canvas->format;
        /* 四块角全在可见区内时免去逐点判断（绝大多数情况） */
        const int whole = (ax0 <= lx && ax1 >= ql_rect_right(outer) &&
                           ay0 <= ty && ay1 >= ql_rect_bottom(outer));

        for (ql_coord_t j = 0; j < span; j++) {
            float qy = (float)pad - (float)j - 0.5f + frad;
            if (qy < 0.0f)
                qy = 0.0f;
            const float qy2 = qy * qy;
            const ql_coord_t yt = ty + j;                 /* 上侧角 */
            const ql_coord_t yb = byy + span - 1 - j;     /* 下侧角（镜像行） */
            const int yt_ok = whole || (yt >= ay0 && yt < ay1);
            const int yb_ok = whole || (yb >= ay0 && yb < ay1);
            if (!yt_ok && !yb_ok)
                continue;
            uint8_t *rt = yt_ok ? canvas->buffer + (uint32_t)yt * canvas->stride : NULL;
            uint8_t *rb = yb_ok ? canvas->buffer + (uint32_t)yb * canvas->stride : NULL;

            for (ql_coord_t i = 0; i < span; i++) {
                float qx = (float)pad - (float)i - 0.5f + frad;
                if (qx < 0.0f)
                    qx = 0.0f;
                float d = sqrtf(qx * qx + qy2) - frad;
                float t = d * inv_blur;
                uint32_t c8 = 0;
                if (d > 0.0f && t < 1.0f) {
                    float cov = 1.0f - (t * t * (3.0f - 2.0f * t));
                    c8 = (uint32_t)(cov * 255.0f + 0.5f);
                    if (c8 > 255u)
                        c8 = 255u;
                }
                uint32_t a = c8 * (uint32_t)alpha / 255u;
                if (a == 0)
                    continue;

                const ql_coord_t xl = lx + i;                 /* 左侧 */
                const ql_coord_t xr = rx + span - 1 - i;      /* 右侧（镜像列） */
                const int xl_ok = whole || (xl >= ax0 && xl < ax1);
                const int xr_ok = whole || (xr >= ax0 && xr < ax1);

                if (rt != NULL) {
                    if (xl_ok)
                        blend_px(rt, xl, fmt, cr, cg, cb, color, a);
                    if (xr_ok)
                        blend_px(rt, xr, fmt, cr, cg, cb, color, a);
                }
                if (rb != NULL) {
                    if (xl_ok)
                        blend_px(rb, xl, fmt, cr, cg, cb, color, a);
                    if (xr_ok)
                        blend_px(rb, xr, fmt, cr, cg, cb, color, a);
                }
            }
        }
    }
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

    const uint8_t fmt = canvas->format;
    const uint32_t cr = QL_COLOR_GET_R(color);
    const uint32_t cg = QL_COLOR_GET_G(color);
    const uint32_t cb = QL_COLOR_GET_B(color);

    for (ql_coord_t gy = gy0; gy <= gy1; gy++) {
        /* 行指针由字体提供：静态位图字体与动态矢量字体的取法不同 */
        const uint8_t *src = ql_font_glyph_row(font, glyph, gy);
        if (src == NULL)
            break;
        ql_coord_t y = origin_y + gy;
        /* 行指针提到行外：字形逐像素写，原来每像素重算一次 buffer + y*stride */
        uint8_t *row = canvas->buffer + (uint32_t)y * canvas->stride;

        switch (font->bpp) {
        case 4:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 2u];
                uint8_t nib = (gx % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
                if (nib != 0)
                    blend_px(row, origin_x + gx, fmt, cr, cg, cb, color,
                             (uint32_t)nib * 17u);
            }
            break;
        case 1:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 8u];
                if (byte & (0x80u >> (gx % 8)))
                    blend_px(row, origin_x + gx, fmt, cr, cg, cb, color, 255u);
            }
            break;
        default:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t a = src[gx];
                if (a != 0)
                    blend_px(row, origin_x + gx, fmt, cr, cg, cb, color, a);
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
