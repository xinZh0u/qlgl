/**
 * @file    example_bench_fps.c
 * @brief   实测渲染吞吐：GL(离屏) 与 CPU 后端，多种分辨率。
 *
 * 运行：./build/example_bench_fps [帧数]
 *
 * 说明：
 *   - 离屏模式没有 vsync，测的是纯吞吐上限；
 *   - 窗口模式的帧率受合成器/垂直同步限制，见 example_gl_window --bench。
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_demo.h"

#define MAX_FRAMES 400

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static const ql_coord_t RES[][2] = {
    { 320, 240 }, { 800, 480 }, { 1280, 720 }, { 1920, 1080 },
};

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 200;
    if (frames <= 0 || frames > MAX_FRAMES)
        frames = MAX_FRAMES;

    if (qlgl_init() != QL_OK)
        return 1;

    printf("%-12s %-8s %10s %10s %10s\n", "分辨率", "后端", "ms/帧", "fps", "顶点");
    printf("------------------------------------------------------\n");

    for (size_t r = 0; r < QL_ARRAY_SIZE(RES); r++) {
        ql_coord_t w = RES[r][0], h = RES[r][1];

        /* ---------- GL 离屏 ---------- */
#if defined(QLGL_ENABLE_GL)
        QLGfxGL *gfx = ql_gfx_gl_create("bench", w, h, QL_GFX_GL_OFFSCREEN);
        if (gfx != NULL) {
            QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
            QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
            ql_ui_layout(ui, w, h);

            /* 预热 */
            ql_gfx_gl_begin(gfx, QL_COLOR_BLACK);
            ql_ui_render(ui, d2d);
            ql_gfx_gl_end(gfx);

            double t0 = now_ms();
            for (int f = 0; f < frames; f++) {
                ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x08, 0x0A, 0x10));
                ql_ui_render(ui, d2d);
                ql_gfx_gl_end(gfx);
            }
            double t1 = now_ms();

            double ms = (t1 - t0) / (double)frames;
            printf("%-12s %-8s %10.3f %10.0f %10u\n",
                   r == 0 ? "320x240" : r == 1 ? "800x480" : r == 2 ? "1280x720" : "1920x1080",
                   "GL", ms, 1000.0 / ms, ql_gfx_gl_vertex_count(gfx));

            ql_ui_free(ui);
            ql_gfx_gl_destroy(gfx);
        } else {
            printf("%-12s %-8s %10s\n", "GL", "不可用", "-");
        }
#endif

        /* ---------- CPU 光栅 ---------- */
        QLDispDriver *disp = ql_disp_mem_create(w, h, QL_COLOR_RGB888);
        if (disp != NULL) {
            QLCanvas canvas;
            ql_canvas_from_disp(&canvas, disp);
            QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
            ql_ui_layout(ui, w, h);

            int cpu_frames = frames / 4 > 0 ? frames / 4 : 1;   /* CPU 慢，少跑几帧 */
            if (w >= 1280)
                cpu_frames = 20;

            ql_canvas_clear(&canvas, QL_COLOR_BLACK);
            ql_ui_render_canvas(ui, &canvas);

            double t0 = now_ms();
            for (int f = 0; f < cpu_frames; f++) {
                ql_canvas_clear(&canvas, QL_COLOR_RGB(0x08, 0x0A, 0x10));
                ql_ui_render_canvas(ui, &canvas);
            }
            double t1 = now_ms();

            double ms = (t1 - t0) / (double)cpu_frames;
            printf("%-12s %-8s %10.3f %10.0f %10s\n",
                   r == 0 ? "320x240" : r == 1 ? "800x480" : r == 2 ? "1280x720" : "1920x1080",
                   "CPU", ms, 1000.0 / ms, "-");

            ql_ui_free(ui);
            ql_disp_mem_destroy(disp);
        }
    }

    qlgl_shutdown();
    return 0;
}
