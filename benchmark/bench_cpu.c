/**
 * @file    bench_cpu.c
 * @brief   CPU 光栅后端（QLCanvas）的可视化 benchmark。
 *
 *   ./build/benchmark/bench_cpu                      # 开窗口，左上角实时帧率
 *   ./build/benchmark/bench_cpu --size 1280x720 --cards 200
 *   ./build/benchmark/bench_cpu --offscreen --frames 300   # 不开窗口，只打数字
 *   ./build/benchmark/bench_cpu --vsync 0            # CPU 后端没有 vsync，参数无副作用
 *
 * 每一帧做三件事：清屏 → 整屏重画界面 → 画 HUD。**整屏**是刻意的：
 * 它测的是光栅吞吐（填充 + 圆角 + 渐变 + 阴影 + 字形），
 * 局部重绘的能力另有 `test_gl_dirty`（GPU）和例程 05 覆盖。
 *
 * `render` 那个数字是"清屏 + 绘制 + 推给窗口"的净耗时，
 * `frame` 是含限速睡眠在内的整帧 —— 两者差得多说明时间花在限速上，不是画得慢。
 */
#include "bench_common.h"

#if defined(QLGL_ENABLE_X11)
#include <qlgl/qldisp_x11.h>
#endif
#if defined(QLGL_ENABLE_WAYLAND)
#include <qlgl/qldisp_wayland.h>
#endif

int main(int argc, char **argv)
{
    BenchOpts o;
    bench_opts_parse(&o, argc, argv);

    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = bench_build_scene(o.cards, o.cjk);
    if (doc == NULL) {
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }

    /* ---------- 后端：窗口优先，失败退回内存显示 ---------- */
    QLDispDriver *disp = NULL;
    enum { KIND_MEM = 0, KIND_WAYLAND, KIND_X11 } kind = KIND_MEM;
    const char *detail = "内存显示（无窗口）";
    if (!o.offscreen) {
#if defined(QLGL_ENABLE_WAYLAND)
        disp = ql_disp_wayland_create("QLGL bench · CPU", o.w, o.h,
                                      QL_COLOR_FORMAT_AUTO);
        if (disp != NULL) {
            kind = KIND_WAYLAND;
            detail = "Wayland 窗口";
        }
#endif
#if defined(QLGL_ENABLE_X11)
        if (disp == NULL) {
            disp = ql_disp_x11_create("QLGL bench · CPU", o.w, o.h,
                                      QL_COLOR_FORMAT_AUTO);
            if (disp != NULL) {
                kind = KIND_X11;
                detail = "X11 窗口";
            }
        }
#endif
    }
    const int windowed = (kind != KIND_MEM);
    if (disp == NULL) {
        disp = ql_disp_mem_create(o.w, o.h, QL_COLOR_RGB888);
        if (disp == NULL) {
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
            qlgl_shutdown();
            return 1;
        }
        printf("（没有窗口，改用内存显示；--frames 生效）\n");
        if (o.frames <= 0 && o.seconds <= 0.0)
            o.frames = 300;      /* 内存显示没有"关窗口"，给个默认上限 */
    }

    /* 布局用 CSS px；面板可能有 scale，这里按驱动给的算 */
    const ql_coord_t cw = ql_disp_css_width(disp);
    const ql_coord_t ch = ql_disp_css_height(disp);
    ql_ui_layout(ui, cw, ch);
    ql_ui_attach_input(ui);

    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK) {
        printf("无法从显示创建画布\n");
        ql_ui_detach_input();
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        ql_disp_mem_destroy(disp);
        qlgl_shutdown();
        return 1;
    }

    /* HUD 要自己一份 QLDraw2D：ql_ui_render_canvas 内部的临时实例用完就丢 */
    QLDraw2D hud_d2d;
    if (ql_draw2d_canvas_init(&hud_d2d, &canvas) != QL_OK) {
        printf("无法创建 QLDraw2D\n");
        ql_ui_detach_input();
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        ql_disp_mem_destroy(disp);
        qlgl_shutdown();
        return 1;
    }

    const ql_color_t bg = ex_clear_color(ui);
    BenchFps fps;
    bench_fps_init(&fps);
    BenchHudInfo info;
    memset(&info, 0, sizeof(info));
    info.backend = "CPU / QLCanvas";
    info.detail = detail;
    info.nodes = (int)ui->node_count;

    printf("CPU benchmark：%dx%d，%d 张卡片，%d 个节点，HUD %s\n",
           (int)cw, (int)ch, o.cards, (int)ui->node_count, o.hud ? "开" : "关");

    const double t_start = bench_now_ms();
    long frames = 0;
    int alive = 1;
    while (alive) {
        switch (kind) {
#if defined(QLGL_ENABLE_WAYLAND)
        case KIND_WAYLAND: alive = ql_disp_wayland_poll(disp); break;
#endif
#if defined(QLGL_ENABLE_X11)
        case KIND_X11:     alive = ql_disp_x11_poll(disp);     break;
#endif
        default:           break;
        }

        ql_input_dispatch();
        (void)ql_ui_anim_tick(ui, ql_anim_now_ms());

        const double f0 = bench_now_ms();

        ql_canvas_clear(&canvas, bg);
        ql_ui_render_canvas(ui, &canvas);
        if (o.hud)
            bench_hud_draw(&hud_d2d, cw, ch, &fps, &info);
        if (disp->flush != NULL)
            disp->flush(disp, ql_rect_make(0, 0, disp->width, disp->height));
        if (disp->flush_done != NULL)
            disp->flush_done(disp);

        const double f1 = bench_now_ms();
        frames++;
        info.busy_ms = f1 - f0;
        bench_fps_add(&fps, f1 - f0);

        if (bench_should_stop(&o, frames, t_start))
            break;
        bench_limit_fps(o.fps_cap, f0);
    }

    /* dump 用内存显示走一遍，拿到和窗口一致的一帧 */
    if (o.dump != NULL) {
        QLDispDriver *mem = ql_disp_mem_create(cw, ch, QL_COLOR_RGB888);
        if (mem != NULL) {
            QLCanvas mc;
            if (ql_canvas_from_disp(&mc, mem) == QL_OK) {
                QLDraw2D dd;
                ql_canvas_clear(&mc, bg);
                ql_ui_render_canvas(ui, &mc);
                /* 截图里要有 HUD，否则文档上看不出"这是个 benchmark" */
                if (o.hud && ql_draw2d_canvas_init(&dd, &mc) == QL_OK)
                    bench_hud_draw(&dd, cw, ch, &fps, &info);
                if (ql_disp_mem_save_ppm(mem, o.dump) == QL_OK)
                    printf("已写出 %s\n", o.dump);
            }
            ql_disp_mem_destroy(mem);
        }
    }

    bench_report("QLGL benchmark · CPU", &o, &fps, &info);

    ql_ui_detach_input();
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    switch (kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case KIND_WAYLAND: ql_disp_wayland_destroy(disp); break;
#endif
#if defined(QLGL_ENABLE_X11)
    case KIND_X11:     ql_disp_x11_destroy(disp);     break;
#endif
    default:           ql_disp_mem_destroy(disp);     break;
    }
    qlgl_shutdown();
    return 0;
}
