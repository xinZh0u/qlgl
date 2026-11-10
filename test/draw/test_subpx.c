/**
 * @file    test_subpx.c
 * @brief   绘制期亚像素（26.6）与边缘覆盖率抗锯齿。
 *
 * 动画的"顺滑"取决于这个里程碑：整像素矩形只能对齐到整格，`translate(0.5px)`
 * 会变成 1px 一跳。这里给光栅化加了"边落在小数上也能画"的原语。
 *
 * 三条契约：
 *   1. **对齐时逐字节不变**：整数矩形转成 fx 矩形（低 6 位为 0）后结果必须与
 *      `ql_canvas_fill_rect_alpha` / `ql_canvas_fill_round_rect_alpha` 完全一致。
 *      这是整个改造的安全阀——只要变了一个像素就说明覆盖率算错了。
 *   2. **覆盖率按面积算**：0.5px 偏移的边缘像素拿一半 alpha，且一行内的覆盖率之和
 *      等于真实宽度。
 *   3. **1px 偏移 == 整体右移 1px**：整数偏移必须退化成纯平移，不带任何毛边。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 40
#define H 40

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

/* 灰度近似比较 */
static int near_gray(ql_color_t c, int v, int tol)
{
    int r = (int)QL_COLOR_GET_R(c);
    return r >= v - tol && r <= v + tol;
}

QL_TEST_BEGIN("test_subpx")

    const ql_color_t black = QL_COLOR_RGB(0, 0, 0);
    const ql_color_t white = QL_COLOR_RGB(0xFF, 0xFF, 0xFF);

    /* ---------- 1. 对齐时逐字节不变（安全阀） ---------- */
    {
        const QLRect rects[] = {
            { 0, 0, 1, 1 },
            { 5, 7, 13, 9 },
            { 0, 0, 40, 40 },
            { 3, 3, 0, 0 },       /* 空 */
            { -4, -3, 20, 18 },   /* 部分出界 */
        };
        const ql_coord_t rads[] = { 0, 1, 4, 10 };
        const uint8_t alphas[] = { 255, 128, 40 };
        int bad = 0;
        static ql_color_t want[W * H];

        for (int i = 0; i < 5; i++) {
            for (int r = 0; r < 4; r++) {
                for (int a = 0; a < 3; a++) {
                    fresh(white);
                    ql_canvas_fill_round_rect_alpha(&s_canvas, rects[i], rads[r], black,
                                                    alphas[a]);
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++)
                            want[y * W + x] = px(x, y);

                    fresh(white);
                    ql_canvas_fill_round_rect_fx(&s_canvas, ql_rectfx_from_rect(rects[i]),
                                                 rads[r] * QL_FX_ONE, black, alphas[a]);
                    for (int y = 0; y < H && bad == 0; y++)
                        for (int x = 0; x < W; x++)
                            if (px(x, y) != want[y * W + x]) {
                                bad++;
                                printf("  fx vs int 不一致：rect#%d rad=%d a=%d (%d,%d)\n", i,
                                       (int)rads[r], (int)alphas[a], x, y);
                                break;
                            }

                    /* 描边同样要对齐 */
                    fresh(white);
                    ql_canvas_stroke_rect(&s_canvas, rects[i], rads[r], 2, black);
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++)
                            want[y * W + x] = px(x, y);

                    fresh(white);
                    ql_canvas_stroke_rect_fx(&s_canvas, ql_rectfx_from_rect(rects[i]),
                                             rads[r] * QL_FX_ONE, 2 * QL_FX_ONE, black);
                    for (int y = 0; y < H && bad == 0; y++)
                        for (int x = 0; x < W; x++)
                            if (px(x, y) != want[y * W + x]) {
                                bad++;
                                printf("  描边 fx vs int 不一致：rect#%d rad=%d (%d,%d)\n", i,
                                       (int)rads[r], x, y);
                                break;
                            }
                }
            }
        }
        QL_CHECK_EQ(bad, 0);
    }

    /* ---------- 2. 覆盖率按面积：0.5px 偏移 ---------- */
    {
        /* 10px 宽的白色矩形，左边落在 x=0.5 → 首尾像素各 50%，中间 9 个满 */
        fresh(black);
        ql_canvas_fill_rect_fx(&s_canvas,
                               ql_rectfx_make(QL_FX_ONE / 2, 0, 10 * QL_FX_ONE,
                                              10 * QL_FX_ONE),
                               white, 255);
        QL_CHECK(near_gray(px(0, 0), 128, 2));    /* 50% */
        QL_CHECK(near_gray(px(1, 0), 255, 1));    /* 满 */
        QL_CHECK(near_gray(px(9, 0), 255, 1));    /* 满 */
        QL_CHECK(near_gray(px(10, 0), 128, 2));   /* 50% */
        QL_CHECK(near_gray(px(11, 0), 0, 1));     /* 外面 */

        /* 一行内的覆盖率之和必须等于真实宽度 10px */
        int sum = 0;
        for (int x = 0; x < W; x++)
            sum += QL_COLOR_GET_R(px(x, 0));
        QL_CHECK(sum >= 10 * 255 - 6 && sum <= 10 * 255 + 6);

        /* 纵向同理：上边落在 y=0.25 → 首行 25% */
        fresh(black);
        ql_canvas_fill_rect_fx(&s_canvas,
                               ql_rectfx_make(0, QL_FX_ONE / 4, 10 * QL_FX_ONE,
                                              4 * QL_FX_ONE),
                               white, 255);
        QL_CHECK(near_gray(px(2, 0), 191, 2));   /* 首行覆盖 [0.25,1) = 75% */
        QL_CHECK(near_gray(px(2, 1), 255, 1));
        QL_CHECK(near_gray(px(2, 4), 64, 2));    /* 末行覆盖 [4,4.25) = 25% */
        QL_CHECK(near_gray(px(2, 5), 0, 1));
    }

    /* ---------- 2b. 一个像素内有多个台阶（"顺滑"的真正含义） ---------- */
    {
        int seen[8];
        int nseen = 0;
        const ql_coord_t offs[6] = { 0, 13, 26, 39, 51, 64 };
        for (int k = 0; k < 6; k++) {
            fresh(black);
            ql_canvas_fill_rect_fx(&s_canvas,
                                   ql_rectfx_make(offs[k], 0, 6 * QL_FX_ONE,
                                                  6 * QL_FX_ONE),
                                   white, 255);
            int v = (int)QL_COLOR_GET_R(px(0, 2));   /* 左边缘所在像素 */
            int dup = 0;
            for (int i = 0; i < nseen; i++)
                if (seen[i] == v)
                    dup = 1;
            if (!dup && nseen < 8)
                seen[nseen++] = v;
        }
        /* 整像素只能给出 1 个台阶；亚像素至少能给出 5 个不同值 */
        QL_CHECK(nseen >= 5);
    }

    /* ---------- 3. 1px 偏移退化成纯平移 ---------- */
    {
        fresh(white);
        QLRect plain = ql_rect_make(10, 12, 8, 6);
        ql_canvas_fill_round_rect_alpha(&s_canvas,
                                        ql_rect_make(plain.x + 1, plain.y, plain.w, plain.h),
                                        3, black, 200);
        static ql_color_t want[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                want[y * W + x] = px(x, y);

        fresh(white);
        ql_canvas_fill_round_rect_fx(&s_canvas,
                                     ql_rectfx_make((plain.x << QL_FX_SHIFT) + QL_FX_ONE,
                                                    plain.y << QL_FX_SHIFT,
                                                    plain.w << QL_FX_SHIFT,
                                                    plain.h << QL_FX_SHIFT),
                                     3 * QL_FX_ONE, black, 200);
        int bad = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (px(x, y) != want[y * W + x])
                    bad++;
        QL_CHECK_EQ(bad, 0);
    }

    /* ---------- 4. QLDraw2D 的绘制期偏移 ---------- */
    {
        fresh(white);
        QLDraw2D d2d;
        QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &s_canvas), QL_OK);
        ql_draw2d_set_offset(&d2d, 0, 0);
        QLRect r = ql_rect_make(6, 6, 12, 9);
        d2d.fill_round_rect(&d2d, r, 4, black, 255);
        static ql_color_t want[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                want[y * W + x] = px(x, y);

        /* offset = 1px：等价于整体右移 1px */
        fresh(white);
        ql_draw2d_set_offset(&d2d, QL_FX_ONE, 0);
        d2d.fill_round_rect(&d2d, r, 4, black, 255);
        int bad = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W - 1; x++)
                if (px(x + 1, y) != want[y * W + x])
                    bad++;
        QL_CHECK_EQ(bad, 0);

        /* offset = 0.5px：左边界那一列应该是半透明，且不再是纯黑或纯白 */
        fresh(white);
        ql_draw2d_set_offset(&d2d, QL_FX_ONE / 2, 0);
        d2d.fill_rect(&d2d, ql_rect_make(10, 10, 10, 10), black, 255);
        ql_coord_t edge = QL_COLOR_GET_R(px(10, 14));   /* 左边缘落 0.5 */
        QL_CHECK(edge > 110 && edge < 145);
        QL_CHECK(near_gray(px(11, 14), 0, 1));          /* 内部仍满 */
        QL_CHECK(near_gray(px(9, 14), 255, 1));         /* 外面不受影响 */

        ql_draw2d_set_offset(&d2d, 0, 0);
    }

#if defined(QLGL_ENABLE_GL)
    /* ---------- 5. GPU 也吃同一个偏移（顶点本来就是浮点，零成本） ---------- */
    {
        const ql_color_t bg = QL_COLOR_RGB(0x08, 0x0C, 0x10);
        QLRect r = ql_rect_make(8, 8, 20, 14);

        fresh(bg);
        QLDraw2D cpu;
        QL_CHECK_EQ(ql_draw2d_canvas_init(&cpu, &s_canvas), QL_OK);
        ql_draw2d_set_offset(&cpu, QL_FX_ONE / 2, QL_FX_ONE / 4);
        cpu.fill_round_rect(&cpu, r, 5, QL_COLOR_RGB(0x40, 0x90, 0xE0), 255);

        QLGfxGL *gfx = ql_gfx_gl_create("subpx", W, H, QL_GFX_GL_OFFSCREEN);
        if (gfx == NULL) {
            QL_TEST_GL_SKIP("亚像素 GPU 对比：无法创建 GL 上下文");
        } else {
            ql_gfx_gl_begin(gfx, bg);
            QLDraw2D *gl = ql_gfx_gl_draw(gfx);
            ql_draw2d_set_offset(gl, QL_FX_ONE / 2, QL_FX_ONE / 4);
            gl->fill_round_rect(gl, r, 5, QL_COLOR_RGB(0x40, 0x90, 0xE0), 255);
            ql_gfx_gl_end(gfx);

            uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * (size_t)H * 4u);
            QL_CHECK(rgba != NULL);
            if (rgba != NULL) {
                QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);
                long bad = 0;
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        ql_color_t c = px(x, y);
                        const uint8_t *g = rgba + ((size_t)y * (size_t)W + (size_t)x) * 4u;
                        int dr = abs((int)QL_COLOR_GET_R(c) - (int)g[0]);
                        int dg = abs((int)QL_COLOR_GET_G(c) - (int)g[1]);
                        int db = abs((int)QL_COLOR_GET_B(c) - (int)g[2]);
                        int d = dr > dg ? dr : dg;
                        if (db > d)
                            d = db;
                        if (d > 40)
                            bad++;
                    }
                }
                double ratio = (double)bad / (double)(W * H);
                printf("  亚像素偏移下 CPU vs GPU：%ld/%d = %.2f%%\n", bad, W * H,
                       ratio * 100.0);
                QL_CHECK(ratio <= 0.02);
                ql_mem_free(rgba);
            }
            ql_gfx_gl_destroy(gfx);
        }
        ql_draw2d_set_offset(&cpu, 0, 0);
    }
#endif

    ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
