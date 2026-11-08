/**
 * @file    test_flip.c
 * @brief   transform: scaleX()/scaleY()——分轴缩放，用来做"假 3D 翻转"。
 *
 * 六条契约：
 *   1. 分轴缩放不进布局（border_box 不变）；
 *   2. scaleX 只改宽度、不改高度，且绕中心；scaleY 反之；
 *   3. 缩到 0 时元素彻底消失（不出墨）；
 *   4. 命中测试按压扁后的位置走（压掉的部分点不中）；
 *   5. @keyframes 分轴插值：另一轴保持基础值；
 *   6. CPU 与 GPU 结果一致。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_flip.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 120
#define H 120
#define ROOT_BG 0x000000u

static uint32_t s_now;

static uint32_t fake_now(void)
{
    return s_now;
}

/* 某一行上某个底色的像素个数 */
static int run_width(QLDispDriver *disp, ql_coord_t y, ql_color_t want)
{
    int n = 0;
    for (ql_coord_t x = 0; x < W; x++)
        if (ql_disp_mem_pixel(disp, x, y) == want)
            n++;
    return n;
}

/* 某一列上某个底色的像素个数 */
static int col_height(QLDispDriver *disp, ql_coord_t x, ql_color_t want)
{
    int n = 0;
    for (ql_coord_t y = 0; y < H; y++)
        if (ql_disp_mem_pixel(disp, x, y) == want)
            n++;
    return n;
}

QL_TEST_BEGIN("test_flip")

    ql_anim_set_time_source(fake_now);

    /* ---------- 1. 布局与分轴缩放无关 ---------- */
    s_now = 0;
    QLUIRoot *ui = ql_ui_build(&ui_flex_flip_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_layout(ui, W, H);
    QL_CHECK_EQ(ui->node_count, 5);

    QLRect sx_box = ql_ui_node_rect(&ui->nodes[1]);
    QLRect sy_box = ql_ui_node_rect(&ui->nodes[2]);
    QLRect fp_box = ql_ui_node_rect(&ui->nodes[3]);
    QL_CHECK_EQ(sx_box.w, 40);
    QL_CHECK_EQ(sx_box.h, 40);
    QL_CHECK_EQ(ui->nodes[1].style->sc_x, 500);
    QL_CHECK_EQ(ui->nodes[1].style->sc_y, 1000);
    QL_CHECK_EQ(ui->nodes[2].style->sc_x, 1000);
    QL_CHECK_EQ(ui->nodes[2].style->sc_y, 500);

    /* ---------- 2. scaleX 只压宽度，scaleY 只压高度 ---------- */
    {
        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK(disp != NULL);
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);

        ql_coord_t sx_cy = sx_box.y + sx_box.h / 2;
        int w = run_width(disp, sx_cy, 0xE63B3Bu);
        printf("  scaleX(0.5) 的 40px 盒子在屏幕上是 %d px 宽\n", w);
        QL_CHECK(w >= 18 && w <= 22);              /* 40 * 0.5 = 20 */
        QL_CHECK_EQ(col_height(disp, sx_box.x + 20, 0xE63B3Bu), 40);   /* 高度不动 */

        /* 绕中心：左边界从 10 退到 20，右边从 50 缩到 40 */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, sx_box.x + 9, sx_cy), ROOT_BG);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, sx_box.x + 20, sx_cy), 0xE63B3Bu);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(sx_box) - 11, sx_cy),
                    0xE63B3Bu);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(sx_box) - 1, sx_cy),
                    ROOT_BG);

        ql_coord_t sy_cx = sy_box.x + sy_box.w / 2;
        int hgt = col_height(disp, sy_cx, 0x2EE66Bu);
        printf("  scaleY(0.5) 的 40px 盒子在屏幕上是 %d px 高\n", hgt);
        QL_CHECK(hgt >= 18 && hgt <= 22);
        QL_CHECK_EQ(run_width(disp, sy_box.y + 20, 0x2EE66Bu), 40);    /* 宽度不动 */

        ql_disp_mem_destroy(disp);
    }

    /* ---------- 3. 缩到 0：彻底消失 ---------- */
    {
        QL_CHECK_EQ(ui->nodes[3].style->sc_x, 1000);
        (void)ql_ui_anim_tick(ui, 0);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 1000);

        s_now = 200;                                /* 一半 → 0.5 */
        (void)ql_ui_anim_tick(ui, 200);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 500);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_y, 1000);   /* 另一轴不动 */

        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);
        int mid = run_width(disp, fp_box.y + 20, 0x2E6BE6u);
        printf("  翻转半程（scaleX 0.5）的 40px 盒子宽 %d px\n", mid);
        QL_CHECK(mid >= 18 && mid <= 22);
        ql_disp_mem_destroy(disp);

        s_now = 400;                                /* 归零 */
        (void)ql_ui_anim_tick(ui, 400);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 0);

        disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);
        QL_CHECK_EQ(run_width(disp, fp_box.y + 20, 0x2E6BE6u), 0);
        QL_CHECK_EQ(col_height(disp, fp_box.x + 20, 0x2E6BE6u), 0);
        /* 连字也不能漏：零宽时文字一个都不画（否则会挤成一条墨线） */
        int ink = 0;
        for (ql_coord_t y = fp_box.y; y < ql_rect_bottom(fp_box); y++)
            for (ql_coord_t x = fp_box.x; x < ql_rect_right(fp_box); x++)
                if (ql_disp_mem_pixel(disp, x, y) != ROOT_BG)
                    ink++;
        QL_CHECK_EQ(ink, 0);
        ql_disp_mem_destroy(disp);

        /* 零面积时点不中（也不能除零）——命中的是背景 #root，不是翻转卡片 */
        QL_CHECK(ql_ui_hit_test(ui, fp_box.x + 20, fp_box.y + 20) != &ui->nodes[3]);
    }

    /* ---------- 4. 命中测试按压扁后的范围 ---------- */
    {
        ql_coord_t cy = sx_box.y + sx_box.h / 2;
        /* 中心还在：点得中 */
        QL_CHECK(ql_ui_hit_test(ui, sx_box.x + 20, cy) == &ui->nodes[1]);
        /* 被压掉的那 10px：布局盒里，视觉上已经没了 */
        QL_CHECK(ql_ui_hit_test(ui, sx_box.x + 4, cy) != &ui->nodes[1]);
        QL_CHECK(ql_ui_hit_test(ui, ql_rect_right(sx_box) - 4, cy) != &ui->nodes[1]);
        /* scaleY 同理：上下被压掉的部分点不中，中线还在 */
        ql_coord_t cx = sy_box.x + sy_box.w / 2;
        QL_CHECK(ql_ui_hit_test(ui, cx, sy_box.y + 20) == &ui->nodes[2]);
        QL_CHECK(ql_ui_hit_test(ui, cx, sy_box.y + 4) != &ui->nodes[2]);
    }

    /* ---------- 5. animation-delay：延迟里不许动 ---------- */
    {
        QL_CHECK(ui->nodes[4].style->anim != 0xFFFF);
        QL_CHECK_EQ(ui->nodes[4].style->sc_x, 1000);

        (void)ql_ui_anim_tick(ui, 0);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->sc_x, 1000);

        s_now = 200;                        /* 还在 300ms 延迟里 */
        (void)ql_ui_anim_tick(ui, 200);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->sc_x, 1000);

        s_now = 500;                        /* 延迟 300 + 动画走 200 → 一半 */
        (void)ql_ui_anim_tick(ui, 500);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->sc_x, 500);

        s_now = 700;                        /* 300 + 400，播完停在末帧（forwards） */
        (void)ql_ui_anim_tick(ui, 700);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->sc_x, 0);
    }

    /* ---------- 6. CPU 与 GPU 一致 ---------- */
#if defined(QLGL_ENABLE_GL)
    {
        s_now = 200;                    /* 用半程那一帧：两轴同时有非 1 值 */
        (void)ql_ui_anim_tick(ui, 200);

        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);

        QLGfxGL *gfx = ql_gfx_gl_create("flip", W, H, QL_GFX_GL_OFFSCREEN);
        if (gfx == NULL) {
            printf("  [SKIP] GL 对比：无法创建上下文\n");
        } else {
            ql_gfx_gl_begin(gfx, ROOT_BG);
            ql_ui_render(ui, ql_gfx_gl_draw(gfx));
            ql_gfx_gl_end(gfx);
            ql_gfx_gl_finish(gfx);

            uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
            QL_CHECK(rgba != NULL);
            if (rgba != NULL) {
                QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);
                long bad = 0;
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        ql_color_t c = ql_disp_mem_pixel(disp, x, y);
                        const uint8_t *p = rgba + ((size_t)y * W + (size_t)x) * 4u;
                        int dr = abs((int)QL_COLOR_GET_R(c) - (int)p[0]);
                        int dg = abs((int)QL_COLOR_GET_G(c) - (int)p[1]);
                        int db = abs((int)QL_COLOR_GET_B(c) - (int)p[2]);
                        int m = dr > dg ? dr : dg;
                        if (db > m) m = db;
                        if (m > 32) bad++;
                    }
                }
                double ratio = (double)bad / (double)(W * H);
                printf("  翻转 CPU vs GPU：%ld/%d = %.2f%%\n", bad, W * H,
                       ratio * 100.0);
                QL_CHECK(ratio <= 0.02);
                ql_mem_free(rgba);
            }
            ql_gfx_gl_destroy(gfx);
        }
        ql_disp_mem_destroy(disp);
    }
#endif

    ql_ui_free(ui);
    ql_anim_set_time_source(NULL);

QL_TEST_END()
