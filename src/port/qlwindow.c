/**
 * @file    qlwindow.c
 * @brief   QLWindow：窗口 + 主循环 + 输入/IME/剪贴板 + 脏区重绘。
 *
 * 这一层只依赖 `QLPlatform` 回调表（见 qlplatform.h）：
 *   - 桌面默认后端 = X11 / Wayland + GL 或 CPU 光栅；
 *   - MCU 板级文件定义 `qlgl_platform_default()` 强符号即可替换；
 *   - 测试可以 `qlgl_set_platform(&fake)` 用假后端。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>
#include <qlgl/qlwindow.h>

#define EX_FRAME_SAMPLES 4096

struct QLWindow {
    const QLPlatform  *plat;
    QLPlatformWindow  *pw;
    QLUIRoot          *ui;

    ql_window_event_fn event_fn;
    void              *event_user;
    ql_window_frame_fn frame_fn;
    void              *frame_user;

    ql_color_t bg;
    int        bg_set;
    int        bound;
    int        quit;
    int        need_full;
    int        input_attached;
    int        warned_no_draw;

    /* `ql_window_run_args` 的开发参数（性能教程用） */
    int        spin;
    int        want_stats;
    long       max_frames;
    int        run_seconds;
    long       frames;
    struct timespec t_start;
    uint32_t   frame_us[EX_FRAME_SAMPLES];
    uint32_t   swap_us[EX_FRAME_SAMPLES];
    uint32_t   gap_us[EX_FRAME_SAMPLES];
    int        stat_n;
    struct timespec t_prev;
};

/* ------------------------------ 文件工具 ------------------------------ */

void *ql_file_read(const char *path, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (path == NULL)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)ql_mem_alloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len != NULL)
        *out_len = got;
    return buf;
}

char *ql_file_read_text(const char *path)
{
    return (char *)ql_file_read(path, NULL);
}

/* ------------------------------ 内部工具 ------------------------------ */

/* 事件过滤器：先问窗口，没消费再交给界面层 */
static void win_sink(const QLInputEvent *ev, void *user)
{
    QLWindow *win = (QLWindow *)user;
    if (win == NULL)
        return;
    if (win->event_fn != NULL && win->event_fn(win->ui, ev, win->event_user))
        return;
    ql_ui_input_event(win->ui, ev);
}

static ql_color_t window_bg(QLWindow *win)
{
    if (win->bg_set)
        return win->bg;
    if (win->ui != NULL && win->ui->node_count > 0) {
        const QLUIStyle *s = ql_ui_node_style(&win->ui->nodes[0]);
        if (s != NULL && s->bg_alpha > 0)
            return s->bg_color;
    }
    return QL_COLOR_RGB(0xFF, 0xFF, 0xFF);
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void window_stats_sample(QLWindow *win)
{
    if (!win->want_stats || win->stat_n >= EX_FRAME_SAMPLES)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int i = win->stat_n;
    win->gap_us[i] = (i == 0) ? 0u
        : (uint32_t)((now.tv_sec - win->t_prev.tv_sec) * 1000000 +
                     (now.tv_nsec - win->t_prev.tv_nsec) / 1000);
    win->t_prev = now;
    win->frame_us[i] = win->plat->frame_us != NULL ? win->plat->frame_us(win->pw) : 0;
    win->swap_us[i] = win->plat->swap_us != NULL ? win->plat->swap_us(win->pw) : 0;
    win->stat_n++;
}

static void window_stats_print(QLWindow *win)
{
    if (!win->want_stats || win->stat_n <= 0)
        return;
    const int n = win->stat_n;
    qsort(win->frame_us, (size_t)n, sizeof(uint32_t), cmp_u32);
    qsort(win->swap_us, (size_t)n, sizeof(uint32_t), cmp_u32);
    printf("帧时间：p50 %.2fms  p95 %.2fms  max %.2fms  （%d 帧）\n",
           win->frame_us[n / 2] / 1000.0,
           win->frame_us[(int)((double)n * 0.95)] / 1000.0,
           win->frame_us[n - 1] / 1000.0, n);
    if (n > 1) {
        qsort(win->gap_us + 1, (size_t)n - 1, sizeof(uint32_t), cmp_u32);
        const uint32_t gap = win->gap_us[1 + (n - 1) / 2];
        printf("帧间隔：p50 %.2fms（≈ %.0f fps）  max %.2fms\n", gap / 1000.0,
               gap > 0 ? 1000000.0 / (double)gap : 0.0,
               win->gap_us[n - 1] / 1000.0);
    }
    printf("其中等 swap：p50 %.2fms  max %.2fms\n",
           win->swap_us[n / 2] / 1000.0, win->swap_us[n - 1] / 1000.0);
}

/* 画一帧：full=1 整屏，否则只重画 rects */
static void render_frame(QLWindow *win, const QLRect *rects, int n, int full)
{
    if (win == NULL || win->plat == NULL || win->pw == NULL || win->ui == NULL)
        return;

    const QLRect *dirty = full ? NULL : rects;
    const int dn = full ? 0 : n;
    int force = win->plat->begin(win->pw, window_bg(win), dirty, dn);
    QLDraw2D *d2d = win->plat->draw(win->pw);
    if (d2d == NULL) {
        /* 后端有窗口却给不出 draw 接口：这帧什么都不会画（白屏），
         * 只在第一次报错，避免每帧刷屏。 */
        if (!win->warned_no_draw) {
            QL_LOGE("ql_window: platform draw() returned NULL, UI not rendered");
            win->warned_no_draw = 1;
        }
        win->plat->end(win->pw);
        return;
    }
    if (full || force || dn <= 0) {
        ql_ui_render(win->ui, d2d);
    } else {
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(win->ui, d2d, rects[i]);
    }
    win->plat->end(win->pw);
}

/* ------------------------------ 公开 API ------------------------------ */

QLWindow *ql_window_create(const char *title, ql_coord_t width, ql_coord_t height,
                           QLWindowBackend backend)
{
    if (width <= 0 || height <= 0)
        return NULL;
    /* 调试/验收：QLGL_WINDOW_CPU=1 强制走 CPU 光栅（与 GL 对照用） */
    if (backend == QL_WINDOW_GL && getenv("QLGL_WINDOW_CPU") != NULL)
        backend = QL_WINDOW_CPU;
    const QLPlatform *plat = qlgl_platform();
    if (plat == NULL || plat->window_open == NULL) {
        QL_LOGE("ql_window: no platform backend (link a board/desktop platform)");
        return NULL;
    }

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
    /* 输入法（XIM）依赖 locale；桌面窗口创建前设一次，应用不用自己记 */
    setlocale(LC_ALL, "");
#endif

    QLPlatformWindow *pw = plat->window_open(title != NULL ? title : "QLGL",
                                             width, height, (int)backend);
    if (pw == NULL)
        return NULL;

    QLWindow *win = (QLWindow *)ql_mem_calloc(1, sizeof(*win));
    if (win == NULL) {
        plat->window_close(pw);
        return NULL;
    }
    win->plat = plat;
    win->pw = pw;
    win->need_full = 1;
    return win;
}

ql_err_t ql_window_set_ui(QLWindow *win, QLUIRoot *ui)
{
    if (win == NULL || win->pw == NULL || ui == NULL)
        return QL_ERR_INVAL;

    /* 换界面：先把旧 root 从输入汇上摘掉，否则悬空指针 */
    if (win->input_attached) {
        ql_ui_detach_input();
        win->input_attached = 0;
    }

    win->ui = ui;
    win->bound = 1;

    /* 输入：先让界面层装好自己的汇，再用"过滤器 + 界面"的复合 sink 顶上 */
    ql_ui_attach_input(ui);
    win->input_attached = 1;
    ql_input_set_sink(win_sink, win);

    if (win->plat->attach_clipboard != NULL)
        win->plat->attach_clipboard(win->pw, ui);
    if (win->plat->attach_ime != NULL)
        win->plat->attach_ime(win->pw, ui);

    ql_coord_t w = 0, h = 0;
    if (win->plat->size != NULL)
        win->plat->size(win->pw, &w, &h);
    if (w > 0 && h > 0)
        ql_ui_layout(ui, w, h);
    win->need_full = 1;
    return QL_OK;
}

ql_err_t ql_window_bind_ui(QLWindow *win, QLUIRoot *ui)
{
    return ql_window_set_ui(win, ui);
}

int ql_window_poll(QLWindow *win)
{
    if (win == NULL || win->pw == NULL || win->quit)
        return 0;

    if (win->run_seconds > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - win->t_start.tv_sec >= win->run_seconds)
            return 0;
    }
    if (win->plat->poll == NULL || !win->plat->poll(win->pw))
        return 0;

    if (win->ui != NULL) {
        /* 窗口被拖动/缩放：后端只报告"画布变了"，重排是界面层的事 */
        if (win->plat->resize_pending != NULL &&
            win->plat->resize_pending(win->pw)) {
            ql_coord_t w = 0, h = 0;
            if (win->plat->size != NULL)
                win->plat->size(win->pw, &w, &h);
            if (w > 0 && h > 0) {
                ql_ui_layout(win->ui, w, h);
                win->need_full = 1;
            }
        }

        ql_input_dispatch();

        /* 输入法候选窗靠这个位置：每轮更新一次（值没变时后端直接返回） */
        if (win->plat->set_ime_spot != NULL) {
            ql_coord_t sx = 0, sy = 0, sh = 0;
            if (ql_ui_ime_spot(win->ui, &sx, &sy, &sh))
                win->plat->set_ime_spot(win->pw, sx, sy, sh);
        }

        const uint32_t moving = ql_ui_anim_tick(win->ui, ql_anim_now_ms());
        if (win->frame_fn != NULL &&
            win->frame_fn(win->ui, win, win->frame_user))
            win->need_full = 1;

        QLRect areas[16];
        const int n = ql_ui_consume_dirty(win->ui, areas, 16);

        if (win->spin)
            win->need_full = 1;

        /* 没有脏区、没有动画、没改尺寸：阻塞等事件，静止时零占用。
         * `--spin`（性能教程）例外：每帧都画。 */
        if (!win->spin && !win->need_full && n == 0 && moving == 0) {
            if (win->plat->wait != NULL && !win->plat->wait(win->pw, 32))
                return 0;
            return 1;
        }

        render_frame(win, areas, n, win->need_full);
        win->need_full = 0;
        win->frames++;
        window_stats_sample(win);
        if (win->max_frames > 0 && win->frames >= win->max_frames)
            return 0;
    } else {
        if (win->plat->wait != NULL && !win->plat->wait(win->pw, 32))
            return 0;
    }
    return 1;
}

int ql_window_run(QLWindow *win)
{
    if (win == NULL || win->pw == NULL)
        return 1;
    while (ql_window_poll(win)) {
        /* 主循环本体在 poll 里 */
    }
    return 0;
}

int ql_window_run_args(QLWindow *win, int argc, char **argv)
{
    const char *dump = NULL;
    int resized = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (strcmp(argv[i], "--vsync") == 0 && i + 1 < argc)
            ql_window_set_vsync(win, atoi(argv[++i]) != 0);
        else if (strcmp(argv[i], "--spin") == 0)
            win->spin = 1;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            win->max_frames = strtol(argv[++i], NULL, 10);
            win->spin = 1;             /* 跑固定帧数就必须每帧都画 */
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            win->run_seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--stats") == 0)
            win->want_stats = 1;
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int sw = 0, sh = 0;
            if (sscanf(argv[++i], "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) {
                resized = (ql_window_resize(win, sw, sh) == QL_OK);
                if (!resized)
                    QL_LOGW("ql_window: --size 不受当前后端支持（Wayland 由合成器定尺寸）");
            }
        }
    }
    if (dump == NULL || dump[0] == '\0')
        dump = getenv("QL_EX_DUMP");
    if (dump != NULL && dump[0] != '\0') {
        /* `--size` 是异步的（X11 ConfigureNotify）：先泵几帧，让布局跟上 */
        if (resized) {
            for (int i = 0; i < 8; i++)
                if (!ql_window_poll(win))
                    break;
        }
        const int ok = ql_window_dump_ppm(win, dump);
        printf(ok ? "已写出 %s\n" : "写出 %s 失败\n", dump);
        return ok ? 0 : 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &win->t_start);
    win->t_prev = win->t_start;
    win->frames = 0;
    win->stat_n = 0;
    const int rc = ql_window_run(win);
    window_stats_print(win);
    return rc;
}

ql_err_t ql_window_resize(QLWindow *win, ql_coord_t width, ql_coord_t height)
{
    if (win == NULL || win->plat == NULL || width <= 0 || height <= 0)
        return QL_ERR_INVAL;
    if (win->plat->resize == NULL)
        return QL_ERR_UNSUPPORTED;
    const ql_err_t err = win->plat->resize(win->pw, width, height);
    if (err == QL_OK)
        win->need_full = 1;
    return err;
}

void ql_window_quit(QLWindow *win)
{
    if (win == NULL)
        return;
    win->quit = 1;
}

void ql_window_set_vsync(QLWindow *win, int on)
{
    if (win != NULL && win->plat != NULL && win->plat->set_vsync != NULL)
        win->plat->set_vsync(win->pw, on);
}

void ql_window_set_bg(QLWindow *win, ql_color_t color)
{
    if (win != NULL) {
        win->bg = color;
        win->bg_set = 1;
        win->need_full = 1;
    }
}

void ql_window_set_event_filter(QLWindow *win, ql_window_event_fn fn, void *user)
{
    if (win != NULL) {
        win->event_fn = fn;
        win->event_user = user;
    }
}

void ql_window_set_frame_cb(QLWindow *win, ql_window_frame_fn fn, void *user)
{
    if (win != NULL) {
        win->frame_fn = fn;
        win->frame_user = user;
    }
}

void ql_window_size(const QLWindow *win, ql_coord_t *out_w, ql_coord_t *out_h)
{
    ql_coord_t w = 0, h = 0;
    if (win != NULL && win->plat != NULL && win->plat->size != NULL)
        win->plat->size(win->pw, &w, &h);
    if (out_w != NULL)
        *out_w = w;
    if (out_h != NULL)
        *out_h = h;
}

QLUIRoot *ql_window_ui(const QLWindow *win)
{
    return (win != NULL) ? win->ui : NULL;
}

#if defined(QLGL_ENABLE_GL)
QLGfxGL *ql_window_gfx(QLWindow *win)
{
    if (win == NULL || win->plat == NULL || win->plat->native == NULL)
        return NULL;
    return (QLGfxGL *)win->plat->native(win->pw, "gl");
}
#endif

int ql_window_dump_ppm(QLWindow *win, const char *path)
{
    if (win == NULL || win->plat == NULL || win->pw == NULL || win->ui == NULL ||
        path == NULL || win->plat->dump_ppm == NULL)
        return 0;
    return win->plat->dump_ppm(win->pw, win->ui, window_bg(win), path);
}

void ql_window_destroy(QLWindow *win)
{
    if (win == NULL)
        return;
    if (win->input_attached) {
        ql_ui_detach_input();
        win->input_attached = 0;
    }
    if (win->plat != NULL && win->plat->window_close != NULL)
        win->plat->window_close(win->pw);
    ql_mem_free(win);
}
