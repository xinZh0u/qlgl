/**
 * @file    test_css_vars.c
 * @brief   CSS 自定义属性 `--x` 与 `var()`（构建期替换）。
 *
 * 为什么这件事在 qlgl 里**便宜**：样式本来就是编译产物，`--x` 没有"运行期改值"这条路
 * （没有 JS、没有运行期样式表），所以整个特性只是"级联之后再查一次表"：
 *
 *     UA 默认 + 级联 + 行内 + 继承  → 得 out
 *     out 里先把 `--x` 自己的值解析掉 → 再把所有 `var()` 换成字面量
 *
 * 下游 `compile_style()` 完全不知道变量存在，`QLUIStyle` 一个字节都没变。
 *
 * 三条硬规矩：
 *   1. `--x` **继承**（与 CSS 一致），后代可以覆盖；
 *   2. 找不到且没有 fallback → **构建期报错**（不静默留成字面量）；
 *   3. 循环引用靠深度上限挡住并报错，不会挂死。
 */
#include "../ql_test.h"

typedef struct {
    unsigned text_color;
    unsigned bg_color;
    unsigned border_color;
    int      border_width;
    int      margin[4];
    int      padding[4];
} Probe;

/* 布局一份界面，取第 idx 个节点的样式快照 */
static int probe_of(const char *html, const char *css, int idx, Probe *out)
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
    if (idx >= (int)ui->node_count) {
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        return 0;
    }
    const QLUIStyle *st = ql_ui_node_style(&ui->nodes[idx]);
    out->text_color = st->text_color;
    out->bg_color = st->bg_color;
    out->border_color = st->border_color;
    out->border_width = st->border_width;
    out->margin[0] = st->margin.top;
    out->margin[1] = st->margin.right;
    out->margin[2] = st->margin.bottom;
    out->margin[3] = st->margin.left;
    out->padding[0] = st->padding.top;
    out->padding[1] = st->padding.right;
    out->padding[2] = st->padding.bottom;
    out->padding[3] = st->padding.left;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return 1;
}

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

QL_TEST_BEGIN("test_css_vars")

    const char *H = "<div id=r><div id=a>x</div><div id=b>y</div></div>";
    Probe p;

    /* ---------- 1. 基本替换 ---------- */
    QL_CHECK(probe_of(H, "#r { --brand: #0969DA } #a { color: var(--brand) }", 1, &p));
    QL_CHECK_EQ(p.text_color, 0x0969DAu);

    /* ---------- 2. 继承：在祖先身上定义，后代能用 ---------- */
    QL_CHECK(probe_of(H, "#r { --brand: #0969DA } #b { color: var(--brand) }", 2, &p));
    QL_CHECK_EQ(p.text_color, 0x0969DAu);

    /* ---------- 3. 覆盖：后代自己定义的不影响兄弟 ---------- */
    QL_CHECK(probe_of(H,
                      "#r { --brand: #0969DA }"
                      "#a { --brand: #B42318; color: var(--brand) }"
                      "#b { color: var(--brand) }",
                      1, &p));
    QL_CHECK_EQ(p.text_color, 0xB42318u);      /* a 用自己被覆盖的值 */
    QL_CHECK(probe_of(H,
                      "#r { --brand: #0969DA }"
                      "#a { --brand: #B42318; color: var(--brand) }"
                      "#b { color: var(--brand) }",
                      2, &p));
    QL_CHECK_EQ(p.text_color, 0x0969DAu);      /* b 仍是祖先的值 */

    /* ---------- 4. fallback：缺了用默认值，有定义时 fallback 不生效 ---------- */
    QL_CHECK(probe_of(H, "#a { padding: var(--missing, 4px) }", 1, &p));
    QL_CHECK_EQ(p.padding[0], 4);
    QL_CHECK_EQ(p.padding[3], 4);
    QL_CHECK(probe_of(H, "#a { --pad: 9px; padding: var(--missing, 4px) }", 1, &p));
    QL_CHECK_EQ(p.padding[0], 4);              /* fallback 里的名字仍缺 → 4 */
    QL_CHECK(probe_of(H, "#a { --pad: 9px; padding: var(--pad, 4px) }", 1, &p));
    QL_CHECK_EQ(p.padding[0], 9);              /* 变量有值 → 用 9 */

    /* ---------- 5. 简写里也能用（先展开成长属性，再替换） ---------- */
    QL_CHECK(probe_of(H,
                      "#a { --c: #1A7F37; border: 2px solid var(--c) }", 1, &p));
    QL_CHECK_EQ(p.border_width, 2);
    QL_CHECK_EQ(p.border_color, 0x1A7F37u);

    QL_CHECK(probe_of(H, "#a { --bg: #F6F8FA; background: var(--bg) }", 1, &p));
    QL_CHECK_EQ(p.bg_color, 0xF6F8FAu);

    QL_CHECK(probe_of(H, "#a { --g: 12px; margin: var(--g) }", 1, &p));
    QL_CHECK_EQ(p.margin[0], 12);
    QL_CHECK_EQ(p.margin[1], 12);
    QL_CHECK_EQ(p.margin[2], 12);
    QL_CHECK_EQ(p.margin[3], 12);

    /* ---------- 6. 替换发生在"值解析"之前：em 照样按元素字号折 ---------- */
    QL_CHECK(probe_of(H,
                      "#a { font-size: 20px; --pad: 2em; padding: var(--pad) }", 1, &p));
    QL_CHECK_EQ(p.padding[0], 40);             /* 2em × 20px */

    /* ---------- 7. 嵌套：--a 引用 --b ---------- */
    QL_CHECK(probe_of(H,
                      "#r { --b: #8250DF } #a { --a: var(--b); color: var(--a) }", 1, &p));
    QL_CHECK_EQ(p.text_color, 0x8250DFu);

    /* ---------- 8. 行内 style 里也能用 ---------- */
    {
        const char *H2 = "<div id=r><div id=a style='color: var(--c)'>x</div></div>";
        QL_CHECK(probe_of(H2, "#r { --c: #BC4C00 }", 1, &p));
        QL_CHECK_EQ(p.text_color, 0xBC4C00u);
    }

    /* ---------- 9. `:root`：真实页面声明变量的地方（Bootstrap 的写法） ---------- */
    QL_CHECK(probe_of(H, ":root { --c: #1F883D } #a { color: var(--c) }", 1, &p));
    QL_CHECK_EQ(p.text_color, 0x1F883Du);

    /* 完整的 `<html><body>` 文档里一样成立（真实网页就是这个形状） */
    {
        Probe q;
        QL_CHECK(probe_of("<html><body><div id=r><div id=a>x</div></div></body></html>",
                          ":root { --c: #BC4C00 } #a { color: var(--c) }", 3, &q));
        QL_CHECK_EQ(q.text_color, 0xBC4C00u);
    }

    /* ---------- 10. 三条报错路径 ---------- */
    QL_CHECK(rejects("#a { color: var(--nope) }", "没有定义"));
    QL_CHECK(rejects("#r { --a: var(--b); --b: var(--a) } #a { color: var(--a) }",
                     "循环引用"));
    QL_CHECK(rejects("#a::before { content: var(--icon) }", "content 不支持 var()"));

QL_TEST_END()
