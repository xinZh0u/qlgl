/**
 * @file    qlimage.c
 * @brief   位图校验与解码入口。
 *
 * 目前只有 RAW 直通（构建期已解好的像素）。BMP / QOI / PNG 解码器按
 * `QL_FEAT_IMG_*` 逐个落地，落地前一律 `QL_ERR_UNSUPPORTED` —— **明确失败**，
 * 不猜、不降级成空白。
 */
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

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

/* ------------------------------ 入口 ------------------------------ */

int ql_image_decode(const QLImageDecodeReq *req, QLImage *out)
{
    if (req == NULL || out == NULL || req->data == NULL)
        return QL_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    if (req->len == 0 || req->len > QL_CFG_IMAGE_MAX_BYTES)
        return QL_ERR_INVAL;

    uint8_t fmt = req->src_format;
    if (fmt == QL_IMAGE_SRC_AUTO) {
        if (req->len >= 8u && memcmp(req->data, "\x89PNG\r\n\x1a\n", 8) == 0)
            fmt = QL_IMAGE_SRC_PNG;
        else if (req->len >= 2u && req->data[0] == 'B' && req->data[1] == 'M')
            fmt = QL_IMAGE_SRC_BMP;
        else if (req->len >= 4u && memcmp(req->data, "qoif", 4) == 0)
            fmt = QL_IMAGE_SRC_QOI;
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

    if (fmt == QL_IMAGE_SRC_BMP) {
#if QL_FEAT_IMG_BMP
        return decode_bmp(req, out, outf);
#else
        return QL_ERR_UNSUPPORTED;
#endif
    }
    if (fmt == QL_IMAGE_SRC_QOI) {
#if QL_FEAT_IMG_QOI
        return decode_qoi(req, out, outf);
#else
        return QL_ERR_UNSUPPORTED;
#endif
    }

    /*
     * PNG（QL_FEAT_IMG_PNG）还没落地，平台后端（QL_FEAT_IMG_PLATFORM）也只搭了接口。
     * 对调用方是同一件事：这个构建不支持这种格式。
     */
    return QL_ERR_UNSUPPORTED;
}
