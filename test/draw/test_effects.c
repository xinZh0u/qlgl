/**
 * @file    test_effects.c
 * @brief   渐变与软阴影的像素级断言（CPU 路径 + GPU/CPU 一致性由 test_gl_vs_cpu 守）。
 */
#include <math.h>

#include "../ql_test.h"

#define W 120
#define H 120

static QLDispDriver *s_disp;
static QLCanvas s_canvas;

static void fresh(ql_color_t bg)
{
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, bg);
}

static ql_color_t px(ql_coord_t x, ql_coord_t y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

/*
 * 朴素参考实现：对整个外框逐像素求圆角矩形 SDF，不做任何九宫格分解。
 *
 * ql_canvas_fill_shadow 内部有若干优化（九宫格分解、整块可见时转调通用圆角
 * 填充、角内合并分支、主格式直写），这些都必须**逐像素等价**于本函数。
 * 下面用它做对照，避免优化悄悄改变像素。
 */
/* 参考 SDF：与 GPU 着色器同一个式子（含 inside 项） */
static float ref_sd(QLRect shaped, ql_coord_t rad, ql_coord_t x, ql_coord_t y)
{
    float hx = (float)shaped.w * 0.5f, hy = (float)shaped.h * 0.5f;
    float cx = (float)shaped.x + hx, cy = (float)shaped.y + hy;
    float fr = (float)rad;
    float qx = fabsf((float)x + 0.5f - cx) - hx + fr;
    float qy = fabsf((float)y + 0.5f - cy) - hy + fr;
    float mx = (qx > 0.0f) ? qx : 0.0f;
    float my = (qy > 0.0f) ? qy : 0.0f;
    float d = sqrtf(mx * mx + my * my);
    float in = (qx > qy) ? qx : qy;
    if (in > 0.0f)
        in = 0.0f;
    return d + in - fr;
}

static void ref_shadow(QLCanvas *c, QLRect inner, ql_coord_t radius, ql_coord_t blur,
                       ql_coord_t dx, ql_coord_t dy, ql_color_t color, uint8_t alpha)
{
    if (blur < 0)
        blur = 0;
    QLRect shaped = ql_rect_make(inner.x + dx, inner.y + dy, inner.w, inner.h);
    ql_coord_t pad = blur + 1;
    QLRect outer = ql_rect_make(shaped.x - pad, shaped.y - pad,
                                shaped.w + 2 * pad, shaped.h + 2 * pad);
    QLRect area = ql_rect_intersect(outer, c->clip);
    if (ql_rect_is_empty(area))
        return;

    ql_coord_t limit = (shaped.w < shaped.h ? shaped.w : shaped.h) / 2;
    ql_coord_t rad = (radius > limit) ? limit : (radius < 0 ? 0 : radius);
    float fb = (float)blur;

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        for (ql_coord_t x = area.x; x < ql_rect_right(area); x++) {
            float d = ref_sd(shaped, rad, x, y);

            uint8_t cov = 255;
            if (d > 0.0f) {
                if (fb <= 0.0f)
                    continue;
                float t = d / fb;
                if (t >= 1.0f)
                    continue;
                uint32_t v = (uint32_t)((1.0f - t * t * (3.0f - 2.0f * t)) * 255.0f + 0.5f);
                cov = (uint8_t)(v > 255u ? 255u : v);
                if (cov == 0)
                    continue;
            }
            uint32_t a = (uint32_t)cov * (uint32_t)alpha / 255u;
            if (a == 0)
                continue;
            /* 手工混合，与画布 blend 同式 */
            uint32_t inv = 255u - a;
            uint8_t *row = c->buffer + (uint32_t)y * c->stride;
            uint32_t *p = (uint32_t *)(void *)(row + (uint32_t)x * 4u);
            uint32_t old = *p;
            uint8_t r = (uint8_t)(((uint32_t)QL_COLOR_GET_R(color) * a +
                                   ((old >> 16) & 0xFFu) * inv) / 255u);
            uint8_t g = (uint8_t)(((uint32_t)QL_COLOR_GET_G(color) * a +
                                   ((old >> 8) & 0xFFu) * inv) / 255u);
            uint8_t b = (uint8_t)(((uint32_t)QL_COLOR_GET_B(color) * a +
                                   (old & 0xFFu) * inv) / 255u);
            *p = 0xFF000000u | QL_COLOR_RGB(r, g, b);
        }
    }
}

/* 单通道近似比较 */
static int near(ql_color_t a, ql_color_t b, int tol)
{
    int dr = (int)QL_COLOR_GET_R(a) - (int)QL_COLOR_GET_R(b);
    int dg = (int)QL_COLOR_GET_G(a) - (int)QL_COLOR_GET_G(b);
    int db = (int)QL_COLOR_GET_B(a) - (int)QL_COLOR_GET_B(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tol && dg <= tol && db <= tol;
}

QL_TEST_BEGIN("test_effects")

    const ql_color_t bg = QL_COLOR_RGB(0x00, 0x00, 0x00);
    const ql_color_t c0 = QL_COLOR_RGB(0x00, 0x00, 0xFF);   /* 蓝 */
    const ql_color_t c1 = QL_COLOR_RGB(0xFF, 0x00, 0x00);   /* 红 */

    /* ---------- 1. 垂直渐变：上蓝下红，中间是过渡 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 0,
                                       c0, c1, QL_GRADIENT_VERTICAL, 255, 255);
    QL_CHECK(near(px(60, 10), c0, 8));
    QL_CHECK(near(px(60, 109), c1, 8));
    ql_color_t mid = px(60, 60);
    QL_CHECK(QL_COLOR_GET_R(mid) > 100 && QL_COLOR_GET_R(mid) < 160);   /* 半程 ≈ 128 */
    QL_CHECK(QL_COLOR_GET_B(mid) > 100 && QL_COLOR_GET_B(mid) < 160);
    /* 同 y 的整行颜色相同（垂直渐变的快路径特征） */
    QL_CHECK_EQ(px(11, 60), px(108, 60));

    /* ---------- 2. 水平渐变：左蓝右红，同列颜色相同 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 0,
                                       c0, c1, QL_GRADIENT_HORIZONTAL, 255, 255);
    QL_CHECK(near(px(10, 60), c0, 8));
    QL_CHECK(near(px(109, 60), c1, 8));
    QL_CHECK_EQ(px(60, 11), px(60, 108));      /* 同一列相同 */

    /* ---------- 3. 渐变 + 圆角：角上不填 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 20,
                                       c0, c1, QL_GRADIENT_VERTICAL, 255, 255);
    QL_CHECK_EQ(px(10, 10), bg);               /* 左上角外 */
    QL_CHECK_EQ(px(109, 10), bg);              /* 右上角外 */
    QL_CHECK(near(px(60, 11), c0, 10));        /* 顶边中点仍然填 */
    QL_CHECK(near(px(60, 108), c1, 10));       /* 底边中点仍然填 */

    /* ---------- 4. 渐变 alpha=0 是空操作 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(0, 0, W, H), 0,
                                       c0, c1, QL_GRADIENT_VERTICAL, 0, 0);
    QL_CHECK_EQ(px(60, 60), bg);

    /* ---------- 5. 软阴影：紧贴边缘有暗晕，远处没有，内部不受影响 ---------- */
    const ql_color_t white = QL_COLOR_RGB(0xFF, 0xFF, 0xFF);
    fresh(white);
    QLRect body = ql_rect_make(30, 30, 60, 60);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 10, 0, 4, QL_COLOR_RGB(0, 0, 0), 255);
    ql_canvas_fill_round_rect(&s_canvas, body, 0, QL_COLOR_RGB(0x20, 0x20, 0x20));

    /* 正下方（偏移方向）应该有渐变暗晕 */
    ql_color_t s1 = px(60, 92);    /* 底边下 2px */
    ql_color_t s2 = px(60, 99);    /* 更远 */
    QL_CHECK(QL_COLOR_GET_R(s1) < 0xF0);       /* 明显变暗 */
    QL_CHECK(QL_COLOR_GET_R(s1) < QL_COLOR_GET_R(s2));   /* 越远越淡 */
    QL_CHECK(QL_COLOR_GET_R(s2) < 0xFF);
    /* 超出 blur 范围应完全恢复 */
    QL_CHECK_EQ(px(60, 110), white);
    /* 上方：偏移为 +4、模糊 10，阴影最远只到 y = 30+4-10 = 24，再往上必须干净 */
    QL_CHECK_EQ(px(60, 20), white);
    int r27 = QL_COLOR_GET_R(px(60, 27));          /* 27 落在 24..34 的衰减带内 */
    QL_CHECK(r27 > 140 && r27 < 230);
    /* 本体内部不受阴影影响 */
    QL_CHECK_EQ(px(60, 60), QL_COLOR_RGB(0x20, 0x20, 0x20));

    /* ---------- 6. blur = 0：硬边阴影 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 0, 0, 6, QL_COLOR_RGB(0, 0, 0), 255);
    ql_canvas_fill_round_rect(&s_canvas, body, 0, QL_COLOR_RGB(0x20, 0x20, 0x20));
    QL_CHECK_EQ(px(60, 93), QL_COLOR_RGB(0, 0, 0));   /* 偏移 6px 内是纯黑 */
    QL_CHECK_EQ(px(60, 97), white);                   /* 6px 外立即恢复 */

    /* ---------- 7. 圆角阴影：角外侧也有晕 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 16, 8, 0, 0, QL_COLOR_RGB(0, 0, 0), 255);
    QL_CHECK(QL_COLOR_GET_R(px(30, 30)) < 0xF0);      /* 圆角外沿 */
    QL_CHECK_EQ(px(3, 3), white);                     /* 远角处没有 */

    /* ---------- 8. shadow alpha = 0 是空操作 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 10, 0, 4, QL_COLOR_RGB(0, 0, 0), 0);
    QL_CHECK_EQ(px(60, 95), white);

    /* ---------- 9. 九宫格分解必须与"逐像素 SDF"逐像素完全相同 ---------- */
    {
        static QLDispDriver *rd = NULL;
        static QLCanvas rc;
        if (rd == NULL)
            rd = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);

        const ql_coord_t boxes[3][4] = {
            { 10, 10, 60, 40 },     /* 完整可见 */
            { -6, -4, 70, 50 },     /* 左上角出界 */
            { 96, 88, 40, 40 },     /* 右下角出界 */
        };
        const ql_coord_t rads[4] = { 0, 1, 4, 12 };
        const ql_coord_t blurs[4] = { 0, 1, 3, 9 };
        const ql_coord_t offs[3][2] = { { 0, 0 }, { 0, 4 }, { -3, 5 } };
        const uint8_t alphas[3] = { 255, 102, 40 };
        int mismatches = 0;
        int cases = 0;

        for (int b = 0; b < 3; b++) {
            for (int r = 0; r < 4; r++) {
                for (int bl = 0; bl < 4; bl++) {
                    for (int o = 0; o < 3; o++) {
                        for (int a = 0; a < 3; a++) {
                            QLRect body2 = ql_rect_make(boxes[b][0], boxes[b][1],
                                                        boxes[b][2], boxes[b][3]);
                            const ql_color_t shadow_col = QL_COLOR_RGB(0x00, 0x00, 0x00);

                            fresh(QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
                            ql_canvas_fill_shadow(&s_canvas, body2, rads[r], blurs[bl],
                                                  offs[o][0], offs[o][1], shadow_col,
                                                  alphas[a]);

                            ql_canvas_from_disp(&rc, rd);
                            ql_canvas_clear(&rc, QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
                            ref_shadow(&rc, body2, rads[r], blurs[bl],
                                       offs[o][0], offs[o][1], shadow_col, alphas[a]);

                            cases++;
                            /* 内实心区（d <= 0）由 round_row_span 的离散判定负责，
                               与连续 SDF 在边界上可差 1px；那是圆角填充的既定行为，
                               且实际被元素自身背景盖住，这里只校验外晕。 */
                            ql_coord_t rl =
                                (body2.w < body2.h ? body2.w : body2.h) / 2;
                            ql_coord_t rr = (rads[r] > rl) ? rl : rads[r];
                            QLRect shp = ql_rect_make(body2.x + offs[o][0],
                                                      body2.y + offs[o][1],
                                                      body2.w, body2.h);
                            for (ql_coord_t y = 0; y < H && mismatches == 0; y++) {
                                for (ql_coord_t x = 0; x < W; x++) {
                                    if (ref_sd(shp, rr, x, y) <= 0.0f)
                                        continue;
                                    if (px(x, y) != ql_disp_mem_pixel(rd, x, y)) {
                                        printf("MISMATCH box=%d rad=%d blur=%d off=%d a=%d "
                                               "at (%d,%d)\n", b, rads[r], blurs[bl], o,
                                               alphas[a], (int)x, (int)y);
                                        printf("  real %06X ref %06X\n",
                                               (unsigned)px(x, y),
                                               (unsigned)ql_disp_mem_pixel(rd, x, y));
                                        printf("  real clip %d,%d,%d,%d\n",
                                               (int)s_canvas.clip.x, (int)s_canvas.clip.y,
                                               (int)s_canvas.clip.w, (int)s_canvas.clip.h);
                                        mismatches++;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        QL_CHECK_EQ(mismatches, 0);
        QL_CHECK_EQ(cases, 3 * 4 * 4 * 3 * 3);
        ql_disp_mem_destroy(rd);
        rd = NULL;
    }

    /* ---------- N. 毛玻璃的圆角裁剪 ---------- */
    /*
     * 模糊是方框运算，元素却常常是圆角的。不裁剪时，圆角缺口里会渗出方形模糊块
     * （颜色跑到边框盒之外，CSS 里不该出现）。这里用"竖直硬边穿过左上圆角"的
     * 场景把它钉住：圆角以外必须**逐像素等于不做模糊的版本**。
     */
    {
        const QLRect bg_box = ql_rect_make(10, 10, 60, 60);
        const ql_coord_t rad = 20;
        const ql_color_t red = QL_COLOR_RGB(0xFF, 0x00, 0x00);

        /* 参考帧：只画硬边，不做任何模糊 */
        QLDispDriver *ref = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas rc;
        ql_canvas_from_disp(&rc, ref);
        ql_canvas_clear(&rc, white);
        ql_canvas_fill_rect(&rc, ql_rect_make(18, 0, 62, 80), red);

        fresh(white);
        ql_canvas_fill_rect(&s_canvas, ql_rect_make(18, 0, 62, 80), red);
        ql_canvas_backdrop_blur_round(&s_canvas, bg_box, 4, rad, 0, 0);

        int leaked = 0;
        for (ql_coord_t y = 0; y < H; y++)
            for (ql_coord_t x = 0; x < W; x++)
                if (ref_sd(bg_box, rad, x, y) > 0.5f &&
                    px(x, y) != ql_disp_mem_pixel(ref, x, y))
                    leaked++;
        QL_CHECK_EQ(leaked, 0);                       /* 圆角外一个像素都不许动 */

        /* 圆角内确实被糊了（否则上面那条断言毫无意义） */
        QL_CHECK(px(24, 24) != ql_disp_mem_pixel(ref, 24, 24));

        /* 灵敏度：同样的场景走直角版本，圆角缺口里一定会渗色 */
        fresh(white);
        ql_canvas_fill_rect(&s_canvas, ql_rect_make(18, 0, 62, 80), red);
        ql_canvas_backdrop_blur(&s_canvas, bg_box, 4, 0);
        QL_CHECK(px(12, 12) != ql_disp_mem_pixel(ref, 12, 12));

        /* corner == 0 必须与直角版本逐像素一致（不给上层留两条语义） */
        static ql_color_t plain[W * H];
        fresh(white);
        ql_canvas_fill_rect(&s_canvas, ql_rect_make(18, 0, 62, 80), red);
        ql_canvas_backdrop_blur(&s_canvas, bg_box, 4, 0);
        for (ql_coord_t y = 0; y < H; y++)
            for (ql_coord_t x = 0; x < W; x++)
                plain[y * W + x] = px(x, y);

        fresh(white);
        ql_canvas_fill_rect(&s_canvas, ql_rect_make(18, 0, 62, 80), red);
        ql_canvas_backdrop_blur_round(&s_canvas, bg_box, 4, 0, 0, 0);
        int rect_diff = 0;
        for (ql_coord_t y = 0; y < H; y++)
            for (ql_coord_t x = 0; x < W; x++)
                if (px(x, y) != plain[y * W + x])
                    rect_diff++;
        QL_CHECK_EQ(rect_diff, 0);
        ql_disp_mem_destroy(ref);
    }

    /* ---------- O. 渐变色标各自带 alpha ---------- */
    /*
     * CSS 允许 `linear-gradient(rgba(255,0,0,1), rgba(255,0,0,0))` ——
     * 从一个不透明红渐变到"全透明红"。两端 alpha 必须沿着渐变插值，
     * 而不是两端都用同一个 alpha（以前就是这样，写透明的收尾色等于没写）。
     */
    {
        const QLRect box = ql_rect_make(10, 10, 100, 60);
        const ql_color_t red = QL_COLOR_RGB(0xFF, 0x00, 0x00);

        fresh(white);
        ql_canvas_fill_round_rect_gradient(&s_canvas, box, 0, red, red,
                                           QL_GRADIENT_HORIZONTAL, 255, 0);

        /* 左端 = 纯红（alpha 255），右端 = 完全没画（白底） */
        QL_CHECK_EQ(px(10, 40), red);
        QL_CHECK_EQ(px(109, 40), white);
        /* 中段必须落在两者之间：既不是纯红也不是纯白 */
        QL_CHECK(px(60, 40) != red);
        QL_CHECK(px(60, 40) != white);
        /* 单调：越往右越接近白 */
        QL_CHECK(abs((int)QL_COLOR_GET_R(px(30, 40)) - 255) <=
                 abs((int)QL_COLOR_GET_R(px(90, 40)) - 255));

        /* 两端 alpha 一样时，结果必须与"整块实心"一致（旧行为不许变） */
        fresh(white);
        ql_canvas_fill_round_rect_gradient(&s_canvas, box, 0, red, red,
                                           QL_GRADIENT_HORIZONTAL, 255, 255);
        QL_CHECK_EQ(px(10, 40), red);
        QL_CHECK_EQ(px(109, 40), red);
    }

    /* ---------- P. backdrop-filter: saturate() ---------- */
    /*
     * 玻璃后面糊完是一片"均值色"，常常发灰。`saturate(N%)` 把饱和度往上推。
     * 测法：同一块灰蓝背景，sat=100（不变）与 sat=220 之后比较
     * "通道极差"（max-min）——推艳必须让它变大，而亮度（三通道均值）基本不动。
     */
    {
        const QLRect area = ql_rect_make(20, 20, 60, 60);
        /* 灰蓝：三通道差异小，饱和度的变化看得清 */
        const ql_color_t dull = QL_COLOR_RGB(0x60, 0x70, 0x88);

        fresh(dull);
        ql_canvas_backdrop_blur(&s_canvas, area, 1, 100);   /* 100 = 不变 */
        ql_color_t base = px(50, 50);
        int span0 = (int)QL_COLOR_GET_R(base) - (int)QL_COLOR_GET_B(base);
        int luma0 = (21 * (int)QL_COLOR_GET_R(base) + 72 * (int)QL_COLOR_GET_G(base) +
                     7 * (int)QL_COLOR_GET_B(base)) / 100;

        fresh(dull);
        ql_canvas_backdrop_blur(&s_canvas, area, 1, 220);
        ql_color_t hot = px(50, 50);
        int span1 = (int)QL_COLOR_GET_R(hot) - (int)QL_COLOR_GET_B(hot);
        int luma1 = (21 * (int)QL_COLOR_GET_R(hot) + 72 * (int)QL_COLOR_GET_G(hot) +
                     7 * (int)QL_COLOR_GET_B(hot)) / 100;

        QL_CHECK(abs(span1) > abs(span0));       /* 更艳 */
        QL_CHECK(abs(luma1 - luma0) <= 2);       /* 亮度不许明显漂 */
        /* sat=0 与 sat=100 都是"不变"，落回原色 */
        fresh(dull);
        ql_canvas_backdrop_blur(&s_canvas, area, 1, 0);
        QL_CHECK_EQ(px(50, 50), dull);
    }

    ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
