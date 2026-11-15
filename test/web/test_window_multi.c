/**
 * @file    test_window_multi.c
 * @brief   多窗口：每窗口一条输入队列，事件只进自己那一个界面。
 *
 * 假的平台实现 `set_input(pw, in)`，把每个窗口的 `QLInput` 记下来；
 * 测试直接往"窗口 0/1 的队列"里塞事件，断言：
 *
 *   1. 每个窗口拿到的是**不同**的 QLInput（不是进程默认实例）；
 *   2. 往窗口 0 塞事件、先去 poll 窗口 1 —— 窗口 1 不能消费它；
 *   3. 两个界面的点击回调互不串；
 *   4. 窗口 1 销毁后，窗口 0 仍然正常工作；
 *   5. `ql_window_set_ui()` 只影响自己那个窗口。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 160
#define H 120
#define MAX_WIN 4

typedef struct {
    QLDispDriver *disp;
    QLCanvas      canvas;
    QLDraw2D      d2d;
    QLInput      *in;
    int           idx;
} FakeWin;

static FakeWin *s_pw[MAX_WIN];
static QLInput *s_in[MAX_WIN];
static int      s_open_count;

/* ------------------------------ 假平台 ------------------------------ */

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
    f->idx = s_open_count++;
    if (s_open_count > MAX_WIN)
        s_open_count = MAX_WIN;
    s_pw[f->idx] = f;
    return (QLPlatformWindow *)f;
}

static void fake_close(QLPlatformWindow *pw)
{
    FakeWin *f = (FakeWin *)pw;
    if (f == NULL)
        return;
    if (f->idx >= 0 && f->idx < MAX_WIN) {
        s_pw[f->idx] = NULL;
        s_in[f->idx] = NULL;
    }
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
    if (n <= 0) {
        ql_canvas_clear(&f->canvas, bg);
        return 0;
    }
    for (int i = 0; i < n; i++)
        ql_canvas_fill_rect(&f->canvas, dirty[i], bg);
    return 0;
}

static QLDraw2D *fake_draw(QLPlatformWindow *pw) { return &((FakeWin *)pw)->d2d; }
static void fake_end(QLPlatformWindow *pw) { (void)pw; }

/* 平台支持多窗口路由：把 QLWindow 建好的实例记下来，测试直接往里塞事件 */
static void fake_set_input(QLPlatformWindow *pw, QLInput *in)
{
    FakeWin *f = (FakeWin *)pw;
    if (f == NULL)
        return;
    f->in = in;
    if (f->idx >= 0 && f->idx < MAX_WIN)
        s_in[f->idx] = in;
}

static const QLPlatform s_fake = {
    .name = "fake-multi",
    .window_open = fake_open,
    .window_close = fake_close,
    .poll = fake_poll,
    .wait = fake_wait,
    .size = fake_size,
    .set_input = fake_set_input,
    .begin = fake_begin,
    .draw = fake_draw,
    .end = fake_end,
};

/* ------------------------------ 断言素材 ------------------------------ */

static int s_clicks[2];
static int s_events[2];

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui; (void)node;
    s_clicks[(int)(intptr_t)user]++;
}

static int on_event(QLUIRoot *ui, const QLInputEvent *ev, void *user)
{
    (void)ui; (void)ev;
    s_events[(int)(intptr_t)user]++;
    return 0;                       /* 不消费：界面层还要处理 */
}

static QLUIRoot *make_ui(const char *css_color, QLUIDoc **out_doc)
{
    char css[256];
    snprintf(css, sizeof(css),
             "#root { width: 100%%; height: 100%%; background-color: #FFFFFF; }"
             "#btn { position: absolute; left: 10px; top: 10px; width: 60px;"
             " height: 30px; background-color: %s; }", css_color);
    QLUIDoc *doc = ql_ui_load_html(
        "<div id=root><div id=btn>tap</div></div>", css);
    if (doc == NULL)
        return NULL;
    QLUIRoot *ui = ql_ui_build(doc);
    /* root 不拥有 doc：doc 必须活到 ui 释放之后 */
    *out_doc = doc;
    return ui;
}

QL_TEST_BEGIN("test_window_multi")

    qlgl_set_platform(&s_fake);
    s_open_count = 0;
    memset(s_pw, 0, sizeof(s_pw));
    memset(s_in, 0, sizeof(s_in));
    memset(s_clicks, 0, sizeof(s_clicks));
    memset(s_events, 0, sizeof(s_events));

    QLUIDoc *doc0 = NULL, *doc1 = NULL, *doc2 = NULL;
    QLUIRoot *ui0 = make_ui("#0969DA", &doc0);
    QLUIRoot *ui1 = make_ui("#2DA44E", &doc1);
    QL_CHECK(ui0 != NULL && ui1 != NULL);
    if (ui0 == NULL || ui1 == NULL) {
        ql_ui_free(ui0);
        ql_ui_free(ui1);
        qlgl_set_platform(NULL);
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_set_click_cb(ui0, on_click, (void *)(intptr_t)0);
    ql_ui_set_click_cb(ui1, on_click, (void *)(intptr_t)1);

    QLWindow *w0 = ql_window_create("win0", W, H, QL_WINDOW_OFFSCREEN);
    QLWindow *w1 = ql_window_create("win1", W, H, QL_WINDOW_OFFSCREEN);
    QL_CHECK(w0 != NULL && w1 != NULL);
    if (w0 == NULL || w1 == NULL) {
        ql_window_destroy(w0);
        ql_window_destroy(w1);
        ql_ui_free(ui0);
        ql_ui_free(ui1);
        qlgl_set_platform(NULL);
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    /* 1. 每窗口一个独立实例，且不是进程默认队列 */
    QL_CHECK(s_in[0] != NULL && s_in[1] != NULL);
    QL_CHECK(s_in[0] != s_in[1]);
    QL_CHECK(s_in[0] != ql_input_default());
    QL_CHECK(s_in[1] != ql_input_default());
    QL_CHECK(s_pw[0] != NULL && s_pw[1] != NULL);

    QL_CHECK_EQ(ql_window_bind_ui(w0, ui0), QL_OK);
    QL_CHECK_EQ(ql_window_bind_ui(w1, ui1), QL_OK);
    ql_window_set_event_filter(w0, on_event, (void *)(intptr_t)0);
    ql_window_set_event_filter(w1, on_event, (void *)(intptr_t)1);

    /* 2. 往窗口 0 塞点击，但先 poll 窗口 1：它不能消费这条事件 */
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[0], 20, 20, QL_BUTTON_LEFT, 1), QL_OK);
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[0], 20, 20, QL_BUTTON_LEFT, 0), QL_OK);
    QL_CHECK_EQ(ql_window_poll(w1), 1);
    QL_CHECK_EQ(s_clicks[0], 0);
    QL_CHECK_EQ(s_clicks[1], 0);
    QL_CHECK_EQ(s_events[1], 0);
    QL_CHECK_EQ(ql_input_pending_ex(s_in[0]), 2u);   /* 还在窗口 0 的队列里 */

    QL_CHECK_EQ(ql_window_poll(w0), 1);
    QL_CHECK_EQ(s_clicks[0], 1);
    QL_CHECK_EQ(s_clicks[1], 0);
    QL_CHECK_EQ(s_events[0], 2);
    QL_CHECK_EQ(s_events[1], 0);
    QL_CHECK_EQ(ql_input_pending_ex(s_in[0]), 0u);

    /* 3. 反过来：事件进窗口 1，窗口 0 不受影响 */
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[1], 20, 20, QL_BUTTON_LEFT, 1), QL_OK);
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[1], 20, 20, QL_BUTTON_LEFT, 0), QL_OK);
    QL_CHECK_EQ(ql_window_poll(w0), 1);
    QL_CHECK_EQ(s_clicks[0], 1);
    QL_CHECK_EQ(ql_window_poll(w1), 1);
    QL_CHECK_EQ(s_clicks[1], 1);
    QL_CHECK_EQ(s_events[0], 2);
    QL_CHECK_EQ(s_events[1], 2);

    /* 4. 换界面只影响自己：w0 换一份新树，w1 的点击照旧 */
    QLUIRoot *ui2 = make_ui("#BF3989", &doc2);
    QL_CHECK(ui2 != NULL);
    if (ui2 != NULL) {
        ql_ui_set_click_cb(ui2, on_click, (void *)(intptr_t)0);
        QL_CHECK_EQ(ql_window_set_ui(w0, ui2), QL_OK);
        QL_CHECK(ql_window_ui(w0) == ui2);
        QL_CHECK_EQ(ql_window_poll(w0), 1);

        QL_CHECK_EQ(ql_input_push_button_ex(s_in[1], 20, 20, QL_BUTTON_LEFT, 1), QL_OK);
        QL_CHECK_EQ(ql_input_push_button_ex(s_in[1], 20, 20, QL_BUTTON_LEFT, 0), QL_OK);
        QL_CHECK_EQ(ql_window_poll(w0), 1);       /* w0 队列是空的 */
        QL_CHECK_EQ(s_clicks[1], 1);
        QL_CHECK_EQ(ql_window_poll(w1), 1);
        QL_CHECK_EQ(s_clicks[1], 2);
    }

    /* 5. 销毁窗口 1 后，窗口 0 仍然能收事件 */
    ql_window_destroy(w1);
    QL_CHECK(s_in[1] == NULL);
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[0], 20, 20, QL_BUTTON_LEFT, 1), QL_OK);
    QL_CHECK_EQ(ql_input_push_button_ex(s_in[0], 20, 20, QL_BUTTON_LEFT, 0), QL_OK);
    QL_CHECK_EQ(ql_window_poll(w0), 1);
    QL_CHECK_EQ(s_clicks[0], 2);                 /* w0 的新树自己的回调 */
    QL_CHECK(s_events[0] > 2);

    ql_window_quit(w0);
    QL_CHECK_EQ(ql_window_poll(w0), 0);
    ql_window_destroy(w0);
    ql_ui_free(ui2);
    ql_ui_free(ui1);
    ql_ui_free(ui0);
    ql_ui_doc_free(doc2);
    ql_ui_doc_free(doc1);
    ql_ui_doc_free(doc0);

    qlgl_set_platform(NULL);

QL_TEST_END()
