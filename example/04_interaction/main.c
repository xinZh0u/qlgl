/**
 * @file    main.c
 * @brief   交互：鼠标移入/按下改样式，点击回调。
 *
 *   ./build/example/example_04_interaction
 *
 * 三件你要知道的事：
 *   1. `:hover` / `:active` / `:focus` 在编译期就变成了"状态变体样式"，
 *      运行期切状态只是换一份样式指针 —— 零解析、零重排；
 *   2. 事件从窗口进来后要 `ql_input_dispatch()` 才会到界面；
 *   3. 点击用 `ql_ui_set_click_cb()` 接回调，不需要自己算命中。
 */
#include "common.h"

static QLUIRoot *s_ui;
static QLUINodeRT *s_log;
static int s_clicks;

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)user;
    if (node->src == NULL || node->src->tag != QL_UI_TAG_DIV)
        return;                             /* 只关心按钮那一层 */
    s_clicks++;
    char buf[64];
    snprintf(buf, sizeof(buf), "点击次数：%d", s_clicks);
    ql_ui_set_text(s_ui, s_log, buf);       /* 改文本会顺带标脏 */
    printf("%s\n", buf);
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
    s_ui = ui;
    /* 节点顺序 = 文档顺序：0=root, 1=btn, 2=log（深度优先，父在前） */
    s_log = &ui->nodes[2];

    /* 接点击回调：所有节点共用一条回调，按 node 区分 */
    ql_ui_set_click_cb(ui, on_click, NULL);

    /* 没有窗口时，用合成事件走一遍，验证交互链路 */
    if (argc == 1) {
        ql_ui_layout(ui, 260, 140);
        printf("合成一次点击（窗口模式请用鼠标点按钮）\n");
        (void)ql_ui_pointer_move(ui, 84, 48);
        (void)ql_ui_pointer_down(ui, 84, 48);
        (void)ql_ui_pointer_up(ui, 84, 48);
    }

    const int rc = ex_run(ui, 260, 140, "QLGL · 04 interaction", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
