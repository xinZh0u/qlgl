/**
 * @file    08_tags.c
 * @brief   HTML 标签与 UA 默认样式。
 *
 *   ./build/example/08_tags                    # 打开窗口
 *   ./build/example/08_tags --dump /tmp/t.ppm  # 截图模式
 *
 * 标签只影响三件事：UA 默认样式、`tag` 选择器、调试名。布局和绘制只看编译出来的
 * QLUIStyle。这个例程就是把这句话演示一遍：
 *
 *   - 上面一半**完全不写 CSS 里的结构性样式**，纯靠 UA 默认值排版；
 *   - 下面一半用同样的标签，只改颜色和字号 —— 说明覆盖有多容易。
 *
 * 语义行内标签现在**真的看得出区别**：`b`/`strong` 是合成加粗（画两遍偏移 1px），
 * `i`/`em` 是合成斜体（按行剪切），`u` 是下划线、`s` 是删除线（各画一条 1px 线），
 * `mark` 是背景高亮。都不需要第二个字体文件。
 */
#include "common.h"

#define W 260
#define H 500

static const char *k_html =
    "<div id=\"root\">"
    /* ---- 第一部分：只靠 UA 默认样式 ---- */
    "  <header>QLGL 标签示例</header>"
    "  <hr>"
    "  <p>普通 <mark>高亮</mark> <b>加粗</b> <i>斜体</i></p>"
    "  <p><u>下划线</u> <s>删除线</s> <span>普通</span></p>"
    "  <h2>二级标题</h2>"
    "  <ul><li>第一项</li><li>第二项</li></ul>"
    "  <blockquote>引用一段话：标签不参与布局，只给默认样式。</blockquote>"
    "  <figure>"
    "    <div class=\"box\"></div>"
    "    <figcaption>图 1：一个圆角方块</figcaption>"
    "  </figure>"
    "  <div class=\"brdemo\">"
    "    <span>上行</span><br><span>下行</span>"
    "  </div>"
    /* ---- 第二部分：同样的标签，只用 CSS 改观感 ---- */
    "  <div class=\"themed\">"
    "    <header>换一套观感</header>"
    "    <hr>"
    "    <ul><li>缩进还在</li><li>颜色变了</li></ul>"
    "    <blockquote>还是同一个 header / ul / blockquote。</blockquote>"
    "  </div>"
    "</div>";

static const char *k_css =
    "#root { width: 100%; background-color: #FFFFFF; color: #1F2328;"
    "        font-size: 12px; padding: 12px; }"

    /* 只给"这个库特有的观感"，不给结构性东西（display / margin 都留空） */
    ".box { width: 140px; height: 28px; border-radius: 4px;"
    "       background-color: #2E6BE6; }"
    ".brdemo { margin: 8px 0; color: #0969DA; }"

    /* 第二部分：同样的标签换个颜色就够了 */
    ".themed { margin-top: 14px; padding: 10px; border-radius: 8px;"
    "          background-color: #FFFFFF; }"
    ".themed header { color: #E6C34A; }"
    ".themed hr { background-color: #D0D7DE; }"
    ".themed blockquote { color: #E6C34A; }";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    printf("%u 个节点 / %u 条样式\n", (unsigned)doc->node_count,
           (unsigned)doc->style_count);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 标签名可以直接取回（文档里只有下标，没有名字） */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *n = &ui->nodes[i];
        const char *name = ql_ui_tag_name(n->src->tag);
        if (strcmp(name, "div") != 0) {
            const QLRect r = ql_ui_node_rect(n);
            printf("  <%-10s> (%3d,%3d %3dx%3d)\n", name, (int)r.x, (int)r.y,
                   (int)r.w, (int)r.h);
        }
    }

    const int rc = ex_run(ui, W, H, "QLGL · 08 tags", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
