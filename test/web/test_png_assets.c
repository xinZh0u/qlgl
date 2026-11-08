/**
 * @file    test_png_assets.c
 * @brief   PNG 对拍：`test/assets/png` 里的样例用自家解码器解，逐字节比对
 *          Pillow 解出来的 RGBA8888 基准（`*.rgba`）。
 *
 * 为什么需要这一层（`test_png` 已经覆盖了容器与位深）：
 *   `test_png` 的输入是"stored（未压缩）deflate"，测的是容器与反过滤；
 *   真实 PNG 的 IDAT 是**动态 Huffman + 回溯拷贝**，那部分只能拿真实
 *   压缩流来验。基准由 `tools/gen_png_assets.py` 用 Pillow 生成并提交。
 *
 * 覆盖的颜色类型：0（位深 1/8/16）、2、3（位深 4）、6。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#ifndef QL_PNG_ASSET_DIR
#define QL_PNG_ASSET_DIR "test/assets/png"
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

QL_TEST_BEGIN("test_png_assets")

#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE
    static const char *const k_cases[] = {
        "rgba8_gradient", "rgb8_quad",  "gray8_ramp", "bw1_checker",
        "pal4_blocks",    "gray16_ramp", "rgba8_sprite",
    };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        char ppath[512], rpath[512];
        snprintf(ppath, sizeof(ppath), "%s/%s.png", QL_PNG_ASSET_DIR, k_cases[i]);
        snprintf(rpath, sizeof(rpath), "%s/%s.rgba", QL_PNG_ASSET_DIR, k_cases[i]);

        size_t png_len = 0, ref_len = 0;
        uint8_t *png = read_file(ppath, &png_len);
        uint8_t *ref = read_file(rpath, &ref_len);
        QL_CHECK(png != NULL);
        QL_CHECK(ref != NULL);
        if (png == NULL || ref == NULL) {
            free(png);
            free(ref);
            continue;
        }

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = png;
        req.len = png_len;
        req.src_format = QL_IMAGE_SRC_AUTO;   /* 按魔数认出来是 PNG */
        req.out_format = QL_IMAGE_RGBA8888;
        QLImage img;
        const int rc = ql_image_decode(&req, &img);
        QL_CHECK_EQ(rc, QL_OK);
        if (rc == QL_OK) {
            QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGBA8888);
            QL_CHECK_EQ((size_t)img.len, ref_len);
            if ((size_t)img.len == ref_len) {
                /* dst_alloc 的 stride 就是 w*4，所以能直接整块比 */
                int bad = -1;
                for (size_t k = 0; k < ref_len; k++) {
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
        free(png);
        free(ref);
    }
#else
    /* 这个构建把 PNG 裁了：同一份字节必须明确说"不支持" */
    {
        char ppath[512];
        snprintf(ppath, sizeof(ppath), "%s/%s.png", QL_PNG_ASSET_DIR, "rgb8_quad");
        size_t len = 0;
        uint8_t *png = read_file(ppath, &len);
        QL_CHECK(png != NULL);
        if (png != NULL) {
            QLImageDecodeReq req;
            memset(&req, 0, sizeof(req));
            req.data = png;
            req.len = len;
            req.src_format = QL_IMAGE_SRC_AUTO;
            QLImage img;
            QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
            free(png);
        }
    }
#endif

QL_TEST_END()
