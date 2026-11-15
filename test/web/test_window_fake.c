/**
 * @file    test_window_fake.c
 * @brief   平台后端 + QLWindow：假平台（内存显示 + 脚本事件）跑通主循环。
 *
 * 验证的是"平台层"本身，不需要 GL/X11：
 *   1. `qlgl_set_platform(&fake)` 后 `ql_window_create()` 用假后端；
 *   2. `ql_window_poll()` 处理输入队列 → 点击回调；
 *   3. `ql_window_dump_ppm()` 走平台 dump，写出非空 PPM；
 *   4. `ql_window_set_ui()` 能换界面；
 *   5. `ql_window_quit()` 后 poll 返回 0。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 100

typedef struct {
    QLDispDriver *disp;
    QLCanvas      canvas;
    QLDraw2D      d2d;
    QLRect        flush;
    int           full;
} FakeWin;

static int s_clicks;

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui; (void)node; (void)user;
    s_clicks++;
}

static QLPlatformWindow *fake_open(const char *title, ql_coord_t w, ql_coord_t h,
                                   int backend)
{
    (void)title; (void)backend;
    FakeWin *f = (FakeWin *)ql_mem_calloc(1, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->disp = ql_disp_mem_create(w, h, QL_COLOR_RGB888);
    if (f->disp == NULL ||
        ql_canvas_from_disp(&f->canvas, f->disp) != QL_OK ||
        ql_draw2d_canvas_init(&f->d2d, &f->canvas) != QL_OK) {
        if (f->disp != NULL)
            ql_disp_mem_destroy(f->disp);
        ql_mem_free(f);
        return NULL;
    }
    return (QLPlatformWindow *)f;
}

static void fake_close(QLPlatformWindow *pw)
{
    FakeWin *f = (FakeWin *)pw;
    if (f == NULL)
        return;
    ql_disp_mem_destroy(f->disp);
    ql_mem_free(f);
}

static int fake_poll(QLPlatformWindow *pw) { (void)pw; return 1; }
static int fake_wait(QLPlatformWindow *pw, int ms) { (void)pw; (void)ms; return 1; }

static void fake_size(QLPlatformWindow *pw, ql_coord_t *w, ql_coord_t *h)
{
    FakeWin *f = (FakeWin *)pw;
    if (w != NULL) *w = f->disp->width;
    if (h != NULL) *h = f->disp->height;
}

static int fake_begin(QLPlatformWindow *pw, ql_color_t bg, const QLRect *dirty, int n)
{
    FakeWin *f = (FakeWin *)pw;
    f->full = 0;
    f->flush = ql_rect_make(0, 0, 0, 0);
    if (n <= 0) {
        ql_canvas_clear(&f->canvas, bg);
        f->full = 1;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        ql_canvas_fill_rect(&f->canvas, dirty[i], bg);
        f->flush = (f->flush.w <= 0) ? dirty[i] : ql_rect_union(f->flush, dirty[i]);
    }
    return 0;
}

static QLDraw2D *fake_draw(QLPlatformWindow *pw) { return &((FakeWin *)pw)->d2d; }
static void fake_end(QLPlatformWindow *pw) { (void)pw; }

static int fake_dump(QLPlatformWindow *pw, QLUIRoot *ui, ql_color_t bg,
                     const char *path)
{
    FakeWin *f = (FakeWin *)pw;
    (void)fake_begin(pw, bg, NULL, 0);
    ql_ui_render(ui, &f->d2d);
    fake_end(pw);
    return ql_disp_mem_save_ppm(f->disp, path) == QL_OK;
}

static const QLPlatform s_fake = {
    .name = "fake-memory",
    .window_open = fake_open,
    .window_close = fake_close,
    .poll = fake_poll,
    .wait = fake_wait,
    .size = fake_size,
    .begin = fake_begin,
    .draw = fake_draw,
    .end = fake_end,
    .dump_ppm = fake_dump,
};

QL_TEST_BEGIN("test_window_fake")

    qlgl_set_platform(&s_fake);

    QLUIDoc *doc = ql_ui_load_html(
        "<div id=root><div id=btn>tap me</div></div>",
        "#root { width: 100%; height: 100%; background-color: #FFFFFF; }"
        "#btn { position: absolute; left: 10px; top: 10px; width: 60px;"
        "       height: 30px; background-color: #0969DA; }");
    QL_CHECK(doc != NULL);
    QLUIRoot *ui = (doc != NULL) ? ql_ui_build(doc) : NULL;
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        qlgl_set_platform(NULL);
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_set_click_cb(ui, on_click, NULL);

    QLWindow *win = ql_window_create("fake", W, H, QL_WINDOW_OFFSCREEN);
    QL_CHECK(win != NULL);
    if (win != NULL) {
        QL_CHECK_EQ(ql_window_bind_ui(win, ui), QL_OK);

        ql_coord_t w = 0, h = 0;
        ql_window_size(win, &w, &h);
        QL_CHECK_EQ((int)w, W);
        QL_CHECK_EQ((int)h, H);

        /* 输入 → 点击：队列里的 down/up 在 poll 里派发到界面层 */
        s_clicks = 0;
        QL_CHECK_EQ(ql_input_push_button(20, 20, QL_BUTTON_BIT_LEFT, 1), QL_OK);
        QL_CHECK_EQ(ql_input_push_button(20, 20, QL_BUTTON_BIT_LEFT, 0), QL_OK);
        QL_CHECK_EQ(ql_window_poll(win), 1);
        QL_CHECK_EQ(s_clicks, 1);

        /* 走平台 dump（不是 GL）：文件必须存在且非空 */
        QL_CHECK_EQ(ql_window_dump_ppm(win, "/tmp/qlgl_fake.ppm"), 1);
        FILE *f = fopen("/tmp/qlgl_fake.ppm", "rb");
        QL_CHECK(f != NULL);
        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            QL_CHECK(ftell(f) > 100);
            fclose(f);
        }

        /* 换界面：新的 root 接管输入，旧的仍由调用方释放 */
        QLUIDoc *doc2 = ql_ui_load_html(
            "<div id=root><div id=b2>第二页</div></div>",
            "#root { width: 100%; height: 100%; background-color: #202020; }");
        QL_CHECK(doc2 != NULL);
        QLUIRoot *ui2 = (doc2 != NULL) ? ql_ui_build(doc2) : NULL;
        QL_CHECK(ui2 != NULL);
        if (ui2 != NULL) {
            QL_CHECK_EQ(ql_window_set_ui(win, ui2), QL_OK);
            QL_CHECK(ql_window_ui(win) == ui2);
            QL_CHECK_EQ(ql_window_poll(win), 1);
            ql_ui_free(ui2);
            ql_ui_doc_free(doc2);
        }

        ql_window_quit(win);
        QL_CHECK_EQ(ql_window_poll(win), 0);
        ql_window_destroy(win);
        remove("/tmp/qlgl_fake.ppm");
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_set_platform(NULL);

QL_TEST_END()
