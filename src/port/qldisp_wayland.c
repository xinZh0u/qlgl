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

#define QL_WL_BUFFERS   2

typedef struct {
    struct wl_buffer *buffer;
    uint8_t          *data;
    size_t            size;
    int               busy;
} QLWaylandBuffer;

typedef struct {
    QLDispDriver base;

    struct wl_display    *dpy;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct xdg_wm_base   *wm_base;
    struct wl_seat       *seat;
    struct wl_keyboard   *keyboard;

    struct wl_surface   *surface;
    struct xdg_surface  *xdg_surface;
    struct xdg_toplevel *toplevel;

    QLWaylandBuffer  buffers[QL_WL_BUFFERS];
    int              current;        /* 下次 flush 用哪个 */

    uint8_t         *pixels;         /* 渲染器用的持久帧缓冲 */
    uint32_t         stride;

    /* 一帧内的提交批处理 */
    int              pending;        /* 本帧已选中的缓冲区下标，-1 = 未选 */
    QLRect           damage;
    int              has_damage;

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
    QL_UNUSED(data); QL_UNUSED(kb); QL_UNUSED(serial);
    QL_UNUSED(surface); QL_UNUSED(keys);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface)
{
    QL_UNUSED(data); QL_UNUSED(kb); QL_UNUSED(serial); QL_UNUSED(surface);
}

/* evdev 键码：1 = Esc，16 = q */
static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    QL_UNUSED(kb); QL_UNUSED(serial); QL_UNUSED(time);
    QLDispWayland *w = (QLDispWayland *)data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    if (key == 1 || key == 16)
        w->closed = 1;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    QL_UNUSED(data); QL_UNUSED(kb); QL_UNUSED(serial);
    QL_UNUSED(dep); QL_UNUSED(lat); QL_UNUSED(lock); QL_UNUSED(grp);
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

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    QLDispWayland *w = (QLDispWayland *)data;
    int has_keyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    if (has_keyboard && w->keyboard == NULL) {
        w->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard, &keyboard_listener, w);
    } else if (!has_keyboard && w->keyboard != NULL) {
        wl_keyboard_release(w->keyboard);
        w->keyboard = NULL;
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

static int wayland_flush_done(QLDispDriver *self)
{
    QLDispWayland *w = (QLDispWayland *)self;
    if (w->pending < 0 || !w->has_damage || w->closed)
        return 0;

    QLWaylandBuffer *slot = &w->buffers[w->pending];

    wl_surface_damage_buffer(w->surface, w->damage.x, w->damage.y,
                             w->damage.w, w->damage.h);
    wl_surface_attach(w->surface, slot->buffer, 0, 0);
    wl_surface_commit(w->surface);

    if (wl_display_flush(w->dpy) < 0 && errno != EAGAIN)
        QL_LOGW("wayland: flush failed (%s)", strerror(errno));

    w->current = (w->pending + 1) % QL_WL_BUFFERS;
    w->pending = -1;
    w->has_damage = 0;
    w->commit_count++;
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

    w->stride = (uint32_t)width * 4u;
    size_t fb_size = (size_t)w->stride * (size_t)height;
    w->pixels = (uint8_t *)ql_mem_calloc(1, fb_size);
    if (w->pixels == NULL) {
        wl_display_disconnect(w->dpy);
        ql_mem_free(w);
        return NULL;
    }

    if (ql_disp_init_fb(&w->base, w->pixels, w->stride, width, height,
                        QL_COLOR_ARGB8888, wayland_flush) != QL_OK) {
        ql_mem_free(w->pixels);
        wl_display_disconnect(w->dpy);
        ql_mem_free(w);
        return NULL;
    }
    w->base.user_data = w;
    w->base.flush_done = wayland_flush_done;
    w->pending = -1;

    w->surface = wl_compositor_create_surface(w->compositor);
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
    xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);

    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, w->title);
    xdg_toplevel_set_app_id(w->toplevel, "qlgl");

    wl_surface_commit(w->surface);

    for (int i = 0; i < QL_WL_BUFFERS; i++) {
        if (!create_buffer(w, &w->buffers[i], WL_SHM_FORMAT_XRGB8888)) {
            QL_LOGE("wayland: cannot create shm buffer %d", i);
            goto fail;
        }
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

    if (w->keyboard != NULL)
        wl_keyboard_release(w->keyboard);
    if (w->seat != NULL)
        wl_seat_release(w->seat);
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

uint32_t ql_disp_wayland_commit_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->commit_count : 0;
}

uint32_t ql_disp_wayland_skip_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->skip_count : 0;
}

#endif /* QLGL_ENABLE_WAYLAND */
