/**
 * @file    common.h
 * @brief   例程共用的几件小事：读文件、挑字体、跑主循环。
 *
 * 每个例程只讲自己那一件事，样板代码都在这里，避免读者被噪音干扰。
 */
#ifndef QL_EXAMPLE_COMMON_H
#define QL_EXAMPLE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#if !defined(QLGL_ENABLE_GL)
#define HAS_GL 0
#else
#define HAS_GL 1
#endif

/* 找一个能显示中文的字体；找不到就退回内置位图字体（只有 ASCII） */
static void ex_load_font(void)
{
    const char *env = getenv("QLGL_FONT");
    if (env != NULL && ql_font_set_default_file(env, -1))
        return;
    const char *cands[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (ql_font_set_default_file(cands[i], -1))
            return;
}

/*
 * 把界面渲染成一帧 PPM（没有窗口/没有 GPU 也能验证结果，也方便文档配图）。
 */
static int ex_dump_ppm(QLUIRoot *ui, int w, int h, const char *path)
{
    QLDispDriver *disp = ql_disp_mem_create((ql_coord_t)w, (ql_coord_t)h,
                                            QL_COLOR_RGB888);
    if (disp == NULL)
        return 0;
    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK) {
        ql_disp_mem_destroy(disp);
        return 0;
    }
    ql_canvas_clear(&canvas, 0x000000);
    ql_ui_render_canvas(ui, &canvas);
    const int ok = (ql_disp_mem_save_ppm(disp, path) == QL_OK);
    ql_disp_mem_destroy(disp);
    return ok;
}

/*
 * 打开一个真实窗口跑主循环（需要 -DQLGL_BUILD_GL=ON 且有 EGL/X11）。
 * `--frames N` 跑 N 帧后退出，`--dump out.ppm` 存最后一帧；没有窗口时自动
 * 退回离屏渲染 + PPM，所以这个函数在没有显示器的机器上也能跑通。
 */
static int ex_run(QLUIRoot *ui, int w, int h, const char *title, int argc,
                  char **argv)
{
    long max_frames = 0;
    const char *dump = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
    }

#if HAS_GL
    QLGfxGL *gfx = ql_gfx_gl_create(title, w, h, QL_GFX_GL_WINDOW);
    if (gfx != NULL) {
        ql_ui_layout(ui, w, h);
        QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
        long frame = 0;
        while (ql_gfx_gl_poll(gfx)) {
            ql_input_dispatch();
            (void)ql_ui_anim_tick(ui, ql_anim_now_ms());
            ql_gfx_gl_begin(gfx, 0x0B0E13);
            ql_ui_render(ui, d2d);
            ql_gfx_gl_end(gfx);
            if (max_frames > 0 && ++frame >= max_frames)
                break;
        }
        if (dump != NULL) {
            /* 窗口关掉之前先抓一帧 */
            if (ex_dump_ppm(ui, w, h, dump))
                printf("已写出 %s\n", dump);
        }
        ql_gfx_gl_destroy(gfx);
        return 0;
    }
    printf("（没有可用窗口，改为离屏渲染）\n");
#endif

    ql_ui_layout(ui, w, h);
    if (dump != NULL) {
        if (ex_dump_ppm(ui, w, h, dump))
            printf("已写出 %s\n", dump);
        else
            printf("写出 %s 失败\n", dump);
    } else {
        printf("没有窗口也没给 --dump，什么也没做。\n");
    }
    return 0;
}

#endif /* QL_EXAMPLE_COMMON_H */
