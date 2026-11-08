/**
 * @file    06_theme.c
 * @brief   换皮肤：同一份 HTML，换一份 CSS 重新编译。
 *
 *   ./build/example/06_theme                用内置的三套主题各渲染一张图
 *   ./build/example/06_theme my.css         用你写的 CSS
 *
 * 这是"运行期编译"最实用的场景：界面结构不变，只是外观变。
 * 编译一次是毫秒级的，而且产物就是静态表 —— 换完皮肤之后性能与
 * 构建期编译出来的完全一样。
 */
#include "common.h"

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"title\">换皮肤</div>"
    "  <div class=\"row\">"
    "    <div class=\"chip on\">选中</div>"
    "    <div class=\"chip\">普通</div>"
    "  </div>"
    "  <div class=\"body\">同一份 HTML，三份 CSS。</div>"
    "</div>";

/* 三套主题只差 CSS —— 结构一个字都没改 */
static const char *k_dark =
    "#root { width: 260px; height: 140px; background-color: #0E1116; }"
    ".title { margin: 10px 0 6px 14px; font-size: 15px; color: #E6EDF3; }"
    ".row { display: flex; gap: 8px; margin: 0 14px; }"
    ".chip { width: 68px; height: 26px; border-radius: 13px; font-size: 12px;"
    "        text-align: center; background-color: #1B222C; color: #8B949E; }"
    ".on { background-color: #2E6BE6; color: #FFFFFF; }"
    ".body { margin: 10px 14px; font-size: 12px; color: #7A8794; }";

static const char *k_light =
    "#root { width: 260px; height: 140px; background-color: #FAFAFA; }"
    ".title { margin: 10px 0 6px 14px; font-size: 15px; color: #1F2328; }"
    ".row { display: flex; gap: 8px; margin: 0 14px; }"
    ".chip { width: 68px; height: 26px; border-radius: 13px; font-size: 12px;"
    "        text-align: center; background-color: #EAEEF2; color: #57606A; }"
    ".on { background-color: #0969DA; color: #FFFFFF; }"
    ".body { margin: 10px 14px; font-size: 12px; color: #57606A; }";

static const char *k_warm =
    "#root { width: 260px; height: 140px; background-color: #241A12;"
    "        border-radius: 12px; }"
    ".title { margin: 10px 0 6px 14px; font-size: 15px; color: #FFE0B2; }"
    ".row { display: flex; gap: 8px; margin: 0 14px; }"
    ".chip { width: 68px; height: 26px; border-radius: 13px; font-size: 12px;"
    "        text-align: center; background-color: #3A2A1C; color: #C9A882; }"
    ".on { background-color: #E8743B; color: #1A0E06; }"
    ".body { margin: 10px 14px; font-size: 12px; color: #A98A6A; }";

/* 编译 + 渲染一张图；返回 0 表示成功 */
static int shot(const char *css, int index, int argc, char **argv)
{
    QLUIDoc *doc = ql_ui_load_text(k_html, css);
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
    ex_load_font();

    int rc = 0;
    if (argc > 1 && argv[1][0] != '-') {
        /* 用外部 CSS 文件 */
        FILE *f = fopen(argv[1], "rb");
        if (f == NULL) {
            printf("打不开 %s\n", argv[1]);
            qlgl_shutdown();
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *css = (char *)malloc((size_t)n + 1u);
        size_t got = (css != NULL) ? fread(css, 1, (size_t)n, f) : 0;
        fclose(f);
        if (css == NULL) {
            qlgl_shutdown();
            return 1;
        }
        css[got] = '\0';
        rc = shot(css, 0, argc, argv);
        free(css);
    } else {
        rc |= shot(k_dark, 1, argc, argv);
        rc |= shot(k_light, 2, argc, argv);
        rc |= shot(k_warm, 3, argc, argv);
    }

    qlgl_shutdown();
    return rc;
}
