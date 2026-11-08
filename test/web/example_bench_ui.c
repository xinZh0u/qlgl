/**
 * @file    example_bench_ui.c
 * @brief   QLUI 层的性能基准：布局、全量重绘、局部重绘、文本量。
 *
 * 用法：
 *   ./build/example_bench_ui            打印各阶段耗时
 *   ./build/example_bench_ui --iter 500 每个项目跑多少轮（默认 200）
 *   ./build/example_bench_ui --gl       额外测 GL 提交
 *
 * 目标（见 doc/开发规划.md §3.1）：
 *   100 节点静态页全量重绘 ≤ 0.3 ms
 *   单属性变更触发的重排 ≤ 0.1 ms
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_flex_bench100.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 480
#define H 360

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    long iter = 200;
    int with_gl = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iter") == 0 && i + 1 < argc)
            iter = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--gl") == 0)
            with_gl = 1;
    }

    if (qlgl_init() != QL_OK)
        return 1;

    /* 有矢量字体就用，保证与真实场景一致 */
    {
        const char *cands[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
            if (ql_font_set_default_file(cands[i], -1))
                break;
    }

    QLUIRoot *ui = ql_ui_build(&ui_flex_bench100_doc);
    if (ui == NULL) {
        printf("build failed\n");
        return 1;
    }
    printf("节点数：%u（%dx%d）\n", (unsigned)ui->node_count, W, H);

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    QLCanvas canvas;
    ql_canvas_from_disp(&canvas, disp);

    /* ---------- 1. 布局 ---------- */
    long long t0 = now_ns();
    for (long i = 0; i < iter; i++)
        ql_ui_layout(ui, W, H);
    long long t1 = now_ns();
    double layout_ms = (double)(t1 - t0) / 1e6 / (double)iter;

    /* ---------- 2. 全量绘制（CPU 光栅） ---------- */
    ql_ui_layout(ui, W, H);
    t0 = now_ns();
    for (long i = 0; i < iter; i++) {
        ql_canvas_clear(&canvas, QL_COLOR_RGB(0x10, 0x14, 0x1C));
        ql_ui_render_canvas(ui, &canvas);
    }
    t1 = now_ns();
    double render_ms = (double)(t1 - t0) / 1e6 / (double)iter;

    /* ---------- 3. 小区域重绘（模拟局部更新） ---------- */
    QLRect dirty = ql_rect_make(100, 100, 80, 60);
    t0 = now_ns();
    for (long i = 0; i < iter; i++) {
        QLCanvas sub = canvas;
        ql_canvas_set_clip(&sub, dirty);
        ql_canvas_clear(&sub, QL_COLOR_RGB(0x10, 0x14, 0x1C));
        ql_ui_render_canvas(ui, &sub);
    }
    t1 = now_ns();
    double partial_ms = (double)(t1 - t0) / 1e6 / (double)iter;

    /* ---------- 4. 改一个文本（触发 layout） ---------- */
    QLUINodeRT *victim = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strncmp(ql_ui_node_text(&ui->nodes[i]), "c5_5", 4) == 0) {
            victim = &ui->nodes[i];
            break;
        }
    }
    t0 = now_ns();
    if (victim != NULL) {
        for (long i = 0; i < iter; i++)
            ql_ui_set_text(ui, victim, (i & 1) ? "c5_5" : "c5_6");
    }
    t1 = now_ns();
    double settext_ms = (double)(t1 - t0) / 1e6 / (double)iter;

    /* ---------- 5. 命中测试 ---------- */
    t0 = now_ns();
    for (long i = 0; i < iter; i++)
        (void)ql_ui_hit_test(ui, 123, 145);
    t1 = now_ns();
    double hit_ms = (double)(t1 - t0) / 1e6 / (double)iter;

    /*
     * 验收口径（doc/开发规划.md §3.1）："100 节点全量重绘 ≤ 0.3 ms"。
     * 这一页节点数不是 100，所以同时打印归一化值，避免拿绝对值和 100 节点的阈值比。
     */
    const double per100 = 100.0 * render_ms / (double)ui->node_count;

    printf("\n%-22s %10s %12s\n", "阶段", "ms/次", "目标");
    printf("------------------------------------------------------------------\n");
    printf("%-22s %10.3f %12s\n", "layout（全树）", layout_ms,
           (layout_ms <= 0.1) ? "≤0.1 ✅" : "≤0.1 ⚠");
    printf("%-22s %10.3f %12s\n", "render（全量 CPU）", render_ms,
           (per100 <= 0.3) ? "≤0.3/百节点 ✅" : "≤0.3/百节点 ⚠");
    printf("%-22s %10.3f %12s\n", "render（80×60 局部）", partial_ms, "—");
    printf("%-22s %10.3f %12s\n", "set_text（单节点）", settext_ms, "—");
    printf("%-22s %10.3f %12s\n", "hit_test", hit_ms, "—");
    printf("\n局部/全量：%.1f%%\n", 100.0 * partial_ms / render_ms);
    printf("归一化：%.4f ms / 100 节点（%u 节点）\n", per100,
           (unsigned)ui->node_count);

#if defined(QLGL_ENABLE_GL)
    if (with_gl) {
        QLGfxGL *gfx = ql_gfx_gl_create("bench", W, H, QL_GFX_GL_OFFSCREEN);
        if (gfx != NULL) {
            QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
            t0 = now_ns();
            for (long i = 0; i < iter; i++) {
                ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x10, 0x14, 0x1C));
                ql_ui_render(ui, d2d);
                ql_gfx_gl_end(gfx);
            }
            t1 = now_ns();
            printf("GL 全量（含 GPU 完成）：%.3f ms/次，%u 顶点，%u draw call\n",
                   (double)(t1 - t0) / 1e6 / (double)iter,
                   ql_gfx_gl_vertex_count(gfx), ql_gfx_gl_draw_calls(gfx));
            ql_gfx_gl_destroy(gfx);
        }
    }
#else
    (void)with_gl;
#endif

    ql_ui_free(ui);
    ql_disp_mem_destroy(disp);
    ql_font_reset_defaults();
    qlgl_shutdown();
    return 0;
}
