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
#if defined(QLGL_ENABLE_WAYLAND)
#include <qlgl/qldisp_wayland.h>
#endif
#if defined(QLGL_ENABLE_X11)
#include <qlgl/qldisp_x11.h>
#endif

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

    /* AUTO 只对窗口后端有意义；内存显示要一个具体格式 */
    QLColorFormat mem_fmt = (hint == QL_COLOR_FORMAT_AUTO) ? QL_COLOR_RGB888 : hint;
    w->disp = ql_disp_mem_create(width, height, mem_fmt);
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

/*
 * 尺寸/缩放变过？（需要重建画布并重排）
 *
 * Wayland 上 `wl_output.scale` 一变，帧缓冲会按 `css × scale` 重新分配，
 * 画布必须跟着重建，否则会画到已释放的内存上。X11 上是窗口被拖动缩放。
 */
static inline int ql_win_resize_pending(QLWin *w)
{
    if (w == NULL || w->disp == NULL)
        return 0;
    switch (w->kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case QL_WIN_KIND_WAYLAND: return ql_disp_wayland_resize_pending(w->disp);
#endif
#if defined(QLGL_ENABLE_X11)
    case QL_WIN_KIND_X11:     return ql_disp_x11_resize_pending(w->disp);
#endif
    default:                  return 0;
    }
}

/* 窗口逻辑尺寸（CSS px）；后端没起来时退回内存尺寸 */
static inline ql_coord_t ql_win_css_width(const QLWin *w)
{
    if (w != NULL && w->disp != NULL && w->kind != QL_WIN_KIND_MEM)
        return ql_disp_css_width(w->disp);
    return (w != NULL && w->disp != NULL) ? w->disp->width : 0;
}

static inline ql_coord_t ql_win_css_height(const QLWin *w)
{
    if (w != NULL && w->disp != NULL && w->kind != QL_WIN_KIND_MEM)
        return ql_disp_css_height(w->disp);
    return (w != NULL && w->disp != NULL) ? w->disp->height : 0;
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

/* ------------------------- 默认矢量字体 ------------------------- */

/*
 * 示例统一在这里找一个可用的中文字体并设为默认：
 *   1. 环境变量 QLGL_FONT 指定；
 *   2. 常见发行版路径依次尝试；
 *   3. 都没有就返回 0，界面回落到内置 ASCII 位图字体。
 */
static inline int ql_win_load_font(void)
{
#if defined(QLGL_ENABLE_FREETYPE)
    const char *env = getenv("QLGL_FONT");
    if (env != NULL && env[0] != '\0') {
        if (ql_font_set_default_file(env, -1)) {
            printf("字体：%s（QLGL_FONT）\n", env);
            return 1;
        }
        printf("字体：QLGL_FONT=%s 加载失败，尝试系统字体\n", env);
    }

    static const char *const candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (ql_font_set_default_file(candidates[i], -1)) {
            printf("字体：%s\n", candidates[i]);
            return 1;
        }
    }
    printf("字体：未找到矢量字体，只用内置 ASCII 位图字体\n");
#else
    printf("字体：未编译 FreeType 后端，只用内置 ASCII 位图字体\n");
#endif
    return 0;
}

#endif /* QLGL_EXAMPLE_WINDOW_HELPER_H */
