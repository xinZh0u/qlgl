/**
 * @file    main.c
 * @brief   B7：表格 —— `display: table` 全家 + `<table>` 系列标签。
 *
 *   ./build/example/example_33_table
 *   ./build/example/example_33_table --size 820x900 --dump table.ppm
 *
 * 这一页只讲表格：列宽怎么定、`colspan`/`rowspan` 怎么跨、`border-spacing` 与
 * `border-collapse` 的区别、单元格里的 `vertical-align`。
 *
 * 实现要点（详见《CSS3 覆盖与裁剪》§18.12）：`table` 只负责"收集行、定列宽、
 * 量行高、摆单元格"，行与行组只是分组（不画东西）；单元格才是真正的盒子。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 820
#define H 1400

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

    QLWindow *win = ql_window_create("QLGL · 33 表格", W, H, QL_WINDOW_GL);
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
