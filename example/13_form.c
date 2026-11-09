/**
 * @file    13_form.c
 * @brief   `<textarea>` 多行输入：和 `<input>` 一起构成一个能填的表单。
 *
 *   ./build/example/example_13_form                    # 打开窗口
 *   ./build/example/example_13_form --dump /tmp/a.ppm   # 截图
 *
 * 多行与单行的关系一句话：**同一套输入池、同一个缓冲区模型**，
 * 区别只有三处 —— 容量（512 vs 96 字节）、`white-space: pre-wrap`、
 * 以及光标从"单行字节偏移"变成"（视觉行, 行内 x）"。
 *
 * 键盘：
 *   Enter      插入换行（单行框里 Enter 只是"提交"，不插换行）
 *   ↑ / ↓      上一行 / 下一行，并且**记住横向位置**（长行→短行→长行会回到原列）
 *   Home / End 本行首尾（不是全文首尾）
 *   鼠标点击   按 y 定行、按 x 定列
 */
#include "common.h"

#define W 340
#define H 400

static const char *k_html =
    "<div id=\"root\">"
    "  <h2>反馈</h2>"

    "  <label>标题</label>"
    "  <input id=\"t\" value=\"图标没有对齐\">"

    "  <label>详细描述</label>"
    "<textarea id=\"d\">第一行：复现步骤\n"
    "第二行：期望结果\n"
    "第三行：实际结果</textarea>"

    "  <label>联系方式（可选）</label>"
    "  <input id=\"m\" value=\"\">"

    "  <div id=\"bar\">"
    "    <button id=\"send\">提交</button>"
    "    <span id=\"hint\">Tab 在三个控件之间跳</span>"
    "  </div>"
    "</div>";

static const char *k_css =
        /*
     * 页面铺满窗口 + 竖向 flex：多行输入框用 `flex: 1 1 0px` 吃掉剩余高度，
     * 窗口拉大它就变高。
     *
     * 注意是 `1 1 0px` 而不是 `1`：`flex: 1` 的 basis 是 auto（= 内容高度），
     * 内容一高就撑爆容器；basis 写成 0 才是"从零开始分配剩余空间"。
     */
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-direction: column;"
    "        background-color: #FFFFFF;"
    "        padding: 14px; font-size: 12px; color: #1F2328; }"
    "h2 { font-size: 16px; color: #0969DA; margin: 0 0 10px 0; }"
    "label { display: block; color: #57606A; font-size: 11px; margin: 8px 0 4px 0; }"
    /*
     * 单行与多行共用同一套外观，读起来才像同一个表单。
     *
     * `box-sizing: border-box` 不能省：`width: 100%` 是**内容盒**宽度，
     * 不加它的话 padding + border 会加在 100% 外面，控件会比父容器更宽、
     * 右边直接顶到窗口边缘（截图里"输入框和窗口贴住了"就是这个）。
     */
    /* `height` 是边框盒：22(内容) + 3×2(内边距) + 1×2(边框) = 30px；
       写成 22px 的话内容盒只剩 14px，比行高还矮，字上下都会被切掉。 */
    "input { display: block; width: 100%; height: 30px; padding: 3px 6px;"
    "        box-sizing: border-box;"
    "        border-width: 1px; border-color: #D0D7DE; border-radius: 4px;"
    "        background-color: #F6F8FA; color: #1F2328; }"
    "textarea { display: block; width: 100%; flex: 1 1 0px; padding: 6px;"
    "           box-sizing: border-box;"
    "           border-width: 1px; border-color: #D0D7DE; border-radius: 4px;"
    "           background-color: #F6F8FA; color: #1F2328;"
    "           font-size: 11px; line-height: 16px; }"
    /* 聚焦态用过渡描边，能直接看出焦点在哪个控件上 */
    "input:focus { border-color: #0969DA; }"
    "textarea:focus { border-color: #0969DA; }"
    "#bar { margin-top: 14px; }"
    "button { display: inline-block; padding: 5px 14px; border-radius: 4px;"
    "         border-width: 1px; border-color: #0969DA;"
    "         background-color: #EDF3FF; color: #0969DA; }"
    "#hint { display: inline-block; margin-left: 10px; color: #6E7781;"
    "        font-size: 10px; }";

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
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 把三个控件的当前值打出来 —— 多行值里的换行会原样出现 */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const uint16_t tag = ui->nodes[i].src->tag;
        if (tag != QL_UI_TAG_INPUT && tag != QL_UI_TAG_TEXTAREA)
            continue;
        const char *v = ql_ui_input_value(ui, &ui->nodes[i]);
        printf("<%s> = [%s]\n", ql_ui_tag_name(tag), (v != NULL) ? v : "");
    }
    printf("输入池：%d 个槽位；textarea arena：%s\n", (int)ui->input_count,
           (ui->ta_arena != NULL) ? "已分配" : "未分配");

    const int rc = ex_run(ui, W, H, "QLGL · 13 form", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
