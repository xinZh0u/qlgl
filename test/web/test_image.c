/**
 * @file    test_image.c
 * @brief   `<img>`：资源注册、固有尺寸、blit、有界校验。
 *
 * 这一层最重要的不是"能画出图"，而是**不能画的部分要明确失败**：
 * 名字对不上 → INVAL，格式没编进来 → UNSUPPORTED，尺寸越界 → INVAL。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 64
#define H 64

static QLDispDriver *s_disp;
static QLCanvas s_cv;

/* 8×8 RGB565：左上红 / 右上绿 / 左下蓝 / 右下白 */
static uint16_t s_raw[8 * 8];

static void fill_raw(void)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const int left = x < 4, top = y < 4;
            s_raw[y * 8 + x] =
                (uint16_t)(top ? (left ? 0xF800 : 0x07E0) : (left ? 0x001F : 0xFFFF));
        }
}

static QLUIRoot *mk(const char *html, const char *css, QLUIDoc **out_doc)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, W, H);
    *out_doc = doc;
    return ui;
}

static QLUIImageInput raw_input(const char *name)
{
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = name;
    in.data = (const uint8_t *)s_raw;
    in.len = sizeof(s_raw);
    in.w = 8;
    in.h = 8;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_RGB565;
    return in;
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, (ql_coord_t)x, (ql_coord_t)y);
}

/* 热区 / 点击回调的计数器（验证"热区优先，不重复派发"） */
static int s_area_hits;
static int s_click_hits;

static void on_area(QLUIRoot *ui, int index, void *user)
{
    (void)ui;
    (void)user;
    if (index == 0)
        s_area_hits++;
}

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    s_click_hits++;
}

QL_TEST_BEGIN("test_image")

    fill_raw();
    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. 解析：src 存进 node->text，但不显示 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"logo.png\"></div>",
                          "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIDoc *d = doc;
            QL_CHECK_EQ((int)d->node_count, 2);
            QL_CHECK(strcmp(ql_ui_tag_name(d->nodes[1].tag), "img") == 0);
            QL_CHECK_STR(ui->nodes[1].text, "logo.png");       /* 资源名 */
            QL_CHECK_STR(ql_ui_node_text(&ui->nodes[1]), "");  /* 不是文字 */
            QL_CHECK_EQ((int)ui->image_count, 1);
            /* 没注册资源：拿不到像素，但池子在 */
            QL_CHECK(ui->images != NULL);
            QL_CHECK(ql_ui_node_image(ui, &ui->nodes[1]) == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 注册 RAW：固有尺寸 + 像素上屏 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>",
                          "#r { width: 64px; height: 64px; background-color: #000000 }",
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 没 CSS 尺寸时按固有尺寸（8×8）排 */
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).w, 0);

            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            QL_CHECK(ql_ui_node_image(ui, &ui->nodes[1]) != NULL);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).w, 8);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).h, 8);

            /* 渲染：四象限颜色（每个象限中心点） */
            ql_canvas_from_disp(&s_cv, s_disp);
            ql_canvas_clear(&s_cv, 0x000000);
            ql_ui_render_canvas(ui, &s_cv);
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            QL_CHECK_EQ((int)QL_COLOR_GET_R(px(r.x + 2, r.y + 2)), 0xFF);   /* 红 */
            QL_CHECK_EQ((int)QL_COLOR_GET_G(px(r.x + 2, r.y + 2)), 0x00);
            QL_CHECK_EQ((int)QL_COLOR_GET_G(px(r.x + 6, r.y + 2)), 0xFF);   /* 绿 */
            QL_CHECK_EQ((int)QL_COLOR_GET_B(px(r.x + 2, r.y + 6)), 0xFF);   /* 蓝 */
            QL_CHECK_EQ((int)QL_COLOR_GET_R(px(r.x + 6, r.y + 6)), 0xFF);   /* 白 */
            QL_CHECK_EQ((int)QL_COLOR_GET_B(px(r.x + 6, r.y + 6)), 0xFF);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. CSS 宽度 + 自动高度 = 按宽高比 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>",
                          "#r { width: 64px } img { width: 32px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            QL_CHECK_EQ((int)r.w, 32);
            QL_CHECK_EQ((int)r.h, 32);       /* 8:8 等比 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 缩放：放大到 32×32 仍是四象限 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>",
                          "#r { width: 64px; height: 64px; background-color: #000000 }"
                          "img { width: 32px; height: 32px }",
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            ql_canvas_from_disp(&s_cv, s_disp);
            ql_canvas_clear(&s_cv, 0x000000);
            ql_ui_render_canvas(ui, &s_cv);
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            QL_CHECK_EQ((int)r.w, 32);
            QL_CHECK_EQ((int)QL_COLOR_GET_R(px(r.x + 8, r.y + 8)), 0xFF);   /* 红 */
            QL_CHECK_EQ((int)QL_COLOR_GET_G(px(r.x + 24, r.y + 8)), 0xFF);  /* 绿 */
            QL_CHECK_EQ((int)QL_COLOR_GET_B(px(r.x + 8, r.y + 24)), 0xFF);  /* 蓝 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 明确失败：名字/格式/尺寸 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>", "", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("nope");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_ERR_INVAL);

            /*
             * 只有 8 字节签名的 PNG 残片：格式认出来了（不是 UNSUPPORTED），
             * 但内容不完整 → INVAL。PNG 解码器本身由 test_png 覆盖。
             */
            static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
            QLUIImageInput p;
            memset(&p, 0, sizeof(p));
            p.name = "pic";
            p.data = png;
            p.len = sizeof(png);
            p.format = QL_IMAGE_SRC_AUTO;
#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE
            QL_CHECK_EQ(ql_ui_register_image(ui, &p), QL_ERR_INVAL);
#else
            QL_CHECK_EQ(ql_ui_register_image(ui, &p), QL_ERR_UNSUPPORTED);
#endif
            /* 失败之后不假装有图 */
            QL_CHECK(ql_ui_node_image(ui, &ui->nodes[1]) == NULL);

            /* 声明的尺寸大于实际字节数：不猜，直接拒绝 */
            QLUIImageInput bad = raw_input("pic");
            bad.w = 64;
            bad.h = 64;
            QL_CHECK_EQ(ql_ui_register_image(ui, &bad), QL_ERR_INVAL);

            /* 指针参数非法 */
            QL_CHECK_EQ(ql_ui_register_image(ui, NULL), QL_ERR_INVAL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 没有 <img> 时池为 NULL ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><span>x</span></div>", "", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ui->images == NULL);
            QL_CHECK_EQ((int)ui->image_count, 0);
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_ERR_INVAL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 有界校验：溢出与不自洽直接拒绝 ---------- */
    {
        QLImage ok = { (const uint8_t *)s_raw, sizeof(s_raw), 8, 8, 16, QL_IMAGE_RGB565, 0 };
        QL_CHECK_EQ(ql_image_validate(&ok), 1);

        /* len 与 stride*h 不一致 */
        QLImage bad = ok;
        bad.len = 5;
        QL_CHECK_EQ(ql_image_validate(&bad), 0);

        /* 尺寸大到让 w*h 溢出 32 位：**先除后乘**必须挡住 */
        QLImage huge = ok;
        huge.w = 60000;
        huge.h = 60000;
        QL_CHECK_EQ(ql_image_validate(&huge), 0);

        /* stride 装不进 uint16 */
        QLImage wide = ok;
        wide.w = 30000;
        wide.stride = 0xFFFF;
        QL_CHECK_EQ(ql_image_validate(&wide), 0);

        /* 未知像素格式 */
        QLImage unknown = ok;
        unknown.format = 9;
        QL_CHECK_EQ(ql_image_validate(&unknown), 0);
        QL_CHECK_EQ((int)ql_image_format_bpp(9), 0);
        QL_CHECK_EQ((int)ql_image_format_bpp(QL_IMAGE_RGBA8888), 4);

        /* 空图 / 零尺寸 */
        QLImage empty = ok;
        empty.px = NULL;
        QL_CHECK_EQ(ql_image_validate(&empty), 0);
        empty = ok;
        empty.h = 0;
        QL_CHECK_EQ(ql_image_validate(&empty), 0);

        /* 合法输入的格式与字节数 */
        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = (const uint8_t *)s_raw;
        req.len = sizeof(s_raw);
        req.src_format = QL_IMAGE_SRC_RAW;
        req.pixel_format = QL_IMAGE_RGB565;
        req.w = 8;
        req.h = 8;
        QLImage out;
        QL_CHECK_EQ(ql_image_decode(&req, &out), QL_OK);
        QL_CHECK_EQ(out.owned, 0);                        /* 零 RAM：直接指过去 */
        QL_CHECK(out.px == (const uint8_t *)s_raw);
        QL_CHECK_EQ((int)out.stride, 16);
        QL_CHECK_EQ((int)out.len, 128);
    }

    /* ---------- 8. BMP 解码（24 位、自下而上、行补齐） ---------- */
    {
        /* 2×2：左上红 右上绿 左下蓝 右下白；行 8 字节对齐 */
        static uint8_t bmp[54 + 2 * 8];
        memset(bmp, 0, sizeof(bmp));
        bmp[0] = 'B'; bmp[1] = 'M';
        uint32_t v32 = sizeof(bmp);
        memcpy(bmp + 2, &v32, 4);
        v32 = 54;
        memcpy(bmp + 10, &v32, 4);
        v32 = 40;
        memcpy(bmp + 14, &v32, 4);
        int32_t i32 = 2;
        memcpy(bmp + 18, &i32, 4);
        memcpy(bmp + 22, &i32, 4);
        uint16_t v16 = 1;
        memcpy(bmp + 26, &v16, 2);
        v16 = 24;
        memcpy(bmp + 28, &v16, 2);
        uint8_t *r0 = bmp + 54;        /* 最下面一行：蓝 白 */
        uint8_t *r1 = bmp + 54 + 8;    /* 上面一行：红 绿 */
        r0[0] = 0xFF; r0[3] = 0xFF; r0[4] = 0xFF; r0[5] = 0xFF;
        r1[2] = 0xFF; r1[4] = 0xFF;

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = bmp;
        req.len = sizeof(bmp);
        req.src_format = QL_IMAGE_SRC_AUTO;
        QLImage img;
#if QL_FEAT_IMG_BMP
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        QL_CHECK_EQ((int)img.w, 2);
        QL_CHECK_EQ((int)img.h, 2);
        QL_CHECK_EQ((int)img.owned, 1);
        const uint8_t *p0 = img.px;                        /* (0,0) 红 */
        const uint8_t *p1 = img.px + 4;                    /* (1,0) 绿 */
        const uint8_t *p2 = img.px + img.stride;           /* (0,1) 蓝 */
        const uint8_t *p3 = img.px + img.stride + 4;       /* (1,1) 白 */
        QL_CHECK_EQ((int)p0[0], 0xFF); QL_CHECK_EQ((int)p0[1], 0x00);
        QL_CHECK_EQ((int)p1[1], 0xFF); QL_CHECK_EQ((int)p1[0], 0x00);
        QL_CHECK_EQ((int)p2[2], 0xFF); QL_CHECK_EQ((int)p2[0], 0x00);
        QL_CHECK_EQ((int)p3[0], 0xFF); QL_CHECK_EQ((int)p3[2], 0xFF);
        ql_mem_free((void *)img.px);

        /* 输出 RGB565：红 F800 / 绿 07E0 / 蓝 001F / 白 FFFF */
        req.out_format = QL_IMAGE_RGB565;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGB565);
        const uint16_t *q = (const uint16_t *)img.px;
        QL_CHECK_EQ((int)q[0], 0xF800);
        QL_CHECK_EQ((int)q[1], 0x07E0);
        QL_CHECK_EQ((int)q[2], 0x001F);
        QL_CHECK_EQ((int)q[3], 0xFFFF);
        ql_mem_free((void *)img.px);

        /* 截断的文件：不猜，直接失败 */
        req.len = 60;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_INVAL);
#else
        /* 这个构建把 BMP 解码器裁了：同一份字节必须明确说"不支持"，不猜 */
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
#endif
    }

    /* ---------- 9. QOI 解码（含 RUN） ---------- */
    {
        static uint8_t qoi[14 + 16 + 8];
        memset(qoi, 0, sizeof(qoi));
        memcpy(qoi, "qoif", 4);
        uint32_t w = 2, h = 2;
        memcpy(qoi + 4, &w, 4);
        memcpy(qoi + 8, &h, 4);
        qoi[12] = 4;
        qoi[13] = 0;
        size_t p = 14;
        qoi[p++] = 0xFE; qoi[p++] = 0xFF; qoi[p++] = 0x00; qoi[p++] = 0x00;  /* 红 */
        qoi[p++] = 0xFE; qoi[p++] = 0x00; qoi[p++] = 0xFF; qoi[p++] = 0x00;  /* 绿 */
        qoi[p++] = 0xFE; qoi[p++] = 0x00; qoi[p++] = 0x00; qoi[p++] = 0xFF;  /* 蓝 */
        qoi[p++] = 0xFE; qoi[p++] = 0xFF; qoi[p++] = 0xFF; qoi[p++] = 0xFF;  /* 白 */

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = qoi;
        req.len = p;
        req.src_format = QL_IMAGE_SRC_AUTO;
        QLImage img;
#if QL_FEAT_IMG_QOI
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        const uint8_t *e = img.px;
        QL_CHECK_EQ((int)e[0], 0xFF); QL_CHECK_EQ((int)e[1], 0x00);
        QL_CHECK_EQ((int)e[4], 0x00); QL_CHECK_EQ((int)e[5], 0xFF);
        QL_CHECK_EQ((int)e[8], 0x00); QL_CHECK_EQ((int)e[10], 0xFF);
        QL_CHECK_EQ((int)e[12], 0xFF); QL_CHECK_EQ((int)e[13], 0xFF);
        ql_mem_free((void *)img.px);

        /* 声明 4 个像素但只给了 1 个：必须失败 */
        uint32_t bw = 4, bh = 1;
        memcpy(qoi + 4, &bw, 4);
        memcpy(qoi + 8, &bh, 4);
        req.len = 14 + 4;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_INVAL);
#else
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
#endif
    }

    /* ---------- 10. doc 资源段：编码 → 加载 → 建树 → 出图 ---------- */
    {
        /* 先长出"有 <img> 的树"，再把资源挂到 doc 上（qltext 就是这么做的） */
        QLUIDoc *doc = ql_ui_load_text("<div id=r><img src=\"pic\"></div>",
                                       "#r { width: 16px; height: 16px }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            uint8_t pixels[8 * 8 * 2];
            memcpy(pixels, s_raw, sizeof(s_raw));
            QLUIImageSrc src;
            memset(&src, 0, sizeof(src));
            src.name = doc->nodes[1].text;   /* 资源名在文本池里的下标 */
            src.off = 0;
            src.len = (uint32_t)sizeof(pixels);
            src.w = 8;
            src.h = 8;
            src.format = QL_IMAGE_SRC_RAW;
            src.pixel_format = QL_IMAGE_RGB565;

            QLUIDoc withimg = *doc;
            withimg.images = &src;
            withimg.image_count = 1;
            withimg.image_data = pixels;
            withimg.image_data_size = (uint32_t)sizeof(pixels);

            const size_t size = ql_ui_doc_encode(&withimg, NULL, 0);
            QL_CHECK(size > 0);
            uint8_t *buf = (uint8_t *)ql_mem_alloc(size);
            QL_CHECK(buf != NULL);
            if (buf != NULL) {
                QL_CHECK_EQ((int)ql_ui_doc_encode(&withimg, buf, size), (int)size);
                QLUIDoc *back = ql_ui_doc_load_mem(buf, size);
                QL_CHECK(back != NULL);
                if (back != NULL) {
                    QL_CHECK_EQ((int)back->image_count, 1);
                    QL_CHECK_EQ((int)back->image_data_size, (int)sizeof(pixels));
                    QLUIRoot *iu = ql_ui_build(back);
                    QL_CHECK(iu != NULL);
                    if (iu != NULL) {
                        ql_ui_layout(iu, 16, 16);
                        const QLImage *ri = ql_ui_node_image(iu, &iu->nodes[1]);
                        QL_CHECK(ri != NULL);
                        if (ri != NULL) {
                            QL_CHECK_EQ((int)ri->w, 8);
                            QL_CHECK_EQ((int)ri->owned, 0);   /* 指向 blob 里的资源段 */
                        }
                        ql_ui_free(iu);
                    }
                    ql_ui_doc_free(back);
                }
                ql_mem_free(buf);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 11. object-fit 五态 ---------- */
    {
        /* 8×8 图：左半红/右半绿（上行）、左半蓝/右半白（下行） */
        struct { const char *fit; int bw, bh; } cases[] = {
            { "fill", 16, 16 },
            { "contain", 32, 8 },
            { "cover", 32, 8 },
            { "none", 32, 32 },
            { "scale-down", 32, 32 },
            { "scale-down", 4, 4 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char css[192];
            snprintf(css, sizeof(css),
                     "#r { width: 40px; height: 40px; background-color: #000000 }"
                     "img { width: %dpx; height: %dpx; object-fit: %s }", cases[i].bw,
                     cases[i].bh, cases[i].fit);
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>", css, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUIImageInput in = raw_input("pic");
                QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
                ql_canvas_from_disp(&s_cv, s_disp);
                ql_canvas_clear(&s_cv, 0x000000);
                ql_ui_render_canvas(ui, &s_cv);
                const QLRect b = ql_ui_node_rect(&ui->nodes[1]);
                const int cx = (int)(b.x + b.w / 2), cy = (int)(b.y + b.h / 2);

                /* 内容盒中心永远是图里的某个像素（除了被裁掉的极端情况） */
                const ql_color_t mid = px(cx, cy);
                QL_CHECK(!(QL_COLOR_GET_R(mid) == 0 && QL_COLOR_GET_G(mid) == 0 &&
                           QL_COLOR_GET_B(mid) == 0));

                if (strcmp(cases[i].fit, "fill") == 0) {
                    /* 拉伸：内容盒四角都被图覆盖 */
                    QL_CHECK(QL_COLOR_GET_R(px(b.x + 1, b.y + 1)) > 0x40);
                    QL_CHECK(QL_COLOR_GET_G(px(ql_rect_right(b) - 2, b.y + 1)) > 0x40);
                } else if (strcmp(cases[i].fit, "contain") == 0) {
                    /* 装进去：左右留空（32 宽盒装 8 宽的图，等比后只有 8 宽） */
                    QL_CHECK(QL_COLOR_GET_R(px(b.x + 1, cy)) == 0);
                    QL_CHECK(QL_COLOR_GET_R(px(ql_rect_right(b) - 2, cy)) == 0);
                } else if (strcmp(cases[i].fit, "cover") == 0) {
                    /*
                     * 盖满：32×8 的盒装 8×8 的图 → 受高度限制，上下各裁 3 行，
                     * 只剩中间 2 行（都是下半部分：左蓝右白）。左右必须被覆盖。
                     */
                    const ql_color_t l = px(b.x + 1, cy);
                    const ql_color_t r2 = px(ql_rect_right(b) - 2, cy);
                    QL_CHECK(QL_COLOR_GET_B(l) > 0x40);       /* 左：蓝 */
                    QL_CHECK(QL_COLOR_GET_R(r2) > 0x40);      /* 右：白 */
                } else if (strcmp(cases[i].fit, "none") == 0) {
                    /* 原始尺寸居中：四角是空的，中心有像素 */
                    QL_CHECK(QL_COLOR_GET_R(px(b.x + 1, b.y + 1)) == 0);
                    /* 中心 -2 落在左下象限（蓝），只要不是黑就算画上了 */
                    const ql_color_t c2 = px(cx - 2, cy);
                    QL_CHECK(QL_COLOR_GET_R(c2) + QL_COLOR_GET_G(c2) +
                                 QL_COLOR_GET_B(c2) >
                             0x40);
                } else {   /* scale-down */
                    if (cases[i].bw > 8) {
                        /* 不放大：8×8 居中，四角空 */
                        QL_CHECK(QL_COLOR_GET_R(px(b.x + 1, b.y + 1)) == 0);
                    } else {
                        /* 4×4 盒：缩到 4×4 铺满 */
                        QL_CHECK(QL_COLOR_GET_R(px(b.x, b.y)) > 0x40);
                    }
                }
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* 非法取值：明确报错 */
        {
            QLUIDoc *bad = ql_ui_load_text("<div id=r><img src=x></div>",
                                           "img { object-fit: squish }");
            QL_CHECK(bad == NULL);
            if (bad != NULL)
                ql_ui_doc_free(bad);
            else
                QL_CHECK(strstr(ql_ui_text_last_error(), "object-fit") != NULL);
        }
    }

    /* ---------- 12. border-radius 会裁掉图片的角 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>",
                          "#r { width: 40px; height: 40px; background-color: #000000 }"
                          "img { width: 16px; height: 16px; border-radius: 8px }",
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            ql_canvas_from_disp(&s_cv, s_disp);
            ql_canvas_clear(&s_cv, 0x000000);
            ql_ui_render_canvas(ui, &s_cv);
            const QLRect b = ql_ui_node_rect(&ui->nodes[1]);
            /* 角被裁掉（半径 8 = 半宽，四角都是圆的） */
            QL_CHECK(QL_COLOR_GET_R(px(b.x, b.y)) == 0);
            QL_CHECK(QL_COLOR_GET_R(px(ql_rect_right(b) - 1, b.y)) == 0);
            /* 中心照旧有像素 */
            QL_CHECK(QL_COLOR_GET_R(px((int)(b.x + b.w / 2), (int)(b.y + b.h / 2))) > 0x40);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 13. <picture> / <source>：容器照排，候选源不参与渲染 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><picture><source srcset=\"a.png\" "
                          "type=\"image/png\"><img src=\"pic\"></picture></div>",
                          "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            /*
             * picture 是 inline-block。本实现的行内块**不收缩**（宽取可用宽），
             * 但它是透明的、不画任何东西，所以视觉上与"包着图片"一致；
             * 只有高度跟着里面的 <img> 走。
             */
            QL_CHECK_EQ((int)ui->nodes[1].style->display,
                        (int)QL_UI_DISPLAY_INLINE_BLOCK);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).h, 8);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[3]).w, 8);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[3]).h, 8);
            /* <source> 是 display:none，盒子为零、位置跟着父级起点 */
            const QLUINodeRT *src = &ui->nodes[2];
            QL_CHECK(strcmp(ql_ui_tag_name(src->src->tag), "source") == 0);
            QL_CHECK_EQ((int)src->style->display, (int)QL_UI_DISPLAY_NONE);
            QL_CHECK_EQ((int)ql_ui_node_rect(src).w, 0);
            QL_CHECK_EQ((int)ql_ui_node_rect(src).h, 0);
            /* img 真的在 picture 里面 */
            QL_CHECK(ui->nodes[3].parent == &ui->nodes[1]);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 14. 热区：rect / circle / poly / default 的命中 ---------- */
    {
        static const char *const html =
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\">"
            "<area shape=\"rect\" coords=\"0 0 3 3\" href=\"#tl\" alt=\"左上\">"
            "<area shape=\"circle\" coords=\"6 6 1\" href=\"#br\" alt=\"右下\">"
            "<area shape=\"poly\" coords=\"4 0 7 0 7 3\" href=\"#tri\">"
            "</map></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(html, "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);

            QL_CHECK_EQ(ql_ui_area_count(ui), 3);
            QL_CHECK(ql_ui_area_image(ui, 0) == &ui->nodes[2]);
            QL_CHECK(strcmp(ql_ui_area_href(ui, 0), "#tl") == 0);
            QL_CHECK(strcmp(ql_ui_area_alt(ui, 0), "左上") == 0);
            QL_CHECK(strcmp(ql_ui_area_href(ui, 2), "#tri") == 0);
            QL_CHECK(ql_ui_area_alt(ui, 2) == NULL);    /* 没写 alt */
            QL_CHECK(ql_ui_area_get(ui, 3) == NULL);    /* 越界 */

            const QLRect r = ql_ui_node_rect(&ui->nodes[2]);   /* 8×8 */
            /* rect (0,0)-(3,3)：包含左上象限的内点，(4,4) 在外面 */
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 1, r.y + 1), 0);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 3, r.y + 3), 0);  /* 边界含 */
            /* circle (6,6) r=1 */
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 6, r.y + 6), 1);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 4, r.y + 6), -1);
            /* poly (4,0)-(7,0)-(7,3)（斜边从 (4,0) 到 (7,3)） */
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 6, r.y + 1), 2);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 4, r.y + 3), -1);  /* 斜边外侧 */
            /* 图外一点：不算命中 */
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 40, r.y + 40), -1);

            /* 包围盒：rect 是 4×4，circle 是 3×3（r=1） */
            const QLRect b0 = ql_ui_area_rect(ui, 0);
            QL_CHECK_EQ((int)b0.x, (int)r.x);
            QL_CHECK_EQ((int)b0.y, (int)r.y);
            QL_CHECK_EQ((int)b0.w, 4);
            QL_CHECK_EQ((int)b0.h, 4);
            const QLRect b1 = ql_ui_area_rect(ui, 1);
            QL_CHECK_EQ((int)b1.w, 3);
            QL_CHECK_EQ((int)b1.h, 3);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 15. 热区跟着图片缩放走（坐标是图片像素） ---------- */
    {
        static const char *const html =
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\"><area coords=\"0 0 3 3\"></map></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(html, "#r { width: 64px; height: 64px } img { width: 32px }",
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            const QLRect r = ql_ui_node_rect(&ui->nodes[2]);   /* 32×32（4 倍） */
            QL_CHECK_EQ((int)r.w, 32);
            /* 图片像素 (0..3) 映射到屏幕 (0..15] */
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 14, r.y + 14), 0);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 18, r.y + 18), -1);
            const QLRect b = ql_ui_area_rect(ui, 0);
            QL_CHECK_EQ((int)b.w, 16);   /* 4 图片像素 × 4 倍 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 16. shape=default 覆盖整张图 ---------- */
    {
        static const char *const html =
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\"><area shape=\"default\" href=\"#all\"></map></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(html, "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            const QLRect r = ql_ui_node_rect(&ui->nodes[2]);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 7, r.y + 7), 0);
            QL_CHECK_EQ(ql_ui_area_at(ui, r.x + 9, r.y + 9), -1);
            const QLRect b = ql_ui_area_rect(ui, 0);
            QL_CHECK_EQ((int)b.w, 8);
            QL_CHECK_EQ((int)b.h, 8);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 17. 点击派发：热区优先，不发 click_cb ---------- */
    {
        static const char *const html =
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\"><area coords=\"0 0 3 3\" href=\"#a\"></map></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(html, "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in = raw_input("pic");
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            const QLRect r = ql_ui_node_rect(&ui->nodes[2]);

            s_area_hits = 0;
            s_click_hits = 0;
            ql_ui_set_area_cb(ui, on_area, NULL);
            ql_ui_set_click_cb(ui, on_click, NULL);

            ql_ui_pointer_down(ui, r.x + 1, r.y + 1);
            ql_ui_pointer_up(ui, r.x + 1, r.y + 1);
            QL_CHECK_EQ(s_area_hits, 1);
            QL_CHECK_EQ(s_click_hits, 0);

            /* 热区外（同一张图上）：照旧走 click_cb */
            ql_ui_pointer_down(ui, r.x + 7, r.y + 7);
            ql_ui_pointer_up(ui, r.x + 7, r.y + 7);
            QL_CHECK_EQ(s_area_hits, 1);
            QL_CHECK_EQ(s_click_hits, 1);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 18. doc 热区段：编码 → 加载 → 数值不变 ---------- */
    {
        static const char *const html =
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\">"
            "<area shape=\"rect\" coords=\"0 0 3 3\" href=\"#p1\" alt=\"A\">"
            "<area shape=\"poly\" coords=\"4 0 7 0 7 3\">"
            "</map></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(html, "#r { width: 64px; height: 64px }", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)doc->area_count, 2);
            const size_t need = ql_ui_doc_encode(doc, NULL, 0);
            QL_CHECK(need > 0);
            uint8_t *buf = (uint8_t *)ql_mem_alloc(need);
            QL_CHECK(buf != NULL);
            QL_CHECK_EQ(ql_ui_doc_encode(doc, buf, need), need);
            QLUIDoc *back = ql_ui_doc_load_mem(buf, need);
            QL_CHECK(back != NULL);
            if (back != NULL) {
                QL_CHECK_EQ((int)back->area_count, 2);
                QL_CHECK_EQ((int)back->area_coord_count,
                            (int)doc->area_coord_count);
                for (int i = 0; i < (int)doc->area_coord_count; i++)
                    QL_CHECK_EQ((int)back->area_coords[i], (int)doc->area_coords[i]);
                QL_CHECK_EQ((int)back->areas[0].img, (int)doc->areas[0].img);
                QL_CHECK_EQ((int)back->areas[1].shape, (int)QL_UI_AREA_POLY);
                QL_CHECK_EQ((int)back->areas[1].ncoord, 6);
                /* 加载回来的 doc 照样能命中 */
                QLUIRoot *ui2 = ql_ui_build(back);
                QL_CHECK(ui2 != NULL);
                if (ui2 != NULL) {
                    QLUIImageInput in = raw_input("pic");
                    QL_CHECK_EQ(ql_ui_register_image(ui2, &in), QL_OK);
                    const QLRect r2 = ql_ui_node_rect(&ui2->nodes[2]);
                    QL_CHECK_EQ(ql_ui_area_at(ui2, r2.x + 1, r2.y + 1), 0);
                    QL_CHECK_EQ(ql_ui_area_at(ui2, r2.x + 6, r2.y + 1), 1);
                    QL_CHECK(strcmp(ql_ui_area_href(ui2, 0), "#p1") == 0);
                    ql_ui_free(ui2);
                }
                ql_ui_doc_free(back);
            }
            ql_mem_free(buf);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 19. 明确失败：coords / shape 不合法就编译报错 ---------- */
    {
        static const char *const bad[] = {
            /* rect 只要 4 个 */
            "<area shape=\"rect\" coords=\"0 0 3\">",
            /* circle 要 3 个 */
            "<area shape=\"circle\" coords=\"6 6\">",
            /* poly 必须偶数且 >= 6 */
            "<area shape=\"poly\" coords=\"0 0 3 3 6\">",
            /* 未知 shape */
            "<area shape=\"triangle\" coords=\"0 0 3 3\">",
            /* default 不该有 coords */
            "<area shape=\"default\" coords=\"0 0 3 3\">",
            /* 非数字 */
            "<area coords=\"0 0 abc 3\">",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char html[256];
            snprintf(html, sizeof(html),
                     "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
                     "<map name=\"m\">%s</map></div>", bad[i]);
            QLUIDoc *doc = ql_ui_load_text(html, "");
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
            ql_ui_text_last_error();   /* 只确认有错误信息可用 */
        }
        /* 合法的写法不报错（对照组） */
        QLUIDoc *ok = ql_ui_load_text(
            "<div id=root><div id=r><img src=\"pic\" usemap=\"#m\"></div>"
            "<map name=\"m\"><area shape=\"poly\" coords=\"0 0 3 3 6 0\">"
            "</map></div>", "");
        QL_CHECK(ok != NULL);
        if (ok != NULL) {
            QL_CHECK_EQ((int)ok->area_count, 1);
            ql_ui_doc_free(ok);
        }
    }

    /* ---------- 20. 没有 <area> 时热区段是空的 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><img src=\"pic\"></div>", "", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_area_count(ui), 0);
            QL_CHECK_EQ(ql_ui_area_at(ui, 1, 1), -1);
            QL_CHECK_EQ((int)doc->area_count, 0);
            QL_CHECK(doc->areas == NULL);
            QL_CHECK(doc->area_coords == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
