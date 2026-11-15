/**
 * @file    main.c
 * @brief   B7 后半：多栏（`column-count` / `column-width` / `column-rule`）。
 *
 *   ./build/example/example_34_columns
 *   ./build/example/example_34_columns --size 820x760 --dump columns.ppm
 *
 * 这一页只讲多栏：栏数怎么定、栏宽怎么算、栏高不够时怎么换栏、分隔线画在哪。
 *
 * 实现要点（详见《CSS3 覆盖与裁剪》§19）：容器按"量一遍 → 按栏高贪心分配 →
 * 平移"三步排；**不把一个子节点劈到两栏**，也不支持 `column-span: all`。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 816
#define H 800

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

    QLWindow *win = ql_window_create("QLGL · 34 多栏", W, H, QL_WINDOW_GL);
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
