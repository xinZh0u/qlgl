/**
 * @file    test_font_ft.c
 * @brief   FreeType 后端：加载、字形缓存有序性、中文度量与光栅化、缓存上限。
 *
 * 找不到矢量字体时整份测试跳过（返回 0），方便在没有字体的 CI 上跑。
 */
#include "../ql_test.h"

#if defined(QLGL_ENABLE_FREETYPE)

#include <stdlib.h>

static const char *find_font(void)
{
    static const char *const candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

/* 把字形画到一个 1×1 画布之外的大画布上，统计墨迹像素 */
static int ink_pixels(const QLFont *font, const char *text)
{
    QLDispDriver *disp = ql_disp_mem_create(120, 40, QL_COLOR_ARGB8888);
    if (disp == NULL)
        return -1;

    QLCanvas canvas;
    ql_canvas_from_disp(&canvas, disp);
    ql_canvas_clear(&canvas, QL_COLOR_WHITE);
    ql_canvas_draw_text(&canvas, font, text, -1, ql_rect_make(0, 0, 120, 40),
                        QL_COLOR_BLACK, QL_TEXT_ALIGN_LEFT);

    int ink = 0;
    for (ql_coord_t y = 0; y < 40; y++) {
        for (ql_coord_t x = 0; x < 120; x++) {
            if (ql_disp_mem_pixel(disp, x, y) != QL_COLOR_WHITE)
                ink++;
        }
    }
    ql_disp_mem_destroy(disp);
    return ink;
}

QL_TEST_BEGIN("test_font_ft")

    const char *path = find_font();
    if (path == NULL) {
        printf("  跳过：系统里没有找到矢量字体\n");
        qlgl_shutdown();
        return 0;
    }
    printf("  字体文件：%s\n", path);

    /* 1. 加载：度量合理，空格已预生成 */
    QLFont *font = ql_font_ft_load(path, -1, 16);
    QL_CHECK(font != NULL);
    if (font == NULL) {
        qlgl_shutdown();
        return 1;
    }
    QL_CHECK_EQ(ql_font_is_dynamic(font), 1);
    QL_CHECK_EQ(font->bpp, 8);
    QL_CHECK(font->ascent > 0);
    QL_CHECK(font->descent >= 0);
    QL_CHECK(font->line_height >= font->ascent + font->descent);
    QL_CHECK_EQ(font->pixel_size, 16);

    /* 2. 按需光栅化：查一个汉字后字形数增加，且度量合理 */
    uint16_t before = font->glyph_count;
    const QLGlyph *zh = ql_font_find_glyph(font, 0x4E2D);   /* 中 */
    QL_CHECK(zh != NULL);
    QL_CHECK(font->glyph_count > before);
    if (zh != NULL) {
        QL_CHECK(zh->codepoint == 0x4E2D);
        QL_CHECK(zh->box_w > 0 && zh->box_h > 0);
        QL_CHECK(zh->adv_w > 0);
        QL_CHECK(zh->box_w <= (int32_t)font->pixel_size + 4);
    }

    /* 3. 重复查找命中缓存，不再增长 */
    uint16_t after_first = font->glyph_count;
    QL_CHECK(ql_font_find_glyph(font, 0x4E2D) != NULL);
    QL_CHECK_EQ(font->glyph_count, after_first);

    /* 4. 缓存表始终按 codepoint 升序（二分查找的前提） */
    for (uint16_t i = 1; i < font->glyph_count; i++)
        QL_CHECK(font->glyphs[i - 1].codepoint < font->glyphs[i].codepoint);

    /* 5. 中文文本度量：5 个汉字宽度接近 5 个全角 */
    ql_coord_t w = 0, h = 0;
    ql_font_measure_text(font, "中文测试好", -1, &w, &h);
    QL_CHECK(w > 0);
    QL_CHECK_EQ(h, font->line_height);
    QL_CHECK(w >= font->pixel_size * 4);

    /* 6. UTF-8 解码与字形查找对齐 */
    uint32_t cp = 0;
    QL_CHECK_EQ(ql_utf8_decode("中", &cp), 3);
    QL_CHECK_EQ(cp, 0x4E2Du);

    /* 7. 真正画出来：中文与英文都有墨迹 */
    int ink_zh = ink_pixels(font, "中文");
    int ink_en = ink_pixels(font, "QLGL");
    printf("  墨迹像素：中文 %d，英文 %d\n", ink_zh, ink_en);
    QL_CHECK(ink_zh > 50);
    QL_CHECK(ink_en > 10);

    /* 8. 缺字：DejaVu 没有汉字，查不到就是 NULL，不崩 */
    if (strstr(path, "DejaVu") != NULL) {
        QL_CHECK(ql_font_find_glyph(font, 0x4E2D) == NULL);
    }

    /* 9. 行指针访问器越界返回 NULL */
    if (zh != NULL) {
        QL_CHECK(ql_font_glyph_row(font, zh, 0) != NULL);
        QL_CHECK(ql_font_glyph_row(font, zh, -1) == NULL);
        QL_CHECK(ql_font_glyph_row(font, zh, (int32_t)zh->box_h) == NULL);
    }

    /* 10. 缓存上限：调小后新字形被拒绝，已缓存的仍然可用 */
    ql_font_ft_set_cache_limit(font->glyph_count);
    QL_CHECK(ql_font_find_glyph(font, 0x6587) != NULL);    /* 文 可能已缓存 */
    QL_CHECK(ql_font_find_glyph(font, 0x9F98) == NULL);    /* 生僻字被丢 */
    ql_font_ft_set_cache_limit(2048);

    /* 11. 默认字体工厂：按字号返回缓存的动态字体 */
    QL_CHECK(ql_font_set_default_file(path, -1) == 1);
    QL_CHECK_STR(ql_font_default_file(), path);
    const QLFont *d16 = ql_font_default(16);
    const QLFont *d24 = ql_font_default(24);
    QL_CHECK(ql_font_is_dynamic(d16));
    QL_CHECK(ql_font_is_dynamic(d24));
    QL_CHECK(d16 != d24);
    QL_CHECK(ql_font_default(16) == d16);      /* 同字号复用 */
    QL_CHECK_EQ(d24->pixel_size, 24);
    QL_CHECK(d24->line_height > d16->line_height);

    /* 12. 非矢量路径仍然工作：内置位图字体 */
    QL_CHECK_EQ(ql_font_is_dynamic(&ql_font_mono_12), 0);
    QL_CHECK(ql_font_find_glyph(&ql_font_mono_12, 'A') != NULL);

    ql_font_destroy(font);

    /* 默认字体缓存由 shutdown 释放；这里先清掉以便检查内存归零 */
    ql_font_reset_defaults();
    ql_font_set_default_file(NULL, -1);
    QL_CHECK(ql_font_default_file() == NULL);

QL_TEST_END()

#else

QL_TEST_BEGIN("test_font_ft")
    printf("  跳过：未编译 FreeType 后端\n");
QL_TEST_END()

#endif
