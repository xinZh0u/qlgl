/**
 * @file    main.c
 * @brief   M9.3：离屏图层 —— `filter` / `mix-blend-mode` / `mask` / 真 group opacity。
 *
 *   ./build/example/example_31_filter
 *   ./build/example/example_31_filter --size 760x760 --dump filter.ppm
 *
 * 这一页演示的是"**先合成再处理**"：带滤镜的元素先画到一块临时图上，
 * 再整体模糊/调色/降透明/混合。所以
 *
 *   - `blur()` 连"子元素之间的重叠"一起糊（逐图元糊是另一回事）；
 *   - `opacity` 是整组降透明，重叠处不会更深；
 *   - `drop-shadow()` 跟着 alpha 形状走（圆角、月牙都行），不是矩形框；
 *   - `mix-blend-mode` 能读到它背后的像素；
 *   - `mask` 用图片的 alpha 再乘一次。
 *
 * 遮罩图在程序里现算（8×8 的 A8），不用外部文件。
 */
#include "common.h"

#define W 760
#define H 760

/* 8×8 A8 遮罩：左半 0、右半 255，中间一格斜着过渡（看得出是"图片 alpha"） */
static uint8_t s_diag[8 * 8];

static void build_mask(void)
{
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int v = (x - 1) * 255 / 5;      /* 从左到右渐亮 */
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            s_diag[y * 8 + x] = (uint8_t)v;
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

    build_mask();
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "diag";
    in.data = s_diag;
    in.len = sizeof(s_diag);
    in.w = 8;
    in.h = 8;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_A8;
    if (ql_ui_register_image(ui, &in) != QL_OK)
        printf("（提示）没有找到叫 diag 的遮罩资源\n");

    const int rc = ex_run(ui, W, H, "QLGL · 31 离屏图层", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
