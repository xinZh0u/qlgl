/**
 * @file    main.c
 * @brief   HTML 标签与 UA 默认样式。
 *
 *   ./build/example/example_08_tags                    # 打开窗口
 *   ./build/example/example_08_tags --dump /tmp/t.ppm  # 截图模式
 *
 * 标签只影响三件事：UA 默认样式、`tag` 选择器、调试名。布局和绘制只看编译出来的
 * QLUIStyle。这个例程就是把这句话演示一遍：
 *
 *   - 上面一半**完全不写 CSS 里的结构性样式**，纯靠 UA 默认值排版；
 *   - 下面一半用同样的标签，只改颜色和字号 —— 说明覆盖有多容易。
 *
 * 语义行内标签现在**真的看得出区别**：`b`/`strong` 是合成加粗（画两遍偏移 1px），
 * `i`/`em` 是合成斜体（按行剪切），`u` 是下划线、`s` 是删除线（各画一条 1px 线），
 * `mark` 是背景高亮。都不需要第二个字体文件。
 */
#include "common.h"

#define W 260
#define H 500

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
    printf("%u 个节点 / %u 条样式\n", (unsigned)doc->node_count,
           (unsigned)doc->style_count);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 标签名可以直接取回（文档里只有下标，没有名字） */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *n = &ui->nodes[i];
        const char *name = ql_ui_tag_name(n->src->tag);
        if (strcmp(name, "div") != 0) {
            const QLRect r = ql_ui_node_rect(n);
            printf("  <%-10s> (%3d,%3d %3dx%3d)\n", name, (int)r.x, (int)r.y,
                   (int)r.w, (int)r.h);
        }
    }

    const int rc = ex_run(ui, W, H, "QLGL · 08 tags", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
