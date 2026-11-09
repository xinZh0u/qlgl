/**
 * @file    test_sub_sup.c
 * @brief   TEXT 包 P4b/P4c：`<sub>` / `<sup>`（基线偏移）+ `font-family` 两级字体族。
 *
 * 这一期给 `QLUIStyle` 加了两个字节（`baseline_dy` / `font_family`），
 * 所以 blob 升到 v15。测试要守住三件事：
 *   1. 两个新字段的语义（上标负、下标正；monospace → family=1）；
 *   2. `<sub>`/`<sup>` 的字号是**相对父级**缩小的（`font-size: smaller`）；
 *   3. 绘制时基线真的挪了（像素级验证），而且**行盒高度不变**（与浏览器一致）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 300
#define H 200

static QLUIDoc *mk(const char *html, const char *css, QLUIRoot **out_ui)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, W, H);
    *out_ui = ui;
    return doc;
}

static QLUINodeRT *find_tag(QLUIRoot *ui, const char *name, int nth)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_tag_name(ui->nodes[i].src->tag), name) != 0)
            continue;
        if (nth-- == 0)
            return &ui->nodes[i];
    }
    return NULL;
}

/* 节点实际用的字号（px）。font_id 索引 doc->font_sizes */
static int font_px_of(const QLUIRoot *ui, const QLUINodeRT *n)
{
    if (n == NULL || ui->doc == NULL || ui->doc->font_sizes == NULL)
        return 0;
    if (n->style->font_id >= ui->doc->font_size_count)
        return 0;
    return (int)ui->doc->font_sizes[n->style->font_id];
}

static QLDispDriver *s_disp;

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, (ql_coord_t)x, (ql_coord_t)y);
}

/* 在 [x0,x1) × [y0,y1) 里有没有非底色像素 */
static int ink_in(QLRect box, ql_color_t bg)
{
    for (int y = box.y; y < box.y + box.h; y++)
        for (int x = box.x; x < box.x + box.w; x++)
            if (px(x, y) != bg)
                return 1;
    return 0;
}

QL_TEST_BEGIN("test_sub_sup")

    /* ---------- 1. 标签认识 + UA 默认 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r>a<sub>b</sub>c<sup>d</sup></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *sb = find_tag(ui, "sub", 0);
            QLUINodeRT *sp = find_tag(ui, "sup", 0);
            QL_CHECK(sb != NULL && sp != NULL);
            if (sb != NULL && sp != NULL) {
                QL_CHECK_EQ((int)sb->style->display, (int)QL_UI_DISPLAY_INLINE);
                QL_CHECK_EQ((int)sp->style->display, (int)QL_UI_DISPLAY_INLINE);
                QL_CHECK(sb->style->baseline_dy > 0);    /* 下标：基线往下 */
                QL_CHECK(sp->style->baseline_dy < 0);    /* 上标：基线往上 */
                /*
                 * 偏移按**元素自身**的字号算（16px 的 sub/sup 字号先缩到 13px）：
                 *   sub = +20% × 13 = +2，super = -33% × 13 = -4
                 */
                printf("    sub dy=%d sup dy=%d\n",
                       (int)sb->style->baseline_dy, (int)sp->style->baseline_dy);
                QL_CHECK_EQ((int)sb->style->baseline_dy, 2);
                QL_CHECK_EQ((int)sp->style->baseline_dy, -4);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. `font-size: smaller` 是相对父级 ---------- */
    {
        QLUIRoot *ui = NULL;
        /* 父级 24px → smaller 后 20px（24*5/6 = 20） */
        QLUIDoc *doc = mk("<div id=r><sub>x</sub></div>", "#r { font-size: 24px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *sb = find_tag(ui, "sub", 0);
            QL_CHECK_EQ(font_px_of(ui, sb), 20);
            /* 偏移跟着字号走：24 * 20% = 4 */
            QL_CHECK_EQ((int)sb->style->baseline_dy, 4);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 父级 12px → 10px（12*5/6 = 10） */
        ui = NULL;
        doc = mk("<div id=r><sub>x</sub></div>", "#r { font-size: 12px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *sb = find_tag(ui, "sub", 0);
            QL_CHECK_EQ(font_px_of(ui, sb), 10);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* larger：16 → 19（16*6/5 = 19.2 → 19） */
        ui = NULL;
        doc = mk("<div id=r><sup>x</sup></div>", "#r { font-size: 16px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        ui = NULL;
        doc = mk("<div id=r><span class=b>x</span></div>",
                 "#r { font-size: 16px } .b { font-size: larger }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *sp = find_tag(ui, "span", 0);
            QL_CHECK_EQ(font_px_of(ui, sp), 19);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 显式 vertical-align ---------- */
    {
        static const struct { const char *v; int dy; } k_c[] = {
            { "baseline", 0 }, { "super", -5 }, { "sub", 3 },
        };
        for (size_t i = 0; i < sizeof(k_c) / sizeof(k_c[0]); i++) {
            char css[96];
            snprintf(css, sizeof(css), "#r { vertical-align: %s }", k_c[i].v);
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk("<div id=r>x</div>", css, &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[0].style->baseline_dy, k_c[i].dy);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
        /* 不认的值明确报错 */
        const char *bad[] = { "#r { vertical-align: middle }",
                              "#r { vertical-align: top }" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", bad[i]);
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. font-family：两级，未知族名不报错 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><code>x</code><pre>y</pre></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *c = find_tag(ui, "code", 0);
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(c != NULL && p != NULL);
            if (c != NULL)
                QL_CHECK_EQ((int)c->style->font_family, (int)QL_UI_FAMILY_MONO);
            if (p != NULL)
                QL_CHECK_EQ((int)p->style->font_family, (int)QL_UI_FAMILY_MONO);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 未知族名落到正文，**不报错**（写 Arial 的页面不该编译不过） */
        ui = NULL;
        doc = mk("<div id=r>x</div>", "#r { font-family: Arial, sans-serif }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[0].style->font_family, (int)QL_UI_FAMILY_DEFAULT);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* `monospace` 出现在候选表里就取等宽 */
        ui = NULL;
        doc = mk("<div id=r>x</div>", "#r { font-family: Menlo, monospace }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[0].style->font_family, (int)QL_UI_FAMILY_MONO);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 行盒高度：上标不撑高（与浏览器一致） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=w><div id=a>x</div><div id=b>x<sup>2</sup></div></div>",
                          "#w { width: 200px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *a = find_tag(ui, "div", 1);
            QLUINodeRT *b = find_tag(ui, "div", 2);
            QL_CHECK(a != NULL && b != NULL);
            if (a != NULL && b != NULL) {
                /* 含上标的那一行比纯文本行高**不会更高**（可能因字号小而略矮） */
                QL_CHECK(b->border_box.h <= a->border_box.h);
                printf("    纯文本行高 %d，含上标行高 %d\n",
                       (int)a->border_box.h, (int)b->border_box.h);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 像素：`sup` 的字形比基线高、`sub` 的更低 ---------- */
    {
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            const ql_color_t bg = 0xFFFFFF;
            /* 三段同样的文字，只有 vertical-align 不同 */
            const char *html =
                "<div id=r>"
                "<span id=n>MMMM</span>"
                "<span id=u style=\"vertical-align: super\">MMMM</span>"
                "<span id=d style=\"vertical-align: sub\">MMMM</span>"
                "</div>";
            const char *css = "#r { width: 300px; background-color: #FFFFFF }"
                              " span { font-size: 16px; color: #000000 }";
            QLUIDoc *doc = ql_ui_load_text(html, css);
            QL_CHECK(doc != NULL);
            if (doc != NULL) {
                QLUIRoot *ui = ql_ui_build(doc);
                QL_CHECK(ui != NULL);
                if (ui != NULL) {
                    ql_ui_layout(ui, W, H);
                    QLCanvas cv;
                    QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                    ql_canvas_clear(&cv, 0xFFFFFF);
                    ql_ui_render_canvas(ui, &cv);

                    /* 找到三个 span 的内容盒，比较"最上面的墨迹行" */
                    int top[3] = { -1, -1, -1 };
                    int n = 0;
                    for (uint16_t i = 0; i < ui->node_count && n < 3; i++) {
                        if (strcmp(ql_ui_tag_name(ui->nodes[i].src->tag), "span") != 0)
                            continue;
                        QLRect cb = ui->nodes[i].content_box;
                        for (int y = cb.y - 8; y < cb.y + cb.h + 8; y++) {
                            if (y < 0 || y >= H)
                                continue;
                            if (ink_in((QLRect){ cb.x, y, cb.w, 1 }, bg)) {
                                top[n] = y;
                                break;
                            }
                        }
                        n++;
                    }
                    QL_CHECK_EQ(n, 3);
                    if (n == 3 && top[0] >= 0 && top[1] >= 0 && top[2] >= 0) {
                        printf("    墨迹顶：normal=%d super=%d sub=%d\n",
                               top[0], top[1], top[2]);
                        QL_CHECK(top[1] < top[0]);   /* 上标更高 */
                        QL_CHECK(top[2] > top[0]);   /* 下标更低 */
                    }
                    ql_ui_free(ui);
                }
                ql_ui_doc_free(doc);
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 7. blob 往返 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r>x<sup>2</sup></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            size_t cap = ql_ui_doc_encode(doc, NULL, 0);
            QL_CHECK(cap > 0);
            void *buf = (cap > 0) ? malloc(cap) : NULL;
            if (buf != NULL) {
                QL_CHECK_EQ((int)ql_ui_doc_encode(doc, buf, cap), (int)cap);
                QLUIDoc *back = ql_ui_doc_load_mem(buf, cap);
                QL_CHECK(back != NULL);
                if (back != NULL) {
                    QLUIRoot *ui2 = ql_ui_build(back);
                    QL_CHECK(ui2 != NULL);
                    if (ui2 != NULL) {
                        QLUINodeRT *sp = find_tag(ui2, "sup", 0);
                        QL_CHECK(sp != NULL);
                        if (sp != NULL) {
                            QL_CHECK_EQ((int)sp->style->baseline_dy, -4);
                            QL_CHECK_EQ((int)sp->style->font_family,
                                        (int)QL_UI_FAMILY_DEFAULT);
                        }
                        ql_ui_free(ui2);
                    }
                    ql_ui_doc_free(back);
                }
                free(buf);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
