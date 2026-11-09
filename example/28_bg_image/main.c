/**
 * @file    main.c
 * @brief   B4 收尾：`background-image: url()` + `size` / `position` / `repeat`。
 *
 *   ./build/example/example_28_bg_image
 *   ./build/example/example_28_bg_image --size 720x700 --dump bg.ppm
 *
 * 图片来自**运行期注册**（`ql_ui_register_image` 按名字填像素）——
 * 与 `<img src>` 共用一张资源表，所以同一个名字只解码一次。
 * 这里的图是程序里现算的一张 16×16 棋盘（不用外部文件，方便直接跑）。
 */
#include "common.h"

#define W 720
#define H 700

/* 16×16 RGB565 棋盘：两种蓝色交替，中间画一个亮色十字（好判断缩放/裁切方向） */
static uint16_t s_checker[16 * 16];

static void build_checker(void)
{
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint16_t c = (((x / 4) + (y / 4)) & 1) ? 0x1B4D /* 深蓝 */ : 0x2E7F;
            if (x == 7 || x == 8 || y == 7 || y == 8)
                c = 0xFE60;   /* 亮黄十字 */
            s_checker[y * 16 + x] = c;
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

    /* 注册背景图（名字与 CSS 里的 url(...) 对应） */
    build_checker();
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "checker";
    in.data = (const uint8_t *)s_checker;
    in.len = sizeof(s_checker);
    in.w = 16;
    in.h = 16;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_RGB565;
    if (ql_ui_register_image(ui, &in) != QL_OK)
        printf("（提示）没有找到叫 checker 的背景图资源\n");

    const int rc = ex_run(ui, W, H, "QLGL · 28 背景图", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
