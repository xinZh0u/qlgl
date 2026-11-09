/**
 * @file    main.c
 * @brief   CSS 绘制效果：渐变 / 圆角 / 阴影 / 透明度 / 毛玻璃 / 变换 / 动画 / 文本样式。
 *
 *   ./build/example/example_18_css_paint
 *
 * 每一格只放一种效果，格子上方写着"应该看到什么"。
 * 毛玻璃那一格盖在一条**会动的跑道**上 —— 方块在动，玻璃里的模糊要跟着变。
 */
#include "common.h"

#define W 660
#define H 430

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

    int glass = 0, anim = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUIStyle *s = ql_ui_node_style(&ui->nodes[i]);
        if (s->backdrop_blur > 0)
            glass++;
        if (s->opacity < 255)
            anim++;
    }
    printf("节点 %d 个：带 backdrop-filter 的 %d 个，带 opacity 的 %d 个\n",
           (int)ui->node_count, glass, anim);

    const int rc = ex_run(ui, W, H, "QLGL · 18 css paint", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
