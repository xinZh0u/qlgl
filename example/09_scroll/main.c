/**
 * @file    main.c
 * @brief   滚动容器：`overflow: auto`。
 *
 *   ./build/example/example_09_scroll                    # 打开窗口（滚轮 / 拖右侧滚动条）
 *   ./build/example/example_09_scroll --dump /tmp/s.ppm  # 截图模式
 *   ./build/example/example_09_scroll --scroll 120       # 命令行直接滚到某处再截
 *
 * 三件事：
 *   1. `overflow: auto` 才滚；`overflow: hidden` 只裁不滚（两者都裁）；
 *   2. 滚动偏移是**烘进布局**的 —— 所以绘制、命中测试、脏区都不用改，
 *      代价是滚动后要重跑一次布局（0.04ms 量级）；
 *   3. 滚动条是覆盖式画的，不占布局空间。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 260
#define H 220

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
    ql_ui_layout(ui, W, H);

    /* 按 id 找滚动容器与行：不依赖文档顺序 */
    QLUINodeRT *list = ql_ui_find_by_id(ui, "list");
    QLUINodeRT *row1 = ql_ui_find_by_id(ui, "row1");
    if (list == NULL) {
        printf("没有找到滚动容器\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }
    printf("列表可滚 %dpx；每行 %.0fpx，视口 %.0fpx\n",
           (int)list->scroll_max_y,
           (double)(row1 != NULL ? ql_ui_node_rect(row1).h : 0),
           (double)ql_ui_node_rect(list).h);

    /* 命令行直接滚一段：方便截图对比 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scroll") == 0 && i + 1 < argc) {
            const ql_coord_t y = (ql_coord_t)strtol(argv[i + 1], NULL, 10);
            (void)ql_ui_scroll_to(ui, list, 0, y);
            ql_ui_layout(ui, W, H);
            printf("已滚到 y=%d（夹紧后 %d）\n", (int)y, (int)list->scroll_y);
            break;
        }
    }

    QLWindow *win = ql_window_create("QLGL · 09 scroll", W, H, QL_WINDOW_GL);
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
