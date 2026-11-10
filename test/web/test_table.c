/**
 * @file    test_table.c
 * @brief   B7 表格：`display: table` 全家 + `<table>` 系列标签 + colspan/rowspan。
 *
 * 表格是**两套 display 交叉**的结构（table / row-group / row / cell），而只有 cell
 * 真正有盒子。所以测试的重点是"几何算得对不对"：
 *
 *   1. 列宽：首行显式 `width` 优先，其余列**等分**剩余宽度；
 *   2. `colspan` 的宽度 = 覆盖的列宽之和（+ 列间距）；`rowspan` 的高度同理；
 *   3. 行高 = 这一行里最高的单元格；跨行单元格不够高时把差额补给覆盖的最后一行；
 *   4. `border-spacing`（separate）真的把单元格推开；`collapse` 时相邻边框重叠成一条；
 *   5. `vertical-align: middle/bottom` 在单元格内真的居中/靠底；
 *   6. `caption` 排在表格上方、宽度等于表格内容宽；
 *   7. 不该支持的写法必须**构建期报错**（红线：不静默降级）。
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

/* 把第 n 个"单元格"找出来（按文档顺序数 display: table-cell 的节点） */
static QLUINodeRT *nth_cell(QLUIRoot *ui, int n)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *nd = &ui->nodes[i];
        if (nd->style == NULL || nd->style->display != QL_UI_DISPLAY_TABLE_CELL)
            continue;
        if (n-- == 0)
            return nd;
    }
    return NULL;
}

/* 第 n 个不是单元格的节点（行 / 行组 / 表格 / caption），方便看整行尺寸 */
static QLUINodeRT *nth_display(QLUIRoot *ui, int disp, int n)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *nd = &ui->nodes[i];
        if (nd->style == NULL || (int)nd->style->display != disp)
            continue;
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

QL_TEST_BEGIN("test_table")

    /* ---------- 1. 标签 → display 角色 ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><caption>cap</caption><thead><tr><th>a</th></tr></thead>"
            "<tbody><tr><td>b</td></tr></tbody></table>",
            "#t{width:200px}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            struct { int disp; int want; } cases[] = {
                { QL_UI_DISPLAY_TABLE, 1 },
                { QL_UI_DISPLAY_TABLE_CAPTION, 1 },
                { QL_UI_DISPLAY_TABLE_ROW_GROUP, 2 },   /* thead + tbody */
                { QL_UI_DISPLAY_TABLE_ROW, 2 },
                { QL_UI_DISPLAY_TABLE_CELL, 2 },
            };
            for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                int n = 0;
                for (uint16_t k = 0; k < ui->node_count; k++) {
                    if (ui->nodes[k].style != NULL &&
                        (int)ui->nodes[k].style->display == cases[i].disp)
                        n++;
                }
                printf("  display %d：%d 个（期望 %d）\n", cases[i].disp, n,
                       cases[i].want);
                QL_CHECK_EQ(n, cases[i].want);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 2. 列宽：等分 + 首行显式宽度 ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><tr><td>a</td><td>b</td><td>c</td></tr></table>",
            "#t{width:300px}td{padding:0}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            for (int i = 0; i < 3; i++) {
                QLUINodeRT *c = nth_cell(ui, i);
                QL_CHECK(c != NULL);
                if (c != NULL) {
                    printf("  等分第 %d 列：x=%d w=%d\n", i, (int)c->border_box.x,
                           (int)c->border_box.w);
                    QL_CHECK_EQ((int)c->border_box.w, 100);
                    QL_CHECK_EQ((int)c->border_box.x, 100 * i);
                }
            }
            ql_ui_free(ui);
        }

        /* 首行写了两列的固定宽，第三列吃掉剩下的 */
        ui = render_html(
            "<table id=t><tr><td class=f1>a</td><td class=f2>b</td><td>c</td></tr></table>",
            "#t{width:300px}td{padding:0}.f1{width:40px}.f2{width:60px}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *c0 = nth_cell(ui, 0);
            QLUINodeRT *c2 = nth_cell(ui, 2);
            QL_CHECK(c0 != NULL && c2 != NULL);
            if (c0 != NULL && c2 != NULL) {
                printf("  固定列：c0 w=%d  c2 x=%d w=%d\n", (int)c0->border_box.w,
                       (int)c2->border_box.x, (int)c2->border_box.w);
                QL_CHECK_EQ((int)c0->border_box.w, 40);
                QL_CHECK_EQ((int)c2->border_box.x, 100);
                QL_CHECK_EQ((int)c2->border_box.w, 200);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 3. colspan / rowspan ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><tr><td colspan=2>a</td></tr>"
            "<tr><td>b</td><td rowspan=2>c</td></tr>"
            "<tr><td>d</td></tr></table>",
            "#t{width:200px}td{padding:0;height:20px}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *a = nth_cell(ui, 0);
            QLUINodeRT *c = nth_cell(ui, 2);   /* b, c, d 里的 c */
            QLUINodeRT *d = nth_cell(ui, 3);
            QL_CHECK(a != NULL && c != NULL && d != NULL);
            if (a != NULL && c != NULL && d != NULL) {
                printf("  colspan：a w=%d；rowspan：c h=%d；d=(%d,%d) %dx%d\n",
                       (int)a->border_box.w, (int)c->border_box.h, (int)d->border_box.x,
                       (int)d->border_box.y, (int)d->border_box.w, (int)d->border_box.h);
                QL_CHECK_EQ((int)a->border_box.w, 200);       /* 跨两列 */
                QL_CHECK_EQ((int)c->border_box.h, 40);        /* 跨两行 */
                QL_CHECK_EQ((int)d->border_box.x, 0);
                QL_CHECK_EQ((int)d->border_box.y, 40);
                QL_CHECK_EQ((int)d->border_box.w, 100);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 4. border-spacing ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><tr><td>a</td><td>b</td></tr></table>",
            "#t{width:204px;border-spacing:4px}td{padding:0;height:10px}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *a = nth_cell(ui, 0);
            QLUINodeRT *b = nth_cell(ui, 1);
            QL_CHECK(a != NULL && b != NULL);
            if (a != NULL && b != NULL) {
                /* 204 - 3*4 = 192 → 每列 96；第二列从 4+96+4 = 104 开始 */
                printf("  spacing=4：a=(%d,%d) %dx%d  b.x=%d\n", (int)a->border_box.x,
                       (int)a->border_box.y, (int)a->border_box.w, (int)a->border_box.h,
                       (int)b->border_box.x);
                QL_CHECK_EQ((int)a->border_box.x, 4);
                QL_CHECK_EQ((int)a->border_box.y, 4);
                QL_CHECK_EQ((int)a->border_box.w, 96);
                QL_CHECK_EQ((int)b->border_box.x, 104);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 5. 像素层：边框 + collapse 单线 ---------- */
    {
        /* separate：两条边框之间有间距（中间是背景色） */
        QLUIRoot *ui = render_html(
            "<table id=t><tr><td>a</td><td>b</td></tr></table>",
            "#t{width:202px;background-color:#FFFFFF;border-spacing:0}"
            "td{padding:0;height:20px;border:1px solid #000000}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *a = nth_cell(ui, 0);
            QL_CHECK(a != NULL);
            if (a != NULL) {
                const int x0 = (int)a->border_box.x;
                const int y0 = (int)a->border_box.y;
                const int w = (int)a->border_box.w;
                /* 左边框黑、内部白、两格之间（separate 且 spacing 0）两条线紧挨 */
                printf("  separate：x0=%d w=%d\n", x0, w);
                QL_CHECK_EQ(QL_COLOR_GET_R(px(x0, y0 + 10)), 0);
                QL_CHECK_EQ(QL_COLOR_GET_R(px(x0 + 10, y0 + 10)), 255);
                ql_ui_free(ui);
            }
        }

        /* collapse：共享边只有一条 1px 黑线，且内部是白的 */
        ui = render_html(
            "<table id=t><tr><td>a</td><td>b</td></tr></table>",
            "#t{width:200px;background-color:#FFFFFF;border-collapse:collapse}"
            "td{padding:0;height:20px;border:1px solid #000000}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const int y = 10;
            /* 外框 + 中间共享的那一条：三条都是黑，且 x=100 不再是黑（没有双线） */
            printf("  collapse：x=0/99/199 黑？%d%d%d   x=100 白？%d\n",
                   QL_COLOR_GET_R(px(0, y)) == 0, QL_COLOR_GET_R(px(99, y)) == 0,
                   QL_COLOR_GET_R(px(199, y)) == 0, QL_COLOR_GET_R(px(100, y)) == 255);
            QL_CHECK_EQ(QL_COLOR_GET_R(px(0, y)), 0);
            QL_CHECK_EQ(QL_COLOR_GET_R(px(99, y)), 0);
            QL_CHECK_EQ(QL_COLOR_GET_R(px(199, y)), 0);
            QL_CHECK_EQ(QL_COLOR_GET_R(px(100, y)), 255);
            /* 单元格内部（避开文字）是白的 */
            QL_CHECK_EQ(QL_COLOR_GET_R(px(60, y)), 255);
            ql_ui_free(ui);
        }
    }

    /* ---------- 6. 单元格垂直对齐（量第一行墨迹） ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><tr><td class=t>x</td><td class=m>x</td>"
            "<td class=b>x</td></tr></table>",
            "#t{width:300px}td{padding:0;height:60px;font-size:12px}"
            ".t{vertical-align:top}.m{vertical-align:middle}.b{vertical-align:bottom}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int first[3] = { -1, -1, -1 };
            for (int i = 0; i < 3; i++) {
                QLUINodeRT *c = nth_cell(ui, i);
                if (c == NULL)
                    continue;
                for (int y = (int)c->border_box.y; y < (int)c->border_box.y +
                                                      (int)c->border_box.h; y++) {
                    int dark = 0;
                    for (int x = (int)c->border_box.x; x < (int)c->border_box.x +
                                                          (int)c->border_box.w; x++) {
                        if (QL_COLOR_GET_R(px(x, y)) < 128)
                            dark = 1;
                    }
                    if (dark) {
                        first[i] = y - (int)c->border_box.y;
                        break;
                    }
                }
            }
            printf("  valign 第一行墨迹：top=%d middle=%d bottom=%d（单元格高 60）\n",
                   first[0], first[1], first[2]);
            QL_CHECK(first[0] >= 0 && first[0] < 6);          /* 贴着上边 */
            QL_CHECK(first[1] > first[0]);                    /* 比 top 低 */
            QL_CHECK(first[2] > first[1]);                    /* 比 middle 更低 */
            QL_CHECK(first[2] > 40);                          /* 贴着下边 */
            ql_ui_free(ui);
        }
    }

    /* ---------- 7. caption 在表格上方、宽度等于表格内容宽 ---------- */
    {
        QLUIRoot *ui = render_html(
            "<table id=t><caption>标题</caption><tr><td>a</td></tr></table>",
            "#t{width:220px}td{padding:0;height:16px}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *cap = nth_display(ui, QL_UI_DISPLAY_TABLE_CAPTION, 0);
            QLUINodeRT *cell = nth_cell(ui, 0);
            QL_CHECK(cap != NULL && cell != NULL);
            if (cap != NULL && cell != NULL) {
                printf("  caption：(%d,%d) %dx%d；第一行 y=%d\n", (int)cap->border_box.x,
                       (int)cap->border_box.y, (int)cap->border_box.w,
                       (int)cap->border_box.h, (int)cell->border_box.y);
                QL_CHECK_EQ((int)cap->border_box.w, 220);
                QL_CHECK_EQ((int)cap->border_box.y, 0);
                QL_CHECK((int)cell->border_box.y >= (int)cap->border_box.h);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 8. 不该支持的写法 ---------- */
    {
        QL_CHECK(rejects("<div id=r></div>", "#r{table-layout:auto-fixed}", "table-layout"));
        QL_CHECK(rejects("<div id=r></div>", "#r{border-collapse:half}", "border-collapse"));
        QL_CHECK(rejects("<div id=r></div>", "#r{border-spacing:10%}", "border-spacing"));
        QL_CHECK(rejects("<div id=r></div>", "#r{display:table-column}", "display"));
        QL_CHECK(rejects("<div id=r>x</div>", "#r{vertical-align:text-top}", "vertical-align"));
    }

    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
