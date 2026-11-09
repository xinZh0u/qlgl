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
#include "common.h"

#define W 640
#define H 522

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
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

    const int rc = ex_run(ui, W, H, "QLGL · 16 css units", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
