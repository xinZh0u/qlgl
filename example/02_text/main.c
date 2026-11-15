/**
 * @file    main.c
 * @brief   文本：字号、颜色、对齐、换行。
 *
 *   ./build/example/example_02_text --dump /tmp/02.ppm
 *
 * 文本的规则很简单：
 *   - 一个元素只能有一整段文本（混合内容在编译期被消解成匿名 <span>）；
 *   - 字号来自 `font-size`，颜色来自 `color`（都会被继承）；
 *   - `text-align` 控制水平对齐。
 * 文本本身**不换行**（不会自动折行），要换行就多写几个元素。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

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

    /* 字体表：doc 里有几种字号 */
    printf("字号表：");
    for (uint16_t i = 0; i < ui->doc->font_size_count; i++)
        printf(" %upx", (unsigned)ui->doc->font_sizes[i]);
    printf("\n");

    QLWindow *win = ql_window_create("QLGL · 02 text", 260, 220, QL_WINDOW_GL);
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
