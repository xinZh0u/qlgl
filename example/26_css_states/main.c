/**
 * @file    main.c
 * @brief   B2：`:disabled` / `:checked` / `::placeholder`（纯 CSS 的状态）。
 *
 *   ./build/example/example_26_css_states
 *   ./build/example/example_26_css_states --size 760x520 --dump states.ppm
 *
 * 三件事都能直接玩：
 *   - 点复选框：`:checked` 变体立刻生效（构建期编好的那份样式，运行期只翻一个位）；
 *   - 禁用的按钮/输入框：拿不到焦点，输入框点了也不出光标；
 *   - 输入框空的时候显示 `::placeholder` 的样式，打字后自动消失。
 */
#include "common.h"

#define W 760
#define H 520

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)user;
    if (node == NULL || node->src == NULL)
        return;
    if ((node->src->flags & QL_UI_NODE_CHECKBOX) != 0)
        printf("复选框：%s\n", (node->flags & QL_UI_RT_CHECKED) ? "已勾选" : "未勾选");
    else if ((node->flags & QL_UI_RT_DISABLED) != 0)
        printf("这个节点是 disabled，点击被忽略\n");
    else
        printf("点击：tag=%d\n", (int)node->src->tag);
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

    const int rc = ex_run(ui, W, H, "QLGL · 26 CSS 状态（点复选框试试）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
