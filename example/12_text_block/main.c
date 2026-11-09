/**
 * @file    main.c
 * @brief   TEXT 包：`<pre>` 空白保留、`<sub>`/`<sup>` 上下标。
 *
 *   ./build/example/example_12_text_block                    # 打开窗口
 *   ./build/example/example_12_text_block --dump /tmp/a.ppm   # 截图
 *
 * 这一期只加两件事，但两件都改变了"文本怎么排"：
 *
 *   1. `white-space: pre` —— 空白与换行**全部保留**、不自动折行。
 *      `<pre>` 的 UA 默认就是它，所以代码块直接能贴。
 *      代价：行首空格是内容的一部分，所以**不能靠缩进美化 `<pre>` 的源码**。
 *   2. `vertical-align: sub / super` —— 基线偏移（按字号比例），
 *      配合 `font-size: smaller`（相对父级 5/6）就是 `<sub>`/`<sup>`。
 *
 * 另外看一眼状态行：本库**没有等宽字体族的分家**（除非注册了 mono factory），
 * 内建位图字体本身就是等宽的；接了矢量字体后 `<pre>`/`<code>` 才算"真等宽"。
 */
#include "common.h"

#define W 340
#define H 470

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
     * 手动查一遍"空白真的留下了"：找到 pre 节点，看它的文本里有没有换行。
     * 这一步在真实界面里不需要，写出来是因为 pre 出问题时最难一眼看出来。
     */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (ui->nodes[i].src->tag != QL_UI_TAG_PRE)
            continue;
        const char *t = ui->nodes[i].text;
        int nl = 0;
        for (const char *p = t; p != NULL && *p != '\0'; p++)
            if (*p == '\n')
                nl++;
        printf("<pre> 里有 %d 个换行，盒子 %dx%d\n", nl,
               (int)ui->nodes[i].border_box.w, (int)ui->nodes[i].border_box.h);
    }

    const int rc = ex_run(ui, W, H, "QLGL · 12 text block", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
