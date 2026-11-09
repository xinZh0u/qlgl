/**
 * @file    main.c
 * @brief   B4 绘制细节（第一批）：`border-style` 与 `outline`。
 *
 *   ./build/example/example_25_b4
 *   ./build/example/example_25_b4 --size 720x760 --dump b4.ppm
 *
 * 这两样都**不占布局**：
 *   - `dashed` / `dotted` 只是在画边时把四条边切成小段填色；
 *   - `outline` 画在边框盒外面，盒子的位置和尺寸一点不变
 *     （所以 focus 环、选中高亮用它最合适）。
 *
 * 第 5 节点一下输入框，就能看到 `:focus` 把 outline 打开（终端会打印焦点节点）。
 */
#include "common.h"

#define W 720
#define H 760

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    if (node == NULL || node->style == NULL)
        return;
    /* 点哪儿就把焦点给哪儿（真实控件由 ql_ui_input_event 内部处理，这里演示意图） */
    ql_ui_set_focus_node(ui, node);
    printf("点击：outline=%dpx/%d，border=%dpx/%d（上边）\n",
           (int)node->style->outline_width, (int)node->style->outline_style,
           (int)node->style->border_w[0], (int)node->style->border_style4[0]);
}

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
    ql_ui_set_click_cb(ui, on_click, NULL);

    /* 启动就把焦点给输入框：截图/首屏都能看到 focus 环长什么样 */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (ui->nodes[i].src != NULL && ui->nodes[i].src->tag == QL_UI_TAG_INPUT) {
            ql_ui_set_focus_node(ui, &ui->nodes[i]);
            break;
        }
    }

    const int rc = ex_run(ui, W, H, "QLGL · 25 B4（点一下输入框）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
