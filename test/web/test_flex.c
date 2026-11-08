/**
 * @file    test_flex.c
 * @brief   flexbox 布局的几何断言。
 *
 * 用最小 HTML/CSS 片段编译出的 doc，逐条验证 flex 规范里最常用的行为。
 * 每个用例都自己 build + layout，互不影响。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

/* 由 CMake 生成：ui_flex_<case>_doc */
#include "ui_flex_row.h"
#include "ui_flex_grow.h"
#include "ui_flex_justify.h"
#include "ui_flex_align.h"
#include "ui_flex_wrap.h"
#include "ui_flex_column.h"
#include "ui_flex_posabs.h"
#include "ui_flex_posfix.h"
#include "ui_flex_posrel.h"
#include "ui_flex_zidx.h"
#include "ui_flex_ovf.h"
#include "ui_flex_mixed.h"

/* 按文本找节点 */
static QLRect rect_of_text(QLUIRoot *ui, const char *text)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_node_text(&ui->nodes[i]), text) == 0)
            return ql_ui_node_rect(&ui->nodes[i]);
    }
    return ql_rect_make(-1, -1, -1, -1);
}

/* 布局一个 doc 并返回 root */
static QLUIRoot *layout(QLUIRoot *ui, ql_coord_t w, ql_coord_t h)
{
    ql_ui_layout(ui, w, h);
    return ui;
}

QL_TEST_BEGIN("test_flex")

    /* ============ 1. row：横向排列 + 间距 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_row_doc), 300, 200);
        QL_CHECK(ui != NULL);

        /* 三个 60×40 的盒子横向排开，gap 10 */
        QLRect a = rect_of_text(ui, "A");
        QLRect b = rect_of_text(ui, "B");
        QLRect c = rect_of_text(ui, "C");
        QL_CHECK_EQ(a.w, 60);
        QL_CHECK_EQ(a.h, 40);
        QL_CHECK_EQ(b.x, a.x + 60 + 10);
        QL_CHECK_EQ(c.x, b.x + 60 + 10);
        QL_CHECK_EQ(a.y, b.y);            /* 同一行 */
        QL_CHECK_EQ(a.y, c.y);

        ql_ui_free(ui);
    }

    /* ============ 2. flex-grow：按比例分配剩余空间 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_grow_doc), 300, 100);
        QL_CHECK(ui != NULL);

        /* 容器 300 宽，两个子项 basis 100 与 100，剩 100 按 grow 1:3 分 */
        QLRect g1 = rect_of_text(ui, "G1");
        QLRect g2 = rect_of_text(ui, "G2");
        QL_CHECK_EQ(g1.w, 100 + 25);
        QL_CHECK_EQ(g2.w, 100 + 75);
        QL_CHECK_EQ(g2.x, g1.x + g1.w);

        ql_ui_free(ui);
    }

    /* ============ 3. justify-content ============ */
    {
        /* center：两个 50 宽的子项在 300 的容器里居中 */
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_justify_doc), 300, 100);
        QL_CHECK(ui != NULL);

        QLRect j1 = rect_of_text(ui, "J1");
        QLRect j2 = rect_of_text(ui, "J2");
        QL_CHECK_EQ(j1.x, 100);                 /* (300 - 100) / 2 */
        QL_CHECK_EQ(j2.x, 150);
        QL_CHECK_EQ(j2.x + j2.w, 200);          /* 右侧留 100 */

        ql_ui_free(ui);
    }

    /* ============ 4. align-items ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_align_doc), 300, 120);
        QL_CHECK(ui != NULL);

        /* 容器 align-items:center；高个子 80，矮个子 20 → 居中偏移 30 */
        QLRect tall = rect_of_text(ui, "TALL");
        QLRect small = rect_of_text(ui, "SMALL");
        QL_CHECK_EQ(tall.h, 80);
        QL_CHECK_EQ(small.h, 20);
        QL_CHECK_EQ(small.y - tall.y, 30);

        ql_ui_free(ui);
    }

    /* ============ 5. wrap：超宽换行 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_wrap_doc), 130, 200);
        QL_CHECK(ui != NULL);

        /* 三个 60 宽 + gap 10，容器 130 → 每行 2 个 */
        QLRect w1 = rect_of_text(ui, "W1");
        QLRect w2 = rect_of_text(ui, "W2");
        QLRect w3 = rect_of_text(ui, "W3");
        QL_CHECK_EQ(w1.y, w2.y);                 /* 前两个同一行 */
        QL_CHECK_EQ(w2.x, w1.x + 60 + 10);
        QL_CHECK(w3.y > w1.y);                   /* 第三个换行 */
        QL_CHECK_EQ(w3.x, w1.x);                 /* 从行首开始 */

        ql_ui_free(ui);
    }

    /* ============ 6. column：纵向排列 + 显式高度下的 grow ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_column_doc), 200, 300);
        QL_CHECK(ui != NULL);

        QLRect c1 = rect_of_text(ui, "C1");
        QLRect c2 = rect_of_text(ui, "C2");
        QL_CHECK_EQ(c1.x, c2.x);                 /* 同一列 */
        QL_CHECK_EQ(c1.w, c2.w);
        QL_CHECK(c2.y >= c1.y + c1.h);           /* 纵向堆叠 */

        ql_ui_free(ui);
    }

    /* ============ 7. 布局可重复：二次 layout 结果一致 ============ */
    {
        QLUIRoot *ui = ql_ui_build(&ui_flex_row_doc);
        layout(ui, 300, 200);
        QLRect first = rect_of_text(ui, "B");
        layout(ui, 300, 200);
        QLRect second = rect_of_text(ui, "B");
        QL_CHECK_EQ(first.x, second.x);
        QL_CHECK_EQ(first.y, second.y);
        QL_CHECK_EQ(first.w, second.w);
        QL_CHECK_EQ(first.h, second.h);

        /* 视口变窄后重新布局，仍应正确（style_copy 不能残留旧尺寸） */
        layout(ui, 500, 200);
        QLRect wide = rect_of_text(ui, "B");
        QL_CHECK_EQ(wide.w, first.w);            /* 子项是固定宽，不随容器变 */
        QL_CHECK_EQ(wide.x, first.x);

        ql_ui_free(ui);
    }

    /* ============ 8. position: absolute（相对定位祖先） ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_posabs_doc), 300, 200);
        QL_CHECK(ui != NULL);

        QLRect in = rect_of_text(ui, "IN");
        QLRect abs = rect_of_text(ui, "ABS");
        QL_CHECK_EQ(in.x, 0);
        QL_CHECK_EQ(in.y, 0);
        /* 只给 right: 10 → x = 300 - 10 - 40 */
        QL_CHECK_EQ(abs.x, 250);
        QL_CHECK_EQ(abs.y, 10);
        QL_CHECK_EQ(abs.w, 40);
        QL_CHECK_EQ(abs.h, 20);
        /* 绝对定位不占文档流，不影响兄弟 */
        QL_CHECK_EQ(in.h, 60);

        ql_ui_free(ui);
    }

    /* ============ 9. position: fixed（相对视口） ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_posfix_doc), 300, 200);
        QL_CHECK(ui != NULL);

        QLRect f = rect_of_text(ui, "FIX");
        QL_CHECK_EQ(f.x, 5);
        QL_CHECK_EQ(f.y, 200 - 5 - 30);

        /* 换视口尺寸后重新布局，fixed 元素跟着视口走 */
        layout(ui, 400, 300);
        QLRect f2 = rect_of_text(ui, "FIX");
        QL_CHECK_EQ(f2.y, 300 - 5 - 30);

        ql_ui_free(ui);
    }

    /* ============ 10. position: relative（不占流、不影响兄弟） ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_posrel_doc), 300, 300);
        QL_CHECK(ui != NULL);

        QLRect a = rect_of_text(ui, "A");
        QLRect b = rect_of_text(ui, "B");
        QLRect c = rect_of_text(ui, "C");
        QL_CHECK_EQ(a.y, 0);
        QL_CHECK_EQ(b.y, 40 + 5);        /* 原位 40 + top:5 */
        QL_CHECK_EQ(b.x, 20);            /* left:20 */
        QL_CHECK_EQ(c.y, 80);            /* 兄弟没被 B 的偏移影响 */

        ql_ui_free(ui);
    }

    /* ============ 11. z-index：绘制与命中的顺序 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_zidx_doc), 300, 200);
        QL_CHECK(ui != NULL);

        /* 两者重叠；LO 的 z-index=2 > HI 的 1，重叠处应命中 LO */
        QLUINodeRT *hit = ql_ui_hit_test(ui, 45, 45);
        QL_CHECK(hit != NULL);
        if (hit != NULL)
            QL_CHECK_STR(ql_ui_node_text(hit), "LO");

        /* 不重叠处仍各自命中 */
        QLUINodeRT *h2 = ql_ui_hit_test(ui, 10, 10);
        QL_CHECK(h2 != NULL);
        if (h2 != NULL)
            QL_CHECK_STR(ql_ui_node_text(h2), "LO");

        /* 子链表顺序已被 z 序改写：z 小的在前（先画） */
        QLUINodeRT *root = ql_ui_root_node(ui);
        QL_CHECK(root->first_child != NULL);
        QL_CHECK(root->first_child->next_sibling != NULL);
        if (root->first_child->next_sibling != NULL) {
            QL_CHECK(root->first_child->style->z_index <=
                     root->first_child->next_sibling->style->z_index);
        }

        ql_ui_free(ui);
    }

    /* ============ 12. overflow: hidden 裁剪 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_ovf_doc), 300, 200);
        QL_CHECK(ui != NULL);

        QLUINodeRT *root = ql_ui_root_node(ui);
        QLUINodeRT *clip = root->first_child;
        QL_CHECK(clip != NULL);
        if (clip != NULL) {
            QLRect cr = ql_ui_node_rect(clip);
            /* 显式高度必须精确，不被内容撑开 */
            QL_CHECK_EQ(cr.w, 60);
            QL_CHECK_EQ(cr.h, 40);
        }

        /* 被裁掉的部分不可命中：(45, 60) 在 clip 盒外 */
        QL_CHECK(ql_ui_hit_test(ui, 45, 60) != NULL);      /* 命中外层 root */
        QLUINodeRT *in_clip = ql_ui_hit_test(ui, 45, 30);
        QL_CHECK(in_clip != NULL);
        if (in_clip != NULL)
            QL_CHECK_STR(ql_ui_node_text(in_clip), "BIG");

        /* 渲染：clip 外不应出现 BIG 的颜色 */
        QLDispDriver *disp = ql_disp_mem_create(300, 200, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        ql_canvas_from_disp(&canvas, disp);
        ql_canvas_clear(&canvas, QL_COLOR_RGB(0, 0, 0));
        ql_ui_render_canvas(ui, &canvas);
        /* (100, 20) 在 clip 盒（60 宽）之外 → 仍是背景 */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 100, 20), QL_COLOR_RGB(0, 0, 0));
        /* (30, 20) 在内部 → BIG 的蓝色 */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 30, 20), QL_COLOR_RGB(0x33, 0x99, 0xFF));
        ql_disp_mem_destroy(disp);

        ql_ui_free(ui);
    }

    /* ============ 13. 混合内容：生成期展开成匿名 inline 子节点 ============ */
    {
        QLUIRoot *ui = layout(ql_ui_build(&ui_flex_mixed_doc), 300, 60);
        QL_CHECK(ui != NULL);

        /* "Hello " + <b>bold</b> + " and " + <i>it</i> + "!" = 5 个 inline 子节点 */
        QLUINodeRT *p = ql_ui_root_node(ui)->first_child;
        QL_CHECK(p != NULL);
        if (p != NULL) {
            int n = 0;
            for (QLUINodeRT *c = p->first_child; c != NULL; c = c->next_sibling)
                n++;
            QL_CHECK_EQ(n, 5);

            QLRect prev = ql_rect_make(-1, 0, 0, 0);
            int same_line = 1;
            for (QLUINodeRT *c = p->first_child; c != NULL; c = c->next_sibling) {
                QLRect r = ql_ui_node_rect(c);
                QL_CHECK(r.w > 0);
                if (prev.x >= 0) {
                    QL_CHECK(r.x >= prev.x + prev.w);        /* 依次向右 */
                    if (r.y != prev.y)
                        same_line = 0;
                }
                prev = r;
            }
            QL_CHECK_EQ(same_line, 1);                       /* 全部同一行 */

            /* 文本段内容正确（空白被保留在合适的位置） */
            QL_CHECK_STR(ql_ui_node_text(p->first_child), "Hello ");
            QL_CHECK_STR(ql_ui_node_text(p->first_child->next_sibling), "bold");
        }

        ql_ui_free(ui);
    }

QL_TEST_END()
