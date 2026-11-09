/**
 * @file    main.c
 * @brief   `@media` 断点：同一份 HTML，窗口变窄就换布局。
 *
 *   ./build/example/example_22_media                  # 打开窗口，拖边缘缩放
 *   ./build/example/example_22_media --size 800x400 --dump wide.ppm
 *   ./build/example/example_22_media --size 420x600 --dump narrow.ppm
 *
 * 运行期**不解析任何条件**：构建期已经把宽度轴切成区间，每个区间编译一套样式；
 * 布局时只按视口宽选区间下标（线性扫一个 6 项以内的小数组）。
 */
#include "common.h"

#define W 820
#define H 420

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
    printf("断点区间 %d 个：", (int)doc->media_count);
    for (uint16_t i = 0; i < doc->media_count; i++) {
        const QLUIMedia *m = &doc->media[i];
        if (m->hi == 0xFFFFu)
            printf(" [%u, ∞)", (unsigned)m->lo);
        else
            printf(" [%u, %u]", (unsigned)m->lo, (unsigned)m->hi);
    }
    printf("\n");

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    const int rc = ex_run(ui, W, H, "QLGL · 22 media（拖边缘缩放）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
