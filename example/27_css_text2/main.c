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
#include "common.h"

#define W 720
#define H 620

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

    const int rc = ex_run(ui, W, H, "QLGL · 27 文本细节", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
