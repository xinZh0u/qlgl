/**
 * @file    qlplatform_desktop.c
 * @brief   桌面平台后端：GL（EGL）与 CPU 光栅（X11 / Wayland / 内存）。
 *
 * `QLWindow` 只通过 `QLPlatform` 调这里；换平台/板级实现时不需要动窗口层。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

struct QLPlatformWindow {
    int backend;                 /* QLWindowBackend */
    int kind;                    /* 0 = GL, 1 = CPU */
    int cpu_kind;                /* 0 = 内存, 1 = Wayland, 2 = X11 */

    QLGfxGL     *gl;
    QLDispDriver *disp;
    QLCanvas     canvas;
    QLDraw2D     d2d;

    QLRect flush_rect;
    int    flush_full;
};

/* ------------------------------ GL ------------------------------ */

static QLPlatformWindow *gl_open(const char *title, ql_coord_t w, ql_coord_t h,
                                 int backend)
{
#if defined(QLGL_ENABLE_GL)
    const uint32_t flags = (backend == QL_WINDOW_OFFSCREEN) ? QL_GFX_GL_OFFSCREEN
                                                           : QL_GFX_GL_WINDOW;
    QLGfxGL *g = ql_gfx_gl_create(title != NULL ? title : "QLGL", w, h, flags);
    if (g == NULL)
        return NULL;

    QLPlatformWindow *pw = (QLPlatformWindow *)ql_mem_calloc(1, sizeof(*pw));
    if (pw == NULL) {
        ql_gfx_gl_destroy(g);
        return NULL;
    }
    pw->backend = backend;
    pw->kind = 0;
    pw->gl = g;
    /* 保留式画布：静止时零重绘，窗口双缓冲下局部重绘才成立 */
    (void)ql_gfx_gl_set_retained(g, 1);
    return pw;
#else
    (void)title; (void)w; (void)h; (void)backend;
    return NULL;
#endif
}

/* ------------------------------ CPU ------------------------------ */

static int cpu_rebind(QLPlatformWindow *pw)
{
    if (ql_canvas_from_disp(&pw->canvas, pw->disp) != QL_OK)
        return 0;
    return ql_draw2d_canvas_init(&pw->d2d, &pw->canvas) == QL_OK;
}

static QLPlatformWindow *cpu_open(const char *title, ql_coord_t w, ql_coord_t h,
                                  int backend)
{
    QLDispDriver *d = NULL;
    int cpu_kind = 0;

    if (backend == QL_WINDOW_OFFSCREEN) {
        d = ql_disp_mem_create(w, h, QL_COLOR_RGB888);
        cpu_kind = 0;
    } else {
#if defined(QLGL_ENABLE_X11)
        d = ql_disp_x11_create(title, w, h, QL_COLOR_FORMAT_AUTO);
        if (d != NULL)
            cpu_kind = 2;
#endif
#if defined(QLGL_ENABLE_WAYLAND)
        if (d == NULL) {
            d = ql_disp_wayland_create(title, w, h, QL_COLOR_FORMAT_AUTO);
            if (d != NULL)
                cpu_kind = 1;
        }
#endif
    }
    if (d == NULL)
        return NULL;

    QLPlatformWindow *pw = (QLPlatformWindow *)ql_mem_calloc(1, sizeof(*pw));
    if (pw == NULL) {
        if (cpu_kind == 0)
            ql_disp_mem_destroy(d);
#if defined(QLGL_ENABLE_X11)
        else if (cpu_kind == 2)
            ql_disp_x11_destroy(d);
#endif
#if defined(QLGL_ENABLE_WAYLAND)
        else if (cpu_kind == 1)
            ql_disp_wayland_destroy(d);
#endif
        return NULL;
    }
    pw->backend = backend;
    pw->kind = 1;
    pw->cpu_kind = cpu_kind;
    pw->disp = d;
    if (!cpu_rebind(pw)) {
        ql_mem_free(pw);
        return NULL;
    }
    return pw;
}

/* ------------------------------ 通用 ------------------------------ */

static QLPlatformWindow *desktop_open(const char *title, ql_coord_t w, ql_coord_t h,
                                      int backend)
{
    if (w <= 0 || h <= 0)
        return NULL;
    if (backend == QL_WINDOW_GL)
        return gl_open(title, w, h, backend);
    return cpu_open(title, w, h, backend);
}

static void desktop_close(QLPlatformWindow *pw)
{
    if (pw == NULL)
        return;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        ql_gfx_gl_destroy(pw->gl);
#endif
    } else if (pw->disp != NULL) {
        switch (pw->cpu_kind) {
        case 0: ql_disp_mem_destroy(pw->disp); break;
#if defined(QLGL_ENABLE_WAYLAND)
        case 1: ql_disp_wayland_destroy(pw->disp); break;
#endif
#if defined(QLGL_ENABLE_X11)
        case 2: ql_disp_x11_destroy(pw->disp); break;
#endif
        default: break;
        }
    }
    ql_mem_free(pw);
}

static int desktop_poll(QLPlatformWindow *pw)
{
    if (pw == NULL)
        return 0;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        return ql_gfx_gl_poll(pw->gl);
#else
        return 0;
#endif
    }
    switch (pw->cpu_kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case 1: return ql_disp_wayland_poll(pw->disp);
#endif
#if defined(QLGL_ENABLE_X11)
    case 2: return ql_disp_x11_poll(pw->disp);
#endif
    default: return 1;                 /* 内存驱动永不关窗 */
    }
}

static int desktop_wait(QLPlatformWindow *pw, int timeout_ms)
{
    if (pw == NULL)
        return 0;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        return ql_gfx_gl_wait(pw->gl, timeout_ms);
#else
        return 0;
#endif
    }
    /* CPU 后端没有阻塞式 wait：睡一小会儿，下一轮 poll 再看事件。
     * 静止时每 32ms 醒一次，CPU 占用可以忽略。 */
    if (timeout_ms > 0) {
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return 1;
}

static void desktop_size(QLPlatformWindow *pw, ql_coord_t *w, ql_coord_t *h)
{
    ql_coord_t ww = 0, hh = 0;
    if (pw != NULL) {
        if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
            ql_gfx_gl_size(pw->gl, &ww, &hh);
#endif
        } else if (pw->disp != NULL) {
            ww = ql_disp_css_width(pw->disp);
            hh = ql_disp_css_height(pw->disp);
        }
    }
    if (w != NULL) *w = ww;
    if (h != NULL) *h = hh;
}

static int desktop_resize_pending(QLPlatformWindow *pw)
{
    if (pw == NULL)
        return 0;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        return ql_gfx_gl_resize_pending(pw->gl);
#else
        return 0;
#endif
    }
    int pending = 0;
    switch (pw->cpu_kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case 1: pending = ql_disp_wayland_resize_pending(pw->disp); break;
#endif
#if defined(QLGL_ENABLE_X11)
    case 2: pending = ql_disp_x11_resize_pending(pw->disp); break;
#endif
    default: break;
    }
    /* X11/Wayland 已经重建了帧缓冲；画布是字段拷贝，必须重新取一份 */
    if (pending && !cpu_rebind(pw))
        return 0;
    return pending;
}

static int desktop_begin(QLPlatformWindow *pw, ql_color_t bg, const QLRect *dirty,
                         int n)
{
    if (pw == NULL)
        return 1;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        return ql_gfx_gl_begin_rects(pw->gl, bg, dirty, n);
#else
        return 1;
#endif
    }

    pw->flush_full = 0;
    pw->flush_rect = ql_rect_make(0, 0, 0, 0);
    if (n <= 0) {
        ql_canvas_clear(&pw->canvas, bg);
        pw->flush_full = 1;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (ql_rect_is_empty(dirty[i]))
            continue;
        ql_canvas_fill_rect(&pw->canvas, dirty[i], bg);
        pw->flush_rect = (pw->flush_rect.w <= 0)
                             ? dirty[i]
                             : ql_rect_union(pw->flush_rect, dirty[i]);
    }
    return 0;
}

static QLDraw2D *desktop_draw(QLPlatformWindow *pw)
{
    return (pw != NULL && pw->kind == 1) ? &pw->d2d : NULL;
}

static void desktop_end(QLPlatformWindow *pw)
{
    if (pw == NULL)
        return;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        ql_gfx_gl_end(pw->gl);
#endif
        return;
    }
    if (pw->disp == NULL)
        return;

    QLRect area = pw->flush_full
                      ? ql_rect_make(0, 0, pw->disp->width, pw->disp->height)
                      : pw->flush_rect;
    if (!ql_rect_is_empty(area) && pw->disp->flush != NULL)
        (void)pw->disp->flush(pw->disp, area);
    if (pw->disp->flush_done != NULL)
        (void)pw->disp->flush_done(pw->disp);
}

static void desktop_set_vsync(QLPlatformWindow *pw, int on)
{
    if (pw == NULL)
        return;
#if defined(QLGL_ENABLE_GL)
    if (pw->kind == 0)
        ql_gfx_gl_set_vsync(pw->gl, on);
#else
    (void)on;
#endif
}

static int desktop_dump(QLPlatformWindow *pw, QLUIRoot *ui, ql_color_t bg,
                        const char *path)
{
    if (pw == NULL || ui == NULL || path == NULL)
        return 0;

    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        QLDraw2D *d2d = ql_gfx_gl_draw(pw->gl);
        (void)ql_gfx_gl_begin_rects(pw->gl, bg, NULL, 0);
        ql_ui_render(ui, d2d);
        ql_gfx_gl_flush_frame(pw->gl);
        ql_gfx_gl_finish(pw->gl);

        ql_coord_t w = 0, h = 0;
        ql_gfx_gl_device_size(pw->gl, &w, &h);
        int ok = 0;
        if (w > 0 && h > 0 && w <= 65535 && h <= 65535) {
            const size_t stride = (size_t)w * 4u;
            uint8_t *px = (uint8_t *)ql_mem_alloc(stride * (size_t)h);
            if (px != NULL) {
                if (ql_gfx_gl_read_pixels(pw->gl, px, (uint32_t)stride) == QL_OK) {
                    FILE *fp = fopen(path, "wb");
                    if (fp != NULL) {
                        int good = (fprintf(fp, "P6\n%d %d\n255\n", (int)w,
                                            (int)h) > 0);
                        for (ql_coord_t y = 0; good && y < h; y++) {
                            const uint8_t *q = px + (size_t)y * stride;
                            for (ql_coord_t x = 0; x < w; x++, q += 4)
                                if (fwrite(q, 1, 3, fp) != 3) {
                                    good = 0;
                                    break;
                                }
                        }
                        if (fclose(fp) != 0)
                            good = 0;
                        ok = good;
                    }
                }
                ql_mem_free(px);
            }
        }
        ql_gfx_gl_end(pw->gl);
        return ok;
#else
        return 0;
#endif
    }

    /* CPU：整屏渲染一帧，再从驱动自己的帧缓冲写成 PPM */
    (void)desktop_begin(pw, bg, NULL, 0);
    QLDraw2D *d2d = desktop_draw(pw);
    if (d2d == NULL)
        return 0;
    ql_ui_render(ui, d2d);
    desktop_end(pw);
    return ql_disp_mem_save_ppm(pw->disp, path) == QL_OK;
}

static void desktop_attach_clipboard(QLPlatformWindow *pw, QLUIRoot *ui)
{
    if (pw == NULL || ui == NULL)
        return;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        ql_gfx_gl_attach_clipboard(pw->gl, ui);
#endif
        return;
    }
#if defined(QLGL_ENABLE_WAYLAND)
    if (pw->cpu_kind == 1)
        ql_disp_wayland_attach_clipboard(pw->disp, ui);
#else
    (void)ui;
#endif
}

static void desktop_attach_ime(QLPlatformWindow *pw, QLUIRoot *ui)
{
    if (pw == NULL || ui == NULL)
        return;
#if defined(QLGL_ENABLE_GL)
    if (pw->kind == 0)
        ql_gfx_gl_attach_ime(pw->gl, ui);
#else
    (void)ui;
#endif
}

static void desktop_set_ime_spot(QLPlatformWindow *pw, ql_coord_t x, ql_coord_t y,
                                 ql_coord_t h)
{
    if (pw == NULL)
        return;
    if (pw->kind == 0) {
#if defined(QLGL_ENABLE_GL)
        ql_gfx_gl_set_ime_spot(pw->gl, x, y, h);
#endif
        return;
    }
#if defined(QLGL_ENABLE_X11)
    if (pw->cpu_kind == 2)
        ql_disp_x11_set_ime_spot(pw->disp, x, y, h);
#else
    (void)x; (void)y; (void)h;
#endif
}

static void *desktop_native(QLPlatformWindow *pw, const char *what)
{
#if defined(QLGL_ENABLE_GL)
    if (pw != NULL && pw->kind == 0 && what != NULL && strcmp(what, "gl") == 0)
        return pw->gl;
#else
    (void)what;
#endif
    return NULL;
}

static uint32_t desktop_frame_us(QLPlatformWindow *pw)
{
#if defined(QLGL_ENABLE_GL)
    if (pw != NULL && pw->kind == 0)
        return ql_gfx_gl_frame_us(pw->gl);
#else
    (void)pw;
#endif
    return 0;
}

static uint32_t desktop_swap_us(QLPlatformWindow *pw)
{
#if defined(QLGL_ENABLE_GL)
    if (pw != NULL && pw->kind == 0)
        return ql_gfx_gl_swap_us(pw->gl);
#else
    (void)pw;
#endif
    return 0;
}

static const QLPlatform s_desktop = {
    .name = "desktop",
    .window_open = desktop_open,
    .window_close = desktop_close,
    .poll = desktop_poll,
    .wait = desktop_wait,
    .size = desktop_size,
    .resize_pending = desktop_resize_pending,
    .begin = desktop_begin,
    .draw = desktop_draw,
    .end = desktop_end,
    .set_vsync = desktop_set_vsync,
    .dump_ppm = desktop_dump,
    .attach_clipboard = desktop_attach_clipboard,
    .attach_ime = desktop_attach_ime,
    .set_ime_spot = desktop_set_ime_spot,
    .native = desktop_native,
    .frame_us = desktop_frame_us,
    .swap_us = desktop_swap_us,
};

const QLPlatform *ql_platform_desktop(void)
{
    return &s_desktop;
}
