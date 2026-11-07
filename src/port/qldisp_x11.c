/**
 * @file    qldisp_x11.c
 * @brief   Linux X11 窗口显示后端。
 *
 * 关键点：XImage 的像素存储就是渲染器的帧缓冲，
 * 因此"绘制"与"上屏"之间没有额外拷贝，flush 只把脏矩形推给 X 服务器。
 */
#include <qlgl/qlgl.h>

#ifdef QLGL_ENABLE_X11

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "qlinput_x11.h"

typedef struct {
    QLDispDriver base;
    Display     *dpy;
    Window       win;
    GC           gc;
    XImage      *image;
    Atom         wm_delete;
    uint8_t     *pixels;        /* XImage 的存储，由我们自己分配 */
    uint32_t     flush_count;
    uint32_t     expose_count;
    uint8_t      buttons;       /* 当前按住的鼠标按钮位图 */
    int          closed;
    char         title[64];
    int          screen;
} QLDispX11;

/* 按 X 服务器的视觉选一个我们能写的格式 */
static int pick_format(Display *dpy, int screen, QLColorFormat hint, QLColorFormat *out)
{
    Visual *visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);
    (void)visual;

    XImage *probe = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, NULL, 1, 1, 32, 0);
    if (probe == NULL)
        return 0;

    int bpp = probe->bits_per_pixel;
    unsigned long rmask = probe->red_mask;
    unsigned long gmask = probe->green_mask;
    unsigned long bmask = probe->blue_mask;
    probe->data = NULL;
    XDestroyImage(probe);

    QLColorFormat detected;
    if (bpp == 32 && rmask == 0xFF0000ul && gmask == 0xFF00ul && bmask == 0xFFul) {
        detected = QL_COLOR_ARGB8888;
    } else if (bpp == 16 && rmask == 0xF800ul && gmask == 0x7E0ul && bmask == 0x1Ful) {
        detected = QL_COLOR_RGB565;
    } else if (bpp == 24) {
        detected = QL_COLOR_RGB888;
    } else {
        QL_LOGE("x11: unsupported visual (bpp=%d masks %#lx/%#lx/%#lx)",
                bpp, rmask, gmask, bmask);
        return 0;
    }

    if (hint != QL_COLOR_FORMAT_AUTO && hint != detected) {
        QL_LOGW("x11: requested %s but visual requires %s; using %s",
                ql_disp_format_name((uint8_t)hint), ql_disp_format_name((uint8_t)detected),
                ql_disp_format_name((uint8_t)detected));
    }

    *out = detected;
    return 1;
}

static int x11_flush(QLDispDriver *self, QLRect area)
{
    QLDispX11 *x = (QLDispX11 *)self;
    if (x->closed || x->dpy == NULL)
        return 0;

    if (ql_rect_is_empty(area))
        return 0;

    XPutImage(x->dpy, x->win, x->gc, x->image,
              area.x, area.y,            /* 源 */
              area.x, area.y,            /* 目标 */
              (unsigned)area.w, (unsigned)area.h);
    x->flush_count++;
    return 0;
}

static int x11_flush_done(QLDispDriver *self)
{
    QLDispX11 *x = (QLDispX11 *)self;
    if (x->dpy != NULL && !x->closed)
        XFlush(x->dpy);
    return 0;
}

QLDispDriver *ql_disp_x11_create(const char *title, ql_coord_t width, ql_coord_t height,
                                 QLColorFormat format)
{
    if (width <= 0 || height <= 0) {
        QL_LOGE("x11: invalid size %dx%d", (int)width, (int)height);
        return NULL;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        QL_LOGE("x11: cannot open display (DISPLAY=%s)",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return NULL;
    }

    int screen = DefaultScreen(dpy);

    QLColorFormat chosen;
    if (!pick_format(dpy, screen, format, &chosen)) {
        XCloseDisplay(dpy);
        return NULL;
    }

    QLDispX11 *x = (QLDispX11 *)ql_mem_calloc(1, sizeof(QLDispX11));
    if (x == NULL) {
        XCloseDisplay(dpy);
        return NULL;
    }

    x->dpy = dpy;
    x->screen = screen;

    Visual *visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    x->image = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, NULL,
                            (unsigned)width, (unsigned)height, 32, 0);
    if (x->image == NULL) {
        QL_LOGE("x11: XCreateImage failed");
        goto fail;
    }

    uint32_t stride = (uint32_t)x->image->bytes_per_line;
    size_t size = (size_t)stride * (size_t)height;
    x->pixels = (uint8_t *)ql_mem_calloc(1, size);
    if (x->pixels == NULL) {
        QL_LOGE("x11: cannot allocate %u bytes framebuffer", (unsigned)size);
        goto fail;
    }
    x->image->data = (char *)x->pixels;

    x->win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                                 (unsigned)width, (unsigned)height, 1,
                                 BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    if (x->win == 0) {
        QL_LOGE("x11: XCreateSimpleWindow failed");
        goto fail;
    }

    strncpy(x->title, (title != NULL) ? title : "QLGL", sizeof(x->title) - 1);
    XStoreName(dpy, x->win, x->title);
    XSelectInput(dpy, x->win,
                 ExposureMask | StructureNotifyMask |
                 KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | EnterWindowMask | LeaveWindowMask);

    x->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, x->win, &x->wm_delete, 1);

    x->gc = XCreateGC(dpy, x->win, 0, NULL);
    if (x->gc == NULL) {
        QL_LOGE("x11: XCreateGC failed");
        goto fail;
    }

    if (ql_disp_init_fb(&x->base, x->pixels, stride, width, height, chosen,
                        x11_flush) != QL_OK) {
        QL_LOGE("x11: ql_disp_init_fb failed");
        goto fail;
    }
    x->base.user_data = x;
    x->base.flush_done = x11_flush_done;

    XMapWindow(dpy, x->win);
    XFlush(dpy);

    if (ql_disp_set(&x->base) != QL_OK) {
        QL_LOGE("x11: ql_disp_set failed");
        goto fail;
    }

    QL_LOGI("x11: window \"%s\" %dx%d %s", (title != NULL) ? title : "QLGL",
            (int)width, (int)height, ql_disp_format_name((uint8_t)chosen));
    return &x->base;

fail:
    /* 走统一销毁路径；此时 base 可能还没初始化完，手动清理 */
    if (x->image != NULL) {
        x->image->data = NULL;          /* 像素内存归我们管 */
        XDestroyImage(x->image);
    }
    if (x->gc != NULL)
        XFreeGC(dpy, x->gc);
    if (x->win != 0)
        XDestroyWindow(dpy, x->win);
    XCloseDisplay(dpy);
    ql_mem_free(x->pixels);
    ql_mem_free(x);
    return NULL;
}

int ql_disp_x11_poll(QLDispDriver *driver)
{
    if (driver == NULL) 
        return 0;

    QLDispX11 *x = (QLDispX11 *)driver;
    if (x->dpy == NULL || x->closed)
        return 0;

    while (XPending(x->dpy) > 0) {
        XEvent ev;
        XNextEvent(x->dpy, &ev);

        switch (ev.type) {
        case Expose:
            x->expose_count++;
            /* 窗口被遮挡/重新显示：整屏都要重画 */
            ql_render_invalidate_all();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == x->wm_delete)
                x->closed = 1;
            break;

        case DestroyNotify:
            x->closed = 1;
            break;

        default:
            /* 其余输入事件交给输入系统；Esc/关闭改由应用决定 */
            ql_x11_push_input(&ev, &x->buttons);
            break;
        }
    }

    return x->closed ? 0 : 1;
}

void ql_disp_x11_close(QLDispDriver *driver)
{
    if (driver != NULL)
        ((QLDispX11 *)driver)->closed = 1;
}

void ql_disp_x11_destroy(QLDispDriver *driver)
{
    if (driver == NULL)
        return;

    QLDispX11 *x = (QLDispX11 *)driver;

    ql_disp_unset(driver);

    if (x->dpy != NULL) {
        if (x->gc != NULL)
            XFreeGC(x->dpy, x->gc);
        if (x->win != 0)
            XDestroyWindow(x->dpy, x->win);
    }
    if (x->image != NULL) {
        x->image->data = NULL;      /* 防止 XDestroyImage 释放我们的内存 */
        XDestroyImage(x->image);
    }
    if (x->dpy != NULL)
        XCloseDisplay(x->dpy);

    ql_mem_free(x->pixels);
    ql_mem_free(x);
}

const char *ql_disp_x11_title(QLDispDriver *driver)
{
    if (driver == NULL)
        return "";
    return ((QLDispX11 *)driver)->title;
}

uint32_t ql_disp_x11_expose_count(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    return ((QLDispX11 *)driver)->expose_count;
}

#endif /* QLGL_ENABLE_X11 */
