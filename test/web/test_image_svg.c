/**
 * @file    test_image_svg.c
 * @brief   运行期 SVG 解码（PlutoSVG）：尺寸、像素、A8 遮罩与拒绝路径。
 *
 * 只断言"结构上确定"的东西（尺寸/透明度/明确失败），不比对逐像素 golden：
 * 抗锯齿细节由 PlutoVG 决定，跨版本可能微调。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

static const char kRects[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\">"
    "<rect width=\"20\" height=\"20\" fill=\"#ff0000\"/>"
    "<circle cx=\"10\" cy=\"10\" r=\"5\" fill=\"#0000ff\"/>"
    "</svg>";

#if QL_FEAT_IMG_SVG
/* viewBox 决定固有尺寸（没有 width/height）；透明背景 + 黑色星形 */
static const char kStar[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M12 2 L15 9 L22 9 L16.5 13.5 L18.5 21 L12 16.5 "
    "L5.5 21 L7.5 13.5 L2 9 L9 9 Z\" fill=\"#000000\"/>"
    "</svg>";

#endif

static int decode_text(const char *svg, uint8_t out_format, uint16_t tw, uint16_t th,
                       QLImage *out)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = (const uint8_t *)svg;
    req.len = strlen(svg);
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = out_format;
    req.target_w = tw;
    req.target_h = th;
    return ql_image_decode(&req, out);
}

#if QL_FEAT_IMG_SVG
static void check_rgba(const QLImage *img, int x, int y, uint8_t r, uint8_t g,
                       uint8_t b, uint8_t a)
{
    const uint8_t *p = img->px + (size_t)y * img->stride + (size_t)x * 4u;
    QL_CHECK_EQ(p[0], r);
    QL_CHECK_EQ(p[1], g);
    QL_CHECK_EQ(p[2], b);
    QL_CHECK_EQ(p[3], a);
}
#endif

QL_TEST_BEGIN("test_image_svg")
    QLImage img;

#if QL_FEAT_IMG_SVG
    /* ---- 1. AUTO 探测 + 固有尺寸 + 非预乘 RGBA ---- */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text(kRects, QL_IMAGE_RGBA8888, 0, 0, &img), QL_OK);
    QL_CHECK_EQ(img.w, 20);
    QL_CHECK_EQ(img.h, 20);
    QL_CHECK_EQ(img.format, QL_IMAGE_RGBA8888);
    QL_CHECK_EQ(img.owned, 1);
    check_rgba(&img, 1, 1, 255, 0, 0, 255);       /* 红底 */
    check_rgba(&img, 10, 10, 0, 0, 255, 255);    /* 蓝圆盖住中心 */
    ql_mem_free((void *)img.px);

    /* ---- 2. 目标尺寸（只给宽 → 按比例）---- */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text(kRects, QL_IMAGE_RGBA8888, 64, 0, &img), QL_OK);
    QL_CHECK_EQ(img.w, 64);
    QL_CHECK_EQ(img.h, 64);
    ql_mem_free((void *)img.px);

    /* ---- 3. A8 = 覆盖率（不是黑底图形的亮度）---- */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text(kStar, QL_IMAGE_A8, 24, 24, &img), QL_OK);
    QL_CHECK_EQ(img.w, 24);
    QL_CHECK_EQ(img.h, 24);
    QL_CHECK_EQ(img.format, QL_IMAGE_A8);
    QL_CHECK_EQ(img.len, 24u * 24u);
    QL_CHECK_EQ(img.px[0], 0);                   /* 左上角在星形外 */
    QL_CHECK(img.px[12 * 24 + 12] > 128);        /* 中心不透明 */
    {
        unsigned long nz = 0;
        for (uint32_t i = 0; i < img.len; i++)
            if (img.px[i] != 0)
                nz++;
        QL_CHECK(nz > img.len / 10u);
        QL_CHECK(nz < img.len * 9u / 10u);       /* 不能整张全白 */
    }
    ql_mem_free((void *)img.px);

    /* ---- 4. 预算：surface 是 4 B/px，超预算要明确失败 ---- */
    ql_image_set_decode_budget(16u * 16u);
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text(kRects, QL_IMAGE_RGBA8888, 64, 64, &img), QL_ERR_NOMEM);
    QL_CHECK(ql_image_last_error() != NULL);
    ql_image_set_decode_budget(0);

    /* ---- 5. 拒绝：外链 / 脚本 / 畸形 ---- */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\""
                            " height=\"8\"><image href=\"https://x/y.png\"/></svg>",
                            QL_IMAGE_RGBA8888, 0, 0, &img), QL_ERR_INVAL);
    QL_CHECK(strstr(ql_image_last_error(), "禁止") != NULL);

    /* 畸形输入：PlutoSVG 直接失败；ThorVG 可能"成功"但画不出任何像素。
     * 两种都算"明确没有画面"，不允许崩或给出非空图像。 */
    memset(&img, 0, sizeof(img));
    {
        const int brc = decode_text("<svg", QL_IMAGE_RGBA8888, 0, 0, &img);
        if (brc != QL_OK) {
            QL_CHECK(1);
        } else {
            uint32_t nz = 0;
            for (uint32_t i = 3; i < img.len; i += 4u)
                if (img.px[i] != 0)
                    nz++;
            QL_CHECK(nz == 0);
            ql_mem_free((void *)img.px);
        }
    }
    ql_image_set_decode_budget(0);

    /* ---- 6. 显式 SRC_SVG 也能走同一条路 ---- */
    {
        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = (const uint8_t *)kRects;
        req.len = strlen(kRects);
        req.src_format = QL_IMAGE_SRC_SVG;
        memset(&img, 0, sizeof(img));
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        QL_CHECK_EQ(img.w, 20);
        ql_mem_free((void *)img.px);
    }
#else
    /* 关掉 SVG 时必须**明确失败**，不能把它当未知格式画成空白 */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text(kRects, QL_IMAGE_RGBA8888, 0, 0, &img),
                QL_ERR_UNSUPPORTED);
    QL_CHECK(ql_image_last_error() != NULL);
#endif

    /* 非 SVG 文本不能被误判 */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode_text("<?xml version=\"1.0\"?><root/>", QL_IMAGE_RGBA8888,
                            0, 0, &img), QL_ERR_UNSUPPORTED);
QL_TEST_END()
