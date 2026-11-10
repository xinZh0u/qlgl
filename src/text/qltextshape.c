/**
 * @file    qltextshape.c
 * @brief   文本整形：HarfBuzz（可选，可 dlopen）+ 内建兜底。
 *
 * 为什么用 dlopen 而不是硬链接：本库要在"只有运行库、没有 -dev 头文件"的
 * 机器上也能开整形（用户的机器往往就是这种），并且在**完全没装** HarfBuzz 的
 * 机器上照常工作（自动退回内建路径）。所以这里只用 dlopen + 手写的最小白名单：
 *
 *   - 只用 HarfBuzz 稳定的 ABI：`hb_glyph_info_t` / `hb_glyph_position_t` 的
 *     内存布局从 1.x 起没变过，字段名与顺序见下面的注释；
 *   - 只用 15 个函数，全部是整形必需的；
 *   - 拿不到库/拿不到符号 → 记一次日志，之后永远走内建路径。
 */
#include <qlgl/qlgl.h>

#include <stdlib.h>
#include <string.h>

#if defined(QLGL_ENABLE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

/* ============================== HarfBuzz 白名单 ============================== */

/*
 * 整形只在"有 FreeType"的构建里存在：HarfBuzz 的 hb-ft 需要 FT_Face 才能把
 * 字形下标和我们的字形缓存对上。MCU（没有 FT）走的是内建逐字形路径。
 */
#if QL_FEAT_CSS_TEXT_SHAPE && defined(QLGL_ENABLE_FREETYPE)

/* 手写的 HarfBuzz 结构体（只用公开且 ABI 稳定的前几个字段） */
typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef struct hb_language_impl_t *hb_language_t;

typedef struct {
    uint32_t codepoint;   /* 字形下标 */
    uint32_t mask;
    uint32_t cluster;
    uint32_t var1;
    uint32_t var2;
} ql_hb_glyph_info_t;

typedef struct {
    int32_t x_advance;    /* 26.6 定点 */
    int32_t y_advance;
    int32_t x_offset;
    int32_t y_offset;
    uint32_t var;
} ql_hb_glyph_position_t;

typedef struct {
    uint32_t tag;
    uint32_t value;
    uint32_t start;
    uint32_t end;
} ql_hb_feature_t;

#define QL_HB_TAG(a, b, c, d)                                       \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) |     \
                ((uint32_t)(c) << 8) | (uint32_t)(d)))

typedef struct {
    void *lib;
    hb_font_t *(*ft_font_create_referenced)(FT_Face);
    void (*font_destroy)(hb_font_t *);
    void (*font_set_scale)(hb_font_t *, int, int);
    hb_buffer_t *(*buffer_create)(void);
    void (*buffer_destroy)(hb_buffer_t *);
    void (*buffer_add_utf8)(hb_buffer_t *, const char *, int, unsigned int, int);
    void (*buffer_guess_segment_properties)(hb_buffer_t *);
    void (*buffer_set_direction)(hb_buffer_t *, int);
    /*
     * 注意参数个数：HarfBuzz 是
     *   `hb_shape(font, buffer, features, num_features)`
     * —— 4 个参数，没有第 5 个。多写一个参数在 x86-64/AAPCS 上"碰巧"没事，
     * 但那是未定义行为（这里被"直连路径"的编译检查抓出来过一次）。
     */
    void (*shape)(hb_font_t *, hb_buffer_t *, const ql_hb_feature_t *, unsigned int);
    ql_hb_glyph_info_t *(*buffer_get_glyph_infos)(hb_buffer_t *, unsigned int *);
    ql_hb_glyph_position_t *(*buffer_get_glyph_positions)(hb_buffer_t *, unsigned int *);
} QLHarfBuzz;

static QLHarfBuzz s_hb;
static int s_hb_state;      /* 0 = 没试过，1 = 可用，-1 = 不可用 */

#if defined(QLGL_HAVE_HARFBUZZ)
/* 构建期直连：直接取符号地址填进同一张表（没有 dlopen 依赖） */
#include <hb.h>
#include <hb-ft.h>

/*
 * 直连时把 ABI 假设钉在编译期：我们手写的 ql_hb_* 结构与 HarfBuzz 的真实结构
 * 必须同宽（字段顺序是 HarfBuzz 公开且稳定的 ABI，宽度由这里兜底）。
 */
_Static_assert(sizeof(hb_glyph_info_t) == sizeof(ql_hb_glyph_info_t),
               "hb_glyph_info_t 的布局与 ql_hb_glyph_info_t 不一致");
_Static_assert(sizeof(hb_glyph_position_t) == sizeof(ql_hb_glyph_position_t),
               "hb_glyph_position_t 的布局与 ql_hb_glyph_position_t 不一致");
_Static_assert(sizeof(hb_feature_t) == sizeof(ql_hb_feature_t),
               "hb_feature_t 的布局与 ql_hb_feature_t 不一致");

static int hb_fill_direct(void)
{
    s_hb.ft_font_create_referenced = hb_ft_font_create_referenced;
    s_hb.font_destroy = hb_font_destroy;
    s_hb.font_set_scale = hb_font_set_scale;
    s_hb.buffer_create = hb_buffer_create;
    s_hb.buffer_destroy = hb_buffer_destroy;
    s_hb.buffer_add_utf8 = hb_buffer_add_utf8;
    s_hb.buffer_guess_segment_properties = hb_buffer_guess_segment_properties;
    s_hb.buffer_set_direction =
        (void (*)(hb_buffer_t *, int))hb_buffer_set_direction;
    /* feature / glyph 结构只是"名字不同"，布局由上面的 static_assert 钉住 */
    s_hb.shape = (void (*)(hb_font_t *, hb_buffer_t *, const ql_hb_feature_t *,
                           unsigned int))hb_shape;
    s_hb.buffer_get_glyph_infos =
        (ql_hb_glyph_info_t * (*)(hb_buffer_t *, unsigned int *))
            hb_buffer_get_glyph_infos;
    s_hb.buffer_get_glyph_positions =
        (ql_hb_glyph_position_t * (*)(hb_buffer_t *, unsigned int *))
            hb_buffer_get_glyph_positions;
    QL_LOGI("text-shape: using harfbuzz (direct link)");
    s_hb_state = 1;
    return 1;
}
#endif

static int hb_load(void)
{
    if (s_hb_state != 0)
        return s_hb_state > 0;

    /* 测试/排查用：QL_TEXT_SHAPE=builtin 强制走内建路径 */
    {
        const char *force = getenv("QL_TEXT_SHAPE");
        if (force != NULL && strcmp(force, "builtin") == 0) {
            s_hb_state = -1;
            return 0;
        }
    }

#if defined(QLGL_HAVE_HARFBUZZ)
    return hb_fill_direct();
#else

    void *lib = ql_dlopen("libharfbuzz.so.0");
    if (lib == NULL)
        lib = ql_dlopen("libharfbuzz.so");
    if (lib == NULL) {
        QL_LOGI("text-shape: harfbuzz not found, using built-in shaper");
        s_hb_state = -1;
        return 0;
    }

    s_hb.lib = lib;
    QL_DLSYM(lib, "hb_ft_font_create_referenced", s_hb.ft_font_create_referenced);
    QL_DLSYM(lib, "hb_font_destroy", s_hb.font_destroy);
    QL_DLSYM(lib, "hb_font_set_scale", s_hb.font_set_scale);
    QL_DLSYM(lib, "hb_buffer_create", s_hb.buffer_create);
    QL_DLSYM(lib, "hb_buffer_destroy", s_hb.buffer_destroy);
    QL_DLSYM(lib, "hb_buffer_add_utf8", s_hb.buffer_add_utf8);
    QL_DLSYM(lib, "hb_buffer_guess_segment_properties",
             s_hb.buffer_guess_segment_properties);
    QL_DLSYM(lib, "hb_buffer_set_direction", s_hb.buffer_set_direction);
    QL_DLSYM(lib, "hb_shape", s_hb.shape);
    QL_DLSYM(lib, "hb_buffer_get_glyph_infos", s_hb.buffer_get_glyph_infos);
    QL_DLSYM(lib, "hb_buffer_get_glyph_positions", s_hb.buffer_get_glyph_positions);

    const int ok = (s_hb.ft_font_create_referenced != NULL) &&
                   (s_hb.buffer_create != NULL) && (s_hb.shape != NULL) &&
                   (s_hb.buffer_get_glyph_infos != NULL) &&
                   (s_hb.buffer_get_glyph_positions != NULL) &&
                   (s_hb.font_set_scale != NULL);
    if (!ok) {
        QL_LOGW("text-shape: harfbuzz found but symbols are missing, using built-in");
        ql_dlclose(lib);
        memset(&s_hb, 0, sizeof(s_hb));
        s_hb_state = -1;
        return 0;
    }
    s_hb_state = 1;
    QL_LOGI("text-shape: using harfbuzz (dlopen)");
    return 1;
#endif /* QLGL_HAVE_HARFBUZZ */
}

int ql_text_shape_has_harfbuzz(void)
{
    return hb_load();
}

#else /* 没有整形 / 没有 FreeType */

int ql_text_shape_has_harfbuzz(void)
{
    return 0;
}

#endif /* QL_FEAT_CSS_TEXT_SHAPE && QLGL_ENABLE_FREETYPE */

/* ============================== 内建整形 ============================== */

/* 字间距 + 字间词距（与旧版逐字循环的加法完全一致，保证像素不变） */
static ql_coord_t spacing_after(uint32_t cp, const QLShapeOpt *opt)
{
    ql_coord_t add = opt->letter_spacing;
    if (cp == (uint32_t)' ')
        add += opt->word_spacing;
    return add;
}

static int builtin_shape_raw(const QLFont *font, const char *text, int32_t len,
                             const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                             ql_coord_t *out_width);

/*
 * 内建整形（没有 HarfBuzz 时的兜底）。
 *
 * 双向：HarfBuzz 那条路是"按段整形、段内自带视觉顺序"，内建路径没有这一步，
 * 所以这里先把整行按 UAX#9 子集排成**视觉顺序**，再逐字取字形 —— 效果与
 * 老终端一样（能读对顺序，但没有连写）。
 */
static int builtin_shape(const QLFont *font, const char *text, int32_t len,
                         const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                         ql_coord_t *out_width)
{
    if (opt->par_rtl || ql_text_bidi_has_rtl(text, len)) {
        char vis[QL_BIDI_LINE_MAX];
        const int32_t total = (len < 0) ? (int32_t)strlen(text) : len;
        const int32_t vn = ql_text_bidi_log2vis(text, total, opt->par_rtl, vis,
                                                (int32_t)sizeof(vis));
        if (vn >= 0)
            return builtin_shape_raw(font, vis, vn, opt, out, cap, out_width);
    }
    return builtin_shape_raw(font, text, len, opt, out, cap, out_width);
}

static int builtin_shape_raw(const QLFont *font, const char *text, int32_t len,
                             const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                             ql_coord_t *out_width)
{
    ql_coord_t pen = 0;
    int32_t n = 0;
    int32_t i = 0;

    const QLGlyph *space = (opt->word_spacing != 0 || opt->tab_size != 0)
                               ? ql_font_find_glyph(font, ' ')
                               : NULL;

    while ((len < 0 ? text[i] != '\0' : i < len) && text[i] != '\0') {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;

        ql_coord_t adv = 0;
        const QLGlyph *glyph = NULL;
        const QLFont *use_font = font;

        if (cp == (uint32_t)'\t' && opt->tab_size != 0) {
            adv = ((space != NULL) ? space->adv_w : 0) * (ql_coord_t)opt->tab_size;
        } else {
            /*
             * 逐字回退：先问主字体，再问回退链 —— 回退链由 ql_font_find_glyph
             * 自动处理，所以这里只要拿"谁提供了它"用于位图归属。
             * 注意别把 `font` 本身改掉：它还是后面每个字的查找起点。
             */
            const QLFont *owner = font;
            (void)ql_font_has_glyph(font, cp, &owner);
            glyph = ql_font_find_glyph((owner != NULL) ? owner : font, cp);
            if (glyph != NULL)
                adv = glyph->adv_w;
            if (owner != NULL)
                use_font = owner;      /* 位图要从真正提供字形的字体里取 */
        }

        const ql_coord_t total = adv + spacing_after(cp, opt);
        if (out != NULL && n < cap) {
            out[n].font = use_font;
            out[n].glyph = glyph;
            out[n].cluster = (uint16_t)i;
            out[n].dx = 0;
            out[n].adv = (int16_t)total;
        }
        n++;
        pen += total;
        i += used;
    }

    if (out_width != NULL)
        *out_width = pen;
    return n;
}

/* ============================== HarfBuzz 整形 ============================== */

#if QL_FEAT_CSS_TEXT_SHAPE && defined(QLGL_ENABLE_FREETYPE)

/* 每个动态字体挂一个 hb_font_t（懒建，字体销毁时一起放） */
typedef struct QLHBFont {
    struct QLHBFont *next;
    const QLFont    *font;
    hb_font_t       *hb;
} QLHBFont;

static QLHBFont *s_hb_fonts;

static hb_font_t *hb_for_font(const QLFont *font)
{
    for (QLHBFont *e = s_hb_fonts; e != NULL; e = e->next) {
        if (e->font == font)
            return e->hb;
    }
    FT_Face face = (FT_Face)ql_font_ft_face(font);
    if (face == NULL)
        return NULL;

    hb_font_t *hb = s_hb.ft_font_create_referenced(face);
    if (hb == NULL)
        return NULL;
    /* 缩放到像素单位（26.6），与 QLGlyph 的 adv_w 同一把尺子 */
    s_hb.font_set_scale(hb, (int)font->pixel_size * 64, (int)font->pixel_size * 64);

    QLHBFont *e = (QLHBFont *)ql_mem_calloc(1, sizeof(QLHBFont));
    if (e == NULL) {
        s_hb.font_destroy(hb);
        return NULL;
    }
    e->font = font;
    e->hb = hb;
    e->next = s_hb_fonts;
    s_hb_fonts = e;
    return hb;
}

/* 字体换了（或字体表被重建）就把 hb_font 缓存全丢掉 */
void ql_text_shape_reset(void)
{
    QLHBFont *e = s_hb_fonts;
    while (e != NULL) {
        QLHBFont *next = e->next;
        if (s_hb.font_destroy != NULL && e->hb != NULL)
            s_hb.font_destroy(e->hb);
        ql_mem_free(e);
        e = next;
    }
    s_hb_fonts = NULL;
}

void ql_text_shape_forget(const QLFont *font)
{
    QLHBFont **pp = &s_hb_fonts;
    while (*pp != NULL) {
        QLHBFont *e = *pp;
        if (e->font == font) {
            *pp = e->next;
            if (s_hb.font_destroy != NULL && e->hb != NULL)
                s_hb.font_destroy(e->hb);
            ql_mem_free(e);
            return;
        }
        pp = &e->next;
    }
}

#else

void ql_text_shape_reset(void)
{
}

void ql_text_shape_forget(const QLFont *font)
{
    QL_UNUSED(font);
}

#endif

/* 用 HarfBuzz 整形一段"同一个字体"的文本（内部用） */
static int32_t hb_shape_run(const QLFont *font, const char *text, int32_t from, int32_t to,
                            int run_rtl, const QLShapeOpt *opt, QLShapeGlyph *out,
                            int32_t cap, ql_coord_t *pen_io)
{
#if QL_FEAT_CSS_TEXT_SHAPE && defined(QLGL_ENABLE_FREETYPE)
    hb_font_t *hb = hb_for_font(font);
    if (hb == NULL)
        return -1;

    hb_buffer_t *buf = s_hb.buffer_create();
    if (buf == NULL)
        return -1;
    s_hb.buffer_add_utf8(buf, text + from, to - from, 0, to - from);
    s_hb.buffer_guess_segment_properties(buf);
    /* 方向由调用方按"这一段的强字符"决定：RTL 段要让 HarfBuzz 做连写与视觉排序 */
    if (s_hb.buffer_set_direction != NULL)
        s_hb.buffer_set_direction(buf, run_rtl ? 5 : 4);   /* HB_DIRECTION_RTL / LTR */

    ql_hb_feature_t feats[4];
    unsigned nf = 0;
    if (!opt->ligatures) {
        feats[nf].tag = QL_HB_TAG('l', 'i', 'g', 'a');
        feats[nf].value = 0;
        feats[nf].start = 0;
        feats[nf].end = (uint32_t)-1;
        nf++;
        feats[nf].tag = QL_HB_TAG('c', 'l', 'i', 'g');
        feats[nf].value = 0;
        feats[nf].start = 0;
        feats[nf].end = (uint32_t)-1;
        nf++;
    }
    if (!opt->kerning) {
        feats[nf].tag = QL_HB_TAG('k', 'e', 'r', 'n');
        feats[nf].value = 0;
        feats[nf].start = 0;
        feats[nf].end = (uint32_t)-1;
        nf++;
    }

    s_hb.shape(hb, buf, (nf > 0) ? feats : NULL, nf);

    unsigned gn = 0;
    ql_hb_glyph_info_t *info = s_hb.buffer_get_glyph_infos(buf, &gn);
    ql_hb_glyph_position_t *pos = s_hb.buffer_get_glyph_positions(buf, &gn);
    if (info == NULL || pos == NULL) {
        s_hb.buffer_destroy(buf);
        return -1;
    }

    ql_coord_t pen = (pen_io != NULL) ? *pen_io : 0;
    int32_t n = 0;
    for (unsigned k = 0; k < gn; k++) {
        const uint32_t gid = info[k].codepoint;
        const QLGlyph *glyph = ql_font_find_glyph_id(font, gid);
        /* 笔画顺序/前后文带来的水平偏移（阿拉伯文连写要用） */
        const ql_coord_t dx = (ql_coord_t)((pos[k].x_offset + 32) >> 6);
        const ql_coord_t adv = (ql_coord_t)((pos[k].x_advance + 32) >> 6);

        uint32_t cp = 0;
        const uint32_t cluster = info[k].cluster;
        if (cluster < (uint32_t)(to - from))
            (void)ql_utf8_decode(text + from + cluster, &cp);

        const ql_coord_t total = adv + spacing_after(cp, opt);
        if (out != NULL && n < cap) {
            out[n].font = font;
            out[n].glyph = glyph;
            out[n].cluster = (uint16_t)(from + (int32_t)cluster);
            out[n].dx = (int16_t)dx;
            out[n].adv = (int16_t)total;
        }
        n++;
        pen += total;
    }
    s_hb.buffer_destroy(buf);
    if (pen_io != NULL)
        *pen_io = pen;
    return n;
#else
    (void)font;
    (void)text;
    (void)from;
    (void)to;
    (void)run_rtl;
    (void)opt;
    (void)out;
    (void)cap;
    (void)pen_io;
    return -1;
#endif
}

/* ============================== 入口 ============================== */

/* 强方向分类：0 = 没有强方向（数字/空白/标点），1 = LTR，2 = RTL */
static int strong_dir(uint32_t cp)
{
    if (ql_text_bidi_is_rtl_cp(cp))
        return 2;
    /*
     * 数字一律按 LTR 整形（UAX#9 里 RTL 段中的数字是"二级"：整体保持 LTR 顺序）。
     * 不这么切的话，"مرحبا 12345" 会把 12345 按 RTL 排成 54321。
     */
    if ((cp >= '0' && cp <= '9') || (cp >= 0x0660u && cp <= 0x0669u) ||
        (cp >= 0x06F0u && cp <= 0x06F9u))
        return 1;
    if (cp < 0x20u || cp == (uint32_t)' ' || cp == (uint32_t)'\t')
        return 0;
    return (cp >= 0x590u && cp <= 0x8FFu) ? 0 : 1;
}

/* 一段"同字体 + 同方向"的文本 */
typedef struct {
    int32_t from;
    int32_t to;
    const QLFont *font;
    int rtl;
} ShapeRun;

#define QL_SHAPE_RUNS  64

/* 把一行切成若干 run；返回个数（0 = 切不出来，调用方退回内建） */
static int split_runs(const QLFont *font, const char *text, int32_t total, ShapeRun *runs,
                      int cap)
{
    int n = 0;
    int32_t i = 0;
    while (i < total) {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        if (n >= cap)
            return -1;

        const QLFont *owner = font;
        if (!ql_font_has_glyph(font, cp, &owner) || owner == NULL)
            owner = font;

        int rtl = 0;
        {
            /*
             * 这一段的基准方向：取段里第一个**强**字符的方向；一个强字符都没有
             * （纯数字/空白）时跟随段落方向。
             */
            int32_t k = i;
            int dir = 0;
            while (k < total) {
                uint32_t c2 = 0;
                const int32_t u2 = ql_utf8_decode(text + k, &c2);
                if (u2 <= 0)
                    break;
                const int d2 = strong_dir(c2);
                if (d2 != 0) {
                    dir = d2;
                    break;
                }
                k += u2;
            }
            rtl = (dir == 2) ? 1 : 0;
        }

        int32_t j = i + used;
        while (j < total) {
            uint32_t cp2 = 0;
            const int32_t u2 = ql_utf8_decode(text + j, &cp2);
            if (u2 <= 0)
                break;
            const QLFont *o2 = font;
            if (!ql_font_has_glyph(font, cp2, &o2) || o2 == NULL)
                o2 = font;
            if (o2 != owner)
                break;
            /* 强方向变了就分段（弱字符跟着当前段走） */
            const int d2 = strong_dir(cp2);
            if (d2 != 0 && (d2 == 2) != (rtl != 0))
                break;
            j += u2;
        }

        runs[n].from = i;
        runs[n].to = j;
        runs[n].font = owner;
        runs[n].rtl = rtl;
        n++;
        i = j;
    }
    return n;
}

static int32_t shape_impl(const QLFont *font, const char *text, int32_t len,
                          const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                          ql_coord_t *out_width)
{
    const int32_t total = (len < 0) ? (int32_t)strlen(text) : len;
    if (cap < 0)
        cap = 0;

#if QL_FEAT_CSS_TEXT_SHAPE && defined(QLGL_ENABLE_FREETYPE)
    if (ql_text_shape_has_harfbuzz()) {
        /*
         * 两遍：
         *   1. 先按"字体 + 方向"切段（HarfBuzz 一次只能一个 face 一个方向）；
         *   2. 再按**显示顺序**逐段整形 —— 段落是 RTL 时段落顺序整体反过来，
         *      段内字形顺序由 HarfBuzz 按方向给（已经是视觉顺序）。
         */
        ShapeRun runs[QL_SHAPE_RUNS];
        const int nruns = split_runs(font, text, total, runs, QL_SHAPE_RUNS);
        if (nruns >= 0) {
            ql_coord_t pen = 0;
            int32_t n = 0;
            int failed = 0;
            for (int k = 0; k < nruns; k++) {
                const int idx = opt->par_rtl ? (nruns - 1 - k) : k;
                /* 逐段往后接：out 是同一个缓冲，必须自己推进（踩过：每段都从 0 写） */
                QLShapeGlyph *dst = (out != NULL) ? (out + n) : NULL;
                const int32_t room = (out != NULL) ? (cap - n) : 0;
                if (dst != NULL && room <= 0)
                    dst = NULL;              /* 缓冲满了：只算宽度 */
                const int32_t got =
                    hb_shape_run(runs[idx].font, text, runs[idx].from, runs[idx].to,
                                 runs[idx].rtl, opt, dst, room, &pen);
                if (got < 0) {
                    failed = 1;
                    break;
                }
                n += got;
            }
            if (!failed) {
                if (out_width != NULL)
                    *out_width = pen;
                return n;
            }
            /* 失败：内建路径会从头覆盖 out */
        }
    }
#endif
    return builtin_shape(font, text, len, opt, out, cap, out_width);
}

int32_t ql_text_shape(const QLFont *font, const char *text, int32_t len,
                      const QLShapeOpt *opt, QLShapeGlyph *out, int32_t cap,
                      ql_coord_t *out_width)
{
    static const QLShapeOpt k_default = { 0, 0, 0, 1, 1, 0 };
    if (font == NULL || text == NULL)
        return 0;
    if (opt == NULL)
        opt = &k_default;
    return shape_impl(font, text, len, opt, out, cap, out_width);
}

const char *ql_text_shape_backend(void)
{
    return ql_text_shape_has_harfbuzz() ? "harfbuzz" : "builtin";
}
