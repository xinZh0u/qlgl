/**
 * @file    02_text.c
 * @brief   文本：字号、颜色、对齐、换行。
 *
 *   ./build/example/02_text --dump /tmp/02.ppm
 *
 * 文本的规则很简单：
 *   - 一个元素只能有一整段文本（混合内容在编译期被消解成匿名 <span>）；
 *   - 字号来自 `font-size`，颜色来自 `color`（都会被继承）；
 *   - `text-align` 控制水平对齐。
 * 文本本身**不换行**（不会自动折行），要换行就多写几个元素。
 */
#include "common.h"

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"h1\">字号 = font-size</div>"
    "  <div class=\"s12\">12px 小字</div>"
    "  <div class=\"s16\">16px 正文</div>"
    "  <div class=\"s24\">24px 标题</div>"
    "  <div class=\"c\">居中的一行</div>"
    "  <div class=\"dim\">灰色、右对齐</div>"
    "</div>";

static const char *k_css =
    "#root { width: 260px; height: 220px; background-color: #0F1218; }"
    ".h1  { margin: 12px 0 8px 14px; font-size: 13px; color: #6C7783; }"
    ".s12 { margin: 2px 14px; font-size: 12px; color: #C8D2DE; }"
    ".s16 { margin: 2px 14px; font-size: 16px; color: #C8D2DE; }"
    ".s24 { margin: 2px 14px; font-size: 24px; color: #7CE0FF; }"
    ".c   { margin: 12px 14px 4px 14px; font-size: 14px; color: #E6C34A;"
    "       text-align: center; }"
    ".dim { margin: 2px 14px; font-size: 12px; color: #7A8794;"
    "       text-align: right; }";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_load_font();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
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

    /* 字体表：doc 里有几种字号 */
    printf("字号表：");
    for (uint16_t i = 0; i < doc->font_size_count; i++)
        printf(" %upx", (unsigned)doc->font_sizes[i]);
    printf("\n");

    const int rc = ex_run(ui, 260, 220, "QLGL · 02 text", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
