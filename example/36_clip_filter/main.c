/**
 * @file    main.c
 * @brief   B9：`clip-path` 的四种形状 + 纯颜色 `filter` 的单批次 + 字体变体。
 *
 *   ./build/example/example_36_clip_filter
 *   ./build/example/example_36_clip_filter --size 848x800 --dump cf.ppm
 *
 * 这个例程想说明三件事：
 *   1. 四种 `clip-path` 形状里，`inset()` 是纯矩形求交（最便宜），
 *      `circle()`/`ellipse()` 是解析遮罩（GPU 一条 SDF），
 *      `polygon()` 走 A8 蒙版 + 离屏图层（最贵，但仍然只多一块 64×64 的图）；
 *   2. 纯颜色的 `filter` **不进离屏图层** —— 折成一个 3×4 矩阵，
 *      CPU 逐像素过一遍、GPU 用 uniform 过一遍；
 *   3. `font-variant-caps: small-caps` / `font-variant-numeric: tabular-nums`
 *      靠 HarfBuzz 的 OpenType 特性，没有整形能力时**构建期报错**（不静默降级）。
 */
#include "common.h"

#define W 848
#define H 800

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    char hp[1024], cp[1024];
    ex_resolve(hp, sizeof(hp), "index.html");
    ex_resolve(cp, sizeof(cp), "style.css");
    char *html = ex_slurp(hp);
    char *css = ex_slurp(cp);
    if (html == NULL || css == NULL) {
        free(html);
        free(css);
        qlgl_shutdown();
        return 1;
    }

    QLUIDoc *doc = ql_ui_load_text(html, css);
    free(html);
    free(css);
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

    printf("整形后端：%s（small-caps / tabular-nums 需要 HarfBuzz）\n",
           ql_text_shape_backend());

    const int rc = ex_run(ui, W, H, "QLGL · 36 clip-path / filter / 字体变体", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
