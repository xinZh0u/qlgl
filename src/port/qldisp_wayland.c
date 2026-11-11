/**
 * @file    qldisp_wayland.c
 * @brief   Linux Wayland 显示后端（纯 CPU 光栅）：xdg-shell + wl_shm 双缓冲。
 *
 * 窗口这一层（xdg-shell / 输入 / 高 DPI / 帧回调 / 剪贴板）都在
 * `qlwindow_wayland.h` 里 —— **GPU 后端用的是同一份**，语义不会分叉。
 * 这里只负责：
 *
 *   - 一块持久帧缓冲（渲染器直接写）；
 *   - 两个 shm 缓冲区轮换：`flush()` 把脏区拷进当前缓冲区，
 *     `flush_done()` 里 damage + attach + commit 一次；
 *   - 缩放变化时按 `css × scale` 重建，并用 `resize_pending` 通知调用方。
 */
/* memfd_create / ftruncate 需要 _GNU_SOURCE；只在平台后端里开，不影响主库 */
#define _GNU_SOURCE 1

#include <qlgl/qlgl.h>

#ifdef QLGL_ENABLE_WAYLAND

#include <sys/mman.h>

#include "qlwindow_wayland.h"

#define QL_WL_BUFFERS 2

typedef struct {
    struct wl_buffer *buffer;
    uint8_t          *data;
    size_t            size;
    int               busy;
} QLWaylandBuffer;

typedef struct {
    QLDispDriver base;
    QLWlWindow   wl;              /* xdg-shell / 输入 / 缩放 / 帧回调 / 剪贴板 */

    QLWaylandBuffer buffers[QL_WL_BUFFERS];
    int             current;      /* 下次 flush 用哪个 */
    uint8_t        *pixels;       /* 渲染器用的持久帧缓冲 */
    uint32_t        stride;

    int             pending;      /* 本帧已选中的缓冲区下标，-1 = 未选 */
    QLRect          damage;
    int             has_damage;

    ql_coord_t      css_w;
    ql_coord_t      css_h;

    uint32_t        commit_count;
    uint32_t        skip_count;
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

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static int create_buffer(QLDispWayland *w, QLWaylandBuffer *slot)
{
    const size_t size = (size_t)w->stride * (size_t)w->base.height;

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

    struct wl_shm_pool *pool = wl_shm_create_pool(w->wl.shm, fd, (int32_t)size);
    slot->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w->base.width,
                                             (int32_t)w->base.height,
                                             (int32_t)w->stride,
                                             WL_SHM_FORMAT_XRGB8888);
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
 * 按当前缩放重建帧缓冲 + shm 缓冲区。
 *
 * `set_buffer_scale(N)` 只负责把逻辑尺寸缩回去，**缓冲区本身必须是 N 倍物理
 * 像素**，所以缩放一变整套都要换（画布由调用方按 resize_pending 重建）。
 */
static int buffers_rebuild(QLDispWayland *w)
{
    const ql_coord_t scale = (w->wl.scale > 0) ? w->wl.scale : 1;
    const ql_coord_t pw = w->css_w * scale;
    const ql_coord_t ph = w->css_h * scale;
    if (pw <= 0 || ph <= 0)
        return 0;

    buffers_release(w);

    const uint32_t stride = (uint32_t)pw * 4u;
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
    ql_disp_set_scale(&w->base, (ql_coord_t)scale * QL_SCALE_ONE);

    if (w->wl.surface != NULL)
        wl_surface_set_buffer_scale(w->wl.surface, w->wl.scale);

    for (int i = 0; i < QL_WL_BUFFERS; i++)
        if (!create_buffer(w, &w->buffers[i])) {
            QL_LOGE("wayland: cannot create shm buffer %d (scale %d)", i,
                    (int)w->wl.scale);
            return 0;
        }
    w->current = 0;
    return 1;
}

/* ------------------------------ flush / commit ------------------------------ */

/*
 * 一帧内的脏区先全部拷进同一个 shm 缓冲区，最后在 flush_done 里
 * attach + damage + commit 一次，避免每块脏区都提交一次。
 */
static int wayland_flush(QLDispDriver *self, QLRect area)
{
    QLDispWayland *w = (QLDispWayland *)self;
    if (w->wl.closed || w->wl.dpy == NULL || ql_rect_is_empty(area))
        return 0;

    if (!w->wl.configured) {
        (void)wl_display_dispatch_pending(w->wl.dpy);
        if (!w->wl.configured)
            return 0;
    }

    if (w->pending < 0) {
        for (int i = 0; i < QL_WL_BUFFERS; i++) {
            const int idx = (w->current + i) % QL_WL_BUFFERS;
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
    for (ql_coord_t y = area.y; y < ql_rect_bottom(area); y++)
        memcpy(slot->data + (size_t)y * w->stride + (size_t)area.x * 4u,
               w->pixels + (size_t)y * w->stride + (size_t)area.x * 4u,
               (size_t)area.w * 4u);

    w->damage = w->has_damage ? ql_rect_union(w->damage, area) : area;
    w->has_damage = 1;
    return 0;
}

/* 把攒下的脏区提交出去（并请求下一次帧回调） */
static void wayland_commit(QLDispWayland *w)
{
    if (w->pending < 0 || !w->has_damage || w->wl.closed || w->wl.surface == NULL)
        return;

    /* 上一帧还没被呈现：脏区继续攒着，等回调（帧率节流） */
    if (!qlwl_should_commit(&w->wl)) {
        w->skip_count++;
        return;
    }

    QLWaylandBuffer *slot = &w->buffers[w->pending];
    wl_surface_damage_buffer(w->wl.surface, w->damage.x, w->damage.y, w->damage.w,
                             w->damage.h);
    wl_surface_attach(w->wl.surface, slot->buffer, 0, 0);
    qlwl_request_frame(&w->wl);
    wl_surface_commit(w->wl.surface);

    if (wl_display_flush(w->wl.dpy) < 0 && errno != EAGAIN)
        QL_LOGW("wayland: flush failed (%s)", strerror(errno));

    w->current = (w->pending + 1) % QL_WL_BUFFERS;
    w->pending = -1;
    w->has_damage = 0;
    w->commit_count++;
}

static int wayland_flush_done(QLDispDriver *self)
{
    QLDispWayland *w = (QLDispWayland *)self;
    wayland_commit(w);
    return 0;
}

/* ------------------------------ 创建 / 销毁 / 轮询 ------------------------------ */

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

    /* 调试用：1x 屏上强制按 N 倍分配缓冲区（验证 2x 路径） */
    int32_t scale_override = 0;
    {
        const char *env = getenv("QLGL_WAYLAND_SCALE");
        if (env != NULL && env[0] != '\0') {
            const long v = strtol(env, NULL, 10);
            if (v >= 1 && v <= 8)
                scale_override = (int32_t)v;
        }
    }

    w->css_w = width;
    w->css_h = height;
    w->pending = -1;

    if (!qlwl_open(&w->wl, title, scale_override) || w->wl.shm == NULL) {
        if (w->wl.dpy != NULL)
            qlwl_close(&w->wl);
        ql_mem_free(w);
        return NULL;
    }

    if (ql_disp_init_fb(&w->base, (void *)(uintptr_t)1, (uint32_t)width * 4u, width,
                        height, QL_COLOR_ARGB8888, wayland_flush) != QL_OK) {
        qlwl_close(&w->wl);
        ql_mem_free(w);
        return NULL;
    }
    w->base.user_data = w;
    w->base.flush_done = wayland_flush_done;

    /* 真正分配帧缓冲 + shm 缓冲区（按当前缩放） */
    if (!buffers_rebuild(w)) {
        qlwl_close(&w->wl);
        ql_mem_free(w->pixels);
        ql_mem_free(w);
        return NULL;
    }

    if (ql_disp_set(&w->base) != QL_OK) {
        buffers_release(w);
        qlwl_close(&w->wl);
        ql_mem_free(w->pixels);
        ql_mem_free(w);
        return NULL;
    }

    QL_LOGI("wayland: window \"%s\" %dx%d ARGB8888 (scale %d)", w->wl.title,
            (int)w->base.width, (int)w->base.height, (int)w->wl.scale);
    return &w->base;
}

int ql_disp_wayland_poll(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    QLDispWayland *w = (QLDispWayland *)driver;

    if (!qlwl_pump(&w->wl))
        return 0;

    /* 缩放变了：缓冲区整套换新，通知调用方重建画布 */
    if (w->wl.resize_pending) {
        w->wl.resize_pending = 0;
        const ql_coord_t old_w = w->base.width;
        const ql_coord_t old_h = w->base.height;
        if (buffers_rebuild(w) &&
            (old_w != w->base.width || old_h != w->base.height))
            w->wl.resize_pending = 1;
    }

    /* 有攒下的脏区、且上一帧已经呈现 → 立刻补一次提交 */
    if (w->has_damage && w->pending >= 0)
        wayland_commit(w);

    return w->wl.closed ? 0 : 1;
}

void ql_disp_wayland_close(QLDispDriver *driver)
{
    if (driver != NULL)
        ((QLDispWayland *)driver)->wl.closed = 1;
}

void ql_disp_wayland_destroy(QLDispDriver *driver)
{
    if (driver == NULL)
        return;

    QLDispWayland *w = (QLDispWayland *)driver;
    ql_disp_unset(driver);
    w->pending = -1;
    buffers_release(w);
    qlwl_close(&w->wl);
    ql_mem_free(w->pixels);
    ql_mem_free(w);
}

const char *ql_disp_wayland_title(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->wl.title : "";
}

int ql_disp_wayland_resize_pending(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    QLDispWayland *w = (QLDispWayland *)driver;
    const int p = w->wl.resize_pending;
    w->wl.resize_pending = 0;
    return p;
}

int ql_disp_wayland_scale(QLDispDriver *driver)
{
    return (driver != NULL) ? (int)((QLDispWayland *)driver)->wl.scale : 1;
}

uint32_t ql_disp_wayland_commit_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->commit_count : 0;
}

uint32_t ql_disp_wayland_skip_count(QLDispDriver *driver)
{
    return (driver != NULL) ? ((QLDispWayland *)driver)->skip_count : 0;
}

void ql_disp_wayland_attach_clipboard(QLDispDriver *driver, QLUIRoot *ui)
{
    if (driver == NULL || ui == NULL)
        return;
    QLDispWayland *w = (QLDispWayland *)driver;
    if (w->wl.data_mgr == NULL) {
        QL_LOGW("wayland: 合成器没有 wl_data_device_manager，剪贴板不可用");
        return;
    }
    qlwl_attach_clipboard(&w->wl, ui);
}

#endif /* QLGL_ENABLE_WAYLAND */
