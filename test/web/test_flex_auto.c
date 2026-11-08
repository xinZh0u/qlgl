/**
 * @file    test_flex_auto.c
 * @brief   flex 项的 `width: auto` 语义 = **max-content**（不是"撑满容器"）。
 *
 * 这是一条把真实 bug 钉住的回归测试：
 *
 *   一个空的 `flex-grow: 1` 占位块，它的 flex 基准尺寸曾经被算成
 *   整个容器的宽度（block 布局的语义），于是"基尺寸之和 > 容器" →
 *   白算出一份负剩余空间 → 触发 shrink → 把旁边**固定宽度**的兄弟一起压扁。
 *   实测 `40px` 变 29px、`56px` 变 40px，浏览器里这两个值根本不该变。
 *
 * 正确语义（CSS Flexbox §9.2）：`flex-basis: auto` 且 `width: auto` 时，
 * 基准尺寸是这个项的 **max-content 尺寸**：
 *   - 空盒子 → 0
 *   - 只有文本 → 文本宽度
 *   - 有子节点 → 主轴方向上"子项之和（同向 flex）/ 最大值（纵向堆叠）"
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

/* 渲染一次，返回第 idx 个节点的 border_box 宽度（-1 = 失败） */
static int width_of(const char *html, const char *css, int w, int h, uint16_t idx,
                    QLRect *out_full)
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
    ql_ui_layout(ui, w, h);
    QLRect b = ql_ui_node_rect(&ui->nodes[idx]);
    if (out_full != NULL)
        *out_full = b;
    const int r = (int)b.w;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return r;
}

QL_TEST_BEGIN("test_flex_auto")

    /* ---------- 1. 固定宽 + flex-grow 占位块（回归用例） ---------- */
    {
        const char *html = "<div id=r><div class=\"a\"></div><div class=\"s\"></div>"
                           "<div class=\"b\"></div></div>";
        const char *css = "#r { width: 300px; height: 20px; display: flex; gap: 8px; }"
                          ".a { width: 40px; height: 20px; }"
                          ".s { flex-grow: 1; }"
                          ".b { width: 56px; height: 20px; }";
        QLRect a, s, b;
        QL_CHECK_EQ(width_of(html, css, 300, 20, 1, &a), 40);
        QL_CHECK_EQ(width_of(html, css, 300, 20, 2, &s), 188);
        QL_CHECK_EQ(width_of(html, css, 300, 20, 3, &b), 56);
        /* 三者 + 两个 gap 正好等于容器：grow 吸收全部剩余，一个像素都不差 */
        printf("  40 + %d + 56 + 2*8 = %d（容器 300）\n", (int)s.w,
               (int)(a.w + s.w + b.w + 16));
        QL_CHECK_EQ(a.w + s.w + b.w + 16, 300);
        /* 位置也要对：a 在 0，b 贴右边界 */
        QL_CHECK_EQ(a.x, 0);
        QL_CHECK_EQ(b.x + b.w, 300);
    }

    /* ---------- 2. 空的 auto 项基准是 0，不是容器宽 ---------- */
    {
        const char *html = "<div id=r><div class=\"e\"></div><div class=\"b\"></div></div>";
        const char *css = "#r { width: 200px; height: 20px; display: flex; gap: 4px;"
                          "      justify-content: flex-start; }"
                          ".e { height: 20px; }"
                          ".b { width: 60px; height: 20px; }";
        QL_CHECK_EQ(width_of(html, css, 200, 20, 1, NULL), 0);
        /* 固定宽的那项不该被压 */
        QL_CHECK_EQ(width_of(html, css, 200, 20, 2, NULL), 60);
    }

    /* ---------- 3. 只有文本的 auto 项：基准是文本宽度 ---------- */
    {
        const char *html = "<div id=r><div class=\"t\">AAAA</div>"
                           "<div class=\"s\"></div><div class=\"b\"></div></div>";
        const char *css = "#r { width: 300px; height: 24px; display: flex; gap: 6px;"
                          "      font-size: 16px; }"
                          ".t { }"
                          ".s { flex-grow: 1; }"
                          ".b { width: 50px; height: 24px; }";
        QLRect t;
        const int tw = width_of(html, css, 300, 24, 1, &t);
        printf("  文本项宽度 = %d（\"AAAA\" @16px，应远小于 300）\n", tw);
        QL_CHECK(tw > 0 && tw < 100);
        QL_CHECK_EQ(width_of(html, css, 300, 24, 3, NULL), 50);
        /* 文本项 + 间距 + 伸缩块 + 间距 + 固定块 == 容器 */
        QL_CHECK_EQ(width_of(html, css, 300, 24, 2, NULL) + tw + 50 + 12, 300);
    }

    /* ---------- 4. box-sizing: content-box 时，带边框的项按 22px 算 ---------- */
    {
        const char *html = "<div id=r><div class=\"c\"></div></div>";
        const char *css = "#r { width: 200px; height: 40px; display: flex; }"
                          ".c { height: 20px; border-width: 1px; background-color: #fff; }";
        QLRect c;
        (void)width_of(html, css, 200, 40, 1, &c);
        printf("  content-box + 1px 边框：高 %d（应为 22）\n", (int)c.h);
        QL_CHECK_EQ(c.h, 22);
    }

    /* ---------- 5. flex-basis 显式给了就不用 max-content ---------- */
    {
        const char *html = "<div id=r><div class=\"a\"></div><div class=\"b\"></div></div>";
        const char *css = "#r { width: 200px; height: 20px; display: flex; gap: 0px; }"
                          ".a { flex: 1 1 0px; height: 20px; }"
                          ".b { flex: 1 1 0px; height: 20px; }";
        QL_CHECK_EQ(width_of(html, css, 200, 20, 1, NULL), 100);
        QL_CHECK_EQ(width_of(html, css, 200, 20, 2, NULL), 100);
    }

QL_TEST_END()
