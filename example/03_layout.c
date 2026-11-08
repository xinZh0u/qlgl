/**
 * @file    03_layout.c
 * @brief   布局：flex 是主力，position 管叠加。
 *
 *   ./build/example/03_layout --dump /tmp/03.ppm
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

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"bar\">"
    "    <div class=\"logo\">QLGL</div>"
    "    <div class=\"spacer\"></div>"
    "    <div class=\"btn\">登录</div>"
    "  </div>"
    "  <div class=\"cards\">"
    "    <div class=\"card a\"></div>"
    "    <div class=\"card b\"></div>"
    "    <div class=\"card c\"></div>"
    "  </div>"
    "  <div class=\"half\"></div>"
    "  <div class=\"badge\">绝对定位</div>"
    "</div>";

static const char *k_css =
    "#root { width: 300px; height: 220px; background-color: #0C1017; }"

    /* 顶栏：横向 flex，spacer 用 flex-grow 把按钮顶到右边 */
    ".bar { display: flex; align-items: center; gap: 8px;"
    "       margin: 10px 10px 0 10px; height: 32px; }"
    ".logo { font-size: 15px; color: #7CE0FF; }"
    ".spacer { flex-grow: 1; }"
    ".btn { width: 56px; height: 26px; border-radius: 13px;"
    "       background-color: #2E6BE6; color: #FFFFFF; font-size: 12px;"
    "       text-align: center; }"

    /* 三张卡：等宽 + 间距 */
    ".cards { display: flex; gap: 8px; margin: 10px; }"
    ".card { width: 80px; height: 60px; border-radius: 8px; }"
    ".a { background-color: #E8743B; }"
    ".b { background-color: #2FB8A6; }"
    ".c { background-color: #E6C34A; }"

    /* 百分比宽度：按父容器 300px 算，一半是 150px */
    ".half { margin: 0 10px; width: 50%; height: 24px;"
    "        background-color: #7A5AF8; border-radius: 6px; }"

    /* 绝对定位：相对最近的 relative/absolute/fixed 祖先（这里是 #root） */
    ".badge { position: absolute; left: 200px; top: 170px;"
    "         width: 86px; height: 24px; border-radius: 12px;"
    "         background-color: #1E2A3A; color: #C8D2DE; font-size: 11px;"
    "         text-align: center; }";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_load_font();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
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
