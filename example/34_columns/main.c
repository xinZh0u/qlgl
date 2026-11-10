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
#include "common.h"

#define W 816
#define H 800

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

    const int rc = ex_run(ui, W, H, "QLGL · 34 多栏", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
