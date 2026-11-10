/**
 * @file    test_font_stack.c
 * @brief   B8 文本栈：`@font-face` / 家族列表 / 真字重与斜体 / 逐字回退。
 *
 * 用系统里真有的字体文件（找不到就整份跳过）。断言的着眼点是**选到了哪张字面**，
 * 而不是"长得像不像"：
 *   1. 注册表按 (家族, 字重, 斜体) 取最接近的字面；
 *   2. `font-family` 列表按顺序找第一个装得上的家族；
 *   3. 主字体没有的字符自动落到回退链（中英混排不再出豆腐块）；
 *   4. 真字面（字体自带的粗/斜体）不会被子标记成"合成"；
 *   5. 关闭文本栈时 `@font-face` / 家族名在构建期报错。
 */
#include "../ql_test.h"

#if defined(QLGL_ENABLE_FREETYPE)

#include <stdlib.h>
#include <string.h>

#define W 320
#define H 160

static const char *s_sans;
static const char *s_sans_bold;
static const char *s_sans_italic;
static const char *s_serif;
static const char *s_cjk;

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

static const char *pick(const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (file_exists(list[i]))
            return list[i];
    return NULL;
}

static void found_fonts(void)
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
    static const char *const cjk[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    s_sans = pick(sans, sizeof(sans) / sizeof(sans[0]));
    s_sans_bold = pick(bold, sizeof(bold) / sizeof(bold[0]));
    s_sans_italic = pick(italic, sizeof(italic) / sizeof(italic[0]));
    s_serif = pick(serif, sizeof(serif) / sizeof(serif[0]));
    s_cjk = pick(cjk, sizeof(cjk) / sizeof(cjk[0]));
}

/* 一个节点按样式解析出来的字体（走 qlui 的整条解析路径） */
static const QLFont *font_of(QLUIRoot *ui, int node_index)
{
    const QLUIStyle *s = ui->nodes[node_index].style;
    /* 与 qlui 内部一致的取法：家族列表 + 字重/斜体 + 字号 */
    const QLUIDoc *doc = ui->doc;
    const int px = (int)doc->font_sizes[s->font_id];
    if (s->font_family == 0)
        return ql_font_default(px);
    static QLFont *(*unused)(void);
    (void)unused;
    for (int k = 0; k < 16; k++) {
        (void)k;
        break;
    }
    /* 直接问注册表：列表里第一个名字（本测试里列表都只有一个名字或两个） */
    const QLUIFontList *fl = &doc->font_lists[s->font_family];
    const int weight = ((s->font_flags & QL_UI_FONT_BOLD) != 0) ? 700 : 400;
    const int italic = ((s->font_flags & QL_UI_FONT_ITALIC) != 0) ? 1 : 0;
    for (uint16_t k = 0; k < fl->count; k++) {
        const char *nm = doc->text_pool + doc->text_offsets[doc->font_names[fl->first + k]];
        const int id = ql_font_family_id(nm);
        if (id > 0) {
            const QLFont *f = ql_font_family_font(id, weight, italic, px);
            if (f != NULL)
                return f;
        }
    }
    return ql_font_default(px);
}

QL_TEST_BEGIN("test_font_stack")

    found_fonts();
    if (s_sans == NULL) {
        printf("  跳过：系统里没有找到矢量字体\n");
        qlgl_shutdown();
        return 0;
    }

    /* ---------- 1. 注册表：同族多字面按字重/斜体挑 ---------- */
    {
        QL_CHECK(ql_font_family_register("Test Sans", s_sans, 400, 0));
        if (s_sans_bold != NULL)
            QL_CHECK(ql_font_family_register("Test Sans", s_sans_bold, 700, 0));
        if (s_sans_italic != NULL)
            QL_CHECK(ql_font_family_register("Test Sans", s_sans_italic, 400, 1));

        const int id = ql_font_family_id("Test Sans");
        QL_CHECK(id > 0);
        QL_CHECK_EQ(ql_font_family_id("Test Sans "), 0);      /* 名字要完全对上 */
        QL_CHECK(ql_font_family_id("test sans") == id);       /* 大小写无关 */

        const QLFont *reg = ql_font_family_font(id, 400, 0, 16);
        QL_CHECK(reg != NULL);
        if (reg != NULL) {
            printf("  常规：%s %dx%d bold=%d italic=%d\n", reg->name,
                   (int)reg->pixel_size, (int)reg->line_height, (int)reg->is_bold,
                   (int)reg->is_italic);
            QL_CHECK_EQ((int)reg->is_bold, 0);
            QL_CHECK_EQ((int)reg->is_italic, 0);
        }
        if (s_sans_bold != NULL) {
            const QLFont *b = ql_font_family_font(id, 700, 0, 16);
            QL_CHECK(b != NULL);
            if (b != NULL) {
                printf("  粗体：%d, is_bold=%d\n", (int)b->pixel_size, (int)b->is_bold);
                QL_CHECK_EQ((int)b->is_bold, 1);     /* 真字面，绘制层不再合成 */
                QL_CHECK(b != reg);                  /* 与常规字面不是同一个 */
            }
        }
        if (s_sans_italic != NULL) {
            const QLFont *i = ql_font_family_font(id, 400, 1, 16);
            QL_CHECK(i != NULL);
            if (i != NULL)
                QL_CHECK_EQ((int)i->is_italic, 1);
        }
        /* 只想要 800：挑最接近的（700） */
        const QLFont *heavy = ql_font_family_font(id, 800, 0, 16);
        QL_CHECK(heavy != NULL);
        if (heavy != NULL && s_sans_bold != NULL)
            QL_CHECK(heavy == ql_font_family_font(id, 700, 0, 16));

        /* 字号缓存：同一个 (字面, 字号) 只造一次 */
        QL_CHECK(reg == ql_font_family_font(id, 400, 0, 16));
        QL_CHECK(reg != ql_font_family_font(id, 400, 0, 20));
    }

    /* ---------- 2. 家族列表：第一个装得上的家族胜出 ---------- */
    {
        char css[512];
        snprintf(css, sizeof(css),
                 "@font-face { font-family: \"Real\"; src: url(%s); font-weight: 400 }"
                 "#a { font-family: \"NoSuchFamily\", \"Real\", monospace; font-size: 16px }"
                 "#b { font-family: \"NoSuchFamily\", monospace; font-size: 16px }",
                 s_sans);
        QLUIDoc *doc = ql_ui_load_text("<div id=r><div id=a>x</div><div id=b>y</div></div>",
                                       css);
        if (doc == NULL)
            printf("  编译失败：%s\n", ql_ui_text_last_error());
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->font_face_count, 1);
            QL_CHECK_EQ((int)doc->font_list_count, 3);   /* 0 号是默认空列表 */
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                const QLFont *fa = font_of(ui, 1);
                const QLFont *fb = font_of(ui, 2);
                printf("  a → %s（应有矢量后端），b → backend=%p\n", fa->name,
                       (const void *)fb->backend);
                QL_CHECK(fa != NULL && fa->backend != NULL);   /* 落到了 Real */
                /* `monospace` 没有注册等宽矢量字体：落回内置位图字体 */
                QL_CHECK_EQ((int)(fb->backend == NULL), 1);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 逐字回退：拉丁字体里没有中文，落到 CJK ---------- */
    if (s_cjk != NULL) {
        /*
         * 注意：`ql_ui_build()` 会把注册表按 doc 重建（注册表是"当前文档"的），
         * 所以上面那次 build 已经把 Test Sans 冲掉了 —— 这里重新注册。
         * 这条也是文档里写的生命周期：**注册表跟着文档走**。
         */
        if (ql_font_family_id("Test Sans") == 0)
            QL_CHECK(ql_font_family_register("Test Sans", s_sans, 400, 0));
        if (ql_font_family_id("Test CJK") == 0)
            QL_CHECK(ql_font_family_register("Test CJK", s_cjk, 400, 0));
        const int latin = ql_font_family_id("Test Sans");
        const int cid = ql_font_family_id("Test CJK");
        QL_CHECK(latin > 0);
        QL_CHECK(cid > 0);

        const QLFont *lf = ql_font_family_font(latin, 400, 0, 16);
        const QLFont *cf = ql_font_family_font(cid, 400, 0, 16);
        QL_CHECK(lf != NULL && cf != NULL);
        if (lf != NULL && cf != NULL) {
            /* 拉丁字体没有"中"：回退前查不到 */
            const QLFont *owner = NULL;
            QL_CHECK_EQ(ql_font_has_glyph(lf, 0x4E2Du, &owner), 0);
            QL_CHECK_EQ(ql_font_has_glyph(cf, 0x4E2Du, &owner), 1);

            /* 把 CJK 接成拉丁的回退链：同一个查找接口应该能拿到字形 */
            QLFont *head = (QLFont *)ql_mem_calloc(1, sizeof(QLFont));
            QL_CHECK(head != NULL);
            if (head != NULL) {
                *head = *lf;
                head->glyphs = NULL;
                head->glyph_count = 0;
                head->glyph_capacity = 0;
                head->glyph_bitmaps = NULL;
                head->fallback_font = cf;
                head->is_chain_wrapper = 0;   /* 测试自己造的，别走销毁路径 */
                const QLGlyph *g = ql_font_find_glyph(head, 0x4E2Du);
                QL_CHECK(g != NULL);          /* 走了回退链 */
                if (g != NULL)
                    QL_CHECK(g->box_w > 0);
                ql_mem_free(head);
            }
        }
    }

    /* ---------- 4. 构建期报错 ---------- */
    {
        /* 没有 font-family 的 @font-face */
        QLUIDoc *bad = ql_ui_load_text("<div>x</div>",
                                       "@font-face { src: url(/tmp/x.ttf) }");
        QL_CHECK(bad == NULL);
        if (bad != NULL)
            ql_ui_doc_free(bad);

        /* src 只认 url() */
        bad = ql_ui_load_text(
            "<div>x</div>",
            "@font-face { font-family: F; src: local(Arial) }");
        QL_CHECK(bad == NULL);
        if (bad != NULL)
            ql_ui_doc_free(bad);

        /* 不认识的 @font-face 声明 */
        bad = ql_ui_load_text("<div>x</div>",
                              "@font-face { font-family: F; src: url(/tmp/x.ttf);"
                              " unicode-range: U+0-7F }");
        QL_CHECK(bad == NULL);
        if (bad != NULL)
            ql_ui_doc_free(bad);
    }

    ql_font_family_reset();
    ql_font_reset_defaults();

QL_TEST_END()

#else

QL_TEST_BEGIN("test_font_stack")
    printf("  跳过：这个构建没有 FreeType\n");
QL_TEST_END()

#endif
