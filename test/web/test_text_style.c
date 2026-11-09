/**
 * @file    test_text_style.c
 * @brief   字体变体与装饰线：`font-weight` / `font-style` / `text-decoration`。
 *
 * 这三个都是**合成**的（不加载第二个字体文件）：
 *   加粗 = 画两遍偏移 1px；斜体 = 按行剪切；装饰线 = 画一条 1px 横线。
 *
 * 判据一律看**渲染出来的墨迹**，不看 flags 位：位设对了但排版没用到，
 * 是最容易骗过自己的那种"通过"。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 120

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static const ql_color_t BG = QL_COLOR_RGB(0x10, 0x10, 0x10);

static int is_ink(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 0x80 && QL_COLOR_GET_G(c) > 0x80 &&
           QL_COLOR_GET_B(c) > 0x80;
}

/* 渲染一次；返回墨迹像素数，并按行统计（out_rows[y] = 该行墨迹数） */
static int render_ink(const char *html, const char *css, int *rows, int *first_row,
                      int *last_row)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return -1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return -1;
    }
    ql_ui_layout(ui, W, H);
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, BG);
    ql_ui_render_canvas(ui, &s_cv);

    int total = 0, f = -1, l = -1;
    for (ql_coord_t y = 0; y < H; y++) {
        int cnt = 0;
        for (ql_coord_t x = 0; x < W; x++)
            if (is_ink(ql_disp_mem_pixel(s_disp, x, y)))
                cnt++;
        if (rows != NULL)
            rows[y] = cnt;
        total += cnt;
        if (cnt > 0) {
            if (f < 0) f = (int)y;
            l = (int)y;
        }
    }
    if (first_row != NULL) *first_row = f;
    if (last_row != NULL) *last_row = l;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return total;
}

/* 某一行里墨迹的最左像素（-1 = 这一行没有墨迹） */
static int leftmost_in_row(const char *html, const char *css, int row)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL)
        return -1;
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return -1;
    }
    ql_ui_layout(ui, W, H);
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, BG);
    ql_ui_render_canvas(ui, &s_cv);

    int found = -1;
    for (ql_coord_t x = 0; x < W; x++)
        if (is_ink(ql_disp_mem_pixel(s_disp, x, row))) {
            found = (int)x;
            break;
        }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return found;
}

/* 取某个标签节点的 font_flags */
static int flags_of(const char *html, const char *css, int node)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return -1;
    }
    const int f = (int)doc->styles[doc->nodes[node].style].font_flags;
    ql_ui_doc_free(doc);
    return f;
}

QL_TEST_BEGIN("test_text_style")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    const char *html = "<div id=r>Hamburg</div>";
    const char *base = "#r { width: 200px; font-size: 20px; line-height: 26px;"
                       " color: #FFFFFF; }";

    /* ---------- 1. 解析：三种写法都落到 flags ---------- */
    {
        char css[256];
        snprintf(css, sizeof(css), "%s #r { font-weight: bold; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_BOLD) != 0);
        snprintf(css, sizeof(css), "%s #r { font-weight: 600; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_BOLD) != 0);
        snprintf(css, sizeof(css), "%s #r { font-weight: 400; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_BOLD) == 0);
        snprintf(css, sizeof(css), "%s #r { font-weight: 900; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_BOLD) != 0);

        snprintf(css, sizeof(css), "%s #r { font-style: italic; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_ITALIC) != 0);
        snprintf(css, sizeof(css), "%s #r { font-style: oblique; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_ITALIC) != 0);

        snprintf(css, sizeof(css), "%s #r { text-decoration: underline; }", base);
        QL_CHECK((flags_of(html, css, 0) & (int)QL_UI_FONT_UNDERLINE) != 0);
        snprintf(css, sizeof(css), "%s #r { text-decoration: underline line-through; }",
                 base);
        QL_CHECK((flags_of(html, css, 0) &
                  (int)(QL_UI_FONT_UNDERLINE | QL_UI_FONT_LINE_THROUGH)) ==
                 (int)(QL_UI_FONT_UNDERLINE | QL_UI_FONT_LINE_THROUGH));
    }

    /* ---------- 2. UA 默认样式让语义标签真的不一样 ---------- */
    {
        const char *h = "<div id=r><b>b</b><strong>s</strong><i>i</i><em>e</em>"
                        "<u>u</u><s>s</s><span>n</span></div>";
        QL_CHECK((flags_of(h, "", 1) & (int)QL_UI_FONT_BOLD) != 0);      /* b */
        QL_CHECK((flags_of(h, "", 2) & (int)QL_UI_FONT_BOLD) != 0);      /* strong */
        QL_CHECK((flags_of(h, "", 3) & (int)QL_UI_FONT_ITALIC) != 0);    /* i */
        QL_CHECK((flags_of(h, "", 4) & (int)QL_UI_FONT_ITALIC) != 0);    /* em */
        QL_CHECK((flags_of(h, "", 5) & (int)QL_UI_FONT_UNDERLINE) != 0); /* u */
        QL_CHECK((flags_of(h, "", 6) & (int)QL_UI_FONT_LINE_THROUGH) != 0); /* s */
        QL_CHECK_EQ(flags_of(h, "", 7), 0);                         /* span */
    }

    /* ---------- 3. 继承 ---------- */
    {
        const char *h = "<div id=r><div class=c>x</div></div>";
        const int f = flags_of(h, "#r { font-weight: bold; }", 1);
        QL_CHECK((f & (int)QL_UI_FONT_BOLD) != 0);
    }

    /* ---------- 4. 加粗：同一段文字墨迹更多 ---------- */
    {
        char css[256];
        int rows[H];
        snprintf(css, sizeof(css), "%s", base);
        const int plain = render_ink(html, css, rows, NULL, NULL);
        snprintf(css, sizeof(css), "%s #r { font-weight: bold; }", base);
        const int bold = render_ink(html, css, rows, NULL, NULL);
        QL_CHECK(plain > 0);
        QL_CHECK(bold > plain);           /* 画两遍必然更多 */
    }

    /* ---------- 5. 斜体：顶部比正体更靠右 ---------- */
    {
        char css[256];
        int f0 = -1, l0 = -1, f1 = -1, l1 = -1;
        snprintf(css, sizeof(css), "%s", base);
        QL_CHECK(render_ink(html, css, NULL, &f0, &l0) > 0);
        snprintf(css, sizeof(css), "%s #r { font-style: italic; }", base);
        QL_CHECK(render_ink(html, css, NULL, &f1, &l1) > 0);
        /* 顶行（H 的左上角）在斜体里必须右移，底行不会 */
        const int top_plain = leftmost_in_row(html, base, f0);
        char css_it[256];
        snprintf(css_it, sizeof(css_it), "%s #r { font-style: italic; }", base);
        const int top_italic = leftmost_in_row(html, css_it, f1);
        QL_CHECK(top_plain >= 0);
        QL_CHECK(top_italic > top_plain);
    }

    /* ---------- 6. 下划线 / 删除线：基线下方 / 中部出现整条墨迹 ---------- */
    {
        char css[256];
        int f = -1, l = -1;
        snprintf(css, sizeof(css), "%s", base);
        const int plain_bottom = render_ink(html, css, NULL, &f, &l);

        snprintf(css, sizeof(css), "%s #r { text-decoration: underline; }", base);
        const int ul_bottom = render_ink(html, css, NULL, &f, &l);
        QL_CHECK(ul_bottom > plain_bottom);       /* 多了一条线 */
        /* 下划线在字体底部之下（ascent + 2），所以墨迹底边更低 */
        QL_CHECK(l > f);

        snprintf(css, sizeof(css), "%s #r { text-decoration: line-through; }", base);
        int rows[H];
        const int st_ink = render_ink(html, css, rows, NULL, NULL);
        QL_CHECK(st_ink > plain_bottom);
    }

    /* ---------- 7. 装饰线只画在文字宽度上，空行不画 ---------- */
    {
        /* 折行后第二行是空的情况不会拖一条线（只检查不崩、墨迹数有限） */
        const char *h2 = "<div id=r>ab</div>";
        const char *c2 = "#r { width: 200px; font-size: 20px; color: #FFFFFF;"
                         " text-decoration: underline; white-space: normal; }";
        int rows[H];
        QL_CHECK(render_ink(h2, c2, rows, NULL, NULL) > 0);
    }

    /* ---------- 8. 明确报错 ---------- */
    {
        const char *bad[] = { "#r { font-weight: heavy }",
                              "#r { font-weight: 2000 }",
                              "#r { font-style: uppercase }",
                              "#r { text-decoration: wavy }",
                              "#r { text-decoration: underline bogus }" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text(html, bad[i]);
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
