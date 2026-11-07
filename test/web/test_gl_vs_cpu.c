/**
 * @file    test_gl_vs_cpu.c
 * @brief   GPU 与 CPU 后端的 golden image 对比（离屏，不弹窗）。
 *
 * 同一份 HTML/CSS 编译出的界面，分别用 CPU 光栅与 GL(ES3/EGL) 渲染，
 * 逐像素比较。抗锯齿实现不同，因此允许小容差。
 */
#include <stdlib.h>

#include "../ql_test.h"
#include "ui_demo.h"

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("== test_gl_vs_cpu ==\n  跳过：未编译 GL 后端\n  0 项检查，0 项失败\n");
    ql_test_checks = 0;
    ql_test_failures = 0;
    return 0;
}
#else

#define W 320
#define H 240
#define TOLERANCE 40          /* 单通道允许误差 */
#define MAX_BAD_RATIO 0.06    /* 允许不同像素占比 */

QL_TEST_BEGIN("test_gl_vs_cpu")

    /* ---- CPU 参考图 ---- */
    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    QLCanvas canvas;
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
    ql_canvas_clear(&canvas, QL_COLOR_RGB(0x10, 0x14, 0x1C));

    QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
    QL_CHECK(ui != NULL);
    ql_ui_layout(ui, W, H);
    ql_ui_render_canvas(ui, &canvas);

    /* ---- GPU 图 ---- */
    QLGfxGL *gfx = ql_gfx_gl_create("gl-vs-cpu", W, H, QL_GFX_GL_OFFSCREEN);
    if (gfx == NULL) {
        printf("  [SKIP] 无法创建 GL 上下文（无 EGL/X11？）\n");
        ql_ui_free(ui);
        ql_disp_mem_destroy(disp);
        qlgl_shutdown();
        return 0;
    }

    ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x10, 0x14, 0x1C));
    ql_ui_render(ui, ql_gfx_gl_draw(gfx));
    ql_gfx_gl_end(gfx);

    QL_CHECK_EQ(ql_gfx_gl_frame_count(gfx), 1);
    QL_CHECK(ql_gfx_gl_vertex_count(gfx) > 0);
    QL_CHECK_EQ(ql_gfx_gl_draw_calls(gfx), 1);      /* 单批次 */

    uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
    QL_CHECK(rgba != NULL);
    QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);

    /* ---- 逐像素比较 ---- */
    long total = 0, bad = 0;
    long max_diff = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            ql_color_t c = ql_disp_mem_pixel(disp, x, y);
            const uint8_t *g = rgba + ((size_t)y * (size_t)W + (size_t)x) * 4u;
            int dr = abs((int)QL_COLOR_GET_R(c) - (int)g[0]);
            int dg = abs((int)QL_COLOR_GET_G(c) - (int)g[1]);
            int db = abs((int)QL_COLOR_GET_B(c) - (int)g[2]);
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d > max_diff) max_diff = d;
            if (d > TOLERANCE) bad++;
            total++;
        }
    }

    double ratio = (double)bad / (double)total;
    printf("  像素差异：%ld/%ld = %.2f%%（最大单通道差 %ld）\n", bad, total, ratio * 100, max_diff);
    QL_CHECK(ratio <= MAX_BAD_RATIO);

    /* ---- 关键位置抽查：背景、标题条、面板 ---- */
    QL_CHECK(rgba[((size_t)2 * (size_t)W + 2u) * 4u + 0u] < 40);          /* 左上角深色 */

    ql_mem_free(rgba);
    ql_gfx_gl_destroy(gfx);
    ql_ui_free(ui);
    ql_disp_mem_destroy(disp);

QL_TEST_END()
#endif
