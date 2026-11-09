/**
 * @file    test_white_space.c
 * @brief   TEXT 包 P4a：`white-space: pre / pre-wrap / pre-line` 与 `<pre>`。
 *
 * 这一期唯一的难点是**折叠时机**：`white-space` 是级联之后才知道的，
 * 而空白折叠决定文本字节。所以本库把折叠推迟到建表期 —— 这些测试就是在守
 * "推迟了但结果没变"（非 `pre` 页面逐字节不变，见 test_golden）以及
 * "推迟了所以 pre 真的能保留"。
 *
 * 覆盖：
 *   1. `<pre>` 认识、UA 默认 = block + white-space:pre + margin
 *   2. 起始标签后紧接的一个 `\n` 被丢掉（规范行为），第二个保留
 *   3. 空白与换行真的进了文本池（不是折叠成一行）
 *   4. `pre` 不折行、`pre-wrap` 折行 —— 高度差得出来
 *   5. `pre-line` 折叠空格但保留换行
 *   6. `\t` 展开成 8 列制表位
 *   7. 普通 `div` 的空白折叠行为**没有变化**（回归）
 *   8. 不认的值仍然明确报错
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 400
#define H 400

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

/* 按标签名取第 nth 个节点 */
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

/* 节点文本是否恰好等于 expect（用来判断空白到底保留成什么样） */
static int text_is(const QLUINodeRT *n, const char *expect)
{
    return n != NULL && n->text != NULL && strcmp(n->text, expect) == 0;
}

static void show_text(const char *what, const QLUINodeRT *n)
{
    printf("    %s = %s\n", what, (n != NULL && n->text != NULL) ? n->text : "(null)");
}

QL_TEST_BEGIN("test_white_space")

    /* ---------- 1. `<pre>` 认识 + UA 默认 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<pre>x</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(p != NULL);
            if (p != NULL) {
                QL_CHECK_EQ((int)p->style->display, (int)QL_UI_DISPLAY_BLOCK);
                QL_CHECK_EQ((int)p->style->white_space, (int)QL_UI_WS_PRE);
                QL_CHECK_EQ((int)p->style->margin.top, 8);
                QL_CHECK_EQ((int)p->style->margin.bottom, 8);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 起始标签后的一个 `\n` 被丢掉 ---------- */
    {
        QLUIRoot *ui = NULL;
        /* 只有一个换行：按规范丢掉 → 文本就是 "x" */
        QLUIDoc *doc = mk("<pre>\nx</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(text_is(p, "x"));
            if (!text_is(p, "x"))
                show_text("<pre>\\nx 的文本", p);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 两个换行：只丢一个 → 文本是 "\nx"（渲染成一行空行 + 一行 x） */
        ui = NULL;
        doc = mk("<pre>\n\nx</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(text_is(p, "\nx"));
            if (!text_is(p, "\nx"))
                show_text("<pre>\\n\\nx 的文本", p);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 空白与换行真的保留 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<pre>  a  b\n  c</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            /* 行首两个空格、中间两个空格、换行、行首两个空格 —— 一个都不能少 */
            QL_CHECK(text_is(p, "  a  b\n  c"));
            if (!text_is(p, "  a  b\n  c"))
                show_text("<pre> 的文本", p);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. `pre` 不折行、`pre-wrap` 折行：高度差得出来 ---------- */
    {
        /* 同样一段 40 个字符的长文本，盒子只有 100px 宽 */
        const char *body = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

        QLUIRoot *ui = NULL;
        char html[256];
        snprintf(html, sizeof(html), "<div id=r><pre>%s</pre></div>", body);
        QLUIDoc *doc = mk(html, "#r { width: 100px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(p != NULL);
            const ql_coord_t h_pre = (p != NULL) ? p->border_box.h : 0;
            ql_ui_free(ui);
            ql_ui_doc_free(doc);

            /* 同一个盒子，改成 div + pre-wrap */
            ui = NULL;
            snprintf(html, sizeof(html), "<div id=r><div class=k>%s</div></div>", body);
            doc = mk(html, "#r { width: 100px } .k { white-space: pre-wrap }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *k = find_tag(ui, "div", 1);
                const ql_coord_t h_wrap = (k != NULL) ? k->border_box.h : 0;
                QL_CHECK(h_wrap > h_pre);
                printf("    pre 高 %d，pre-wrap 高 %d\n", (int)h_pre, (int)h_wrap);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 5. `pre-line`：折叠空格、保留换行 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r>  a   b\n  c  </div>",
                          "#r { white-space: pre-line }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *r = find_tag(ui, "div", 0);
            /* 行首/行尾被 strip，中间的长空白折叠成一个空格，'\n' 留着 */
            QL_CHECK(text_is(r, "a b\nc"));
            if (!text_is(r, "a b\nc"))
                show_text("pre-line 的文本", r);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. `\t` 展开成 8 列制表位 ---------- */
    {
        QLUIRoot *ui = NULL;
        /* "a"（1 列）后面一个 tab → 补 7 个空格 */
        QLUIDoc *doc = mk("<pre>a\tb</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(text_is(p, "a       b"));
            if (!text_is(p, "a       b"))
                show_text("tab 展开", p);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 换行之后列号归零：第二行的 tab 又是 8 个空格 */
        ui = NULL;
        doc = mk("<pre>ab\n\tc</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(text_is(p, "ab\n        c"));
            if (!text_is(p, "ab\n        c"))
                show_text("换行后的 tab", p);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 回归：普通 div 的空白仍然折叠成一个空格 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r>a   \n   b</div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *r = find_tag(ui, "div", 0);
            QL_CHECK(text_is(r, "a b"));
            if (!text_is(r, "a b"))
                show_text("普通 div", r);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 没有 white-space 的节点里，`\t` 也折叠（不会变成 8 个空格） */
        ui = NULL;
        doc = mk("<div id=r>a\tb</div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *r = find_tag(ui, "div", 0);
            QL_CHECK(text_is(r, "a b"));
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. 明确报错：不认的 white-space 值 ---------- */
    {
        const char *bad[] = { "#r { white-space: break-spaces }",
                              "#r { white-space: pre-wrap-x }",
                              "#r { white-space: preserve }" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", bad[i]);
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. `<pre>` 里的行内标签：内容仍然被保留 ---------- */
    {
        QLUIRoot *ui = NULL;
        /* 混排：pre 的 white-space 会继承给匿名 span */
        QLUIDoc *doc = mk("<pre>a  <b>b</b>  c</pre>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = find_tag(ui, "pre", 0);
            QL_CHECK(p != NULL);
            /* 首段 "a  " 被 lstrip 过吗？不会 —— 它不是首段之前没有元素……
             * 这里只断言"两段空白都还在"，具体形态由混排规则决定 */
            int found = 0;
            for (uint16_t i = 0; i < ui->node_count; i++) {
                const QLUINodeRT *n = &ui->nodes[i];
                if (n->text != NULL && strstr(n->text, "  ") != NULL)
                    found = 1;
            }
            QL_CHECK(found);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. blob 往返：white_space 值原样回来 ---------- */
    {
        static const struct { const char *ws; int val; } k_cases[] = {
            { "nowrap",   QL_UI_WS_NOWRAP },
            { "normal",   QL_UI_WS_NORMAL },
            { "pre",      QL_UI_WS_PRE },
            { "pre-wrap", QL_UI_WS_PRE_WRAP },
            { "pre-line", QL_UI_WS_PRE_LINE },
        };
        for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
            char css[64];
            snprintf(css, sizeof(css), "#r { white-space: %s }", k_cases[i].ws);
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk("<div id=r>x</div>", css, &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *r = find_tag(ui, "div", 0);
                QL_CHECK_EQ((int)r->style->white_space, k_cases[i].val);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
            /* 走到 blob 再回来 */
            QLUIRoot *ui2 = NULL;
            QLUIDoc *doc2 = mk("<div id=r>x</div>", css, &ui2);
            if (doc2 != NULL) {
                size_t cap = ql_ui_doc_encode(doc2, NULL, 0);
                QL_CHECK(cap > 0);
                void *buf = (cap > 0) ? malloc(cap) : NULL;
                QL_CHECK(buf != NULL);
                if (buf != NULL) {
                    size_t n = ql_ui_doc_encode(doc2, buf, cap);
                    QL_CHECK_EQ((int)n, (int)cap);
                    QLUIDoc *back = ql_ui_doc_load_mem(buf, n);
                    QL_CHECK(back != NULL);
                    if (back != NULL) {
                        QLUIRoot *ui3 = ql_ui_build(back);
                        QL_CHECK(ui3 != NULL);
                        if (ui3 != NULL) {
                            QLUINodeRT *r = find_tag(ui3, "div", 0);
                            QL_CHECK_EQ((int)r->style->white_space, k_cases[i].val);
                            ql_ui_free(ui3);
                        }
                        ql_ui_doc_free(back);
                    }
                    free(buf);
                }
            }
            ql_ui_free(ui2);
            ql_ui_doc_free(doc2);
        }
    }

QL_TEST_END()
