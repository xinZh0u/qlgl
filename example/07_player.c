/**
 * @file    07_player.c
 * @brief   一个"真实界面"：音乐播放器。
 *
 *   ./build/example/07_player                    # 打开窗口
 *   ./build/example/07_player --dump /tmp/p.ppm  # 截图模式（一帧后退出）
 *
 * 这个例程不是新 API 的演示，而是**压力测试**：把几种常见的界面需求放在一起，
 * 看这套 HTML+CSS 子集够不够用。它用到：
 *
 *   - `flex-direction: column` + `justify-content` 做整页纵向布局；
 *   - `background-image: linear-gradient(...)` 做封面（没有图片解码器）；
 *   - 百分比宽度做进度条；
 *   - `white-space: normal` + `line-height` 做**自动折行的简介**（本库默认不折行）；
 *   - `line-height == height` 让按钮里的字垂直居中；
 *   - `:hover` / `:active` + `transition` 让按钮有反馈。
 *
 * 缺的东西也在这个例程里看得见：没有滚动条、不能输入、没有图片。这些是下一步。
 */
#include "common.h"

#define W 300
#define H 316

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"cover\">"
    "    <div class=\"disc\"></div>"
    "  </div>"
    "  <div class=\"meta\">"
    "    <div class=\"title\">晚风心里吹</div>"
    "    <div class=\"artist\">阿梨粤 · 粤语流行</div>"
    "  </div>"
    "  <div class=\"track\"><div class=\"track-fill\"></div></div>"
    "  <div class=\"times\"><span>1:23</span><span>3:47</span></div>"
    /* 简介：一段会自动折行的文字（white-space: normal） */
    "  <div class=\"desc\">这首歌唱的是夏天的傍晚：风穿过窗户，把心事吹散。"
    "副歌部分的编曲克制，只留一把吉他和一声轻叹。</div>"
    "  <div class=\"row\">"
    "    <div class=\"btn\">上一首</div>"
    "    <div class=\"btn play\">播放</div>"
    "    <div class=\"btn\">下一首</div>"
    "  </div>"
    "</div>";

static const char *k_css =
    "#root { width: 300px; height: 316px; background-color: #0B0E14;"
    "        display: flex; flex-direction: column; gap: 10px;"
    "        padding: 16px; }"

    /* 封面：没有图片解码器，用渐变代替；中间一块深色圆盘 */
    /* position: relative 让封面成为圆盘的包含块（否则会相对 #root 定位） */
    ".cover { position: relative; width: 268px; height: 116px; border-radius: 12px;"
    "         background-image: linear-gradient(to right, #2E6BE6, #7A5AF8); }"
    ".disc { position: absolute; left: 106px; top: 30px;"
    "        width: 56px; height: 56px; border-radius: 28px;"
    "        background-color: #101725; }"

    ".meta { display: flex; flex-direction: column; gap: 2px; }"
    ".title { font-size: 17px; color: #E6EDF3; }"
    ".artist { font-size: 12px; color: #7A8794; }"

    /* 进度条：底轨 + 按百分比填充 */
    ".track { width: 268px; height: 4px; border-radius: 2px;"
    "         background-color: #1B222C; }"
    ".track-fill { width: 36%; height: 4px; border-radius: 2px;"
    "              background-color: #2E6BE6; }"
    ".times { width: 268px; display: flex; justify-content: space-between;"
    "         font-size: 10px; color: #6C7783; }"

    /* 简介：默认不折行；显式开 white-space: normal 才会按宽度断行 */
    ".desc { width: 268px; font-size: 11px; line-height: 18px;"
    "        color: #7A8794; white-space: normal; }"

    ".row { width: 268px; display: flex; gap: 10px; justify-content: center;"
    "       margin-top: 2px; }"
    /* text-align 管水平、line-height 管竖直：两个都写才是"居中" */
    ".btn { width: 76px; height: 30px; border-radius: 15px; font-size: 12px;"
    "       text-align: center; line-height: 30px;"
    "       background-color: #1B222C; color: #C8D2DE;"
    "       transition: 140ms ease-out; }"
    ".btn:hover { background-color: #2A3546; color: #FFFFFF; }"
    ".btn:active { background-color: #3A475C; }"
    ".play { background-color: #2E6BE6; color: #FFFFFF; }"
    ".play:hover { background-color: #3D7BF6; }";

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
    printf("%u 个节点 / %u 条样式 / %u 个字号\n", (unsigned)doc->node_count,
           (unsigned)doc->style_count, (unsigned)doc->font_size_count);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 折行结果直接摆出来：这就是"布局与绘制共用一份断行"的证据 */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *n = &ui->nodes[i];
        if (n->style != NULL && n->style->white_space == 1 && n->text != NULL)
            printf("自动折行：%s → %d 行，高 %dpx\n", n->text,
                   (int)(ql_ui_node_rect(n).h / n->style->line_h),
                   (int)ql_ui_node_rect(n).h);
    }

    const int rc = ex_run(ui, W, H, "QLGL · 07 player", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
