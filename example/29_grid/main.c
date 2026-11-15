/**
 * @file    main.c
 * @brief   B6：CSS Grid（具名区域 / 12 列栅格 / 跨格 / 自动放置 / 对齐）。
 *
 *   ./build/example/example_29_grid
 *   ./build/example/example_29_grid --size 760x560 --dump grid.ppm
 *
 * 拖动窗口边缘看一件事：**列宽是按容器宽度重新分配的**（fr 与百分比），
 * 而行高在 `grid-auto-rows` 定死的那些行里保持不变。
 *
 * 这里没有任何布局代码 —— 全部来自 style.css。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 760
#define H 560

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

    QLWindow *win = ql_window_create("QLGL · 29 Grid（拖边缘缩放）", W, H, QL_WINDOW_GL);
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
