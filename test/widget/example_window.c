/**
 * @file    example_window.c
 * @brief   在 Linux 桌面上开一个真实窗口，演示动画 + 增量重绘。
 *
 * 运行：
 *   ./build/example_window                    自动选择后端（优先 Wayland，其次 X11）
 *   ./build/example_window --backend=wayland  强制 Wayland
 *   ./build/example_window --backend=x11      强制 X11
 *   ./build/example_window --frames 120       跑满 120 帧后自动退出
 *   ./build/example_window --headless         改用内存显示并输出 PPM
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -std=c11 下没有 POSIX 声明；本示例只在 Linux 上跑，自行声明即可 */
#if defined(__linux__)
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

#include <qlgl/qlgl.h>

#define WIN_W   360
#define WIN_H   240
#define FRAME_US (1000000 / 60)

/* ------------------------- 自定义控件：实心圆 ------------------------- */
QL_PROPERTY_ENUM_EMPTY(QL_DOT, QLDot, QL_WIDGET, 10)
QL_EVENT_ENUM_EMPTY(QL_DOT, QLDot, QL_WIDGET, 10)

QL_DECLARE(QLDot, ql_dot)

struct QLDot        { QLWidget parent; };
struct QLDotClass   { QLWidgetClass parent; };
struct QLDotPrivate { ql_color_t color; };

QL_DEFINE_WITH_PRIVATE(QL_DOT, QLDot, ql_dot, ql_widget)

/* 用逐行扫描线画圆，避免引入额外依赖 */
static void ql_dot_draw(QLWidget *self, QLCanvas *canvas)
{
    struct QLDotPrivate *priv = ql_dot_get_private((QLDot *)(void *)self);
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    ql_coord_t rad = (r.w < r.h ? r.w : r.h) / 2;
    ql_coord_t cx = r.x + r.w / 2;
    ql_coord_t cy = r.y + r.h / 2;

    for (ql_coord_t dy = -rad; dy <= rad; dy++) {
        ql_coord_t dx = 0;
        /* dx = sqrt(rad^2 - dy^2)，用整数逼近 */
        ql_coord_t limit = rad * rad - dy * dy;
        while ((dx + 1) * (dx + 1) <= limit)
            dx++;
        ql_canvas_draw_hline(canvas, cx - dx, cx + dx, cy + dy, 1, priv->color);
    }
}

static void ql_dot_class_init(QLDotClass *klass)
{
    ql_widget_class_set_draw(((QLObjectClass *)(void *)klass)->type, ql_dot_draw);
}

static void ql_dot_init(QLDot *self)
{
    struct QLDotPrivate *priv = ql_dot_get_private(self);
    priv->color = QL_COLOR_SKY;
}

static void ql_dot_deinit(QLDot *self) { QL_UNUSED(self); }

/* ------------------------- 主程序 ------------------------- */
#if defined(__linux__)
static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}
#else
static void sleep_us(long us) { QL_UNUSED(us); }
#endif

int main(int argc, char **argv)
{
    long max_frames = 0;          /* 0 = 不限 */
    int headless = 0;
    const char *backend = NULL;   /* NULL = 自动 */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strncmp(argv[i], "--backend=", 10) == 0)
            backend = argv[i] + 10;
        else if (strcmp(argv[i], "--headless") == 0)
            headless = 1;
    }
#if !defined(QLGL_ENABLE_X11)
    QL_UNUSED(headless);      /* 没编译窗口后端时该开关无意义 */
#endif

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }

    QLDispDriver *disp = NULL;
    enum { BACKEND_NONE, BACKEND_WAYLAND, BACKEND_X11 } kind = BACKEND_NONE;

#if defined(QLGL_ENABLE_WAYLAND)
    if (!headless && (backend == NULL || strcmp(backend, "wayland") == 0)) {
        disp = ql_disp_wayland_create("QLGL Demo", WIN_W, WIN_H, QL_COLOR_FORMAT_AUTO);
        if (disp != NULL)
            kind = BACKEND_WAYLAND;
        else if (backend != NULL)
            printf("Wayland 后端不可用\n");
    }
#endif
#if defined(QLGL_ENABLE_X11)
    if (!headless && disp == NULL && (backend == NULL || strcmp(backend, "x11") == 0)) {
        disp = ql_disp_x11_create("QLGL Demo", WIN_W, WIN_H, QL_COLOR_FORMAT_AUTO);
        if (disp != NULL)
            kind = BACKEND_X11;
        else if (backend != NULL)
            printf("X11 后端不可用\n");
    }
#endif
    if (backend != NULL && disp == NULL && !headless)
        printf("指定的后端 %s 不可用，改用内存显示\n", backend);

    int using_window = (kind != BACKEND_NONE);
    int using_x11 = (kind == BACKEND_X11);
    QL_UNUSED(using_x11);

    if (disp == NULL) {
        printf("未使用窗口后端，改用内存显示\n");
        disp = ql_disp_mem_create(WIN_W, WIN_H, QL_COLOR_RGB888);
        if (disp == NULL) {
            printf("创建显示失败\n");
            return 1;
        }
        if (max_frames == 0)
            max_frames = 1;       /* 无窗口时画一帧就退出 */
    }

    printf("显示：%s，%dx%d，格式 %s\n",
           kind == BACKEND_WAYLAND ? "Wayland 窗口"
                                   : (kind == BACKEND_X11 ? "X11 窗口" : "内存"),
           (int)disp->width, (int)disp->height, ql_disp_format_name(disp->format));

    ql_disp_set_bg_color(QL_COLOR_RGB(0x10, 0x14, 0x1C));

    /* ---- 界面 ---- */
    QLWidget *root = ql_widget_new_ex(0, 0, WIN_W, WIN_H);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x10, 0x14, 0x1C), 255);
    ql_render_set_root((QLObject *)root);

    QLWidget *card = ql_widget_new_child((QLObject *)root, 20, 20, WIN_W - 40, WIN_H - 40);
    ql_widget_set_bg((QLObject *)card, QL_COLOR_RGB(0x1E, 0x25, 0x33), 255);
    ql_widget_set_border((QLObject *)card, 2, QL_COLOR_RGB(0x3A, 0x48, 0x60), 14);
    ql_object_set_property((QLObject *)card, QL_WIDGET_PROPERTY_CLIP_CHILDREN,
                           ql_value_bool(1));

    QLWidget *bar = ql_widget_new_child((QLObject *)card, 16, 16, WIN_W - 72, 26);
    ql_widget_set_bg((QLObject *)bar, QL_COLOR_SKY, 255);
    ql_widget_set_border((QLObject *)bar, 0, QL_COLOR_SKY, 6);

    QLWidget *pill = ql_widget_new_child((QLObject *)card, 16, 56, 120, 30);
    ql_widget_set_bg((QLObject *)pill, QL_COLOR_RGB(0x3C, 0xC0, 0x7A), 255);
    ql_widget_set_border((QLObject *)pill, 0, QL_COLOR_WHITE, 15);

    QLWidget *ghost = ql_widget_new_child((QLObject *)card, 150, 56, 120, 30);
    ql_widget_set_bg((QLObject *)ghost, QL_COLOR_WHITE, 36);
    ql_widget_set_border((QLObject *)ghost, 1, QL_COLOR_GRAY, 15);

    /* 会动的小球 */
    QLType dot_type = QL_REGISTER(ql_dot, QL_WIDGET_TYPE);
    if (dot_type == QL_TYPE_NONE) {
        printf("注册 QLDot 失败\n");
        return 1;
    }
    QLDot *dot = (QLDot *)ql_object_new(dot_type);
    const ql_coord_t dot_size = 34;
    ql_object_set_property((QLObject *)dot, QL_WIDGET_PROPERTY_WIDTH,
                           ql_value_int(dot_size));
    ql_object_set_property((QLObject *)dot, QL_WIDGET_PROPERTY_HEIGHT,
                           ql_value_int(dot_size));
    ql_object_set_property((QLObject *)dot, QL_WIDGET_PROPERTY_Y, ql_value_int(110));
    ql_object_add_child((QLObject *)card, (QLObject *)dot);

    /* ---- 文本：标题、按钮文字、状态栏 ---- */
    QLLabel *title = ql_label_new_child((QLObject *)bar, "QLGL Demo", 10, 3);
    ql_label_set_font((QLObject *)title, &ql_font_mono_16);
    ql_label_set_text_color((QLObject *)title, QL_COLOR_RGB(0x0C, 0x1A, 0x2A));

    QLLabel *primary_label = ql_label_new_child((QLObject *)pill, "Primary", 12, 6);
    ql_label_set_text_color((QLObject *)primary_label, QL_COLOR_BLACK);

    QLLabel *ghost_label = ql_label_new_child((QLObject *)ghost, "Secondary", 12, 6);
    ql_label_set_text_color((QLObject *)ghost_label, QL_COLOR_RGB(0xD0, 0xD8, 0xE4));

    QLLabel *status = ql_label_new_child((QLObject *)card, "frame 0", 16, 186);
    ql_label_set_text_color((QLObject *)status, QL_COLOR_RGB(0x8A, 0x98, 0xAC));

    /* 进度条底/前：演示纯矩形动画 */
    QLWidget *track = ql_widget_new_child((QLObject *)card, 16, 160, WIN_W - 72, 12);
    ql_widget_set_bg((QLObject *)track, QL_COLOR_RGB(0x2E, 0x38, 0x4A), 255);
    ql_widget_set_border((QLObject *)track, 0, QL_COLOR_GRAY, 6);
    QLWidget *fill = ql_widget_new_child((QLObject *)track, 0, 0, 1, 12);
    ql_widget_set_bg((QLObject *)fill, QL_COLOR_RGB(0xF0, 0xB0, 0x30), 255);
    ql_widget_set_border((QLObject *)fill, 0, QL_COLOR_WHITE, 6);

    /* ---- 主循环 ---- */
    long frame = 0;
    int running = 1;

    while (running) {
        /* 1. 处理窗口事件 */
#if defined(QLGL_ENABLE_X11)
        if (using_x11) {
            running = ql_disp_x11_poll(disp);
            if (!running)
                break;
        }
#endif
#if defined(QLGL_ENABLE_WAYLAND)
        if (kind == BACKEND_WAYLAND) {
            running = ql_disp_wayland_poll(disp);
            if (!running)
                break;
        }
#endif

        /* 2. 更新动画：小球左右移动，进度条循环 */
        long period = 120;
        long t = frame % period;
        long half = period / 2;
        long pos = (t < half) ? t : (period - t);        /* 0..60 往复 */
        ql_coord_t travel = (WIN_W - 72) - 16 - dot_size;
        ql_coord_t dx = (ql_coord_t)(pos * travel / half);
        ql_object_set_property((QLObject *)dot, QL_WIDGET_PROPERTY_X, ql_value_int(dx));

        ql_coord_t track_w = WIN_W - 72;
        ql_coord_t fill_w = (ql_coord_t)((frame % 60) * track_w / 59);
        if (fill_w < 1)
            fill_w = 1;
        ql_object_set_property((QLObject *)fill, QL_WIDGET_PROPERTY_WIDTH,
                               ql_value_int(fill_w));

        /* 状态文本每 10 帧刷新一次：文本会被 QLStr 复制，演示自有内存 */
        if (frame % 10 == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "frame %ld", frame);
            ql_label_set_text((QLObject *)status, buf);
        }

        /* 3. 渲染（只有脏区会被重画） */
        int drawn = ql_render();
        if (drawn < 0) {
            printf("ql_render 失败：%d\n", drawn);
            break;
        }
        frame++;

        if (max_frames > 0 && frame >= max_frames)
            break;

        if (using_window)
            sleep_us(FRAME_US);
    }

    printf("共 %ld 帧；窗口%s\n", frame, using_window ? "已关闭" : "未使用");

#if defined(QLGL_ENABLE_WAYLAND)
    if (kind == BACKEND_WAYLAND)
        printf("commit %u 次，因缓冲区忙跳过 %u 帧\n",
               ql_disp_wayland_commit_count(disp),
               ql_disp_wayland_skip_count(disp));
#endif

    if (!using_window) {
        /* 无窗口时把结果存成图片，便于人工检查 */
        if (ql_disp_mem_save_ppm(disp, "/tmp/qlgl_window_headless.ppm") == QL_OK)
            printf("已写出 /tmp/qlgl_window_headless.ppm\n");
    }

    ql_object_unref((QLObject *)root);

#if defined(QLGL_ENABLE_WAYLAND)
    if (kind == BACKEND_WAYLAND)
        ql_disp_wayland_destroy(disp);
    else
#endif
#if defined(QLGL_ENABLE_X11)
    if (kind == BACKEND_X11)
        ql_disp_x11_destroy(disp);
    else
#endif
        ql_disp_mem_destroy(disp);

    QLMemStats stats;
    ql_mem_stats(&stats);
    printf("内存：分配 %zu / 释放 %zu / 存活 %zu 字节\n",
           stats.alloc_count, stats.free_count, stats.bytes_live);

    qlgl_shutdown();
    return stats.bytes_live == 0 ? 0 : 1;
}
