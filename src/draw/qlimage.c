/**
 * @file    qlimage.c
 * @brief   位图校验与解码入口。
 *
 * 目前只有 RAW 直通（构建期已解好的像素）。BMP / QOI / PNG 解码器按
 * `QL_FEAT_IMG_*` 逐个落地，落地前一律 `QL_ERR_UNSUPPORTED` —— **明确失败**，
 * 不猜、不降级成空白。
 */
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

int ql_image_decode(const QLImageDecodeReq *req, QLImage *out)
{
    if (req == NULL || out == NULL || req->data == NULL)
        return QL_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    if (req->len == 0 || req->len > QL_CFG_IMAGE_MAX_BYTES)
        return QL_ERR_INVAL;

    uint8_t fmt = req->src_format;
    if (fmt == QL_IMAGE_SRC_AUTO) {
        /* 魔数探测：只认自带解码器的几种 */
        if (req->len >= 8u && memcmp(req->data, "\x89PNG\r\n\x1a\n", 8) == 0)
            fmt = QL_IMAGE_SRC_PNG;
        else if (req->len >= 2u && req->data[0] == 'B' && req->data[1] == 'M')
            fmt = QL_IMAGE_SRC_BMP;
        else if (req->len >= 4u && memcmp(req->data, "qoif", 4) == 0)
            fmt = QL_IMAGE_SRC_QOI;
        else
            return QL_ERR_UNSUPPORTED;   /* RAW 无法探测，必须显式指定 */
    }

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

    /*
     * 其余格式：解码器还没落地 / 本构建被裁掉了。两者对调用方是同一件事 ——
     * 这个构建不支持这种格式，所以统一 UNSUPPORTED，由调用方决定怎么报。
     */
    return QL_ERR_UNSUPPORTED;
}
