/**
 * @file    test_x11_clip_multi.c
 * @brief   多窗口剪贴板：两个窗口各有一份 `QLClipX11`，互不覆盖。
 *
 * 不需要开真窗口，但需要 X 服务器（没有就 SKIP）：
 * 两个 `Display` 连接 = 两个窗口，各自 `attach` 一份状态。
 * A 成为 selection owner；B 发请求；A 的事件循环应答；B 收到内容。
 * 以前状态是文件级 static，第二个窗口 attach 会覆盖第一个 —— 这条会失败。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#if defined(QLGL_ENABLE_X11) && QLGL_ENABLE_X11

#include <X11/Xlib.h>

/* 内部头（不安装）：测的就是"每窗口一份状态"这个约定 */
#include "../../src/port/qlclipboard_x11.h"

/* 把某个 Display 上挂起的事件喂给它的剪贴板状态 */
static void pump(Display *d, QLClipX11 *clip)
{
    while (d != NULL && XPending(d) > 0) {
        XEvent ev;
        XNextEvent(d, &ev);
        (void)ql_x11_clip_handle_event(clip, &ev);
    }
}

/* 让"请求方"重试到拿到内容（真实主循环里是每帧一次） */
static int wait_text(Display *req_dpy, QLClipX11 *req, Display *own_dpy,
                     QLClipX11 *own, char *buf, size_t cap)
{
    for (int i = 0; i < 100; i++) {
        pump(own_dpy, own);
        pump(req_dpy, req);
        XFlush(req_dpy);
        XFlush(own_dpy);
        if (ql_x11_clip_get_text(req, buf, cap))
            return 1;
    }
    return 0;
}

#endif /* QLGL_ENABLE_X11 */

QL_TEST_BEGIN("test_x11_clip_multi")

#if defined(QLGL_ENABLE_X11) && QLGL_ENABLE_X11

    Display *d1 = XOpenDisplay(NULL);
    Display *d2 = XOpenDisplay(NULL);
    if (d1 == NULL || d2 == NULL) {
        if (d1 != NULL) XCloseDisplay(d1);
        if (d2 != NULL) XCloseDisplay(d2);
        printf("  [SKIP] 没有 X 服务器（DISPLAY 为空或连接失败）\n");
    } else {
        Window w1 = XCreateSimpleWindow(d1, DefaultRootWindow(d1), 0, 0, 10, 10,
                                        0, BlackPixel(d1, 0), BlackPixel(d1, 0));
        Window w2 = XCreateSimpleWindow(d2, DefaultRootWindow(d2), 0, 0, 10, 10,
                                        0, BlackPixel(d2, 0), BlackPixel(d2, 0));
        QL_CHECK(w1 != 0 && w2 != 0);

        QLClipX11 a = { 0 }, b = { 0 };
        ql_x11_clip_attach(&a, d1, w1);
        ql_x11_clip_attach(&b, d2, w2);
        XFlush(d1);
        XFlush(d2);

        char got[QL_CLIP_MAX];

        /* 1. A 复制 → B 粘贴（B 必须从 A 的连接上拿到，而不是被覆盖） */
        ql_x11_clip_set_text(&a, "hello 世界");
        XSync(d1, False);        /* 先让 server 认下 owner，另一个连接再请求 */
        QL_CHECK_EQ(wait_text(d2, &b, d1, &a, got, sizeof(got)), 1);
        QL_CHECK_STR(got, "hello 世界");

        /* 2. 反过来：B 复制 → A 粘贴 */
        ql_x11_clip_set_text(&b, "second window");
        XSync(d2, False);
        QL_CHECK_EQ(wait_text(d1, &a, d2, &b, got, sizeof(got)), 1);
        QL_CHECK_STR(got, "second window");

        /* 3. A 的状态没有被 B 的 attach/set 清掉：A 仍然能重新成为 owner */
        ql_x11_clip_set_text(&a, "still a");
        XSync(d1, False);
        QL_CHECK_EQ(wait_text(d2, &b, d1, &a, got, sizeof(got)), 1);
        QL_CHECK_STR(got, "still a");

        ql_x11_clip_detach(&a);
        ql_x11_clip_detach(&b);
        XDestroyWindow(d1, w1);
        XDestroyWindow(d2, w2);
        XFlush(d1);
        XFlush(d2);
        XCloseDisplay(d1);
        XCloseDisplay(d2);
    }

#else
    printf("  [SKIP] 本构建没有 X11\n");
#endif

QL_TEST_END()
