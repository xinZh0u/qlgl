/**
 * @file    main.c
 * @brief   `calc()` / `min()` / `max()` / `clamp()`：布局期才求值的算术。
 *
 *   ./build/example/example_23_calc                       # 打开窗口，拖边缘缩放
 *   ./build/example/example_23_calc --size 760x420 --dump wide.ppm
 *   ./build/example/example_23_calc --size 420x460 --dump narrow.ppm
 *
 * 为什么不能像别的长度那样在构建期算成一个数：表达式里有 `%` / `vw` / `vh`，
 * 它们的基准（父内容盒、视口）只有布局时才知道。所以构建期把公式编译成一段
 * 后缀字节码放进文档（`doc->calc_ops`），样式里只留 16 位下标；布局时求值。
 */
#include "common.h"

#define W 760
#define H 420

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
    printf("calc 字节码 %d 条（每条 4 字节）\n", (int)doc->calc_op_count);
    for (uint16_t i = 0; i < doc->calc_op_count; i++) {
        const QLUICalcOp *op = &doc->calc_ops[i];
        const char *name = "?";
        switch (op->op) {
        case QL_UI_CALC_TERM: name = "TERM"; break;
        case QL_UI_CALC_ADD: name = "ADD"; break;
        case QL_UI_CALC_SUB: name = "SUB"; break;
        case QL_UI_CALC_MIN: name = "MIN"; break;
        case QL_UI_CALC_MAX: name = "MAX"; break;
        case QL_UI_CALC_CLAMP: name = "CLAMP"; break;
        case QL_UI_CALC_END: name = "END"; break;
        default: break;
        }
        if (op->op == QL_UI_CALC_TERM)
            printf("  [%2u] %-5s value=%d unit=%u\n", (unsigned)i, name, (int)op->value,
                   (unsigned)op->unit);
        else
            printf("  [%2u] %-5s n=%u\n", (unsigned)i, name, (unsigned)op->unit);
    }

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    const int rc = ex_run(ui, W, H, "QLGL · 23 calc（拖边缘缩放）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
