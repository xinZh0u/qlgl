/**
 * @file    qlfont_ft.c
 * @brief   FreeType 后端：按需光栅化 + 有序字形缓存。
 *
 * 数据结构（每个动态字体一份）：
 *     glyphs[0..count)        按 codepoint 升序，二分查找复用 ql_font_find_glyph
 *     glyph_bitmaps[0..count) 每个字形自己的 8bpp 位图，行宽 = box_w
 *
 * 插入保持有序（memmove 搬数组），查找是纯二分。缓存满了就拒绝新字形并
 * 告警一次——宁可少画一个生僻字，也不做不可预测的淘汰抖动。
 */
#include <qlgl/qlgl.h>

#if defined(QLGL_ENABLE_FREETYPE)

#include <ft2build.h>
#include FT_FREETYPE_H

#include <string.h>

#define QL_FT_DEFAULT_CACHE   2048

typedef struct {
    FT_Library   lib;
    FT_Face      face;
    void        *file_data;      /* 我们自己读进内存的字体文件 */
    long         file_size;
    uint8_t      warned_full;
    uint8_t      reserved[3];
} QLFTFont;

static uint32_t s_cache_limit = QL_FT_DEFAULT_CACHE;
static char     s_default_path[256];
static int      s_default_face = -1;

void ql_font_ft_set_cache_limit(uint32_t glyphs)
{
    s_cache_limit = (glyphs < 1) ? 1 : glyphs;
}

int ql_font_ft_available(void)
{
    return 1;
}

/* ------------------------------ 位图拷贝 ------------------------------ */

/*
 * FreeType 的 bitmap 有 pitch 概念（rows 可能带 padding），且 bitmap.buffer
 * 只在下次渲染前有效，所以必须整块拷出来自己持有。
 */
static uint8_t *copy_bitmap(const FT_Bitmap *bmp, uint32_t box_w, uint32_t box_h)
{
    if (box_w == 0 || box_h == 0)
        return NULL;

    size_t bytes = (size_t)box_w * box_h;
    uint8_t *dst = (uint8_t *)ql_mem_alloc(bytes);
    if (dst == NULL)
        return NULL;

    if (bmp->pitch < 0) {
        /* 倒序位图：FreeType 只在少数变换下产生，直接按行倒着读 */
        for (uint32_t y = 0; y < box_h; y++) {
            const uint8_t *src = bmp->buffer + (ptrdiff_t)(box_h - 1 - y) * (-bmp->pitch);
            memcpy(dst + (size_t)y * box_w, src, box_w);
        }
    } else {
        for (uint32_t y = 0; y < box_h; y++)
            memcpy(dst + (size_t)y * box_w, bmp->buffer + (size_t)y * (size_t)bmp->pitch,
                   box_w);
    }
    return dst;
}

/* ------------------------------ 缓存插入 ------------------------------ */

/*
 * 扩容：glyphs 与 glyph_bitmaps 必须同步增长，否则下标对不上。
 * 先申请新块再拷贝，失败时保持原状（不破坏已有缓存）。
 */
static int grow_cache(QLFont *font)
{
    uint16_t old_cap = font->glyph_capacity;
    uint16_t new_cap = (uint16_t)((old_cap == 0) ? 128 : old_cap * 2);
    if (new_cap > 8192)
        return 0;

    QLGlyph *new_glyphs = (QLGlyph *)ql_mem_alloc((size_t)new_cap * sizeof(QLGlyph));
    const uint8_t **new_bitmaps =
        (const uint8_t **)ql_mem_alloc((size_t)new_cap * sizeof(uint8_t *));
    if (new_glyphs == NULL || new_bitmaps == NULL) {
        ql_mem_free(new_glyphs);
        ql_mem_free(new_bitmaps);
        return 0;
    }

    if (font->glyphs != NULL && font->glyph_count > 0) {
        memcpy(new_glyphs, font->glyphs, (size_t)font->glyph_count * sizeof(QLGlyph));
        memcpy(new_bitmaps, font->glyph_bitmaps,
               (size_t)font->glyph_count * sizeof(uint8_t *));
    }

    ql_mem_free((void *)font->glyphs);
    ql_mem_free((void *)font->glyph_bitmaps);
    font->glyphs = new_glyphs;
    font->glyph_bitmaps = (const uint8_t *const *)new_bitmaps;
    font->glyph_capacity = new_cap;
    return 1;
}

static const QLGlyph *insert_glyph(QLFont *font, uint32_t codepoint, QLGlyph glyph,
                                   uint8_t *bitmap)
{
    if (font->glyph_count + 1 > font->glyph_capacity) {
        if (!grow_cache(font))
            return NULL;
    }

    /* 二分找插入点，保持按 codepoint 升序 */
    uint32_t lo = 0, hi = font->glyph_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (font->glyphs[mid].codepoint < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }

    QLGlyph *dest = (QLGlyph *)(uintptr_t)font->glyphs;
    const uint8_t **bmp_dest = (const uint8_t **)(uintptr_t)font->glyph_bitmaps;

    if (lo < font->glyph_count) {
        memmove(&dest[lo + 1], &dest[lo],
                (size_t)(font->glyph_count - lo) * sizeof(QLGlyph));
        memmove(&bmp_dest[lo + 1], &bmp_dest[lo],
                (size_t)(font->glyph_count - lo) * sizeof(uint8_t *));
    }
    dest[lo] = glyph;
    bmp_dest[lo] = bitmap;
    ((QLFont *)(uintptr_t)font)->glyph_count++;
    return &dest[lo];
}

/* ------------------------------ 光栅化 ------------------------------ */

static const QLGlyph *ft_ensure_glyph(QLFont *font, uint32_t codepoint)
{
    QLFTFont *ft = (QLFTFont *)font->impl;
    if (ft == NULL || ft->face == NULL)
        return NULL;

    if (font->glyph_count >= s_cache_limit) {
        if (!ft->warned_full) {
            QL_LOGW("font \"%s\": glyph cache full (%u), further glyphs are dropped",
                    font->name ? font->name : "?", (unsigned)s_cache_limit);
            ft->warned_full = 1;
        }
        return NULL;
    }

    FT_UInt index = FT_Get_Char_Index(ft->face, codepoint);
    if (index == 0)
        return NULL;                       /* 字体里确实没有这个字 */

    if (FT_Load_Glyph(ft->face, index, FT_LOAD_DEFAULT) != 0)
        return NULL;
    if (FT_Render_Glyph(ft->face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return NULL;

    FT_GlyphSlot slot = ft->face->glyph;
    const FT_Bitmap *bmp = &slot->bitmap;

    QLGlyph glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.codepoint = codepoint;
    /* 26.6 定点 → 像素；advance 四舍五入避免累积误差 */
    glyph.adv_w = (int16_t)((slot->advance.x + 32) >> 6);
    glyph.box_w = (uint8_t)((bmp->width > 255) ? 255 : bmp->width);
    glyph.box_h = (uint8_t)((bmp->rows > 255) ? 255 : bmp->rows);
    glyph.ofs_x = (int8_t)slot->bitmap_left;
    glyph.ofs_y = (int8_t)(-slot->bitmap_top);   /* 相对基线，向上为负 */

    uint8_t *bitmap = NULL;
    if (glyph.box_w > 0 && glyph.box_h > 0) {
        bitmap = copy_bitmap(bmp, glyph.box_w, glyph.box_h);
        if (bitmap == NULL) {
            glyph.box_w = 0;
            glyph.box_h = 0;
        }
    }

    /* 空字形（空格）也要进缓存，否则每次都要走一遍 FT */
    const QLGlyph *inserted = insert_glyph(font, codepoint, glyph, bitmap);
    if (inserted == NULL) {
        ql_mem_free(bitmap);
        return NULL;
    }
    return inserted;
}

/* ------------------------------ 生命周期 ------------------------------ */

static void ft_destroy(QLFont *font)
{
    if (font == NULL)
        return;

    if (font->glyph_bitmaps != NULL) {
        const uint8_t **bm = (const uint8_t **)(uintptr_t)font->glyph_bitmaps;
        for (uint16_t i = 0; i < font->glyph_count; i++)
            ql_mem_free((void *)bm[i]);
        ql_mem_free(bm);
    }
    ql_mem_free((void *)font->glyphs);

    QLFTFont *ft = (QLFTFont *)font->impl;
    if (ft != NULL) {
        if (ft->face != NULL)
            FT_Done_Face(ft->face);
        if (ft->lib != NULL)
            FT_Done_FreeType(ft->lib);
        ql_mem_free(ft->file_data);
        ql_mem_free(ft);
    }
    ql_mem_free(font);
}

static const QLFontBackend s_ft_backend = {
    .ensure_glyph = ft_ensure_glyph,
    .destroy = ft_destroy,
};

/* 从内存读字体：避免 FreeType 自己持有文件句柄，退出时不需要额外管理 */
static void *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    void *data = ql_mem_alloc((size_t)size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        ql_mem_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = size;
    return data;
}

/* TTC 里挑字面：优先 family 名带 "SC"（简体中文）的 */
static int pick_face(FT_Face *faces, long count, int requested)
{
    if (requested >= 0 && requested < count)
        return requested;
    if (requested < 0 && count == 1)
        return 0;

    for (long i = 0; i < count; i++) {
        const char *family = faces[i]->family_name;
        if (family != NULL && strstr(family, "SC") != NULL)
            return (int)i;
    }
    return 0;
}

QLFont *ql_font_ft_load(const char *path, int face_index, int pixel_size)
{
    if (path == NULL || pixel_size <= 0)
        return NULL;

    long file_size = 0;
    void *data = read_file(path, &file_size);
    if (data == NULL) {
        QL_LOGE("font: cannot read \"%s\"", path);
        return NULL;
    }

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib) != 0) {
        QL_LOGE("font: FT_Init_FreeType failed");
        ql_mem_free(data);
        return NULL;
    }

    FT_Face face = NULL;
    if (FT_New_Memory_Face(lib, (const FT_Byte *)data, file_size, 0, &face) != 0) {
        QL_LOGE("font: not a valid font file: \"%s\"", path);
        FT_Done_FreeType(lib);
        ql_mem_free(data);
        return NULL;
    }

    /* 多字面（TTC）：把每个字面都打开一次，选好后关掉其余的 */
    long face_count = (face->num_faces > 0) ? (long)face->num_faces : 1;
    int chosen = 0;
    if (face_count > 1) {
        FT_Face *faces = (FT_Face *)ql_mem_calloc((size_t)face_count, sizeof(FT_Face));
        if (faces != NULL) {
            for (long i = 0; i < face_count; i++) {
                if (FT_New_Memory_Face(lib, (const FT_Byte *)data, file_size, i,
                                       &faces[i]) != 0)
                    faces[i] = NULL;
            }
            chosen = pick_face(faces, face_count, face_index);
            for (long i = 0; i < face_count; i++) {
                if (faces[i] != NULL && i != chosen)
                    FT_Done_Face(faces[i]);
            }
            if (faces[chosen] != NULL)
                face = faces[chosen];   /* 复用，不再重新打开 */
            ql_mem_free(faces);
        }
    }

    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size) != 0) {
        QL_LOGE("font: FT_Set_Pixel_Sizes(%d) failed", pixel_size);
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        ql_mem_free(data);
        return NULL;
    }

    QLFTFont *ft = (QLFTFont *)ql_mem_calloc(1, sizeof(QLFTFont));
    QLFont *font = (QLFont *)ql_mem_calloc(1, sizeof(QLFont));
    if (ft == NULL || font == NULL) {
        ql_mem_free(ft);
        ql_mem_free(font);
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        ql_mem_free(data);
        return NULL;
    }

    ft->lib = lib;
    ft->face = face;
    ft->file_data = data;
    ft->file_size = file_size;

    /* 度量：用字体自己的缩放度量，保证多行基线一致 */
    ql_coord_t ascent = (ql_coord_t)((face->size->metrics.ascender + 32) >> 6);
    ql_coord_t descent = (ql_coord_t)((-face->size->metrics.descender + 32) >> 6);
    ql_coord_t height = (ql_coord_t)((face->size->metrics.height + 32) >> 6);
    if (height < ascent + descent)
        height = ascent + descent;

    font->name = "ft";
    font->bpp = 8;
    font->ascent = (int16_t)ascent;
    font->descent = (int16_t)descent;
    font->line_height = (int16_t)height;
    font->fallback = 0;
    font->glyph_count = 0;
    font->glyphs = NULL;
    font->bitmap = NULL;
    font->glyph_bitmaps = NULL;
    font->backend = &s_ft_backend;
    font->impl = ft;
    font->glyph_capacity = 0;
    font->pixel_size = (uint16_t)pixel_size;

    /* 预先生成空格，包住最常见的一次插入 */
    (void)ft_ensure_glyph(font, ' ');
    return font;
}

/* ------------------------------ 默认字体 ------------------------------ */

static QLFont *default_factory(int pixel_size)
{
    if (s_default_path[0] == '\0')
        return NULL;
    return ql_font_ft_load(s_default_path, s_default_face, pixel_size);
}

int ql_font_set_default_file(const char *path, int face_index)
{
    if (path == NULL) {
        s_default_path[0] = '\0';
        s_default_face = -1;
        ql_font_set_default_factory(NULL);
        return 0;
    }

    /* 先试加载一次，别把坏路径留成默认值 */
    QLFont *probe = ql_font_ft_load(path, face_index, 16);
    if (probe == NULL)
        return 0;
    ql_font_destroy(probe);

    size_t n = strlen(path);
    if (n + 1 > sizeof(s_default_path))
        n = sizeof(s_default_path) - 1;
    memcpy(s_default_path, path, n);
    s_default_path[n] = '\0';
    s_default_face = face_index;

    ql_font_set_default_factory(default_factory);
    QL_LOGI("font: default face \"%s\"", path);
    return 1;
}

const char *ql_font_default_file(void)
{
    return (s_default_path[0] != '\0') ? s_default_path : NULL;
}

#endif /* QLGL_ENABLE_FREETYPE */
