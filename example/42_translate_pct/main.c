/**
 * @file    main.c
 * @brief   `transform: translate(<x>%, <y>%)`：百分比相对元素自身边框盒。
 *
 *   ./build/example/example_42_translate_pct
 *
 * 平移不进布局（绘制期加偏移），但百分比要等盒子尺寸才折得出 px，
 * 所以解析时先存百分数并打标记（`tf_pct` 位图），布局后由
 * `resolve_tf_pct()` 折成 px 写回样式副本 —— 绘制/命中读到的仍是 px。
 */
#include "common.h"

#define W 720
#define H 300

static const char *first_text(const QLUINodeRT *n)
{
    for (const QLUINodeRT *c = n->first_child; c != NULL; c = c->next_sibling) {
        const char *t = ql_ui_node_text(c);
        if (t != NULL && t[0] != '\0')
            return t;
        t = first_text(c);
        if (t != NULL)
            return t;
    }
    return NULL;
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

    /*
     * 自检：位移已经在布局后折成 px（tf_pct 应已被清零），
     * 横向按盒子宽、纵向按盒子高。打印每个有位移的盒子。
     */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *node = &ui->nodes[i];
        const QLUIStyle *st = node->style;
        if (st == NULL || (st->tf_x == 0 && st->tf_y == 0))
            continue;
        const char *t = ql_ui_node_text(node);   /* 文本可能直接挂在节点上 */
        if (t == NULL || t[0] == '\0')
            t = first_text(node);
        if (t == NULL)
            continue;
        printf("位移 '%s' 盒子 %dx%d → (%d, %d) 残留标记 %d\n", t,
               node->border_box.w, node->border_box.h, (int)st->tf_x,
               (int)st->tf_y, st->tf_pct);
    }

    const int rc = ex_run(ui, W, H, "QLGL · 42 translate 百分比", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
