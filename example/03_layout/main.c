/**
 * @file    main.c
 * @brief   布局：flex 是主力，position 管叠加。
 *
 *   ./build/example/example_03_layout --dump /tmp/03.ppm
 *
 * 布局规则（和浏览器一致的部分）：
 *   - `display: flex` 的容器按 `flex-direction` 排子元素；
 *   - `justify-content` 管主轴，`align-items` 管交叉轴；
 *   - `flex-grow` 是"剩余空间按比例分"；
 *   - `gap` 是子元素之间的间距（不占两端）；
 *   - `width: 50%` 按**父容器宽度**算。
 * 布局单位一律是 CSS px（整数），高 DPI 由绘制后端乘缩放。
 */
#include "common.h"

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
    ql_ui_layout(ui, 300, 220);

    /* 布局结果是可以直接读的：每个节点的 border_box */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLRect b = ql_ui_node_rect(&ui->nodes[i]);
        printf("  #%u  %4d,%-4d %3dx%-3d\n", (unsigned)i, (int)b.x, (int)b.y,
               (int)b.w, (int)b.h);
    }

    const int rc = ex_run(ui, 300, 220, "QLGL · 03 layout", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
