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
#include "common.h"

#define W 260
#define H 220

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

    /* 找出那个滚动容器（.list）：它是唯一 overflow == auto 的节点 */
    QLUINodeRT *list = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ui->nodes[i].style->overflow == QL_UI_OVERFLOW_AUTO) {
            list = &ui->nodes[i];
            break;
        }
    if (list == NULL) {
        printf("没有找到滚动容器\n");
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    printf("列表可滚 %dpx；每行 %.0fpx，视口 %.0fpx\n",
           (int)list->scroll_max_y,
           (double)(ql_ui_node_rect(&ui->nodes[3]).h),
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

    const int rc = ex_run(ui, W, H, "QLGL · 09 scroll", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
