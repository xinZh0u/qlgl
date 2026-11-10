/**
 * @file    main.c
 * @brief   B8 文本栈：`@font-face` / 家族列表 / 真字重与斜体 / 连字 / 双向。
 *
 *   ./build/example/example_35_text_stack
 *   ./build/example/example_35_text_stack --size 860x760 --dump text.ppm
 *
 * 这个例程把"系统里到底有哪些字体"留给运行期：启动时探测常见的 DejaVu /
 * Liberation / Noto 路径，**按找到的文件**拼出 `@font-face` 规则，再拼到
 * style.css 前面一起编译。这样在没有这些字体的机器上也能跑（家族名解析不到就
 * 落到 `serif` / `sans-serif`），不会因为写死路径而编译失败。
 */
#include "common.h"

#define W 848
#define H 800

/* 探测：第一个存在的路径 */
static const char *pick_font(const char *const *cand, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        FILE *f = fopen(cand[i], "rb");
        if (f != NULL) {
            fclose(f);
            return cand[i];
        }
    }
    return NULL;
}

/* 把 @font-face 规则拼出来（找不到就少几条，CSS 里引用的家族名自然落到通用族） */
static void build_face_css(char *out, size_t cap)
{
    static const char *const sans[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    static const char *const bold[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    };
    static const char *const italic[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
    };
    static const char *const serif[] = {
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    };

    const char *s = pick_font(sans, sizeof(sans) / sizeof(sans[0]));
    const char *b = pick_font(bold, sizeof(bold) / sizeof(bold[0]));
    const char *i = pick_font(italic, sizeof(italic) / sizeof(italic[0]));
    const char *r = pick_font(serif, sizeof(serif) / sizeof(serif[0]));

    out[0] = '\0';
    size_t used = 0;
    if (s != NULL)
        used += (size_t)snprintf(out + used, cap - used,
                                 "@font-face { font-family: \"Example Sans\";"
                                 " src: url(%s); font-weight: 400 }\n", s);
    if (b != NULL && used < cap)
        used += (size_t)snprintf(out + used, cap - used,
                                 "@font-face { font-family: \"Example Sans\";"
                                 " src: url(%s); font-weight: 700 }\n", b);
    if (i != NULL && used < cap)
        used += (size_t)snprintf(out + used, cap - used,
                                 "@font-face { font-family: \"Example Sans\";"
                                 " src: url(%s); font-weight: 400; font-style: italic }\n",
                                 i);
    if (r != NULL && used < cap)
        (void)snprintf(out + used, cap - used,
                       "@font-face { font-family: \"Example Serif\";"
                       " src: url(%s); font-weight: 400 }\n", r);
}

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

    /* @font-face 规则拼在 style.css 前面（同一份 CSS，先声明后使用） */
    char faces[2048];
    build_face_css(faces, sizeof(faces));
    const size_t total = strlen(faces) + strlen(css) + 1u;
    char *css_all = (char *)malloc(total);
    if (css_all == NULL) {
        free(html);
        free(css);
        qlgl_shutdown();
        return 1;
    }
    snprintf(css_all, total, "%s%s", faces, css);

    int face_lines = 0;
    for (const char *q = faces; *q != '\0'; q++)
        if (*q == '@')
            face_lines++;
    printf("读入 %s + %s（探测到 %d 条 @font-face）\n", hp, cp, face_lines);
    QLUIDoc *doc = ql_ui_load_text(html, css_all);
    free(html);
    free(css);
    free(css_all);
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

    printf("整形后端：%s；双向后端：%s\n", ql_text_shape_backend(),
           ql_text_bidi_backend());

    const int rc = ex_run(ui, W, H, "QLGL · 35 文本栈", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
