/**
 * @file    main.c
 * @brief   液态玻璃：`backdrop-filter: blur()` 的玻璃按钮 + 背后会动的色块。
 *
 *   ./build/example/example_38_liquid_glass                    # 打开窗口
 *   ./build/example/example_38_liquid_glass --dump /tmp/g.ppm  # 抓一帧
 *
 * 三件值得看的东西：
 *   1. 玻璃不是"白色半透明块"—— 背后**真的被模糊**了，色块从它底下穿过时
 *      玻璃里的颜色跟着变（`backdrop-filter` 采样的是已画好的背景）；
 *   2. 玻璃套玻璃（按钮在托盘上）也能正确工作：模糊按绘制顺序逐层取样；
 *   3. 按下时只改 `background-image` / `border-color` / `box-shadow`，
 *      走 `transition` 平滑过渡 —— 改动不含布局属性，所以**不重排**。
 *
 * 为什么"玻璃"要三层：只加 `backdrop-filter` 会得到一块灰扑扑的毛玻璃，
 * 亮边（1px 白边）和内阴影高光才是"玻璃有厚度"的错觉来源，`.sheen`
 * 那层斜向渐变是镜面反射。这三样都是纯绘制，不额外增加模糊次数。
 */
#include "common.h"

#define W 720
#define H 460

static QLUIRoot  *s_ui;
static QLUINodeRT *s_status;
static int        s_count;

/* `ql_ui_set_text()` 不复制字符串：缓冲必须一直有效 */
static char s_buf[64];

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    if (node->src == NULL || node->src->tag != QL_UI_TAG_DIV)
        return;
    s_count++;
    const QLUITag t = node->src->tag;
    (void)t;
    snprintf(s_buf, sizeof(s_buf), "第 %d 次点击 · 玻璃后面还在动", s_count);
    ql_ui_set_text(ui, s_status, s_buf);
    printf("%s\n", s_buf);
}

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
    s_ui = ui;

    ql_ui_layout(ui, W, H);
    /* 状态栏是最后一个节点（`.status > .status-text`，取文本节点本身） */
    s_status = &ui->nodes[ui->node_count - 1];

    ql_ui_set_click_cb(ui, on_click, NULL);

    const int rc = ex_run(ui, W, H, "QLGL · 38 liquid glass", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
