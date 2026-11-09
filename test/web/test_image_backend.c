/**
 * @file    test_image_backend.c
 * @brief   平台后端契约：装/卸、原生优先、返回值校验、像素格式收口。
 *
 * 这里的后端是**测试自己写的假实现**，所以每条契约都能精确构造：
 * 返回 0 / 返回 -1、返回不自洽的图（stride 错、px NULL、格式非法、尺寸超上限）、
 * 返回 rgb565 让本库转换。真后端（libjpeg）由 `test_jpeg` 覆盖。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#ifndef QL_PNG_ASSET_DIR
#define QL_PNG_ASSET_DIR "test/assets/png"
#endif

/* ------------------------------ 假后端 ------------------------------ */

typedef struct {
    int         mode;        /* 见下 */
    const char *err;         /* mode == FAIL 时的文案 */
    uint8_t     ret_format;  /* mode == SOLID 时产出什么格式 */
    int         calls;
} FakeState;

#define FAKE_ACCEPT 0        /* 认识，产出 2×2 四色 */
#define FAKE_DECLINE 1       /* 不认识（返回 0） */
#define FAKE_FAIL 2          /* 认识但失败（返回 -1） */
#define FAKE_BAD_STRIDE 3    /* stride 与 w*bpp 不一致 */
#define FAKE_NULL_PX 4       /* px == NULL */
#define FAKE_BAD_FORMAT 5    /* format 不在枚举内 */
#define FAKE_TOO_BIG 6       /* 尺寸超 QL_CFG_IMAGE_MAX_PIXELS */
#define FAKE_LEN_MISMATCH 7  /* len != stride*h */
#define FAKE_SOLID 8         /* 单色（用于验证格式转换） */

static FakeState s_fake;

/* 四色：左上红 / 右上绿 / 左下蓝 / 右下白 */
static void put_solid(uint8_t *px, uint32_t stride, uint8_t fmt, int x, int y)
{
    uint8_t r = 0, g = 0, b = 0;
    if (x == 0 && y == 0) { r = 255; }
    else if (x == 1 && y == 0) { g = 255; }
    else if (x == 0 && y == 1) { b = 255; }
    else { r = g = b = 255; }
    uint8_t *p = px + (size_t)y * stride;
    switch (fmt) {
    case QL_IMAGE_RGB565: {
        const uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        p[x * 2] = (uint8_t)v;
        p[x * 2 + 1] = (uint8_t)(v >> 8);
        break;
    }
    case QL_IMAGE_RGB888:
        p[x * 3] = r; p[x * 3 + 1] = g; p[x * 3 + 2] = b;
        break;
    case QL_IMAGE_A8:
        p[x] = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u + (uint32_t)b * 29u) >> 8);
        break;
    default:
        p[x * 4] = r; p[x * 4 + 1] = g; p[x * 4 + 2] = b; p[x * 4 + 3] = 255;
        break;
    }
}

static int fake_decode(const uint8_t *data, size_t len, uint8_t out_format, QLImage *out,
                       char *err, size_t err_cap)
{
    (void)data;
    (void)len;
    (void)out_format;
    s_fake.calls++;

    memset(out, 0, sizeof(*out));

    if (s_fake.mode == FAKE_DECLINE)
        return 0;
    if (s_fake.mode == FAKE_FAIL) {
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "%s", s_fake.err != NULL ? s_fake.err : "");
        return -1;
    }
    if (s_fake.mode == FAKE_NULL_PX)
        return 1;                       /* 什么都不给，但声称成功 */
    if (s_fake.mode == FAKE_TOO_BIG) {
        out->w = 0xFFFF;
        out->h = 0xFFFF;
        out->format = QL_IMAGE_RGBA8888;
        out->stride = 4;
        out->len = 4;
        uint8_t *px = (uint8_t *)ql_mem_alloc(4);
        out->px = px;
        out->owned = 1;
        return 1;
    }

    uint8_t fmt = (s_fake.mode == FAKE_SOLID) ? s_fake.ret_format
                                             : (uint8_t)QL_IMAGE_RGBA8888;
    if (s_fake.mode == FAKE_BAD_FORMAT)
        fmt = 99;                       /* 枚举外：validate 必须挡 */
    const uint32_t bpp = ql_image_format_bpp(fmt);
    uint16_t w = 2, h = 2;
    uint32_t stride = w * bpp;
    if (s_fake.mode == FAKE_BAD_STRIDE && bpp > 0)
        stride = w * bpp - 1;            /* 装不下一行：真的非法（行尾补齐是合法的） */
    uint32_t total = stride * h;
    if (s_fake.mode == FAKE_LEN_MISMATCH)
        total--;

    uint8_t *px = (uint8_t *)ql_mem_calloc(1, stride * h);
    if (px == NULL)
        return -1;
    /* 故意造出来的坏行宽/坏格式装不下像素，别去填（那才是越界的那一方） */
    if (bpp > 0 && stride >= (uint32_t)w * bpp) {
        for (uint16_t y = 0; y < h; y++)
            for (uint16_t x = 0; x < w; x++)
                put_solid(px, stride, fmt, x, y);
    }

    out->px = px;
    out->w = w;
    out->h = h;
    out->stride = (uint16_t)stride;
    out->len = total;
    out->format = fmt;
    out->owned = 1;
    return 1;
}

static const QLImageBackend s_fake_backend = { fake_decode, "fake" };

/* ------------------------------ 小工具 ------------------------------ */

/* 走 AUTO 探测（用真 JPEG 魔数，好确认"探测到了 JPEG 才会问后端"） */
static const uint8_t k_jpeg_magic[3] = { 0xFF, 0xD8, 0xFF };

static int decode_bytes(const uint8_t *d, size_t n, uint8_t out_format, QLImage *img)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = d;
    req.len = n;
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = out_format;
    return ql_image_decode(&req, img);
}

static void expect_rgba(const QLImage *img, int x, int y, int r, int g, int b, int a)
{
    const uint8_t *p = img->px + (size_t)y * img->stride + (size_t)x * 4u;
    QL_CHECK_EQ((int)p[0], r);
    QL_CHECK_EQ((int)p[1], g);
    QL_CHECK_EQ((int)p[2], b);
    QL_CHECK_EQ((int)p[3], a);
}

QL_TEST_BEGIN("test_image_backend")

    ql_image_set_platform_backend(NULL);
    QL_CHECK(ql_image_platform_backend() == NULL);

    /* ---------- 1. 没有后端：JPEG 明确不支持，且没有文案 ---------- */
    {
        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_UNSUPPORTED);
        QL_CHECK(ql_image_last_error() == NULL);
    }

#if !QL_FEAT_IMG_PLATFORM
    /*
     * 这个构建把平台后端整套裁掉了（QLGL_IMG_PRESET=lean / raw-only）：
     * 装了后端也不该被调用，JPEG 一律 UNSUPPORTED。
     */
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.mode = FAKE_ACCEPT;
    ql_image_set_platform_backend(&s_fake_backend);
    {
        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_UNSUPPORTED);
        QL_CHECK_EQ(s_fake.calls, 0);   /* 后端根本没被问 */
    }
    ql_image_set_platform_backend(NULL);
#else
    /* ---------- 2. 装上后端：JPEG 魔数被识别，结果来自后端 ---------- */
    {
        memset(&s_fake, 0, sizeof(s_fake));
        s_fake.mode = FAKE_ACCEPT;
        ql_image_set_platform_backend(&s_fake_backend);
        QL_CHECK(ql_image_platform_backend() == &s_fake_backend);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img), QL_OK);
        QL_CHECK_EQ(s_fake.calls, 1);
        QL_CHECK_EQ((int)img.w, 2);
        QL_CHECK_EQ((int)img.h, 2);
        QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGBA8888);
        QL_CHECK_EQ((int)img.owned, 1);
        expect_rgba(&img, 0, 0, 255, 0, 0, 255);
        expect_rgba(&img, 1, 0, 0, 255, 0, 255);
        expect_rgba(&img, 0, 1, 0, 0, 255, 255);
        expect_rgba(&img, 1, 1, 255, 255, 255, 255);
        if (img.owned)
            ql_mem_free((void *)img.px);

        /* 显式指定格式也走后端 */
        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = k_jpeg_magic;
        req.len = sizeof(k_jpeg_magic);
        req.src_format = QL_IMAGE_SRC_JPEG;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        QL_CHECK_EQ(s_fake.calls, 2);
        if (img.owned)
            ql_mem_free((void *)img.px);
    }

    /* ---------- 3. 原生解码器优先：后端不该被调用 ---------- */
    {
        /* 8 字节 PNG 签名：格式被识别成 PNG，但数据不完整 → INVAL（不是 UNSUPPORTED），
         * 所以**不会**落到后端 —— 这正是"后端抢不走原生格式"的判据。 */
        static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        memset(&s_fake, 0, sizeof(s_fake));
        s_fake.mode = FAKE_ACCEPT;
        QLImage img;
        QL_CHECK_EQ(decode_bytes(png, sizeof(png), 0, &img), QL_ERR_INVAL);
        QL_CHECK_EQ(s_fake.calls, 0);

    }

    /* ---------- 3b. 原生解码器说 UNSUPPORTED 时后端接手（隔行 PNG） ---------- */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/rgb8_quad.png", QL_PNG_ASSET_DIR);
        FILE *f = fopen(path, "rb");
        QL_CHECK(f != NULL);
        if (f != NULL) {
            static uint8_t buf[4096];
            const size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            QL_CHECK(n > 30);
            /* IHDR 的 interlace 字节在 8(签名)+8(长度+类型)+8(宽高) = 24 .. 28 */
            buf[8 + 8 + 8] = 0;         /* 宽 */
            buf[28] = 1;                /* interlace = 1（不校 CRC，故意的） */

            memset(&s_fake, 0, sizeof(s_fake));
            s_fake.mode = FAKE_ACCEPT;
            QLImage img;
            /* 没有后端时：明确不支持 */
            ql_image_set_platform_backend(NULL);
            QL_CHECK_EQ(decode_bytes(buf, n, 0, &img), QL_ERR_UNSUPPORTED);
            /* 有后端时：后端接手（这就是"后端补上原生不做的分支"） */
            ql_image_set_platform_backend(&s_fake_backend);
            QL_CHECK_EQ(decode_bytes(buf, n, 0, &img), QL_OK);
            QL_CHECK_EQ(s_fake.calls, 1);
            QL_CHECK_EQ((int)img.w, 2);          /* 假后端的 2×2 */
            if (img.owned)
                ql_mem_free((void *)img.px);
        }
    }

    /* ---------- 4. 后端返回不识别的数据（返回 0）→ UNSUPPORTED ---------- */
    {
        memset(&s_fake, 0, sizeof(s_fake));
        s_fake.mode = FAKE_DECLINE;
        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_UNSUPPORTED);
        QL_CHECK_EQ(s_fake.calls, 1);
        QL_CHECK(ql_image_last_error() == NULL);
    }

    /* ---------- 5. 后端失败：INVAL + 文案带后端名 ---------- */
    {
        memset(&s_fake, 0, sizeof(s_fake));
        s_fake.mode = FAKE_FAIL;
        s_fake.err = "bad huffman table";
        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_INVAL);
        const char *msg = ql_image_last_error();
        QL_CHECK(msg != NULL);
        if (msg != NULL) {
            QL_CHECK(strstr(msg, "fake") != NULL);        /* 后端名 */
            QL_CHECK(strstr(msg, "huffman") != NULL);     /* 后端文案 */
        }
        /* 后端没写文案时也要有兜底 */
        s_fake.err = NULL;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_INVAL);
        QL_CHECK(ql_image_last_error() != NULL);
    }

    /* ---------- 6. 后端返回值一律重新校验 ---------- */
    {
        static const struct {
            int mode;
        } k_bad[] = { { FAKE_BAD_STRIDE }, { FAKE_NULL_PX }, { FAKE_BAD_FORMAT },
                      { FAKE_TOO_BIG },    { FAKE_LEN_MISMATCH } };
        for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
            memset(&s_fake, 0, sizeof(s_fake));
            s_fake.mode = k_bad[i].mode;
            QLImage img;
            QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                        QL_ERR_INVAL);
            /* 坏结果不能被交出去 */
            QL_CHECK(img.px == NULL);
            QL_CHECK(ql_image_last_error() != NULL);
        }
    }

    /* ---------- 7. 格式收口：后端只给 RGBA8888，本库负责转换 ---------- */
    {
        memset(&s_fake, 0, sizeof(s_fake));
        s_fake.mode = FAKE_SOLID;
        s_fake.ret_format = QL_IMAGE_RGB565;   /* 后端偷懒给了 565 */

        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), QL_IMAGE_RGBA8888,
                                 &img), QL_OK);
        QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGBA8888);
        expect_rgba(&img, 0, 0, 255, 0, 0, 255);
        expect_rgba(&img, 1, 0, 0, 255, 0, 255);
        expect_rgba(&img, 0, 1, 0, 0, 255, 255);
        expect_rgba(&img, 1, 1, 255, 255, 255, 255);
        if (img.owned)
            ql_mem_free((void *)img.px);

        /* 要 A8：转换后应是灰度（红 76 / 绿 150 / 蓝 29 / 白 255 附近） */
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), QL_IMAGE_A8, &img),
                    QL_OK);
        QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_A8);
        QL_CHECK_EQ((int)img.px[0], 76);
        QL_CHECK((int)img.px[1] >= 148 && (int)img.px[1] <= 152);
        QL_CHECK((int)img.px[img.stride] >= 27 && (int)img.px[img.stride] <= 31);
        if (img.owned)
            ql_mem_free((void *)img.px);

        /* 要 RGB565：直接给的就是 565，不该再转（同格式零拷贝） */
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), QL_IMAGE_RGB565,
                                 &img), QL_OK);
        QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGB565);
        if (img.owned)
            ql_mem_free((void *)img.px);
    }

#endif  /* QL_FEAT_IMG_PLATFORM */

    /* ---------- 8. ql_image_convert 自身：四格式互转 ---------- */
    {
        /* 造一张 2×2 RGBA8888，转成 565 再转回来，颜色应在量化误差内 */
        uint8_t src[2 * 2 * 4] = {
            255, 0,   0,   255, 0,   255, 0,   255,
            0,   0,   255, 255, 255, 255, 255, 255,
        };
        QLImage a;
        memset(&a, 0, sizeof(a));
        a.px = src;
        a.w = 2;
        a.h = 2;
        a.stride = 8;
        a.len = 16;
        a.format = QL_IMAGE_RGBA8888;
        a.owned = 0;
        QL_CHECK(ql_image_validate(&a));

        QLImage b;
        QL_CHECK_EQ(ql_image_convert(&a, QL_IMAGE_RGB565, &b), QL_OK);
        QL_CHECK_EQ((int)b.format, (int)QL_IMAGE_RGB565);
        QL_CHECK_EQ((int)b.owned, 1);
        const uint16_t *q = (const uint16_t *)b.px;
        QL_CHECK_EQ((int)q[0], 0xF800);
        QL_CHECK_EQ((int)q[1], 0x07E0);
        QL_CHECK_EQ((int)q[2], 0x001F);

        QLImage c;
        QL_CHECK_EQ(ql_image_convert(&b, QL_IMAGE_RGBA8888, &c), QL_OK);
        /* 565 → 888 走整数展开，红仍是 255、绿 255、蓝 255 */
        const uint8_t *p = c.px;
        QL_CHECK_EQ((int)p[0], 255);
        QL_CHECK_EQ((int)p[1], 0);
        QL_CHECK_EQ((int)p[2], 0);
        QL_CHECK_EQ((int)p[3], 255);
        ql_mem_free((void *)b.px);
        ql_mem_free((void *)c.px);

        /* 同格式不复制 */
        QLImage same;
        QL_CHECK_EQ(ql_image_convert(&a, QL_IMAGE_RGBA8888, &same), QL_OK);
        QL_CHECK(same.px == a.px);

        /* 非法参数 */
        QLImage bad;
        QL_CHECK_EQ(ql_image_convert(NULL, QL_IMAGE_RGBA8888, &bad), QL_ERR_INVAL);
        QL_CHECK_EQ(ql_image_convert(&a, 99, &bad), QL_ERR_INVAL);
    }

    /* ---------- 9. 卸载后端后立刻失效 ---------- */
    {
        ql_image_set_platform_backend(NULL);
        QLImage img;
        QL_CHECK_EQ(decode_bytes(k_jpeg_magic, sizeof(k_jpeg_magic), 0, &img),
                    QL_ERR_UNSUPPORTED);
    }

QL_TEST_END()
