/**
 * @file    test_text_adv.c
 * @brief   文本细节：`letter-spacing` / `text-indent` / `text-transform` /
 *          `text-overflow: ellipsis` / `word-break: break-all`。
 *
 * 这一批的三条契约：
 *   1. **布局与绘制必须用同一份折行**（`ql_text_wrap_opt`），否则行数/位置打架；
 *   2. `letter-spacing` 加在**每个字符之后**（含最后一个），宽度与绘制一起变；
 *   3. `text-transform` 在**建表期**结算（与空白折叠同一处），
 *      但输入框/多行输入跳过 —— 那里的文字是运行期的值。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 80

static const ql_color_t BG = QL_COLOR_RGB(0xFF, 0xFF, 0xFF);

/* 渲染一份界面，返回画布（调用方负责 destroy） */
static QLDispDriver *render_doc(const char *html, const char *css)
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

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    if (disp != NULL) {
        QLCanvas cv;
        ql_canvas_from_disp(&cv, disp);
        ql_canvas_clear(&cv, BG);
        ql_ui_render_canvas(ui, &cv);
    }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return disp;
}

/* 某一行里最右边的"非背景"像素列；没有则返回 -1 */
static int rightmost_ink(QLDispDriver *disp, int y0, int y1)
{
    int best = -1;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < W; x++)
            if (ql_disp_mem_pixel(disp, x, y) != BG && x > best)
                best = x;
    return best;
}

int ql_font_line_width_ls(const QLFont *font, const char *text, int32_t len,
                         ql_coord_t letter_spacing);
int ql_text_wrap_opt(const QLFont *font, const char *text, ql_coord_t max_w,
                     const QLTextWrapOpt *opt, QLTextLine *out, int max_lines,
                     int keep_trailing);

QL_TEST_BEGIN("test_text_adv")

    const QLFont *font = ql_font_default(12);
    QL_CHECK(font != NULL);

    /* ---------- 1. letter-spacing：每个字符之后都加一份 ---------- */
    const ql_coord_t plain = ql_font_line_width(font, "abcd", -1);
    const ql_coord_t spaced = ql_font_line_width_ls(font, "abcd", -1, 3);
    QL_CHECK_EQ((int)spaced, (int)plain + 4 * 3);

    /* ---------- 2. text-indent：只压缩整段第一行的可用宽度 ---------- */
    {
        QLTextLine lines[8];
        QLTextWrapOpt o;
        memset(&o, 0, sizeof(o));
        const ql_coord_t maxw = ql_font_line_width(font, "abcd", -1) + 1;

        /* 不缩进：正好放得下，一行 */
        QL_CHECK_EQ(ql_text_wrap_opt(font, "abcd", maxw, &o, lines, 8, 0), 1);
        /* 首行缩进 40px：一行放不下了，变两行 */
        o.indent = 40;
        const int n = ql_text_wrap_opt(font, "abcd", maxw, &o, lines, 8, 0);
        QL_CHECK(n > 1);
        QL_CHECK(lines[0].len < 4);
        /* 第二行不受缩进影响：剩下的字应当一行装完 */
        QL_CHECK_EQ(lines[1].len, 4 - lines[0].len);
    }

    /* ---------- 3. word-break: break-all 让行被填得更满 ---------- */
    {
        QLTextLine lines[8];
        QLTextWrapOpt o;
        memset(&o, 0, sizeof(o));
        /* 宽度刚好装下 "aaa b"，但装不下 "aaa bbb" */
        const ql_coord_t maxw = ql_font_line_width(font, "aaa b", -1) + 1;

        const int n0 = ql_text_wrap_opt(font, "aaa bbb", maxw, &o, lines, 8, 0);
        const int32_t head0 = lines[0].len;
        o.break_any = 1;
        const int n1 = ql_text_wrap_opt(font, "aaa bbb", maxw, &o, lines, 8, 0);
        const int32_t head1 = lines[0].len;

        QL_CHECK_EQ(n0, 2);
        QL_CHECK_EQ(n1, 2);
        QL_CHECK_EQ(head0, 3);        /* 只在空格后断："aaa" */
        QL_CHECK(head1 > head0);      /* break-all 会吃掉空格后面的字 */
    }

    /* ---------- 4. text-transform 在建表期生效 ---------- */
    {
        const char *html = "<div id=r><div id=a>hello World</div>"
                           "<div id=b>hello World</div>"
                           "<div id=c>hello world</div></div>";
        QLUIDoc *doc = ql_ui_load_text(html, "#a { text-transform: uppercase }"
                                          "#b { text-transform: capitalize }"
                                          "#c { text-transform: lowercase }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK(strcmp(ql_ui_node_text(&ui->nodes[1]), "HELLO WORLD") == 0);
                QL_CHECK(strcmp(ql_ui_node_text(&ui->nodes[2]), "Hello World") == 0);
                QL_CHECK(strcmp(ql_ui_node_text(&ui->nodes[3]), "hello world") == 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 输入框不吃 text-transform（值是运行期的） ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=a><input id=i value='abc'></div>",
                                       "#a { text-transform: uppercase }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                /* 找那个带值的输入框节点（下标不稳，按"有没有 value"找） */
                const char *val = NULL;
                for (uint16_t i = 0; i < ui->node_count; i++) {
                    const char *v = ql_ui_input_value(ui, &ui->nodes[i]);
                    if (v != NULL) {
                        val = v;
                        break;
                    }
                }
                QL_CHECK(val != NULL && strcmp(val, "abc") == 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 像素层：ellipsis 不许画到盒子外面 ---------- */
    {
        const char *html = "<div id=a>This is a very long line of text</div>";
        const char *CSS_ELL = "#a { width: 60px; white-space: nowrap;"
                              " text-overflow: ellipsis; font-size: 12px }";
        const char *CSS_CLIP = "#a { width: 60px; white-space: nowrap;"
                               " text-overflow: clip; font-size: 12px }";

        QLDispDriver *de = render_doc(html, CSS_ELL);
        QLDispDriver *dc = render_doc(html, CSS_CLIP);
        QL_CHECK(de != NULL && dc != NULL);
        if (de != NULL && dc != NULL) {
            const int xe = rightmost_ink(de, 0, H);
            const int xc = rightmost_ink(dc, 0, H);
            QL_CHECK(xe >= 0);            /* 省略号版画了东西 */
            QL_CHECK(xc > 60);            /* clip 版照旧溢出（overflow: visible） */
            QL_CHECK(xe < 60);            /* 省略号版截在盒子里 */
        }
        if (de != NULL)
            ql_disp_mem_destroy(de);
        if (dc != NULL)
            ql_disp_mem_destroy(dc);
    }

    /* ---------- 7. 像素层：letter-spacing 让同一段字更宽 ---------- */
    {
        const char *html = "<div id=a>abcd</div>";
        QLDispDriver *d0 = render_doc(html, "#a { font-size: 12px }");
        QLDispDriver *d1 = render_doc(html, "#a { font-size: 12px; letter-spacing: 4px }");
        QL_CHECK(d0 != NULL && d1 != NULL);
        if (d0 != NULL && d1 != NULL) {
            const int x0 = rightmost_ink(d0, 0, H);
            const int x1 = rightmost_ink(d1, 0, H);
            QL_CHECK(x1 - x0 >= 12);      /* 4 个字 × 4px，误差给几像素 */
        }
        if (d0 != NULL)
            ql_disp_mem_destroy(d0);
        if (d1 != NULL)
            ql_disp_mem_destroy(d1);
    }

QL_TEST_END()
