/**
 * @file    qlwindow.c
 * @brief   QLWindow：把"窗口 + 主循环 + 输入/IME/剪贴板 + 脏区重绘"收进一个对象。
 *
 * 这一层只依赖 `QLGfxGL` 的公开接口，不碰平台细节；CPU 后端后续用同一组
 * 函数实现即可。没有 GL 的构建里整份编译成空壳（创建窗口返回 NULL），
 * 不影响 MCU / 离屏用户链接。
 */
/* clock_gettime（--stats 用）：必须在任何系统头之前定义 */
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

#if defined(QLGL_ENABLE_GL)

#include <qlgl/qlgfx_gl.h>

#define EX_FRAME_SAMPLES 4096

struct QLWindow {
    QLGfxGL      *gfx;
    QLUIRoot     *ui;
    QLWindowBackend backend;

    ql_window_event_fn event_fn;
    void              *event_user;
    ql_window_frame_fn frame_fn;
    void              *frame_user;

    ql_color_t bg;
    int        bg_set;
    int        bound;
    int        shown;
    int        quit;
    int        need_full;
    int        input_attached;

    /* `ql_window_run_args` 的开发参数（性能教程用） */
    int        spin;          /* 每帧都画 */
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

/* `--stats`：记一帧的耗时/间隔（与旧 common.h 的 ex_run 同口径） */
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
    win->frame_us[i] = ql_gfx_gl_frame_us(win->gfx);
    win->swap_us[i] = ql_gfx_gl_swap_us(win->gfx);
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

QLWindow *ql_window_create(const char *title, ql_coord_t width, ql_coord_t height,
                           QLWindowBackend backend)
{
    if (width <= 0 || height <= 0)
        return NULL;
    if (backend == QL_WINDOW_CPU) {
        QL_LOGE("ql_window: CPU 窗口后端尚未实现（用 QL_WINDOW_GL）");
        return NULL;
    }

    /* 输入法（XIM）依赖 locale；桌面窗口创建前设一次，应用不用自己记 */
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
    setlocale(LC_ALL, "");
#endif

    QLWindow *win = (QLWindow *)ql_mem_calloc(1, sizeof(*win));
    if (win == NULL)
        return NULL;
    win->backend = backend;
    win->need_full = 1;

    const uint32_t flags = (backend == QL_WINDOW_OFFSCREEN) ? QL_GFX_GL_OFFSCREEN
                                                           : QL_GFX_GL_WINDOW;
    win->gfx = ql_gfx_gl_create(title != NULL ? title : "QLGL", width, height, flags);
    if (win->gfx == NULL) {
        ql_mem_free(win);
        return NULL;
    }
    /* 保留式画布：动画静止时零重绘，窗口双缓冲下局部重绘才成立 */
    (void)ql_gfx_gl_set_retained(win->gfx, 1);
    return win;
}

ql_err_t ql_window_set_ui(QLWindow *win, QLUIRoot *ui)
{
    if (win == NULL || win->gfx == NULL || ui == NULL)
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

    /* 桌面专属能力：剪贴板与输入法。没编译对应后端时内部是空操作 */
    ql_gfx_gl_attach_clipboard(win->gfx, ui);
    ql_gfx_gl_attach_ime(win->gfx, ui);

    ql_coord_t w = 0, h = 0;
    ql_gfx_gl_size(win->gfx, &w, &h);
    if (w > 0 && h > 0)
        ql_ui_layout(ui, w, h);
    win->need_full = 1;
    return QL_OK;
}

ql_err_t ql_window_bind_ui(QLWindow *win, QLUIRoot *ui)
{
    return ql_window_set_ui(win, ui);
}

void ql_window_show(QLWindow *win)
{
    if (win == NULL)
        return;
    win->shown = 1;
    win->need_full = 1;
}

/* 画一帧：full=1 整屏，否则只重画 rects */
static void render_frame(QLWindow *win, const QLRect *rects, int n, int full)
{
    if (win == NULL || win->gfx == NULL || win->ui == NULL)
        return;

    QLDraw2D *d2d = ql_gfx_gl_draw(win->gfx);
    const int must_full = ql_gfx_gl_begin_rects(win->gfx, window_bg(win), rects, n);
    if (full || must_full || n <= 0) {
        ql_ui_render(win->ui, d2d);
    } else {
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(win->ui, d2d, rects[i]);
    }
    ql_gfx_gl_end(win->gfx);
}

void ql_window_render(QLWindow *win)
{
    if (win == NULL)
        return;
    render_frame(win, NULL, 0, 1);
    win->need_full = 0;
}

int ql_window_poll(QLWindow *win)
{
    if (win == NULL || win->gfx == NULL || win->quit)
        return 0;

    if (win->run_seconds > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - win->t_start.tv_sec >= win->run_seconds)
            return 0;
    }
    if (!ql_gfx_gl_poll(win->gfx))
        return 0;
    win->shown = 1;

    if (win->ui != NULL) {
        /* 窗口被拖动/缩放：后端只报告"画布变了"，重排是界面层的事 */
        if (ql_gfx_gl_resize_pending(win->gfx)) {
            ql_coord_t w = 0, h = 0;
            ql_gfx_gl_size(win->gfx, &w, &h);
            if (w > 0 && h > 0) {
                ql_ui_layout(win->ui, w, h);
                win->need_full = 1;
            }
        }

        ql_input_dispatch();

        /* 输入法候选窗靠这个位置：每轮更新一次（值没变时后端直接返回） */
        {
            ql_coord_t sx = 0, sy = 0, sh = 0;
            if (ql_ui_ime_spot(win->ui, &sx, &sy, &sh))
                ql_gfx_gl_set_ime_spot(win->gfx, sx, sy, sh);
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
            if (!ql_gfx_gl_wait(win->gfx, 32))
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
        /* 没绑界面：只维持窗口事件（离屏/预热阶段） */
        if (!ql_gfx_gl_wait(win->gfx, 32))
            return 0;
    }
    return 1;
}

int ql_window_run(QLWindow *win)
{
    if (win == NULL || win->gfx == NULL)
        return 1;
    win->shown = 1;
    while (ql_window_poll(win)) {
        /* 主循环本体在 poll 里；这里留空给未来的"每帧外部逻辑"钩子 */
    }
    return 0;
}

int ql_window_run_args(QLWindow *win, int argc, char **argv)
{
    const char *dump = NULL;
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
    }
    if (dump == NULL || dump[0] == '\0')
        dump = getenv("QL_EX_DUMP");
    if (dump != NULL && dump[0] != '\0') {
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

#if defined(QLGL_ENABLE_GL)
QLGfxGL *ql_window_gfx(QLWindow *win)
{
    return (win != NULL) ? win->gfx : NULL;
}
#endif

void ql_window_quit(QLWindow *win)
{
    if (win == NULL)
        return;
    win->quit = 1;
    if (win->gfx != NULL)
        ql_gfx_gl_close(win->gfx);
}

void ql_window_set_vsync(QLWindow *win, int on)
{
    if (win != NULL && win->gfx != NULL)
        ql_gfx_gl_set_vsync(win->gfx, on);
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
    if (win != NULL && win->gfx != NULL)
        ql_gfx_gl_size(win->gfx, &w, &h);
    if (out_w != NULL)
        *out_w = w;
    if (out_h != NULL)
        *out_h = h;
}

QLUIRoot *ql_window_ui(const QLWindow *win)
{
    return (win != NULL) ? win->ui : NULL;
}

int ql_window_dump_ppm(QLWindow *win, const char *path)
{
    if (win == NULL || win->gfx == NULL || win->ui == NULL || path == NULL)
        return 0;

    /* 画一帧但不交换，然后从后缓冲读回（顺序同 QL_EX_WINDOW_DUMP） */
    QLDraw2D *d2d = ql_gfx_gl_draw(win->gfx);
    (void)ql_gfx_gl_begin_rects(win->gfx, window_bg(win), NULL, 0);
    ql_ui_render(win->ui, d2d);
    ql_gfx_gl_flush_frame(win->gfx);
    ql_gfx_gl_finish(win->gfx);

    ql_coord_t w = 0, h = 0;
    ql_gfx_gl_device_size(win->gfx, &w, &h);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) {
        ql_gfx_gl_end(win->gfx);
        return 0;
    }

    const size_t stride = (size_t)w * 4u;
    uint8_t *px = (uint8_t *)ql_mem_alloc(stride * (size_t)h);
    if (px == NULL) {
        ql_gfx_gl_end(win->gfx);
        return 0;
    }
    const int ok = (ql_gfx_gl_read_pixels(win->gfx, px, (uint32_t)stride) == QL_OK);
    ql_gfx_gl_end(win->gfx);

    int wrote = 0;
    if (ok) {
        FILE *fp = fopen(path, "wb");
        if (fp != NULL) {
            int good = (fprintf(fp, "P6\n%d %d\n255\n", (int)w, (int)h) > 0);
            for (ql_coord_t y = 0; good && y < h; y++) {
                const uint8_t *q = px + (size_t)y * stride;
                for (ql_coord_t x = 0; x < w; x++, q += 4) {
                    if (fwrite(q, 1, 3, fp) != 3) {
                        good = 0;
                        break;
                    }
                }
            }
            if (fclose(fp) != 0)
                good = 0;
            wrote = good;
        }
    }
    ql_mem_free(px);
    return wrote;
}

void ql_window_destroy(QLWindow *win)
{
    if (win == NULL)
        return;
    if (win->input_attached) {
        ql_ui_detach_input();
        win->input_attached = 0;
    }
    if (win->gfx != NULL)
        ql_gfx_gl_destroy(win->gfx);
    ql_mem_free(win);
}

#else /* !QLGL_ENABLE_GL */

/* 没有 GL 后端：窗口 API 仍然存在，但创建一定失败（MCU 用 ql_disp_*） */
struct QLWindow {
    int unused;
};

QLWindow *ql_window_create(const char *title, ql_coord_t width, ql_coord_t height,
                           QLWindowBackend backend)
{
    (void)title;
    (void)width;
    (void)height;
    (void)backend;
    QL_LOGE("ql_window: this build has no GL backend");
    return NULL;
}

ql_err_t ql_window_bind_ui(QLWindow *win, QLUIRoot *ui)
{
    (void)win;
    (void)ui;
    return QL_ERR_UNSUPPORTED;
}
void ql_window_show(QLWindow *win) { (void)win; }
int  ql_window_run(QLWindow *win) { (void)win; return 1; }
int  ql_window_run_args(QLWindow *win, int argc, char **argv)
{
    (void)win; (void)argc; (void)argv;
    return 1;
}
ql_err_t ql_window_set_ui(QLWindow *win, QLUIRoot *ui)
{
    (void)win; (void)ui;
    return QL_ERR_UNSUPPORTED;
}
int  ql_window_poll(QLWindow *win) { (void)win; return 0; }
void ql_window_render(QLWindow *win) { (void)win; }
void ql_window_quit(QLWindow *win) { (void)win; }
void ql_window_destroy(QLWindow *win) { (void)win; }
void ql_window_set_vsync(QLWindow *win, int on) { (void)win; (void)on; }
void ql_window_set_bg(QLWindow *win, ql_color_t color) { (void)win; (void)color; }
void ql_window_set_event_filter(QLWindow *win, ql_window_event_fn fn, void *user)
{
    (void)win; (void)fn; (void)user;
}
void ql_window_set_frame_cb(QLWindow *win, ql_window_frame_fn fn, void *user)
{
    (void)win; (void)fn; (void)user;
}
void ql_window_size(const QLWindow *win, ql_coord_t *out_w, ql_coord_t *out_h)
{
    (void)win;
    if (out_w != NULL) *out_w = 0;
    if (out_h != NULL) *out_h = 0;
}
QLUIRoot *ql_window_ui(const QLWindow *win) { (void)win; return NULL; }
int ql_window_dump_ppm(QLWindow *win, const char *path)
{
    (void)win; (void)path;
    return 0;
}

#endif /* QLGL_ENABLE_GL */
