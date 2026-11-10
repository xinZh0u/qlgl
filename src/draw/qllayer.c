/**
 * @file    qllayer.c
 * @brief   离屏图层的像素运算（实现见 qllayer.h 的说明）。
 *
 * 全部是纯函数式的缓冲运算：输入一块预乘 RGBA，输出还是那一块。
 * 不做任何绘制、不碰 QLDraw2D —— 这样 GPU 那侧只要照着同一套公式写着色器，
 * 两边结果就能对上（golden 测试守着）。
 */
#include "qllayer.h"

#include <math.h>
#include <string.h>

#include "qlgl/qlmem.h"

#if !QL_FEAT_CSS_FILTER_LAYER
/*
 * 关掉能力包时这一整份编译成空：它只在离屏图层里用得上，
 * MCU 档位（`-DQL_FEAT_CSS_FILTER_LAYER=0`）连一个字节都不该占。
 * 调用方（qldraw2d.c）在同一个开关里，所以不会出现"调了但没实现"。
 */
typedef int ql_layer_disabled_t;
#else

/* ------------------------------ 小工具 ------------------------------ */

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/* 与 qlcanvas.c 的 round_box_cov 同一公式（那边是 static，这里是图层合成用） */
static inline float layer_smoothstep(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

uint32_t ql_layer_round_cov(QLRect box, ql_coord_t corner, ql_coord_t x, ql_coord_t y)
{
    if (corner <= 0)
        return 255u;
    const float hw = (float)box.w * 0.5f;
    const float hh = (float)box.h * 0.5f;
    float r = (float)corner;
    const float mxr = (hw < hh ? hw : hh);
    if (r > mxr)
        r = mxr;
    const float cx = (float)box.x + hw;
    const float cy = (float)box.y + hh;
    const float px = (float)x + 0.5f - cx;
    const float py = (float)y + 0.5f - cy;
    const float qx = fabsf(px) - hw + r;
    const float qy = fabsf(py) - hh + r;
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    float d = (qx > qy ? qx : qy);
    if (d > 0.0f)
        d = 0.0f;
    d += sqrtf(ax * ax + ay * ay) - r;
    const float cov = 1.0f - layer_smoothstep(-0.5f, 0.5f, d);
    return (uint32_t)(cov * 255.0f + 0.5f);
}

/* ------------------------------ 反解 alpha ------------------------------ */

void ql_layer_reconstruct(const uint8_t *black, const uint8_t *white, int w, int h,
                          uint8_t *rgba)
{
    if (black == NULL || white == NULL || rgba == NULL || w <= 0 || h <= 0)
        return;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *bp = black + i * 3u;
        const uint8_t *wp = white + i * 3u;
        /*
         * 三个通道给出的 alpha 理论上有同一个值（白-黑 = 255*(1-a)），
         * 但 8 位量化会让它们差 1~2，取平均把误差摊掉。
         */
        const int32_t d = ((int32_t)(wp[0] - bp[0]) + (int32_t)(wp[1] - bp[1]) +
                           (int32_t)(wp[2] - bp[2])) / 3;
        int32_t a = 255 - d;
        if (a < 0)
            a = 0;
        if (a > 255)
            a = 255;
        uint8_t *o = rgba + i * 4u;
        if (a == 0) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        /* 预乘色 = 黑底结果；反解出 alpha 之后把"越过 a 的部分"钳回去 */
        for (int c = 0; c < 3; c++) {
            int32_t v = bp[c];
            if (v > a)
                v = a;           /* 预乘色不能大于 alpha，否则合成时会溢出 */
            o[c] = (uint8_t)v;
        }
        o[3] = (uint8_t)a;
    }
}

/* ------------------------------ 模糊 ------------------------------ */

/*
 * 9 抽头可分离高斯的一趟（4 通道，预乘），**与 GL 后端逐项一致**：
 * 同一组权重、同一个"抽头间距 = 半径/2"、同样的边缘钳位。
 *
 * 为什么不用 CPU 侧的滑动和盒式模糊（毛玻璃那套）：那套是"两趟盒式 ≈ 钟形"，
 * 与 GL 的 9 抽头高斯形状不同 —— 同一份 CSS 在两条后端上会糊出不同宽度，
 * 而滤镜必须两端一致（golden / 差分测试都靠它）。这里选"两边跑同一条公式"。
 */
static const float k_gauss9[5] = { 0.227027f, 0.194594f, 0.121621f, 0.054054f,
                                   0.016216f };

static void gauss_pass4(const uint8_t *src, uint8_t *dst, int w, int h, int step,
                        int horizontal)
{
    const int n = horizontal ? w : h;
    const int m = horizontal ? h : w;
    const size_t delta = horizontal ? 4u : (size_t)4u * (size_t)w;
    const size_t line = horizontal ? (size_t)4u * (size_t)w : 4u;

    for (int j = 0; j < m; j++) {
        const uint8_t *s0 = src + (size_t)j * line;
        uint8_t *d0 = dst + (size_t)j * line;
        for (int i = 0; i < n; i++) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int c = 0; c < 4; c++)
                acc[c] = (float)s0[(size_t)i * delta + (size_t)c] * k_gauss9[0];
            for (int k = 1; k < 5; k++) {
                int a = i + k * step;
                int b = i - k * step;
                if (a >= n)
                    a = n - 1;
                if (b < 0)
                    b = 0;
                const uint8_t *pa = s0 + (size_t)a * delta;
                const uint8_t *pb = s0 + (size_t)b * delta;
                for (int c = 0; c < 4; c++)
                    acc[c] += ((float)pa[(size_t)c] + (float)pb[(size_t)c]) *
                              k_gauss9[k];
            }
            uint8_t *d = d0 + (size_t)i * delta;
            for (int c = 0; c < 4; c++) {
                const float v = acc[c] + 0.5f;
                d[(size_t)c] = (uint8_t)((v < 0.0f) ? 0.0f : (v > 255.0f ? 255.0f : v));
            }
        }
    }
}

void ql_layer_blur(uint8_t *rgba, int w, int h, int radius)
{
    if (rgba == NULL || w <= 1 || h <= 1 || radius <= 0)
        return;
    int step = (radius + 1) / 2;      /* 与 GL 的 `radius / 2` 对齐（这里向上取整） */
    if (step < 1)
        step = 1;
    const size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *tmp = (uint8_t *)ql_mem_alloc(bytes);
    if (tmp == NULL)
        return;
    gauss_pass4(rgba, tmp, w, h, step, 1);
    gauss_pass4(tmp, rgba, w, h, step, 0);
    ql_mem_free(tmp);
}

/* ------------------------------ 颜色矩阵族 ------------------------------ */

/*
 * 把 sRGB 的一个像素按 CSS 的 filter 公式变换。`kind` 里除 BLUR /
 * DROP_SHADOW 之外的都在这里（它们对每个像素独立作用）。
 * `arg` 是千分比（1000 = 不变）或角度。
 */
static void color_op_px(uint8_t kind, int32_t arg, int32_t *r, int32_t *g, int32_t *b)
{
    switch (kind) {
    case QL_UI_FILTER_BRIGHTNESS:
        *r = *r * arg / 1000;
        *g = *g * arg / 1000;
        *b = *b * arg / 1000;
        break;
    case QL_UI_FILTER_CONTRAST: {
        /* CSS: c' = (c - 0.5) * k + 0.5 */
        *r = (int32_t)(((int64_t)(*r - 128) * arg) / 1000) + 128;
        *g = (int32_t)(((int64_t)(*g - 128) * arg) / 1000) + 128;
        *b = (int32_t)(((int64_t)(*b - 128) * arg) / 1000) + 128;
        break;
    }
    case QL_UI_FILTER_SATURATE: {
        /* 饱和度矩阵（与 CSS Filter Effects 同一系数） */
        const int32_t s = arg;
        const int32_t ir = 213, ig = 715, ib = 72;   /* 0.213/0.715/0.072 */
        const int32_t rr = (ir + (1000 - ir) * s / 1000);
        const int32_t rg = (ig - ig * s / 1000);
        const int32_t rb = (ib - ib * s / 1000);
        const int32_t gr = (ir - ir * s / 1000);
        const int32_t gg = (ig + (1000 - ig) * s / 1000);
        const int32_t gb = (ib - ib * s / 1000);
        const int32_t br = (ir - ir * s / 1000);
        const int32_t bg = (ig - ig * s / 1000);
        const int32_t bb = (ib + (1000 - ib) * s / 1000);
        const int32_t o0 = *r, o1 = *g, o2 = *b;
        *r = (rr * o0 + rg * o1 + rb * o2) / 1000;
        *g = (gr * o0 + gg * o1 + gb * o2) / 1000;
        *b = (br * o0 + bg * o1 + bb * o2) / 1000;
        break;
    }
    case QL_UI_FILTER_GRAYSCALE: {
        const int32_t k = arg;          /* 0..1000 */
        const int32_t lum = (213 * *r + 715 * *g + 72 * *b) / 1000;
        *r = *r + (lum - *r) * k / 1000;
        *g = *g + (lum - *g) * k / 1000;
        *b = *b + (lum - *b) * k / 1000;
        break;
    }
    case QL_UI_FILTER_SEPIA: {
        const int32_t k = arg;          /* 0..1000 */
        const int32_t o0 = *r, o1 = *g, o2 = *b;
        const int32_t nr = (393 * o0 + 769 * o1 + 189 * o2) / 1000;
        const int32_t ng = (349 * o0 + 686 * o1 + 168 * o2) / 1000;
        const int32_t nb = (272 * o0 + 534 * o1 + 131 * o2) / 1000;
        *r = o0 + (nr - o0) * k / 1000;
        *g = o1 + (ng - o1) * k / 1000;
        *b = o2 + (nb - o2) * k / 1000;
        break;
    }
    case QL_UI_FILTER_INVERT: {
        const int32_t k = arg;
        *r = *r + ((255 - *r) - *r) * k / 1000;
        *g = *g + ((255 - *g) - *g) * k / 1000;
        *b = *b + ((255 - *b) - *b) * k / 1000;
        break;
    }
    case QL_UI_FILTER_HUE_ROTATE: {
        /*
         * CSS 的 hue-rotate 是一段固定的线性矩阵（Filter Effects 规范里的
         * hueRotate(angle)），不是转成 HSL 再转回来 —— 照抄矩阵，
         * 这样和浏览器逐像素一致。系数用千分比的三角函数表近似不划算，
         * 直接调 cos/sin（一帧只算一次，见 ql_layer_apply_color_ops）。
         */
        const double a = (double)arg * 3.14159265358979323846 / 180.0;
        const double c = cos(a), s = sin(a);
        const double m[9] = {
            0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715,
            0.072 - c * 0.072 + s * 0.928,
            0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140,
            0.072 - c * 0.072 - s * 0.283,
            0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715,
            0.072 + c * 0.928 + s * 0.072,
        };
        const int32_t o0 = *r, o1 = *g, o2 = *b;
        *r = (int32_t)(m[0] * o0 + m[1] * o1 + m[2] * o2 + 0.5);
        *g = (int32_t)(m[3] * o0 + m[4] * o1 + m[5] * o2 + 0.5);
        *b = (int32_t)(m[6] * o0 + m[7] * o1 + m[8] * o2 + 0.5);
        break;
    }
    default:
        break;
    }
}

void ql_layer_apply_color_ops(uint8_t *rgba, int w, int h,
                              const QLDraw2DLayerFx *fx)
{
    if (rgba == NULL || fx == NULL || fx->nops == 0 || w <= 0 || h <= 0)
        return;

    /* 只有颜色类函数才需要逐像素；数一下，一个都没有就直接返回 */
    int ncolor = 0;
    for (uint8_t i = 0; i < fx->nops && i < 6u; i++) {
        const uint8_t k = QL_UI_FILTER_KIND(fx->op[i]);
        if (k != QL_UI_FILTER_BLUR && k != QL_UI_FILTER_DROP_SHADOW)
            ncolor++;
    }
    if (ncolor == 0)
        return;

    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = rgba + i * 4u;
        const uint32_t a = p[3];
        if (a == 0)
            continue;
        /* 预乘 → 直通色（颜色矩阵按直通色定义） */
        int32_t r = (int32_t)p[0] * 255 / (int32_t)a;
        int32_t g = (int32_t)p[1] * 255 / (int32_t)a;
        int32_t b = (int32_t)p[2] * 255 / (int32_t)a;
        int32_t aa = (int32_t)a;

        for (uint8_t k = 0; k < fx->nops && k < 6u; k++) {
            const uint8_t kind = QL_UI_FILTER_KIND(fx->op[k]);
            const int32_t arg = QL_UI_FILTER_ARG(fx->op[k]);
            if (kind == QL_UI_FILTER_OPACITY) {
                aa = aa * arg / 1000;
                continue;
            }
            color_op_px(kind, arg, &r, &g, &b);
        }
        r = clamp_u8(r);
        g = clamp_u8(g);
        b = clamp_u8(b);
        aa = aa < 0 ? 0 : (aa > 255 ? 255 : aa);
        /* 回预乘 */
        p[0] = (uint8_t)(r * aa / 255);
        p[1] = (uint8_t)(g * aa / 255);
        p[2] = (uint8_t)(b * aa / 255);
        p[3] = (uint8_t)aa;
    }
}

/* ------------------------------ drop-shadow ------------------------------ */

void ql_layer_drop_shadow(uint8_t *rgba, int w, int h, const QLDraw2DLayerFx *fx)
{
    if (rgba == NULL || fx == NULL || !fx->has_drop || w <= 0 || h <= 0)
        return;

    /* 颜色在 qlui 侧就已经把 currentColor 折进去了（0 = 不透明白？不，用黑） */
    const uint32_t col = fx->ds_color;
    int32_t cr = (int32_t)QL_COLOR_GET_R(col);
    int32_t cg = (int32_t)QL_COLOR_GET_G(col);
    int32_t cb = (int32_t)QL_COLOR_GET_B(col);
    int32_t ca = (fx->ds_a != 0) ? (int32_t)fx->ds_a : 255;

    const size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *sh = (uint8_t *)ql_mem_alloc(bytes);
    if (sh == NULL)
        return;

    /* 1. 抽出图层 alpha，平移到 (dx,dy) */
    memset(sh, 0, bytes);
    for (int y = 0; y < h; y++) {
        const int sy = y - fx->ds_dy;
        if (sy < 0 || sy >= h)
            continue;
        for (int x = 0; x < w; x++) {
            const int sx = x - fx->ds_dx;
            if (sx < 0 || sx >= w)
                continue;
            const uint32_t a = rgba[((size_t)sy * (size_t)w + (size_t)sx) * 4u + 3u];
            if (a == 0)
                continue;
            uint8_t *o = sh + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            const uint32_t ea = a * (uint32_t)ca / 255u;
            o[0] = (uint8_t)((uint32_t)cr * ea / 255u);
            o[1] = (uint8_t)((uint32_t)cg * ea / 255u);
            o[2] = (uint8_t)((uint32_t)cb * ea / 255u);
            o[3] = (uint8_t)ea;
        }
    }

    /* 2. 模糊（阴影的边界已经在 allocate bounds 时留出来了） */
    if (fx->ds_blur > 0)
        ql_layer_blur(sh, w, h, (int)fx->ds_blur);

    /*
     * 3. 叠在图层**下面**：out = layer + shadow*(1 - layer_a)。
     * 注意顺序：阴影是被盖住的那一层，写成 `shadow + layer*(1-shadow_a)` 就是
     * 把阴影画在图层上面（重叠处整块变成阴影色），视觉上像"阴影盖住了元素"。
     */
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = rgba + i * 4u;
        const uint8_t *q = sh + i * 4u;
        const uint32_t inv = 255u - p[3];
        p[0] = (uint8_t)(p[0] + q[0] * inv / 255u);
        p[1] = (uint8_t)(p[1] + q[1] * inv / 255u);
        p[2] = (uint8_t)(p[2] + q[2] * inv / 255u);
        p[3] = (uint8_t)(p[3] + q[3] * inv / 255u);
    }
    ql_mem_free(sh);
}

/* ------------------------------ 合成 ------------------------------ */

/* 一个通道的混合函数（直通色 0..255） */
static inline int32_t blend_ch(uint8_t mode, int32_t cb, int32_t cs)
{
    switch (mode) {
    case QL_UI_BLEND_MULTIPLY:
        return cb * cs / 255;
    case QL_UI_BLEND_SCREEN:
        return 255 - (255 - cb) * (255 - cs) / 255;
    case QL_UI_BLEND_OVERLAY:
        return (cb < 128) ? (2 * cb * cs / 255)
                          : (255 - 2 * (255 - cb) * (255 - cs) / 255);
    case QL_UI_BLEND_DARKEN:
        return cb < cs ? cb : cs;
    case QL_UI_BLEND_LIGHTEN:
        return cb > cs ? cb : cs;
    case QL_UI_BLEND_DIFFERENCE: {
        const int32_t d = cb - cs;
        return d < 0 ? -d : d;
    }
    default:
        return cs;
    }
}

/* 目标画布的原始像素读写（绕过 clip/mask：合成自己管裁剪） */
static inline void dst_get(const QLCanvas *c, ql_coord_t x, ql_coord_t y, uint8_t *rgb)
{
    const uint8_t *row = c->buffer + (uint32_t)y * c->stride;
    switch (c->format) {
    case QL_COLOR_RGB565: {
        uint16_t v;
        memcpy(&v, row + (uint32_t)x * 2u, 2);
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
    case QL_COLOR_ARGB8888: {
        uint32_t v;
        memcpy(&v, row + (uint32_t)x * 4u, 4);
        rgb[0] = (uint8_t)((v >> 16) & 0xFFu);
        rgb[1] = (uint8_t)((v >> 8) & 0xFFu);
        rgb[2] = (uint8_t)(v & 0xFFu);
        break;
    }
    default:
        rgb[0] = rgb[1] = rgb[2] = row[(uint32_t)x];
        break;
    }
}

static inline void dst_set(QLCanvas *c, ql_coord_t x, ql_coord_t y, const uint8_t *rgb)
{
    uint8_t *row = c->buffer + (uint32_t)y * c->stride;
    switch (c->format) {
    case QL_COLOR_RGB565: {
        uint16_t v = (uint16_t)((((uint32_t)rgb[0] & 0xF8u) << 8) |
                                (((uint32_t)rgb[1] & 0xFCu) << 3) |
                                ((uint32_t)rgb[2] >> 3));
        memcpy(row + (uint32_t)x * 2u, &v, 2);
        break;
    }
    case QL_COLOR_RGB888: {
        uint8_t *p = row + (uint32_t)x * 3u;
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
        break;
    }
    case QL_COLOR_ARGB8888: {
        const uint32_t v = 0xFF000000u | ((uint32_t)rgb[0] << 16) |
                           ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
        memcpy(row + (uint32_t)x * 4u, &v, 4);
        break;
    }
    default:
        row[(uint32_t)x] = (uint8_t)(((uint32_t)rgb[0] * 77u + (uint32_t)rgb[1] * 150u +
                                      (uint32_t)rgb[2] * 29u) >> 8);
        break;
    }
}

/* 遮罩图的一次采样（拉伸到 `box` 上；用 alpha） */
static uint32_t mask_alpha_at(const QLImage *m, QLRect box, ql_coord_t x, ql_coord_t y)
{
    if (m == NULL || m->px == NULL || m->w == 0 || m->h == 0 || box.w <= 0 || box.h <= 0)
        return 255u;
    int32_t sx = (int32_t)(((int64_t)(x - box.x) * 2 + 1) * m->w / (2 * (int64_t)box.w));
    int32_t sy = (int32_t)(((int64_t)(y - box.y) * 2 + 1) * m->h / (2 * (int64_t)box.h));
    if (sx < 0)
        sx = 0;
    if (sx >= (int32_t)m->w)
        sx = (int32_t)m->w - 1;
    if (sy < 0)
        sy = 0;
    if (sy >= (int32_t)m->h)
        sy = (int32_t)m->h - 1;

    const uint8_t *row = m->px + (uint32_t)sy * m->stride;
    switch (m->format) {
    case QL_IMAGE_RGBA8888:
        return row[(uint32_t)sx * 4u + 3u];
    case QL_IMAGE_A8:
        return row[(uint32_t)sx];
    case QL_IMAGE_RGB565:
    case QL_IMAGE_RGB888:
        return 255u;   /* 没有 alpha 通道的图当"全不遮" */
    default:
        return 255u;
    }
}

void ql_layer_composite(QLCanvas *dst, const uint8_t *rgba, int w, int h, QLRect at,
                        const QLDraw2DLayerFx *fx)
{
    if (dst == NULL || rgba == NULL || w <= 0 || h <= 0 || dst->buffer == NULL)
        return;
    const QLRect view = ql_rect_make(0, 0, dst->width, dst->height);
    const QLRect area = ql_rect_intersect(ql_rect_intersect(at, view), dst->clip);
    if (ql_rect_is_empty(area))
        return;

    const uint32_t gop = (fx != NULL && fx->opacity != 0) ? fx->opacity : 255u;
    const uint8_t mode = (fx != NULL) ? fx->blend : (uint8_t)QL_UI_BLEND_NORMAL;
    const QLImage *mask = (fx != NULL) ? fx->mask : NULL;
    const QLRect box = (fx != NULL) ? fx->box : at;
    const ql_coord_t corner = (fx != NULL) ? fx->corner : 0;

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        const int sy = (int)(y - at.y);
        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            const int sx = (int)(x - at.x);
            const uint8_t *s = rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4u;
            uint32_t a = s[3];
            if (a == 0)
                continue;

            uint32_t cov = 255u;
            if (corner > 0)
                cov = ql_layer_round_cov(box, corner, x, y);
            if (cov == 0u)
                continue;
            if (mask != NULL) {
                const uint32_t m2 = mask_alpha_at(mask, box, x, y);
                if (m2 == 0u)
                    continue;
                cov = cov * m2 / 255u;
            }
            a = a * gop / 255u;
            a = a * cov / 255u;
            if (a == 0)
                continue;

            uint8_t d[3];
            dst_get(dst, x, y, d);
            uint8_t o[3];
            /*
             * 只有**普通混合**才走"预乘 src-over"这条快路。
             * 别把 `a == 255` 也并进来：混合模式跟源的不透明度无关，
             * 不透明图层同样要和背景相乘/相加（写成 or 的话混合模式等于没实现）。
             */
            if (mode == QL_UI_BLEND_NORMAL) {
                for (int c = 0; c < 3; c++) {
                    const uint32_t src = s[c] * gop / 255u * cov / 255u;
                    o[c] = (uint8_t)(src + (uint32_t)d[c] * (255u - a) / 255u);
                }
            } else {
                /* 直通色 → 混合 → 再按 alpha 回混 */
                for (int c = 0; c < 3; c++) {
                    const uint32_t cs = s[c] * 255u / s[3];
                    const uint32_t bs = (uint32_t)blend_ch(mode, d[c], (int32_t)cs);
                    o[c] = (uint8_t)(((uint32_t)d[c] * (255u - a) + a * bs) / 255u);
                }
            }
            dst_set(dst, x, y, o);
        }
    }
}

#endif /* QL_FEAT_CSS_FILTER_LAYER */
