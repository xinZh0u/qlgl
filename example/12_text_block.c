/**
 * @file    12_text_block.c
 * @brief   TEXT 包：`<pre>` 空白保留、`<sub>`/`<sup>` 上下标。
 *
 *   ./build/example/example_12_text_block                    # 打开窗口
 *   ./build/example/example_12_text_block --dump /tmp/a.ppm   # 截图
 *
 * 这一期只加两件事，但两件都改变了"文本怎么排"：
 *
 *   1. `white-space: pre` —— 空白与换行**全部保留**、不自动折行。
 *      `<pre>` 的 UA 默认就是它，所以代码块直接能贴。
 *      代价：行首空格是内容的一部分，所以**不能靠缩进美化 `<pre>` 的源码**。
 *   2. `vertical-align: sub / super` —— 基线偏移（按字号比例），
 *      配合 `font-size: smaller`（相对父级 5/6）就是 `<sub>`/`<sup>`。
 *
 * 另外看一眼状态行：本库**没有等宽字体族的分家**（除非注册了 mono factory），
 * 内建位图字体本身就是等宽的；接了矢量字体后 `<pre>`/`<code>` 才算"真等宽"。
 */
#include "common.h"

#define W 340
#define H 470

static const char *k_html =
    "<div id=\"root\">"

    "  <h2>1 · 空白保留</h2>"
    /* 注意：源码里的缩进会被原样画出来 —— 这就是 pre 的语义 */
    "<pre>int main(void) {\n"
    "    if (x > 0) {\n"
    "        return 1;\t// tab 对齐到 8 列\n"
    "    }\n"
    "    return 0;\n"
    "}</pre>"

    "  <h2>2 · 只保留换行</h2>"
    "<div class=\"poem\">床前明月光\n疑是地上霜\n举头望明月\n低头思故乡</div>"

    "  <h2>3 · 上下标</h2>"
    "<p>水分子 H<sub>2</sub>O，面积单位 cm<sup>2</sup>，"
    "能量 E = mc<sup>2</sup>。</p>"
    "<p class=\"big\">字号跟着父级走：<b>x<sup>2</sup></b></p>"

    "  <h2>4 · 行内代码</h2>"
    "<p>按 <code>ql_ui_layout()</code> 之后才能取到盒子；"
    "<code>white-space</code> 默认是 <code>nowrap</code>。</p>"

    "</div>";

static const char *k_css =
    "#root { width: 340px; height: 470px; background-color: #0B0E14;"
    "        padding: 14px; font-size: 12px; color: #C8D2DE; }"
    "h2 { font-size: 13px; color: #7CE0FF; margin: 10px 0 6px 0; }"
    /* 代码块：pre 已经给了 block / 空白保留，这里只补外观 */
    "pre { background-color: #11161F; padding: 8px; border-radius: 6px;"
    "      border-width: 1px; border-color: #1E2836; color: #9AE6B4;"
    "      font-size: 11px; line-height: 15px; margin: 0 0 8px 0; }"
    /* pre-line：折叠多余空格、保留换行 —— 适合诗句/地址这类"按行写"的文本 */
    ".poem { white-space: pre-line; line-height: 19px; color: #C8D2DE;"
    "        margin: 0 0 8px 0; }"
    "p { line-height: 19px; margin: 0 0 6px 0; }"
    ".big { font-size: 18px; }"
    /* 上下标不用写样式：UA 默认已经给了 vertical-align 与 smaller 字号 */
    "sub { color: #F6C177; }"
    "sup { color: #F6C177; }"
    "code { color: #7CE0FF; }";

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

    /*
     * 手动查一遍"空白真的留下了"：找到 pre 节点，看它的文本里有没有换行。
     * 这一步在真实界面里不需要，写出来是因为 pre 出问题时最难一眼看出来。
     */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (ui->nodes[i].src->tag != QL_UI_TAG_PRE)
            continue;
        const char *t = ui->nodes[i].text;
        int nl = 0;
        for (const char *p = t; p != NULL && *p != '\0'; p++)
            if (*p == '\n')
                nl++;
        printf("<pre> 里有 %d 个换行，盒子 %dx%d\n", nl,
               (int)ui->nodes[i].border_box.w, (int)ui->nodes[i].border_box.h);
    }

    const int rc = ex_run(ui, W, H, "QLGL · 12 text block", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
