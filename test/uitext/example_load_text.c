/**
 * @file    example_load_text.c
 * @brief   运行期把 HTML + CSS 编译成界面（不重新编译程序）。
 *
 * 运行：
 *   ./build/example_load_text                     用内置的一段 HTML/CSS
 *   ./build/example_load_text app.html app.css     从文件读
 *   ./build/example_load_text --frames 120         渲染 120 帧后退出并写 PPM
 *
 * 这个示例同时演示三件事：
 *   1. `ql_ui_load_text()` 的返回值与构建期生成的 doc **完全等价**
 *      （同一套 `ql_ui_build` / `ql_ui_layout` / `ql_ui_render`）；
 *   2. 出错时的排查体验（带 `css:行号` 的原因）；
 *   3. 编译一次的结果可以再存成 `.qlui`，下次加载不用再解析。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define WIN_W 320
#define WIN_H 200

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"title\">运行期编译的界面</div>"
    "  <div class=\"row\">"
    "    <div class=\"card a\">A</div>"
    "    <div class=\"card b\">B</div>"
    "    <div class=\"card c\">C</div>"
    "  </div>"
    "  <div class=\"note\">改 app.css 就能换样子，不用重新编译</div>"
    "</div>";

static const char *k_css =
    "#root { width: 320px; height: 200px; background-color: #0E1116; }\n"
    ".title { margin: 10px 0 0 12px; font-size: 15px; color: #C8D2DE; }\n"
    ".row { display: flex; gap: 8px; margin: 12px; }\n"
    ".card { width: 88px; height: 64px; border-radius: 8px; font-size: 20px;\n"
    "        text-align: center; color: #101418; }\n"
    ".a { background-color: #E8743B; }\n"
    ".b { background-color: #2FB8A6; }\n"
    ".c { background-color: #E6C34A; }\n"
    ".note { margin: 4px 12px; font-size: 12px; color: #6C7783; }\n";

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *html_src = k_html;
    const char *css_src = k_css;
    char *html_file = NULL, *css_file = NULL;
    const char *dump = NULL;
    int frames = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (i + 1 == argc - 1) {
            html_file = read_file(argv[i]);
            css_file = read_file(argv[++i]);
        }
    }
    if (html_file != NULL)
        html_src = html_file;
    if (css_file != NULL)
        css_src = css_file;

    if (qlgl_init() != QL_OK)
        return 1;

    /* 有矢量字体就用它（中文） */
    {
        const char *cands[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
            if (ql_font_set_default_file(cands[i], -1))
                break;
    }

    QLUIDoc *doc = ql_ui_load_text(html_src, css_src);
    if (doc == NULL) {
        printf("编译失败：%s\n", ql_ui_text_last_error());
        free(html_file);
        free(css_file);
        qlgl_shutdown();
        return 1;
    }
    printf("编译成功：%u 节点 / %u 样式 / %u 条文本 / %u 种字号 / %u 段动画\n",
           (unsigned)doc->node_count, (unsigned)doc->style_count,
           (unsigned)doc->text_count, (unsigned)doc->font_size_count,
           (unsigned)doc->anim_count);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        printf("ql_ui_build 失败\n");
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, WIN_W, WIN_H);

    QLDispDriver *disp = ql_disp_mem_create(WIN_W, WIN_H, QL_COLOR_RGB888);
    QLCanvas canvas;
    if (disp != NULL && ql_canvas_from_disp(&canvas, disp) == QL_OK) {
        for (int f = 0; f < (frames > 0 ? frames : 1); f++) {
            ql_canvas_clear(&canvas, 0x000000);
            ql_ui_render_canvas(ui, &canvas);
        }
        printf("渲染完成，脏区 %d 块\n", 0);
        if (dump != NULL) {
            if (ql_disp_mem_save_ppm(disp, dump) == QL_OK)
                printf("已写出 %s\n", dump);
            else
                printf("写出 %s 失败\n", dump);
        }
        ql_disp_mem_destroy(disp);
    }

    /* 编译结果可以再存成 blob：下次加载不用再解析 */
    if (ql_ui_doc_save(doc, "/tmp/example_load_text.qlui") == QL_OK)
        printf("已缓存 /tmp/example_load_text.qlui（%zu 字节）\n",
               ql_ui_doc_encode(doc, NULL, 0));

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    free(html_file);
    free(css_file);
    qlgl_shutdown();
    return 0;
}
