/**
 * @file    window_helper.h
 * @brief   示例程序共用的"优先开窗口"辅助：Wayland → X11 → 内存显示。
 *
 * 命令行：
 *   --frames N   跑 N 帧后自动退出（默认 0 = 一直运行到关闭窗口）
 *   --ppm PATH   无窗口时导出最后一帧
 *   --mem        强制使用内存显示（不开窗口）
 */
#ifndef QLGL_EXAMPLE_WINDOW_HELPER_H
#define QLGL_EXAMPLE_WINDOW_HELPER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

typedef enum {
    QL_WIN_KIND_MEM = 0,
    QL_WIN_KIND_WAYLAND,
    QL_WIN_KIND_X11,
} QLWinKind;

typedef struct {
    QLDispDriver *disp;
    QLWinKind     kind;
    long          max_frames;
    const char   *ppm_path;
    int           force_mem;
} QLWin;

static inline void ql_win_parse_args(QLWin *w, int argc, char **argv)
{
    memset(w, 0, sizeof(*w));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            w->max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
            w->ppm_path = argv[++i];
        else if (strcmp(argv[i], "--mem") == 0)
            w->force_mem = 1;
    }
}

static inline int ql_win_open(QLWin *w, const char *title, ql_coord_t width,
                              ql_coord_t height, QLColorFormat hint)
{
    w->disp = NULL;
    w->kind = QL_WIN_KIND_MEM;

    if (!w->force_mem) {
#if defined(QLGL_ENABLE_WAYLAND)
        w->disp = ql_disp_wayland_create(title, width, height, hint);
        if (w->disp != NULL)
            w->kind = QL_WIN_KIND_WAYLAND;
#endif
#if defined(QLGL_ENABLE_X11)
        if (w->disp == NULL) {
            w->disp = ql_disp_x11_create(title, width, height, hint);
            if (w->disp != NULL)
                w->kind = QL_WIN_KIND_X11;
        }
#endif
    }

    if (w->disp != NULL) {
        printf("显示：%s %dx%d %s\n",
               w->kind == QL_WIN_KIND_WAYLAND ? "Wayland 窗口" : "X11 窗口",
               (int)w->disp->width, (int)w->disp->height,
               ql_disp_format_name(w->disp->format));
        return 1;
    }

    w->disp = ql_disp_mem_create(width, height, QL_COLOR_RGB888);
    if (w->disp == NULL) {
        printf("创建显示失败\n");
        return 0;
    }
    printf("无窗口环境，使用内存显示（--ppm 可导出）\n");
    if (w->max_frames == 0)
        w->max_frames = 1;
    return 1;
}

/* 返回 1 继续，0 表示窗口已关闭 */
static inline int ql_win_poll(QLWin *w)
{
    switch (w->kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case QL_WIN_KIND_WAYLAND: return ql_disp_wayland_poll(w->disp);
#endif
#if defined(QLGL_ENABLE_X11)
    case QL_WIN_KIND_X11:     return ql_disp_x11_poll(w->disp);
#endif
    default:                  return 1;
    }
}

static inline void ql_win_close(QLWin *w)
{
    if (w->disp == NULL)
        return;

    switch (w->kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case QL_WIN_KIND_WAYLAND:
        ql_disp_wayland_destroy(w->disp);
        return;
#endif
#if defined(QLGL_ENABLE_X11)
    case QL_WIN_KIND_X11:
        ql_disp_x11_destroy(w->disp);
        return;
#endif
    default:
        if (w->ppm_path != NULL)
            ql_disp_mem_save_ppm(w->disp, w->ppm_path);
        ql_disp_mem_destroy(w->disp);
        return;
    }
}

#endif /* QLGL_EXAMPLE_WINDOW_HELPER_H */
