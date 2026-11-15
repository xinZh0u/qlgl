/**
 * @file    main.c
 * @brief   CSS 选择器与伪元素：每条规则都配一块看得见的样例。
 *
 *   ./build/example/example_17_css_selectors
 *
 * 左列是"单条件命中"（结构伪类、属性选择器、:not()、:nth-of-type()），
 * 右列是"组合与生成"（::before/::after、`+`、`~`、`*`、状态伪类）。
 *
 * 全部在**构建期**结算成具体样式：运行期既没有选择器匹配，也没有伪元素对象。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 640
#define H 512

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

    /* 数一下 ::before / ::after 生成了几个真节点：伪元素在本库是实体 */
    int bullet = 0, quote = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const char *t = ql_ui_node_text(&ui->nodes[i]);
        if (t == NULL)
            continue;
        const unsigned char b0 = (unsigned char)t[0];
        if (b0 == 0xE2)          /* "•" 与 “ ” 都是 0xE2 开头的三字节 UTF-8 */
            bullet++;
        else if (b0 == '"')
            quote++;
        (void)quote;
    }
    printf("节点 %d 个，其中 ::before 生成的圆点 %d 个\n", (int)ui->node_count, bullet);

    QLWindow *win = ql_window_create("QLGL · 17 css selectors", W, H, QL_WINDOW_GL);
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
