/**
 * @file    test_web_ui.c
 * @brief   示例：构建期把 HTML+CSS 编译成 C，运行期物化 + 布局 + 绘制。
 *
 * 被测页面：test/web/ui_demo.html + ui_demo.css
 * 生成物：build/ui/ui_demo.c（由 tools/qlui 生成）
 */
#include "../ql_test.h"
#include "ui_demo.h"          /* extern const QLUIDoc ui_demo_doc; */

static int g_clicked;
static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user_data)
{
    QL_UNUSED(ui);
    QL_UNUSED(node);
    (*(int *)user_data)++;
}

/*
 * demo 页的 .btn 带 180ms transition，所以状态切换后样式是"渐变中"。
 * 断言"最终样式"之前先把过渡一次跑完（真实主循环就是每帧 ql_ui_anim_tick）。
 */
static void settle(QLUIRoot *ui)
{
    (void)ql_ui_anim_tick(ui, ql_anim_now_ms() + 5000u);
}

QL_TEST_BEGIN("test_web_ui")

    /* 1. 文档内容符合预期：14 个节点（section + 3 div + 5 p + 4 span） */
    QL_CHECK_EQ(ui_demo_doc.node_count, 24);   /* M3 示例页：flex + 混合内容 */
    QL_CHECK(ui_demo_doc.nodes != NULL);
    QL_CHECK(ui_demo_doc.styles != NULL);
    QL_CHECK_STR(ui_demo_doc.name, "ui_demo");

    /* 2. 物化 */
    QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
    QL_CHECK(ui != NULL);
    QL_CHECK_EQ(ui->node_count, ui_demo_doc.node_count);
    QLUINodeRT *root = ql_ui_root_node(ui);
    QL_CHECK(root != NULL);
    QL_CHECK(root->src->tag == QL_UI_TAG_DIV);
    QL_CHECK(root->first_child != NULL);
    QL_CHECK(root->first_child->parent == root);

    /* 3. 布局：根节点 100% 宽，撑满视口 */
    ql_ui_layout(ui, 320, 240);
    QLRect rr = ql_ui_node_rect(root);
    QL_CHECK_EQ(rr.x, 0);
    QL_CHECK_EQ(rr.y, 0);
    QL_CHECK_EQ(rr.w, 320);
    QL_CHECK(rr.h > 0);

    /* 4. 子节点在父内容盒内：父有 12px padding */
    QLUINodeRT *bar = root->first_child;
    QL_CHECK(bar != NULL);
    QLRect bar_rect = ql_ui_node_rect(bar);
    QL_CHECK_EQ(bar_rect.x, 12);
    QL_CHECK_EQ(bar_rect.y, 12);
    QL_CHECK_EQ(bar_rect.w, 320 - 24);

    /* 5. 百分比宽度：50% 的盒子 = 父内容宽的一半 */
    QLUINodeRT *pct = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const char *t = ql_ui_node_text(&ui->nodes[i]);
        if (strcmp(t, "50% width box") == 0) {
            pct = &ui->nodes[i];
            break;
        }
    }
    QL_CHECK(pct != NULL);
    QLUINodeRT *panel = pct->parent;
    QL_CHECK(panel != NULL);
    QLRect panel_rect = ql_ui_node_rect(panel);
    QLRect pct_rect = ql_ui_node_rect(pct);
    /* panel 有 8px padding + 1px 边框 */
    ql_coord_t panel_content = panel_rect.w - 2 * 1 - 2 * 8;
    QL_CHECK_EQ(pct_rect.w, panel_content / 2);

    /* 6. 文本被正确带出来（编译期折叠进文本池） */
    QLUINodeRT *title = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_node_text(&ui->nodes[i]), "QLGL UI") == 0) {
            title = &ui->nodes[i];
            break;
        }
    }
    QL_CHECK(title != NULL);
    QL_CHECK_STR(ql_ui_node_text(title), "QLGL UI");

    /* 7. 字号映射：16px → mono_16，12px → mono_12 */
    QL_CHECK(ql_ui_node_font(title) == &ql_font_mono_16);
    QL_CHECK(ql_ui_node_font(pct) == &ql_font_mono_12);

    /* 8. 标签查找：页面里只有 span，没有 button */
    QL_CHECK(ql_ui_find_by_tag(ui, QL_UI_TAG_BUTTON, 0) == NULL);
    QL_CHECK(ql_ui_find_by_tag(ui, QL_UI_TAG_SPAN, 0) != NULL);
    QL_CHECK_STR(ql_ui_tag_name(QL_UI_TAG_BUTTON), "button");

    /* 9. 命中测试：点在标题条上应命中它自己或其后代 */
    QLUINodeRT *hit = ql_ui_hit_test(ui, 20, 20);
    QL_CHECK(hit != NULL);
    if (hit != NULL)
        QL_CHECK(hit->src->tag == QL_UI_TAG_P || hit->src->tag == QL_UI_TAG_DIV ||
                 hit->src->tag == QL_UI_TAG_SECTION);

    /* 10. 渲染到内存显示并做像素断言 */
    QLDispDriver *disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    ql_disp_set_bg_color(QL_COLOR_RGB(0x10, 0x14, 0x1C));

    QLCanvas canvas;
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
    ql_canvas_clear(&canvas, QL_COLOR_RGB(0x10, 0x14, 0x1C));
    ql_ui_render_canvas(ui, &canvas);

    /* 标题条是 #3399ff */
    ql_color_t bar_px = ql_disp_mem_pixel(disp, 20, 16);
    QL_CHECK(QL_COLOR_GET_B(bar_px) > 200);      /* 蓝色分量明显 */
    /* 主按钮是 #3cc07a（绿色）：按文本找节点 */
    QLUINodeRT *btn = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_node_text(&ui->nodes[i]), "Primary") == 0) {
            btn = &ui->nodes[i];
            break;
        }
    }
    QL_CHECK(btn != NULL);
    QLRect br = ql_ui_node_rect(btn);
    QL_CHECK(!ql_rect_is_empty(br));
    if (!ql_rect_is_empty(br)) {
        ql_color_t bp = ql_disp_mem_pixel(disp, br.x + 4, br.y + br.h / 2);
        QL_CHECK(QL_COLOR_GET_G(bp) > 100);
    }

    /* 11. 文本绘制确实产生了非背景像素 */
    int inked = 0;
    for (ql_coord_t y = 12; y < 24; y++)
        for (ql_coord_t x = 20; x < 90; x++)
            if (ql_disp_mem_pixel(disp, x, y) == QL_COLOR_RGB(0x0C, 0x1A, 0x2A))
                inked++;
    QL_CHECK(inked > 0);

    /* 12. 交互：hover / active 状态位 + 点击回调 */
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 320 + 50, 240 + 50), 0);   /* 屏幕外 */
    QL_CHECK(ql_ui_pointer_down(ui, br.x + 2, br.y + 2) == 1);
    QL_CHECK((btn->flags & QL_UI_RT_ACTIVE) != 0);

    g_clicked = 0;
    ql_ui_set_click_cb(ui, on_click, &g_clicked);
    QL_CHECK_EQ(ql_ui_pointer_up(ui, br.x + 2, br.y + 2), 1);
    QL_CHECK_EQ(g_clicked, 1);
    QL_CHECK((btn->flags & QL_UI_RT_ACTIVE) == 0);

    /* 13. 运行期改文本 */
    QL_CHECK_EQ(ql_ui_set_text(ui, title, "Changed"), QL_OK);
    QL_CHECK_STR(ql_ui_node_text(title), "Changed");
    ql_ui_layout(ui, 320, 240);
    ql_ui_render_canvas(ui, &canvas);

    /* 14. 伪类状态样式：构建期已经编译成独立样式，运行期只切下标 */
    QLUINodeRT *primary = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_node_text(&ui->nodes[i]), "Primary") == 0)
            primary = &ui->nodes[i];
    }
    QL_CHECK(primary != NULL);
    ql_ui_layout(ui, 320, 240);

    /* .primary 现在用 linear-gradient，bg_color 是渐变起始色，终止色在 bg_color2 */
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_color, QL_COLOR_RGB(0x4A, 0xD0, 0x8A));
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_color2, QL_COLOR_RGB(0x2A, 0x9A, 0x5C));
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_grad_dir, 1);   /* 1 = 垂直 */
    QL_CHECK(ql_ui_node_style(primary)->shadow_alpha > 0);     /* 有阴影 */

    QLRect pr = ql_ui_node_rect(primary);
    QL_CHECK_EQ(ql_ui_pointer_move(ui, pr.x + 3, pr.y + 3), 1);
    QL_CHECK((primary->flags & QL_UI_RT_HOVER) != 0);
    settle(ui);   /* 过渡跑完再看最终样式 */
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_color, QL_COLOR_RGB(0x4A, 0xD0, 0x8A));
    /* 祖先链也带 hover（与 Web 一致） */
    QL_CHECK((primary->parent->flags & QL_UI_RT_HOVER) != 0);

    QL_CHECK_EQ(ql_ui_pointer_move(ui, pr.x + 3, pr.y + 3), 0);   /* 同一目标不重复变脏 */

    QL_CHECK_EQ(ql_ui_pointer_down(ui, pr.x + 3, pr.y + 3), 1);
    settle(ui);
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_color, QL_COLOR_RGB(0x2A, 0x9A, 0x5C));
    ql_ui_pointer_up(ui, pr.x + 3, pr.y + 3);
    settle(ui);
    QL_CHECK_EQ(ql_ui_node_style(primary)->bg_color, QL_COLOR_RGB(0x4A, 0xD0, 0x8A));

    ql_ui_set_focus_node(ui, primary);
    QL_CHECK(ql_ui_focus_node(ui) == primary);
    settle(ui);
    QL_CHECK_EQ(ql_ui_node_style(primary)->border_color, QL_COLOR_RGB(0xFF, 0xD0, 0x60));
    ql_ui_set_focus_node(ui, NULL);
    settle(ui);
    QL_CHECK_EQ(ql_ui_node_style(primary)->border_color, QL_COLOR_RGB(0xE0, 0xFF, 0xE0));  /* 回落到 :hover */

    /* 15. 视口变窄：根节点宽度跟着变，百分比仍按父内容宽计算 */
    ql_ui_layout(ui, 200, 160);
    QL_CHECK_EQ(ql_ui_node_rect(root).w, 200);

    ql_ui_free(ui);
    ql_disp_mem_destroy(disp);

QL_TEST_END()
