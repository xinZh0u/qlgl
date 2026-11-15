/**
 * @file    main.c
 * @brief   `calc()` / `min()` / `max()` / `clamp()`：布局期才求值的算术。
 *
 *   ./build/example/example_23_calc                       # 打开窗口，拖边缘缩放
 *   ./build/example/example_23_calc --size 760x420 --dump wide.ppm
 *   ./build/example/example_23_calc --size 420x460 --dump narrow.ppm
 *
 * 为什么不能像别的长度那样在构建期算成一个数：表达式里有 `%` / `vw` / `vh`，
 * 它们的基准（父内容盒、视口）只有布局时才知道。所以构建期把公式编译成一段
 * 后缀字节码放进文档（`ui->doc->calc_ops`），样式里只留 16 位下标；布局时求值。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 760
#define H 420

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

    QLWindow *win = ql_window_create("QLGL · 23 calc（拖边缘缩放）", W, H, QL_WINDOW_GL);
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
