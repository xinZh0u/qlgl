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
#include "common.h"

/*
 * 编译 + 渲染一张图；返回 0 表示成功。
 * `css_file` 只是文件名 —— 换皮肤就是换这一个参数。
 */
static int shot(const char *css_file, int index, int argc, char **argv)
{
    QLUIDoc *doc = ex_load_ui("index.html", css_file);
    if (doc == NULL) {
        printf("第 %d 套皮肤编译失败：%s\n", index, ql_ui_text_last_error());
        return 1;
    }
    char name[64];
    snprintf(name, sizeof(name), "theme%d", index);
    ql_ui_text_set_name(name);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return 1;
    }

    int rc = 0;
    if (argc <= 1) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/06_theme_%d.ppm", index);
        ql_ui_layout(ui, 260, 140);
        if (ex_dump_ppm(ui, 260, 140, path)) {
            printf("第 %d 套皮肤 → %s（%u 条样式）\n", index, path,
                   (unsigned)doc->style_count);
        } else {
            rc = 1;
        }
    } else {
        rc = ex_run(ui, 260, 140, "QLGL · 06 theme", argc, argv);
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return rc;
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    int rc = 0;
    if (argc > 1 && argv[1][0] != '-') {
        /* 用你写的 CSS：相对名先找当前目录，再找例程目录（见 common.h） */
        rc = shot(argv[1], 0, argc, argv);
    } else {
        rc |= shot("dark.css", 1, argc, argv);
        rc |= shot("light.css", 2, argc, argv);
        rc |= shot("warm.css", 3, argc, argv);
    }

    qlgl_shutdown();
    return rc;
}
