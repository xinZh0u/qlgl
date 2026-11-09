/**
 * @file    main.c
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

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
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
