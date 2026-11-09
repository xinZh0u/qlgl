/**
 * @file    main.c
 * @brief   CSS 文本细节：letter-spacing / text-indent / text-transform /
 *          text-overflow: ellipsis / word-break。
 *
 *   ./build/example/example_19_css_text
 *
 * 每一格都摆成"对照组"：上面那格是不生效时的样子，下面那格是生效后的样子。
 * 省略号那一段就是真实界面里最常见的写法（左名字自动缩、右数字固定宽）。
 */
#include "common.h"

#define W 640
#define H 440

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

    int ls = 0, ell = 0, brk = 0, tt = 0, ind = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUIStyle *s = ql_ui_node_style(&ui->nodes[i]);
        if (s->letter_spacing != 0)
            ls++;
        if (s->text_overflow == QL_UI_OVERFLOW_ELLIPSIS)
            ell++;
        if (s->word_break == QL_UI_WORDBREAK_ALL)
            brk++;
        if (s->text_transform != QL_UI_TEXT_NONE)
            tt++;
        if (s->text_indent != 0)
            ind++;
    }
    printf("节点 %d 个：带字距 %d，带省略号 %d，break-all %d，大小写 %d，首行缩进 %d\n",
           (int)ui->node_count, ls, ell, brk, tt, ind);

    const int rc = ex_run(ui, W, H, "QLGL · 19 css text", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
