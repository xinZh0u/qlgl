/**
 * @file    test_font.c
 * @brief   示例：位图字体 —— 字形查找、缺字兜底、灰度读取、UTF-8 编解码、文本度量。
 */
#include "../ql_test.h"

QL_TEST_BEGIN("test_font")

    /* 1. 内置字体基本属性 */
    const QLFont *f = &ql_font_mono_16;
    QL_CHECK(f->bpp == 4);
    QL_CHECK(f->glyph_count == 95);          /* ASCII 32..126 */
    QL_CHECK(f->line_height > 0);
    QL_CHECK(f->ascent > 0);
    QL_CHECK(f->ascent + f->descent == f->line_height);
    QL_CHECK_STR(f->name, "DejaVuSansMono-16");
    QL_CHECK(&ql_font_mono_12 != &ql_font_mono_16);

    /* 2. 字形查找：命中 */
    const QLGlyph *g_a = ql_font_find_glyph(f, 'A');
    QL_CHECK(g_a != NULL);
    QL_CHECK_EQ(g_a->codepoint, 'A');
    QL_CHECK(g_a->adv_w > 0);
    QL_CHECK(g_a->box_w > 0 && g_a->box_h > 0);
    QL_CHECK(g_a->box_h <= f->line_height);

    /* 等宽字体：所有可打印字符前进宽度相同 */
    const QLGlyph *g_i = ql_font_find_glyph(f, 'i');
    const QLGlyph *g_w = ql_font_find_glyph(f, 'W');
    QL_CHECK_EQ(g_i->adv_w, g_w->adv_w);
    QL_CHECK_EQ(g_a->adv_w, g_w->adv_w);

    /* 3. 空格：没有墨迹，但仍要前进 */
    const QLGlyph *g_space = ql_font_find_glyph(f, ' ');
    QL_CHECK(g_space != NULL);
    QL_CHECK_EQ(g_space->box_w, 0);
    QL_CHECK(g_space->adv_w > 0);

    /* 4. 缺字：落在 fallback('?') 上 */
    const QLGlyph *g_cn = ql_font_find_glyph(f, 0x4F60);    /* 你 */
    const QLGlyph *g_q = ql_font_find_glyph(f, '?');
    QL_CHECK(g_cn != NULL);
    QL_CHECK(g_cn == g_q);
    QL_CHECK(f->fallback == '?');

    /* 5. 位图读取：墨迹内部有非零覆盖率，边界外一律 0 */
    int inked = 0;
    for (int y = 0; y < (int)g_a->box_h; y++) {
        for (int x = 0; x < (int)g_a->box_w; x++) {
            if (ql_font_glyph_alpha(f, g_a, x, y) > 0)
                inked++;
        }
    }
    QL_CHECK(inked > 0);
    QL_CHECK_EQ(ql_font_glyph_alpha(f, g_a, -1, 0), 0);
    QL_CHECK_EQ(ql_font_glyph_alpha(f, g_a, g_a->box_w, 0), 0);
    QL_CHECK_EQ(ql_font_glyph_alpha(f, g_a, 0, -1), 0);
    QL_CHECK_EQ(ql_font_glyph_alpha(NULL, g_a, 0, 0), 0);
    QL_CHECK_EQ(ql_font_glyph_alpha(f, NULL, 0, 0), 0);

    /* 4bpp 的覆盖率是 17 的倍数 */
    for (int y = 0; y < (int)g_a->box_h; y++) {
        for (int x = 0; x < (int)g_a->box_w; x++) {
            uint8_t a = ql_font_glyph_alpha(f, g_a, x, y);
            QL_CHECK_EQ(a % 17, 0);
        }
    }

    /* 6. 行字节数 */
    QL_CHECK_EQ(ql_font_glyph_stride(f, g_a), (g_a->box_w + 1u) / 2u);
    QL_CHECK_EQ(ql_font_glyph_stride(f, g_space), 0);
    QL_CHECK_EQ(ql_font_glyph_stride(NULL, g_a), 0);

    /* 7. UTF-8 解码：1/2/3/4 字节 + 非法序列 */
    uint32_t cp = 0;
    QL_CHECK_EQ(ql_utf8_decode("A", &cp), 1);
    QL_CHECK_EQ(cp, 'A');
    QL_CHECK_EQ(ql_utf8_decode("\xC3\xA9", &cp), 2);        /* é */
    QL_CHECK_EQ(cp, 0x00E9);
    QL_CHECK_EQ(ql_utf8_decode("\xE4\xBD\xA0", &cp), 3);    /* 你 */
    QL_CHECK_EQ(cp, 0x4F60);
    QL_CHECK_EQ(ql_utf8_decode("\xF0\x9F\x98\x80", &cp), 4);/* 😀 */
    QL_CHECK_EQ(cp, 0x1F600);
    QL_CHECK_EQ(ql_utf8_decode("\xB0", &cp), 1);            /* 孤立续字节 */
    QL_CHECK_EQ(cp, 0xFFFD);
    QL_CHECK_EQ(ql_utf8_decode("\xE4\xBD", &cp), 1);        /* 截断的多字节 */
    QL_CHECK_EQ(cp, 0xFFFD);
    QL_CHECK_EQ(ql_utf8_decode(NULL, &cp), 0);

    /* 7b. 合法前缀长度：截断/非法字节都停在上一个完整字符边界 */
    QL_CHECK_EQ(ql_utf8_valid_len("ABC", 3), 3);
    QL_CHECK_EQ(ql_utf8_valid_len("\xE4\xBD\xA0", 3), 3);   /* 你 */
    QL_CHECK_EQ(ql_utf8_valid_len("\xE4\xBD\xA0", 2), 0);   /* 尾巴被截断 */
    QL_CHECK_EQ(ql_utf8_valid_len("\xE4\xBD\xA0Z", 2), 0);
    QL_CHECK_EQ(ql_utf8_valid_len("A\xB0", 2), 1);           /* 孤儿续字节 */
    QL_CHECK_EQ(ql_utf8_valid_len("A\xE4\xBD", 3), 1);      /* 后面跟半截 */
    QL_CHECK_EQ(ql_utf8_valid_len("A\xFF", 2), 1);
    QL_CHECK_EQ(ql_utf8_valid_len("", 0), 0);
    QL_CHECK_EQ(ql_utf8_valid_len(NULL, 5), 0);
    QL_CHECK_EQ(ql_utf8_valid_len("\xF0\x9F\x98\x80", 3), 0); /* 4 字节只给 3 */

    /* 8. UTF-8 编码：与解码互逆 */
    char buf[8];
    static const uint32_t samples[] = { 'A', 0x00E9, 0x4F60, 0x1F600 };
    for (size_t i = 0; i < QL_ARRAY_SIZE(samples); i++) {
        int32_t n = ql_utf8_encode(samples[i], buf);
        QL_CHECK(n >= 1 && n <= 4);
        uint32_t back = 0;
        QL_CHECK_EQ(ql_utf8_decode(buf, &back), n);
        QL_CHECK_EQ(back, samples[i]);
    }

    /* 9. 度量：单行 */
    ql_coord_t w = 0;
    ql_coord_t h = 0;
    ql_font_measure_text(f, "ABC", -1, &w, &h);
    QL_CHECK_EQ(w, g_a->adv_w * 3);
    QL_CHECK_EQ(h, f->line_height);

    /* 10. 度量：多行取最宽行，高度按行数累加 */
    ql_font_measure_text(f, "ABC\nAB\nA", -1, &w, &h);
    QL_CHECK_EQ(w, g_a->adv_w * 3);
    QL_CHECK_EQ(h, f->line_height * 3);
    QL_CHECK_EQ(ql_font_line_count("ABC\nAB\nA", -1), 3);
    QL_CHECK_EQ(ql_font_line_count("", -1), 1);
    QL_CHECK_EQ(ql_font_line_count(NULL, -1), 1);

    /* 11. 指定长度时不会越过结尾 */
    ql_font_measure_text(f, "ABCD", 2, &w, &h);
    QL_CHECK_EQ(w, g_a->adv_w * 2);
    QL_CHECK_EQ(ql_font_line_width(f, "ABC", -1), g_a->adv_w * 3);
    QL_CHECK_EQ(ql_font_line_width(f, "ABC", 1), g_a->adv_w);

    /* 12. 非 ASCII 文本按 fallback 宽度计算，不会死循环 */
    ql_font_measure_text(f, "你好", -1, &w, &h);
    QL_CHECK_EQ(w, g_q->adv_w * 2);
    QL_CHECK_EQ(h, f->line_height);

    /* 13. NULL 安全 */
    ql_font_measure_text(NULL, "x", -1, &w, &h);
    QL_CHECK_EQ(w, 0);
    QL_CHECK_EQ(h, 0);
    QL_CHECK_EQ(ql_font_line_width(NULL, "x", -1), 0);
    QL_CHECK(ql_font_find_glyph(NULL, 'A') == NULL);

QL_TEST_END()
