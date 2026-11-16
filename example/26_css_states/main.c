/**
 * @file    main.c
 * @brief   B2：`:disabled` / `:checked` / `::placeholder`（纯 CSS 的状态）。
 *
 *   ./build/example/example_26_css_states
 *   ./build/example/example_26_css_states --size 760x520 --dump states.ppm
 *
 * 三件事都能直接玩：
 *   - 点复选框：`:checked` 变体立刻生效（构建期编好的那份样式，运行期只翻一个位）；
 *   - 禁用的按钮/输入框：拿不到焦点，输入框点了也不出光标；
 *   - 输入框空的时候显示 `::placeholder` 的样式，打字后自动消失。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 760
#define H 520

static void on_check_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    if (node == NULL)
        return;
    printf("复选框 %s：%s\n", (const char *)user,
           (node->flags & QL_UI_RT_CHECKED) ? "已勾选" : "未勾选");
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

    /* 每个复选框各自接一个回调；禁用的那个不会触发（与浏览器一致） */
    static const char *const ids[] = { "c1", "c2", "c3" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        QLUINodeRT *cb = ql_ui_find_by_id(ui, ids[i]);
        if (cb != NULL)
            (void)ql_ui_set_node_click_cb(ui, cb, on_check_click, (void *)ids[i]);
    }

    QLWindow *win = ql_window_create("QLGL · 26 CSS 状态（点复选框试试）", W, H, QL_WINDOW_GL);
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
