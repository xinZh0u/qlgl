/**
 * @file    test_media.c
 * @brief   `@media (min-width/max-width)`：按视口宽切断言点区间。
 *
 * 实现思路（与"状态变体"同构）：
 *   1. 构建期把条件在**宽度轴**上切成互不相交的区间（`QLUIMedia{lo,hi}`）；
 *   2. 每个区间编译一套样式变体（base / hover / active / focus）；
 *   3. 运行期 `ql_ui_layout(ui, w, h)` 按 w 选区间下标，把变体装回节点。
 *
 * 所以运行期**没有条件表达式求值**，也没有重新解析 CSS。
 *
 * 三条要守住的语义：
 *   - 同一个宽度下，`media == -1` 的规则与匹配的媒体块**一起**参与级联；
 *   - 后代元素继承的是**同一区间**的父级值（不是别的区间的）；
 *   - 没有 `@media` 的 doc，行为与以前逐字节相同（golden 守着）。
 */
#include "../ql_test.h"

typedef struct {
    int x, y, w, h;
    unsigned bg;
    unsigned text_color;
} Box;

/* 按指定视口宽布局，取第 idx 个节点 */
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
    const QLUIStyle *st = ql_ui_node_style(&ui->nodes[idx]);
    out->x = r.x;
    out->y = r.y;
    out->w = r.w;
    out->h = r.h;
    out->bg = st->bg_color;
    out->text_color = st->text_color;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return 1;
}

/* 编译期必须失败，且报错里含 needle */
static int rejects(const char *css, const char *needle)
{
    const char *H = "<div id=r><div id=a>x</div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
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

QL_TEST_BEGIN("test_media")

    const char *H = "<div id=r><div id=a>x</div></div>";
    const char *CSS =
        "#r { width: 100%; }"
        "#a { width: 200px; height: 30px; background-color: #0969DA; }"
        "@media (max-width: 500px) {"
        "  #a { width: 100px; height: 20px; background-color: #B42318; }"
        "}";

    Box b;

    /* ---------- 1. 窄视口：媒体块生效 ---------- */
    QL_CHECK(probe_at(H, CSS, 400, 300, 1, &b));
    QL_CHECK_EQ(b.w, 100);
    QL_CHECK_EQ(b.h, 20);
    QL_CHECK_EQ(b.bg, 0xB42318u);

    /* ---------- 2. 宽视口：媒体块不生效，回到基础样式 ---------- */
    QL_CHECK(probe_at(H, CSS, 800, 300, 1, &b));
    QL_CHECK_EQ(b.w, 200);
    QL_CHECK_EQ(b.h, 30);
    QL_CHECK_EQ(b.bg, 0x0969DAu);

    /* 边界：500 命中（max-width 是闭区间），501 不命中 */
    QL_CHECK(probe_at(H, CSS, 500, 300, 1, &b));
    QL_CHECK_EQ(b.w, 100);
    QL_CHECK(probe_at(H, CSS, 501, 300, 1, &b));
    QL_CHECK_EQ(b.w, 200);

    /* ---------- 3. min-width，以及两个断点同时写 ---------- */
    const char *CSS2 =
        "#a { width: 300px; background-color: #0969DA; }"
        "@media (min-width: 600px) { #a { width: 250px; } }"
        "@media (min-width: 900px) { #a { width: 200px;"
        " background-color: #1A7F37; } }";
    QL_CHECK(probe_at(H, CSS2, 400, 300, 1, &b));
    QL_CHECK_EQ(b.w, 300);
    QL_CHECK_EQ(b.bg, 0x0969DAu);
    QL_CHECK(probe_at(H, CSS2, 700, 300, 1, &b));
    QL_CHECK_EQ(b.w, 250);                 /* 只有第一个断点生效 */
    QL_CHECK_EQ(b.bg, 0x0969DAu);
    QL_CHECK(probe_at(H, CSS2, 1000, 300, 1, &b));
    QL_CHECK_EQ(b.w, 200);                 /* 两个都生效，后者按源码顺序赢 */
    QL_CHECK_EQ(b.bg, 0x1A7F37u);

    /* ---------- 4. 与普通规则交错：源码顺序决定谁赢 ---------- */
    const char *CSS3 =
        "@media (max-width: 400px) { #a { color: #B42318; } }"
        "#a { color: #0969DA; }";              /* 写在媒体块之后 → 赢 */
    QL_CHECK(probe_at(H, CSS3, 300, 300, 1, &b));
    QL_CHECK_EQ(b.text_color, 0x0969DAu);

    /* ---------- 5. 继承走同一区间：父级的媒体值能传给子级 ---------- */
    const char *H5 = "<div id=r><div id=p><div id=c>x</div></div></div>";
    const char *CSS5 =
        "#p { color: #0969DA; }"                 /* color 是继承属性 */
        "#c { width: 50px; height: 10px; }"
        "@media (max-width: 400px) { #p { color: #B42318; } }";
    QL_CHECK(probe_at(H5, CSS5, 300, 300, 2, &b));
    QL_CHECK_EQ(b.text_color, 0xB42318u);      /* 窄视口：继承到媒体值 */
    QL_CHECK(probe_at(H5, CSS5, 800, 300, 2, &b));
    QL_CHECK_EQ(b.text_color, 0x0969DAu);

    /* ---------- 6. 断点里改 :hover —— 悬停不能把响应式尺寸丢掉 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text(
            H, "#r { width: 100% }"
               "#a { width: 200px; height: 30px; background-color: #0969DA; }"
               "#a:hover { background-color: #1F883D; }"
               "@media (max-width: 500px) { #a { width: 120px; } }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, 400, 300);
                QL_CHECK_EQ(ql_ui_node_rect(&ui->nodes[1]).w, 120);
                /* 伪造成 hover */
                ui->nodes[1].flags |= 0x01u;   /* QL_UI_RT_HOVER */
                const QLUIStyle *hs = ql_ui_node_style(&ui->nodes[1]);
                QL_CHECK_EQ(hs->bg_color, 0x1F883Du);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. `print` 整块跳过（不是报错） ---------- */
    QL_CHECK(probe_at(H,
                      "@media print { #a { width: 999px } }"
                      "#a { width: 10px }", 800, 300, 1, &b));
    QL_CHECK_EQ(b.w, 10);

    /* ---------- 8. 报错路径 ---------- */
    QL_CHECK(rejects("@media (orientation: landscape) { #a { width: 1px } }",
                     "只支持 min-width / max-width"));
    QL_CHECK(rejects("@media (min-width: 10em) { #a { width: 1px } }", "只支持 px"));
    QL_CHECK(rejects("@media screen { #a { width: 1px } }", "没有条件"));

QL_TEST_END()
