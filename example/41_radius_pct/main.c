/**
 * @file    main.c
 * @brief   `border-radius` 的百分比：横向按宽、纵向按高，布局期折成 px。
 *
 *   ./build/example/example_41_radius_pct
 *
 * 百分比圆角在构建期算不出来（要盒子自身的宽高），所以解析时存成**负数**
 * 当标记（`50%` → -50），布局结束拿到边框盒后再折成 px 写进样式副本。
 * 绘制侧读到的仍是 px，因此没有任何绘制/命中代码需要改。
 */
#include "common.h"

#define W 720
#define H 300

/* 找节点的第一段后代文本（卡片自己的文本节点是子节点） */
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

    /* 自检：布局后每个带圆角的节点，半径应该都已经是 px（不再有负数） */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *node = &ui->nodes[i];
        const QLUIStyle *st = node->style;
        if (st == NULL)
            continue;
        const int any = st->radius[0] | st->radius[1] | st->radius[2] |
                        st->radius[3] | st->radius_v[0] | st->radius_v[1] |
                        st->radius_v[2] | st->radius_v[3];
        if (any == 0)
            continue;
        const char *t = first_text(node);
        printf("圆角 '%s' 盒子 %dx%d → 横向 %d/%d/%d/%d 纵向 %d/%d/%d/%d\n",
               (t != NULL) ? t : "(无文本)", node->border_box.w, node->border_box.h,
               (int)st->radius[0], (int)st->radius[1], (int)st->radius[2],
               (int)st->radius[3], (int)st->radius_v[0], (int)st->radius_v[1],
               (int)st->radius_v[2], (int)st->radius_v[3]);
    }

    const int rc = ex_run(ui, W, H, "QLGL · 41 border-radius 百分比", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
