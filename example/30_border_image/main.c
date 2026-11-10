/**
 * @file    main.c
 * @brief   B6：`border-image` 九宫格（stretch / repeat / round）。
 *
 *   ./build/example/example_30_border_image
 *   ./build/example/example_30_border_image --size 720x320 --dump bi.ppm
 *
 * 图片来自**运行期注册**（`ql_ui_register_image` 按名字填像素），
 * 与 `<img src>` / `background-image: url()` 共用同一张资源表。
 * 这里现算一张 24×24 的**带 alpha** 的图：四角红、四边绿、中间透明 ——
 * 一眼就能看出"角没被拉伸、边沿一个方向铺开"，同时中间透明所以文字透出来
 * （CSS 里 `border-image` 会把中间也铺满，除非源图那儿是透明的）。
 */
#include "common.h"

#define W 720
#define H 320

#define IMG 24
static uint32_t s_frame[IMG * IMG];   /* RGBA8888，非预乘 */

static void build_frame(void)
{
    for (int y = 0; y < IMG; y++) {
        for (int x = 0; x < IMG; x++) {
            const int corner = (x < 8 || x >= 16) && (y < 8 || y >= 16);
            const int edge = (x < 8 || x >= 16) || (y < 8 || y >= 16);
            uint32_t c;
            if (corner)
                c = 0xFF0000FFu;          /* 红，不透明 */
            else if (edge)
                c = 0xFF00FF00u;          /* 绿，不透明 */
            else
                c = 0x00000000u;          /* 中间完全透明 */
            s_frame[y * IMG + x] = c;
        }
    }
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
    ql_ui_layout(ui, W, H);

    /* 注册边框图（名字与 CSS 里的 url(frame) 对应） */
    build_frame();
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "frame";
    in.data = (const uint8_t *)s_frame;
    in.len = sizeof(s_frame);
    in.w = IMG;
    in.h = IMG;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_RGBA8888;
    if (ql_ui_register_image(ui, &in) != QL_OK)
        printf("（提示）没有找到叫 frame 的边框图资源\n");

    const int rc = ex_run(ui, W, H, "QLGL · 30 border-image", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
