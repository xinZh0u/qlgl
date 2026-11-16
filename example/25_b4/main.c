/**
 * @file    main.c
 * @brief   B4 绘制细节（第一批）：`border-style` 与 `outline`。
 *
 *   ./build/example/example_25_b4
 *   ./build/example/example_25_b4 --size 720x760 --dump b4.ppm
 *
 * 这两样都**不占布局**：
 *   - `dashed` / `dotted` 只是在画边时把四条边切成小段填色；
 *   - `outline` 画在边框盒外面，盒子的位置和尺寸一点不变
 *     （所以 focus 环、选中高亮用它最合适）。
 *
 * 点一下输入框，就能看到 `:focus` 把 outline 打开（终端会打印焦点节点）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 720
#define H 760

static void on_input_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    if (node == NULL || node->style == NULL)
        return;
    /* 输入框自己会拿焦点，这里只是把事件打出来（真实控件由 ql_ui_input_event 处理） */
    ql_ui_set_focus_node(ui, node);
    printf("点击：outline=%dpx/%d，border=%dpx/%d（上边）\n",
           (int)node->style->outline_width, (int)node->style->outline_style,
           (int)node->style->border_w[0], (int)node->style->border_style4[0]);
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

    /* 启动就把焦点给输入框：截图/首屏都能看到 focus 环长什么样 */
    QLUINodeRT *input = ql_ui_find_by_id(ui, "name");
    if (input != NULL) {
        ql_ui_set_focus_node(ui, input);
        (void)ql_ui_set_node_click_cb(ui, input, on_input_click, NULL);
    }

    QLWindow *win = ql_window_create("QLGL · 25 B4（点一下输入框）", W, H, QL_WINDOW_GL);
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
