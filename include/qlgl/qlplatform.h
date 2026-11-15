/**
 * @file    qlplatform.h
 * @brief   平台后端：窗口/显示、事件泵、输入与可选剪贴板/输入法。
 *
 * `QLWindow` 只依赖这张回调表，不知道底下是 X11、Wayland 还是 MCU 的板级驱动。
 * 挑选顺序：
 *
 *   1. 应用在 `qlgl_init()` 之前调 `qlgl_set_platform(&my_platform)`；
 *   2. 否则用 `qlgl_platform_default()`（弱符号）。桌面构建链到
 *      `ql_platform_desktop()`；MCU 板级文件定义同名强符号即可覆盖，
 *      应用层仍然只写 `qlgl_init()`。
 *
 * 一个后端要提供的最小集合：`window_open/close`、`poll`、`wait`、`size`、
 * `begin/draw/end`、`dump_ppm`。剪贴板/输入法/垂直同步可以留 NULL。
 *
 * `begin` 的脏区语义与 `ql_gfx_gl_begin_rects()` 相同：`n == 0` 表示整屏；
 * 返回 1 表示后端要求整屏重画（首帧/FBO 刚建/不支持局部）。
 */
#ifndef QLGL_PLATFORM_H
#define QLGL_PLATFORM_H

#include "qlgl.h"

typedef struct QLPlatformWindow QLPlatformWindow;

typedef struct QLPlatform {
    const char *name;

    /* backend 取 QLWindowBackend（QL_WINDOW_GL / CPU / OFFSCREEN） */
    QLPlatformWindow *(*window_open)(const char *title, ql_coord_t w, ql_coord_t h,
                                     int backend);
    void (*window_close)(QLPlatformWindow *pw);

    /* 处理事件；1 = 继续，0 = 窗口已关 */
    int  (*poll)(QLPlatformWindow *pw);
    /* 空闲等待；1 = 继续，0 = 已关。可以留 NULL（不等待） */
    int  (*wait)(QLPlatformWindow *pw, int timeout_ms);
    void (*size)(QLPlatformWindow *pw, ql_coord_t *w, ql_coord_t *h);
    /* 尺寸/缩放变了：取一次清零。返回 1 时 QLWindow 会重新布局 */
    int  (*resize_pending)(QLPlatformWindow *pw);

    /* 绘制一帧：清脏区 → 用 draw() 的接口画 → end() 呈现 */
    int  (*begin)(QLPlatformWindow *pw, ql_color_t bg, const QLRect *dirty, int n);
    QLDraw2D *(*draw)(QLPlatformWindow *pw);
    void (*end)(QLPlatformWindow *pw);

    void (*set_vsync)(QLPlatformWindow *pw, int on);

    /* 把当前界面渲染成 PPM（不经过正常帧循环，用于截图/CI） */
    int  (*dump_ppm)(QLPlatformWindow *pw, QLUIRoot *ui, ql_color_t bg,
                     const char *path);

    /* 可选能力（没有就留 NULL） */
    void (*attach_clipboard)(QLPlatformWindow *pw, QLUIRoot *ui);
    void (*attach_ime)(QLPlatformWindow *pw, QLUIRoot *ui);
    void (*set_ime_spot)(QLPlatformWindow *pw, ql_coord_t x, ql_coord_t y,
                         ql_coord_t h);

    /* 低层句柄：`ql_window_gfx()` 用 "gl" 取 QLGfxGL*；没有返回 NULL */
    void *(*native)(QLPlatformWindow *pw, const char *what);

    /* 性能统计（没有就返回 0） */
    uint32_t (*frame_us)(QLPlatformWindow *pw);
    uint32_t (*swap_us)(QLPlatformWindow *pw);
} QLPlatform;

/* 在 qlgl_init() 之前调用；传 NULL 恢复默认后端 */
void qlgl_set_platform(const QLPlatform *platform);
/* 当前生效的后端（未注册时返回默认后端；可能是 NULL） */
const QLPlatform *qlgl_platform(void);

/*
 * 默认后端（弱符号：MCU 板级文件可以定义同名强符号覆盖）。
 * 桌面实现见 `ql_platform_desktop()`；没有任何窗口系统时返回 NULL。
 */
const QLPlatform *qlgl_platform_default(void);

/* 桌面后端（X11 / Wayland + GL；离屏时用内存画布） */
const QLPlatform *ql_platform_desktop(void);

#endif /* QLGL_PLATFORM_H */
