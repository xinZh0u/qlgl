/**
 * @file    11_semantic.c
 * @brief   纯语义标签：真实 HTML 片段贴进来就能画。
 *
 *   ./build/example/example_11_semantic                    # 打开窗口
 *   ./build/example/example_11_semantic --dump /tmp/a.ppm   # 截图模式
 *
 * 这些标签**不引入任何新能力** —— UA 默认样式只有 block / inline，加它们的目的是
 * "别报错"。所以这个例程的重点是：哪些能看出区别，哪些看不出。
 *
 *   能看出区别：<dl>/<dd> 的缩进、<cite>/<var>/<dfn> 的斜体、
 *               <del> 的删除线、<ins> 的下划线、<fieldset> 的边框
 *   看不出区别：<a href>（没有导航）、<abbr title>（没有 tooltip）、
 *               <code>/<kbd>/<samp>（没有等宽字体）、<q>（没有生成引号）、
 *               <time>/<data>（属性被忽略）
 */
#include "common.h"

#define W 320
#define H 348

static const char *k_html =
    "<div id=\"root\">"
    "  <h2>变更说明</h2>"
    "  <p class=\"lead\">这一版把 <code>ql_ui_scroll_by()</code> 的夹紧行为改了，"
    "     参考 <cite>QLGL 开发规划</cite> 的 ⑭ 一节。</p>"

    "  <dl>"
    "    <dt>新增</dt>"
    "    <dd><ins>滚动容器支持横向滚动</ins></dd>"
    "    <dt>修复</dt>"
    "    <dd><del>列表滚到底会卡在越界位置</del></dd>"
    "    <dt>说明</dt>"
    "    <dd>按下 <kbd>PageDown</kbd> 翻一屏，<small>（约 140px）</small></dd>"
    "  </dl>"

    "  <blockquote>滚动偏移烘进布局，所以绘制、命中、脏区都不用改。</blockquote>"

    "  <fieldset>"
    "    <legend>兼容性</legend>"
    "    <p class=\"note\">发布于 <time>2026-11-08</time>，"
    "       适用于 <abbr>QLGL</abbr> 0.1 之后的所有版本。</p>"
    "  </fieldset>"

    "  <address>维护：<a>qlgl-dev</a> · 反馈请提 issue</address>"
    "</div>";

static const char *k_css =
    "#root { width: 320px; height: 348px; background-color: #0B0E14;"
    "        padding: 14px; font-size: 12px; color: #C8D2DE; }"
    "h2 { font-size: 16px; color: #7CE0FF; margin: 0 0 8px 0; }"
    ".lead { line-height: 20px; margin: 0 0 10px 0; }"
    "code { color: #7CE0FF; }"
    "cite { color: #9AA7B4; }"
    "dl { margin: 0 0 10px 0; }"
    "dt { color: #566270; }"
    "dd { line-height: 18px; }"
    "ins { color: #4ADE80; }"
    "del { color: #F87171; }"
    "blockquote { margin: 0 0 10px 0; padding-left: 10px;"
    "             border-width: 2px; border-color: #2A3646; color: #9AA7B4; }"
    "fieldset { margin: 0 0 10px 0; }"
    "legend { color: #7CE0FF; }"
    ".note { line-height: 18px; margin: 0; color: #C8D2DE; }"
    "address { font-size: 10px; color: #566270; }";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_load_font();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);
    printf("%d 个节点，%d 个不同标签\n", (int)doc->node_count,
           (int)QL_UI_TAG_COUNT);

    const int rc = ex_run(ui, W, H, "QLGL · 11 semantic", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
