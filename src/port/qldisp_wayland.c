/**
 * @file    qldisp_wayland.c
 * @brief   Linux Wayland 窗口后端：xdg-shell + wl_shm 双缓冲。
 */
/* memfd_create / ftruncate 需要 _GNU_SOURCE；只在平台后端里开，不影响主库 */
#define _GNU_SOURCE 1

#include <qlgl/qlgl.h>

#ifdef QLGL_ENABLE_WAYLAND

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "qlinput_evdev.h"

#define QL_WL_BUFFERS   2
/* 剪贴板文本上限（与 X11 后端一致） */
#define QL_WL_CLIP_MAX  1024

typedef struct {
    struct wl_buffer *buffer;
    uint8_t          *data;
    size_t            size;
    int               busy;
} QLWaylandBuffer;

/* 最多跟踪几块输出（笔记本 + 外接屏就够；多了只影响"换屏后谁说了算"） */
#define QL_WL_MAX_OUTPUTS 4

typedef struct {
    struct wl_output *obj;
    int32_t           scale;      /* wl_output.scale，至少 1 */
} QLWaylandOutput;

typedef struct {
    QLDispDriver base;

    struct wl_display    *dpy;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct xdg_wm_base   *wm_base;
    struct wl_seat       *seat;
    struct wl_keyboard   *keyboard;
    struct wl_pointer    *pointer;
    struct wl_touch      *touch;

    /* 输入状态：后端自己维护，渲染/控件层不关心 */
    uint8_t         buttons;
    uint8_t         mods;
    wl_fixed_t      pointer_x;
    wl_fixed_t      pointer_y;

    struct wl_surface   *surface;
    struct xdg_surface  *xdg_surface;
    struct xdg_toplevel *toplevel;

    /*
     * 高 DPI：`wl_output.scale`（整数倍）决定缓冲区要按几倍分配。
     * `css_w/css_h` 是**逻辑尺寸**（应用要的 320x240），缓冲区与
     * `base.width/height` 是物理尺寸（css × scale），画面靠
     * `wl_surface_set_buffer_scale()` 让合成器缩回去。
     */
    /* 剪贴板（wl_data_device） */
    struct wl_data_device_manager *data_mgr;
    struct wl_data_device         *data_dev;
    struct wl_data_source         *clip_source;
    struct wl_data_offer          *clip_offer;
    char                           clip_mime[40];
    char                           clip_out[QL_WL_CLIP_MAX];
    int                            clip_out_len;
    char                           clip_in[QL_WL_CLIP_MAX];
    int                            clip_in_len;
    int                            clip_in_ready;
    int                            clip_pipe;      /* 正在读的管道（-1 = 无） */
    uint32_t                       last_serial;    /* set_selection 要用的串号 */

    QLWaylandOutput   outputs[QL_WL_MAX_OUTPUTS];
    int               output_count;
    struct wl_output *cur_output;   /* surface.enter 来的那块（多数场景只有一块） */
    ql_coord_t        css_w;
    ql_coord_t        css_h;
    int32_t           scale;        /* 1 = 不缩放 */
    int               resize_pending;  /* 尺寸/缩放变了，调用方要重建画布并重排 */

    QLWaylandBuffer  buffers[QL_WL_BUFFERS];
    int              current;        /* 下次 flush 用哪个 */

    uint8_t         *pixels;         /* 渲染器用的持久帧缓冲 */
    uint32_t         stride;

    /* 一帧内的提交批处理 */
    int              pending;        /* 本帧已选中的缓冲区下标，-1 = 未选 */
    QLRect           damage;
    int              has_damage;

    /*
     * `wl_surface.frame` 回调：合成器在**上一次提交被呈现**之后才回。
     * 用它把提交节奏压到刷新率 —— 不然 60 Hz 的屏上跑 1000 fps 的空转
     * 提交既费电又白占带宽（roadmap 里"帧回调 = 省电关键"说的就是它）。
     */
    struct wl_callback *frame_cb;
    int              frame_pending;   /* 已请求、还没回 */
    int              frame_stall;     /* 连续被节流几次（兜底：太久就强提交） */

    uint32_t         commit_count;
    uint32_t         skip_count;
    int              closed;
    int              configured;
    char             title[64];
} QLDispWayland;

/* ------------------------------ shm 工具 ------------------------------ */

static int create_shm_file(size_t size)
{
    char name[64];
    snprintf(name, sizeof(name), "/qlgl-%d-%u", (int)getpid(), (unsigned)size);

    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------ 缓冲区 ------------------------------ */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    QLWaylandBuffer *slot = (QLWaylandBuffer *)data;
    QL_UNUSED(buffer);
    slot->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int create_buffer(QLDispWayland *w, QLWaylandBuffer *slot, uint32_t format)
{
    size_t size = (size_t)w->stride * (size_t)w->base.height;

    int fd = create_shm_file(size);
    if (fd < 0) {
        QL_LOGE("wayland: memfd_create failed (%s)", strerror(errno));
        return 0;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        QL_LOGE("wayland: mmap failed (%s)", strerror(errno));
        close(fd);
        return 0;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)size);
    slot->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w->base.width,
                                             (int32_t)w->base.height,
                                             (int32_t)w->stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (slot->buffer == NULL) {
        munmap(data, size);
        return 0;
    }

    slot->data = (uint8_t *)data;
    slot->size = size;
    slot->busy = 0;
    wl_buffer_add_listener(slot->buffer, &buffer_listener, slot);
    return 1;
}

/* ------------------------------ 剪贴板（wl_data_device） ------------------------------ */

static const char *const k_clip_mimes[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
};

static void clip_offer_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
    QL_UNUSED(offer);
    /* 只记第一个文本类型：我们不做富文本 */
    QLDispWayland *w = (QLDispWayland *)data;
    if (w == NULL || w->clip_mime[0] != '\0')
        return;
    for (size_t i = 0; i < sizeof(k_clip_mimes) / sizeof(k_clip_mimes[0]); i++)
        if (strcmp(mime, k_clip_mimes[i]) == 0) {
            snprintf(w->clip_mime, sizeof(w->clip_mime), "%s", mime);
            return;
        }
}

static const struct wl_data_offer_listener clip_offer_listener = {
    .offer = clip_offer_offer,
};

/* 新 offer 先到：这里只挂监听器（不能重复挂，否则 libwayland 会报
 * "proxy already has listener"） */
static void clip_dev_data_offer(void *data, struct wl_data_device *dev,
                                struct wl_data_offer *offer)
{
    QL_UNUSED(dev);
    QLDispWayland *w = (QLDispWayland *)data;
    if (offer != NULL)
        wl_data_offer_add_listener(offer, &clip_offer_listener, w);
}

/* 再告诉我们"当前 selection 是谁"（NULL = 剪贴板空了） */
static void clip_selection(void *data, struct wl_data_device *dev,
                           struct wl_data_offer *offer)
{
    QL_UNUSED(dev);
    QLDispWayland *w = (QLDispWayland *)data;
    if (w->clip_offer != NULL && w->clip_offer != offer) {
        wl_data_offer_destroy(w->clip_offer);
    }
    w->clip_offer = offer;
    w->clip_mime[0] = '\0';
    w->clip_in_len = 0;
    w->clip_in_ready = 0;
    w->clip_pipe = -1;
}

static void clip_dev_enter(void *data, struct wl_data_device *dev, uint32_t serial,
                           struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                           struct wl_data_offer *offer)
{
    QL_UNUSED(data); QL_UNUSED(dev); QL_UNUSED(serial);
    QL_UNUSED(surface); QL_UNUSED(x); QL_UNUSED(y); QL_UNUSED(offer);
}

static void clip_dev_leave(void *data, struct wl_data_device *dev)
{
    QL_UNUSED(data); QL_UNUSED(dev);
}

static void clip_dev_motion(void *data, struct wl_data_device *dev, uint32_t time,
                            wl_fixed_t x, wl_fixed_t y)
{
    QL_UNUSED(data); QL_UNUSED(dev); QL_UNUSED(time); QL_UNUSED(x); QL_UNUSED(y);
}

static void clip_dev_drop(void *data, struct wl_data_device *dev)
{
    QL_UNUSED(data); QL_UNUSED(dev);
}

static const struct wl_data_device_listener clip_dev_listener = {
    .data_offer = clip_dev_data_offer,
    .enter = clip_dev_enter,
    .leave = clip_dev_leave,
    .motion = clip_dev_motion,
    .drop = clip_dev_drop,
    .selection = clip_selection,
};

/* 有人来要内容（我们持有 selection） */
static void clip_source_send(void *data, struct wl_data_source *src, const char *mime,
                             int32_t fd)
{
    QL_UNUSED(src);
    QL_UNUSED(mime);
    QLDispWayland *w = (QLDispWayland *)data;
    const char *text = w->clip_out;
    size_t left = (size_t)w->clip_out_len;
    while (left > 0) {
        const ssize_t n = write(fd, text, left);
        if (n <= 0)
            break;
        text += n;
        left -= (size_t)n;
    }
    close(fd);                        /* 关写端 = 告诉对方"完了" */
}

static void clip_source_cancelled(void *data, struct wl_data_source *src)
{
    QL_UNUSED(data);
    wl_data_source_destroy(src);
}

static void clip_source_target(void *data, struct wl_data_source *src, const char *mime)
{
    QL_UNUSED(data); QL_UNUSED(src); QL_UNUSED(mime);
}

static const struct wl_data_source_listener clip_source_listener = {
    .target = clip_source_target,
    .send = clip_source_send,
    .cancelled = clip_source_cancelled,
};

static void clip_set_text(const char *utf8, void *user)
{
    QLDispWayland *w = (QLDispWayland *)user;
    if (w == NULL || w->data_dev == NULL || w->data_mgr == NULL)
        return;

    size_t n = (utf8 != NULL) ? strlen(utf8) : 0u;
    if (n > (size_t)QL_WL_CLIP_MAX - 1u)
        n = (size_t)QL_WL_CLIP_MAX - 1u;
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
    for (size_t i = 0; i < sizeof(k_clip_mimes) / sizeof(k_clip_mimes[0]); i++)
        wl_data_source_offer(src, k_clip_mimes[i]);
    wl_data_source_add_listener(src, &clip_source_listener, w);
    w->clip_source = src;
    /* serial 必须是"当前这次键盘/鼠标事件"的，否则合成器会拒绝 */
    wl_data_device_set_selection(w->data_dev, src, w->last_serial);
}

/*
 * 读回剪贴板。返回 1 = 内容已就绪（可能是空串），0 = 还没到。
 * 走匿名管道 + 非阻塞读：对方可能分几次写，读不到 EOF 就下一帧再问。
 */
static int clip_get_text(char *buf, size_t cap, void *user)
{
    QLDispWayland *w = (QLDispWayland *)user;
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
        fcntl(w->clip_pipe, F_SETFL, O_NONBLOCK);
        wl_display_flush(w->dpy);
    }

    if (w->clip_pipe >= 0) {
        uint8_t tmp[256];
        for (;;) {
            const ssize_t n = read(w->clip_pipe, tmp, sizeof(tmp));
            if (n > 0) {
                size_t room = (size_t)QL_WL_CLIP_MAX - 1u - (size_t)w->clip_in_len;
                if ((size_t)n < room)
                    room = (size_t)n;
                if (room > 0) {
                    memcpy(w->clip_in + w->clip_in_len, tmp, room);
                    w->clip_in_len += (int)room;
                }
                continue;
            }
            if (n == 0) {                      /* EOF：内容到齐 */
                close(w->clip_pipe);
                w->clip_pipe = -1;
                w->clip_in[w->clip_in_len] = '\0';
                w->clip_in_ready = 1;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;                      /* 还没写完，下一帧再问 */
            close(w->clip_pipe);               /* 出错：当作空内容收尾 */
            w->clip_pipe = -1;
            w->clip_in[w->clip_in_len] = '\0';
            w->clip_in_ready = 1;
            break;
        }
    }
    return 0;
}

static const QLUIClipboard k_wl_clipboard = {
    .set_text = clip_set_text,
    .get_text = clip_get_text,
};

void ql_disp_wayland_attach_clipboard(QLDispDriver *driver, QLUIRoot *ui)
{
    if (driver == NULL || ui == NULL)
        return;
    QLDispWayland *w = (QLDispWayland *)driver;
    if (w->data_mgr == NULL) {
        QL_LOGW("wayland: 合成器没有 wl_data_device_manager，剪贴板不可用");
        return;
    }
    ql_ui_set_clipboard(ui, &k_wl_clipboard, w);
}

/* ------------------------------ 高 DPI ------------------------------ */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    QL_UNUSED(data); QL_UNUSED(o); QL_UNUSED(x); QL_UNUSED(y);
    QL_UNUSED(pw); QL_UNUSED(ph); QL_UNUSED(subpixel);
    QL_UNUSED(make); QL_UNUSED(model); QL_UNUSED(transform);
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    QL_UNUSED(data); QL_UNUSED(o); QL_UNUSED(flags);
    QL_UNUSED(width); QL_UNUSED(height); QL_UNUSED(refresh);
}

static void output_done(void *data, struct wl_output *o)
{
    QL_UNUSED(data); QL_UNUSED(o);
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
    QL_UNUSED(data);
    for (int i = 0; i < QL_WL_MAX_OUTPUTS; i++) {
        QLWaylandOutput *out = &((QLDispWayland *)data)->outputs[i];
        if (out->obj == o) {
            out->scale = (factor > 0) ? factor : 1;
            break;
        }
    }
}

static void output_name(void *data, struct wl_output *o, const char *name)
{
    QL_UNUSED(data); QL_UNUSED(o); QL_UNUSED(name);
}

static void output_description(void *data, struct wl_output *o, const char *desc)
{
    QL_UNUSED(data); QL_UNUSED(o); QL_UNUSED(desc);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static ql_coord_t scale_to_fx(int32_t scale)
{
    return (ql_coord_t)((scale > 0) ? scale : 1) * QL_SCALE_ONE;
}

static void wayland_surface_enter(void *data, struct wl_surface *surface,
                                  struct wl_output *output)
{
    QL_UNUSED(surface);
    QLDispWayland *w = (QLDispWayland *)data;
    int32_t s = 1;
    for (int i = 0; i < QL_WL_MAX_OUTPUTS; i++)
        if (w->outputs[i].obj == output) {
            s = w->outputs[i].scale;
            break;
        }
    w->cur_output = output;
    if (s != w->scale) {
        w->scale = s;
        w->resize_pending = 1;      /* 由 poll 里重建缓冲区 */
    }
}

static void wayland_surface_leave(void *data, struct wl_surface *surface,
                                  struct wl_output *output)
{
    QL_UNUSED(surface);
    QL_UNUSED(output);
    QL_UNUSED(data);
}

static const struct wl_surface_listener surface_listener = {
    .enter = wayland_surface_enter,
    .leave = wayland_surface_leave,
};

/* 释放 shm 缓冲区（保留 surface/xdg 对象） */
static void buffers_release(QLDispWayland *w)
{
    for (int i = 0; i < QL_WL_BUFFERS; i++) {
        if (w->buffers[i].buffer != NULL) {
            wl_buffer_destroy(w->buffers[i].buffer);
            w->buffers[i].buffer = NULL;
        }
        if (w->buffers[i].data != NULL) {
            munmap(w->buffers[i].data, w->buffers[i].size);
            w->buffers[i].data = NULL;
        }
        w->buffers[i].size = 0;
        w->buffers[i].busy = 0;
    }
    w->pending = -1;
}

/*
 * 按当前缩放重建帧缓冲与 shm 缓冲区。
 *
 * 为什么整块重建而不是"渲染时放大"：合成器的 `set_buffer_scale(N)` 要求
 * 缓冲区本身就是 N 倍物理像素（它只负责把逻辑尺寸缩回去）。所以缩放一变，
 * 缓冲区尺寸、stride、`pixels`、`base.width/height` 全要跟着变，
 * 画布也必须由调用方重建（`ql_disp_wayland_resize_pending()` 通知）。
 */
static int buffers_rebuild(QLDispWayland *w)
{
    const ql_coord_t scale = (w->scale > 0) ? w->scale : 1;
    const ql_coord_t pw = w->css_w * scale;
    const ql_coord_t ph = w->css_h * scale;
    const uint32_t stride = (uint32_t)pw * 4u;
    if (pw <= 0 || ph <= 0)
        return 0;

    buffers_release(w);

    uint8_t *pixels = (uint8_t *)ql_mem_calloc(1, (size_t)stride * (size_t)ph);
    if (pixels == NULL)
        return 0;
    ql_mem_free(w->pixels);
    w->pixels = pixels;
    w->stride = stride;

    w->base.width = pw;
    w->base.height = ph;
    w->base.framebuffer = pixels;
    w->base.stride = stride;
    ql_disp_set_scale(&w->base, scale_to_fx(w->scale));

    if (w->surface != NULL)
        wl_surface_set_buffer_scale(w->surface, w->scale);

    for (int i = 0; i < QL_WL_BUFFERS; i++) {
        if (!create_buffer(w, &w->buffers[i], WL_SHM_FORMAT_XRGB8888)) {
            QL_LOGE("wayland: cannot recreate shm buffer %d (scale %d)", i, (int)w->scale);
            return 0;
        }
    }
    w->current = 0;
    return 1;
}

/* ------------------------------ 监听器 ------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    QL_UNUSED(data);
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    QLDispWayland *w = (QLDispWayland *)data;
    xdg_surface_ack_configure(surface, serial);
    w->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    QL_UNUSED(data);
    QL_UNUSED(toplevel);
    QL_UNUSED(width);
    QL_UNUSED(height);
    QL_UNUSED(states);
    /* v0.1 固定尺寸，忽略合成器给的尺寸 */
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    QL_UNUSED(toplevel);
    ((QLDispWayland *)data)->closed = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                            int32_t fd, uint32_t size)
{
    QL_UNUSED(data);
    QL_UNUSED(kb);
    QL_UNUSED(format);
    QL_UNUSED(size);
    close(fd);                    /* 我们不用键盘映射表 */
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    QL_UNUSED(kb);
    QL_UNUSED(surface); QL_UNUSED(keys);
    /* set_selection 要"最近一次输入事件"的 serial，否则合成器会忽略 */
    ((QLDispWayland *)data)->last_serial = serial;
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface)
{
    QL_UNUSED(data); QL_UNUSED(kb); QL_UNUSED(serial); QL_UNUSED(surface);
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    QL_UNUSED(kb); QL_UNUSED(serial); QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;

    uint32_t code = ql_evdev_key(key);
    if (code == QL_KEY_NONE)
        return;

    int down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    ql_input_push_key(code, down, w->mods);

    /* 可打印字符顺带合成文本输入：没有 xkbcommon 时按 US 布局近似 */
    if (down && code < 0x80 && (w->mods & (QL_MOD_CTRL | QL_MOD_ALT | QL_MOD_META)) == 0) {
        char buf[2] = { (char)code, '\0' };
        if ((w->mods & QL_MOD_SHIFT) != 0 && buf[0] >= 'a' && buf[0] <= 'z')
            buf[0] = (char)(buf[0] - 'a' + 'A');
        ql_input_push_text(buf);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    QL_UNUSED(kb); QL_UNUSED(serial); QL_UNUSED(lat); QL_UNUSED(grp);
    QLDispWayland *w = (QLDispWayland *)data;

    /* xkb 修饰键掩码：1=Shift 4=Ctrl 8=Alt 64=Super */
    uint8_t mods = 0;
    if ((dep & 1u) != 0)  mods |= QL_MOD_SHIFT;
    if ((dep & 4u) != 0)  mods |= QL_MOD_CTRL;
    if ((dep & 8u) != 0)  mods |= QL_MOD_ALT;
    if ((dep & 64u) != 0) mods |= QL_MOD_META;
    if ((lock & 2u) != 0) mods |= QL_MOD_CAPS;
    w->mods = mods;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                                 int32_t delay)
{
    QL_UNUSED(data); QL_UNUSED(kb); QL_UNUSED(rate); QL_UNUSED(delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ----- wl_pointer ----- */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    QL_UNUSED(pointer); QL_UNUSED(serial); QL_UNUSED(surface);
    QLDispWayland *w = (QLDispWayland *)data;
    w->pointer_x = sx;
    w->pointer_y = sy;
    ql_input_push_move(wl_fixed_to_int(sx), wl_fixed_to_int(sy), w->buttons);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    QL_UNUSED(pointer); QL_UNUSED(serial); QL_UNUSED(surface);
    QL_UNUSED(data);
    ql_input_push_leave();
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    QL_UNUSED(pointer); QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;
    w->pointer_x = sx;
    w->pointer_y = sy;
    ql_input_push_move(wl_fixed_to_int(sx), wl_fixed_to_int(sy), w->buttons);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    QL_UNUSED(pointer); QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;
    w->last_serial = serial;

    uint8_t btn = ql_evdev_button(button);
    if (btn == 0xFF)
        return;

    int down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (down)
        w->buttons |= (uint8_t)(1u << btn);
    else
        w->buttons &= (uint8_t)~(1u << btn);

    ql_input_push_button(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                         btn, down);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    QL_UNUSED(pointer); QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;

    /* wl_fixed 是定点滚动量，一格约 15；放大成"行"便于上层按行滚动 */
    int32_t delta = (int32_t)(wl_fixed_to_double(value) * 20.0);
    int32_t dx = (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) ? delta : 0;
    int32_t dy = (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? delta : 0;
    ql_input_push_wheel(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                        dx, dy);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

/* ----- wl_touch ----- */

/*
 * 触摸直接映射成"主键按下 / 移动 / 抬起"：界面层只认指针事件，触摸与鼠标
 * 走同一条路（多指手势先不做 —— 没有真实手机之前写了也没法验证）。
 *
 * 坐标同样是 **surface 的逻辑坐标**（= CSS px），所以不用换算；
 * 拖动与点击的区分交给界面层（移动超过阈值就不算点击）。
 */
static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t x, wl_fixed_t y)
{
    QL_UNUSED(touch); QL_UNUSED(serial); QL_UNUSED(time);
    QL_UNUSED(surface); QL_UNUSED(id);
    QLDispWayland *w = (QLDispWayland *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    w->buttons |= 1u;                      /* 主键 */
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
    ql_input_push_button(wl_fixed_to_int(x), wl_fixed_to_int(y), 0, 1);
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
                     uint32_t time, int32_t id)
{
    QL_UNUSED(touch); QL_UNUSED(serial); QL_UNUSED(time); QL_UNUSED(id);
    QLDispWayland *w = (QLDispWayland *)data;
    ql_input_push_button(wl_fixed_to_int(w->pointer_x), wl_fixed_to_int(w->pointer_y),
                         0, 0);
    w->buttons &= (uint8_t)~1u;
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
                         int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    QL_UNUSED(touch); QL_UNUSED(time); QL_UNUSED(id);
    QLDispWayland *w = (QLDispWayland *)data;
    w->pointer_x = x;
    w->pointer_y = y;
    ql_input_push_move(wl_fixed_to_int(x), wl_fixed_to_int(y), w->buttons);
}

static void touch_frame(void *data, struct wl_touch *touch)
{
    QL_UNUSED(data); QL_UNUSED(touch);
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
    QL_UNUSED(touch);
    QLDispWayland *w = (QLDispWayland *)data;
    if ((w->buttons & 1u) != 0) {
        ql_input_push_button(wl_fixed_to_int(w->pointer_x),
                             wl_fixed_to_int(w->pointer_y), 0, 0);
        w->buttons &= (uint8_t)~1u;
    }
}

static void touch_shape(void *data, struct wl_touch *touch, int32_t id,
                        wl_fixed_t major, wl_fixed_t minor)
{
    QL_UNUSED(data); QL_UNUSED(touch); QL_UNUSED(id);
    QL_UNUSED(major); QL_UNUSED(minor);
}

static void touch_orientation(void *data, struct wl_touch *touch, int32_t id,
                              wl_fixed_t orientation)
{
    QL_UNUSED(data); QL_UNUSED(touch); QL_UNUSED(id); QL_UNUSED(orientation);
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
    .shape = touch_shape,
    .orientation = touch_orientation,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    QLDispWayland *w = (QLDispWayland *)data;
    int has_keyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    int has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    int has_touch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0;

    if (has_keyboard && w->keyboard == NULL) {
        w->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard, &keyboard_listener, w);
    } else if (!has_keyboard && w->keyboard != NULL) {
        wl_keyboard_destroy(w->keyboard);
        w->keyboard = NULL;
    }

    if (has_pointer && w->pointer == NULL) {
        w->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer, &pointer_listener, w);
    } else if (!has_pointer && w->pointer != NULL) {
        wl_pointer_destroy(w->pointer);
        w->pointer = NULL;
    }

    if (has_touch && w->touch == NULL) {
        w->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(w->touch, &touch_listener, w);
    } else if (!has_touch && w->touch != NULL) {
        wl_touch_destroy(w->touch);
        w->touch = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    QL_UNUSED(data); QL_UNUSED(seat); QL_UNUSED(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    QLDispWayland *w = (QLDispWayland *)data;
    QL_UNUSED(version);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        w->compositor = (struct wl_compositor *)wl_registry_bind(
            registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        w->shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        w->wm_base = (struct xdg_wm_base *)wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        w->data_mgr = (struct wl_data_device_manager *)wl_registry_bind(
            registry, name, &wl_data_device_manager_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (w->output_count < QL_WL_MAX_OUTPUTS) {
            /* v2 才有 scale 事件；绑定 v2 足够（name/description 不用） */
            struct wl_output *o = (struct wl_output *)wl_registry_bind(
                registry, name, &wl_output_interface, 2);
            if (o != NULL) {
                QLWaylandOutput *slot = &w->outputs[w->output_count++];
                slot->obj = o;
                slot->scale = 1;
                wl_output_add_listener(o, &output_listener, w);
            }
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        w->seat = (struct wl_seat *)wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(w->seat, &seat_listener, w);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    QL_UNUSED(data); QL_UNUSED(registry); QL_UNUSED(name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------ flush ------------------------------ */

/*
 * 一帧内的脏区先全部拷进同一个 shm 缓冲区，最后在 flush_done 里
 * attach + damage + commit 一次，避免每块脏区都提交一次。
 */
static int wayland_flush(QLDispDriver *self, QLRect area)
{
    QLDispWayland *w = (QLDispWayland *)self;
    if (w->closed || w->dpy == NULL || ql_rect_is_empty(area))
        return 0;

    if (!w->configured) {
        wl_display_dispatch_pending(w->dpy);
        if (!w->configured)
            return 0;
    }

    if (w->pending < 0) {
        /* 本帧第一次写入：挑一个空闲缓冲区并立即占住 */
        for (int i = 0; i < QL_WL_BUFFERS; i++) {
            int idx = (w->current + i) % QL_WL_BUFFERS;
            if (!w->buffers[idx].busy) {
                w->pending = idx;
                w->buffers[idx].busy = 1;
                break;
            }
        }
        if (w->pending < 0) {
            w->skip_count++;
            return 0;              /* 两个缓冲区都在用：本帧不提交 */
        }
    }

    QLWaylandBuffer *slot = &w->buffers[w->pending];

    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++) {
        memcpy(slot->data + (size_t)y * w->stride + (size_t)area.x * 4u,
               w->pixels + (size_t)y * w->stride + (size_t)area.x * 4u,
               (size_t)area.w * 4u);
    }

    w->damage = w->has_damage ? ql_rect_union(w->damage, area) : area;
    w->has_damage = 1;
    return 0;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;
    wl_callback_destroy(cb);
    w->frame_cb = NULL;
    w->frame_pending = 0;
    w->frame_stall = 0;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/*
 * 把攒下的脏区真正提交出去，并请求下一次帧回调。
 *
 * 同一时刻只挂一个回调；`frame_pending` 为真说明"上一帧还没被合成器呈现"，
 * 这时**不提交**（脏区继续攒着），等回调来了在 poll 里补提交 —— 这就是
 * 帧率节流。兜底：连续被节流 `QL_WL_MAX_STALL` 次就强提交一次，
 * 免得某个合成器不回帧回调时界面卡死。
 */
#define QL_WL_MAX_STALL 3

static void wayland_commit_pending(QLDispWayland *w)
{
    if (w->pending < 0 || !w->has_damage || w->closed || w->surface == NULL)
        return;

    if (w->frame_pending && w->frame_stall < QL_WL_MAX_STALL) {
        w->frame_stall++;
        w->skip_count++;
        return;
    }

    QLWaylandBuffer *slot = &w->buffers[w->pending];

    wl_surface_damage_buffer(w->surface, w->damage.x, w->damage.y,
                             w->damage.w, w->damage.h);
    wl_surface_attach(w->surface, slot->buffer, 0, 0);

    if (!w->frame_pending) {
        w->frame_cb = wl_surface_frame(w->surface);
        if (w->frame_cb != NULL) {
            wl_callback_add_listener(w->frame_cb, &frame_listener, w);
            w->frame_pending = 1;
        }
    }
    wl_surface_commit(w->surface);

    if (wl_display_flush(w->dpy) < 0 && errno != EAGAIN)
        QL_LOGW("wayland: flush failed (%s)", strerror(errno));

    w->current = (w->pending + 1) % QL_WL_BUFFERS;
    w->pending = -1;
    w->has_damage = 0;
    w->commit_count++;
}

static int wayland_flush_done(QLDispDriver *self)
{
    QLDispWayland *w = (QLDispWayland *)self;
    wayland_commit_pending(w);
    return 0;
}

/* ------------------------------ 创建 / 销毁 ------------------------------ */

QLDispDriver *ql_disp_wayland_create(const char *title, ql_coord_t width, ql_coord_t height,
                                     QLColorFormat format)
{
    if (width <= 0 || height <= 0) {
        QL_LOGE("wayland: invalid size %dx%d", (int)width, (int)height);
        return NULL;
    }
    if (format != QL_COLOR_FORMAT_AUTO && format != QL_COLOR_ARGB8888) {
        QL_LOGE("wayland: only ARGB8888 is supported (got %s)",
                ql_disp_format_name((uint8_t)format));
        return NULL;
    }

    QLDispWayland *w = (QLDispWayland *)ql_mem_calloc(1, sizeof(QLDispWayland));
    if (w == NULL)
        return NULL;

    strncpy(w->title, (title != NULL) ? title : "QLGL", sizeof(w->title) - 1);

    w->dpy = wl_display_connect(NULL);
    if (w->dpy == NULL) {
        QL_LOGE("wayland: cannot connect (WAYLAND_DISPLAY=%s, XDG_RUNTIME_DIR=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        ql_mem_free(w);
        return NULL;
    }

    w->registry = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(w->registry, &registry_listener, w);
    wl_display_roundtrip(w->dpy);       /* 第一轮：拿到全局对象 */
    wl_display_roundtrip(w->dpy);       /* 第二轮：拿到 seat 能力等 */

    if (w->compositor == NULL || w->shm == NULL || w->wm_base == NULL) {
        QL_LOGE("wayland: missing globals (compositor=%p shm=%p xdg_wm_base=%p)",
                (void *)w->compositor, (void *)w->shm, (void *)w->wm_base);
        wl_display_disconnect(w->dpy);
        ql_mem_free(w);
        return NULL;
    }

    /* 逻辑尺寸先记下来，缩放由 wl_output 决定（见 buffers_rebuild） */
    w->css_w = width;
    w->css_h = height;
    w->scale = 1;
    {
        /*
         * 手动指定缩放：合成器上报的 scale 是整数倍，但调试/无 HiDPI 屏时
         * 想验证 2x 效果，用 `QLGL_WAYLAND_SCALE=2` 覆盖（不影响输入，
         * 因为 pointer 坐标始终是 surface 的逻辑坐标）。
         */
        const char *env = getenv("QLGL_WAYLAND_SCALE");
        if (env != NULL && env[0] != '\0') {
            const long v = strtol(env, NULL, 10);
            if (v >= 1 && v <= 8)
                w->scale = (int32_t)v;
        }
    }

    if (ql_disp_init_fb(&w->base, (void *)(uintptr_t)1, (uint32_t)width * 4u, width,
                        height, QL_COLOR_ARGB8888, wayland_flush) != QL_OK) {
        wl_display_disconnect(w->dpy);
        ql_mem_free(w);
        return NULL;
    }
    w->base.user_data = w;
    w->base.flush_done = wayland_flush_done;
    w->pending = -1;

    w->surface = wl_compositor_create_surface(w->compositor);
    wl_surface_add_listener(w->surface, &surface_listener, w);
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
    xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);

    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, w->title);
    xdg_toplevel_set_app_id(w->toplevel, "qlgl");

    wl_surface_commit(w->surface);

    /*
     * xdg-shell 的第一帧：合成器要先把 configure 发过来我们才能画。
     * 这里做一次 roundtrip（把 configure 收下来），否则前几帧 flush 会
     * 一直因为 `!configured` 被丢掉。
     */
    if (wl_display_roundtrip(w->dpy) < 0) {
        QL_LOGE("wayland: roundtrip for configure failed");
        goto fail;
    }

    /* 剪贴板：data_device 需要 seat */
    if (w->data_mgr != NULL && w->seat != NULL) {
        w->data_dev = wl_data_device_manager_get_data_device(w->data_mgr, w->seat);
        if (w->data_dev != NULL) {
            wl_data_device_add_listener(w->data_dev, &clip_dev_listener, w);
            w->clip_pipe = -1;
        }
    }

    /* 真正分配帧缓冲 + shm 缓冲区（按当前缩放） */
    if (!buffers_rebuild(w)) {
        QL_LOGE("wayland: cannot allocate buffers");
        goto fail;
    }

    if (ql_disp_set(&w->base) != QL_OK)
        goto fail;

    QL_LOGI("wayland: window \"%s\" %dx%d ARGB8888", w->title, (int)width, (int)height);
    return &w->base;

fail:
    /* 交给 destroy 统一清理 */
    ql_disp_wayland_destroy(&w->base);
    return NULL;
}

void ql_disp_wayland_close(QLDispDriver *driver)
{
    if (driver != NULL)
        ((QLDispWayland *)driver)->closed = 1;
}

int ql_disp_wayland_poll(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;

    QLDispWayland *w = (QLDispWayland *)driver;
    if (w->dpy == NULL || w->closed)
        return 0;

    /* 先把已经到达的事件处理掉 */
    wl_display_dispatch_pending(w->dpy);

    while (wl_display_prepare_read(w->dpy) != 0) {
        if (wl_display_dispatch_pending(w->dpy) < 0)
            return 0;
    }
    wl_display_flush(w->dpy);

    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(w->dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 0);
    if (ret > 0) {
        if (wl_display_read_events(w->dpy) < 0)
            return 0;
    } else {
        wl_display_cancel_read(w->dpy);
    }

    if (wl_display_dispatch_pending(w->dpy) < 0)
        return 0;

    /* 有攒下的脏区、且上一帧已经呈现 → 立刻补一次提交 */
    if (w->has_damage && w->pending >= 0 && !w->frame_pending)
        wayland_commit_pending(w);

    /*
     * 缩放变了（surface.enter 到另一块屏、或合成器改了 scale）：
     * 缓冲区尺寸要跟着变，画布得重建 —— 由调用方通过
     * `ql_disp_wayland_resize_pending()` 得知。
     */
    if (w->resize_pending) {
        w->resize_pending = 0;
        const ql_coord_t want_w = w->base.width;
        const ql_coord_t want_h = w->base.height;
        if (buffers_rebuild(w)) {
            /* 尺寸真的变了才让调用方重建（1x → 1x 的 enter 不该触发重排） */
            if (want_w != w->base.width || want_h != w->base.height)
                w->resize_pending = 1;
        }
    }

    return w->closed ? 0 : 1;
}

void ql_disp_wayland_destroy(QLDispDriver *driver)
{
    if (driver == NULL)
        return;

    QLDispWayland *w = (QLDispWayland *)driver;
    ql_disp_unset(driver);
    w->pending = -1;

    for (int i = 0; i < QL_WL_BUFFERS; i++) {
        if (w->buffers[i].buffer != NULL)
            wl_buffer_destroy(w->buffers[i].buffer);
        if (w->buffers[i].data != NULL)
            munmap(w->buffers[i].data, w->buffers[i].size);
    }

    /*
     * wl_seat 按 version 1 绑定，所以 pointer/keyboard/seat 都是 v1 对象。
     * *_release 是 v3+ 才有的请求，v1 上必须用 *_destroy，
     * 否则会向服务器发一个不存在的方法。
     */
    if (w->touch != NULL)
        wl_touch_destroy(w->touch);
    if (w->pointer != NULL)
        wl_pointer_destroy(w->pointer);
    if (w->keyboard != NULL)
        wl_keyboard_destroy(w->keyboard);
    if (w->clip_pipe >= 0)
        close(w->clip_pipe);
    if (w->clip_offer != NULL)
        wl_data_offer_destroy(w->clip_offer);
    if (w->clip_source != NULL)
        wl_data_source_destroy(w->clip_source);
    if (w->data_dev != NULL)
        wl_data_device_destroy(w->data_dev);
    if (w->data_mgr != NULL)
        wl_data_device_manager_destroy(w->data_mgr);
    for (int i = 0; i < w->output_count; i++)
        if (w->outputs[i].obj != NULL)
            wl_output_destroy(w->outputs[i].obj);
    if (w->seat != NULL)
        wl_seat_destroy(w->seat);
    if (w->frame_cb != NULL)
        wl_callback_destroy(w->frame_cb);
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
    }

    ql_mem_free(w->pixels);
    ql_mem_free(w);
}

const char *ql_disp_wayland_title(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->title : "";
}

int ql_disp_wayland_resize_pending(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    QLDispWayland *w = (QLDispWayland *)driver;
    /* 只在 poll 里置位；这里读一次就清（与 X11 后端同语义） */
    const int p = w->resize_pending;
    w->resize_pending = 0;
    return p;
}

int ql_disp_wayland_scale(QLDispDriver *driver)
{
    return (driver != NULL) ? (int)((QLDispWayland *)driver)->scale : 1;
}

uint32_t ql_disp_wayland_commit_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->commit_count : 0;
}

uint32_t ql_disp_wayland_skip_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->skip_count : 0;
}

#endif /* QLGL_ENABLE_WAYLAND */
