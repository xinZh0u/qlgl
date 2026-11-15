/**
 * @file    main.c
 * @brief   布局：flex 是主力，position 管叠加。
 *
 *   ./build/example/example_03_layout --dump /tmp/03.ppm
 *
 * 布局规则（和浏览器一致的部分）：
 *   - `display: flex` 的容器按 `flex-direction` 排子元素；
 *   - `justify-content` 管主轴，`align-items` 管交叉轴；
 *   - `flex-grow` 是"剩余空间按比例分"；
 *   - `gap` 是子元素之间的间距（不占两端）；
 *   - `width: 50%` 按**父容器宽度**算。
 * 布局单位一律是 CSS px（整数），高 DPI 由绘制后端乘缩放。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

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
    ql_ui_layout(ui, 300, 220);

    /* 布局结果是可以直接读的：每个节点的 border_box */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLRect b = ql_ui_node_rect(&ui->nodes[i]);
        printf("  #%u  %4d,%-4d %3dx%-3d\n", (unsigned)i, (int)b.x, (int)b.y,
               (int)b.w, (int)b.h);
    }

    QLWindow *win = ql_window_create("QLGL · 03 layout", 300, 220, QL_WINDOW_GL);
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
