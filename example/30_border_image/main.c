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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 720
#define H 320

#define IMG 24
static uint32_t s_frame[IMG * IMG];   /* RGBA8888，非预乘 */

/* 内存字节序是 R,G,B,A（小端下 uint32 的**低字节**是 R），
 * 直接写 0xAABBGGRR 容易把红蓝写反 —— 用这个宏按 惯用的 R,G,B 顺序拼。 */
#define RGBA8888(r, g, b) \
    ((uint32_t)0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | \
     (uint32_t)(r))

static void build_frame(void)
{
    for (int y = 0; y < IMG; y++) {
        for (int x = 0; x < IMG; x++) {
            const int corner = (x < 8 || x >= 16) && (y < 8 || y >= 16);
            const int edge = (x < 8 || x >= 16) || (y < 8 || y >= 16);
            uint32_t c;
            if (corner)
                c = RGBA8888(0xB4, 0x23, 0x18);   /* 红 */
            else if (edge)
                c = RGBA8888(0x1A, 0x7F, 0x37);   /* 绿 */
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

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
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

    QLWindow *win = ql_window_create("QLGL · 30 border-image", W, H, QL_WINDOW_GL);
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
