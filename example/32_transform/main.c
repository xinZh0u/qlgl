/**
 * @file    main.c
 * @brief   M9.4：旋转光栅 —— `transform: rotate()` / `skew()` / `matrix()`。
 *
 *   ./build/example/example_32_transform
 *   ./build/example/example_32_transform --size 760x820 --dump tr.ppm
 *
 * 这一页只讲一件事：**CSS 的 transform 作用在元素的渲染结果上**。
 * 因此实现不是"把每个图元按矩阵转一遍"（那样文字会变成马赛克、圆角会变方），
 * 而是"先把子树合成到临时图，再整体做一次仿射重采样"。
 *
 * 观察要点：
 *
 *   - 第 1 节：同一个标签转不同角度，字是**转过**的字（不是重新排版）；
 *   - 第 2 节：同样的 30°，`transform-origin` 不同 → 落在不同位置；
 *   - 第 5 节：圆角 + rotate(45°) 看到的是圆的边缘 —— 说明是"先裁再转"；
 *   - 第 6 节：指针绕自己的轴转（origin 在圆心那一端）。
 *
 * `translate()` / `scale()` 仍然是原来的绘制期变换（不进离屏图层），
 * 只有"非轴对齐"的旋转/斜切/矩阵才需要重采样。
 */
#include "common.h"

#define W 760
#define H 1000

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

    const int rc = ex_run(ui, W, H, "QLGL · 32 旋转光栅", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
