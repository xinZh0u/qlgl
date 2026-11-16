/**
 * @file    test_image_svg_thorvg.c
 * @brief   ThorVG SVG 后端：与 PlutoSVG 后端对齐的像素/尺寸/currentColor。
 *
 * 没有 ThorVG 的构建里只打印 SKIP（0 项检查），不注册失败。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#if QL_FEAT_IMG_SVG_THORVG
static const char kRects[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\">"
    "<rect width=\"20\" height=\"20\" fill=\"#ff0000\"/>"
    "<circle cx=\"10\" cy=\"10\" r=\"5\" fill=\"#0000ff\"/>"
    "</svg>";

static const char kStar[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M12 2 L15 9 L22 9 L16.5 13.5 L18.5 21 L12 16.5 "
    "L5.5 21 L7.5 13.5 L2 9 L9 9 Z\" fill=\"#000000\"/>"
    "</svg>";

static const char kColor[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">"
    "<rect width=\"24\" height=\"24\" fill=\"currentColor\"/></svg>";

static const char kText[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"32\">"
    "<text x=\"2\" y=\"24\" font-family=\"DejaVu Sans\" font-size=\"20\""
    " fill=\"#000000\">Hi</text></svg>";

/* 内嵌图片（ThorVG 全量 loader）：读测试资产并确认画出了像素 */
static void check_embedded(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", QL_SVG_ASSET_DIR, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  [SKIP] 缺少 %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc((size_t)n);
    if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        QL_CHECK(0);
        return;
    }
    fclose(f);

    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = (const uint8_t *)data;
    req.len = (size_t)n;
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = QL_IMAGE_RGBA8888;
    req.target_w = 32;
    req.target_h = 0;
    QLImage img;
    memset(&img, 0, sizeof(img));
    const QLErr rc = ql_image_decode(&req, &img);
    QL_CHECK_EQ(rc, QL_OK);
    if (rc == QL_OK) {
        uint32_t nz = 0;
        for (uint32_t i = 3; i < img.len; i += 4u)
            if (img.px[i] != 0)
                nz++;
        QL_CHECK(nz > 0);
        ql_mem_free((void *)img.px);
    }
    free(data);
}

static int decode(const char *svg, uint8_t fmt, uint16_t tw, uint16_t th,
                  uint32_t current, QLImage *out)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = (const uint8_t *)svg;
    req.len = strlen(svg);
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = fmt;
    req.target_w = tw;
    req.target_h = th;
    req.current_color = current;
    return ql_image_decode(&req, out);
}
#endif

QL_TEST_BEGIN("test_image_svg_thorvg")
{
#if QL_FEAT_IMG_SVG_THORVG
    QL_CHECK_EQ(ql_image_svg_set_backend("thorvg"), QL_OK);
    QL_CHECK(strcmp(ql_image_svg_backend_name(), "thorvg") == 0);

    /* 颜色 + 默认尺寸 */
    QLImage img;
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode(kRects, QL_IMAGE_RGBA8888, 0, 0, 0, &img), QL_OK);
    QL_CHECK_EQ(img.w, 20);
    QL_CHECK_EQ(img.h, 20);
    if (img.px != NULL && img.w == 20) {
        const uint8_t *red = img.px + 1u * img.stride + 1u * 4u;
        const uint8_t *blue = img.px + 10u * img.stride + 10u * 4u;
        QL_CHECK(red[0] > 0xC0 && red[1] < 0x40 && red[2] < 0x40 && red[3] > 0xC0);
        QL_CHECK(blue[0] < 0x40 && blue[1] < 0x40 && blue[2] > 0xC0 && blue[3] > 0xC0);
    }
    if (img.px != NULL)
        ql_mem_free((void *)img.px);

    /* 目标尺寸（只给宽，按比例） */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode(kRects, QL_IMAGE_RGBA8888, 64, 0, 0, &img), QL_OK);
    QL_CHECK_EQ(img.w, 64);
    QL_CHECK_EQ(img.h, 64);
    if (img.px != NULL)
        ql_mem_free((void *)img.px);

    /* A8 覆盖率 */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode(kStar, QL_IMAGE_A8, 24, 24, 0, &img), QL_OK);
    QL_CHECK_EQ(img.w, 24);
    QL_CHECK_EQ(img.h, 24);
    QL_CHECK_EQ(img.format, QL_IMAGE_A8);
    if (img.px != NULL && img.w == 24) {
        QL_CHECK_EQ(img.px[0], 0);
        QL_CHECK(img.px[12 * 24 + 12] > 128);
    }
    if (img.px != NULL)
        ql_mem_free((void *)img.px);

    /* currentColor（ThorVG 后端在加载前替换成具体颜色） */
    memset(&img, 0, sizeof(img));
    QL_CHECK_EQ(decode(kColor, QL_IMAGE_RGBA8888, 24, 24, 0x00FF00u, &img), QL_OK);
    if (img.px != NULL && img.w == 24) {
        const uint8_t *p = img.px + 12 * img.stride + 12 * 4;
        QL_CHECK(p[0] < 0x40 && p[1] > 0xC0 && p[2] < 0x40 && p[3] > 0xC0);
    }
    if (img.px != NULL)
        ql_mem_free((void *)img.px);

    /*
     * 字体 loader（sfnt + ttf/otf）：加载系统字体后 <text> 应能画出像素。
     * 没装 DejaVu 的机器跳过这一小段（不把系统字体当成测试前提）。
     */
    {
        const char *font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        FILE *f = fopen(font, "rb");
        if (f != NULL) {
            fclose(f);
            QL_CHECK_EQ(ql_image_svg_load_font(font), QL_OK);
            memset(&img, 0, sizeof(img));
            QL_CHECK_EQ(decode(kText, QL_IMAGE_RGBA8888, 80, 32, 0, &img), QL_OK);
            if (img.px != NULL) {
                uint32_t nz = 0;
                for (uint32_t i = 3; i < img.len; i += 4u)
                    if (img.px[i] != 0)
                        nz++;
                QL_CHECK(nz > 0);
                ql_mem_free((void *)img.px);
            }
        } else {
            printf("  [SKIP] 没有 DejaVuSans.ttf，跳过 <text> 用例\n");
        }
    }

    /* 内嵌 PNG / JPEG / WebP：ThorVG static loader */
    check_embedded("embedded_png.svg");
    check_embedded("embedded_jpg.svg");
    check_embedded("embedded_webp.svg");

    QL_CHECK_EQ(ql_image_svg_set_backend(NULL), QL_OK);
#else
    printf("  [SKIP] 本构建没有 ThorVG 后端\n");
#endif
}
QL_TEST_END()
