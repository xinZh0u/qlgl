/**
 * @file    qlclipboard_x11.h
 * @brief   X11 剪贴板（ICCCM CLIPBOARD + UTF8_STRING）。内部头文件，不安装。
 *
 * 为什么不是简单的一个 `XGetSelection`：X11 的剪贴板是**点对点协议**，
 * 没有"系统剪贴板变量"这种东西。复制 = 成为 selection owner 并随时应答
 * SelectionRequest；粘贴 = 向 owner 发 SelectionRequest，等它回 SelectionNotify。
 * 所以这条路上有**异步**：粘贴请求发出去之后，内容在下一帧才可能到。
 *
 * 用法（X11 后端在 create 里接上，在事件循环里转发）：
 *     ql_x11_clip_attach(dpy, win);
 *     ... 每个事件： if (ql_x11_clip_handle_event(&ev)) continue;
 *
 * 注意：这里是 static 状态，被 qlgfx_gl.c / qldisp_x11.c 各自编译一份。
 * 同一进程里只有一个后端在跑，所以没问题；两个后端同时用剪贴板不支持。
 */
#ifndef QLGL_PORT_QLCLIPBOARD_X11_H
#define QLGL_PORT_QLCLIPBOARD_X11_H

#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define QL_CLIP_MAX 1024

static Display *s_clip_dpy;
static Window   s_clip_win;
static Atom     s_clip_sel, s_clip_utf8, s_clip_targets, s_clip_prop;
static char     s_clip_out[QL_CLIP_MAX];   /* 我们持有 selection 时的内容 */
static int      s_clip_out_len;
static char     s_clip_in[QL_CLIP_MAX];    /* 从别的程序要回来的内容 */
static size_t   s_clip_in_len;
static int      s_clip_in_ready;           /* 1 = 有一次结果可读（可能为空） */
static int      s_clip_waiting;            /* 已经发出请求，等 SelectionNotify */

static void ql_x11_clip_attach(Display *dpy, Window win)
{
    s_clip_dpy = dpy;
    s_clip_win = win;
    if (dpy == NULL || win == 0)
        return;
    s_clip_sel = XInternAtom(dpy, "CLIPBOARD", False);
    s_clip_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    s_clip_targets = XInternAtom(dpy, "TARGETS", False);
    s_clip_prop = XInternAtom(dpy, "QLGL_CLIP", False);
}

static void ql_x11_clip_detach(void)
{
    s_clip_dpy = NULL;
    s_clip_win = 0;
}

/* 成为 selection owner：之后别的程序粘贴时来问我们 */
static void ql_x11_clip_set_text(const char *utf8)
{
    if (s_clip_dpy == NULL)
        return;
    size_t n = (utf8 != NULL) ? strlen(utf8) : 0u;
    if (n > (size_t)QL_CLIP_MAX - 1u)
        n = (size_t)QL_CLIP_MAX - 1u;
    if (n > 0)
        memcpy(s_clip_out, utf8, n);
    s_clip_out[n] = '\0';
    s_clip_out_len = (int)n;
    XSetSelectionOwner(s_clip_dpy, s_clip_sel, s_clip_win, CurrentTime);
}

/*
 * 读回剪贴板内容。返回 1 = 内容已就绪（buf 里是 UTF-8，可能是空串）；
 * 0 = 还没到（已发出请求，下一帧再来问）。
 */
static int ql_x11_clip_get_text(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0)
        return 0;
    if (s_clip_in_ready) {
        const size_t n = (s_clip_in_len < cap - 1u) ? s_clip_in_len : cap - 1u;
        if (n > 0)
            memcpy(buf, s_clip_in, n);
        buf[n] = '\0';
        s_clip_in_ready = 0;
        return 1;
    }
    if (s_clip_dpy == NULL || s_clip_waiting)
        return 0;
    XConvertSelection(s_clip_dpy, s_clip_sel, s_clip_utf8, s_clip_prop,
                      s_clip_win, CurrentTime);
    XFlush(s_clip_dpy);
    s_clip_waiting = 1;
    return 0;
}

/* 处理剪贴板相关事件；返回 1 = 这条事件已被消费 */
static int ql_x11_clip_handle_event(XEvent *ev)
{
    if (ev == NULL || s_clip_dpy == NULL)
        return 0;

    switch (ev->type) {
    case SelectionRequest: {
        /* 别的程序来要内容：按它要的 target 回，拿不到就回 property=None */
        const XSelectionRequestEvent *rq = &ev->xselectionrequest;
        XSelectionEvent note;
        memset(&note, 0, sizeof(note));
        note.type = SelectionNotify;
        note.display = rq->display;
        note.requestor = rq->requestor;
        note.selection = rq->selection;
        note.target = rq->target;
        note.time = rq->time;
        note.property = None;

        if (rq->target == s_clip_targets) {
            const Atom avail[1] = { s_clip_utf8 };
            XChangeProperty(s_clip_dpy, rq->requestor, rq->property, XA_ATOM, 32,
                            PropModeReplace, (const unsigned char *)avail, 1);
            note.property = rq->property;
        } else if (rq->target == s_clip_utf8 || rq->target == XA_STRING) {
            XChangeProperty(s_clip_dpy, rq->requestor, rq->property,
                            rq->target, 8, PropModeReplace,
                            (const unsigned char *)s_clip_out, s_clip_out_len);
            note.property = rq->property;
        }
        XSendEvent(s_clip_dpy, rq->requestor, False, NoEventMask, (XEvent *)&note);
        XFlush(s_clip_dpy);
        return 1;
    }

    case SelectionNotify: {
        /* 我们问别人要的内容到了（或对方拒绝了） */
        s_clip_waiting = 0;
        s_clip_in_len = 0;
        s_clip_in[0] = '\0';
        if (ev->xselection.property != None) {
            Atom type = None;
            int fmt = 0;
            unsigned long n = 0, rest = 0;
            unsigned char *data = NULL;
            if (XGetWindowProperty(s_clip_dpy, s_clip_win, ev->xselection.property,
                                   0, QL_CLIP_MAX / 4, True, AnyPropertyType,
                                   &type, &fmt, &n, &rest, &data) == Success &&
                data != NULL) {
                if (fmt == 8 && n > 0) {
                    const size_t k = (n < (unsigned long)QL_CLIP_MAX - 1ul)
                                         ? (size_t)n
                                         : (size_t)QL_CLIP_MAX - 1u;
                    memcpy(s_clip_in, data, k);
                    s_clip_in_len = k;
                }
                XFree(data);
            }
            s_clip_in[s_clip_in_len] = '\0';
            XDeleteProperty(s_clip_dpy, s_clip_win, ev->xselection.property);
        }
        s_clip_in_ready = 1;      /* 即使空也算"有结果"，别让调用方一直等 */
        return 1;
    }

    case SelectionClear:
        /* 别的程序抢走了 selection：我们不再持有 */
        s_clip_out_len = 0;
        return 1;

    default:
        return 0;
    }
}

#endif /* QLGL_PORT_QLCLIPBOARD_X11_H */
