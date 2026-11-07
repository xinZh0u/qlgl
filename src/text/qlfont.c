/**
 * @file    qlfont.c
 * @brief   字形查找、UTF-8 编解码、文本度量。
 */
#include <qlgl/qlgl.h>

const QLGlyph *ql_font_find_glyph(const QLFont *font, uint32_t codepoint)
{
    if (font == NULL || font->glyphs == NULL || font->glyph_count == 0)
        return NULL;

    uint32_t lo = 0;
    uint32_t hi = font->glyph_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t cp = font->glyphs[mid].codepoint;
        if (cp == codepoint)
            return &font->glyphs[mid];
        if (cp < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* 缺字：退回 fallback 字形；fallback 为 0 时返回 NULL */
    if (font->fallback != 0 && font->fallback != codepoint)
        return ql_font_find_glyph(font, font->fallback);
    return NULL;
}

uint32_t ql_font_glyph_stride(const QLFont *font, const QLGlyph *glyph)
{
    if (font == NULL || glyph == NULL || glyph->box_w == 0)
        return 0;
    switch (font->bpp) {
    case 1:  return (uint32_t)((glyph->box_w + 7) / 8);
    case 4:  return (uint32_t)((glyph->box_w + 1) / 2);
    case 8:  return (uint32_t)glyph->box_w;
    default: return 0;
    }
}

uint8_t ql_font_glyph_alpha(const QLFont *font, const QLGlyph *glyph, int32_t x, int32_t y)
{
    if (font == NULL || glyph == NULL)
        return 0;
    if (x < 0 || y < 0 || x >= (int32_t)glyph->box_w || y >= (int32_t)glyph->box_h)
        return 0;

    uint32_t stride = ql_font_glyph_stride(font, glyph);
    const uint8_t *row = font->bitmap + glyph->data_offset + (uint32_t)y * stride;

    switch (font->bpp) {
    case 1: {
        uint8_t byte = row[(uint32_t)x / 8u];
        return (byte & (0x80u >> (x % 8))) ? 255u : 0u;
    }
    case 4: {
        uint8_t byte = row[(uint32_t)x / 2u];
        uint8_t nibble = (x % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
        return (uint8_t)(nibble * 17u);          /* 0..15 -> 0..255 */
    }
    case 8:
        return row[x];
    default:
        return 0;
    }
}

int32_t ql_utf8_decode(const char *text, uint32_t *codepoint)
{
    if (text == NULL || codepoint == NULL)
        return 0;

    const uint8_t *s = (const uint8_t *)text;
    uint8_t b0 = s[0];

    if (b0 < 0x80u) {
        *codepoint = b0;
        return 1;
    }

    int32_t need = 0;
    uint32_t cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) {
        need = 1; cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        need = 2; cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        need = 3; cp = b0 & 0x07u;
    } else {
        *codepoint = 0xFFFDu;
        return 1;
    }

    for (int32_t i = 1; i <= need; i++) {
        uint8_t b = s[i];
        if ((b & 0xC0u) != 0x80u) {
            *codepoint = 0xFFFDu;
            return 1;                 /* 截断：只消费当前字节，避免死循环 */
        }
        cp = (cp << 6) | (uint32_t)(b & 0x3Fu);
    }

    *codepoint = cp;
    return need + 1;
}

int32_t ql_utf8_encode(uint32_t codepoint, char *buf)
{
    if (buf == NULL)
        return 0;

    if (codepoint < 0x80u) {
        buf[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800u) {
        buf[0] = (char)(0xC0u | (codepoint >> 6));
        buf[1] = (char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint < 0x10000u) {
        buf[0] = (char)(0xE0u | (codepoint >> 12));
        buf[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    buf[0] = (char)(0xF0u | (codepoint >> 18));
    buf[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    buf[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    buf[3] = (char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

ql_coord_t ql_font_line_width(const QLFont *font, const char *text, int32_t len)
{
    if (font == NULL || text == NULL)
        return 0;

    ql_coord_t width = 0;
    int32_t i = 0;
    while ((len < 0 ? text[i] != '\0' : i < len) && text[i] != '\0') {
        uint32_t cp = 0;
        int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        const QLGlyph *glyph = ql_font_find_glyph(font, cp);
        width += (glyph != NULL) ? glyph->adv_w : 0;
        i += used;
    }
    return width;
}

uint32_t ql_font_line_count(const char *text, int32_t len)
{
    if (text == NULL)
        return 1;

    uint32_t lines = 1;
    int32_t i = 0;
    while ((len < 0 ? text[i] != '\0' : i < len) && text[i] != '\0') {
        if (text[i] == '\n')
            lines++;
        i++;
    }
    return lines;
}

void ql_font_measure_text(const QLFont *font, const char *text, int32_t len,
                          ql_coord_t *out_w, ql_coord_t *out_h)
{
    ql_coord_t width = 0;
    ql_coord_t height = 0;

    if (font != NULL) {
        if (text == NULL) {
            height = font->line_height;
        } else {
            height = (ql_coord_t)ql_font_line_count(text, len) * font->line_height;

            int32_t i = 0;
            int32_t line_start = 0;
            for (;;) {
                int at_end = (len < 0) ? (text[i] == '\0')
                                       : (i >= len || text[i] == '\0');
                if (at_end || text[i] == '\n') {
                    ql_coord_t w = ql_font_line_width(font, text + line_start,
                                                      i - line_start);
                    if (w > width)
                        width = w;
                    if (at_end)
                        break;
                    line_start = i + 1;
                }
                i++;
            }
        }
    }

    if (out_w != NULL)
        *out_w = width;
    if (out_h != NULL)
        *out_h = height;
}
