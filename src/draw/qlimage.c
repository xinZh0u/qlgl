/**
 * @file    qlimage.c
 * @brief   位图校验与解码入口。
 *
 * RAW 直通（构建期已解好的像素）、BMP、QOI、PNG。每种编码一个 `QL_FEAT_IMG_*`
 * 开关，裁掉后一律 `QL_ERR_UNSUPPORTED` —— **明确失败**，不猜、不降级成空白。
 * PNG 复用同一份 DEFLATE（`qlinflate.c`，`QL_FEAT_IMG_INFLATE`）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "qlinflate.h"

uint32_t ql_image_format_bpp(uint8_t format)
{
    switch (format) {
    case QL_IMAGE_RGB565:
        return 2u;
    case QL_IMAGE_RGB888:
        return 3u;
    case QL_IMAGE_RGBA8888:
        return 4u;
    case QL_IMAGE_A8:
        return 1u;
    default:
        return 0u;
    }
}

int ql_image_validate(const QLImage *img)
{
    if (img == NULL || img->px == NULL)
        return 0;
    if (img->w == 0 || img->h == 0)
        return 0;

    const uint32_t bpp = ql_image_format_bpp(img->format);
    if (bpp == 0)
        return 0;

    /*
     * 像素数上界：**先除后乘**。`w * h * bpp` 在 32 位下会溢出，
     * 那正是这类解码器最经典的漏洞（算出个小值，然后逐像素写越界）。
     */
    if ((uint32_t)img->w > QL_CFG_IMAGE_MAX_PIXELS / (uint32_t)img->h)
        return 0;

    /* 每行至少要装得下 w 个像素，且 stride 是 uint16 */
    const uint64_t min_stride = (uint64_t)img->w * bpp;
    if (min_stride > 0xFFFFu)
        return 0;
    if ((uint32_t)img->stride < (uint32_t)min_stride)
        return 0;

    if ((uint64_t)img->stride * img->h != (uint64_t)img->len)
        return 0;

    return 1;
}

/* ------------------------------ 输出像素写入 ------------------------------ */

typedef struct {
    uint8_t *px;
    uint16_t w, h, stride;
    uint8_t  format;
} Dst;

/* 按输出格式写一个像素（RGBA 输入，非预乘） */
static void dst_put(const Dst *d, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b,
                    uint8_t a)
{
    if (x >= d->w || y >= d->h)
        return;
    uint8_t *p = d->px + (uint32_t)y * d->stride;
    switch (d->format) {
    case QL_IMAGE_RGB565: {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        memcpy(p + (uint32_t)x * 2u, &v, 2);
        break;
    }
    case QL_IMAGE_RGB888:
        p[x * 3 + 0] = r;
        p[x * 3 + 1] = g;
        p[x * 3 + 2] = b;
        break;
    case QL_IMAGE_RGBA8888:
        p[x * 4 + 0] = r;
        p[x * 4 + 1] = g;
        p[x * 4 + 2] = b;
        p[x * 4 + 3] = a;
        break;
    default:
        p[x] = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u + (uint32_t)b * 29u) >> 8);
        break;
    }
}

/* 分配输出缓冲；尺寸上界检查全在这里做（先除后乘） */
static uint8_t *dst_alloc(Dst *d, uint16_t w, uint16_t h, uint8_t format)
{
    const uint32_t bpp = ql_image_format_bpp(format);
    if (bpp == 0 || w == 0 || h == 0)
        return NULL;
    if ((uint32_t)w > QL_CFG_IMAGE_MAX_PIXELS / (uint32_t)h)
        return NULL;
    const uint64_t stride = (uint64_t)w * bpp;
    if (stride > 0xFFFFu)
        return NULL;
    const uint64_t len = stride * h;
    if (len > QL_CFG_IMAGE_MAX_BYTES * 8u)   /* 解码后允许比编码大，但仍要有界 */
        return NULL;

    uint8_t *px = (uint8_t *)ql_mem_calloc(1, (size_t)len);
    if (px == NULL)
        return NULL;
    d->px = px;
    d->w = w;
    d->h = h;
    d->stride = (uint16_t)stride;
    d->format = format;
    return px;
}

/* ------------------------------ BMP ------------------------------ */

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* PNG 全是大端（BMP/QOI 是小端）—— 这一点写错的话表现是"长度像天文数字" */
static uint32_t rd_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t rd_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/*
 * BMP：只做 UI 素材常见的那些 —— 24/32 位真彩、8 位调色板、BITFIELDS（565/888）。
 * **不解** RLE4/RLE8（压缩位图在 UI 素材里基本见不到，解它不如明确报错）。
 */
static int decode_bmp(const QLImageDecodeReq *req, QLImage *out, uint8_t outf)
{
    const uint8_t *d = req->data;
    const size_t n = req->len;
    if (n < 26u || d[0] != 'B' || d[1] != 'M')
        return QL_ERR_INVAL;

    const uint32_t data_off = rd_u32le(d + 10);
    const uint32_t hdr_size = rd_u32le(d + 14);
    if (n < 14u + hdr_size || hdr_size < 12u)
        return QL_ERR_INVAL;

    int32_t w = 0, h = 0;
    uint16_t bpp = 0;
    uint32_t compression = 0;
    uint32_t palette_used = 0;
    uint32_t palette_off = 14u + 11u;   /* BITMAPINFOHEADER 里调色板紧跟着 */
    if (hdr_size >= 40u) {
        w = (int32_t)rd_u32le(d + 18);
        h = (int32_t)rd_u32le(d + 22);
        bpp = rd_u16le(d + 28);
        compression = rd_u32le(d + 30);
        palette_used = rd_u32le(d + 46);
        palette_off = 14u + 40u;
    } else {
        /* BITMAPCOREHEADER：宽度高度是 uint16 */
        w = (int32_t)rd_u16le(d + 18);
        h = (int32_t)rd_u16le(d + 20);
        bpp = rd_u16le(d + 24);
        palette_off = 14u + 12u;
    }

    const int top_down = (h < 0);
    if (top_down)
        h = -h;
    if (w <= 0 || h <= 0)
        return QL_ERR_INVAL;
    if (w > 0xFFFF || h > 0xFFFF)
        return QL_ERR_INVAL;
    if (compression != 0u && compression != 3u)
        return QL_ERR_UNSUPPORTED;      /* RLE 不做 */
    if (bpp != 8u && bpp != 24u && bpp != 32u)
        return QL_ERR_UNSUPPORTED;

    Dst dst;
    memset(&dst, 0, sizeof(dst));
    if (dst_alloc(&dst, (uint16_t)w, (uint16_t)h, outf) == NULL)
        return QL_ERR_INVAL;

    const uint32_t row_bytes = (((uint32_t)w * bpp + 31u) / 32u) * 4u;
    if ((uint64_t)data_off + (uint64_t)row_bytes * (uint32_t)h > (uint64_t)n) {
        ql_mem_free(dst.px);
        return QL_ERR_INVAL;            /* 文件被截断：不猜 */
    }

    /* BITFIELDS 的通道掩码（compression==3） */
    uint32_t mask_r = 0, mask_g = 0, mask_b = 0;
    if (compression == 3u) {
        const uint32_t fo = 14u + 40u;
        if (n < fo + 12u) {
            ql_mem_free(dst.px);
            return QL_ERR_INVAL;
        }
        mask_r = rd_u32le(d + fo);
        mask_g = rd_u32le(d + fo + 4);
        mask_b = rd_u32le(d + fo + 8);
        palette_off = fo + 12u;
    }

    for (int32_t y = 0; y < h; y++) {
        const int32_t src_y = top_down ? y : (h - 1 - y);
        const uint8_t *row = d + data_off + (uint64_t)src_y * row_bytes;
        for (int32_t x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 8u) {
                const uint8_t idx = row[x];
                const uint32_t cnt = (palette_used > 0) ? palette_used : 256u;
                if (idx >= cnt) {
                    ql_mem_free(dst.px);
                    return QL_ERR_INVAL;
                }
                const uint32_t po = palette_off + (uint32_t)idx * 4u;   /* B,G,R,0 */
                if (n < po + 3u) {
                    ql_mem_free(dst.px);
                    return QL_ERR_INVAL;
                }
                b = d[po];
                g = d[po + 1];
                r = d[po + 2];
            } else if (bpp == 24u) {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else {   /* 32 位 */
                const uint32_t px = rd_u32le(row + x * 4);
                if (compression == 3u) {
                    r = (uint8_t)((px & mask_r) >> 16);
                    g = (uint8_t)((px & mask_g) >> 8);
                    b = (uint8_t)(px & mask_b);
                } else {
                    b = row[x * 4 + 0];
                    g = row[x * 4 + 1];
                    r = row[x * 4 + 2];
                    a = row[x * 4 + 3];
                }
            }
            dst_put(&dst, (uint16_t)x, (uint16_t)y, r, g, b, a);
        }
    }

    memset(out, 0, sizeof(*out));
    out->px = dst.px;
    out->len = (uint32_t)dst.stride * dst.h;
    out->w = dst.w;
    out->h = dst.h;
    out->stride = dst.stride;
    out->format = dst.format;
    out->owned = 1;
    return QL_OK;
}

/* ------------------------------ QOI ------------------------------ */

/* QOI：规范极小（六种 op），天然适合当"构建期转码的中间格式" */
static int decode_qoi(const QLImageDecodeReq *req, QLImage *out, uint8_t outf)
{
    const uint8_t *d = req->data;
    const size_t n = req->len;
    if (n < 14u || memcmp(d, "qoif", 4) != 0)
        return QL_ERR_INVAL;

    const uint32_t w = rd_u32le(d + 4);
    const uint32_t h = rd_u32le(d + 8);
    if (w == 0 || h == 0 || w > 0xFFFFu || h > 0xFFFFu)
        return QL_ERR_INVAL;

    Dst dst;
    memset(&dst, 0, sizeof(dst));
    if (dst_alloc(&dst, (uint16_t)w, (uint16_t)h, outf) == NULL)
        return QL_ERR_INVAL;

    uint8_t index[64][4];
    memset(index, 0, sizeof(index));
    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t p = 14;
    const uint32_t total = w * h;

    for (uint32_t i = 0; i < total; i++) {
        if (p >= n) {
            ql_mem_free(dst.px);
            return QL_ERR_INVAL;        /* 数据不够：不猜 */
        }
        const uint8_t op = d[p++];
        uint32_t run = 0;
        if (op == 0xFEu) {                       /* RGB */
            if (p + 3u > n) goto bad;
            r = d[p];
            g = d[p + 1];
            b = d[p + 2];
            p += 3u;
        } else if (op == 0xFFu) {                /* RGBA */
            if (p + 4u > n) goto bad;
            r = d[p];
            g = d[p + 1];
            b = d[p + 2];
            a = d[p + 3];
            p += 4u;
        } else if ((op & 0xC0u) == 0x00u) {      /* INDEX */
            const uint8_t *e = index[op & 0x3Fu];
            r = e[0];
            g = e[1];
            b = e[2];
            a = e[3];
        } else if ((op & 0xC0u) == 0x40u) {      /* DIFF */
            r = (uint8_t)(r + ((op >> 4) & 0x03u) - 2);
            g = (uint8_t)(g + ((op >> 2) & 0x03u) - 2);
            b = (uint8_t)(b + (op & 0x03u) - 2);
        } else if ((op & 0xC0u) == 0x80u) {      /* LUMA */
            if (p + 1u > n) goto bad;
            const uint8_t dg = (uint8_t)((op & 0x3Fu) - 32);
            const uint8_t b2 = d[p++];
            r = (uint8_t)(r + dg + (b2 >> 4) - 8);
            g = (uint8_t)(g + dg);
            b = (uint8_t)(b + dg + (b2 & 0x0Fu) - 8);
        } else {                                  /* RUN：重复上一个像素 */
            run = (uint32_t)(op & 0x3Fu) + 1u;
        }

        /*
         * 写像素：RUN 写 run 个，其余写 1 个。
         * **别把"是否 RUN"写成 `(op & 0xC0) != 0xC0`** —— 0xFE/0xFF（RGB/RGBA）
         * 的高两位也是 11，那样会把所有真彩色像素整块吞掉（第一次就踩了）。
         */
        const uint32_t cnt = (run > 0u) ? run : 1u;
        if (i + cnt > total)
            goto bad;
        for (uint32_t k = 0; k < cnt; k++, i++)
            dst_put(&dst, (uint16_t)(i % w), (uint16_t)(i / w), r, g, b, a);
        i--;                                      /* for 循环还会再 ++ */

        const uint8_t h6 = (uint8_t)((r * 3u + g * 5u + b * 7u + a * 11u) % 64u);
        index[h6][0] = r;
        index[h6][1] = g;
        index[h6][2] = b;
        index[h6][3] = a;
    }

    memset(out, 0, sizeof(*out));
    out->px = dst.px;
    out->len = (uint32_t)dst.stride * dst.h;
    out->w = dst.w;
    out->h = dst.h;
    out->stride = dst.stride;
    out->format = dst.format;
    out->owned = 1;
    return QL_OK;

bad:
    ql_mem_free(dst.px);
    return QL_ERR_INVAL;
}

/* ------------------------------ PNG ------------------------------ */

#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE

/*
 * PNG 的难点只有 DEFLATE 一处（已经由 qlinflate.c 解决）；容器本身是
 * "块（长度 + 类型 + 数据 + CRC）"。这里刻意**不校验 CRC**：CRC 只说明"传输没坏"，
 * 而本库关心的是"能不能安全地解出来" —— 结构性错误（块越界、码表非法、
 * 回溯越界、调色板索引越界）全都会明确失败，CRC 坏但结构合法的图我们会照解。
 * 代价写进文档，收益是少一份 1 KB 的查表。
 *
 * **不支持**：隔行扫描（Adam7）、APNG 多帧、iCCP/gAMA 等色彩管理块。
 * 隔行扫描在 UI 素材里极少，解它需要 7 趟子图重建，明确报 UNSUPPORTED 更诚实。
 */

#define PNG_SIG_LEN 8u

/*
 * IDAT 可能被拆成多个块，中间夹着 12 字节（长度+类型 / CRC），所以做成流式读取。
 *
 * 注意 **PNG 的 IDAT 是 zlib 数据流（RFC 1950）而不是裸 DEFLATE**：
 * 开头有 2 字节头（CMF/FLG），结尾有 4 字节 Adler-32。头在 `skip` 里跳掉，
 * 尾用 `stop` 截断 —— 这样交给 `ql_inflate` 的就是纯 DEFLATE 片段。
 */
typedef struct {
    const uint8_t *d;
    size_t         n;
    size_t         next;    /* 下一个 chunk 头部偏移 */
    size_t         stop;    /* 全局上界 = 最后一个 IDAT 数据结尾 - 4（Adler-32 之前） */
    size_t         skip;    /* 第一个 IDAT 段开头要跳过的字节数（zlib 头） */
    const uint8_t *p;
    size_t         left;    /* 当前 IDAT 段剩余字节 */
    int            done;
} PngIdat;

static int png_is(const uint8_t *t, const char *name)
{
    return t[0] == (uint8_t)name[0] && t[1] == (uint8_t)name[1] &&
           t[2] == (uint8_t)name[2] && t[3] == (uint8_t)name[3];
}

/* 推进到下一个 IDAT 段；返回 0 = 没有更多（IEND / 文件结束 / 结构错） */
static int png_next_idat(PngIdat *s)
{
    while (!s->done) {
        if (s->next + 12u > s->n) {
            s->done = 1;
            return 0;
        }
        const uint32_t clen = rd_u32be(s->d + s->next);
        const uint8_t *ct = s->d + s->next + 4u;
        if ((size_t)clen > s->n || s->next + 12u + (size_t)clen > s->n) {
            s->done = 1;
            return 0;
        }
        const size_t data_off = s->next + 8u;
        const size_t next_hdr = data_off + (size_t)clen + 4u;

        if (png_is(ct, "IEND")) {
            s->done = 1;
            return 0;
        }
        if (!png_is(ct, "IDAT")) {
            s->next = next_hdr;
            continue;
        }
        /* 第一段要跳 zlib 头；所有段都要在 Adler-32 之前收住 */
        const size_t off = data_off + s->skip;
        s->skip = 0;
        s->next = next_hdr;
        if (off >= s->stop || off < data_off) {
            s->done = 1;
            return 0;
        }
        size_t avail = (size_t)clen - (off - data_off);
        const size_t room = s->stop - off;
        if (avail > room)
            avail = room;
        if (avail == 0)
            continue;                  /* 空 IDAT：去下一个 */
        s->p = s->d + off;
        s->left = avail;
        return 1;
    }
    return 0;
}

static size_t png_read(void *ctx, uint8_t *dst, size_t max)
{
    PngIdat *s = (PngIdat *)ctx;
    size_t got = 0;
    while (got < max) {
        if (s->left == 0 && !png_next_idat(s))
            break;
        size_t take = max - got;
        if (take > s->left)
            take = s->left;
        memcpy(dst + got, s->p, take);
        s->p += take;
        s->left -= take;
        got += take;
    }
    return got;
}

/* PNG 的"每像素字节数"（过滤器用，向上取整，最小 1） */
static size_t png_filter_bpp(uint64_t bits_per_px)
{
    const size_t b = (size_t)((bits_per_px + 7u) / 8u);
    return (b == 0) ? 1u : b;
}

static int png_paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = (p > a) ? (p - a) : (a - p);
    const int pb = (p > b) ? (p - b) : (b - p);
    const int pc = (p > c) ? (p - c) : (c - p);
    if (pa <= pb && pa <= pc)
        return a;
    return (pb <= pc) ? b : c;
}

/* zlib 的 Adler-32（模数 65521；5552 是"不溢出 uint32"的最大块） */
static uint32_t png_adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t chunk = (n < 5552u) ? n : 5552u;
        n -= chunk;
        while (chunk-- > 0) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

/* 行反过滤（就地）。返回 0 = 非法的过滤类型 */
static int png_unfilter(int ft, uint8_t *cur, const uint8_t *prev, size_t n, size_t bpp)
{
    switch (ft) {
    case 0:                            /* None */
        return 1;
    case 1:                            /* Sub */
        for (size_t i = bpp; i < n; i++)
            cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        return 1;
    case 2:                            /* Up */
        if (prev != NULL)
            for (size_t i = 0; i < n; i++)
                cur[i] = (uint8_t)(cur[i] + prev[i]);
        return 1;
    case 3:                            /* Average：左边没有就是 0，上一行没有也是 0 */
        for (size_t i = 0; i < n; i++) {
            const uint32_t left = (i >= bpp) ? cur[i - bpp] : 0u;
            const uint32_t up = (prev != NULL) ? prev[i] : 0u;
            cur[i] = (uint8_t)(cur[i] + (uint8_t)((left + up) >> 1));
        }
        return 1;
    case 4:                            /* Paeth：上一行不存在时退化成 Sub */
        for (size_t i = 0; i < n; i++) {
            const int left = (i >= bpp) ? cur[i - bpp] : 0;
            const int up = (prev != NULL) ? prev[i] : 0;
            const int ul = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0;
            cur[i] = (uint8_t)(cur[i] + (uint8_t)png_paeth(left, up, ul));
        }
        return 1;
    default:
        return 0;
    }
}

/* 取第 idx 个采样值（原样，未缩放到 8 位） */
static uint32_t png_sample(const uint8_t *row, size_t idx, int depth)
{
    switch (depth) {
    case 8:
        return row[idx];
    case 16:
        return ((uint32_t)row[idx * 2u] << 8) | row[idx * 2u + 1u];
    default: {
        const size_t bit = idx * (size_t)depth;
        const uint32_t b = (uint32_t)row[bit >> 3] >> (8u - (uint32_t)depth - (bit & 7u));
        return b & ((1u << depth) - 1u);
    }
    }
}

/* 采样值缩放到 8 位（1/2/4 位用整数倍扩展，16 位取高字节） */
static uint8_t png_to8(uint32_t v, int depth)
{
    switch (depth) {
    case 1:
        return (uint8_t)(v ? 255u : 0u);
    case 2:
        return (uint8_t)(v * 85u);
    case 4:
        return (uint8_t)(v * 17u);
    case 8:
        return (uint8_t)v;
    default:
        return (uint8_t)(v >> 8);
    }
}

static int decode_png(const QLImageDecodeReq *req, QLImage *out, uint8_t outf)
{
    const uint8_t *d = req->data;
    const size_t n = req->len;

    if (n < PNG_SIG_LEN + 25u || memcmp(d, "\x89PNG\r\n\x1a\n", PNG_SIG_LEN) != 0)
        return QL_ERR_INVAL;
    /* 第一个块必须是 13 字节的 IHDR */
    if (rd_u32be(d + PNG_SIG_LEN) != 13u ||
        !png_is(d + PNG_SIG_LEN + 4u, "IHDR"))
        return QL_ERR_INVAL;

    const uint8_t *ih = d + PNG_SIG_LEN + 8u;
    const uint32_t pw = rd_u32be(ih);
    const uint32_t ph = rd_u32be(ih + 4u);
    const int depth = ih[8];
    const int ctype = ih[9];
    const int comp = ih[10];
    const int filt = ih[11];
    const int inter = ih[12];

    if (pw == 0u || ph == 0u || pw > 0xFFFFu || ph > 0xFFFFu)
        return QL_ERR_INVAL;
    if (pw > QL_CFG_IMAGE_MAX_PIXELS / ph)
        return QL_ERR_INVAL;           /* 先除后乘，挡 w*h 溢出 */
    if (comp != 0 || filt != 0)
        return QL_ERR_UNSUPPORTED;     /* 只定义了 0；将来也别猜 */
    if (inter != 0)
        return QL_ERR_UNSUPPORTED;     /* Adam7 不做 */

    int chan = 0;
    int depth_ok = 0;
    switch (ctype) {
    case 0:                            /* 灰度 */
        chan = 1;
        depth_ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16);
        break;
    case 2:                            /* 真彩 */
        chan = 3;
        depth_ok = (depth == 8 || depth == 16);
        break;
    case 3:                            /* 调色板 */
        chan = 1;
        depth_ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8);
        break;
    case 4:                            /* 灰度 + alpha */
        chan = 2;
        depth_ok = (depth == 8 || depth == 16);
        break;
    case 6:                            /* 真彩 + alpha */
        chan = 4;
        depth_ok = (depth == 8 || depth == 16);
        break;
    default:
        return QL_ERR_INVAL;
    }
    if (!depth_ok)
        return QL_ERR_INVAL;

    const uint64_t bits_per_px = (uint64_t)chan * (uint64_t)depth;
    const size_t row_bytes = (size_t)(((uint64_t)pw * bits_per_px + 7u) / 8u);
    const uint64_t raw_size64 = (uint64_t)ph * ((uint64_t)row_bytes + 1u);
    /*
     * 过滤后的字节数也要有界。它比解码输出大：16 位 RGBA 是 8 字节/像素，
     * 而输出 RGBA8888 只有 4 —— 所以预算取"解码预算的 2 倍 + 每行 1 字节"，
     * 这样 1024×1024 的 16 位 RGBA 正好卡在边上而不被误杀。
     */
    if (raw_size64 > (uint64_t)QL_CFG_IMAGE_MAX_BYTES * 16u + (uint64_t)ph)
        return QL_ERR_INVAL;

    /* ---- 扫一遍块：取 PLTE / tRNS，并记住第一个 IDAT ---- */
    uint8_t plte[256 * 3];
    uint8_t trns[256];
    uint16_t plte_n = 0;               /* 0..256：**不能**用 uint8，256 会回绕成 0 */
    int trns_n = 0;
    int trns_gray = -1;
    int trns_rgb[3] = { -1, -1, -1 };
    size_t first_idat_hdr = 0;         /* 第一个 IDAT 的 chunk 头偏移 */
    size_t first_idat = 0;             /* 第一个 IDAT 的 **数据** 起始偏移 */
    size_t last_idat_end = 0;          /* 最后一个 IDAT 的数据结尾（CRC 之前） */
    for (size_t co = PNG_SIG_LEN; co + 12u <= n;) {
        const uint32_t clen = rd_u32be(d + co);
        const uint8_t *ct = d + co + 4u;
        if ((size_t)clen > n || co + 12u + (size_t)clen > n)
            return QL_ERR_INVAL;
        const uint8_t *cd = d + co + 8u;
        if (png_is(ct, "IEND"))
            break;
        if (png_is(ct, "IDAT")) {
            if (first_idat_hdr == 0) {
                first_idat_hdr = co;
                first_idat = co + 8u;
            }
            last_idat_end = co + 8u + (size_t)clen;
        } else if (png_is(ct, "PLTE")) {
            if (clen == 0u || (clen % 3u) != 0u || clen > 768u)
                return QL_ERR_INVAL;
            plte_n = (uint16_t)(clen / 3u);
            memcpy(plte, cd, clen);
        } else if (png_is(ct, "tRNS")) {
            if (ctype == 3) {
                if (clen > 256u)
                    return QL_ERR_INVAL;
                trns_n = (int)clen;
                memcpy(trns, cd, clen);
            } else if (ctype == 0 && clen >= 2u) {
                trns_gray = (int)rd_u16be(cd);
            } else if (ctype == 2 && clen >= 6u) {
                trns_rgb[0] = (int)rd_u16be(cd);
                trns_rgb[1] = (int)rd_u16be(cd + 2u);
                trns_rgb[2] = (int)rd_u16be(cd + 4u);
            }
        }
        co += 12u + (size_t)clen;
    }
    /* zlib 头（2）+ 至少一个 DEFLATE 字节 + Adler-32（4） */
    if (first_idat == 0 || last_idat_end < first_idat + 7u)
        return QL_ERR_INVAL;           /* 没有像素数据 / 短到不可能是 zlib 流 */
    if (ctype == 3 && plte_n == 0)
        return QL_ERR_INVAL;           /* 调色板图必须有 PLTE */

    /* 校验 zlib 头：CM=8（DEFLATE）、CINFO<=7、FCHECK、FDICT==0 */
    {
        const uint8_t cmf = d[first_idat];
        const uint8_t flg = d[first_idat + 1u];
        if ((cmf & 0x0Fu) != 8u || (cmf >> 4) > 7u)
            return QL_ERR_UNSUPPORTED; /* 压缩方法是别的（PNG 只定义了 0=zlib） */
        if ((((uint32_t)cmf << 8) | (uint32_t)flg) % 31u != 0u)
            return QL_ERR_INVAL;       /* 头本身不合法 */
        if ((flg & 0x20u) != 0u)
            return QL_ERR_UNSUPPORTED; /* 预置字典：没有字典就没法解 */
    }

    /* ---- 解压 ---- */
    const size_t raw_size = (size_t)raw_size64;
    uint8_t *raw = (uint8_t *)ql_mem_alloc(raw_size);
    if (raw == NULL)
        return QL_ERR_NOMEM;

    PngIdat idat;
    memset(&idat, 0, sizeof(idat));
    idat.d = d;
    idat.n = n;
    idat.next = first_idat_hdr;
    idat.stop = last_idat_end - 4u;    /* Adler-32 之前收尾 */
    idat.skip = 2u;                    /* zlib 头 */

    QLZSource zsrc;
    zsrc.read = png_read;
    zsrc.ctx = &idat;

    size_t got = 0;
    const int zrc = ql_inflate(&zsrc, raw, raw_size, &got);
    /* 解出来的字节数必须**正好**等于 h*(1+row_bytes)：少了是残图，多了是撒谎 */
    if (zrc != QLZ_OK || got != raw_size) {
        ql_mem_free(raw);
        return QL_ERR_INVAL;
    }
    /* zlib 的 Adler-32 是"整段解密数据"的校验，成本几乎为零，顺手就校了 */
    if (png_adler32(raw, raw_size) != rd_u32be(d + last_idat_end - 4u)) {
        ql_mem_free(raw);
        return QL_ERR_INVAL;
    }

    /* ---- 逐行反过滤 ---- */
    const size_t bpp = png_filter_bpp(bits_per_px);
    for (uint32_t y = 0; y < ph; y++) {
        uint8_t *row = raw + (size_t)y * (row_bytes + 1u);
        const uint8_t *prev = (y > 0) ? (row - (row_bytes + 1u) + 1u) : NULL;
        if (!png_unfilter(row[0], row + 1u, prev, row_bytes, bpp)) {
            ql_mem_free(raw);
            return QL_ERR_INVAL;
        }
    }

    /* ---- 转成输出像素 ---- */
    Dst dst;
    memset(&dst, 0, sizeof(dst));
    if (dst_alloc(&dst, (uint16_t)pw, (uint16_t)ph, outf) == NULL) {
        ql_mem_free(raw);
        return QL_ERR_INVAL;
    }

    for (uint32_t y = 0; y < ph; y++) {
        const uint8_t *row = raw + (size_t)y * (row_bytes + 1u) + 1u;
        for (uint32_t x = 0; x < pw; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            const size_t px = (size_t)x;
            if (ctype == 3) {
                const uint32_t idx = png_sample(row, px, depth);
                if (idx >= (uint32_t)plte_n) {
                    ql_mem_free(raw);
                    ql_mem_free(dst.px);
                    return QL_ERR_INVAL;   /* 索引越界：不猜成黑色 */
                }
                r = plte[idx * 3u];
                g = plte[idx * 3u + 1u];
                b = plte[idx * 3u + 2u];
                a = (idx < (uint32_t)trns_n) ? trns[idx] : 255u;
            } else if (ctype == 0) {
                const uint32_t v = png_sample(row, px, depth);
                r = g = b = png_to8(v, depth);
                a = (trns_gray >= 0 && v == (uint32_t)trns_gray) ? 0u : 255u;
            } else if (ctype == 2) {
                const uint32_t sr = png_sample(row, px * 3u + 0u, depth);
                const uint32_t sg = png_sample(row, px * 3u + 1u, depth);
                const uint32_t sb = png_sample(row, px * 3u + 2u, depth);
                r = png_to8(sr, depth);
                g = png_to8(sg, depth);
                b = png_to8(sb, depth);
                if (trns_rgb[0] >= 0 && sr == (uint32_t)trns_rgb[0] &&
                    sg == (uint32_t)trns_rgb[1] && sb == (uint32_t)trns_rgb[2])
                    a = 0;
            } else if (ctype == 4) {
                r = g = b = png_to8(png_sample(row, px * 2u, depth), depth);
                a = png_to8(png_sample(row, px * 2u + 1u, depth), depth);
            } else {
                r = png_to8(png_sample(row, px * 4u + 0u, depth), depth);
                g = png_to8(png_sample(row, px * 4u + 1u, depth), depth);
                b = png_to8(png_sample(row, px * 4u + 2u, depth), depth);
                a = png_to8(png_sample(row, px * 4u + 3u, depth), depth);
            }
            dst_put(&dst, (uint16_t)x, (uint16_t)y, r, g, b, a);
        }
    }
    ql_mem_free(raw);

    memset(out, 0, sizeof(*out));
    out->px = dst.px;
    out->len = (uint32_t)dst.stride * dst.h;
    out->w = dst.w;
    out->h = dst.h;
    out->stride = dst.stride;
    out->format = dst.format;
    out->owned = 1;
    return QL_OK;
}

#endif /* QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE */

/* ------------------------------ GIF（首帧） ------------------------------ */

#if QL_FEAT_IMG_GIF

/*
 * GIF：**只解首帧**。动画（帧时钟 + 帧缓冲 + 循环策略）明确不做，第二帧起忽略。
 *
 * 为什么自己写而不是给平台后端：GIF 的静态部分（调色板 + LZW + 隔行）
 * 是纯无状态算法，和 QOI 一个难度档；而 giflib 也不是到处都有。
 * 当初"GIF 不自己写"的理由是**动画**，那条限制没变。
 */

/* 子块读取：GIF 的 LZW 数据被切成 ≤255 字节的块，每块前面一个长度字节 */
typedef struct {
    const uint8_t *d;
    size_t         n;
    size_t         p;      /* 下一个块的长度字节位置 */
    size_t         left;   /* 当前块剩余 */
    size_t         cur;    /* 当前块读取位置 */
    int            done;
} GifSub;

static int gif_sub_next(GifSub *s)
{
    while (!s->done) {
        if (s->p >= s->n) {
            s->done = 1;
            return 0;
        }
        const uint8_t sz = s->d[s->p++];
        if (sz == 0) {
            s->done = 1;
            return 0;
        }
        if (s->p + sz > s->n) {
            s->done = 1;
            return 0;
        }
        s->cur = s->p;
        s->left = sz;
        s->p += sz;
        return 1;
    }
    return 0;
}

static int gif_bit(GifSub *s, int *bits_left, uint32_t *acc, int nbits, uint32_t *out)
{
    while (*bits_left < nbits) {
        if (s->left == 0 && !gif_sub_next(s))
            return 0;
        *acc |= (uint32_t)s->d[s->cur++] << *bits_left;
        s->left--;
        *bits_left += 8;
    }
    *out = *acc & ((1u << nbits) - 1u);
    *acc >>= nbits;
    *bits_left -= nbits;
    return 1;
}

/* LZW 的表（16 KB）。做成调用方传入的工作区而不是 static：
 * 静态就不重入，放在栈上又太大 —— 一次性 malloc 更好，用完就还。 */
typedef struct {
    uint16_t prefix[4096];
    uint8_t  suffix[4096];
    uint8_t  stack[4096];
} GifLzw;

/* LZW 解码：把索引流写进 idx[]（容量 cap），返回写出的个数，-1 = 数据非法 */
static int gif_lzw(GifLzw *w, GifSub *s, int min_size, uint8_t *idx, size_t cap,
                   size_t *out_n)
{
    if (min_size < 2 || min_size > 8)
        return -1;
    const int clear = 1 << min_size;
    const int end = clear + 1;
    uint16_t *const prefix = w->prefix;
    uint8_t *const suffix = w->suffix;
    uint8_t *const stack = w->stack;

    int code_size = min_size + 1;
    int next = end + 1;
    int old = -1;
    int first = 0;
    size_t n = 0;
    int bits_left = 0;
    uint32_t acc = 0;

    for (;;) {
        if (next == (1 << code_size) && code_size < 12)
            code_size++;
        uint32_t code32 = 0;
        if (!gif_bit(s, &bits_left, &acc, code_size, &code32))
            return -1;                 /* 流在 end 码之前就没了 */
        int code = (int)code32;

        if (code == clear) {
            code_size = min_size + 1;
            next = end + 1;
            old = -1;
            continue;
        }
        if (code == end)
            break;
        if (old < 0) {
            if (code >= clear)
                return -1;
            if (n >= cap)
                return -1;
            idx[n++] = (uint8_t)code;
            first = code;
            old = code;
            continue;
        }

        const int in = code;
        size_t sp = 0;
        if (code >= next) {
            if (code != next)
                return -1;             /* 超出"下一个待建表项"：非法码 */
            stack[sp++] = (uint8_t)first;   /* KwKwK：先补首字节 */
            code = old;
        }
        while (code >= clear) {
            if (sp >= sizeof(w->stack) || code >= 4096)
                return -1;
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        first = code;
        stack[sp++] = (uint8_t)first;
        while (sp > 0) {
            if (n >= cap)
                return -1;
            idx[n++] = stack[--sp];
        }
        if (next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = (uint8_t)first;
            next++;
        }
        old = in;
    }

    *out_n = n;
    return 0;
}

static int decode_gif(const QLImageDecodeReq *req, QLImage *out, uint8_t outf)
{
    const uint8_t *d = req->data;
    const size_t n = req->len;

    if (n < 14u)
        return QL_ERR_INVAL;
    if (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0)
        return QL_ERR_INVAL;

    const uint32_t sw = rd_u16le(d + 6);
    const uint32_t sh = rd_u16le(d + 8);
    if (sw == 0u || sh == 0u || sw > 0xFFFFu || sh > 0xFFFFu)
        return QL_ERR_INVAL;
    if (sw > QL_CFG_IMAGE_MAX_PIXELS / sh)
        return QL_ERR_INVAL;           /* 先除后乘 */

    const uint8_t fl = d[10];
    size_t p = 13;
    uint8_t gct[256 * 3];
    int gct_n = 0;
    if ((fl & 0x80u) != 0u) {
        gct_n = 2 << (fl & 7u);
        if (p + (size_t)gct_n * 3u > n)
            return QL_ERR_INVAL;
        memcpy(gct, d + p, (size_t)gct_n * 3u);
        p += (size_t)gct_n * 3u;
    }

    /* ---- 走到第一个图像描述符，路上记下透明色（图形控制扩展） ---- */
    int trans = -1;
    for (;;) {
        if (p >= n)
            return QL_ERR_INVAL;
        const uint8_t b = d[p++];
        if (b == 0x3Bu)
            return QL_ERR_INVAL;       /* 只到 trailer：没有图像 */
        if (b == 0x21u) {              /* 扩展块 */
            if (p >= n)
                return QL_ERR_INVAL;
            const uint8_t label = d[p++];
            if (label == 0xF9u) {      /* 图形控制 */
                if (p >= n)
                    return QL_ERR_INVAL;
                const uint8_t bs = d[p++];
                if (bs < 4u || p + 4u > n)
                    return QL_ERR_INVAL;
                const uint8_t gp = d[p];
                trans = ((gp & 0x01u) != 0u) ? (int)d[p + 3u] : -1;
                p += 4u;
                if (p >= n || d[p] != 0u)
                    return QL_ERR_INVAL;   /* 块终止符 */
                p++;
            } else {                   /* 其它扩展：整段跳过 */
                for (;;) {
                    if (p >= n)
                        return QL_ERR_INVAL;
                    const uint8_t sz = d[p++];
                    if (sz == 0u)
                        break;
                    if (p + sz > n)
                        return QL_ERR_INVAL;
                    p += sz;
                }
            }
            continue;
        }
        if (b == 0x2Cu)
            break;                     /* 图像描述符 */
        return QL_ERR_INVAL;           /* 不认识的块 */
    }

    if (p + 9u > n)
        return QL_ERR_INVAL;
    const uint32_t left = rd_u16le(d + p);
    const uint32_t top = rd_u16le(d + p + 2u);
    const uint32_t iw = rd_u16le(d + p + 4u);
    const uint32_t ih = rd_u16le(d + p + 6u);
    const uint8_t ip = d[p + 8u];
    p += 9u;
    if (iw == 0u || ih == 0u)
        return QL_ERR_INVAL;
    if (iw > QL_CFG_IMAGE_MAX_PIXELS / ih)
        return QL_ERR_INVAL;

    uint8_t lct[256 * 3];
    int lct_n = 0;
    if ((ip & 0x80u) != 0u) {
        lct_n = 2 << (ip & 7u);
        if (p + (size_t)lct_n * 3u > n)
            return QL_ERR_INVAL;
        memcpy(lct, d + p, (size_t)lct_n * 3u);
        p += (size_t)lct_n * 3u;
    }
    const int interlace = (ip & 0x40u) != 0u;

    const uint8_t *pal = (lct_n > 0) ? lct : gct;
    const int pal_n = (lct_n > 0) ? lct_n : gct_n;
    if (pal_n == 0)
        return QL_ERR_INVAL;           /* 没有颜色表：没法上色 */

    if (p >= n)
        return QL_ERR_INVAL;
    const int min_size = d[p++];

    /* ---- LZW 解出索引流 ---- */
    const size_t npix = (size_t)iw * (size_t)ih;
    uint8_t *idx = (uint8_t *)ql_mem_alloc(npix);
    if (idx == NULL)
        return QL_ERR_NOMEM;

    GifLzw *lzw = (GifLzw *)ql_mem_alloc(sizeof(GifLzw));
    if (lzw == NULL) {
        ql_mem_free(idx);
        return QL_ERR_NOMEM;
    }

    GifSub sub;
    memset(&sub, 0, sizeof(sub));
    sub.d = d;
    sub.n = n;
    sub.p = p;

    size_t got = 0;
    const int lrc = gif_lzw(lzw, &sub, min_size, idx, npix, &got);
    ql_mem_free(lzw);
    if (lrc != 0 || got != npix) {
        ql_mem_free(idx);
        return QL_ERR_INVAL;           /* 像素不够 / 码流非法：不画半个图 */
    }

    /* ---- 上屏：画布 = 逻辑屏幕，未覆盖处透明 ---- */
    Dst dst;
    memset(&dst, 0, sizeof(dst));
    if (dst_alloc(&dst, (uint16_t)sw, (uint16_t)sh, outf) == NULL) {
        ql_mem_free(idx);
        return QL_ERR_INVAL;
    }

    /* 隔行 GIF 的行序：0,8,16… / 4,12,… / 2,6,… / 1,3,… */
    static const int k_pass_start[4] = { 0, 4, 2, 1 };
    static const int k_pass_step[4] = { 8, 8, 4, 2 };

    size_t src_row = 0;
    for (int pass = 0; pass < (interlace ? 4 : 1); pass++) {
        const int start = interlace ? k_pass_start[pass] : 0;
        const int step = interlace ? k_pass_step[pass] : 1;
        for (uint32_t y = (uint32_t)start; y < ih; y += (uint32_t)step) {
            const size_t base = src_row * (size_t)iw;
            src_row++;
            for (uint32_t x = 0; x < iw; x++) {
                const uint8_t ci = idx[base + x];
                /* pal_n 可能是 256 —— 用 uint8_t 比会回绕成 0，整张图全透明 */
                if ((int)ci >= pal_n)
                    continue;          /* 越界索引：留透明，也不猜颜色 */
                if (trans >= 0 && (int)ci == trans)
                    continue;
                const uint32_t dx = left + x;
                const uint32_t dy = top + y;
                if (dx >= sw || dy >= sh)
                    continue;          /* 超出画布：裁掉（合法） */
                dst_put(&dst, (uint16_t)dx, (uint16_t)dy, pal[ci * 3u],
                        pal[ci * 3u + 1u], pal[ci * 3u + 2u], 255);
            }
        }
    }
    ql_mem_free(idx);

    memset(out, 0, sizeof(*out));
    out->px = dst.px;
    out->len = (uint32_t)dst.stride * dst.h;
    out->w = dst.w;
    out->h = dst.h;
    out->stride = dst.stride;
    out->format = dst.format;
    out->owned = 1;
    return QL_OK;
}

#endif /* QL_FEAT_IMG_GIF */

/* ------------------------------ 像素格式转换 ------------------------------ */

/* 从任意输出格式读一个像素（非预乘 RGBA） */
static void src_get(const QLImage *s, uint32_t x, uint32_t y, uint8_t *r, uint8_t *g,
                    uint8_t *b, uint8_t *a)
{
    const uint8_t *p = s->px + (size_t)y * s->stride;
    switch (s->format) {
    case QL_IMAGE_RGB565: {
        const uint32_t v = (uint32_t)p[x * 2u] | ((uint32_t)p[x * 2u + 1u] << 8);
        *r = (uint8_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
        *g = (uint8_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        *b = (uint8_t)((v & 0x1Fu) * 255u / 31u);
        *a = 255;
        break;
    }
    case QL_IMAGE_RGB888:
        *r = p[x * 3u];
        *g = p[x * 3u + 1u];
        *b = p[x * 3u + 2u];
        *a = 255;
        break;
    case QL_IMAGE_A8:
        *r = *g = *b = p[x];
        *a = 255;
        break;
    default:
        *r = p[x * 4u];
        *g = p[x * 4u + 1u];
        *b = p[x * 4u + 2u];
        *a = p[x * 4u + 3u];
        break;
    }
}

int ql_image_convert(const QLImage *src, uint8_t dst_format, QLImage *out)
{
    if (out == NULL)
        return QL_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (src == NULL || src->px == NULL || ql_image_format_bpp(dst_format) == 0)
        return QL_ERR_INVAL;
    if (!ql_image_validate(src))
        return QL_ERR_INVAL;

    if (src->format == dst_format) {
        *out = *src;                   /* 同格式：不复制，指针直接交出去 */
        return QL_OK;
    }

    Dst dst;
    memset(&dst, 0, sizeof(dst));
    if (dst_alloc(&dst, src->w, src->h, dst_format) == NULL)
        return QL_ERR_NOMEM;

    for (uint32_t y = 0; y < src->h; y++) {
        for (uint32_t x = 0; x < src->w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            src_get(src, x, y, &r, &g, &b, &a);
            dst_put(&dst, (uint16_t)x, (uint16_t)y, r, g, b, a);
        }
    }

    out->px = dst.px;
    out->len = (uint32_t)dst.stride * dst.h;
    out->w = dst.w;
    out->h = dst.h;
    out->stride = dst.stride;
    out->format = dst.format;
    out->owned = 1;
    return QL_OK;
}

/* ------------------------------ 平台后端 ------------------------------ */

static const QLImageBackend *s_backend;
static char s_err[160];

const char *ql_image_last_error(void)
{
    return (s_err[0] != '\0') ? s_err : NULL;
}

void ql_image_set_platform_backend(const QLImageBackend *be)
{
    s_backend = be;
}

const QLImageBackend *ql_image_platform_backend(void)
{
    return s_backend;
}

/*
 * 问平台后端，然后**把它的返回值当外来数据**：先校验、再按需要转格式。
 * 这是全库唯一一处"第三方库的结果直接进绘制层"的入口，所以宁可贵一点。
 */
static int decode_via_backend(const QLImageDecodeReq *req, QLImage *out, uint8_t outf)
{
    char buf[128];
    buf[0] = '\0';

    QLImage tmp;
    memset(&tmp, 0, sizeof(tmp));
    const int rc = s_backend->decode(req->data, req->len, outf, &tmp, buf, sizeof(buf));
    if (rc != 1) {
        if (rc != 0) {
            snprintf(s_err, sizeof(s_err), "%s: %s",
                     (s_backend->name != NULL) ? s_backend->name : "后端",
                     (buf[0] != '\0') ? buf : "解码失败");
        }
        return (rc == 0) ? QL_ERR_UNSUPPORTED : QL_ERR_INVAL;
    }

    /* 不信任返回值：结构必须自洽、格式必须在枚举内、owned 必须是 0/1 */
    if (tmp.px == NULL || tmp.owned > 1 || !ql_image_validate(&tmp)) {
        if (tmp.owned == 1 && tmp.px != NULL)
            ql_mem_free((void *)tmp.px);
        snprintf(s_err, sizeof(s_err), "后端 %s 返回了不自洽的图（w=%u h=%u stride=%u）",
                 (s_backend->name != NULL) ? s_backend->name : "?",
                 (unsigned)tmp.w, (unsigned)tmp.h, (unsigned)tmp.stride);
        return QL_ERR_INVAL;
    }

    /* 后端只保证"解出来了"，格式在这里收口（转换代码只有一份） */
    QLImage conv;
    const int crc = ql_image_convert(&tmp, outf, &conv);
    /* 目标格式与后端给的一致时 ql_image_convert 不复制，所有权直接转移过来
     * —— 那种情况下**不能**释放 tmp.px（会把 conv 指的那块也释放掉）。 */
    if (crc == QL_OK && conv.px == tmp.px)
        tmp.owned = 0;
    if (tmp.owned == 1)
        ql_mem_free((void *)tmp.px);
    if (crc != QL_OK) {
        snprintf(s_err, sizeof(s_err), "后端 %s 的图无法转成目标像素格式",
                 (s_backend->name != NULL) ? s_backend->name : "?");
        return crc;
    }

    *out = conv;
    return QL_OK;
}

/* ------------------------------ 入口 ------------------------------ */

int ql_image_decode(const QLImageDecodeReq *req, QLImage *out)
{
    if (req == NULL || out == NULL || req->data == NULL)
        return QL_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    s_err[0] = '\0';

    if (req->len == 0 || req->len > QL_CFG_IMAGE_MAX_BYTES)
        return QL_ERR_INVAL;

    uint8_t fmt = req->src_format;
    if (fmt == QL_IMAGE_SRC_AUTO) {
        if (req->len >= 8u && memcmp(req->data, "\x89PNG\r\n\x1a\n", 8) == 0)
            fmt = QL_IMAGE_SRC_PNG;
        else if (req->len >= 6u && memcmp(req->data, "GIF87a", 6) == 0)
            fmt = QL_IMAGE_SRC_GIF;
        else if (req->len >= 6u && memcmp(req->data, "GIF89a", 6) == 0)
            fmt = QL_IMAGE_SRC_GIF;
        else if (req->len >= 2u && req->data[0] == 'B' && req->data[1] == 'M')
            fmt = QL_IMAGE_SRC_BMP;
        else if (req->len >= 4u && memcmp(req->data, "qoif", 4) == 0)
            fmt = QL_IMAGE_SRC_QOI;
        else if (req->len >= 3u && req->data[0] == 0xFFu && req->data[1] == 0xD8u &&
                 req->data[2] == 0xFFu)
            fmt = QL_IMAGE_SRC_JPEG;
        else
            return QL_ERR_UNSUPPORTED;   /* RAW 无法探测，必须显式指定 */
    }

    /* out_format == 0 就是 RGBA8888（枚举顺序保证了这一点），非法值退回 RGBA8888 */
    const uint8_t outf = (ql_image_format_bpp(req->out_format) != 0)
                             ? req->out_format
                             : (uint8_t)QL_IMAGE_OUT_DEFAULT;

    if (fmt == QL_IMAGE_SRC_RAW) {
        QLImage img;
        memset(&img, 0, sizeof(img));
        img.px = req->data;
        img.w = req->w;
        img.h = req->h;
        img.format = req->pixel_format;
        const uint32_t bpp = ql_image_format_bpp(img.format);
        if (bpp == 0)
            return QL_ERR_INVAL;
        img.stride = (uint16_t)((uint32_t)img.w * bpp);
        img.len = (uint32_t)img.stride * img.h;
        if (img.len > req->len)
            return QL_ERR_INVAL;         /* 声明的尺寸大于实际字节数：不猜 */
        if (!ql_image_validate(&img))
            return QL_ERR_INVAL;

        /* RAW 不复制：直接把指针交出去（零 RAM 路径） */
        img.owned = 0;
        *out = img;
        return QL_OK;
    }

    /* 自有解码器优先。返回 UNSUPPORTED 才问平台后端 —— 这样后端永远抢不走
     * BMP/QOI/PNG，但能补上自有解码器明确不做的分支（如隔行扫描的 PNG）。 */
    int rc = QL_ERR_UNSUPPORTED;
    switch (fmt) {
    case QL_IMAGE_SRC_BMP:
#if QL_FEAT_IMG_BMP
        rc = decode_bmp(req, out, outf);
#endif
        break;
    case QL_IMAGE_SRC_QOI:
#if QL_FEAT_IMG_QOI
        rc = decode_qoi(req, out, outf);
#endif
        break;
    case QL_IMAGE_SRC_PNG:
#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE
        rc = decode_png(req, out, outf);
#endif
        break;
    case QL_IMAGE_SRC_GIF:
#if QL_FEAT_IMG_GIF
        rc = decode_gif(req, out, outf);
#endif
        break;
    default:                             /* JPEG 等：自有解码器一律不做 */
        break;
    }

    if (rc != QL_ERR_UNSUPPORTED)
        return rc;

#if QL_FEAT_IMG_PLATFORM
    if (s_backend != NULL && s_backend->decode != NULL)
        return decode_via_backend(req, out, outf);
#endif

    return QL_ERR_UNSUPPORTED;
}
