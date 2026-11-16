/**
 * @file    main.c
 * @brief   多窗口：两个窗口、两棵界面树、两条输入队列互不干扰。
 *
 *   ./build/example/example_43_multi_window
 *   ./build/example/example_43_multi_window --seconds 3   # 跑 3 秒自动退出
 *
 * 关键点不在"开两个窗口"，而在**输入路由**：
 * 每个 `QLWindow` 在创建时拿到自己的 `QLInput`，平台后端（X11/Wayland/GL）
 * 把该窗口的事件推进这一条队列；`ql_window_poll()` 只派发自己的队列。
 * 所以点左边窗口的按钮，右边窗口的界面不会有任何反应。
 *
 * 关闭其中一个窗口只销毁它自己；两个都关了（或 --seconds 到点）才退出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#define W 260
#define H 160

typedef struct {
    const char *name;
    int         clicks;
    char        text[64];   /* ql_ui_set_text() 不复制：缓冲必须一直有效 */
} Counter;

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    Counter *c = (Counter *)user;
    c->clicks++;
    snprintf(c->text, sizeof(c->text), "%s: %d click%s", c->name, c->clicks,
             c->clicks == 1 ? "" : "s");
    /* 按钮文字自己更新（click 回调给的就是被点的节点）。
     * 注意 set_text 不复制：必须传长期有效的缓冲，不能传栈上的临时 buf。 */
    (void)ql_ui_set_text(ui, node, c->text);
    printf("[%s] click #%d\n", c->name, c->clicks);
    fflush(stdout);
}

static QLUIRoot *make_ui(const char *title, const char *hint, const char *bg,
                         Counter *counter, QLUIDoc **out_doc)
{
    char html[512];
    char css[512];
    snprintf(html, sizeof(html),
             "<div id=root>"
             "<div class=\"t\">%s</div>"
             "<div id=btn>%s: 0 clicks</div>"
             "<div class=\"h\">%s</div>"
             "</div>", title, counter->name, hint);
    snprintf(css, sizeof(css),
             "#root { width: 100%%; height: 100%%; padding: 10px;"
             " background-color: %s; color: #FFFFFF; font-size: 12px; }"
             ".t { font-size: 16px; margin-bottom: 8px; }"
             "#btn { height: 36px; padding: 6px; border-radius: 6px;"
             " background-color: rgba(255,255,255,0.22); }"
             ".h { margin-top: 10px; font-size: 11px; color: rgba(255,255,255,0.75); }",
             bg);
    QLUIDoc *doc = ql_ui_load_html(html, css);
    if (doc == NULL)
        return NULL;
    QLUIRoot *ui = ql_ui_build(doc);
    /* 手动 build 时 root **不拥有** doc：doc 必须活到 ui 释放之后 */
    *out_doc = doc;
    if (ui != NULL) {
        QLUINodeRT *btn = ql_ui_find_by_id(ui, "btn");
        if (btn != NULL)
            (void)ql_ui_set_node_click_cb(ui, btn, on_click, counter);
    }
    return ui;
}

int main(int argc, char **argv)
{
    int seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
    }

    if (qlgl_init() != QL_OK)
        return 1;

    static Counter left  = { "left", 0, "" };
    static Counter right = { "right", 0, "" };

    QLUIDoc *doc_l = NULL, *doc_r = NULL;
    QLUIRoot *ui_l = make_ui("Window A", "click me", "#0969DA", &left, &doc_l);
    QLUIRoot *ui_r = make_ui("Window B", "click me", "#1A7F37", &right, &doc_r);
    if (ui_l == NULL || ui_r == NULL) {
        fprintf(stderr, "ui build failed\n");
        return 1;
    }

    QLWindow *wl = ql_window_create("QLGL · A", W, H, QL_WINDOW_GL);
    QLWindow *wr = ql_window_create("QLGL · B", W, H, QL_WINDOW_GL);
    if (wl == NULL || wr == NULL) {
        fprintf(stderr, "window create failed（没有 GL/X11？）\n");
        return 1;
    }
    (void)ql_window_bind_ui(wl, ui_l);
    (void)ql_window_bind_ui(wr, ui_r);
    ql_window_set_bg(wl, QL_COLOR_RGB(0x09, 0x69, 0xDA));
    ql_window_set_bg(wr, QL_COLOR_RGB(0x1A, 0x7F, 0x37));

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (wl != NULL || wr != NULL) {
        if (wl != NULL && !ql_window_poll(wl)) {
            ql_window_destroy(wl);
            wl = NULL;
            printf("window A closed\n");
        }
        if (wr != NULL && !ql_window_poll(wr)) {
            ql_window_destroy(wr);
            wr = NULL;
            printf("window B closed\n");
        }
        if (seconds > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= seconds)
                break;
        }
    }

    if (wl != NULL)
        ql_window_destroy(wl);
    if (wr != NULL)
        ql_window_destroy(wr);
    ql_ui_free(ui_l);
    ql_ui_free(ui_r);
    ql_ui_doc_free(doc_l);
    ql_ui_doc_free(doc_r);
    qlgl_shutdown();
    printf("总计：left %d 次，right %d 次\n", left.clicks, right.clicks);
    return 0;
}
