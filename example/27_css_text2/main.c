/**
 * @file    main.c
 * @brief   B3：`word-spacing` / `tab-size` / `text-decoration-*`。
 *
 *   ./build/example/example_27_css_text2
 *   ./build/example/example_27_css_text2 --size 720x620 --dump text2.ppm
 *
 * 三样都只在**绘制/量宽**时生效，布局大框架不动：
 *   - `word-spacing` 只加在空格之后（负值可以把词挤在一起）；
 *   - `tab-size` 把 '\t' 折算成 N 个空格（构建期展开，运行期零成本）；
 *   - `text-decoration-*` 决定画哪几条线、什么形态、多粗、什么颜色。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 720
#define H 620

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

    QLWindow *win = ql_window_create("QLGL · 27 文本细节", W, H, QL_WINDOW_GL);
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
