/**
 * @file    test_columns.c
 * @brief   多栏（`column-count` / `column-width` / `columns` / `column-rule`）。
 *
 * 本库的多栏是**贪心分栏**：按顺序把子节点塞进当前栏，塞不下就换下一栏，
 * 不把一个子节点劈成两半。所以测试的重点是：
 *
 *   1. 栏数：只给 `count` 用它；只给 `width` 按容器宽反推；两个都给取小的；
 *   2. 栏宽 = (容器内容宽 - 栏间距×(n-1)) / n；
 *   3. 容器高度：给了 `height` 就用它当栏高（放不下就换栏），
 *      否则按"等高栏"均分（ceil(总高/n)，不低于最高的子节点）；
 *   4. `column-rule` 画在缝的正中间，`column-rule-width: 0` / `none` 时不画；
 *   5. 不该支持的写法必须**构建期报错**（红线：不静默降级）。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 400
#define H 300
#define BG  0xFFFFFFu

static QLDispDriver *s_disp;
static QLCanvas s_cv;
static QLUIDoc *s_doc;

static QLUIRoot *render_html(const char *html, const char *css)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    s_doc = doc;
    ql_ui_layout(ui, W, H);
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, BG);
    ql_ui_render_canvas(ui, &s_cv);
    return ui;
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

/* 第 n 个 .item（4 个测试用例的块高都固定为 20/60，用来区分） */
static QLUINodeRT *nth_item(QLUIRoot *ui, int n)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *nd = &ui->nodes[i];
        if (nd->src == NULL || nd->src->tag != QL_UI_TAG_DIV)
            continue;
        if (nd->style == NULL || nd->style->display == QL_UI_DISPLAY_NONE)
            continue;
        if (nd->text == NULL || nd->text[0] == '\0')
            continue;      /* .item 里都有文字，容器没有 */
        if (n-- == 0)
            return nd;
    }
    return NULL;
}

/* 期望被拒绝，且报错文案里含 needle */
static int rejects(const char *html, const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  本该报错却编译通过了：%s\n", css);
        return 0;
    }
    const char *msg = ql_ui_text_last_error();
    if (msg == NULL || strstr(msg, needle) == NULL) {
        printf("  报错文案不含 '%s'：%s\n", needle, msg ? msg : "(null)");
        return 0;
    }
    return 1;
}

QL_TEST_BEGIN("test_columns")

/* ---------- 1. column-count：栏宽与栏数 ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "  <div class=item>c</div><div class=item>d</div>"
        "</div>";
    const char *css =
        "#box{width:232px;column-count:2;column-gap:16px}"
        ".item{height:20px}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        /* 栏宽 = (232 - 16) / 2 = 108；4 个 20px 高的项 → 每栏 2 个，栏高 40 */
        QLUINodeRT *a = nth_item(ui, 0);
        QLUINodeRT *b = nth_item(ui, 1);
        QLUINodeRT *c = nth_item(ui, 2);
        QL_CHECK(a != NULL && b != NULL && c != NULL);
        if (a != NULL && b != NULL && c != NULL) {
            printf("  count=2：a=(%d,%d) b=(%d,%d) c=(%d,%d) w=%d\n",
                   (int)a->border_box.x, (int)a->border_box.y,
                   (int)b->border_box.x, (int)b->border_box.y,
                   (int)c->border_box.x, (int)c->border_box.y,
                   (int)a->border_box.w);
            QL_CHECK_EQ((int)a->border_box.w, 108);
            QL_CHECK_EQ((int)a->border_box.x, 0);
            QL_CHECK_EQ((int)b->border_box.x, 0);              /* 前两项在同一栏 */
            QL_CHECK_EQ((int)b->border_box.y, 20);
            QL_CHECK_EQ((int)c->border_box.x, 124);            /* 第二栏 = 108 + 16 */
            QL_CHECK_EQ((int)c->border_box.y, 0);
        }
        const QLUINodeRT *box = a != NULL ? a->parent : NULL;
        if (box != NULL) {
            printf("  容器高 %d（两栏各 40）\n", (int)box->border_box.h);
            QL_CHECK_EQ((int)box->border_box.h, 40);
        }
        ql_ui_free(ui);
    }
}

/* ---------- 2. column-width：按宽度反推栏数 ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "  <div class=item>c</div>"
        "</div>";
    /* 343 宽、缝 16：能放几栏 100px？(343+16)/(100+16)=3.09 → 3 栏 */
    const char *css =
        "#box{width:343px;column-width:100px}"
        ".item{height:20px}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QLUINodeRT *a = nth_item(ui, 0);
        QLUINodeRT *c = nth_item(ui, 2);
        QL_CHECK(a != NULL && c != NULL);
        if (a != NULL && c != NULL) {
            /* 栏宽 = (343 - 32) / 3 = 103；第三栏 x = 2×(103+16) = 238 */
            printf("  width=100：a w=%d，c x=%d\n", (int)a->border_box.w,
                   (int)c->border_box.x);
            QL_CHECK_EQ((int)a->border_box.w, 103);
            QL_CHECK_EQ((int)c->border_box.x, 238);
        }
        ql_ui_free(ui);
    }
}

/* ---------- 3. count 与 width 同时给：取小的那个 ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "  <div class=item>c</div><div class=item>d</div>"
        "</div>";
    /* 400 宽只能放下 2 栏 180px（(400+16)/(180+16)=2.1），写了 4 栏也只给 2 栏 */
    const char *css =
        "#box{width:400px;column-count:4;column-width:180px}"
        ".item{height:20px}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QLUINodeRT *a = nth_item(ui, 0);
        QLUINodeRT *b = nth_item(ui, 1);
        QL_CHECK(a != NULL && b != NULL);
        if (a != NULL && b != NULL) {
            /* 2 栏、栏宽 (400-16)/2 = 192 */
            printf("  count=4 + width=180 → 栏宽 %d\n", (int)a->border_box.w);
            QL_CHECK_EQ((int)a->border_box.w, 192);
            QL_CHECK_EQ((int)b->border_box.y, 20);         /* 同一栏里第二个 */
        }
        ql_ui_free(ui);
    }
}

/* ---------- 4. 确定高度：放不下就换栏（哪怕只放得下一半） ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "  <div class=item>c</div>"
        "</div>";
    /* 容器高 100，每项 60：每栏最多 1 项 */
    const char *css =
        "#box{width:232px;height:100px;column-count:2;column-gap:16px}"
        ".item{height:60px}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QLUINodeRT *a = nth_item(ui, 0);
        QLUINodeRT *b = nth_item(ui, 1);
        QLUINodeRT *c = nth_item(ui, 2);
        QL_CHECK(a != NULL && b != NULL && c != NULL);
        if (a != NULL && b != NULL && c != NULL) {
            printf("  height=100：b x=%d，c x=%d\n", (int)b->border_box.x,
                   (int)c->border_box.x);
            QL_CHECK_EQ((int)a->border_box.x, 0);
            QL_CHECK_EQ((int)b->border_box.x, 124);
            /* 两栏都满了，第三项只能挤在最后一栏里（不丢内容） */
            QL_CHECK_EQ((int)c->border_box.x, 124);
            QL_CHECK_EQ((int)c->border_box.y, 60);
        }
        ql_ui_free(ui);
    }
}

/* ---------- 5. column-rule 画在缝的中间 ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "</div>";
    const char *css =
        "#box{width:216px;column-count:2;column-gap:16px;"
        "     column-rule:1px solid #FF0000}"
        ".item{height:20px;background-color:#FFFFFF}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        /* 栏宽 (216-16)/2 = 100，缝在 100..116，线在 108（1px 宽） */
        const ql_color_t on = px(108, 5);
        const ql_color_t off = px(120, 5);
        printf("  分隔线：x=108 → #%06X，x=120 → #%06X\n", on & 0xFFFFFFu,
               off & 0xFFFFFFu);
        QL_CHECK_EQ(on & 0xFFFFFFu, 0xFF0000u);
        QL_CHECK_EQ(off & 0xFFFFFFu, BG);
        ql_ui_free(ui);
    }

    /* width: 0 → 不画 */
    ui = render_html(html, "#box{width:216px;column-count:2;column-gap:16px;"
                           "column-rule:0 solid #FF0000}.item{height:20px}");
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QL_CHECK_EQ(px(108, 5) & 0xFFFFFFu, BG);
        ql_ui_free(ui);
    }
}

/* ---------- 6. `columns` 简写：两种顺序都要认 ---------- */
{
    const char *html =
        "<div id=box>"
        "  <div class=item>a</div><div class=item>b</div>"
        "  <div class=item>c</div><div class=item>d</div>"
        "</div>";
    /* 顺序反过来写（先宽后数），栏数仍取小的那个：400 宽只放得下 2 栏 180px */
    const char *css = "#box{width:400px;columns:180px 4}.item{height:20px}";
    QLUIRoot *ui = render_html(html, css);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QLUINodeRT *a = nth_item(ui, 0);
        QL_CHECK(a != NULL);
        if (a != NULL) {
            printf("  columns:180px 4 → 栏宽 %d\n", (int)a->border_box.w);
            QL_CHECK_EQ((int)a->border_box.w, 192);
        }
        ql_ui_free(ui);
    }
}

/* ---------- 7. 不该支持的写法 ---------- */
{
    QL_CHECK(rejects("<div id=r><div>a</div></div>", "#r{column-count:many}",
                     "column-count"));
    QL_CHECK(rejects("<div id=r><div>a</div></div>", "#r{column-width:-4px}",
                     "column-width"));
    QL_CHECK(rejects("<div id=r><div>a</div></div>", "#r{columns:auto}",
                     "columns"));
    QL_CHECK(rejects("<div id=r><div>a</div></div>", "#r{column-rule:3px wavy red}",
                     "column-rule"));
    QL_CHECK(rejects("<div id=r><div>a</div></div>", "#r{column-span:all}",
                     "column-span"));
}

if (s_doc != NULL) {
    ql_ui_doc_free(s_doc);
    s_doc = NULL;
}
if (s_disp != NULL)
    ql_disp_mem_destroy(s_disp);
s_disp = NULL;

QL_TEST_END()
