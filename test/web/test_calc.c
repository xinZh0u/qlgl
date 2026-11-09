/**
 * @file    test_calc.c
 * @brief   `calc()` / `min()` / `max()` / `clamp()`。
 *
 * 实现取舍：公式**不在构建期算成数值**（表达式里可能有 `%` / `vw` / `vh`，
 * 只有布局期才知道基准），而是编译成一段**后缀字节码**放在文档级池里，
 * 样式字段只留一个 16 位下标：
 *
 *     #a { width: calc(100% - 40px) }
 *        → style.width = { .value = <池下标>, .unit = QL_UI_UNIT_CALC }
 *        → calc_ops[] = TERM(100,%) TERM(40,px) SUB END
 *
 * 三条硬规矩：
 *   1. 标量乘除、纯数字加减、全常量的 `min()` 在**构建期**折掉，
 *      所以池里只剩"带单位的操作数"（运行期零除法以外的运算成本）；
 *   2. 表达式非法（未知单位、长度×长度、除 0、括号不配对）→ **构建期报错**，
 *      不静默降级；
 *   3. 只在放得下表达式下标的字段上支持（width/height/min/max/inset）。
 *      `font-size` / `border-*` / `margin` / `padding` 只接受能折成 px 的 calc()。
 */
#include <stdlib.h>

#include "../ql_test.h"

typedef struct {
    int x, y, w, h;
} Box;

static int probe_at(const char *html, const char *css, int vw, int vh, int idx,
                    Box *out)
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
    if (idx >= (int)ui->node_count) {
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        return 0;
    }
    const QLRect r = ql_ui_node_rect(&ui->nodes[idx]);
    out->x = r.x;
    out->y = r.y;
    out->w = r.w;
    out->h = r.h;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return 1;
}

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

/* 取样式里的长度字段，确认"是不是表达式" */
static int field_unit(const char *css, uint8_t *unit, int *value)
{
    const char *H = "<div id=r><div id=a>x</div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return 0;
    }
    const QLUIStyle *st = &doc->styles[doc->nodes[1].style];
    *unit = st->width.unit;
    *value = st->width.value;
    ql_ui_doc_free(doc);
    return 1;
}

QL_TEST_BEGIN("test_calc")

    /* 容器固定 200 宽，子项用 calc 从父宽里减 */
    const char *H = "<div id=r><div id=a>x</div></div>";
    const char *BASE =
        "#r { width: 200px; height: 100px; }"
        "#a { height: 10px; }";

    Box b;

    /* ---------- 1. 百分比 ± 像素（最常见的一类） ---------- */
    {
        char css[512];
        snprintf(css, sizeof(css), "%s #a { width: calc(100%% - 40px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 160);

        snprintf(css, sizeof(css), "%s #a { width: calc(25%% + 10px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 60);
    }

    /* ---------- 2. 构建期折叠：标量乘除 / 常量加减 ---------- */
    {
        char css[512];
        snprintf(css, sizeof(css), "%s #a { width: calc(2 * 30px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 60);

        /* 1.5 倍：小数标量按"千分比"折进去，结果四舍五入 */
        snprintf(css, sizeof(css), "%s #a { width: calc(1.5 * 20px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 30);

        snprintf(css, sizeof(css), "%s #a { width: calc(100%% / 4); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 50);

        /* 全常量：构建期折成 px，池里一条都不占 */
        uint8_t unit = 0;
        int value = 0;
        snprintf(css, sizeof(css), "%s #a { width: calc(2 * 3px + 4px); }", BASE);
        QL_CHECK(field_unit(css, &unit, &value));
        QL_CHECK_EQ(unit, QL_UI_UNIT_PX);
        QL_CHECK_EQ(value, 10);
    }

    /* ---------- 3. 真的进池：unit == CALC 且字节码可读 ---------- */
    {
        uint8_t unit = 0;
        int value = 0;
        QL_CHECK(field_unit("#a { width: calc(100% - 40px); }", &unit, &value));
        QL_CHECK_EQ(unit, QL_UI_UNIT_CALC);
        QL_CHECK(value >= 0);
    }

    /* ---------- 4. min() / max() / clamp() ---------- */
    {
        char css[512];
        snprintf(css, sizeof(css), "%s #a { width: min(80px, 50%%); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 80);          /* 50% = 100 → 取 80 */

        snprintf(css, sizeof(css), "%s #a { width: min(180px, 50%%); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 100);         /* 50% 更小 */

        snprintf(css, sizeof(css), "%s #a { width: max(180px, 50%%); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 180);

        snprintf(css, sizeof(css), "%s #a { width: clamp(90px, 10%%, 150px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 90);          /* 10% = 20 → 抬到下限 90 */

        snprintf(css, sizeof(css), "%s #a { width: clamp(90px, 40%%, 150px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 90);          /* 40% = 40 → 仍是下限 */

        snprintf(css, sizeof(css), "%s #a { width: clamp(90px, 60%%, 150px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 120);         /* 60% = 60 … 但 200*0.6 = 120，落在区间内 */

        snprintf(css, sizeof(css), "%s #a { width: clamp(90px, 90%%, 150px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 150);         /* 90% = 180 → 压到上限 150 */

        /* 嵌套：min() 出现在 calc() 里，clamp() 里套 calc() */
        snprintf(css, sizeof(css),
                 "%s #a { width: calc(min(120px, 50%%) + 10px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 110);

        snprintf(css, sizeof(css),
                 "%s #a { width: clamp(20px, calc(50%% - 10px), 80px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 80);
    }

    /* ---------- 5. 视口单位 ---------- */
    {
        const char *H2 = "<div id=a>x</div>";
        QL_CHECK(probe_at(H2, "#a { width: calc(50vw - 10px); height: 10px; }",
                          400, 300, 0, &b));
        QL_CHECK_EQ(b.w, 190);

        QL_CHECK(probe_at(H2, "#a { width: calc(2 * 10vh); height: 10px; }",
                          400, 300, 0, &b));
        QL_CHECK_EQ(b.w, 60);

        /* vmin / vmax */
        QL_CHECK(probe_at(H2, "#a { width: calc(10vmin + 0px); height: 10px; }",
                          400, 300, 0, &b));
        QL_CHECK_EQ(b.w, 30);
        QL_CHECK(probe_at(H2, "#a { width: calc(10vmax + 0px); height: 10px; }",
                          400, 300, 0, &b));
        QL_CHECK_EQ(b.w, 40);
    }

    /* ---------- 6. em / rem：构建期折（em 用元素字号，rem 用根字号） ---------- */
    {
        char css[512];
        snprintf(css, sizeof(css),
                 "%s #a { font-size: 20px; width: calc(1em + 4px); }", BASE);
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 24);

        snprintf(css, sizeof(css),
                 "#r { width: 200px; height: 100px; font-size: 10px; }"
                 "#a { height: 10px; font-size: 20px; width: calc(1em + 2rem); }");
        QL_CHECK(probe_at(H, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.w, 40);          /* 1em=20 + 2rem=20 */
    }

    /* ---------- 7. inset（绝对定位） ---------- */
    {
        const char *H2 = "<div id=r><div id=a>x</div></div>";
        const char *css =
            "#r { width: 200px; height: 100px; position: relative; }"
            "#a { position: absolute; top: 0; bottom: 0;"
            "     left: calc(50% - 20px); right: 10px; }";
        QL_CHECK(probe_at(H2, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.x, 80);
        QL_CHECK_EQ(b.w, 110);         /* 200 - 80 - 10 */
    }

    /* ---------- 8. 百分比高度：父级不定 → 退回 auto ---------- */
    {
        const char *H2 = "<div id=r><div id=a>x</div></div>";
        /* 父级 auto 高度：calc(50% - 5px) 应当当作 auto（高度由内容撑开） */
        const char *css =
            "#r { width: 200px; }"
            "#a { font-size: 16px; line-height: 20px; width: 50px;"
            "     height: calc(50% - 5px); }";
        QL_CHECK(probe_at(H2, css, 300, 300, 1, &b));
        QL_CHECK_EQ(b.h, 20);          /* 一行文本的高度，而不是 -5 */
    }

    /* ---------- 9. 多断点 / 状态变体里也能用 calc ---------- */
    {
        const char *H2 = "<div id=r><div id=a>x</div></div>";
        const char *css =
            "#r { width: 100%; height: 100px; }"
            "#a { width: calc(100% - 100px); height: 10px; }"
            "@media (max-width: 500px) {"
            "  #a { width: calc(50% - 10px); }"
            "}";
        QL_CHECK(probe_at(H2, css, 400, 300, 1, &b));
        QL_CHECK_EQ(b.w, 190);         /* 400*0.5 - 10 */
        QL_CHECK(probe_at(H2, css, 600, 300, 1, &b));
        QL_CHECK_EQ(b.w, 500);         /* 600 - 100 */
    }

    /* ---------- 10. blob 往返：字节码池要跟着走 ---------- */
    {
        const char *H2 = "<div id=r><div id=a>x</div></div>";
        const char *css =
            "#r { width: 200px; height: 100px; }"
            "#a { width: calc(100% - 40px); height: max(30px, 10vh); }";
        QLUIDoc *doc = ql_ui_load_text(H2, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK(doc->calc_op_count > 0);
            QL_CHECK(doc->calc_ops != NULL);
            const size_t need = ql_ui_doc_encode(doc, NULL, 0);
            QL_CHECK(need > sizeof(QLUIDocBlobHeader));
            uint8_t *buf = (uint8_t *)malloc(need);
            QL_CHECK(buf != NULL);
            if (buf != NULL) {
                QL_CHECK_EQ(ql_ui_doc_encode(doc, buf, need), need);
                QLUIDoc *back = ql_ui_doc_load_mem(buf, need);
                QL_CHECK(back != NULL);
                if (back != NULL) {
                    QL_CHECK_EQ(back->calc_op_count, doc->calc_op_count);
                    QL_CHECK(back->calc_ops != NULL);
                    if (back->calc_ops != NULL) {
                        int same = 1;
                        for (uint16_t i = 0; i < doc->calc_op_count; i++) {
                            const QLUICalcOp *x = &doc->calc_ops[i];
                            const QLUICalcOp *y = &back->calc_ops[i];
                            if (x->op != y->op || x->unit != y->unit ||
                                x->value != y->value)
                                same = 0;
                        }
                        QL_CHECK(same);
                    }
                    /* 加载回来的 doc 布局结果必须与原来一致 */
                    QLUIRoot *ui = ql_ui_build(back);
                    QL_CHECK(ui != NULL);
                    if (ui != NULL) {
                        ql_ui_layout(ui, 300, 300);
                        QL_CHECK_EQ(ql_ui_node_rect(&ui->nodes[1]).w, 160);
                        ql_ui_free(ui);
                    }
                    ql_ui_doc_free(back);
                }
                free(buf);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 11. 构建期必须报错的写法 ---------- */
    QL_CHECK(rejects(H, "#a { width: calc(10px * 2px); }", "长度相乘"));
    QL_CHECK(rejects(H, "#a { width: calc(10px + 2); }", "纯数字与长度"));
    QL_CHECK(rejects(H, "#a { width: calc(10px - ; }", "数字"));
    QL_CHECK(rejects(H, "#a { width: calc(10qq + 2px); }", "单位"));
    QL_CHECK(rejects(H, "#a { width: calc(10px / 0); }", "除以 0"));
    QL_CHECK(rejects(H, "#a { width: calc(100% - 2px; }", ")"));
    QL_CHECK(rejects(H, "#a { width: clamp(1px, 2px); }", "3 个参数"));
    QL_CHECK(rejects(H, "#a { margin: calc(100% - 2px); }", "百分比"));
    QL_CHECK(rejects(H, "#a { font-size: calc(1em + 2px); }", "px"));
    QL_CHECK(rejects(H, "#a { width: calc(50% - 2px) extra; }", "多余内容"));

QL_TEST_END()
