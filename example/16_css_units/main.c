/**
 * @file    main.c
 * @brief   CSS 单位与尺寸的一张对照图：每个格子标了"应该是什么"。
 *
 *   ./build/example/example_16_css_units
 *   ./build/example/example_16_css_units --size 400x460   # 看 vw / vh 跟着变
 *
 * 这一页用的是"绝对定位的格子 + 块内正常流"的画法：格子位置写死，
 * 里面的示例块按 CSS 算尺寸 —— 所以哪一块不对，一眼能看出来是哪个特性。
 *
 * 覆盖：px / % / em / rem / vw / vh / vmin / vmax、min/max-width、aspect-ratio、
 *       margin:auto、padding 的 em、box-sizing、hsl()、#rrggbbaa。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 640
#define H 522

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

    /* 把几块关键尺寸打出来：截图之外还能对数字 */
    struct { const char *what; int want; } expect[] = {
        { "2em × 1.5em（字号 16）", 32 },
        { "6rem（根字号 12）", 72 },
        { "aspect 16/9 宽 140", 140 },
    };
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLRect r = ql_ui_node_rect(&ui->nodes[i]);
        const char *t = ql_ui_node_text(&ui->nodes[i]);
        if (t == NULL)
            continue;
        for (size_t k = 0; k < sizeof(expect) / sizeof(expect[0]); k++)
            if (strstr(t, expect[k].what) != NULL)
                printf("%-28s 声明宽 %d\n", expect[k].what, (int)r.w);
    }

    QLWindow *win = ql_window_create("QLGL · 16 css units", W, H, QL_WINDOW_GL);
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
