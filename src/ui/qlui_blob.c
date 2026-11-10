/**
 * @file    qlui_blob.c
 * @brief   QLUIDoc ↔ .qlui 二进制 blob 的编解码。
 *
 * 为什么 blob 的"加载"这么便宜：
 *   QLUIDoc 里所有内部引用都是 uint16 下标，唯一的指针是 doc 自己的 6 个基址
 *   字段（nodes/styles/text_pool/text_offsets/font_sizes/name）。所以解码 =
 *   "校验 + 设 6 个指针"，没有任何递归重定位。
 *
 * 刻意不做的事：
 *   - 不做版本迁移（version 不匹配就拒绝，明确失败胜过静默半正确）；
 *   - 不做部分修复（少一个字节就整份失败）；
 *   - 不做跨架构兼容（结构体布局随 ABI 变，头里带 struct_size 逐项校验）。
 *
 * 本文件是独立编译单元，core 里没有任何静态引用指向它，
 * 因此 MCU 侧只要不把 .c 加进构建，就是零字节成本。
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

/* ------------------------------ 小工具 ------------------------------ */

#define QL_UI_ALIGN8(x)   (((x) + 7u) & ~(size_t)7u)

/* FNV-1a 32：blob 的校验和，编解码两侧必须逐位一致 */
static uint32_t fnv1a(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static char s_error[192];

static void set_error_str(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text != NULL ? text : "?");
}

const char *ql_ui_doc_last_error(void)
{
    return s_error;
}

/* 文本池的字节数：最后一个字符串的偏移 + 它自己的长度 + '\0' */
size_t ql_ui_doc_text_pool_size(const QLUIDoc *doc)
{
    if (doc == NULL || doc->text_pool == NULL || doc->text_offsets == NULL ||
        doc->text_count == 0)
        return 0;

    uint16_t last = doc->text_offsets[doc->text_count - 1];
    const char *str = doc->text_pool + last;
    return (size_t)last + strlen(str) + 1u;
}

/* ------------------------------ 布局计算 ------------------------------ */

typedef struct {
    size_t nodes;
    size_t styles;
    size_t text_pool;
    size_t text_offsets;
    size_t font_sizes;
    size_t anims;
    size_t keyframes;
    size_t images;
    size_t image_data;
    size_t areas;
    size_t area_coords;
    size_t media;
    size_t node_media;
    size_t media_styles;
    size_t calc_ops;
    size_t grids;
    size_t grid_tracks;
    size_t grid_areas;
    size_t name;
    size_t total;
} QLUIBlobLayout;

/*
 * 统一算偏移：encode 与 load 共用，保证两边永远一致。
 * pool_size 由调用方给出（编码时统计；解码时由段偏移相减得到）。
 */
static void compute_layout(uint16_t node_count, uint16_t style_count,
                           uint16_t text_count, uint16_t font_size_count,
                           uint16_t anim_count, uint16_t keyframe_count,
                           uint16_t image_count, size_t image_data_size,
                           uint16_t area_count, uint16_t area_coord_count,
                           uint16_t media_count, uint32_t media_pool_count,
                           uint16_t calc_op_count, uint16_t grid_count,
                           uint16_t grid_track_count, uint16_t grid_area_count,
                           size_t pool_size, size_t name_len, QLUIBlobLayout *out)
{
    memset(out, 0, sizeof(*out));
    size_t off = sizeof(QLUIDocBlobHeader);

    out->nodes = off;
    off = QL_UI_ALIGN8(off + (size_t)node_count * sizeof(QLUINode));

    out->styles = off;
    off = QL_UI_ALIGN8(off + (size_t)style_count * sizeof(QLUIStyle));

    out->text_pool = off;
    off = QL_UI_ALIGN8(off + pool_size);

    out->text_offsets = off;
    off = QL_UI_ALIGN8(off + (size_t)text_count * sizeof(uint16_t));

    out->font_sizes = off;
    off = QL_UI_ALIGN8(off + (size_t)font_size_count * sizeof(uint16_t));

    out->anims = off;
    off = QL_UI_ALIGN8(off + (size_t)anim_count * sizeof(QLUIAnimation));

    out->keyframes = off;
    off = QL_UI_ALIGN8(off + (size_t)keyframe_count * sizeof(QLUIKeyframe));

    out->images = off;
    off = QL_UI_ALIGN8(off + (size_t)image_count * sizeof(QLUIImageSrc));

    out->image_data = off;
    off = QL_UI_ALIGN8(off + image_data_size);

    out->areas = off;
    off = QL_UI_ALIGN8(off + (size_t)area_count * sizeof(QLUIArea));

    out->area_coords = off;
    off = QL_UI_ALIGN8(off + (size_t)area_coord_count * sizeof(int16_t));

    /*
     * `@media` 三段只在有断点时才出现（media_count == 0 时长度为 0），
     * 这样"没写 @media 的 doc" 的 blob 与 v20 一样紧凑。
     */
    out->media = off;
    off = QL_UI_ALIGN8(off + (size_t)media_count * sizeof(QLUIMedia));

    out->node_media = off;
    if (media_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)node_count * sizeof(uint16_t));

    out->media_styles = off;
    if (media_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)media_pool_count * sizeof(uint16_t));

    /* calc() 池同样只在用到时才占空间 */
    out->calc_ops = off;
    if (calc_op_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)calc_op_count * sizeof(QLUICalcOp));

    /* Grid（B6）：三段池，没有 grid 的文档长度为 0 */
    out->grids = off;
    if (grid_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)grid_count * sizeof(QLUIGrid));

    out->grid_tracks = off;
    if (grid_track_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)grid_track_count * sizeof(QLUIGridTrack));

    out->grid_areas = off;
    if (grid_area_count > 0)
        off = QL_UI_ALIGN8(off + (size_t)grid_area_count * sizeof(QLUIGridArea));

    out->name = off;
    off = QL_UI_ALIGN8(off + name_len);

    out->total = off;
}

/* ------------------------------ 编码 ------------------------------ */

size_t ql_ui_doc_encode(const QLUIDoc *doc, void *buf, size_t cap)
{
    if (doc == NULL)
        return 0;
    if (doc->node_count > 0 && doc->nodes == NULL)
        return 0;
    if (doc->style_count > 0 && doc->styles == NULL)
        return 0;

    if (doc->anim_count > 0 && doc->anims == NULL)
        return 0;
    if (doc->keyframe_count > 0 && doc->keyframes == NULL)
        return 0;

    size_t pool_size = ql_ui_doc_text_pool_size(doc);
    size_t name_len = (doc->name != NULL) ? strlen(doc->name) : 0u;

    if (doc->image_count > 0 && (doc->images == NULL || doc->image_data == NULL))
        return 0;
    const size_t image_data_size = (doc->image_count > 0) ? doc->image_data_size : 0u;

    if (doc->area_count > 0 && (doc->areas == NULL || doc->area_coords == NULL))
        return 0;

    const uint16_t media_count = (doc->node_media != NULL) ? doc->media_count : 0;
    uint32_t media_pool_count = 0;
    if (media_count > 0) {
        const uint32_t need = (uint32_t)doc->node_count * (uint32_t)media_count * 4u;
        /* 池的真实长度 = 最后一个有变体的节点的偏移 + 它的块大小 */
        for (int i = 0; i < (int)doc->node_count; i++)
            if (doc->node_media[i] != 0xFFFFu) {
                const uint32_t end = (uint32_t)doc->node_media[i] + need / doc->node_count;
                if (end > media_pool_count)
                    media_pool_count = end;
            }
    }

    const uint16_t calc_op_count = (doc->calc_ops != NULL) ? doc->calc_op_count : 0;

    if (doc->grid_count > 0 &&
        (doc->grids == NULL || (doc->grid_track_count > 0 && doc->grid_tracks == NULL) ||
         (doc->grid_area_count > 0 && doc->grid_areas == NULL)))
        return 0;
    const uint16_t grid_count = doc->grid_count;
    const uint16_t grid_track_count = doc->grid_track_count;
    const uint16_t grid_area_count = doc->grid_area_count;

    QLUIBlobLayout lay;
    compute_layout(doc->node_count, doc->style_count, doc->text_count,
                   doc->font_size_count, doc->anim_count, doc->keyframe_count,
                   doc->image_count, image_data_size, doc->area_count,
                   doc->area_coord_count, media_count, media_pool_count,
                   calc_op_count, grid_count, grid_track_count, grid_area_count,
                   pool_size, name_len, &lay);

    if (buf == NULL || cap < lay.total)
        return lay.total;

    uint8_t *base = (uint8_t *)buf;
    memset(base, 0, lay.total);      /* 补齐段间空隙，保证可逐字节比对 */

    QLUIDocBlobHeader head;
    memset(&head, 0, sizeof(head));
    head.magic = QL_UI_BLOB_MAGIC;
    head.version = QL_UI_BLOB_VERSION;
    head.header_size = (uint16_t)sizeof(QLUIDocBlobHeader);
    head.node_count = doc->node_count;
    head.style_count = doc->style_count;
    head.text_count = doc->text_count;
    head.font_size_count = doc->font_size_count;
    head.anim_count = doc->anim_count;
    head.keyframe_count = doc->keyframe_count;
    head.image_count = doc->image_count;
    head.name_len = (uint16_t)name_len;
    head.node_struct_size = (uint16_t)sizeof(QLUINode);
    head.style_struct_size = (uint16_t)sizeof(QLUIStyle);
    head.anim_struct_size = (uint16_t)sizeof(QLUIAnimation);
    head.keyframe_struct_size = (uint16_t)sizeof(QLUIKeyframe);
    head.total_size = (uint32_t)lay.total;
    head.off_nodes = (uint32_t)lay.nodes;
    head.off_styles = (uint32_t)lay.styles;
    head.off_text_pool = (uint32_t)lay.text_pool;
    head.off_text_offsets = (uint32_t)lay.text_offsets;
    head.off_font_sizes = (uint32_t)lay.font_sizes;
    head.off_anims = (uint32_t)lay.anims;
    head.off_keyframes = (uint32_t)lay.keyframes;
    head.off_images = (uint32_t)lay.images;
    head.off_image_data = (uint32_t)lay.image_data;
    head.image_data_size = (uint32_t)image_data_size;
    head.off_name = (uint32_t)lay.name;
    head.area_count = doc->area_count;
    head.area_coord_count = doc->area_coord_count;
    head.off_areas = (uint32_t)lay.areas;
    head.off_area_coords = (uint32_t)lay.area_coords;
    head.media_count = media_count;
    head.off_media = (uint32_t)lay.media;
    head.off_node_media = (uint32_t)lay.node_media;
    head.off_media_styles = (uint32_t)lay.media_styles;
    head.media_pool_count = media_pool_count;
    head.calc_op_count = calc_op_count;
    head.off_calc_ops = (uint32_t)lay.calc_ops;
    head.grid_count = grid_count;
    head.grid_track_count = grid_track_count;
    head.grid_area_count = grid_area_count;
    head.off_grids = (uint32_t)lay.grids;
    head.off_grid_tracks = (uint32_t)lay.grid_tracks;
    head.off_grid_areas = (uint32_t)lay.grid_areas;

    if (doc->node_count > 0)
        memcpy(base + lay.nodes, doc->nodes,
               (size_t)doc->node_count * sizeof(QLUINode));
    if (doc->style_count > 0)
        memcpy(base + lay.styles, doc->styles,
               (size_t)doc->style_count * sizeof(QLUIStyle));
    if (pool_size > 0)
        memcpy(base + lay.text_pool, doc->text_pool, pool_size);
    if (doc->text_count > 0)
        memcpy(base + lay.text_offsets, doc->text_offsets,
               (size_t)doc->text_count * sizeof(uint16_t));
    if (doc->font_size_count > 0)
        memcpy(base + lay.font_sizes, doc->font_sizes,
               (size_t)doc->font_size_count * sizeof(uint16_t));
    if (doc->anim_count > 0)
        memcpy(base + lay.anims, doc->anims,
               (size_t)doc->anim_count * sizeof(QLUIAnimation));
    if (doc->keyframe_count > 0)
        memcpy(base + lay.keyframes, doc->keyframes,
               (size_t)doc->keyframe_count * sizeof(QLUIKeyframe));
    if (doc->image_count > 0)
        memcpy(base + lay.images, doc->images,
               (size_t)doc->image_count * sizeof(QLUIImageSrc));
    if (image_data_size > 0)
        memcpy(base + lay.image_data, doc->image_data, image_data_size);
    if (doc->area_count > 0)
        memcpy(base + lay.areas, doc->areas, (size_t)doc->area_count * sizeof(QLUIArea));
    if (doc->area_coord_count > 0)
        memcpy(base + lay.area_coords, doc->area_coords,
               (size_t)doc->area_coord_count * sizeof(int16_t));
    if (media_count > 0) {
        if (doc->media != NULL)
            memcpy(base + lay.media, doc->media,
                   (size_t)media_count * sizeof(QLUIMedia));
        if (doc->node_media != NULL)
            memcpy(base + lay.node_media, doc->node_media,
                   (size_t)doc->node_count * sizeof(uint16_t));
        if (doc->media_styles != NULL && media_pool_count > 0)
            memcpy(base + lay.media_styles, doc->media_styles,
                   (size_t)media_pool_count * sizeof(uint16_t));
    }
    if (calc_op_count > 0)
        memcpy(base + lay.calc_ops, doc->calc_ops,
               (size_t)calc_op_count * sizeof(QLUICalcOp));
    if (grid_count > 0)
        memcpy(base + lay.grids, doc->grids, (size_t)grid_count * sizeof(QLUIGrid));
    if (grid_track_count > 0)
        memcpy(base + lay.grid_tracks, doc->grid_tracks,
               (size_t)grid_track_count * sizeof(QLUIGridTrack));
    if (grid_area_count > 0)
        memcpy(base + lay.grid_areas, doc->grid_areas,
               (size_t)grid_area_count * sizeof(QLUIGridArea));
    if (name_len > 0)
        memcpy(base + lay.name, doc->name, name_len);

    memcpy(base, &head, sizeof(head));

    /* 校验和覆盖 header 之后的全部字节（此时 head 已落盘，可直接算） */
    head.checksum = fnv1a(base + sizeof(head), lay.total - sizeof(head));
    memcpy(base, &head, sizeof(head));

    return lay.total;
}

ql_err_t ql_ui_doc_save(const QLUIDoc *doc, const char *path)
{
    if (path == NULL)
        return QL_ERR_INVAL;

    size_t size = ql_ui_doc_encode(doc, NULL, 0);
    if (size == 0)
        return QL_ERR_INVAL;

    uint8_t *buf = (uint8_t *)ql_mem_alloc(size);
    if (buf == NULL)
        return QL_ERR_NOMEM;

    ql_err_t err = QL_OK;
    if (ql_ui_doc_encode(doc, buf, size) != size) {
        err = QL_ERR_INVAL;
    } else {
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            err = QL_ERR_INVAL;
        } else {
            if (fwrite(buf, 1, size, f) != size)
                err = QL_ERR_INVAL;
            fclose(f);
        }
    }

    ql_mem_free(buf);
    return err;
}

/* ------------------------------ 解码 ------------------------------ */

int ql_ui_doc_is_owned(const QLUIDoc *doc)
{
    return doc != NULL && doc->arena != NULL;
}

void ql_ui_doc_free(QLUIDoc *doc)
{
    if (doc == NULL)
        return;
    /* 静态 doc（.rodata / 调用方自己管）不进 free 路径 —— 判据只有 arena */
    if (doc->arena == NULL)
        return;
    void *arena = doc->arena;
    ql_mem_free(arena);
    ql_mem_free(doc);
}

QLUIDoc *ql_ui_doc_load_mem(const void *data, size_t size)
{
    s_error[0] = '\0';

    if (data == NULL || size < sizeof(QLUIDocBlobHeader)) {
        set_error_str("blob too small");
        return NULL;
    }

    QLUIDocBlobHeader head;
    memcpy(&head, data, sizeof(head));

    if (head.magic != QL_UI_BLOB_MAGIC) {
        set_error_str("bad magic (not a .qlui blob)");
        return NULL;
    }
    if (head.version != QL_UI_BLOB_VERSION) {
        set_error_str("version mismatch: blob built by a different qlgl");
        return NULL;
    }
    if (head.header_size != sizeof(QLUIDocBlobHeader)) {
        set_error_str("header size mismatch");
        return NULL;
    }
    if (head.node_struct_size != sizeof(QLUINode)) {
        set_error_str("QLUINode layout mismatch (blob built on another ABI)");
        return NULL;
    }
    if (head.style_struct_size != sizeof(QLUIStyle)) {
        set_error_str("QLUIStyle layout mismatch (blob built on another ABI)");
        return NULL;
    }
    if (head.anim_struct_size != sizeof(QLUIAnimation)) {
        set_error_str("QLUIAnimation layout mismatch (blob built on another ABI)");
        return NULL;
    }
    if (head.keyframe_struct_size != sizeof(QLUIKeyframe)) {
        set_error_str("QLUIKeyframe layout mismatch (blob built on another ABI)");
        return NULL;
    }
    if (head.total_size != size) {
        set_error_str("size mismatch (file truncated or corrupted)");
        return NULL;
    }
    if (head.total_size < head.header_size) {
        set_error_str("size field is inconsistent");
        return NULL;
    }

    const uint8_t *base = (const uint8_t *)data;
    if (fnv1a(base + head.header_size, head.total_size - head.header_size) !=
        head.checksum) {
        set_error_str("checksum mismatch (file corrupted)");
        return NULL;
    }

    /* 容量上限：静态 doc 是编译器保证的常量，blob 是外来数据，必须设界 */
    if (head.node_count > QL_CFG_UI_MAX_NODES) {
        set_error_str("too many nodes (see QL_CFG_UI_MAX_NODES)");
        return NULL;
    }
    if (head.style_count > QL_CFG_UI_MAX_STYLES) {
        set_error_str("too many styles (see QL_CFG_UI_MAX_STYLES)");
        return NULL;
    }
    if (head.font_size_count > QL_CFG_UI_MAX_FONT_SIZES) {
        set_error_str("too many font sizes (see QL_CFG_UI_MAX_FONT_SIZES)");
        return NULL;
    }
    if (head.anim_count > QL_CFG_UI_MAX_ANIMS) {
        set_error_str("too many @keyframes blocks (see QL_CFG_UI_MAX_ANIMS)");
        return NULL;
    }
    if (head.keyframe_count > QL_CFG_UI_MAX_KEYFRAMES) {
        set_error_str("too many keyframes (see QL_CFG_UI_MAX_KEYFRAMES)");
        return NULL;
    }

    /* 段偏移必须与"由各段计数推出的布局"完全一致——挡住手工构造的越界 blob */
    size_t pool_size = 0;
    if (head.off_text_offsets >= head.off_text_pool)
        pool_size = (size_t)(head.off_text_offsets - head.off_text_pool);

    if (pool_size > QL_CFG_UI_MAX_TEXT_BYTES) {
        set_error_str("text pool too large (see QL_CFG_UI_MAX_TEXT_BYTES)");
        return NULL;
    }

    if (head.image_count > QL_CFG_UI_IMAGE_MAX) {
        set_error_str("too many images (see QL_CFG_UI_IMAGE_MAX)");
        return NULL;
    }
    if (head.image_data_size > QL_CFG_IMAGE_MAX_BYTES) {
        set_error_str("image data too large (see QL_CFG_IMAGE_MAX_BYTES)");
        return NULL;
    }
    if (head.area_count > QL_CFG_UI_AREA_MAX) {
        set_error_str("too many <area> hotspots (see QL_CFG_UI_AREA_MAX)");
        return NULL;
    }
    if (head.area_coord_count > (uint32_t)QL_CFG_UI_AREA_MAX * 128u) {
        set_error_str("<area coords> too long");
        return NULL;
    }

    if (head.media_count > QL_CFG_UI_MAX_MEDIA) {
        set_error_str("too many @media ranges (see QL_CFG_UI_MAX_MEDIA)");
        return NULL;
    }
    if (head.media_count > 0 && head.media_pool_count >
                                    (uint32_t)head.node_count * head.media_count * 4u) {
        set_error_str("media style pool too large");
        return NULL;
    }
    if (head.calc_op_count > QL_CFG_UI_MAX_CALC_OPS) {
        set_error_str("too many calc() instructions (see QL_CFG_UI_MAX_CALC_OPS)");
        return NULL;
    }
    if (head.grid_count > QL_CFG_UI_MAX_STYLES ||
        head.grid_track_count > QL_CFG_UI_GRID_MAX_TRACKS_TOTAL ||
        head.grid_area_count > QL_CFG_UI_GRID_MAX_AREAS_TOTAL) {
        set_error_str("grid pools too large");
        return NULL;
    }

    QLUIBlobLayout lay;
    compute_layout(head.node_count, head.style_count, head.text_count,
                   head.font_size_count, head.anim_count, head.keyframe_count,
                   head.image_count, head.image_data_size, head.area_count,
                   head.area_coord_count, head.media_count, head.media_pool_count,
                   head.calc_op_count, head.grid_count, head.grid_track_count,
                   head.grid_area_count, pool_size, head.name_len, &lay);

    if (lay.total != head.total_size ||
        head.off_nodes != lay.nodes || head.off_styles != lay.styles ||
        head.off_text_pool != lay.text_pool ||
        head.off_text_offsets != lay.text_offsets ||
        head.off_font_sizes != lay.font_sizes ||
        head.off_anims != lay.anims || head.off_keyframes != lay.keyframes ||
        head.off_images != lay.images || head.off_image_data != lay.image_data ||
        head.off_areas != lay.areas || head.off_area_coords != lay.area_coords ||
        head.off_media != lay.media || head.off_node_media != lay.node_media ||
        head.off_media_styles != lay.media_styles ||
        head.off_calc_ops != lay.calc_ops ||
        head.off_grids != lay.grids || head.off_grid_tracks != lay.grid_tracks ||
        head.off_grid_areas != lay.grid_areas || head.off_name != lay.name) {
        set_error_str("section offsets inconsistent with counts");
        return NULL;
    }

    /* arena 里放整份数据，另一次分配放 doc 本体，各自一次释放 */
    void *arena = ql_mem_alloc(lay.total);
    if (arena == NULL) {
        set_error_str("out of memory");
        return NULL;
    }
    memcpy(arena, data, lay.total);

    QLUIDoc *doc = (QLUIDoc *)ql_mem_calloc(1, sizeof(QLUIDoc));
    if (doc == NULL) {
        ql_mem_free(arena);
        set_error_str("out of memory");
        return NULL;
    }

    uint8_t *p = (uint8_t *)arena;
    doc->node_count = head.node_count;
    doc->style_count = head.style_count;
    doc->text_count = head.text_count;
    doc->font_size_count = head.font_size_count;
    doc->anim_count = head.anim_count;
    doc->keyframe_count = head.keyframe_count;
    doc->image_count = head.image_count;
    doc->image_data_size = head.image_data_size;
    doc->nodes = (head.node_count > 0) ? (const QLUINode *)(p + lay.nodes) : NULL;
    doc->styles = (head.style_count > 0) ? (const QLUIStyle *)(p + lay.styles) : NULL;
    doc->text_pool = (pool_size > 0) ? (const char *)(p + lay.text_pool) : NULL;
    doc->text_offsets = (head.text_count > 0)
                            ? (const uint16_t *)(p + lay.text_offsets)
                            : NULL;
    doc->font_sizes = (head.font_size_count > 0)
                          ? (const uint16_t *)(p + lay.font_sizes)
                          : NULL;
    doc->anims = (head.anim_count > 0)
                     ? (const QLUIAnimation *)(p + lay.anims)
                     : NULL;
    doc->keyframes = (head.keyframe_count > 0)
                         ? (const QLUIKeyframe *)(p + lay.keyframes)
                         : NULL;
    doc->images = (head.image_count > 0) ? (const QLUIImageSrc *)(p + lay.images) : NULL;
    doc->image_data = (head.image_data_size > 0) ? (const uint8_t *)(p + lay.image_data)
                                                 : NULL;
    doc->area_count = head.area_count;
    doc->area_coord_count = head.area_coord_count;
    doc->areas = (head.area_count > 0) ? (const QLUIArea *)(p + lay.areas) : NULL;
    doc->area_coords = (head.area_coord_count > 0)
                           ? (const int16_t *)(p + lay.area_coords)
                           : NULL;
    doc->name = (head.name_len > 0) ? (const char *)(p + lay.name) : "";
    doc->media_count = head.media_count;
    doc->media = (head.media_count > 0) ? (const QLUIMedia *)(p + lay.media) : NULL;
    doc->node_media =
        (head.media_count > 0) ? (const uint16_t *)(p + lay.node_media) : NULL;
    doc->media_styles =
        (head.media_count > 0) ? (const uint16_t *)(p + lay.media_styles) : NULL;
    doc->grid_count = head.grid_count;
    doc->grid_track_count = head.grid_track_count;
    doc->grid_area_count = head.grid_area_count;
    doc->grids = (head.grid_count > 0)
                     ? (const QLUIGrid *)(const void *)(p + lay.grids)
                     : NULL;
    doc->grid_tracks = (head.grid_track_count > 0)
                           ? (const QLUIGridTrack *)(const void *)(p + lay.grid_tracks)
                           : NULL;
    doc->grid_areas = (head.grid_area_count > 0)
                          ? (const QLUIGridArea *)(const void *)(p + lay.grid_areas)
                          : NULL;
    doc->calc_op_count = head.calc_op_count;
    doc->calc_ops =
        (head.calc_op_count > 0) ? (const QLUICalcOp *)(p + lay.calc_ops) : NULL;

    /* 热区：img 必须真的是 <img>，坐标区间必须落在 coords 段内 */
    for (uint16_t i = 0; i < doc->area_count; i++) {
        const QLUIArea *a = &doc->areas[i];
        if (a->img >= doc->node_count ||
            doc->nodes[a->img].tag != (uint16_t)QL_UI_TAG_IMG ||
            (uint32_t)a->coord_off + (uint32_t)a->ncoord > doc->area_coord_count) {
            set_error_str("<area> entry out of range or not bound to <img>");
            ql_mem_free(doc);
            ql_mem_free(arena);
            return NULL;
        }
    }

    doc->arena = arena;

    return doc;
}

/* 读取上限：防止误把巨大文件当 .qlui 读进内存 */
#define QL_UI_BLOB_MAX_SIZE   (16u * 1024u * 1024u)

QLUIDoc *ql_ui_doc_load(const char *path)
{
    s_error[0] = '\0';

    if (path == NULL) {
        set_error_str("path is NULL");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_error_str("cannot open file");
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_error_str("cannot seek file");
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || (unsigned long)size > QL_UI_BLOB_MAX_SIZE ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_error_str("file size out of range");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)ql_mem_alloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        set_error_str("out of memory");
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    QLUIDoc *doc = NULL;
    if (got != (size_t)size)
        set_error_str("short read");
    else
        doc = ql_ui_doc_load_mem(buf, (size_t)size);

    ql_mem_free(buf);
    return doc;
}
