/**
 * @file    qlclipboard_x11.h
 * @brief   X11 剪贴板（ICCCM CLIPBOARD + UTF8_STRING）。内部头文件，不安装。
 *
 * 为什么不是简单的一个 `XGetSelection`：X11 的剪贴板是**点对点协议**，
 * 没有"系统剪贴板变量"这种东西。复制 = 成为 selection owner 并随时应答
 * SelectionRequest；粘贴 = 向 owner 发 SelectionRequest，等它回 SelectionNotify。
 * 所以这条路上有**异步**：粘贴请求发出去之后，内容在下一帧才可能到。
 *
 * 用法（每个窗口一份状态，绑定到自己的 Display/Window）：
 *
 *     QLClipX11 clip;                       // 随驱动/窗口一起分配（零初始化即可）
 *     ql_x11_clip_attach(&clip, dpy, win);
 *     ... 每个事件： if (ql_x11_clip_handle_event(&clip, &ev)) continue;
 *     ... 销毁：     ql_x11_clip_detach(&clip);
 *
 * **多窗口**：状态是每个窗口一份。X11 的 selection owner 是"每个 Display
 * 一个"（准确说是全局唯一 owner），但请求/应答都在各自的连接上走，
 * 所以两个窗口各自维护自己的 `QLClipX11` 是正确且必要的 —— 以前这里是
 * 文件级 static，第二个窗口 attach 会把第一个窗口的状态覆盖掉。
 *
 * 这个头被 `qlgfx_gl.c` / `qldisp_x11.c` 各自编译一份，类型定义相同、
 * 互不影响（都是单线程，各自窗口的事件循环里调用）。
 */
#ifndef QLGL_PORT_QLCLIPBOARD_X11_H
#define QLGL_PORT_QLCLIPBOARD_X11_H

#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <qlgl/qlgl.h>

#define QL_CLIP_MAX 1024

typedef struct {
    Display *dpy;
    Window   win;
    Atom     sel, utf8, targets, prop;

    char     out[QL_CLIP_MAX];   /* 我们持有 selection 时的内容 */
    int      out_len;
    char     in[QL_CLIP_MAX];    /* 从别的程序要回来的内容 */
    size_t   in_len;
    int      in_ready;           /* 1 = 有一次结果可读（可能为空） */
    int      waiting;            /* 已经发出请求，等 SelectionNotify */
} QLClipX11;

static void ql_x11_clip_attach(QLClipX11 *c, Display *dpy, Window win)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof(*c));
    c->dpy = dpy;
    c->win = win;
    if (dpy == NULL || win == 0)
        return;
    c->sel = XInternAtom(dpy, "CLIPBOARD", False);
    c->utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    c->targets = XInternAtom(dpy, "TARGETS", False);
    c->prop = XInternAtom(dpy, "QLGL_CLIP", False);
}

static void ql_x11_clip_detach(QLClipX11 *c)
{
    if (c == NULL)
        return;
    /*
     * 异步粘贴状态必须一起清掉：旧 Display 上的请求永远不会再收到
     * SelectionNotify，留着 c->waiting 会让重建之后的粘贴一直"等"。
     */
    c->waiting = 0;
    c->in_ready = 0;
    c->in_len = 0;
    c->in[0] = '\0';
    c->out_len = 0;
    c->out[0] = '\0';
    c->dpy = NULL;
    c->win = 0;
}

/* 成为 selection owner：之后别的程序粘贴时来问我们 */
static void ql_x11_clip_set_text(QLClipX11 *c, const char *utf8)
{
    if (c == NULL || c->dpy == NULL)
        return;
    size_t n = (utf8 != NULL) ? strlen(utf8) : 0u;
    if (n > (size_t)QL_CLIP_MAX - 1u) {
        n = (size_t)QL_CLIP_MAX - 1u;
        n = ql_utf8_valid_len(utf8, n);    /* 截断点退到码点边界 */
    }
    if (n > 0)
        memcpy(c->out, utf8, n);
    c->out[n] = '\0';
    c->out_len = (int)n;
    XSetSelectionOwner(c->dpy, c->sel, c->win, CurrentTime);
}

/*
 * 读回剪贴板内容。返回 1 = 内容已就绪（buf 里是 UTF-8，可能是空串）；
 * 0 = 还没到（已发出请求，下一帧再来问）。
 */
static int ql_x11_clip_get_text(QLClipX11 *c, char *buf, size_t cap)
{
    if (c == NULL || buf == NULL || cap == 0)
        return 0;
    if (c->in_ready) {
        size_t n = (c->in_len < cap - 1u) ? c->in_len : cap - 1u;
        if (n < c->in_len)
            n = ql_utf8_valid_len(c->in, n);
        if (n > 0)
            memcpy(buf, c->in, n);
        buf[n] = '\0';
        c->in_ready = 0;
        return 1;
    }
    if (c->dpy == NULL || c->waiting)
        return 0;
    XConvertSelection(c->dpy, c->sel, c->utf8, c->prop, c->win, CurrentTime);
    XFlush(c->dpy);
    c->waiting = 1;
    return 0;
}

/* 处理剪贴板相关事件；返回 1 = 这条事件已被消费 */
static int ql_x11_clip_handle_event(QLClipX11 *c, XEvent *ev)
{
    if (c == NULL || ev == NULL || c->dpy == NULL)
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

        if (rq->target == c->targets) {
            const Atom avail[1] = { c->utf8 };
            XChangeProperty(c->dpy, rq->requestor, rq->property, XA_ATOM, 32,
                            PropModeReplace, (const unsigned char *)avail, 1);
            note.property = rq->property;
        } else if (rq->target == c->utf8 || rq->target == XA_STRING) {
            XChangeProperty(c->dpy, rq->requestor, rq->property, rq->target, 8,
                            PropModeReplace, (const unsigned char *)c->out,
                            c->out_len);
            note.property = rq->property;
        }
        XSendEvent(c->dpy, rq->requestor, False, NoEventMask, (XEvent *)&note);
        XFlush(c->dpy);
        return 1;
    }

    case SelectionNotify: {
        /* 我们问别人要的内容到了（或对方拒绝了） */
        c->waiting = 0;
        c->in_len = 0;
        c->in[0] = '\0';
        if (ev->xselection.property != None) {
            Atom type = None;
            int fmt = 0;
            unsigned long n = 0, rest = 0;
            unsigned char *data = NULL;
            if (XGetWindowProperty(c->dpy, c->win, ev->xselection.property, 0,
                                   QL_CLIP_MAX / 4, True, AnyPropertyType, &type,
                                   &fmt, &n, &rest, &data) == Success &&
                data != NULL) {
                if (fmt == 8 && n > 0) {
                    size_t k = (n < (unsigned long)QL_CLIP_MAX - 1ul)
                                   ? (size_t)n
                                   : (size_t)QL_CLIP_MAX - 1u;
                    if (k < (size_t)n)
                        k = ql_utf8_valid_len((const char *)data, k);
                    memcpy(c->in, data, k);
                    c->in_len = k;
                }
                XFree(data);
            }
            c->in[c->in_len] = '\0';
            XDeleteProperty(c->dpy, c->win, ev->xselection.property);
        }
        c->in_ready = 1;      /* 即使空也算"有结果"，别让调用方一直等 */
        return 1;
    }

    case SelectionClear:
        /* 别的程序抢走了 selection：我们不再持有 */
        c->out_len = 0;
        return 1;

    default:
        return 0;
    }
}

#endif /* QLGL_PORT_QLCLIPBOARD_X11_H */
