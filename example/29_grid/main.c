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
#include "common.h"

#define W 760
#define H 560

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

    const int rc = ex_run(ui, W, H, "QLGL · 29 Grid（拖边缘缩放）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
