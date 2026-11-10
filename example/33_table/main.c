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
#include "common.h"

#define W 820
#define H 1400

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

    const int rc = ex_run(ui, W, H, "QLGL · 33 表格", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
