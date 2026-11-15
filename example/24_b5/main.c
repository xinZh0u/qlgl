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

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)user;
    const QLUITag tag = (node != NULL && node->src != NULL)
                            ? (QLUITag)node->src->tag
                            : (QLUITag)0;
    printf("点击命中：节点 tag=%d，文本=\"%s\"\n", (int)tag,
           (node != NULL && node->text != NULL) ? node->text : "");
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
    ql_ui_set_click_cb(ui, on_click, NULL);

    /* sticky 列表先滚一点，截图里才看得出"表头钉住了、内容滚过去" */
    QLUINodeRT *list = ql_ui_find_by_tag(ui, QL_UI_TAG_DIV, 0);
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (ui->nodes[i].src != NULL && ui->nodes[i].src->tag == QL_UI_TAG_DIV &&
            ui->nodes[i].text == NULL && ui->nodes[i].style != NULL &&
            ui->nodes[i].style->overflow_y == QL_UI_OVERFLOW_AUTO) {
            list = &ui->nodes[i];
            break;
        }
    }
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
