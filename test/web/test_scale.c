/**
 * @file    test_scale.c
 * @brief   transform: scale()：绘制期等比缩放，**不进布局**。
 *
 * 五条契约：
 *   1. 布局结果（border_box）与 scale 无关；
 *   2. 视觉上按 1.5× 放大，且绕元素中心；
 *   3. 命中测试按放大后的位置走（点了就看到）；
 *   4. @keyframes 里插值 scale，中途取中间值；
 *   5. CPU 与 GPU 结果一致。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_scale.h"

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

/* 找到某个底色像素横跨多少列（在给定的行上） */
static int run_width(QLDispDriver *disp, ql_coord_t y, ql_color_t want)
{
    int n = 0;
    for (ql_coord_t x = 0; x < W; x++)
        if (ql_disp_mem_pixel(disp, x, y) == want)
            n++;
    return n;
}

QL_TEST_BEGIN("test_scale")

    ql_anim_set_time_source(fake_now);

    /* ---------- 1. 布局与 scale 无关 ---------- */
    s_now = 0;
    QLUIRoot *ui = ql_ui_build(&ui_flex_scale_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_layout(ui, W, H);
    QL_CHECK_EQ(ui->node_count, 3);

    QLRect big = ql_ui_node_rect(&ui->nodes[1]);
    QL_CHECK_EQ(big.w, 40);              /* 布局还是 40x40 */
    QL_CHECK_EQ(big.h, 40);
    QL_CHECK_EQ(ui->nodes[1].style->sc_x, 1500);

    /* ---------- 2. 视觉上 1.5×，绕中心 ---------- */
    {
        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK(disp != NULL);
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);

        ql_coord_t cy = big.y + big.h / 2;
        int w = run_width(disp, cy, 0x2E6BE6u);
        printf("  scale(1.5) 的 40px 盒子在屏幕上是 %d px 宽\n", w);
        QL_CHECK(w >= 58 && w <= 62);      /* 40 * 1.5 = 60 */

        /* 绕中心：左右各多出 10px */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, big.x - 9, cy), 0x2E6BE6u);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(big) + 8, cy), 0x2E6BE6u);
        /* 原来盒子内、放大后仍在里面的角落是背景（四角在圆外？这里没圆角，所以是实心） */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, big.x + 20, big.y + 20), 0x2E6BE6u);

        ql_disp_mem_destroy(disp);
    }

    /* ---------- 3. 命中测试按放大后的范围 ---------- */
    {
        ql_coord_t cy = big.y + big.h / 2;
        /* 放大后伸出去的左边缘：能点中 */
        QL_CHECK(ql_ui_hit_test(ui, big.x - 8, cy) == &ui->nodes[1]);
        /* 更外面：点不中 */
        QL_CHECK(ql_ui_hit_test(ui, big.x - 14, cy) != &ui->nodes[1]);
    }

    /* ---------- 4. @keyframes 插值 ---------- */
    {
        QL_CHECK_EQ(ui->nodes[2].style->sc_x, 1000);
        (void)ql_ui_anim_tick(ui, 0);       /* 起始时刻 */
        s_now = 200;                        /* 一半 → 1.25 */
        (void)ql_ui_anim_tick(ui, 200);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[2])->sc_x, 1250);
        s_now = 400;
        (void)ql_ui_anim_tick(ui, 400);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[2])->sc_x, 1500);
    }

    /* ---------- 4.5 放大中的脏区必须真的够大 ---------- */
    {
        /*
         * scale 会让元素画到布局盒外面。paint_box 只看基础样式的话脏区会算小：
         * 上一帧多出来的那一圈擦不掉、新长出来的一圈画不上。
         * 这里复现真实主循环——先整幅画一帧，之后只重绘脏区，
         * 结果必须和"直接整幅重画这一帧"逐像素相同。
         */
        QLDispDriver *live = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLDispDriver *want = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas cl, cw;
        QL_CHECK_EQ(ql_canvas_from_disp(&cl, live), QL_OK);
        QL_CHECK_EQ(ql_canvas_from_disp(&cw, want), QL_OK);

        QLRect areas[8];
        (void)ql_ui_anim_restart(ui, &ui->nodes[2]);
        (void)ql_ui_consume_dirty(ui, areas, 8);

        /* 帧 A：整幅画（首帧总是全量） */
        ql_canvas_clear(&cl, ROOT_BG);
        ql_ui_render_canvas(ui, &cl);
        s_now = 100;
        (void)ql_ui_anim_tick(ui, 100);

        /* 帧 B：只重绘脏区 */
        ql_canvas_clear(&cl, ROOT_BG);
        ql_ui_render_canvas(ui, &cl);       /* 先把帧 A 的状态画上去 */
        s_now = 250;                        /* 1.188×，比布局盒大一圈 */
        (void)ql_ui_anim_tick(ui, 250);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[2])->sc_x, 1188);

        int n = ql_ui_consume_dirty(ui, areas, 8);
        QL_CHECK(n >= 1);
        QLDraw2D d2d;
        QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &cl), QL_OK);
        ui->root_clip_valid = 0;
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(ui, &d2d, areas[i]);

        /* 期望：同一时刻整幅重画 */
        ql_canvas_clear(&cw, ROOT_BG);
        ql_ui_render_canvas(ui, &cw);

        long bad = 0;
        for (ql_coord_t y = 0; y < H; y++) {
            for (ql_coord_t x = 0; x < W; x++)
                if (ql_disp_mem_pixel(live, x, y) != ql_disp_mem_pixel(want, x, y))
                    bad++;
        }
        printf("  1.188× 放大中：局部重绘 vs 整幅重画 %ld 像素不一致\n", bad);
        QL_CHECK_EQ(bad, 0);

        ql_disp_mem_destroy(live);
        ql_disp_mem_destroy(want);
        (void)ql_ui_anim_tick(ui, 400);
    }

    /* ---------- 5. CPU 与 GPU 一致 ---------- */
#if defined(QLGL_ENABLE_GL)
    {
        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);

        QLGfxGL *gfx = ql_gfx_gl_create("scale", W, H, QL_GFX_GL_OFFSCREEN);
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
                printf("  scale CPU vs GPU：%ld/%d = %.2f%%\n", bad, W * H,
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
