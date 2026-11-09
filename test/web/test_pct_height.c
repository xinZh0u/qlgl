/**
 * @file    test_pct_height.c
 * @brief   百分比高度的解析链：`height:100%` 只有在父级高度确定时才有解。
 *
 * 这条以前是错的（一律按基准 0 解析 → 0 高），代价是"套一层
 * `<div class=stage>` 就整页空白"，写页面的人只能把 `position:relative`
 * 硬塞在 `#root` 上绕开。
 */
#include "../ql_test.h"

typedef struct { int x, y, w, h; } Box;

/* 布局一份界面，回填指定节点的盒子（按文档顺序取下标） */
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

QL_TEST_BEGIN("test_pct_height")

    Box b[6];

    /* ---------- 1. 根级 100%：等于视口 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=o><div class=i></div></div></div>",
                          "#r { width: 100%; height: 100% }"
                          ".o { width: 100%; height: 100% }"
                          ".i { width: 100%; height: 100% }",
                          300, 200, b, 3));
    QL_CHECK_EQ(b[0].h, 200);
    QL_CHECK_EQ(b[1].h, 200);    /* 这一条以前是 0 */
    QL_CHECK_EQ(b[2].h, 200);    /* 再套一层也要能连锁解析 */

    /* ---------- 2. 父级 auto：百分比高度按 auto 处理 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=o><div class=i></div></div></div>",
                          "#r { width: 300px; height: 200px }"
                          ".o { width: 100% }"
                          ".i { width: 50%; height: 100% }",
                          300, 200, b, 3));
    QL_CHECK_EQ(b[1].w, 300);
    QL_CHECK_EQ(b[1].h, 0);      /* 父级没高度：内容空 → 0，与 CSS 一致 */
    QL_CHECK_EQ(b[2].h, 0);

    /* ---------- 3. 父级 px：50% 精确一半 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=o><div class=i></div></div></div>",
                          "#r { width: 300px; height: 200px }"
                          ".o { width: 100px; height: 80px }"
                          ".i { width: 10px; height: 50% }",
                          300, 200, b, 3));
    QL_CHECK_EQ(b[2].h, 40);

    /* ---------- 4. flex 容器：行方向子项 100% = 容器内容高 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=f><div class=c></div>"
                          "<div class=c></div></div></div>",
                          "#r { width: 300px; height: 200px }"
                          ".f { display: flex; width: 100%; height: 120px; gap: 10px }"
                          ".c { width: 40px; height: 100% }",
                          300, 200, b, 4));
    QL_CHECK_EQ(b[2].h, 120);
    QL_CHECK_EQ(b[3].h, 120);

    /* ---------- 5. flex 列方向：50% = 内容高的一半 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=f><div class=c></div></div></div>",
                          "#r { width: 300px; height: 200px }"
                          ".f { display: flex; flex-direction: column;"
                          "     width: 100px; height: 120px }"
                          ".c { width: 20px; height: 50% }",
                          300, 200, b, 3));
    QL_CHECK_EQ(b[2].h, 60);

    /* ---------- 6. 绝对定位：基准是包含块（padding box）高度 ---------- */
    QL_CHECK(layout_boxes("<div id=r><div class=o><div class=a></div></div></div>",
                          "#r { width: 300px; height: 200px }"
                          ".o { position: relative; width: 100px; height: 80px }"
                          ".a { position: absolute; left: 0px; top: 0px;"
                          "     width: 10px; height: 100% }",
                          300, 200, b, 3));
    QL_CHECK_EQ(b[2].h, 80);

QL_TEST_END()
