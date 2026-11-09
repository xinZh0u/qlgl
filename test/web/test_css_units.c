/**
 * @file    test_css_units.c
 * @brief   `em` / `rem` / `vw` / `vh` 与 `margin: auto`。
 *
 * 这几条是"照搬真实页面"最常撞上的语法，之前只支持 px / %：
 *
 *   em    相对**元素自己的字号**，在构建期折成 px（字号那时已确定）
 *   rem   相对**根元素字号**，要等整棵树走完再折（根的 font-size 也可能是 em）
 *   vw/vh 相对视口，只有布局期才知道 → 活到布局期解析
 *   auto  横向把剩余空间分掉（`margin: 0 auto` 居中）
 */
#include "../ql_test.h"

typedef struct { int x, y, w, h; } Box;

/* 布局一份界面并取若干节点的盒子；失败返回 0 */
static int layout_boxes(const char *html, const char *css, int vw, int vh, Box *out,
                        int want)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return 0;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return 0;
    }
    ql_ui_layout(ui, (ql_coord_t)vw, (ql_coord_t)vh);
    int ok = 1;
    for (int i = 0; i < want; i++) {
        if (i >= (int)ui->node_count) {
            ok = 0;
            break;
        }
        const QLRect r = ql_ui_node_rect(&ui->nodes[i]);
        out[i].x = r.x;
        out[i].y = r.y;
        out[i].w = r.w;
        out[i].h = r.h;
    }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return ok;
}

QL_TEST_BEGIN("test_css_units")

    Box b[4];
    const char *H = "<div id=r><div id=a></div></div>";

    /* ---------- 1. em：相对元素自己的字号 ---------- */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 200px; font-size: 16px }"
                             "#a { width: 2em; height: 1.5em }",
                          300, 200, b, 2));
    QL_CHECK_EQ(b[1].w, 32);      /* 2 × 16 */
    QL_CHECK_EQ(b[1].h, 24);      /* 1.5 × 16 */

    /* 字号自己写 em 时，宽度要跟着新字号走（20px → 2em = 40） */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 200px; font-size: 10px }"
                             "#a { font-size: 20px; width: 2em; height: 1em }",
                          300, 200, b, 2));
    QL_CHECK_EQ(b[1].w, 40);
    QL_CHECK_EQ(b[1].h, 20);

    /* ---------- 2. rem：相对根元素字号，不受自身字号影响 ---------- */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 200px; font-size: 10px }"
                             "#a { font-size: 40px; width: 2rem; height: 1rem }",
                          300, 200, b, 2));
    QL_CHECK_EQ(b[1].w, 20);      /* 2 × 10（不是 2 × 40） */
    QL_CHECK_EQ(b[1].h, 10);

    /* ---------- 3. vw / vh：相对视口，跟着窗口变 ---------- */
    QL_CHECK(layout_boxes(H, "#r { width: 100%; height: 100% }"
                             "#a { width: 50vw; height: 25vh }",
                          400, 200, b, 2));
    QL_CHECK_EQ(b[1].w, 200);
    QL_CHECK_EQ(b[1].h, 50);
    /* 换视口：同一份界面要跟着变 */
    QL_CHECK(layout_boxes(H, "#r { width: 100%; height: 100% }"
                             "#a { width: 50vw; height: 25vh }",
                          200, 400, b, 2));
    QL_CHECK_EQ(b[1].w, 100);
    QL_CHECK_EQ(b[1].h, 100);

    /* ---------- 4. inset 也吃同一套单位 ---------- */
    QL_CHECK(layout_boxes(H, "#r { position: relative; width: 300px; height: 200px;"
                             "     font-size: 20px }"
                             "#a { position: absolute; left: 1em; top: 10vh;"
                             "     width: 10px; height: 10px }",
                          300, 200, b, 2));
    QL_CHECK_EQ(b[1].x, 20);      /* 1em @ 20px */
    QL_CHECK_EQ(b[1].y, 20);      /* 10vh @ 200px */

    /* ---------- 5. margin / padding 的 em ---------- */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 200px; font-size: 16px }"
                             "#a { margin: 1em 2em; padding: 0.5em;"
                             "     width: 10px; height: 10px }",
                          300, 200, b, 2));
    QL_CHECK_EQ(b[1].x, 32);      /* margin-left = 2em = 32 */
    QL_CHECK_EQ(b[1].y, 16);      /* margin-top = 1em = 16 */
    QL_CHECK_EQ(b[1].w, 10 + 16); /* padding 0.5em 两侧 = 8+8（content-box） */

    /* ---------- 6. margin: auto 居中 ---------- */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 100px }"
                             "#a { width: 100px; height: 20px; margin: 0 auto }",
                          300, 100, b, 2));
    QL_CHECK_EQ(b[1].x, 100);
    QL_CHECK_EQ(b[1].w, 100);

    /* 只有左边 auto：靠右 */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 100px }"
                             "#a { width: 100px; height: 20px; margin: 0 0 0 auto }",
                          300, 100, b, 2));
    QL_CHECK_EQ(b[1].x, 200);

    /* 带 padding：居中算的是边框盒 */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 100px }"
                             "#a { width: 100px; padding: 10px; height: 20px;"
                             "     margin: 0 auto }",
                          300, 100, b, 2));
    QL_CHECK_EQ(b[1].x, 90);
    QL_CHECK_EQ(b[1].w, 120);

    /* 比容器还宽：剩余为 0，不出现负边距 */
    QL_CHECK(layout_boxes(H, "#r { width: 200px; height: 100px }"
                             "#a { width: 300px; height: 20px; margin: 0 auto }",
                          200, 100, b, 2));
    QL_CHECK_EQ(b[1].x, 0);

    /* 普通 margin 不受影响 */
    QL_CHECK(layout_boxes(H, "#r { width: 300px; height: 100px }"
                             "#a { width: 100px; height: 20px; margin: 10px }",
                          300, 100, b, 2));
    QL_CHECK_EQ(b[1].x, 10);

QL_TEST_END()
