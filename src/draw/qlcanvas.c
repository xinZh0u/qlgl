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
    canvas->scale_fx = (driver->scale_fx > 0) ? driver->scale_fx : QL_SCALE_ONE;
    canvas->clip = ql_rect_intersect(clip, ql_rect_make(0, 0, driver->width, driver->height));
    canvas->mask_r = -1;      /* 默认关闭（memset 是 0，那会被当成"半径 0 = 全裁"） */
    return QL_OK;
}

void ql_canvas_set_scale(QLCanvas *canvas, ql_coord_t scale_fx)
{
    if (canvas != NULL)
        canvas->scale_fx = (scale_fx > 0) ? scale_fx : QL_SCALE_ONE;
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
/* ------------------------------ 圆遮罩 ------------------------------ */

void ql_canvas_set_mask(QLCanvas *canvas, ql_coord_t cx, ql_coord_t cy, ql_coord_t r)
{
    if (canvas == NULL)
        return;
    if (r < 0) {
        canvas->mask_r = -1;
        return;
    }
    canvas->mask_cx = cx;
    canvas->mask_cy = cy;
    canvas->mask_r = r;
}

int ql_canvas_mask_active(const QLCanvas *canvas)
{
    return canvas != NULL && canvas->mask_r >= 0;
}

/* ------------------------------ 毛玻璃（backdrop blur） ------------------------------ */

/* 原始像素读写：绕过 clip / mask（模糊的是"已经画好的东西"，不该再被裁一次） */
static inline void raw_px_get(const QLCanvas *c, ql_coord_t x, ql_coord_t y,
                              uint8_t *rgb)
{
    const uint8_t *row = c->buffer + (uint32_t)y * c->stride;
    switch (c->format) {
    case QL_COLOR_RGB565: {
        const uint16_t *p = (const uint16_t *)(const void *)(row + (uint32_t)x * 2u);
        uint16_t v = *p;
        rgb[0] = (uint8_t)(((uint32_t)((v >> 11) & 0x1Fu) * 255u) / 31u);
        rgb[1] = (uint8_t)(((uint32_t)((v >> 5) & 0x3Fu) * 255u) / 63u);
        rgb[2] = (uint8_t)(((uint32_t)(v & 0x1Fu) * 255u) / 31u);
        break;
    }
    case QL_COLOR_RGB888: {
        const uint8_t *p = row + (uint32_t)x * 3u;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
        break;
    }
    default: {
        const uint8_t *p = row + (uint32_t)x * 4u;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
        break;
    }
    }
}

static inline void raw_px_set(QLCanvas *c, ql_coord_t x, ql_coord_t y,
                              const uint8_t *rgb)
{
    uint8_t *row = c->buffer + (uint32_t)y * c->stride;
    switch (c->format) {
    case QL_COLOR_RGB565: {
        uint16_t *p = (uint16_t *)(void *)(row + (uint32_t)x * 2u);
        *p = (uint16_t)(((uint32_t)(rgb[0] & 0xF8u) << 8) |
                        ((uint32_t)(rgb[1] & 0xFCu) << 3) | ((uint32_t)rgb[2] >> 3));
        break;
    }
    case QL_COLOR_RGB888: {
        uint8_t *p = row + (uint32_t)x * 3u;
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
        break;
    }
    default: {
        uint8_t *p = row + (uint32_t)x * 4u;
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
        break;
    }
    }
}

/*
 * 可分离盒式卷积的一趟：src → dst，`dir` 为 1 走水平、0 走垂直。
 * 用滑动和（每个像素 O(1)），边缘钳位（不采样区域外）。
 */
static void box_pass(const uint8_t *src, uint8_t *dst, int w, int h, int r, int dir)
{
    const int n = dir ? w : h;      /* 这一趟的长度 */
    const int m = dir ? h : w;      /* 有多少条 */
    const size_t step = dir ? 3u : (size_t)3u * (size_t)w;   /* 同一条上相邻元素的距离 */
    const size_t line = dir ? (size_t)3u * (size_t)w : 3u;   /* 换一条线的距离 */

    const uint32_t win = (uint32_t)(2 * r + 1);

    for (int j = 0; j < m; j++) {
        const uint8_t *s0 = src + (size_t)j * line;
        uint8_t *d0 = dst + (size_t)j * line;
        uint32_t sum[3] = { 0, 0, 0 };

        /* 初值：把 [-r, r] 加上（越界钳到端点） */
        for (int k = -r; k <= r; k++) {
            int idx = k < 0 ? 0 : (k >= n ? n - 1 : k);
            const uint8_t *p = s0 + (size_t)idx * step;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
        }

        for (int i = 0; i < n; i++) {
            uint8_t *d = d0 + (size_t)i * step;
            d[0] = (uint8_t)(sum[0] / win);
            d[1] = (uint8_t)(sum[1] / win);
            d[2] = (uint8_t)(sum[2] / win);

            /* 滑动：去掉左边、加上右边（两端钳位） */
            int out = i - r;
            int in = i + r + 1;
            out = out < 0 ? 0 : (out >= n ? n - 1 : out);
            in = in < 0 ? 0 : (in >= n ? n - 1 : in);
            const uint8_t *po = s0 + (size_t)out * step;
            const uint8_t *pi = s0 + (size_t)in * step;
            sum[0] = sum[0] + pi[0] - po[0];
            sum[1] = sum[1] + pi[1] - po[1];
            sum[2] = sum[2] + pi[2] - po[2];
        }
    }
}

ql_err_t ql_canvas_backdrop_blur(QLCanvas *canvas, QLRect rect, int radius)
{
    if (canvas == NULL || canvas->buffer == NULL)
        return QL_ERR_INVAL;
    if (radius <= 0)
        return QL_OK;

    QLRect r = ql_rect_intersect(rect, ql_rect_make(0, 0, canvas->width, canvas->height));
    if (ql_rect_is_empty(r))
        return QL_OK;
    if (radius > r.w + r.h)
        radius = (int)(r.w + r.h);   /* 大半径没有更多信息，别白扫 */

    const int w = (int)r.w, h = (int)r.h;
    const size_t bytes = (size_t)w * (size_t)h * 3u;
    uint8_t *a = (uint8_t *)ql_mem_alloc(bytes);
    uint8_t *b = (uint8_t *)ql_mem_alloc(bytes);
    if (a == NULL || b == NULL) {
        ql_mem_free(a);
        ql_mem_free(b);
        return QL_ERR_NOMEM;
    }

    /* 取出来（去掉 padding，变成紧凑的 RGB 三元组） */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            raw_px_get(canvas, r.x + x, r.y + y,
                       a + ((size_t)y * (size_t)w + (size_t)x) * 3u);

    /* 两趟 (H,V) 近似高斯：一次盒式卷积是方的，两次叠加接近钟形 */
    for (int it = 0; it < 2; it++) {
        box_pass(a, b, w, h, radius, 1);
        box_pass(b, a, w, h, radius, 0);
    }

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            raw_px_set(canvas, r.x + x, r.y + y,
                       a + ((size_t)y * (size_t)w + (size_t)x) * 3u);

    ql_mem_free(a);
    ql_mem_free(b);
    return QL_OK;
}

/* smoothstep，与 GPU 片元着色器同一公式，保证两条后端视觉一致 */
static inline float ql_smoothstep(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* 单个像素的圆覆盖率 0..255（无遮罩时恒 255） */
static inline uint32_t mask_cov_at(const QLCanvas *c, ql_coord_t x, ql_coord_t y)
{
    if (c->mask_r < 0)
        return 255u;
    if (c->mask_r == 0)
        return 0u;              /* 半径 0：整屏被裁掉 */

    float dx = ((float)x + 0.5f) - (float)c->mask_cx;
    float dy = ((float)y + 0.5f) - (float)c->mask_cy;
    float d = sqrtf(dx * dx + dy * dy) - (float)c->mask_r;
    /* d < -0.5 完全在内；d > 0.5 完全在外 */
    float cov = 1.0f - ql_smoothstep(-0.5f, 0.5f, d);
    return (uint32_t)(cov * 255.0f + 0.5f);
}

/* 某一行被圆覆盖的完整区间（不含抗锯齿边界）；无交返回 0 */
static int mask_row_span(const QLCanvas *c, ql_coord_t y, ql_coord_t *lo,
                         ql_coord_t *hi)
{
    float dy = ((float)y + 0.5f) - (float)c->mask_cy;
    float r = (float)c->mask_r;
    if (dy <= -r - 0.5f || dy >= r + 0.5f)
        return 0;

    /* 留 0.5px 给边缘做覆盖率，内层走整行填充 */
    float rr = r - 0.5f;
    if (rr < 0.0f)
        rr = 0.0f;
    float span2 = rr * rr - dy * dy;
    if (span2 < 0.0f)
        span2 = 0.0f;
    float half = sqrtf(span2);

    float f0 = (float)c->mask_cx - half;
    float f1 = (float)c->mask_cx + half;
    *lo = (ql_coord_t)ceilf(f0);
    *hi = (ql_coord_t)floorf(f1);
    if (*hi < *lo) {
        /* 只覆盖到一个像素的一部分：交给边界处理 */
        *lo = (ql_coord_t)floorf(f0);
        *hi = *lo - 1;
    }
    return 1;
}

static inline void put_px(QLCanvas *c, ql_coord_t x, ql_coord_t y,
                          ql_color_t color, uint8_t alpha)
{
    if (c->mask_r >= 0 && alpha > 0) {
        uint32_t cov = mask_cov_at(c, x, y);
        if (cov == 0)
            return;
        if (cov < 255)
            alpha = (uint8_t)(((uint32_t)alpha * cov) / 255u);
    }
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
                     ql_color_t color, uint8_t alpha);

static void fill_row_raw(QLCanvas *c, ql_coord_t x0, ql_coord_t x1, ql_coord_t y,
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
    if (dir != QL_GRADIENT_RADIAL && span <= 1) {
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
        } else if (dir == QL_GRADIENT_HORIZONTAL) {
            for (ql_coord_t x = sx0; x <= sx1; x++) {
                uint32_t t = (uint32_t)((x - rect.x) * 255) / (uint32_t)(span - 1);
                put_px(canvas, x, y, lerp_color(from, to, t), alpha);
            }
        } else {
            /*
             * 径向：t 由"到中心的距离 / 到最远角的距离"决定。
             * 逐像素算 hypot 太贵，用增量法：(x-cx)² 相邻两点差 2*(x-cx)+1。
             */
            const float cx = (float)rect.x + (float)rect.w * 0.5f;
            const float cy = (float)rect.y + (float)rect.h * 0.5f;
            const float half_w = (float)rect.w * 0.5f;
            const float half_h = (float)rect.h * 0.5f;
            const float d_max = sqrtf(half_w * half_w + half_h * half_h);
            const float dy = ((float)y + 0.5f) - cy;
            const float dy2 = dy * dy;

            float dx = ((float)sx0 + 0.5f) - cx;
            float dx2 = dx * dx;
            for (ql_coord_t x = sx0; x <= sx1; x++) {
                float t = sqrtf(dx2 + dy2) / d_max;
                if (t > 1.0f)
                    t = 1.0f;
                put_px(canvas, x, y, lerp_color(from, to, (uint32_t)(t * 255.0f)),
                       alpha);
                dx2 += 2.0f * dx + 1.0f;
                dx += 1.0f;
            }
        }
    }
}

/*
 * 单像素混合写（行指针已给）。颜色分量与格式预先取好，
 * 供阴影角区那种"一次算覆盖率、写四个镜像位置"的循环使用。
 */
/*
 * 整行填充 + 圆遮罩。
 * 内层交给 fill_row_raw（SIMD 快路径），两端的半覆盖像素用 put_px 各来一次，
 * 所以一个带遮罩的矩形只比不带多两次单像素混合 + 每行一次 sqrt。
 */
static void fill_row(QLCanvas *c, ql_coord_t x0, ql_coord_t x1, ql_coord_t y,
                     ql_color_t color, uint8_t alpha)
{
    if (x1 < x0)
        return;
    if (c->mask_r < 0) {
        fill_row_raw(c, x0, x1, y, color, alpha);
        return;
    }

    ql_coord_t lo = 0, hi = 0;
    if (!mask_row_span(c, y, &lo, &hi))
        return;

    /* 两端各留一个像素给覆盖率 */
    ql_coord_t full0 = lo;
    ql_coord_t full1 = hi;
    if (full1 >= full0) {
        if (full0 <= x1 && full1 >= x0)
            fill_row_raw(c, full0 > x0 ? full0 : x0, full1 < x1 ? full1 : x1, y,
                         color, alpha);
        /* 左边界像素 */
        if (full0 - 1 >= x0 && full0 - 1 <= x1)
            put_px(c, full0 - 1, y, color, alpha);
        /* 右边界像素 */
        if (full1 + 1 >= x0 && full1 + 1 <= x1)
            put_px(c, full1 + 1, y, color, alpha);
    } else {
        /* 这一行只被切到一点点：交给逐像素（put_px 自己会算覆盖率） */
        ql_coord_t a = x0 > lo ? x0 : lo;
        ql_coord_t b = x1 < hi ? x1 : hi;
        for (ql_coord_t x = a; x <= b; x++)
            put_px(c, x, y, color, alpha);
    }
}

static inline void blend_px(uint8_t *row, ql_coord_t x, uint8_t fmt, uint32_t cr,
                            uint32_t cg, uint32_t cb, ql_color_t color, uint32_t a);

/* 带圆遮罩的 blend：alpha 先乘上该像素的圆覆盖率 */
static inline void blend_px_masked(const QLCanvas *c, uint8_t *row, ql_coord_t x,
                                   ql_coord_t y, uint8_t fmt, uint32_t cr,
                                   uint32_t cg, uint32_t cb, ql_color_t color,
                                   uint32_t a)
{
    if (c != NULL && c->mask_r >= 0) {
        uint32_t cov = mask_cov_at(c, x, y);
        if (cov == 0)
            return;
        a = a * cov / 255u;
        if (a == 0)
            return;
    }
    blend_px(row, x, fmt, cr, cg, cb, color, a);
}

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
        /*
         * 起点必须与裁剪求交：下边带的自然起点是 sy1，它在裁剪上界之下时
         * （比如阴影被 overflow:hidden 或脏区裁掉上半部分）会往上画到裁剪外面。
         */
        ql_coord_t y_begin = (side == 0) ? ay0 : ((sy1 > ay0) ? sy1 : ay0);
        ql_coord_t y_end   = (side == 0) ? ((sy0 < ay1) ? sy0 : ay1)
                                         : ((ql_rect_bottom(outer) < ay1)
                                                ? ql_rect_bottom(outer) : ay1);
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
        /* 同 ②：右边带的自然起点 sx1 也要与裁剪求交 */
        ql_coord_t x_begin = (side == 0) ? ax0 : ((sx1 > ax0) ? sx1 : ax0);
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
                        blend_px_masked(canvas, rt, xl, ty + j, fmt, cr, cg, cb,
                                        color, a);
                    if (xr_ok)
                        blend_px_masked(canvas, rt, xr, ty + j, fmt, cr, cg, cb,
                                        color, a);
                }
                if (rb != NULL) {
                    if (xl_ok)
                        blend_px_masked(canvas, rb, xl, byy + j, fmt, cr, cg, cb,
                                        color, a);
                    if (xr_ok)
                        blend_px_masked(canvas, rb, xr, byy + j, fmt, cr, cg, cb,
                                        color, a);
                }
            }
        }
    }
}

/* ---------------------- 亚像素（26.6）填充：面积覆盖率 ---------------------- */

/* 两个区间的重叠长度，结果 0..64 */
static inline ql_coord_t ovl(ql_coord_t a0, ql_coord_t a1, ql_coord_t b0, ql_coord_t b1)
{
    ql_coord_t lo = (a0 > b0) ? a0 : b0;
    ql_coord_t hi = (a1 < b1) ? a1 : b1;
    return (hi > lo) ? (hi - lo) : 0;
}

/*
 * 填一行里的一段 fx 区间 [fx0, fx1)。
 *
 * 两端像素按覆盖率混合，中间整像素走 fill_row（SIMD 快路径）。
 * 所以一个带小数的矩形只比整数版本多两次 put_px，不是逐像素。
 */
static void fill_span_fx(QLCanvas *c, ql_coord_t y, ql_coord_t fx0, ql_coord_t fx1,
                         ql_color_t color, uint32_t arow)
{
    if (fx1 <= fx0 || arow == 0)
        return;

    /* 横向先与裁剪求交：fill_row 不做边界检查（调用方保证） */
    const ql_coord_t cx0 = ql_fx_of(c->clip.x);
    const ql_coord_t cx1 = ql_fx_of(ql_rect_right(c->clip));
    if (fx0 < cx0)
        fx0 = cx0;
    if (fx1 > cx1)
        fx1 = cx1;
    if (fx1 <= fx0)
        return;

    const ql_coord_t full0 = (fx0 + QL_FX_ONE - 1) >> QL_FX_SHIFT;   /* 首个整像素 */
    const ql_coord_t full1 = fx1 >> QL_FX_SHIFT;                    /* 整像素半开上界 */

    if (full1 > full0) {
        if ((fx0 & (QL_FX_ONE - 1)) != 0) {
            uint32_t cov = (uint32_t)((ql_fx_of(full0)) - fx0);
            put_px(c, full0 - 1, y, color, (uint8_t)(arow * cov / 64u));
        }
        fill_row(c, full0, full1 - 1, y, color, (uint8_t)arow);
        uint32_t tail = (uint32_t)(fx1 & (QL_FX_ONE - 1));
        if (tail != 0)
            put_px(c, full1, y, color, (uint8_t)(arow * tail / 64u));
        return;
    }

    /* 不足一个整像素（细边、或落在像素边界上）：只能逐像素算 */
    ql_coord_t x0 = fx0 >> QL_FX_SHIFT;
    ql_coord_t x1 = (fx1 - 1) >> QL_FX_SHIFT;
    for (ql_coord_t x = x0; x <= x1; x++) {
        ql_coord_t cov = ovl(ql_fx_of(x), (ql_fx_of(x)) + QL_FX_ONE, fx0, fx1);
        if (cov > 0)
            put_px(c, x, y, color, (uint8_t)(arow * (uint32_t)cov / 64u));
    }
}

/*
 * 圆角矩形某一行的 fx 跨度。
 *
 * **刻意复用整数版 round_row_span**（在取整后的矩形上算），再把得到的整像素跨度
 * 整体平移亚像素余量。理由：
 *   · 中心采样式（直接用 SDF）算出来的圆角比整数版大将近 2px，
 *     动画跨过 0 时会出现"拐角突然胀一圈"的跳变；
 *   · 用整数跨度保证对齐（低 6 位为 0）时与整数路径**逐像素相同**，
 *     这就是这次改造的安全阀。
 */
static int round_span_fx(QLRectFx r, ql_coord_t rad_fx, ql_coord_t y, ql_coord_t *ox0,
                         ql_coord_t *ox1)
{
    const QLRect ir = ql_rect_make(r.x >> QL_FX_SHIFT, r.y >> QL_FX_SHIFT,
                                   ((r.x + r.w + QL_FX_ONE - 1) >> QL_FX_SHIFT) -
                                       (r.x >> QL_FX_SHIFT),
                                   ((r.y + r.h + QL_FX_ONE - 1) >> QL_FX_SHIFT) -
                                       (r.y >> QL_FX_SHIFT));
    const ql_coord_t rad = (rad_fx + (QL_FX_ONE / 2)) >> QL_FX_SHIFT;

    ql_coord_t sx0 = 0, sx1 = 0;
    if (!round_row_span(ir, y, rad, &sx0, &sx1))
        return 0;

    const ql_coord_t fxoff = r.x & (QL_FX_ONE - 1);
    ql_coord_t fx0 = (ql_fx_of(sx0)) + fxoff;
    ql_coord_t fx1 = (ql_fx_of(sx1 + 1)) + fxoff;
    if (fx0 < r.x)
        fx0 = r.x;
    if (fx1 > r.x + r.w)
        fx1 = r.x + r.w;
    if (fx1 <= fx0)
        return 0;
    *ox0 = fx0;
    *ox1 = fx1;
    return 1;
}

/* 统一的 fx 填充内核：rad_fx == 0 时每行跨度就是整条矩形 */
static void fill_round_fx(QLCanvas *c, QLRectFx r, ql_coord_t rad_fx, ql_color_t color,
                          uint8_t alpha)
{
    if (c == NULL || alpha == 0 || r.w <= 0 || r.h <= 0)
        return;

    if (rad_fx > 0) {
        ql_coord_t lim = (r.w < r.h ? r.w : r.h) / 2;
        if (rad_fx > lim)
            rad_fx = lim;
    }

    ql_coord_t iy0 = r.y >> QL_FX_SHIFT;
    ql_coord_t iy1 = (r.y + r.h + QL_FX_ONE - 1) >> QL_FX_SHIFT;
    if (iy0 < c->clip.y)
        iy0 = c->clip.y;
    if (iy1 > ql_rect_bottom(c->clip))
        iy1 = ql_rect_bottom(c->clip);

    for (ql_coord_t y = iy0; y < iy1; y++) {
        ql_coord_t cov = ovl(ql_fx_of(y), (ql_fx_of(y)) + QL_FX_ONE, r.y,
                             r.y + r.h);
        if (cov == 0)
            continue;
        uint32_t ay = (uint32_t)alpha * (uint32_t)cov / 64u;
        if (ay == 0)
            continue;

        if (rad_fx <= 0) {
            fill_span_fx(c, y, r.x, r.x + r.w, color, ay);
            continue;
        }
        ql_coord_t sx0 = 0, sx1 = 0;
        if (!round_span_fx(r, rad_fx, y, &sx0, &sx1))
            continue;
        fill_span_fx(c, y, sx0, sx1, color, ay);
    }
}

void ql_canvas_fill_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_color_t color,
                            uint8_t alpha)
{
    fill_round_fx(canvas, rect, 0, color, alpha);
}

void ql_canvas_fill_round_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_coord_t radius_fx,
                                  ql_color_t color, uint8_t alpha)
{
    fill_round_fx(canvas, rect, radius_fx, color, alpha);
}

/*
 * 描边环（fx）：每行 = 外跨度减去内跨度。
 * 与整数版本同构，只是端点带小数 → 两边都有覆盖率。
 */
void ql_canvas_stroke_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_coord_t radius_fx,
                              ql_coord_t thickness_fx, ql_color_t color)
{
    if (canvas == NULL || thickness_fx <= 0 || rect.w <= 0 || rect.h <= 0)
        return;

    /* 上限与整数版逐字一致（整数除法，所以 1px 宽的框直接被判为"画不下"） */
    const ql_coord_t lim_w = ql_fx_of((rect.w >> QL_FX_SHIFT) / 2);
    const ql_coord_t lim_h = ql_fx_of((rect.h >> QL_FX_SHIFT) / 2);
    if (thickness_fx > lim_w)
        thickness_fx = lim_w;
    if (thickness_fx > lim_h)
        thickness_fx = lim_h;
    if (thickness_fx <= 0)
        return;

    const QLRectFx inner = ql_rectfx_make(rect.x + thickness_fx, rect.y + thickness_fx,
                                          rect.w - 2 * thickness_fx,
                                          rect.h - 2 * thickness_fx);

    ql_coord_t iy0 = rect.y >> QL_FX_SHIFT;
    ql_coord_t iy1 = (rect.y + rect.h + QL_FX_ONE - 1) >> QL_FX_SHIFT;
    if (iy0 < canvas->clip.y)
        iy0 = canvas->clip.y;
    if (iy1 > ql_rect_bottom(canvas->clip))
        iy1 = ql_rect_bottom(canvas->clip);

    for (ql_coord_t y = iy0; y < iy1; y++) {
        ql_coord_t cov = ovl(ql_fx_of(y), (ql_fx_of(y)) + QL_FX_ONE, rect.y,
                             rect.y + rect.h);
        if (cov == 0)
            continue;
        uint32_t ay = 255u * (uint32_t)cov / 64u;

        ql_coord_t ox0 = 0, ox1 = 0;
        if (!round_span_fx(rect, radius_fx, y, &ox0, &ox1))
            continue;

        ql_coord_t ix0 = 0, ix1 = 0;
        if (round_span_fx(inner, radius_fx > thickness_fx ? radius_fx - thickness_fx : 0, y,
                          &ix0, &ix1)) {
            fill_span_fx(canvas, y, ox0, ix0 < ox1 ? ix0 : ox1, color, ay);
            fill_span_fx(canvas, y, ix1 > ox0 ? ix1 : ox0, ox1, color, ay);
        } else {
            fill_span_fx(canvas, y, ox0, ox1, color, ay);
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

/*
 * 合成斜体的斜率：整字高度上的水平位移 = 高度 × 21/100（FreeType 默认 oblique 的量级）。
 *
 * 精度用 1/16 像素而不是整数像素：9px 高的小字整数剪切只有 1px，
 * 分摊到 9 行里几乎看不出来（每行要么 0 要么 1）。用 1/16 定点后
 * 每行都能分到不同的位移，视觉上才像"斜"。
 */
#define QL_GLYPH_SLANT_NUM 21
#define QL_GLYPH_SLANT_DEN 100
#define QL_GLYPH_SHEAR_SUB 16

/*
 * 真正的逐像素实现。`shear` = 顶部相对底部右移多少像素（0 = 不斜）；
 * `strike` = 加粗时这一遍的额外横移（0 = 主笔画，1 = 加粗补的第二遍）。
 *
 * 剪切在**行循环内部**做：每一行的起点右移 row_shear，行内像素照旧连续写，
 * 所以不会退化成逐像素随机访问。
 */
static void glyph_blit(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                       ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                       ql_coord_t shear, ql_coord_t strike)
{
    if (canvas == NULL || font == NULL || glyph == NULL)
        return;
    if (glyph->box_w == 0 || glyph->box_h == 0)
        return;

    ql_coord_t origin_x = pen_x + glyph->ofs_x + strike;
    ql_coord_t origin_y = baseline_y + glyph->ofs_y;

    /*
     * 斜体让顶部外扩，横向可见范围要放宽 shear 个像素，否则右上方会被裁掉。
     * 这里把 origin_x 临时左移 shear 做统一的裁剪计算，行内再加回来。
     */
    const ql_coord_t shear_px = (shear > 0) ? (shear / QL_GLYPH_SHEAR_SUB) : 0;
    const ql_coord_t clip_x0 = origin_x - shear_px;

    ql_coord_t gx0 = 0, gx1 = (ql_coord_t)glyph->box_w - 1;
    if (clip_x0 < canvas->clip.x)
        gx0 = canvas->clip.x - clip_x0;
    if (clip_x0 + gx1 > ql_rect_right(canvas->clip) - 1)
        gx1 = ql_rect_right(canvas->clip) - 1 - clip_x0;
    if (clip_x0 < 0)
        gx0 = (gx0 > -clip_x0) ? gx0 : -clip_x0;
    if (clip_x0 + gx1 > canvas->width - 1)
        gx1 = canvas->width - 1 - clip_x0;
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
        const uint8_t *src = ql_font_glyph_row(font, glyph, gy);
        if (src == NULL)
            break;
        const ql_coord_t y = origin_y + gy;
        uint8_t *row = canvas->buffer + (uint32_t)y * canvas->stride;

        /*
         * 剪切：越靠上右移越多（gy 小 = 靠上）。
         * shear 是 1/16 像素单位的总位移，这里按行线性分摊并四舍五入回整像素：
         *     row = round( shear * (hh - gy) / (hh * 16) )
         */
        const ql_coord_t hh = (ql_coord_t)glyph->box_h - 1;
        ql_coord_t row_shear = 0;
        if (shear > 0 && hh > 0) {
            const int64_t num = (int64_t)shear * (int64_t)(hh - gy);
            const int64_t den = (int64_t)hh * QL_GLYPH_SHEAR_SUB;
            row_shear = (ql_coord_t)((num + den / 2) / den);
        }
        const ql_coord_t line_x0 = clip_x0 + row_shear;

        switch (font->bpp) {
        case 4:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 2u];
                uint8_t nib = (gx % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
                if (nib != 0)
                    blend_px(row, line_x0 + gx, fmt, cr, cg, cb, color,
                             (uint32_t)nib * 17u);
            }
            break;
        case 1:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t byte = src[(uint32_t)gx / 8u];
                if (byte & (0x80u >> (gx % 8)))
                    blend_px_masked(canvas, row, line_x0 + gx, y, fmt, cr, cg, cb, color,
                                    255u);
            }
            break;
        default:
            for (ql_coord_t gx = gx0; gx <= gx1; gx++) {
                uint8_t a = src[gx];
                if (a != 0)
                    blend_px_masked(canvas, row, line_x0 + gx, y, fmt, cr, cg, cb, color,
                                    a);
            }
            break;
        }
    }
}

/*
 * 水平 affine 版的字形绘制：**逐个目的像素反查源列**。
 *
 * 为什么不复用 glyph_blit：那条路径是"遍历源列往目的写"，只适用于恒等变换。
 * 压扁/镜像时一个目的列对应多个源列（或反过来），必须从目的端遍历，
 * 否则会出现空隙（放大）或锯齿空洞（压扁）。
 */
static void glyph_blit_xf(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                          ql_coord_t shear, ql_coord_t strike,
                          ql_coord_t x_fx, ql_coord_t y_fx,
                          ql_coord_t pivot_x, ql_coord_t pivot_y)
{
    if (canvas == NULL || font == NULL || glyph == NULL)
        return;
    if (glyph->box_w == 0 || glyph->box_h == 0 || x_fx == 0 || y_fx == 0)
        return;

    const ql_coord_t origin_x = pen_x + glyph->ofs_x + strike;
    const ql_coord_t origin_y = baseline_y + glyph->ofs_y;

    /* 源区间的两端映射到目的端，再归一化（镜像时顺序会反） */
    ql_coord_t dx0 = pivot_x + (ql_coord_t)((int64_t)(origin_x - pivot_x) * x_fx /
                                            QL_SCALE_ONE);
    ql_coord_t dx1 = pivot_x + (ql_coord_t)((int64_t)(origin_x + (ql_coord_t)glyph->box_w -
                                                      pivot_x) * x_fx / QL_SCALE_ONE);
    if (dx1 < dx0) {
        const ql_coord_t t = dx0;
        dx0 = dx1;
        dx1 = t;
    }
    if (dx1 <= dx0)
        dx1 = dx0 + 1;

    const ql_coord_t x_lo = (canvas->clip.x > 0) ? canvas->clip.x : 0;
    const ql_coord_t x_hi = ql_rect_right(canvas->clip) - 1;
    const ql_coord_t y_lo = (canvas->clip.y > 0) ? canvas->clip.y : 0;
    const ql_coord_t y_hi = ql_rect_bottom(canvas->clip) - 1;
    const ql_coord_t cw = canvas->width - 1;
    const ql_coord_t ch = canvas->height - 1;

    const uint8_t fmt = canvas->format;
    const uint32_t cr = QL_COLOR_GET_R(color);
    const uint32_t cg = QL_COLOR_GET_G(color);
    const uint32_t cb = QL_COLOR_GET_B(color);
    const ql_coord_t hh = (ql_coord_t)glyph->box_h - 1;

    /* 竖向同理：源行区间 → 目的行区间，再归一化 */
    ql_coord_t dy0 = pivot_y + (ql_coord_t)((int64_t)(origin_y - pivot_y) * y_fx /
                                            QL_SCALE_ONE);
    ql_coord_t dy1 = pivot_y + (ql_coord_t)((int64_t)(origin_y + (ql_coord_t)glyph->box_h -
                                                      pivot_y) * y_fx / QL_SCALE_ONE);
    if (dy1 < dy0) {
        const ql_coord_t ty = dy0;
        dy0 = dy1;
        dy1 = ty;
    }
    if (dy1 <= dy0)
        dy1 = dy0 + 1;

    for (ql_coord_t dy = dy0; dy < dy1; dy++) {
        const ql_coord_t y = dy;
        if (y < y_lo || y > y_hi || y > ch || y < 0)
            continue;
        /* 反查源行区间，取最大 alpha（理由同横向） */
        const ql_coord_t r_lo = pivot_y +
                                (ql_coord_t)(((int64_t)(dy - pivot_y) * QL_SCALE_ONE) / y_fx);
        const ql_coord_t r_hi = pivot_y +
                                (ql_coord_t)(((int64_t)(dy + 1 - pivot_y) * QL_SCALE_ONE) /
                                             y_fx);
        ql_coord_t r0 = ((r_lo < r_hi) ? r_lo : r_hi) - origin_y;
        ql_coord_t r1 = ((r_lo < r_hi) ? r_hi : r_lo) - origin_y;
        if (r1 <= r0)
            r1 = r0 + 1;
        if (r0 < 0)
            r0 = 0;
        if (r1 > (ql_coord_t)glyph->box_h)
            r1 = (ql_coord_t)glyph->box_h;
        if (r0 >= r1)
            continue;
        const ql_coord_t gy = r0;   /* 斜体分摊只用行号，取段首即可 */
        uint8_t *row = canvas->buffer + (uint32_t)y * canvas->stride;

        /* 斜体：越靠上右移越多（与 glyph_blit 同一套分摊） */
        ql_coord_t row_shear = 0;
        if (shear > 0 && hh > 0) {
            const int64_t num = (int64_t)shear * (int64_t)(hh - gy);
            const int64_t den = (int64_t)hh * QL_GLYPH_SHEAR_SUB;
            row_shear = (ql_coord_t)((num + den / 2) / den);
        }

        for (ql_coord_t dx = dx0; dx < dx1; dx++) {
            const ql_coord_t x = dx + row_shear;
            if (x < x_lo || x > x_hi || x > cw || x < 0)
                continue;
            /*
             * 一个目的列对应源上的一段（压扁时有好几个源列）。
             * 取这段的**最大** alpha 而不是抽样一个：
             * 抽样会把细笔画整条丢掉（"翻"字的横画在 scaleX(0.5) 下消失，
             * 视觉上像字被咬掉了）。取最大值保证笔画不会凭空不见。
             */
            const ql_coord_t s_lo = pivot_x +
                                    (ql_coord_t)(((int64_t)(dx - pivot_x) * QL_SCALE_ONE) /
                                                 x_fx);
            const ql_coord_t s_hi = pivot_x +
                                    (ql_coord_t)(((int64_t)(dx + 1 - pivot_x) *
                                                  QL_SCALE_ONE) / x_fx);
            ql_coord_t c0 = ((s_lo < s_hi) ? s_lo : s_hi) - origin_x;
            ql_coord_t c1 = ((s_lo < s_hi) ? s_hi : s_lo) - origin_x;
            if (c1 <= c0)
                c1 = c0 + 1;
            if (c0 < 0)
                c0 = 0;
            if (c1 > (ql_coord_t)glyph->box_w)
                c1 = (ql_coord_t)glyph->box_w;
            if (c0 >= c1)
                continue;

            uint32_t a = 0;
            for (ql_coord_t sr = r0; sr < r1 && a < 255u; sr++)
                for (ql_coord_t col = c0; col < c1; col++) {
                    const uint32_t v = ql_font_glyph_alpha(font, glyph, col, sr);
                    if (v > a)
                        a = v;
                }
            if (a == 0)
                continue;
            blend_px_masked(canvas, row, x, y, fmt, cr, cg, cb, color, a);
        }
    }
}

void ql_canvas_draw_glyph_xf2(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                              ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                              int italic, int bold,
                              ql_coord_t x_fx, ql_coord_t y_fx,
                              ql_coord_t pivot_x, ql_coord_t pivot_y)
{
    if (glyph == NULL)
        return;
    if (x_fx == QL_SCALE_ONE && y_fx == QL_SCALE_ONE) {
        ql_canvas_draw_glyph_ex(canvas, font, glyph, pen_x, baseline_y, color, italic,
                                bold);
        return;
    }
    ql_coord_t shear = 0;
    if (italic && glyph->box_h > 1)
        shear = (ql_coord_t)((int64_t)glyph->box_h * QL_GLYPH_SLANT_NUM *
                             QL_GLYPH_SHEAR_SUB / QL_GLYPH_SLANT_DEN);
    glyph_blit_xf(canvas, font, glyph, pen_x, baseline_y, color, shear, 0, x_fx, y_fx,
                  pivot_x, pivot_y);
    if (bold)
        glyph_blit_xf(canvas, font, glyph, pen_x, baseline_y, color, shear, 1, x_fx, y_fx,
                      pivot_x, pivot_y);
}

void ql_canvas_draw_glyph_ex(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                             ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                             int italic, int bold)
{
    if (glyph == NULL)
        return;
    ql_coord_t shear = 0;      /* 1/16 像素单位 */
    if (italic && glyph->box_h > 1)
        shear = (ql_coord_t)((int64_t)glyph->box_h * QL_GLYPH_SLANT_NUM *
                             QL_GLYPH_SHEAR_SUB / QL_GLYPH_SLANT_DEN);
    glyph_blit(canvas, font, glyph, pen_x, baseline_y, color, shear, 0);
    if (bold) {
        /* 第二遍右移 1px：先画斜体那份、再补一遍，两遍剪切一致 */
        glyph_blit(canvas, font, glyph, pen_x, baseline_y, color, shear, 1);
    }
}

void ql_canvas_draw_glyph(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color)
{
    ql_canvas_draw_glyph_ex(canvas, font, glyph, pen_x, baseline_y, color, 0, 0);
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

/* ------------------------------ 位图搬运 ------------------------------ */

/* 读源图一个像素（按它的像素格式）→ RGBA */
static void image_read_rgba(const QLImage *img, ql_coord_t x, ql_coord_t y,
                            uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    const uint8_t *row = img->px + (uint32_t)y * img->stride;

    switch (img->format) {
    case QL_IMAGE_RGB565: {
        uint16_t px;
        memcpy(&px, row + (uint32_t)x * 2u, 2);
        *r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
        *g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
        *b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
        *a = 255;
        break;
    }
    case QL_IMAGE_RGB888:
        *r = row[x * 3 + 0];
        *g = row[x * 3 + 1];
        *b = row[x * 3 + 2];
        *a = 255;
        break;
    case QL_IMAGE_RGBA8888:
        *r = row[x * 4 + 0];
        *g = row[x * 4 + 1];
        *b = row[x * 4 + 2];
        *a = row[x * 4 + 3];
        break;
    default:
        *r = *g = *b = row[x];
        *a = 255;
        break;
    }
}

/* 圆角矩形的单像素覆盖率（0..255）。radius == 0 时恒 255（省掉 SDF 的开销） */
static uint32_t round_cov_at(const QLRect box, ql_coord_t radius, ql_coord_t x,
                             ql_coord_t y)
{
    if (radius <= 0)
        return 255u;
    const ql_coord_t hx = box.w / 2;
    const ql_coord_t hy = box.h / 2;
    ql_coord_t r = radius;
    const ql_coord_t half_min = (hx < hy) ? hx : hy;
    if (r > half_min)
        r = half_min;

    /* 与 fill_round_rect 同一个 SDF：中心到"内缩矩形"的距离 - 半径 */
    const float px = ((float)x + 0.5f) - ((float)box.x + (float)hx);
    const float py = ((float)y + 0.5f) - ((float)box.y + (float)hy);
    const float bx = (float)hx - (float)r;
    const float by = (float)hy - (float)r;
    float qx = px < 0.0f ? -px : px;
    float qy = py < 0.0f ? -py : py;
    qx -= bx;
    qy -= by;
    float outside = 0.0f;
    if (qx > 0.0f || qy > 0.0f) {
        const float ex = qx > 0.0f ? qx : 0.0f;
        const float ey = qy > 0.0f ? qy : 0.0f;
        outside = sqrtf(ex * ex + ey * ey);
    }
    const float inner = (qx > qy ? qx : qy);
    float d = (inner < 0.0f ? inner : 0.0f) + outside - (float)r;
    const float cov = 1.0f - ql_smoothstep(-0.5f, 0.5f, d);
    if (cov <= 0.0f)
        return 0u;
    return (uint32_t)(cov * 255.0f + 0.5f);
}

void ql_canvas_blit_image(QLCanvas *canvas, const QLImage *img, QLRect dst,
                          const QLRect *src, ql_coord_t radius, uint8_t alpha)
{
    if (canvas == NULL || img == NULL || alpha == 0)
        return;
    if (!ql_image_validate(img))
        return;

    QLRect s = (src != NULL) ? *src : ql_rect_make(0, 0, img->w, img->h);
    s = ql_rect_intersect(s, ql_rect_make(0, 0, img->w, img->h));
    if (ql_rect_is_empty(s) || ql_rect_is_empty(dst))
        return;

    /* 与 clip 求交，跳过完全看不到的部分（整块图在屏幕外时这里是纯收益） */
    const QLRect box = ql_rect_intersect(dst, canvas->clip);
    const QLRect view = ql_rect_make(0, 0, canvas->width, canvas->height);
    const QLRect area = ql_rect_intersect(box, view);
    if (ql_rect_is_empty(area))
        return;

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        /*
         * 最近邻：把目标像素的中心映射回源坐标。整数运算，避免浮点差异
         * 让 CPU 与 GPU 的取样点在边界上分叉。
         */
        int32_t sy = s.y + (int32_t)(((int32_t)(y - dst.y) * 2 + 1) * s.h /
                                    (2 * dst.h));
        if (sy < s.y)
            sy = s.y;
        if (sy >= ql_rect_bottom(s))
            sy = ql_rect_bottom(s) - 1;

        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            int32_t sx2 = s.x + (int32_t)(((int32_t)(x - dst.x) * 2 + 1) * s.w /
                                         (2 * dst.w));
            if (sx2 < s.x)
                sx2 = s.x;
            if (sx2 >= ql_rect_right(s))
                sx2 = ql_rect_right(s) - 1;

            uint8_t r, g, b, a;
            image_read_rgba(img, (ql_coord_t)sx2, (ql_coord_t)sy, &r, &g, &b, &a);

            uint32_t cov = mask_cov_at(canvas, x, y);
            if (cov == 0u)
                continue;
            if (radius > 0) {
                const uint32_t rc = round_cov_at(area, radius, x, y);
                if (rc == 0u)
                    continue;
                cov = cov * rc / 255u;
            }
            uint32_t total = (uint32_t)a * (uint32_t)alpha * cov / (255u * 255u);
            if (total == 0u)
                continue;
            ql_canvas_write_pixel(canvas, x, y, QL_COLOR_RGB(r, g, b), (uint8_t)total);
        }
    }
}
