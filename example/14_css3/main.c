/**
 * @file    main.c
 * @brief   CSS3 补上的那几件"真实页面天天用"的东西。
 *
 *   ./build/example/example_14_css3
 *
 * 这一版补的是（对应《CSS3 覆盖与裁剪》的 B2 + B1 第一片）：
 *
 *   ::before / content   图标、装饰、前缀 —— 构建期展开成真节点
 *   :nth-child(2n)       斑马纹，不用手工加 class
 *   [attr] / [attr=v]    按数据属性出样式
 *   :not()               排除某几条
 *   :first-child         首行/末行特殊处理
 *   min-width/max-width  列表卡片在窄窗口里不再被压扁，宽窗口里不再拉太长
 *   !important           兜底覆盖
 *   hsl()                配色不用手算十六进制
 *
 * 注意 **没有 JS**：所有交互都是 CSS 状态样式 + @keyframes。
 */
#include "common.h"

#define W 420
#define H 460

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
     * 看一眼"构建期展开"的结果：`::before` 的圆点/星号已经变成真节点，
     * 所以节点数比 HTML 里写的元素多。这就是"零运行期成本"的证据。
     */
    int pseudo = 0;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ql_ui_node_text(&ui->nodes[i]) != NULL &&
            (unsigned char)ql_ui_node_text(&ui->nodes[i])[0] == 0xE2)  /* "•" 首字节 */
            pseudo++;
    printf("节点 %d 个（其中 %d 个是 ::before 生成的）\n",
           (int)ui->node_count, pseudo);

    const int rc = ex_run(ui, W, H, "QLGL · 14 css3", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
