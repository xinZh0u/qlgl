/**
 * @file    main.c
 * @brief   换皮肤：同一份 HTML，换一份 CSS 重新编译。
 *
 *   ./build/example/example_06_theme              三套主题各渲染一张图
 *   ./build/example/example_06_theme my.css       用你写的 CSS
 *
 * 目录里的文件：
 *   index.html            结构（三套皮肤共用，一个字都不改）
 *   dark/light/warm.css   三套外观
 *
 * 这是"运行期编译"最实用的场景：界面结构不变，只是外观变。
 * 编译一次是毫秒级的，而且产物就是静态表 —— 换完皮肤之后性能与
 * 构建期编译出来的完全一样。
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

/* 相对名 → 例程目录下的绝对路径；绝对路径原样用 */
static void css_path_of(char *out, size_t cap, const char *css_file)
{
    if (css_file[0] == '/')
        snprintf(out, cap, "%s", css_file);
    else
        snprintf(out, cap, "%s/%s", QL_EX_DIR, css_file);
}

/*
 * 编译 + 渲染一张图；返回 0 表示成功。
 * `css_file` 只是文件名 —— 换皮肤就是换这一个参数。
 */
static int shot(const char *css_file, int index, int argc, char **argv)
{
    char cp[1200];
    css_path_of(cp, sizeof(cp), css_file);

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", cp);
    if (ui == NULL) {
        printf("第 %d 套皮肤编译失败：%s\n", index, ql_ui_text_last_error());
        return 1;
    }

    char name[64];
    snprintf(name, sizeof(name), "theme%d", index);
    ql_ui_text_set_name(name);

    int rc = 0;
    if (argc <= 1) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/06_theme_%d.ppm", index);
        if (ql_ui_dump_ppm(ui, 260, 140, path)) {
            printf("第 %d 套皮肤 → %s（%u 条样式）\n", index, path,
                   (unsigned)ui->doc->style_count);
        } else {
            rc = 1;
        }
    } else {
        QLWindow *win = ql_window_create("QLGL · 06 theme", 260, 140, QL_WINDOW_GL);
        if (win == NULL) {
            printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
            ql_ui_free(ui);
            return 1;
        }
        ql_window_bind_ui(win, ui);
        rc = ql_window_run_args(win, argc, argv);
        ql_window_destroy(win);
    }

    ql_ui_free(ui);
    return rc;
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    int rc = 0;
    if (argc > 1 && argv[1][0] != '-') {
        rc = shot(argv[1], 0, argc, argv);
    } else {
        rc |= shot("dark.css", 1, argc, argv);
        rc |= shot("light.css", 2, argc, argv);
        rc |= shot("warm.css", 3, argc, argv);
    }

    qlgl_shutdown();
    return rc;
}
