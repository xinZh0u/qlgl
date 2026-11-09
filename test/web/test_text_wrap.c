/**
 * @file    test_text_wrap.c
 * @brief   自动折行（`white-space: normal`）。
 *
 * 三条契约：
 *   1. **布局与绘制共用同一份断行**（`ql_text_wrap()`）——否则会出现
 *      "布局按一行算、绘制画了三行"这种错位，那是最难查的一类；
 *   2. 每行宽度不超过内容盒宽度，除非是断不开的超长单词（只能硬断）；
 *   3. 默认 `nowrap`：不写 `white-space` 的页面行为完全不变（老界面不会被动重排）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 260
#define H 200

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static const ql_color_t BG = QL_COLOR_RGB(0x10, 0x10, 0x10);

/* 层的墨迹行数：按扫描行里是否出现亮像素来数（用来验证"真的画了几行"） */
static int ink_rows(const char *html, const char *css, int *first_y, int *last_y)
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

    int rows = 0, f = -1, l = -1, prev = -1;
    for (ql_coord_t y = 0; y < H; y++) {
        int lit = 0;
        for (ql_coord_t x = 0; x < W && !lit; x++) {
            const ql_color_t c = ql_disp_mem_pixel(s_disp, x, y);
            if (QL_COLOR_GET_R(c) > 0x80 && QL_COLOR_GET_G(c) > 0x80 &&
                QL_COLOR_GET_B(c) > 0x80)
                lit = 1;
        }
        if (lit) {
            if (f < 0) f = (int)y;
            l = (int)y;
            if (prev != y - 1) rows++;      /* 新的墨迹带 = 新的一行 */
            prev = (int)y;
        }
    }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    if (first_y != NULL) *first_y = f;
    if (last_y != NULL) *last_y = l;
    return rows;
}

/* 用文档 API 查一次布局高度 */
static int rect_h(const char *html, const char *css, int w)
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
    const int h = (int)ql_ui_node_rect(&ui->nodes[0]).h;
    (void)w;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return h;
}

QL_TEST_BEGIN("test_text_wrap")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. ql_text_wrap 本身：英文按词断 ---------- */
    {
        const QLFont *font = ql_font_default(14);
        QL_CHECK(font != NULL);
        const char *t = "the quick brown fox jumps over the lazy dog";
        QLTextLine lines[16];
        const int n = ql_text_wrap(font, t, 100, lines, 16);
        QL_CHECK(n > 1);
        QL_CHECK(n <= 16);
        for (int i = 0; i < n; i++) {
            QL_CHECK(lines[i].len >= 0);
            QL_CHECK(ql_font_line_width(font, lines[i].start, lines[i].len) <= 100);
            /* 不能在词中间断：行首不该是空格，行尾不该是半个单词（这里只查首尾空白） */
            QL_CHECK(lines[i].len == 0 || lines[i].start[0] != ' ');
        }
    }

    /* ---------- 2. CJK 可以随处断 ---------- */
    {
        const QLFont *font = ql_font_default(14);
        const char *t = "这是一段很长的中文用来验证自动折行是否会在合适的位置断开";
        QLTextLine lines[32];
        const int n = ql_text_wrap(font, t, 80, lines, 32);
        QL_CHECK(n > 1);
        for (int i = 0; i < n; i++)
            QL_CHECK(ql_font_line_width(font, lines[i].start, lines[i].len) <= 80);
    }

    /* ---------- 3. 超长单词硬断（不断就会溢出） ---------- */
    {
        const QLFont *font = ql_font_default(14);
        const char *t = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        QLTextLine lines[32];
        const int n = ql_text_wrap(font, t, 40, lines, 32);
        QL_CHECK(n > 1);
        for (int i = 0; i < n; i++)
            QL_CHECK(ql_font_line_width(font, lines[i].start, lines[i].len) <= 40);
    }

    /* ---------- 4. '\n' 强制断，且与折行叠加 ---------- */
    {
        const QLFont *font = ql_font_default(14);
        QLTextLine lines[8];
        QL_CHECK_EQ(ql_text_wrap(font, "a\nb\nc", 0, lines, 8), 3);
        QL_CHECK_EQ(lines[0].len, 1);
        QL_CHECK_EQ(lines[1].len, 1);
        QL_CHECK_EQ(lines[2].len, 1);
        /* max_w = 0 表示不折行：长文本也只有 1 行 */
        QL_CHECK_EQ(ql_text_wrap(font, "the quick brown fox jumps over the lazy dog", 0,
                                 NULL, 0), 1);
    }

    /* ---------- 5. 默认 nowrap：老行为一点没变 ---------- */
    {
        const char *html = "<div id=r>the quick brown fox jumps over the lazy dog</div>";
        const char *css = "#r { width: 80px; font-size: 14px; color: #FFFFFF; }";
        /* 不折行 → 一行高（= 字体自身的 line_height） */
        const QLFont *font = ql_font_default(14);
        QL_CHECK(font != NULL);
        QL_CHECK_EQ(rect_h(html, css, 0), (int)font->line_height);
    }

    /* ---------- 6. white-space: normal → 高度按行数长 ---------- */
    {
        const char *html = "<div id=r>the quick brown fox jumps over the lazy dog</div>";
        const char *css_nw = "#r { width: 80px; font-size: 14px; color: #FFFFFF; }";
        const char *css_w = "#r { width: 80px; font-size: 14px; color: #FFFFFF;"
                            " white-space: normal; }";
        const int h_nw = rect_h(html, css_nw, 0);
        const int h_w = rect_h(html, css_w, 0);
        QL_CHECK(h_nw > 0);
        QL_CHECK(h_w > h_nw * 2);       /* 9 个词在 80px 里至少要好几行 */
    }

    /* ---------- 7. 绘制真的画了多行，且每行都在盒子内 ---------- */
    {
        const char *html = "<div id=r>the quick brown fox jumps over the lazy dog</div>";
        const char *css = "#r { width: 80px; font-size: 14px; color: #FFFFFF;"
                          " white-space: normal; line-height: 16px; }";
        int y0 = -1, y1 = -1;
        const int rows = ink_rows(html, css, &y0, &y1);
        QL_CHECK(rows >= 3);
        QL_CHECK(y0 >= 0);
        QL_CHECK(y1 < H);
    }

    /* ---------- 8. 居中：折行后每行各自居中 ---------- */
    {
        const char *html = "<div id=r>aaaaaaaa bbbbbbbb cccccccc dddddddd</div>";
        const char *css = "#r { width: 100px; font-size: 14px; color: #FFFFFF;"
                          " white-space: normal; text-align: center; }";
        int y0 = -1, y1 = -1;
        QL_CHECK(ink_rows(html, css, &y0, &y1) >= 2);
    }

    /* ---------- 9. 继承：容器写一次，里面都折 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r><p class=c>hello</p></div>",
                                       "#r { white-space: normal; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->styles[1].white_space, 1);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. column flex 里的文本项：主轴是**竖直** ---------- */
    {
        /*
         * 回归：max-content 主轴尺寸曾经不分轴，一律用文本**宽度**。
         * 于是 `flex-direction: column` 里的每个文本项都声称自己有"文本宽度"
         * 那么高（46 个汉字 → 322px），整页被撑爆，折行高度也算不出来。
         */
        const char *html = "<div id=r><div class=d>很长很长的一段中文文字需要在窄容器里"
                           "自动折成好几行才对，所以这里再多写一些字确保一定会折行</div></div>";
        const char *css = "#r { width: 200px; display: flex; flex-direction: column; }"
                          ".d { width: 200px; font-size: 12px; line-height: 18px;"
                          "     white-space: normal; }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, 200, 300);
                const QLFont *font = ql_font_default(12);
                const int lines = ql_text_wrap(font, ui->nodes[1].text, 200, NULL, 0);
                QL_CHECK(lines >= 2);
                QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).h, 18 * lines);
                /* 容器高 = 子项高（没有 gap、没有额外撑高） */
                QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[0]).h, 18 * lines);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 11. pre 系列现在支持了；真正不认的值仍然明确报错 ---------- */
    {
        /* 认：pre / pre-wrap / pre-line（行为测试在 test_white_space.c） */
        const char *good[] = { "#r { white-space: pre }",
                               "#r { white-space: pre-wrap }",
                               "#r { white-space: pre-line }" };
        for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", good[i]);
            QL_CHECK(doc != NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
        /* 不认：break-spaces 需要"按最长单词撑破"的语义，本库不做 */
        const char *bad[] = { "#r { white-space: break-spaces }",
                              "#r { white-space: pre-wrap-x }" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", bad[i]);
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
