/**
 * @file    test_line_height.c
 * @brief   `line-height`：单行文字垂直居中的唯一正解。
 *
 * 背景（真实场景）：`height: 26px; text-align: center` 的按钮，文字水平居中但
 * **贴着内容盒顶部**。这不是 bug —— CSS 块布局本来就这样，浏览器里也一样。
 * 要垂直居中只能写 `line-height: 26px`。本库之前没有这个属性，于是"按钮里的字
 * 对不齐"根本没法表达。
 *
 * 判据一律用**渲染出来的墨迹（ink）包围盒**，不看结构体：
 * 只看 `style.line_h` 等于几，测不出"排版有没有真的用它"。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 120

static QLDispDriver *s_disp;
static QLCanvas s_cv;

/* 深底白字，便于按亮度分离墨迹与底色 */
static const ql_color_t BG = QL_COLOR_RGB(0x10, 0x10, 0x10);

/* 渲染一次，返回"亮的像素"（= 字形墨迹）的包围盒；没有墨迹时返回 0 */
static int ink_box(const char *html, const char *css, QLRect *out)
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
    ql_ui_layout(ui, W, H);
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, BG);
    ql_ui_render_canvas(ui, &s_cv);

    ql_coord_t x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (ql_coord_t y = 0; y < H; y++) {
        for (ql_coord_t x = 0; x < W; x++) {
            const ql_color_t c = ql_disp_mem_pixel(s_disp, x, y);
            if (QL_COLOR_GET_R(c) > 0x80 && QL_COLOR_GET_G(c) > 0x80 &&
                QL_COLOR_GET_B(c) > 0x80) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    if (x1 < 0)
        return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0 + 1;
    out->h = y1 - y0 + 1;
    return 1;
}

/* 墨迹中心的垂直位置，用 2 倍值避免半像素比较 */
static int ink_center_y2(const QLRect *ink)
{
    return 2 * (int)ink->y + (int)ink->h;
}

QL_TEST_BEGIN("test_line_height")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    const char *html = "<div id=r>Hi</div>";

    /* ---------- 1. 不给 line-height：贴顶部（旧行为，作为对照） ---------- */
    {
        const char *css = "#r { width: 120px; height: 40px; font-size: 12px;"
                          " color: #FFFFFF; text-align: center; }";
        QLRect ink;
        QL_CHECK(ink_box(html, css, &ink) == 1);
        /* 墨迹顶边应当紧贴内容盒顶边（容差 3px：字形上方本来就有内部空白） */
        QL_CHECK(ink.y <= 3);
        QL_CHECK(ink_center_y2(&ink) < 40);          /* 明显在盒子中线之上 */
    }

    /* ---------- 2. line-height == height：垂直居中 ---------- */
    {
        const char *css = "#r { width: 120px; height: 40px; font-size: 12px;"
                          " color: #FFFFFF; text-align: center;"
                          " line-height: 40px; }";
        QLRect ink;
        QL_CHECK(ink_box(html, css, &ink) == 1);
        /* 墨迹中心 = 盒子中心（y=20），2 倍值相等，容差 1px */
        const int c = ink_center_y2(&ink);
        QL_CHECK(c >= 2 * 20 - 2 && c <= 2 * 20 + 2);
    }

    /* ---------- 3. 无单位倍数：按 font-size 折算 ---------- */
    {
        /* 20px 字体 × 2 = 40px 行高，与第 2 例等价 */
        const char *css = "#r { width: 120px; height: 40px; font-size: 20px;"
                          " color: #FFFFFF; text-align: center;"
                          " line-height: 2; }";
        QLRect a, b;
        /* 先量一次 20px 字体+40px 行高的 px 写法，两者墨迹中心应当一样 */
        const char *css_px = "#r { width: 120px; height: 40px; font-size: 20px;"
                             " color: #FFFFFF; text-align: center;"
                             " line-height: 40px; }";
        QL_CHECK(ink_box(html, css, &a) == 1);
        QL_CHECK(ink_box(html, css_px, &b) == 1);
        QL_CHECK_EQ(ink_center_y2(&a), ink_center_y2(&b));
    }

    /* ---------- 4. auto 高度用行高撑开内容盒 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r>Hi</div>",
                                       "#r { width: 120px; font-size: 12px;"
                                       " line-height: 30px; color: #FFFFFF; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[0]).h, 30);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 继承：子节点没写也吃父节点的行高 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r><div class=c>hi</div></div>",
                                       "#r { width: 120px; font-size: 12px;"
                                       " line-height: 30px; }"
                                       ".c { color: #FFFFFF; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK(doc->styles[1].line_h == 30);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. normal 显式写法 = 不设 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text(html,
                                       "#r { width: 120px; line-height: normal; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->styles[0].line_h, 0);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 明确报错 ---------- */
    {
        struct { const char *css; } bad[] = {
            { "#r { line-height: 1.5em }" },
            { "#r { line-height: 0 }" },
            { "#r { line-height: 40000px }" },
            { "#r { line-height: auto }" },
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text(html, bad[i].css);
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    /*
     * ---------- 8. `text-align` 也管行内子节点，不只管子节点里的文本 ----------
     *
     * 踩过的坑：`<div text-align:center><span class=label>x</span></div>`
     * 里的标签贴在左边 —— 因为行内盒的排布只按 cursor_x 累积，压根没看
     * 容器的 text-align。浏览器里 inline-level 子节点同样是水平对齐的对象。
     */
    {
        struct { const char *ta; int want_x0; } cases[] = {
            { "left",   0   },
            { "center", 80  },
            { "right",  160 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char css[256];
            snprintf(css, sizeof(css),
                     "#r { width: 200px; text-align: %s; }"
                     ".b { display: inline-block; width: 20px; height: 10px;"
                     " background-color: #FFFFFF; }", cases[i].ta);
            QLUIDoc *doc = ql_ui_load_text(
                "<div id=r><span class=b></span><span class=b></span></div>", css);
            QL_CHECK(doc != NULL);
            if (doc == NULL)
                continue;
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QLUINodeRT *n1 = ql_ui_root_node(ui)->first_child;
                QL_CHECK(n1 != NULL);
                if (n1 != NULL) {
                    const QLRect r1 = ql_ui_node_rect(n1);
                    const QLRect r2 = ql_ui_node_rect(n1->next_sibling);
                    QL_CHECK_EQ(r1.x, cases[i].want_x0);
                    QL_CHECK_EQ(r2.x, cases[i].want_x0 + 20);  /* 兄弟跟着一起走 */
                    QL_CHECK_EQ(r1.y, r2.y);                   /* 仍在同一行 */
                }
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. 换行时**每一行各自**按剩余空间居中 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=r><span class=b></span><span class=b></span>"
            "<span class=b></span></div>",
            "#r { width: 200px; text-align: center; }"
            ".b { display: inline-block; width: 90px; height: 10px;"
            " background-color: #FFFFFF; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QLUINodeRT *n = ql_ui_root_node(ui)->first_child;
                QL_CHECK(n != NULL);
                if (n != NULL) {
                    const QLRect r1 = ql_ui_node_rect(n);
                    const QLRect r2 = ql_ui_node_rect(n->next_sibling);
                    const QLRect r3 = ql_ui_node_rect(n->next_sibling->next_sibling);
                    QL_CHECK_EQ(r1.x, 10);    /* 一行两个，剩 20 分两半 */
                    QL_CHECK_EQ(r2.x, 100);
                    QL_CHECK_EQ(r3.x, 55);    /* 第二行只有一个，剩 110 分两半 */
                    QL_CHECK(r3.y > r1.y);    /* 真的换行了 */
                }
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
