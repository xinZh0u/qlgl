/**
 * @file    qlwindow_wayland.h
 * @brief   Wayland 窗口外壳：xdg-shell + 输入 + 高 DPI + 帧回调 + 剪贴板。
 *
 * 只有"窗口"这一层，**不含渲染**：
 *   - `qldisp_wayland.c`（CPU）自己管 shm 缓冲区与 damage；
 *   - `qlgfx_gl.c`（GPU）把它交给 EGL / `wl_egl_window`。
 *
 * 两边共用同一份输入/缩放/剪贴板语义（"语义只有一份"是本库的红线）：
 * 各写一遍必然在"pointer 坐标要不要换算""serial 用哪个"这种地方分叉。
 *
 * 用法：
 * ```c
 * QLWlWindow w;
 * if (!qlwl_open(&w, "QLGL", 800, 600)) return NULL;
 * while (qlwl_pump(&w)) {
 *     ...渲染 + 提交...
 *     qlwl_frame_done(&w);      // 请求下一次帧回调（节流用）
 * }
 * qlwl_close(&w);
 * ```
 */
#ifndef QLGL_PORT_QLWINDOW_WAYLAND_H
#define QLGL_PORT_QLWINDOW_WAYLAND_H

#if defined(QLGL_ENABLE_WAYLAND)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "qlinput_evdev.h"

/* 剪贴板文本上限（与 X11 后端一致） */
#define QLWL_CLIP_MAX   1024
/* 最多跟踪几块输出（笔记本 + 外接屏就够） */
#define QLWL_MAX_OUTPUTS 4
/* 连续被节流几次就强提交一次（防某些合成器不回帧回调时界面僵住） */
#define QLWL_MAX_STALL  3

/*
 * 这个头里的函数都是 `static`（两个后端各编译一份），但**不是每个 TU 都用得上**
 * 全部：GPU 后端不走 `wl_surface.commit`（EGL swap 自己提交），用不到
 * `qlwl_should_commit`；严格构建会把它当 unused-function 报错。统一挂上
 * unused 属性，别为了消警告去调用用不着的函数。
 */
#if defined(__GNUC__) || defined(__clang__)
#define QLWL_MAYBE_UNUSED __attribute__((unused))
#else
#define QLWL_MAYBE_UNUSED
#endif

typedef struct {
    struct wl_output *obj;
    int32_t           scale;      /* wl_output.scale，至少 1 */
} QLWlOutput;

typedef struct {
    struct wl_display    *dpy;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    /* CPU 后端（wl_shm）用；GPU 后端用不到，但绑一下无副作用（同一份外壳） */
    struct wl_shm        *shm;
    struct xdg_wm_base   *wm_base;
    struct wl_seat       *seat;
    struct wl_keyboard   *keyboard;
    struct wl_pointer    *pointer;
    struct wl_touch      *touch;

    struct wl_surface    *surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *toplevel;

    /* 输入状态：后端自己维护，控件层不关心 */
    uint8_t    buttons;
    uint8_t    mods;
    wl_fixed_t pointer_x;
    wl_fixed_t pointer_y;
    uint32_t   last_serial;       /* set_selection 要用"最近一次输入事件"的 */

    /* 高 DPI */
    QLWlOutput outputs[QLWL_MAX_OUTPUTS];
    int        output_count;
    int32_t    scale;             /* 1 = 不缩放；可由 QLGL_WAYLAND_SCALE 覆盖 */
    int32_t    scale_lock;        /* >0：强制这个倍数，忽略合成器上报值 */

    /* 帧回调（提交节流） */
    struct wl_callback *frame_cb;
    int                 frame_pending;
    int                 frame_stall;

    /* 剪贴板 */
    struct wl_data_device_manager *data_mgr;
    struct wl_data_device         *data_dev;
    struct wl_data_source         *clip_source;
    struct wl_data_offer          *clip_offer;
    char                           clip_mime[40];
    char                           clip_out[QLWL_CLIP_MAX];
    int                            clip_out_len;
    char                           clip_in[QLWL_CLIP_MAX];
    int                            clip_in_len;
    int                            clip_in_ready;
    int                            clip_pipe;

    int  configured;
    int  closed;
    int  resize_pending;          /* 尺寸/缩放变了，调用方要重建画布并重排 */
    char title[64];
} QLWlWindow;

/* ------------------------------ 监听器 ------------------------------ */

static void qlwl_wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener qlwl_wm_listener = { .ping = qlwl_wm_ping };

static void qlwl_xdg_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    QLWlWindow *w = (QLWlWindow *)data;
    xdg_surface_ack_configure(s, serial);
    w->configured = 1;
}

static const struct xdg_surface_listener qlwl_xdg_listener = { .configure = qlwl_xdg_configure };

static void qlwl_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width,
                                    int32_t height, struct wl_array *states)
{
    (void)data; (void)t; (void)width; (void)height; (void)states;
    /* 尺寸由应用决定（CSS 尺寸固定）；缩放走 wl_output.scale */
}

static void qlwl_toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    ((QLWlWindow *)data)->closed = 1;
}

static const struct xdg_toplevel_listener qlwl_toplevel_listener = {
    .configure = qlwl_toplevel_configure,
    .close = qlwl_toplevel_close,
};

/* ------------------------------ 键盘 ------------------------------ */

static void qlwl_kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                           int32_t fd, uint32_t size)
{
    (void)data; (void)kb; (void)format; (void)size;
    close(fd);                        /* 不用键盘映射表（按键码走 evdev 表） */
}

static void qlwl_kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                          struct wl_surface *s, struct wl_array *keys)
{
    (void)kb; (void)s; (void)keys;
    ((QLWlWindow *)data)->last_serial = serial;
}

static void qlwl_kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                          struct wl_surface *s)
{
    (void)data; (void)kb; (void)serial; (void)s;
}

static void qlwl_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                        uint32_t time, uint32_t key, uint32_t state);

static void qlwl_kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                              uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)kb; (void)serial; (void)lat; (void)group;
    QLWlWindow *w = (QLWlWindow *)data;

    /*
     * 没有 xkbcommon，就用 xkb 的修饰键位掩码近似：
     * 1=Shift 4=Ctrl 8=Alt 64=Super，lock 里的 2=CapsLock。
     * （与 CPU 后端 `qldisp_wayland.c` 完全一致 —— 两边分叉过一次，别再犯。）
     */
    uint8_t mods = 0;
    if ((dep & 1u) != 0)  mods |= QL_MOD_SHIFT;
    if ((dep & 4u) != 0)  mods |= QL_MOD_CTRL;
    if ((dep & 8u) != 0)  mods |= QL_MOD_ALT;
    if ((dep & 64u) != 0) mods |= QL_MOD_META;
    if ((lock & 2u) != 0) mods |= QL_MOD_CAPS;
    w->mods = mods;
}

static void qlwl_kb_repeat(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener qlwl_kb_listener = {
    .keymap = qlwl_kb_keymap,
    .enter = qlwl_kb_enter,
    .leave = qlwl_kb_leave,
    .key = qlwl_kb_key,
    .modifiers = qlwl_kb_modifiers,
    .repeat_info = qlwl_kb_repeat,
};

/* ------------------------------ 指针 ------------------------------ */

static void qlwl_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                           struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)p; (void)serial; (void)s;
    QLWlWindow *w = (QLWlWindow *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
}

static void qlwl_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                           struct wl_surface *s)
{
    (void)data; (void)p; (void)serial; (void)s;
    ql_input_push_leave();
}

static void qlwl_ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                            wl_fixed_t x, wl_fixed_t y)
{
    (void)p; (void)time;
    QLWlWindow *w = (QLWlWindow *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
}

static void qlwl_ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state)
{
    (void)p; (void)time;
    QLWlWindow *w = (QLWlWindow *)data;
    w->last_serial = serial;

    uint8_t btn = ql_evdev_button(button);
    if (btn == 0xFF)
        return;
    const int down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (down)
        w->buttons |= (uint8_t)(1u << btn);
    else
        w->buttons &= (uint8_t)~(1u << btn);
    ql_input_push_button(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                         btn, down);
}

static void qlwl_ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis, wl_fixed_t value)
{
    (void)p; (void)time;
    QLWlWindow *w = (QLWlWindow *)data;
    /* wl_fixed 是定点滚动量，一格约 15；放大成"行"便于上层按行滚动 */
    const int32_t delta = (int32_t)(wl_fixed_to_double(value) * 20.0);
    const int32_t dx = (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) ? delta : 0;
    const int32_t dy = (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? delta : 0;
    ql_input_push_wheel(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                        dx, dy);
}

static const struct wl_pointer_listener qlwl_ptr_listener = {
    .enter = qlwl_ptr_enter,
    .leave = qlwl_ptr_leave,
    .motion = qlwl_ptr_motion,
    .button = qlwl_ptr_button,
    .axis = qlwl_ptr_axis,
};

/* ------------------------------ 触摸 ------------------------------ */

/*
 * 触摸映射成"主键按下 / 移动 / 抬起"：界面层只认指针事件，触摸与鼠标
 * 走同一条路。坐标是 surface 的逻辑坐标（= CSS px），不用换算。
 */
static void qlwl_touch_down(void *data, struct wl_touch *t, uint32_t serial,
                            uint32_t time, struct wl_surface *s, int32_t id,
                            wl_fixed_t x, wl_fixed_t y)
{
    (void)t; (void)serial; (void)time; (void)s; (void)id;
    QLWlWindow *w = (QLWlWindow *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    w->buttons |= 1u;
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
    ql_input_push_button(wl_fixed_to_int(x), wl_fixed_to_int(y), 0, 1);
}

static void qlwl_touch_up(void *data, struct wl_touch *t, uint32_t serial,
                          uint32_t time, int32_t id)
{
    (void)t; (void)serial; (void)time; (void)id;
    QLWlWindow *w = (QLWlWindow *)data;
    ql_input_push_button(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                         0, 0);
    w->buttons &= (uint8_t)~1u;
}

static void qlwl_touch_motion(void *data, struct wl_touch *t, uint32_t time,
                              int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)t; (void)time; (void)id;
    QLWlWindow *w = (QLWlWindow *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
}

static void qlwl_touch_frame(void *data, struct wl_touch *t)
{
    (void)data; (void)t;
}

static void qlwl_touch_cancel(void *data, struct wl_touch *t)
{
    (void)t;
    QLWlWindow *w = (QLWlWindow *)data;
    if ((w->buttons & 1u) != 0) {
        ql_input_push_button(wl_fixed_to_int(w->pointer_x),
                             wl_fixed_to_int(w->pointer_y), 0, 0);
        w->buttons &= (uint8_t)~1u;
    }
}

static void qlwl_touch_shape(void *data, struct wl_touch *t, int32_t id,
                             wl_fixed_t major, wl_fixed_t minor)
{
    (void)data; (void)t; (void)id; (void)major; (void)minor;
}

static void qlwl_touch_orientation(void *data, struct wl_touch *t, int32_t id,
                                   wl_fixed_t orientation)
{
    (void)data; (void)t; (void)id; (void)orientation;
}

static const struct wl_touch_listener qlwl_touch_listener = {
    .down = qlwl_touch_down,
    .up = qlwl_touch_up,
    .motion = qlwl_touch_motion,
    .frame = qlwl_touch_frame,
    .cancel = qlwl_touch_cancel,
    .shape = qlwl_touch_shape,
    .orientation = qlwl_touch_orientation,
};

/* ------------------------------ 输出 / 高 DPI ------------------------------ */

static void qlwl_out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                              int32_t pw, int32_t ph, int32_t subpixel,
                              const char *make, const char *model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void qlwl_out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
                          int32_t h, int32_t refresh)
{
    (void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}

static void qlwl_out_done(void *data, struct wl_output *o)
{
    (void)data; (void)o;
}

static void qlwl_out_scale(void *data, struct wl_output *o, int32_t factor)
{
    QLWlWindow *w = (QLWlWindow *)data;
    for (int i = 0; i < QLWL_MAX_OUTPUTS; i++)
        if (w->outputs[i].obj == o) {
            w->outputs[i].scale = (factor > 0) ? factor : 1;
            break;
        }
}

static void qlwl_out_name(void *data, struct wl_output *o, const char *name)
{
    (void)data; (void)o; (void)name;
}

static void qlwl_out_desc(void *data, struct wl_output *o, const char *desc)
{
    (void)data; (void)o; (void)desc;
}

static const struct wl_output_listener qlwl_out_listener = {
    .geometry = qlwl_out_geometry,
    .mode = qlwl_out_mode,
    .done = qlwl_out_done,
    .scale = qlwl_out_scale,
    .name = qlwl_out_name,
    .description = qlwl_out_desc,
};

static void qlwl_surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
    (void)s;
    QLWlWindow *w = (QLWlWindow *)data;
    int32_t sc = 1;
    for (int i = 0; i < QLWL_MAX_OUTPUTS; i++)
        if (w->outputs[i].obj == o) {
            sc = w->outputs[i].scale;
            break;
        }
    if (w->scale_lock > 0)
        sc = w->scale_lock;       /* 调试强制倍数：合成器说什么都不听 */
    if (sc != w->scale) {
        w->scale = sc;
        w->resize_pending = 1;
    }
}

static void qlwl_surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
    (void)data; (void)s; (void)o;
}

static const struct wl_surface_listener qlwl_surf_listener = {
    .enter = qlwl_surf_enter,
    .leave = qlwl_surf_leave,
};

/* ------------------------------ 帧回调 ------------------------------ */

static void qlwl_frame_done_cb(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    QLWlWindow *w = (QLWlWindow *)data;
    wl_callback_destroy(cb);
    if (w->frame_cb == cb)
        w->frame_cb = NULL;
    w->frame_pending = 0;
    w->frame_stall = 0;
}

static const struct wl_callback_listener qlwl_frame_listener = { .done = qlwl_frame_done_cb };

/* 请求下一次帧回调（幂等：已经挂上就不再挂） */
static QLWL_MAYBE_UNUSED void qlwl_request_frame(QLWlWindow *w)
{
    if (w == NULL || w->surface == NULL || w->frame_pending)
        return;
    w->frame_cb = wl_surface_frame(w->surface);
    if (w->frame_cb != NULL) {
        wl_callback_add_listener(w->frame_cb, &qlwl_frame_listener, w);
        w->frame_pending = 1;
    }
}

/*
 * 现在可以提交吗？（帧率节流）
 *
 * 上一帧还没被合成器呈现时返回 0，调用方**先别提交**、脏区继续攒着；
 * 连续被挡 `QLWL_MAX_STALL` 次就放行一次，免得某个合成器不回帧回调时僵住。
 */
static QLWL_MAYBE_UNUSED int qlwl_should_commit(QLWlWindow *w)
{
    if (w == NULL)
        return 0;
    if (!w->frame_pending)
        return 1;
    if (w->frame_stall >= QLWL_MAX_STALL)
        return 1;
    w->frame_stall++;
    return 0;
}

/* ------------------------------ 剪贴板 ------------------------------ */

static const char *const qlwl_clip_mimes[] = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING",
};

static void qlwl_clip_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
    QLWlWindow *w = (QLWlWindow *)data;
    (void)offer;
    if (w->clip_mime[0] != '\0')
        return;
    for (size_t i = 0; i < sizeof(qlwl_clip_mimes) / sizeof(qlwl_clip_mimes[0]); i++)
        if (strcmp(mime, qlwl_clip_mimes[i]) == 0) {
            snprintf(w->clip_mime, sizeof(w->clip_mime), "%s", mime);
            return;
        }
}

static const struct wl_data_offer_listener qlwl_clip_offer_listener = {
    .offer = qlwl_clip_offer,
};

/* 新 offer 先到：只挂监听器（同一个 offer 不能挂两次，否则 libwayland 报错） */
static void qlwl_clip_data_offer(void *data, struct wl_data_device *dev,
                                 struct wl_data_offer *offer)
{
    (void)dev;
    if (offer != NULL)
        wl_data_offer_add_listener(offer, &qlwl_clip_offer_listener, data);
}

static void qlwl_clip_selection(void *data, struct wl_data_device *dev,
                                struct wl_data_offer *offer)
{
    (void)dev;
    QLWlWindow *w = (QLWlWindow *)data;
    if (w->clip_offer != NULL && w->clip_offer != offer)
        wl_data_offer_destroy(w->clip_offer);
    w->clip_offer = offer;
    w->clip_mime[0] = '\0';
    w->clip_in_len = 0;
    w->clip_in_ready = 0;
    if (w->clip_pipe >= 0) {
        close(w->clip_pipe);
        w->clip_pipe = -1;
    }
}

static void qlwl_clip_enter(void *data, struct wl_data_device *dev, uint32_t serial,
                            struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                            struct wl_data_offer *offer)
{
    (void)data; (void)dev; (void)serial; (void)s; (void)x; (void)y; (void)offer;
}

static void qlwl_clip_leave(void *data, struct wl_data_device *dev)
{
    (void)data; (void)dev;
}

static void qlwl_clip_motion(void *data, struct wl_data_device *dev, uint32_t time,
                             wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)dev; (void)time; (void)x; (void)y;
}

static void qlwl_clip_drop(void *data, struct wl_data_device *dev)
{
    (void)data; (void)dev;
}

static const struct wl_data_device_listener qlwl_clip_dev_listener = {
    .data_offer = qlwl_clip_data_offer,
    .enter = qlwl_clip_enter,
    .leave = qlwl_clip_leave,
    .motion = qlwl_clip_motion,
    .drop = qlwl_clip_drop,
    .selection = qlwl_clip_selection,
};

static void qlwl_clip_send(void *data, struct wl_data_source *src, const char *mime,
                           int32_t fd)
{
    (void)src; (void)mime;
    QLWlWindow *w = (QLWlWindow *)data;
    const char *p = w->clip_out;
    size_t left = (size_t)w->clip_out_len;
    while (left > 0) {
        const ssize_t n = write(fd, p, left);
        if (n <= 0)
            break;
        p += n;
        left -= (size_t)n;
    }
    close(fd);                        /* 关写端 = 告诉对方"完了" */
}

static void qlwl_clip_cancelled(void *data, struct wl_data_source *src)
{
    (void)data;
    wl_data_source_destroy(src);
}

static void qlwl_clip_target(void *data, struct wl_data_source *src, const char *mime)
{
    (void)data; (void)src; (void)mime;
}

static const struct wl_data_source_listener qlwl_clip_src_listener = {
    .target = qlwl_clip_target,
    .send = qlwl_clip_send,
    .cancelled = qlwl_clip_cancelled,
};

static void qlwl_clip_set(const char *utf8, void *user)
{
    QLWlWindow *w = (QLWlWindow *)user;
    if (w == NULL || w->data_dev == NULL || w->data_mgr == NULL)
        return;

    size_t n = (utf8 != NULL) ? strlen(utf8) : 0u;
    if (n > (size_t)QLWL_CLIP_MAX - 1u)
        n = (size_t)QLWL_CLIP_MAX - 1u;
    if (n > 0)
        memcpy(w->clip_out, utf8, n);
    w->clip_out[n] = '\0';
    w->clip_out_len = (int)n;

    if (w->clip_source != NULL) {
        wl_data_source_destroy(w->clip_source);
        w->clip_source = NULL;
    }
    struct wl_data_source *src = wl_data_device_manager_create_data_source(w->data_mgr);
    if (src == NULL)
        return;
    for (size_t i = 0; i < sizeof(qlwl_clip_mimes) / sizeof(qlwl_clip_mimes[0]); i++)
        wl_data_source_offer(src, qlwl_clip_mimes[i]);
    wl_data_source_add_listener(src, &qlwl_clip_src_listener, w);
    w->clip_source = src;
    /* serial 必须是"当前这次输入事件"的，否则合成器会拒绝 */
    wl_data_device_set_selection(w->data_dev, src, w->last_serial);
}

/* 1 = 内容已就绪（可能是空串），0 = 还没到（下一帧再问） */
static int qlwl_clip_get(char *buf, size_t cap, void *user)
{
    QLWlWindow *w = (QLWlWindow *)user;
    if (w == NULL || buf == NULL || cap == 0)
        return 0;

    if (w->clip_in_ready) {
        const size_t have = (size_t)w->clip_in_len;
        const size_t n = (have < cap - 1u) ? have : cap - 1u;
        if (n > 0)
            memcpy(buf, w->clip_in, n);
        buf[n] = '\0';
        w->clip_in_ready = 0;
        return 1;
    }

    if (w->clip_pipe < 0 && w->clip_offer != NULL && w->clip_mime[0] != '\0') {
        int fds[2];
        if (pipe(fds) != 0)
            return 0;
        wl_data_offer_receive(w->clip_offer, w->clip_mime, fds[1]);
        close(fds[1]);
        w->clip_pipe = fds[0];
        (void)fcntl(w->clip_pipe, F_SETFL, O_NONBLOCK);
        (void)wl_display_flush(w->dpy);
    }

    if (w->clip_pipe >= 0) {
        uint8_t tmp[256];
        for (;;) {
            const ssize_t n = read(w->clip_pipe, tmp, sizeof(tmp));
            if (n > 0) {
                size_t room = (size_t)QLWL_CLIP_MAX - 1u - (size_t)w->clip_in_len;
                if ((size_t)n < room)
                    room = (size_t)n;
                if (room > 0) {
                    memcpy(w->clip_in + w->clip_in_len, tmp, room);
                    w->clip_in_len += (int)room;
                }
                continue;
            }
            if (n == 0) {                    /* EOF：内容到齐 */
                close(w->clip_pipe);
                w->clip_pipe = -1;
                w->clip_in[w->clip_in_len] = '\0';
                w->clip_in_ready = 1;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;                    /* 还没写完，下一帧再问 */
            close(w->clip_pipe);
            w->clip_pipe = -1;
            w->clip_in[w->clip_in_len] = '\0';
            w->clip_in_ready = 1;
            break;
        }
    }
    return 0;
}

static const QLUIClipboard qlwl_clipboard_ops = {
    .set_text = qlwl_clip_set,
    .get_text = qlwl_clip_get,
};

/* ------------------------------ 键盘按键 ------------------------------ */

static void qlwl_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                        uint32_t time, uint32_t key, uint32_t state)
{
    (void)kb; (void)serial; (void)time;
    QLWlWindow *w = (QLWlWindow *)data;

    const uint32_t code = ql_evdev_key(key);
    if (code == QL_KEY_NONE)
        return;

    const int down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    ql_input_push_key(code, down, w->mods);

    /*
     * 可打印字符顺带合成文本输入（没有 xkbcommon 时按 US 布局近似）。
     * 原生 Wayland 下真正的中文/拼音要靠 `zwp_text_input_v3`，那是下一步。
     */
    if (down && code < 0x80 &&
        (w->mods & (QL_MOD_CTRL | QL_MOD_ALT | QL_MOD_META)) == 0) {
        char buf[2] = { (char)code, '\0' };
        if ((w->mods & QL_MOD_SHIFT) != 0 && buf[0] >= 'a' && buf[0] <= 'z')
            buf[0] = (char)(buf[0] - 'a' + 'A');
        ql_input_push_text(buf);
    }
}

/* ------------------------------ seat / registry ------------------------------ */

static void qlwl_seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    QLWlWindow *w = (QLWlWindow *)data;
    const int has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    const int has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    const int has_touch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0;

    if (has_kb && w->keyboard == NULL) {
        w->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard, &qlwl_kb_listener, w);
    } else if (!has_kb && w->keyboard != NULL) {
        wl_keyboard_destroy(w->keyboard);
        w->keyboard = NULL;
    }
    if (has_ptr && w->pointer == NULL) {
        w->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer, &qlwl_ptr_listener, w);
    } else if (!has_ptr && w->pointer != NULL) {
        wl_pointer_destroy(w->pointer);
        w->pointer = NULL;
    }
    if (has_touch && w->touch == NULL) {
        w->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(w->touch, &qlwl_touch_listener, w);
    } else if (!has_touch && w->touch != NULL) {
        wl_touch_destroy(w->touch);
        w->touch = NULL;
    }
}

static void qlwl_seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener qlwl_seat_listener = {
    .capabilities = qlwl_seat_caps,
    .name = qlwl_seat_name,
};

static void qlwl_registry_global(void *data, struct wl_registry *reg, uint32_t name,
                                 const char *iface, uint32_t version)
{
    QLWlWindow *w = (QLWlWindow *)data;
    (void)version;

    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        w->compositor = (struct wl_compositor *)wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        w->shm = (struct wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        w->wm_base = (struct xdg_wm_base *)wl_registry_bind(reg, name,
                                                           &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wm_base, &qlwl_wm_listener, w);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        w->seat = (struct wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(w->seat, &qlwl_seat_listener, w);
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        w->data_mgr = (struct wl_data_device_manager *)wl_registry_bind(
            reg, name, &wl_data_device_manager_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (w->output_count < QLWL_MAX_OUTPUTS) {
            struct wl_output *o = (struct wl_output *)wl_registry_bind(
                reg, name, &wl_output_interface, 2);
            if (o != NULL) {
                w->outputs[w->output_count].obj = o;
                w->outputs[w->output_count].scale = 1;
                w->output_count++;
                wl_output_add_listener(o, &qlwl_out_listener, w);
            }
        }
    }
}

static void qlwl_registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener qlwl_registry_listener = {
    .global = qlwl_registry_global,
    .global_remove = qlwl_registry_remove,
};

/* ------------------------------ 打开 / 关闭 / 泵事件 ------------------------------ */

static QLWL_MAYBE_UNUSED void qlwl_close(QLWlWindow *w);



/*
 * 建窗口。`scale_override` 为 0 表示听合成器的，否则强制用这个整数倍
 * （`QLGL_WAYLAND_SCALE` 就是它，方便在 1x 屏上验证 2x 路径）。
 *
 * 不建渲染资源：CPU 后端自己建 shm 缓冲区，GPU 后端自己建 EGL surface。
 */
static QLWL_MAYBE_UNUSED int qlwl_open(QLWlWindow *w, const char *title, int32_t scale_override)
{
    if (w == NULL)
        return 0;
    memset(w, 0, sizeof(*w));
    w->clip_pipe = -1;
    w->scale_lock = (scale_override > 0) ? scale_override : 0;
    w->scale = (scale_override > 0) ? scale_override : 1;
    snprintf(w->title, sizeof(w->title), "%s", (title != NULL) ? title : "QLGL");

    w->dpy = wl_display_connect(NULL);
    if (w->dpy == NULL) {
        QL_LOGE("wayland: cannot connect (WAYLAND_DISPLAY=%s, XDG_RUNTIME_DIR=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        return 0;
    }
    w->registry = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(w->registry, &qlwl_registry_listener, w);
    wl_display_roundtrip(w->dpy);      /* 全局对象 */
    wl_display_roundtrip(w->dpy);      /* seat 能力 / output scale */

    if (w->compositor == NULL || w->wm_base == NULL) {
        QL_LOGE("wayland: missing globals (compositor=%p xdg_wm_base=%p)",
                (void *)w->compositor, (void *)w->wm_base);
        qlwl_close(w);
        return 0;
    }

    w->surface = wl_compositor_create_surface(w->compositor);
    wl_surface_add_listener(w->surface, &qlwl_surf_listener, w);
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
    xdg_surface_add_listener(w->xdg_surface, &qlwl_xdg_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->toplevel, &qlwl_toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, w->title);
    xdg_toplevel_set_app_id(w->toplevel, "qlgl");
    wl_surface_commit(w->surface);

    /*
     * xdg-shell 的第一帧：必须把 configure 收下来才能画（否则 `!configured`
     * 会让提交被静默丢弃 —— 实测前几帧全丢）。
     */
    if (wl_display_roundtrip(w->dpy) < 0) {
        QL_LOGE("wayland: roundtrip for configure failed");
        qlwl_close(w);
        return 0;
    }

    if (w->data_mgr != NULL && w->seat != NULL) {
        w->data_dev = wl_data_device_manager_get_data_device(w->data_mgr, w->seat);
        if (w->data_dev != NULL)
            wl_data_device_add_listener(w->data_dev, &qlwl_clip_dev_listener, w);
    }

    wl_surface_set_buffer_scale(w->surface, w->scale);
    return 1;
}

static void qlwl_close(QLWlWindow *w)
{
    if (w == NULL)
        return;
    if (w->clip_pipe >= 0) {
        close(w->clip_pipe);
        w->clip_pipe = -1;
    }
    if (w->clip_offer != NULL)
        wl_data_offer_destroy(w->clip_offer);
    if (w->clip_source != NULL)
        wl_data_source_destroy(w->clip_source);
    if (w->data_dev != NULL)
        wl_data_device_destroy(w->data_dev);
    if (w->data_mgr != NULL)
        wl_data_device_manager_destroy(w->data_mgr);
    if (w->frame_cb != NULL)
        wl_callback_destroy(w->frame_cb);
    if (w->touch != NULL)
        wl_touch_destroy(w->touch);
    if (w->pointer != NULL)
        wl_pointer_destroy(w->pointer);
    if (w->keyboard != NULL)
        wl_keyboard_destroy(w->keyboard);
    if (w->seat != NULL)
        wl_seat_destroy(w->seat);
    for (int i = 0; i < w->output_count; i++)
        if (w->outputs[i].obj != NULL)
            wl_output_destroy(w->outputs[i].obj);
    if (w->toplevel != NULL)
        xdg_toplevel_destroy(w->toplevel);
    if (w->xdg_surface != NULL)
        xdg_surface_destroy(w->xdg_surface);
    if (w->surface != NULL)
        wl_surface_destroy(w->surface);
    if (w->wm_base != NULL)
        xdg_wm_base_destroy(w->wm_base);
    if (w->shm != NULL)
        wl_shm_destroy(w->shm);
    if (w->compositor != NULL)
        wl_compositor_destroy(w->compositor);
    if (w->registry != NULL)
        wl_registry_destroy(w->registry);
    if (w->dpy != NULL) {
        wl_display_flush(w->dpy);
        wl_display_disconnect(w->dpy);
        w->dpy = NULL;
    }
}

/*
 * 处理挂起事件（非阻塞）。返回 0 = 窗口关了或连接断了。
 *
 * 与 X11 后端的 `poll()` 同语义：调用方在主循环里每帧调一次即可。
 */
static QLWL_MAYBE_UNUSED int qlwl_pump(QLWlWindow *w)
{
    if (w == NULL || w->dpy == NULL || w->closed)
        return 0;

    if (wl_display_dispatch_pending(w->dpy) < 0)
        return 0;

    while (wl_display_prepare_read(w->dpy) != 0)
        if (wl_display_dispatch_pending(w->dpy) < 0)
            return 0;
    (void)wl_display_flush(w->dpy);

    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(w->dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, 0) > 0) {
        if (wl_display_read_events(w->dpy) < 0)
            return 0;
    } else {
        wl_display_cancel_read(w->dpy);
    }
    if (wl_display_dispatch_pending(w->dpy) < 0)
        return 0;

    return w->closed ? 0 : 1;
}

/* 把 Wayland 剪贴板接到界面上 */
static QLWL_MAYBE_UNUSED void qlwl_attach_clipboard(QLWlWindow *w, QLUIRoot *ui)
{
    if (w == NULL || ui == NULL || w->data_mgr == NULL)
        return;
    ql_ui_set_clipboard(ui, &qlwl_clipboard_ops, w);
}

#endif /* QLGL_ENABLE_WAYLAND */

#endif /* QLGL_PORT_QLWINDOW_WAYLAND_H */
