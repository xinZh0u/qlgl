/**
 * @file    main.c
 * @brief   `::before` / `::after` 的**多条规则**：一个伪元素只生成一个节点。
 *
 *   ./build/example/example_39_pseudo_multi
 *
 * 浏览器里 `.x::before` 可以被拆成任意多条规则写，声明按级联合并到**同一个**
 * 伪元素上。早先版本每条规则各建一个伪节点，于是：
 *   `.x::after { content:""; … }` + `.x::after { transform: … }` 直接报
 *   "缺少 content"，而两条都写 content 时会冒出两个伪元素。
 *
 * 现在按"选择器完全相同"分组：组里任一条写了 `content` 就生成**一个**节点，
 * 其余规则只贡献声明（仍是一条规则一条内部规则，级联语义不变）。
 *
 * 启动后对照界面上的四个格子：每个格子的徽标/装饰都只该出现**一次**。
 */
#include "common.h"

#define W 720
#define H 396

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
     * 抠一下伪元素的数量：伪元素在本库是**实体节点**，文本就是 `content` 的值。
     * 徽标 "1"/"2"/"3" 各应当只出现一次 —— 出现两次就说明冒出了重复的伪元素。
     */
    int n1 = 0, n2 = 0, n3 = 0, nodes = (int)ui->node_count;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const char *t = ql_ui_node_text(&ui->nodes[i]);
        if (t == NULL || t[1] != '\0')
            continue;
        if (t[0] == '1')
            n1++;
        else if (t[0] == '2')
            n2++;
        else if (t[0] == '3')
            n3++;
    }
    printf("节点 %d 个；徽标 1/2/3 的出现次数 = %d/%d/%d（都应为 1）\n", nodes,
           n1, n2, n3);

    const int rc = ex_run(ui, W, H, "QLGL · 39 pseudo multi-rule", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
