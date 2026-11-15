/**
 * @file    main.c
 * @brief   一个"真实界面"：音乐播放器。
 *
 *   ./build/example/example_07_player                    # 打开窗口
 *   ./build/example/example_07_player --dump /tmp/p.ppm  # 截图模式（一帧后退出）
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 300
#define H 316

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    printf("%u 个节点 / %u 条样式 / %u 个字号\n", (unsigned)ui->doc->node_count,
           (unsigned)ui->doc->style_count, (unsigned)ui->doc->font_size_count);

    ql_ui_layout(ui, W, H);

    /* 折行结果直接摆出来：这就是"布局与绘制共用一份断行"的证据 */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *n = &ui->nodes[i];
        if (n->style != NULL && n->style->white_space == 1 && n->text != NULL)
            printf("自动折行：%s → %d 行，高 %dpx\n", n->text,
                   (int)(ql_ui_node_rect(n).h / n->style->line_h),
                   (int)ql_ui_node_rect(n).h);
    }

    QLWindow *win = ql_window_create("QLGL · 07 player", W, H, QL_WINDOW_GL);
    if (win == NULL) {
        printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }
    ql_window_bind_ui(win, ui);
    const int rc = ql_window_run_args(win, argc, argv);
    ql_window_destroy(win);
    ql_ui_free(ui);
    qlgl_shutdown();
    return rc;
}
