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
    void        *file_data;      /* 字体文件数据（可能是我们读的，也可能是调用方给的） */
    long         file_size;
    uint8_t      owns_data;      /* 1 = file_data 由我们分配，销毁时要 free */
    uint8_t      warned_full;
    uint8_t      reserved[2];
} QLFTFont;

static uint32_t s_cache_limit = QL_FT_DEFAULT_CACHE;
static char     s_default_path[256];
static int      s_default_face = -1;

/*
 * 共享的 FreeType 库句柄。
 *
 * 早先每个字体文件各开一个 FT_Library（几十 KB 的常驻内存 × 字体数）。
 * 文本栈一来就要同时挂着"正文 / 等宽 / 粗体 / 斜体"好几张字面，所以改成
 * 进程内一个库、多个 face。
 */
static FT_Library s_shared_lib;

void ql_font_ft_set_cache_limit(uint32_t glyphs)
{
    s_cache_limit = (glyphs < 1) ? 1 : glyphs;
}

int ql_font_ft_available(void)
{
    return 1;
}

/* 拿共享的 FT_Library（第一次调用时初始化；失败就一直是 NULL） */
static FT_Library shared_library(void)
{
    if (s_shared_lib == NULL) {
        if (FT_Init_FreeType(&s_shared_lib) != 0) {
            s_shared_lib = NULL;
            QL_LOGE("font: FT_Init_FreeType failed");
        }
    }
    return s_shared_lib;
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

/*
 * 按**字形下标**光栅化（整形后端给出的就是下标）。
 *
 * 缓存键用 `QL_GLYPH_ID_BASE + gid` 这个合成码位：真实码位不超过 0x10FFFF，
 * 所以不会撞车，也不用给 QLGlyph 加字段（缓存、位图、二分查找全部原样复用）。
 */
static const QLGlyph *ft_ensure_glyph_id(QLFont *font, uint32_t glyph_id)
{
    QLFTFont *ft = (QLFTFont *)font->impl;
    if (ft == NULL || ft->face == NULL)
        return NULL;
    if (font->glyph_count >= s_cache_limit)
        return NULL;

    const uint32_t key = QL_GLYPH_ID_BASE + glyph_id;

    /* 已经被别人缓存过就直接用（同一行里同一个字形会重复出现） */
    uint32_t lo = 0, hi = font->glyph_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (font->glyphs[mid].codepoint == key)
            return &font->glyphs[mid];
        if (font->glyphs[mid].codepoint < key)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (FT_Load_Glyph(ft->face, (FT_UInt)glyph_id, FT_LOAD_DEFAULT) != 0)
        return NULL;
    if (FT_Render_Glyph(ft->face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return NULL;

    FT_GlyphSlot slot = ft->face->glyph;
    const FT_Bitmap *bmp = &slot->bitmap;

    QLGlyph glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.codepoint = key;
    glyph.adv_w = (int16_t)((slot->advance.x + 32) >> 6);
    glyph.box_w = (uint8_t)((bmp->width > 255) ? 255 : bmp->width);
    glyph.box_h = (uint8_t)((bmp->rows > 255) ? 255 : bmp->rows);
    glyph.ofs_x = (int8_t)slot->bitmap_left;
    glyph.ofs_y = (int8_t)(-slot->bitmap_top);

    uint8_t *bitmap = NULL;
    if (glyph.box_w > 0 && glyph.box_h > 0) {
        bitmap = copy_bitmap(bmp, glyph.box_w, glyph.box_h);
        if (bitmap == NULL) {
            glyph.box_w = 0;
            glyph.box_h = 0;
        }
    }
    const QLGlyph *inserted = insert_glyph(font, key, glyph, bitmap);
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

    /*
     * 链头包装体（`is_chain_wrapper`）只是"借"了 impl：自己的字形缓存已经在上
     * 面放掉了，这里只放结构体。原字体（注册表缓存）会把 impl 放掉。
     */
    if (font->is_chain_wrapper) {
        ql_mem_free(font);
        return;
    }

    QLFTFont *ft = (QLFTFont *)font->impl;
    if (ft != NULL) {
        if (ft->face != NULL)
            FT_Done_Face(ft->face);
        /* 库是共享的：这里只放 face，不放库（qlgl_shutdown 统一放） */
        if (ft->owns_data)
            ql_mem_free(ft->file_data);
        ql_mem_free(ft);
    }
    ql_mem_free(font);
}

static const QLFontBackend s_ft_backend = {
    .ensure_glyph = ft_ensure_glyph,
    .ensure_glyph_id = ft_ensure_glyph_id,
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

/*
 * 从**内存**造一个动态字体。`owns_data` 表示这块内存是不是我们分配的
 * （路径加载时是我们读的，注册表把文件读进来时也是；调用方给的内存不是）。
 */
static QLFont *ft_font_from_memory(void *data, long file_size, int face_index,
                                   int pixel_size, int owns_data)
{
    if (data == NULL || file_size <= 0 || pixel_size <= 0)
        return NULL;

    FT_Library lib = shared_library();
    if (lib == NULL)
        return NULL;

    FT_Face face = NULL;
    if (FT_New_Memory_Face(lib, (const FT_Byte *)data, file_size, 0, &face) != 0) {
        QL_LOGE("font: not a valid font file");
        if (owns_data)
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
            if (faces[chosen] != NULL) {
                /* 剩下来的那个就是要用的；原来 0 号字面也要放掉 */
                if (face != faces[chosen])
                    FT_Done_Face(face);
                face = faces[chosen];
            }
            ql_mem_free(faces);
        }
    }

    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size) != 0) {
        QL_LOGE("font: FT_Set_Pixel_Sizes(%d) failed", pixel_size);
        FT_Done_Face(face);
        if (owns_data)
            ql_mem_free(data);
        return NULL;
    }

    QLFTFont *ft = (QLFTFont *)ql_mem_calloc(1, sizeof(QLFTFont));
    QLFont *font = (QLFont *)ql_mem_calloc(1, sizeof(QLFont));
    if (ft == NULL || font == NULL) {
        ql_mem_free(ft);
        ql_mem_free(font);
        FT_Done_Face(face);
        if (owns_data)
            ql_mem_free(data);
        return NULL;
    }

    ft->lib = lib;
    ft->face = face;
    ft->file_data = data;
    ft->file_size = file_size;
    ft->owns_data = (uint8_t)(owns_data ? 1 : 0);

    /* 度量：用字体自己的缩放度量，保证多行基线一致 */
    ql_coord_t ascent = (ql_coord_t)((face->size->metrics.ascender + 32) >> 6);
    ql_coord_t descent = (ql_coord_t)((-face->size->metrics.descender + 32) >> 6);
    ql_coord_t height = (ql_coord_t)((face->size->metrics.height + 32) >> 6);
    if (height < ascent + descent)
        height = ascent + descent;

    font->name = (face->family_name != NULL) ? face->family_name : "ft";
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
    /* 字面自带的粗/斜体（`FT_STYLE_FLAG_*`）：绘制层据此决定要不要合成 */
    font->is_bold = (uint8_t)((face->style_flags & FT_STYLE_FLAG_BOLD) ? 1 : 0);
    font->is_italic = (uint8_t)((face->style_flags & FT_STYLE_FLAG_ITALIC) ? 1 : 0);

    /* 预先生成空格，包住最常见的一次插入 */
    (void)ft_ensure_glyph(font, ' ');
    return font;
}

QLFont *ql_font_ft_load_mem(const void *data, long size, int face_index, int pixel_size)
{
    if (data == NULL || size <= 0)
        return NULL;
    /* 拷贝一份：调用方可能在字体还活着的时候释放自己的缓冲 */
    void *copy = ql_mem_alloc((size_t)size);
    if (copy == NULL)
        return NULL;
    memcpy(copy, data, (size_t)size);
    return ft_font_from_memory(copy, size, face_index, pixel_size, 1);
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
    QLFont *font = ft_font_from_memory(data, file_size, face_index, pixel_size, 1);
    if (font == NULL)
        QL_LOGE("font: cannot load \"%s\"", path);
    return font;
}

/* ============================== 文本栈注册表 ============================== */
#if QL_FEAT_CSS_FONT_STACK

#define QL_FAMILY_NAME_MAX  48
#define QL_FAMILY_PATH_MAX  192

typedef struct QLFTCache {
    struct QLFTCache *next;
    int      px;
    QLFont  *font;
} QLFTCache;

typedef struct {
    char       family[QL_FAMILY_NAME_MAX];
    char       path[QL_FAMILY_PATH_MAX];   /* 空 = 用 mem */
    const void *mem;                       /* 调用方给的内存字体（只读，不拷贝） */
    long       mem_size;
    int        face_index;
    uint16_t   weight;
    uint8_t    italic;
    uint8_t    used;
    QLFTCache *cache;                      /* 按字号的缓存 */
} QLFTFaceEntry;

static QLFTFaceEntry s_faces[QL_CFG_UI_FONT_FACE_MAX];
static int s_face_count;

static int family_match(const char *a, const char *b)
{
    /* 家族名比较：忽略大小写（CSS 的家族名本来就是大小写无关的） */
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int find_entry(const char *family, int weight, int italic)
{
    for (int i = 0; i < s_face_count; i++) {
        if (!s_faces[i].used || !family_match(s_faces[i].family, family))
            continue;
        if ((int)s_faces[i].weight == weight && (int)s_faces[i].italic == (italic ? 1 : 0))
            return i;
    }
    return -1;
}

static int add_entry(const char *family, int weight, int italic)
{
    if (family == NULL || family[0] == '\0')
        return -1;
    const size_t n = strlen(family);
    if (n + 1 > QL_FAMILY_NAME_MAX)
        return -1;

    int slot = find_entry(family, weight, italic);
    if (slot < 0) {
        if (s_face_count >= (int)QL_CFG_UI_FONT_FACE_MAX) {
            QL_LOGE("font: family table full (%d), \"%s\" dropped",
                    (int)QL_CFG_UI_FONT_FACE_MAX, family);
            return -1;
        }
        slot = s_face_count++;
        memset(&s_faces[slot], 0, sizeof(s_faces[slot]));
        memcpy(s_faces[slot].family, family, n + 1);
        s_faces[slot].face_index = -1;
    } else {
        /* 重复注册：把旧缓存字体放掉，换成新的来源 */
        QLFTCache *c = s_faces[slot].cache;
        while (c != NULL) {
            QLFTCache *next = c->next;
            ql_font_destroy(c->font);
            ql_mem_free(c);
            c = next;
        }
        s_faces[slot].cache = NULL;
    }
    s_faces[slot].weight = (uint16_t)((weight < 1) ? 400 : ((weight > 1000) ? 1000 : weight));
    s_faces[slot].italic = (uint8_t)(italic ? 1 : 0);
    s_faces[slot].used = 1;
    return slot;
}

int ql_font_family_register(const char *family, const char *path, int weight, int italic)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    /* 先真的读一遍：坏路径/坏文件在这里就报错，不留到绘制时 */
    long size = 0;
    void *data = read_file(path, &size);
    if (data == NULL) {
        QL_LOGE("font: @font-face \"%s\": cannot read \"%s\"", family, path);
        return 0;
    }
    ql_mem_free(data);

    const int slot = add_entry(family, weight, italic);
    if (slot < 0)
        return 0;
    const size_t pn = strlen(path);
    if (pn + 1 > QL_FAMILY_PATH_MAX) {
        QL_LOGE("font: @font-face \"%s\": path too long", family);
        return 0;
    }
    memcpy(s_faces[slot].path, path, pn + 1);
    s_faces[slot].mem = NULL;
    s_faces[slot].mem_size = 0;
    return 1;
}

int ql_font_family_register_mem(const char *family, const void *data, long size, int weight,
                                int italic)
{
    if (data == NULL || size <= 0)
        return 0;
    const int slot = add_entry(family, weight, italic);
    if (slot < 0)
        return 0;
    s_faces[slot].path[0] = '\0';
    s_faces[slot].mem = data;
    s_faces[slot].mem_size = size;
    return 1;
}

int ql_font_family_id(const char *family)
{
    if (family == NULL || family[0] == '\0')
        return 0;
    for (int i = 0; i < s_face_count; i++) {
        if (s_faces[i].used && family_match(s_faces[i].family, family))
            return i + 1;          /* 0 留给"默认" */
    }
    return 0;
}

/* 这个家族里最接近 (weight, italic) 的字面 */
static int best_entry(int family_id, int weight, int italic)
{
    const int want = (family_id > 0 && family_id <= s_face_count) ? family_id - 1 : -1;
    if (want < 0 || !s_faces[want].used)
        return -1;
    const char *fam = s_faces[want].family;

    int best = -1;
    int best_score = 0x7FFFFFFF;
    for (int i = 0; i < s_face_count; i++) {
        if (!s_faces[i].used || !family_match(s_faces[i].family, fam))
            continue;
        const int dw = (int)s_faces[i].weight - weight;
        int score = ((dw < 0) ? -dw : dw) * 4;
        if ((int)s_faces[i].italic != (italic ? 1 : 0))
            score += 1000;          /* 斜体不匹配比字重差一点更"贵" */
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

const QLFont *ql_font_family_font(int family_id, int weight, int italic, int pixel_size)
{
    const int idx = best_entry(family_id, weight, italic);
    if (idx < 0 || pixel_size <= 0)
        return NULL;

    QLFTFaceEntry *e = &s_faces[idx];
    for (QLFTCache *c = e->cache; c != NULL; c = c->next) {
        if (c->px == pixel_size)
            return c->font;
    }

    QLFont *font = NULL;
    if (e->mem != NULL) {
        font = ql_font_ft_load_mem(e->mem, e->mem_size, e->face_index, pixel_size);
    } else if (e->path[0] != '\0') {
        font = ql_font_ft_load(e->path, e->face_index, pixel_size);
    }
    if (font == NULL)
        return NULL;

    /* 字面自带的粗/斜体优先；文件没带就按"注册时声明的"记（绘制层据此不再合成） */
    if (e->weight >= 600)
        font->is_bold = 1;
    if (e->italic)
        font->is_italic = 1;
    font->family_id = (uint16_t)(idx + 1);

    QLFTCache *c = (QLFTCache *)ql_mem_calloc(1, sizeof(QLFTCache));
    if (c != NULL) {
        c->px = pixel_size;
        c->font = font;
        c->next = e->cache;
        e->cache = c;
    }
    return font;
}

void ql_font_family_reset(void)
{
    for (int i = 0; i < s_face_count; i++) {
        QLFTCache *c = s_faces[i].cache;
        while (c != NULL) {
            QLFTCache *next = c->next;
            ql_font_destroy(c->font);
            ql_mem_free(c);
            c = next;
        }
    }
    memset(s_faces, 0, sizeof(s_faces));
    s_face_count = 0;
}

#endif /* QL_FEAT_CSS_FONT_STACK */

/* ------------------------------ 给整形后端用 ------------------------------ */

void *ql_font_ft_face(const QLFont *font)
{
    if (font == NULL || font->backend != &s_ft_backend || font->impl == NULL)
        return NULL;
    QLFTFont *ft = (QLFTFont *)font->impl;
    return (void *)ft->face;
}

uint32_t ql_font_ft_glyph_index(const QLFont *font, uint32_t codepoint)
{
    FT_Face face = (FT_Face)ql_font_ft_face(font);
    if (face == NULL)
        return 0;
    return (uint32_t)FT_Get_Char_Index(face, (FT_ULong)codepoint);
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
