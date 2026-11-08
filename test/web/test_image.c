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

            /* PNG 字节：本构建的解码器还没落地 → UNSUPPORTED（数据能识别，但解不了） */
            static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
            QLUIImageInput p;
            memset(&p, 0, sizeof(p));
            p.name = "pic";
            p.data = png;
            p.len = sizeof(png);
            p.format = QL_IMAGE_SRC_AUTO;
            QL_CHECK_EQ(ql_ui_register_image(ui, &p), QL_ERR_UNSUPPORTED);
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

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
