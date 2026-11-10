/**
 * @file    test_text_shape.c
 * @brief   B8 整形与双向：连字/字距、字形下标缓存、阿拉伯文连写、RTL 排布。
 *
 * 两条后端都要测：HarfBuzz（系统里装了才测）和**内建**（永远测）。
 * `QL_TEXT_SHAPE=builtin` / `QL_TEXT_BIDI=builtin` 是给这两条路用的开关。
 */
#include "../ql_test.h"

#if defined(QLGL_ENABLE_FREETYPE)

#include <stdlib.h>
#include <string.h>

static const char *find_font(void)
{
    static const char *const cand[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    };
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        FILE *f = fopen(cand[i], "rb");
        if (f != NULL) {
            fclose(f);
            return cand[i];
        }
    }
    return NULL;
}

/* "مرحبا"（5 个阿拉伯字母，UTF-8） */
#define ARABIC_WORD  "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"

QL_TEST_BEGIN("test_text_shape")

    const char *path = find_font();
    if (path == NULL) {
        printf("  跳过：系统里没有找到矢量字体\n");
        qlgl_shutdown();
        return 0;
    }

    QLFont *font = ql_font_ft_load(path, -1, 16);
    QL_CHECK(font != NULL);
    if (font == NULL) {
        qlgl_shutdown();
        return 0;
    }

    QLShapeOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.ligatures = 1;
    opt.kerning = 1;

    const int has_hb = ql_text_shape_has_harfbuzz();
    printf("  整形后端：%s；双向后端：%s\n", ql_text_shape_backend(),
           ql_text_bidi_backend());

    /* ---------- 1. 量宽度与字形数：内建与 HarfBuzz 必须"同多同少" ---------- */
    {
        /* `ABCDE` 没有连字，两条路的字形数必须相同、宽度相近 */
        ql_coord_t w = 0;
        const int32_t n = ql_text_shape(font, "ABCDE", -1, &opt, NULL, 0, &w);
        printf("  ABCDE：%d 个字形、宽 %d\n", (int)n, (int)w);
        QL_CHECK_EQ((int)n, 5);
        QL_CHECK(w > 0);

        /* 字间距加在**每个字符之后**（含最后一个），与 CSS 一致 */
        QLShapeOpt ls = opt;
        ls.letter_spacing = 3;
        ql_coord_t w2 = 0;
        (void)ql_text_shape(font, "ABCDE", -1, &ls, NULL, 0, &w2);
        printf("  +3px 字间距：宽 %d（应 ≈ %d）\n", (int)w2, (int)(w + 15));
        QL_CHECK(w2 >= w + 15 - 2 && w2 <= w + 15 + 2);
    }

    /* ---------- 2. 连字：开了比关了少字形 ---------- */
    if (has_hb) {
        QLShapeOpt lig = opt;
        QLShapeOpt nolig = opt;
        nolig.ligatures = 0;
        ql_coord_t w_on = 0, w_off = 0;
        const int n_on = ql_text_shape(font, "office fjord affluent", -1, &lig, NULL, 0, &w_on);
        const int n_off =
            ql_text_shape(font, "office fjord affluent", -1, &nolig, NULL, 0, &w_off);
        printf("  连字：开 %d 字形 / 关 %d 字形（宽 %d / %d）\n", n_on, n_off, (int)w_on,
               (int)w_off);
        QL_CHECK(n_on < n_off);          /* ffi / fj / fl 之类合并了 */
        /* 合并没有改变行宽的量级 */
        QL_CHECK(w_on + 8 >= w_off && w_on <= w_off + 8);
    } else {
        printf("  跳过连字断言：这个环境没有 HarfBuzz\\n");
    }

    /* ---------- 3. 输出的字形数与 cap 的关系（分块绘制用） ---------- */
    {
        QLShapeGlyph buf[4];
        ql_coord_t w = 0;
        const int32_t n = ql_text_shape(font, "ABCDEFG", -1, &opt, buf, 4, &w);
        printf("  cap=4：写入 %d 个（总数 7），宽 %d\n", (int)n, (int)w);
        QL_CHECK_EQ((int)n, 7);          /* 个数照数，只是没写进去 */
        QL_CHECK_EQ((int)buf[0].adv, (int)buf[1].adv);   /* 等宽字母，推进相同 */
    }

    /* ---------- 4. 阿拉伯文：连写（字形下标 ≠ 逐字映射） ---------- */
    {
        ql_coord_t w_ar = 0;
        const int32_t n = ql_text_shape(font, ARABIC_WORD, -1, &opt, NULL, 0, &w_ar);
        printf("  阿拉伯文：%d 个字形，宽 %d\n", (int)n, (int)w_ar);
        QL_CHECK_EQ((int)n, 5);
        QL_CHECK(w_ar > 0);

        /* 双向：逻辑顺序 → 视觉顺序，头尾互换 */
        char vis[64];
        const int32_t vn = ql_text_bidi_log2vis(ARABIC_WORD, -1, 1, vis, (int)sizeof(vis));
        QL_CHECK(vn > 0);
        if (vn > 0) {
            /* 视觉顺序第一个字节应该是最后一个字母（ا = D8 A7） */
            printf("  视觉首字节：%02X %02X（期望 D8 A7 = ا）\n",
                   (unsigned char)vis[0], (unsigned char)vis[1]);
            QL_CHECK_EQ((unsigned char)vis[0], 0xD8);
            QL_CHECK_EQ((unsigned char)vis[1], 0xA7);
        }

        /* 纯 LTR 文本：原样拷贝（不重排） */
        const int32_t lp = ql_text_bidi_log2vis("hello", -1, 0, vis, (int)sizeof(vis));
        QL_CHECK_EQ((int)lp, 5);
        QL_CHECK(strcmp(vis, "hello") == 0);
        QL_CHECK_EQ(ql_text_bidi_has_rtl("hello", -1), 0);
        QL_CHECK_EQ(ql_text_bidi_has_rtl(ARABIC_WORD, -1), 1);
    }

    /* ---------- 5. RTL 段落 + 数字：数字整体保持 LTR ---------- */
    {
        const char *text = ARABIC_WORD " 12345";

        /* 先看双向重排本身（两条后端必须一致）：视觉顺序以 "12345" 开头 */
        char vis[64];
        const int32_t vn = ql_text_bidi_log2vis(text, -1, 1, vis, (int)sizeof(vis));
        QL_CHECK(vn > 5);
        if (vn > 5) {
            printf("  视觉顺序：%s\n", vis);
            QL_CHECK(strncmp(vis, "12345", 5) == 0);
        }

        /* 整形：段落方向不改变整行宽度（只是摆放顺序不同） */
        QLShapeOpt rtl = opt;
        rtl.par_rtl = 1;
        ql_coord_t w_rtl = 0, w_ltr = 0;
        const int32_t n_rtl = ql_text_shape(font, text, -1, &rtl, NULL, 0, &w_rtl);
        const int32_t n_ltr = ql_text_shape(font, text, -1, &opt, NULL, 0, &w_ltr);
        printf("  RTL 段落：%d 个字形 / 宽 %d；LTR 段落：%d 个字形 / 宽 %d\n",
               (int)n_rtl, (int)w_rtl, (int)n_ltr, (int)w_ltr);
        QL_CHECK_EQ((int)n_rtl, (int)n_ltr);
        QL_CHECK_EQ((int)w_rtl, (int)w_ltr);

        /*
         * 前 5 个字形的推进宽度应该彼此相同（那是 1..5，字体里等宽），
         * 而阿拉伯文那一组不会全等 —— 这就是"数字整体保持 LTR"的可观测结果。
         */
        QLShapeGlyph buf[32];
        const int32_t m = ql_text_shape(font, text, -1, &rtl, buf, 32, &w_rtl);
        QL_CHECK_EQ((int)m, (int)n_rtl);
        if (m >= 6) {
            int same = 1;
            for (int32_t k = 1; k < 5; k++)
                if (buf[k].adv != buf[0].adv)
                    same = 0;
            printf("  前 5 个字形：adv = %d %d %d %d %d\n", (int)buf[0].adv,
                   (int)buf[1].adv, (int)buf[2].adv, (int)buf[3].adv, (int)buf[4].adv);
            QL_CHECK(same);
        }
    }

    /* ---------- 6. 字形下标缓存：同一个字形取两次是同一个指针 ---------- */
    {
        const QLGlyph *g1 = ql_font_find_glyph_id(font, 19);
        QL_CHECK(g1 != NULL);
        if (g1 != NULL) {
            const QLGlyph *g2 = ql_font_find_glyph_id(font, 19);
            QL_CHECK(g1 == g2);              /* 命中缓存，不重复光栅化 */
            /* 合成码位不许和真实码位撞车 */
            QL_CHECK(g1->codepoint > 0x10FFFFu);
        }
    }

    ql_font_destroy(font);
    ql_text_shape_reset();
    ql_font_reset_defaults();

QL_TEST_END()

#else

QL_TEST_BEGIN("test_text_shape")
    printf("  跳过：这个构建没有 FreeType\n");
QL_TEST_END()

#endif
