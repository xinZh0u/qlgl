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
#include <X11/Xresource.h>

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
    char         title[128];
    int          screen;
    Visual      *visual;        /* 重建 XImage 要用 */
    int          depth;
    int          resize_pending; /* 窗口尺寸变过，调用方需要重建画布并重排 */
    /*
     * 输入法（XIM）。和 GL 后端同一套三件套：`XSetLocaleModifiers("")` +
     * `XOpenIM` + `FocusChangeMask`/`XSetICFocus`。漏一条就"输入法活着但
     * 一个中文都打不出来"，细节见《QLRender 阅读指南》踩坑表。
     */
    XIM          xim;
    XIC          xic;
} QLDispX11;

/*
 * 设置 UTF-8 窗口标题。
 *
 * ICCCM 的 WM_NAME 是 latin-1（STRING）语义，直接把 UTF-8 字节塞进去，
 * 现代 WM 会按 latin-1 解释 → 中文变成 "â€¦" 这样的乱码。
 * 正确的做法是同时写 EWMH 的 _NET_WM_NAME（类型 UTF8_STRING），
 * WM_NAME 仍然写一份（老 WM 的回退，纯 ASCII 时两者一致）。
 */
static void x11_set_title(Display *dpy, Window win, const char *title)
{
    const char *utf8 = (title != NULL) ? title : "QLGL";

    XStoreName(dpy, win, utf8);

    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    Atom utf8_str = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, win, net_name, utf8_str, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
    XChangeProperty(dpy, win, net_icon, utf8_str, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
}

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

/*
 * 窗口尺寸变了：**重新分配帧缓冲**。
 *
 * 这里没有别的选择 —— 渲染器直接写 XImage 的像素存储，"画布多大"就等于
 * "这块内存多大"。旧的 XImage/pixels 必须先拆掉再按新尺寸建，
 * 顺便把 base.width/height/stride 一起更新（调用方据此重建 QLCanvas）。
 *
 * 失败时保持旧尺寸不变（下一次 ConfigureNotify 还会再来）。
 */
static int x11_apply_resize(QLDispX11 *x, int w, int h)
{
    if (w <= 0 || h <= 0)
        return 0;
    if (x->image != NULL && x->image->width == w && x->image->height == h)
        return 0;

    XImage *ni = XCreateImage(x->dpy, x->visual, (unsigned)x->depth, ZPixmap, 0,
                              NULL, (unsigned)w, (unsigned)h, 32, 0);
    if (ni == NULL) {
        QL_LOGW("x11: resize %dx%d: XCreateImage failed", w, h);
        return 0;
    }
    const uint32_t stride = (uint32_t)ni->bytes_per_line;
    uint8_t *np = (uint8_t *)ql_mem_calloc(1, (size_t)stride * (size_t)h);
    if (np == NULL) {
        ni->data = NULL;
        XDestroyImage(ni);
        QL_LOGW("x11: resize %dx%d: out of memory", w, h);
        return 0;
    }
    ni->data = (char *)np;

    if (x->image != NULL) {
        x->image->data = NULL;      /* 像素内存归我们管，别让 XDestroyImage 去 free */
        XDestroyImage(x->image);
    }
    ql_mem_free(x->pixels);

    x->image = ni;
    x->pixels = np;
    x->base.framebuffer = np;
    x->base.stride = stride;
    x->base.width = (ql_coord_t)w;
    x->base.height = (ql_coord_t)h;
    return 1;
}

/*
 * 从 X 资源里读 Xft.dpi（桌面环境普遍会设），换算成 26.6 缩放系数。
 * 拿不到就返回 1.0；应用随时可以用 ql_disp_set_scale() 覆盖。
 */
static ql_coord_t x11_query_scale(Display *dpy)
{
    const char *res = XResourceManagerString(dpy);
    if (res == NULL)
        return QL_SCALE_ONE;

    XrmInitialize();
    XrmDatabase db = XrmGetStringDatabase(res);
    if (db == NULL)
        return QL_SCALE_ONE;

    char *type = NULL;
    XrmValue value;
    ql_coord_t scale = QL_SCALE_ONE;
    if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr != NULL) {
        double dpi = atof(value.addr);
        if (dpi > 0.0) {
            /* CSS px 的基准是 96 dpi */
            long fx = (long)(dpi * (double)QL_SCALE_ONE / 96.0 + 0.5);
            if (fx < QL_SCALE_ONE)
                fx = QL_SCALE_ONE;
            if (fx > 8 * QL_SCALE_ONE)
                fx = 8 * QL_SCALE_ONE;
            scale = (ql_coord_t)fx;
        }
    }
    XrmDestroyDatabase(db);
    return scale;
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

    x->visual = visual;
    x->depth = depth;
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
    x11_set_title(dpy, x->win, x->title);
    long evmask = ExposureMask | StructureNotifyMask | FocusChangeMask |
                  KeyPressMask | KeyReleaseMask |
                  ButtonPressMask | ButtonReleaseMask |
                  PointerMotionMask | EnterWindowMask | LeaveWindowMask;
    XSelectInput(dpy, x->win, evmask);

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
    ql_disp_set_scale(&x->base, x11_query_scale(dpy));

    XMapWindow(dpy, x->win);
    XFlush(dpy);

    /*
     * 接上 XIM。三件事缺一不可（顺序也重要）：
     *   setlocale(LC_ALL,"") → XSetLocaleModifiers("") → XOpenIM → XCreateIC
     * 再把这个 IC 声明的事件位并进窗口掩码，并在 FocusIn 时 XSetICFocus。
     */
    (void)XSetLocaleModifiers("");
    x->xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (x->xim != NULL) {
        x->xic = XCreateIC(x->xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, x->win, XNFocusWindow, x->win, NULL);
        if (x->xic != NULL) {
            long im_events = NoEventMask;
            if (XGetICValues(x->xic, XNFilterEvents, &im_events, NULL) == NULL &&
                im_events != NoEventMask)
                XSelectInput(dpy, x->win, evmask | im_events);
        } else {
            XCloseIM(x->xim);
            x->xim = NULL;
        }
    }

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
        if (x->xic != NULL)
        XDestroyIC(x->xic);
    if (x->xim != NULL)
        XCloseIM(x->xim);
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
            /* 窗口被遮挡/重新显示：整屏都要重画。
             * 计数本身就是信号 —— 应用（或测试）看到它变了就整帧重绘，
             * 这一层不持有任何"脏区"状态。 */
            x->expose_count++;
            break;

        case ConfigureNotify: {
            /*
             * 拖动缩放：帧缓冲要跟着变大变小。只置位 + 重建，重排是应用的事
             * （它知道界面该按哪个逻辑尺寸布局，而且缩放是 CSS px）。
             */
            if (x11_apply_resize(x, ev.xconfigure.width, ev.xconfigure.height))
                x->resize_pending = 1;
            break;
        }

        case FocusIn:
            if (x->xic != NULL)
                XSetICFocus(x->xic);
            break;

        case FocusOut:
            if (x->xic != NULL)
                XUnsetICFocus(x->xic);
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == x->wm_delete)
                x->closed = 1;
            break;

        case DestroyNotify:
            x->closed = 1;
            break;

        default:
            /* 输入法先过一遍：正在组字时按键不能给界面层 */
            if (x->xic != NULL && XFilterEvent(&ev, None))
                break;
            /* 其余输入事件交给输入系统；Esc/关闭改由应用决定 */
            ql_x11_push_input_scaled(&ev, &x->buttons, x->base.scale_fx, x->xic);
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
        if (x->xic != NULL) {
            XDestroyIC(x->xic);
            x->xic = NULL;
        }
        if (x->xim != NULL) {
            XCloseIM(x->xim);
            x->xim = NULL;
        }
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

int ql_disp_x11_resize_pending(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    QLDispX11 *x = (QLDispX11 *)driver;
    const int p = x->resize_pending;
    x->resize_pending = 0;
    return p;
}

uint32_t ql_disp_x11_expose_count(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    return ((QLDispX11 *)driver)->expose_count;
}

#endif /* QLGL_ENABLE_X11 */
