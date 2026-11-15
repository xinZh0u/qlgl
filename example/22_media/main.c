/**
 * @file    main.c
 * @brief   `@media` 断点：同一份 HTML，窗口变窄就换布局。
 *
 *   ./build/example/example_22_media                  # 打开窗口，拖边缘缩放
 *   ./build/example/example_22_media --size 800x400 --dump wide.ppm
 *   ./build/example/example_22_media --size 420x600 --dump narrow.ppm
 *
 * 运行期**不解析任何条件**：构建期已经把宽度轴切成区间，每个区间编译一套样式；
 * 布局时只按视口宽选区间下标（线性扫一个 6 项以内的小数组）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 820
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
    printf("断点区间 %d 个：", (int)ui->doc->media_count);
    for (uint16_t i = 0; i < ui->doc->media_count; i++) {
        const QLUIMedia *m = &ui->doc->media[i];
        if (m->hi == 0xFFFFu)
            printf(" [%u, ∞)", (unsigned)m->lo);
        else
            printf(" [%u, %u]", (unsigned)m->lo, (unsigned)m->hi);
    }
    printf("\n");

    ql_ui_layout(ui, W, H);

    QLWindow *win = ql_window_create("QLGL · 22 media（拖边缘缩放）", W, H, QL_WINDOW_GL);
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
