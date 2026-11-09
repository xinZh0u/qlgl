/**
 * @file    test_jpeg.c
 * @brief   JPEG 走平台后端：libjpeg 参考实现 + 与 Pillow 的**容差**比对。
 *
 * JPEG 是有损的，两个不同的 IDCT 实现结果会差 1~2 个色阶，所以这里不比逐字节，
 * 而是要求"最大差 ≤ 3、平均差 < 1" —— 足以抓出色偏/通道错位这类真 bug，
 * 又不会因为不同 SIMD 路径的舍入而假报。
 *
 * 这个构建没有 libjpeg 时（`QL_FEAT_IMG_LIBJPEG == 0`），断言的是另一件事：
 * **JPEG 明确失败**，不会悄悄画出一张灰图。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#ifndef QL_IMG_ASSET_DIR
#define QL_IMG_ASSET_DIR "test/assets/img"
#endif

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int decode_jpeg(const uint8_t *d, size_t n, uint8_t out_format, QLImage *img)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = d;
    req.len = n;
    req.src_format = QL_IMAGE_SRC_AUTO;      /* 按 FFD8FF 认出 JPEG */
    req.out_format = out_format;
    return ql_image_decode(&req, img);
}

QL_TEST_BEGIN("test_jpeg")

#if QL_FEAT_IMG_LIBJPEG
    const QLImageBackend *be = ql_image_backend_libjpeg();
    QL_CHECK(be != NULL);
    if (be != NULL)
        QL_CHECK(be->name != NULL && strcmp(be->name, "libjpeg") == 0);
    ql_image_set_platform_backend(be);   /* NULL 也能装，等于"只有自有解码器" */

    static const char *const k_cases[] = { "quad.jpg", "gray.jpg" };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        char jpath[512], rpath[512];
        snprintf(jpath, sizeof(jpath), "%s/%s", QL_IMG_ASSET_DIR, k_cases[i]);
        snprintf(rpath, sizeof(rpath), "%s/%s.rgba", QL_IMG_ASSET_DIR, k_cases[i]);

        size_t jlen = 0, rlen = 0;
        uint8_t *jpg = read_file(jpath, &jlen);
        uint8_t *ref = read_file(rpath, &rlen);
        QL_CHECK(jpg != NULL);
        QL_CHECK(ref != NULL);
        if (jpg == NULL || ref == NULL) {
            free(jpg);
            free(ref);
            continue;
        }

        QLImage img;
        const int rc = decode_jpeg(jpg, jlen, QL_IMAGE_RGBA8888, &img);
        QL_CHECK_EQ(rc, QL_OK);
        if (rc == QL_OK) {
            QL_CHECK_EQ((size_t)img.len, rlen);
            if ((size_t)img.len == rlen) {
                int maxdiff = 0;
                long sum = 0;
                for (size_t k = 0; k < rlen; k++) {
                    if ((k & 3u) == 3u)          /* alpha：两边都必须是 255 */
                        continue;
                    const int d = (int)img.px[k] - (int)ref[k];
                    const int ad = (d < 0) ? -d : d;
                    if (ad > maxdiff)
                        maxdiff = ad;
                    sum += ad;
                }
                const long ncomp = (long)(rlen / 4u) * 3L;
                const double avg = (ncomp > 0) ? (double)sum / (double)ncomp : 0.0;
                if (maxdiff > 3)
                    printf("  [差异] %s 最大色差 %d（平均 %.3f）\n", k_cases[i], maxdiff,
                           avg);
                QL_CHECK(maxdiff <= 3);
                QL_CHECK(avg < 1.0);
                /* alpha 必须是全 255（libjpeg 的 RGBA 输出固定不透明） */
                int alpha_ok = 1;
                for (size_t k = 3; k < rlen; k += 4)
                    if (img.px[k] != 255)
                        alpha_ok = 0;
                QL_CHECK(alpha_ok);
            }
            if (img.owned)
                ql_mem_free((void *)img.px);
        }
        free(jpg);
        free(ref);
    }

    /* 转成 RGB565 也要能走同一条路（后端给 RGBA，本库负责转换） */
    {
        char jpath[512];
        snprintf(jpath, sizeof(jpath), "%s/quad.jpg", QL_IMG_ASSET_DIR);
        size_t jlen = 0;
        uint8_t *jpg = read_file(jpath, &jlen);
        QL_CHECK(jpg != NULL);
        if (jpg != NULL) {
            QLImage img;
            QL_CHECK_EQ(decode_jpeg(jpg, jlen, QL_IMAGE_RGB565, &img), QL_OK);
            if (img.px != NULL) {
                QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGB565);
                QL_CHECK_EQ((int)img.stride, (int)img.w * 2);
                /* 左上角是红块 (220,30,40) → 565 的 0xD8xx（高 5 位 27） */
                const uint16_t v = (uint16_t)(img.px[0] | (img.px[1] << 8));
                QL_CHECK((v >> 11) >= 0x1A && (v >> 11) <= 0x1C);
                QL_CHECK(((v >> 5) & 0x3F) < 12);
                if (img.owned)
                    ql_mem_free((void *)img.px);
            }
            free(jpg);
        }
    }

    /* 坏数据：必须是"返回失败"，不能崩（libjpeg 默认的 error_exit 是 exit(1)） */
    {
        char jpath[512];
        snprintf(jpath, sizeof(jpath), "%s/quad.jpg", QL_IMG_ASSET_DIR);
        size_t jlen = 0;
        uint8_t *jpg = read_file(jpath, &jlen);
        QL_CHECK(jpg != NULL);
        if (jpg != NULL) {
            QLImage img;
            /* 截断到一半：**必须失败**（libjpeg 会把缺的行填灰，那是半个正确） */
            const int rc_cut = decode_jpeg(jpg, jlen / 2u, 0, &img);
            QL_CHECK(rc_cut != QL_OK);
            if (rc_cut == QL_OK && img.owned)
                ql_mem_free((void *)img.px);
            QL_CHECK(ql_image_last_error() != NULL);
            /* 完整签名 + 垃圾 */
            uint8_t junk[64];
            memset(junk, 0xA5, sizeof(junk));
            junk[0] = 0xFF; junk[1] = 0xD8; junk[2] = 0xFF;
            {
                const int rc_junk = decode_jpeg(junk, sizeof(junk), 0, &img);
                QL_CHECK(rc_junk != QL_OK);
                if (rc_junk == QL_OK && img.owned)
                    ql_mem_free((void *)img.px);
            }
            /* 每个字节翻一位（随机几处）都不能崩 */
            for (int it = 0; it < 40; it++) {
                uint8_t *copy = (uint8_t *)malloc(jlen);
                QL_CHECK(copy != NULL);
                memcpy(copy, jpg, jlen);
                for (int k = 0; k < 4; k++)
                    copy[((size_t)it * 37u + (size_t)k * 11u) % jlen] ^= (uint8_t)(1u << (unsigned)(k & 7));
                const int rc = decode_jpeg(copy, jlen, 0, &img);
                if (rc == QL_OK && img.owned)
                    ql_mem_free((void *)img.px);
                QL_CHECK(rc == QL_OK || rc < 0);
                free(copy);
            }
            free(jpg);
        }
    }

    ql_image_set_platform_backend(NULL);
#else
    /* 这个构建没有 libjpeg：JPEG 必须明确失败 */
    {
        char jpath[512];
        snprintf(jpath, sizeof(jpath), "%s/quad.jpg", QL_IMG_ASSET_DIR);
        size_t jlen = 0;
        uint8_t *jpg = read_file(jpath, &jlen);
        QL_CHECK(jpg != NULL);
        if (jpg != NULL) {
            QLImage img;
            ql_image_set_platform_backend(NULL);
            QL_CHECK_EQ(decode_jpeg(jpg, jlen, 0, &img), QL_ERR_UNSUPPORTED);
            QL_CHECK(img.px == NULL);
            free(jpg);
        }
    }
#endif

QL_TEST_END()
