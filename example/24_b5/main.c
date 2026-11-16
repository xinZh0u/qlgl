/**
 * @file    main.c
 * @brief   B5 小件：`visibility` / `pointer-events` / `align-self` / `order`。
 *
 *   ./build/example/example_24_b5
 *   ./build/example/example_24_b5 --size 720x520 --dump b5.ppm
 *
 * 点击"可点区域"：装饰层写的是 `pointer-events: none`，所以点击穿过它落到按钮上
 * （终端会打印命中的节点）。把那一行删掉再跑，点击就只会命中装饰层。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 720
#define H 900

static int s_hits;

static void on_ok_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)node;
    (void)user;
    s_hits++;
    static char buf[128];
    snprintf(buf, sizeof(buf), "点击 %d 次：装饰层 pointer-events:none，没挡住", s_hits);
    QLUINodeRT *hits = ql_ui_find_by_id(ui, "hits");
    if (hits != NULL)
        (void)ql_ui_set_text(ui, hits, buf);
    printf("%s\n", buf);
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 只给"可点区域"接回调：点到装饰层会穿过去落到 #ok（见上面注释） */
    QLUINodeRT *ok = ql_ui_find_by_id(ui, "ok");
    if (ok != NULL)
        (void)ql_ui_set_node_click_cb(ui, ok, on_ok_click, NULL);

    /* sticky 列表先滚一点，截图里才看得出"表头钉住了、内容滚过去" */
    QLUINodeRT *list = ql_ui_find_by_id(ui, "list");
    if (list != NULL && ql_ui_scrollable(list))
        (void)ql_ui_scroll_by(ui, list, 0, 46);

    QLWindow *win = ql_window_create("QLGL · 24 B5（点一下紫色按钮）", W, H, QL_WINDOW_GL);
    if (win == NULL) {
        printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }
    ql_window_bind_ui(win, ui);
    const int rc = ql_window_run_args(win, argc, argv);
    ql_window_destroy(win);
    ql_ui_free(ui);
    qlgl_shutdown();
    return rc;
}
