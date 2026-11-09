/**
 * @file    test_gif_assets.c
 * @brief   GIF 对拍：`test/assets/img` 里的 GIF 用自家解码器解，逐字节比对 Pillow 的
 *          RGBA8888 基准（`*.rgba`）。
 *
 * `test_gif` 用现场构造的文件覆盖 LZW/隔行/透明的**逻辑**；
 * 这一层用 Pillow 写出的真实文件覆盖"别人的编码器会怎么写"
 * （Pillow 默认写**隔行** GIF、8 色调色板、`tRNS` 用图形控制扩展）。
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

QL_TEST_BEGIN("test_gif_assets")

#if QL_FEAT_IMG_GIF
    static const char *const k_cases[] = { "quad.gif", "trans.gif" };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        char gpath[512], rpath[512];
        snprintf(gpath, sizeof(gpath), "%s/%s", QL_IMG_ASSET_DIR, k_cases[i]);
        snprintf(rpath, sizeof(rpath), "%s/%s.rgba", QL_IMG_ASSET_DIR, k_cases[i]);

        size_t glen = 0, rlen = 0;
        uint8_t *gif = read_file(gpath, &glen);
        uint8_t *ref = read_file(rpath, &rlen);
        QL_CHECK(gif != NULL);
        QL_CHECK(ref != NULL);
        if (gif == NULL || ref == NULL) {
            free(gif);
            free(ref);
            continue;
        }

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = gif;
        req.len = glen;
        req.src_format = QL_IMAGE_SRC_AUTO;      /* 按魔数认出 GIF8 */
        req.out_format = QL_IMAGE_RGBA8888;
        QLImage img;
        const int rc = ql_image_decode(&req, &img);
        QL_CHECK_EQ(rc, QL_OK);
        if (rc == QL_OK) {
            QL_CHECK_EQ((size_t)img.len, rlen);
            if ((size_t)img.len == rlen) {
                int bad = -1;
                for (size_t k = 0; k < rlen; k++) {
                    if (img.px[k] != ref[k]) {
                        bad = (int)k;
                        break;
                    }
                }
                if (bad >= 0)
                    printf("  [差异] %s 第 %d 字节：%u != %u\n", k_cases[i], bad,
                           (unsigned)img.px[bad], (unsigned)ref[bad]);
                QL_CHECK(bad < 0);
            }
            if (img.owned)
                ql_mem_free((void *)img.px);
        }
        free(gif);
        free(ref);
    }
#else
    {
        static const uint8_t sig[6] = { 'G', 'I', 'F', '8', '9', 'a' };
        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = sig;
        req.len = sizeof(sig);
        req.src_format = QL_IMAGE_SRC_AUTO;
        QLImage img;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
    }
#endif

QL_TEST_END()
