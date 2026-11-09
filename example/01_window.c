/**
 * @file    01_window.c
 * @brief   最小的 QLGL 程序：编译一份界面，显示出来。
 *
 * 编译运行：
 *   cmake -S . -B build && cmake --build build -j
 *   ./build/example/01_window --dump /tmp/01.ppm
 *
 * 这个程序做了四件事，多一件也没有：
 *   1. `qlgl_init()`      初始化库
 *   2. `ql_ui_load_text()` 把 HTML+CSS 编译成界面数据（doc）
 *   3. `ql_ui_build()`    用 doc 建出运行期节点树，然后布局一次
 *   4. 交给窗口 / 或者离屏渲染成图片
 */
#include "common.h"

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"hello\">Hello, QLGL</div>"
    "</div>";

static const char *k_css =
    "#root { width: 100%; height: 100%; box-sizing: border-box; background-color: #FFFFFF; }"
    ".hello { margin: 40px 0 0 24px; font-size: 20px; color: #0969DA; }";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    /* ① 编译界面。失败时 last_error 里有 "css:行号: 原因" */
    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }

    /* ② 建运行期树并布局（视口 240x120，与 #root 一致） */
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        printf("ql_ui_build 失败\n");
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }

    printf("%u 个节点 / %u 条样式\n", (unsigned)doc->node_count,
           (unsigned)doc->style_count);

    /* ③ 跑起来 */
    const int rc = ex_run(ui, 240, 120, "QLGL · 01 window", argc, argv);

    /* ④ 收尾：先释放运行期树，再释放 doc（doc 是独立的所有权） */
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
