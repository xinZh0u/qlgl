/**
 * @file    test_pseudo_text.c
 * @brief   宿主自己有文本时 `::before` / `::after` 的排布与计数。
 *
 * 这里踩过的坑：混排（文本 + 子元素）本来由 `flatten_node` 在解析期拆成匿名
 * `<span>`，但伪元素是**之后**才由 `expand_pseudo` 造出来的 —— 于是
 * `<div class="b">正文</div>` + `.b::before` 变成"文本留在宿主上、伪元素节点
 * 又从内容盒原点开始排"，两者叠在同一个位置（圆点被正文盖掉），
 * `::before` 也失去了"在正文之前"的含义。
 *
 * 现在宿主文本会被挪进一个匿名 span（`is_anon_text`），走的是与混排完全相同的
 * 那条路。它**不是元素**：不参与 `:first-child` / `+` / `~` 的匹配与计数。
 */
#include "../ql_test.h"

typedef struct { int x, y, w, h; } Box;

static int layout_boxes(const char *html, const char *css, int vw, int vh, Box *out,
                        int want)
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
    int ok = 1;
    for (int i = 0; i < want; i++) {
        if (i >= (int)ui->node_count) {
            ok = 0;
            break;
        }
        const QLRect r = ql_ui_node_rect(&ui->nodes[i]);
        out[i].x = r.x;
        out[i].y = r.y;
        out[i].w = r.w;
        out[i].h = r.h;
    }
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return ok;
}

/* 节点 i 的文本（没有文本返回空串） */
static const char *node_text_of(const char *html, const char *css, int i)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL)
        return "";
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return "";
    }
    static char buf[256];
    const char *t = (i < (int)ui->node_count)
                        ? ql_ui_node_text(&ui->nodes[i])
                        : NULL;
    snprintf(buf, sizeof(buf), "%s", (t != NULL) ? t : "");
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return buf;
}

QL_TEST_BEGIN("test_pseudo_text")

    /*
     * ---------- 1. `::before` + 宿主文本：伪元素在前、正文紧接着 ----------
     * 节点顺序由 DFS 决定：0=#r，1=#a，2=::before 的合成节点，3=匿名文本 span。
     */
    Box b[4];
    const char *H1 = "<div id=r><div id=a>正文</div></div>";
    const char *C1 = "#r { width: 200px }"
                     "#a { font-size: 10px }"
                     "#a::before { content: '* ' }";
    QL_CHECK(layout_boxes(H1, C1, 200, 100, b, 4));
    QL_CHECK_EQ(b[2].y, b[3].y);          /* 同一行 */
    QL_CHECK_EQ(b[3].x, b[2].x + b[2].w); /* 正文从伪元素的右边缘开始，不重叠 */
    QL_CHECK_EQ(b[2].x, b[1].x);          /* 伪元素在内容盒原点 */
    QL_CHECK(strcmp(node_text_of(H1, C1, 2), "* ") == 0);
    QL_CHECK(strcmp(node_text_of(H1, C1, 3), "正文") == 0);

    /*
     * ---------- 2. `::after` + 宿主文本：正文在前、伪元素在最后 ----------
     */
    Box b2[4];
    const char *C2 = "#r { width: 200px }"
                     "#a { font-size: 10px }"
                     "#a::after { content: '!' }";
    QL_CHECK(layout_boxes(H1, C2, 200, 100, b2, 4));
    QL_CHECK_EQ(b2[3].x, b2[2].x + b2[2].w); /* 0-1 是宿主链，2=正文，3=伪元素 */
    QL_CHECK_EQ(b2[3].y, b2[2].y);

    /*
     * ---------- 3. 匿名文本不算元素：`:first-child` 与 `+` 都不该命中它 ----------
     * 左：真 span 是唯一的元素子节点 → 吃 margin-left。
     * 若匿名文本被当成元素，它会抢走 `:first-child`，这两条断言都会红。
     */
    Box b3[5];
    const char *H3 = "<div id=r><div id=a><span id=s>x</span></div></div>";
    const char *C3 = "#r { width: 200px }"
                     "#a { font-size: 10px }"
                     "#a::before { content: '.' }"
                     "#a span:first-child { margin-left: 7px }";
    QL_CHECK(layout_boxes(H3, C3, 200, 100, b3, 4));
    QL_CHECK_EQ(b3[3].x - b3[2].x - b3[2].w, 7); /* 伪元素 → 7px 间隔 → 真 span */

    /*
     * `+` 只看元素兄弟：伪元素节点和匿名文本都不算。
     * 节点顺序 0=#r，1=#a，2=伪元素，3=#a 的正文，4=#b。
     */
    Box b4[5];
    const char *H4 = "<div id=r><div id=a>文字</div><div id=b>y</div></div>";
    const char *C4 = "#r { width: 200px }"
                     "#a::before { content: '.' }"
                     "#a + #b { margin-left: 9px }";
    QL_CHECK(layout_boxes(H4, C4, 200, 100, b4, 5));
    QL_CHECK_EQ(b4[4].x - b4[1].x, 9);

QL_TEST_END()
