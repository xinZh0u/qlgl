/**
 * @file    main.c
 * @brief   `outline` 的宽度关键字（`thin` / `medium` / `thick`）与 `outline: 0`。
 *
 *   ./build/example/example_40_outline
 *
 * 浏览器接受这三种关键字（实测 thin=1 / medium=3 / thick=5 px），
 * `outline: 0` 则是"去掉 focus 环"最常用的写法。早先版本把这两种 token
 * 都当成颜色，于是 `outline: 0` 报"不支持的颜色"、`outline: thin` 直接报错。
 *
 * 启动后对照界面上的六个格子：环宽依次是 无 / 1 / 3 / 5 / 3(内缩) / 5(外扩)。
 */
#include "common.h"

#define W 720
#define H 300

/* 找节点的第一段后代文本（卡片自己的文本节点是子节点） */
static const char *first_text(QLUIRoot *ui, const QLUINodeRT *n)
{
    for (const QLUINodeRT *c = n->first_child; c != NULL; c = c->next_sibling) {
        const char *t = ql_ui_node_text(c);
        if (t != NULL && t[0] != '\0')
            return t;
        t = first_text(ui, c);
        if (t != NULL)
            return t;
        (void)ui;
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

    /* 自检：把每个画了描边的节点打出来（宽度 / offset / 颜色） */
    int n = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *node = &ui->nodes[i];
        const QLUIStyle *st = node->style;
        if (st == NULL || st->outline_width <= 0)
            continue;
        const char *t = first_text(ui, node);
        printf("描边 %d：'%s' 宽 %dpx offset %dpx 色 %06X\n", ++n,
               (t != NULL) ? t : "(无文本)", (int)st->outline_width,
               (int)st->outline_offset, (unsigned)st->outline_color);
    }
    printf("共 %d 个描边（应为 5 个：thin / medium / thick / 内缩 / 长写）\n", n);

    const int rc = ex_run(ui, W, H, "QLGL · 40 outline 宽度关键字", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
