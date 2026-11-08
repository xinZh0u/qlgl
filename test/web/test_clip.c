/**
 * @file    test_clip.c
 * @brief   clip-path: circle() 圆形揭示：遮罩必须作用于**整棵子树**。
 *
 * 四条契约：
 *   1. 半径 0% 时什么都不画（连子元素也不见）；
 *   2. 半径 100% 时完全恢复；
 *   3. 中间状态：圆内有内容、圆外是父节点背景；
 *   4. **子元素**（故意画得比父大）也被一起裁掉——这是"子树遮罩"和
 *      "只裁元素自己的背景"（`border-radius` 做得到的那种）的关键区别。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"
#include "ui_flex_clip.h"

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

/*
 * 在时刻 t 渲染一帧：新建 ui → 布局 → tick 到 t → 画到新的内存显示。
 * 返回驱动，*out_ui 由调用方 ql_ui_free。
 */
static QLDispDriver *render_at(uint32_t t, QLUIRoot **out_ui, QLCanvas *canvas)
{
    *out_ui = NULL;
    QLUIRoot *ui = ql_ui_build(&ui_flex_clip_doc);
    if (ui == NULL)
        return NULL;
    ql_ui_layout(ui, W, H);
    s_now = 0;
    (void)ql_ui_anim_tick(ui, 0);       /* 起始时刻 = 0 */
    s_now = t;
    (void)ql_ui_anim_tick(ui, t);

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    if (disp == NULL) {
        ql_ui_free(ui);
        return NULL;
    }
    QL_CHECK_EQ(ql_canvas_from_disp(canvas, disp), QL_OK);
    ql_disp_mem_clear(disp, ROOT_BG);
    ql_ui_render_canvas(ui, canvas);
    *out_ui = ui;
    return disp;
}

/* 统计非背景像素 */
static long ink_count(QLDispDriver *disp)
{
    long n = 0;
    for (ql_coord_t y = 0; y < H; y++)
        for (ql_coord_t x = 0; x < W; x++)
            if (ql_disp_mem_pixel(disp, x, y) != ROOT_BG)
                n++;
    return n;
}

QL_TEST_BEGIN("test_clip")

    ql_anim_set_time_source(fake_now);

    QLUIRoot *ui = NULL;
    QLCanvas canvas;
    QLRect box;

    /* ---------- 0. 文档结构 ---------- */
    {
        QLUIRoot *probe = ql_ui_build(&ui_flex_clip_doc);
        QL_CHECK(probe != NULL);
        if (probe == NULL) {
            printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
            return 1;
        }
        QL_CHECK_EQ(probe->node_count, 4);          /* root + box + kid + kid2 */
        ql_ui_layout(probe, W, H);
        box = ql_ui_node_rect(&probe->nodes[1]);
        QL_CHECK(!ql_rect_is_empty(box));
        ql_ui_free(probe);
    }

    /* ---------- 1. 0%：什么都不画 ---------- */
    {
        QLDispDriver *disp = render_at(0, &ui, &canvas);
        QL_CHECK(disp != NULL);
        if (disp != NULL) {
            QL_CHECK_EQ(ink_count(disp), 0);
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x + 30, box.y + 20), ROOT_BG);
            ql_disp_mem_destroy(disp);
        }
        ql_ui_free(ui);
    }

    /* ---------- 2. 100%：完全恢复 ---------- */
    long ink_full = 0;
    {
        QLDispDriver *disp = render_at(400, &ui, &canvas);
        QL_CHECK(disp != NULL);
        if (disp != NULL) {
            ink_full = ink_count(disp);
            QL_CHECK(ink_full > 0);
            /* 父盒子自己的底色（避开两个子元素） */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x + 30, box.y + 20), 0x204060u);
            /* 子元素在父盒子外面也能看到（100% 时圆盖住了它们） */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x - 8, box.y + 40), 0xFF8000u);
            ql_disp_mem_destroy(disp);
        }
        ql_ui_free(ui);
    }

    /* ---------- 3. 50%：圆内有、圆外无，子元素一起被裁 ---------- */
    {
        QLDispDriver *disp = render_at(200, &ui, &canvas);
        QL_CHECK(disp != NULL);
        if (disp != NULL) {
            long ink_mid = ink_count(disp);
            printf("  0%%=%d 墨迹，50%%=%ld，100%%=%ld\n", 0, ink_mid, ink_full);
            QL_CHECK(ink_mid > 0);
            QL_CHECK(ink_mid < ink_full);

            /* 圆内（避开子元素）是父盒子底色 */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x + 30, box.y + 20), 0x204060u);
            /* 盒子四角在圆外 → 只有父背景 */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x, box.y), ROOT_BG);
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(box) - 1,
                                          ql_rect_bottom(box) - 1),
                        ROOT_BG);
            /*
             * 关键：子元素伸出父盒子的那一段在圆外，必须看不见。
             * .kid 在父盒子的中线附近向左伸出 12px；50% 的半径盖不到那里。
             */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, box.x - 8, box.y + 40), ROOT_BG);
            ql_disp_mem_destroy(disp);
        }
        ql_ui_free(ui);
    }

    /* ---------- 4. GPU 与 CPU 的遮罩一致 ---------- */
#if defined(QLGL_ENABLE_GL)
    {
        QLDispDriver *disp = render_at(200, &ui, &canvas);
        QL_CHECK(disp != NULL);
        if (disp != NULL) {
            QLGfxGL *gfx = ql_gfx_gl_create("clip", W, H, QL_GFX_GL_OFFSCREEN);
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
                            const uint8_t *p =
                                rgba + ((size_t)y * W + (size_t)x) * 4u;
                            int dr = abs((int)QL_COLOR_GET_R(c) - (int)p[0]);
                            int dg = abs((int)QL_COLOR_GET_G(c) - (int)p[1]);
                            int db = abs((int)QL_COLOR_GET_B(c) - (int)p[2]);
                            int m = dr > dg ? dr : dg;
                            if (db > m) m = db;
                            if (m > 32) bad++;
                        }
                    }
                    double ratio = (double)bad / (double)(W * H);
                    printf("  遮罩 50%% CPU vs GPU：%ld/%d = %.2f%%\n", bad, W * H,
                           ratio * 100.0);
                    QL_CHECK(ratio <= 0.03);
                    ql_mem_free(rgba);
                }
                ql_gfx_gl_destroy(gfx);
            }
        }
        ql_ui_free(ui);
        if (disp != NULL)
            ql_disp_mem_destroy(disp);
    }
#endif

    ql_anim_set_time_source(NULL);

QL_TEST_END()
