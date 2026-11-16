/**
 * @file    main.c
 * @brief   SVG 图标：`<img src="*.svg">` + `mask-image` 染色（运行期 PlutoSVG）。
 *
 *   ./build/example/example_46_svg_icon
 *   ./build/example/example_46_svg_icon --backend thorvg   # 需 QLGL_BUILD_THORVG=ON
 *   ./build/example/example_46_svg_icon --dump /tmp/svg_icon.ppm
 *
 * 两条路径共用一张资源表：
 *   - 多色 logo 当普通 `<img>` 画；
 *   - 单色 star 同时被 `mask-image: url(star.svg)` 引用，靠 alpha 跟随主题色。
 *
 * SVG 是**运行期**解码的：应用读文件 → `ql_ui_register_image()` 按名字填像素。
 * 构建时没有 SVG 支持（`QLGL_BUILD_SVG=OFF`）也能运行，只是图片位置是占位框。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 640
#define H 420

/* 读整个文件；失败返回 NULL（调用方打印名字） */
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (p == NULL || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return p;
}

static void register_svg(QLUIRoot *ui, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/assets/%s", QL_EX_DIR, name);

    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (data == NULL) {
        printf("读不到 %s\n", path);
        return;
    }

    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = name;
    in.data = data;
    in.len = len;
    in.format = QL_IMAGE_SRC_AUTO;      /* 按 `<svg` 探测 */
    const QLErr rc = ql_ui_register_image(ui, &in);
    free(data);
    if (rc != QL_OK) {
        printf("%s 解码失败：%s\n", name,
               ql_image_last_error() != NULL ? ql_image_last_error()
                                             : "本构建可能没有 SVG 支持");
    }
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html",
                                        QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 可选：对比两个 SVG 后端（默认 PlutoSVG） */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 &&
            ql_image_svg_set_backend(argv[i + 1]) != QL_OK)
            printf("没有可用的 SVG 后端：%s\n", argv[i + 1]);
    }

    register_svg(ui, "logo.svg");
    register_svg(ui, "star.svg");

    QLWindow *win = ql_window_create("QLGL · 46 SVG 图标", W, H, QL_WINDOW_GL);
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
