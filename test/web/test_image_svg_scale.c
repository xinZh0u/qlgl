/**
 * @file    test_image_svg_scale.c
 * @brief   SVG 按元素尺寸 / DPR 自动重栅格化。
 *
 * 契约：
 *   1. 运行期注册的 SVG 在第一次渲染时按元素盒子栅格化（不是固有 24×24）；
 *   2. canvas 的 DPR 变化会重栅格化（1x → 2x，像素尺寸翻倍）；
 *   3. 元素尺寸变化后按新盒子重栅格化；
 *   4. ±25% 以内的抖动不重栅格化（避免动画/拖动窗口每帧重来）；
 *   5. 布局/`object-fit` 仍按**固有 CSS 尺寸**（24×24），不跟光栅尺寸走。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define PAGE_W 128
#define PAGE_H 128

static const char kSvg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">"
    "<rect width=\"24\" height=\"24\" fill=\"#ff0000\"/></svg>";

#if QL_FEAT_IMG_SVG
/* 左半有、右半透明：用来验证 mask / background 在 DPR 下的坐标 */
static const char kHalf[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">"
    "<rect width=\"12\" height=\"24\" fill=\"#000000\"/></svg>";

static const char kHalfRed[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">"
    "<rect width=\"12\" height=\"24\" fill=\"#ff0000\"/></svg>";

static const char kCurrentColor[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">"
    "<rect width=\"24\" height=\"24\" fill=\"currentColor\"/></svg>";
#endif

/* 建页面：图片占 50% × 50%（= 64×64，在 128 的视口里） */
static QLUIRoot *mk_ui(QLUIDoc **out_doc, QLUINodeRT **out_img)
{
    QLUIDoc *doc = ql_ui_load_text(
        "<div id='root'><img id='i' src='icon.svg'></div>",
        "#root { width: 100%; height: 100%; }"
        "#i { width: 50%; height: 50%; object-fit: fill; }");
    if (doc == NULL)
        return NULL;
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, PAGE_W, PAGE_H);
    *out_doc = doc;
    *out_img = ql_ui_find_by_tag(ui, QL_UI_TAG_IMG, 0);
    return ui;
}

static int register_svg(QLUIRoot *ui)
{
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "icon.svg";
    in.data = (const uint8_t *)kSvg;
    in.len = strlen(kSvg);
    in.format = QL_IMAGE_SRC_AUTO;
    return ql_ui_register_image(ui, &in);
}

#if QL_FEAT_IMG_SVG
static void render_at(QLUIRoot *ui, ql_coord_t w, ql_coord_t h,
                      ql_coord_t scale_fx)
{
    QLDispDriver *disp = ql_disp_mem_create(w, h, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    if (disp == NULL)
        return;
    if (scale_fx > 0)
        ql_disp_set_scale(disp, scale_fx);
    QLCanvas cv;
    QL_CHECK_EQ(ql_canvas_from_disp(&cv, disp), QL_OK);
    ql_ui_render_canvas(ui, &cv);
    ql_disp_mem_destroy(disp);
}

/* mask-image 在 DPR 2 下仍铺满整个盒子（不是左上四分之一） */
static void test_mask_dpr(void)
{
    QLUIDoc *doc = ql_ui_load_text(
        "<div id='m'></div>",
        "#m { width: 64px; height: 64px; background-color: #ff0000;"
        " mask-image: url('half.svg'); }");
    QL_CHECK(doc != NULL);
    if (doc == NULL)
        return;
    QLUIRoot *ui = ql_ui_build(doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return;
    }
    ql_ui_layout(ui, 64, 64);

    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "half.svg";
    in.data = (const uint8_t *)kHalf;
    in.len = strlen(kHalf);
    in.format = QL_IMAGE_SRC_AUTO;
    QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);

    QLDispDriver *disp = ql_disp_mem_create(128, 128, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    if (disp != NULL) {
        ql_disp_set_scale(disp, 2 * QL_SCALE_ONE);
        QLCanvas cv;
        QL_CHECK_EQ(ql_canvas_from_disp(&cv, disp), QL_OK);
        ql_disp_mem_clear(disp, 0xFFFFFFu);
        ql_ui_render_canvas(ui, &cv);

        const ql_color_t left = ql_disp_mem_pixel(disp, 32, 64);
        const ql_color_t right = ql_disp_mem_pixel(disp, 96, 64);
        QL_CHECK(QL_COLOR_GET_R(left) > 0xC0 && QL_COLOR_GET_G(left) < 0x40);
        QL_CHECK(QL_COLOR_GET_R(right) > 0xE0 && QL_COLOR_GET_G(right) > 0xE0);
        ql_disp_mem_destroy(disp);
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
}

/* background-image: url(svg) 在 DPR 2 下也铺满盒子 */
static void test_background_dpr(void)
{
    QLUIDoc *doc = ql_ui_load_text(
        "<div id='b'></div>",
        "#b { width: 64px; height: 64px;"
        " background-image: url('bg.svg'); background-size: 100% 100%; }");
    QL_CHECK(doc != NULL);
    if (doc == NULL)
        return;
    QLUIRoot *ui = ql_ui_build(doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return;
    }
    ql_ui_layout(ui, 64, 64);

    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "bg.svg";
    in.data = (const uint8_t *)kHalfRed;
    in.len = strlen(kHalfRed);
    in.format = QL_IMAGE_SRC_AUTO;
    QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);

    QLDispDriver *disp = ql_disp_mem_create(128, 128, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    if (disp != NULL) {
        ql_disp_set_scale(disp, 2 * QL_SCALE_ONE);
        QLCanvas cv;
        QL_CHECK_EQ(ql_canvas_from_disp(&cv, disp), QL_OK);
        ql_disp_mem_clear(disp, 0xFFFFFFu);
        ql_ui_render_canvas(ui, &cv);

        const ql_color_t left = ql_disp_mem_pixel(disp, 32, 64);
        const ql_color_t right = ql_disp_mem_pixel(disp, 96, 64);
        QL_CHECK(QL_COLOR_GET_R(left) > 0xC0 && QL_COLOR_GET_G(left) < 0x40);
        QL_CHECK(QL_COLOR_GET_R(right) > 0xE0 && QL_COLOR_GET_G(right) > 0xE0);
        ql_disp_mem_destroy(disp);
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
}
/* 同一个 SVG 资源名被两个元素使用，currentColor 各用各的颜色 */
static void test_current_color(void)
{
    QLUIDoc *doc = ql_ui_load_text(
        "<div id='root'><img id='a' src='color.svg'>"
        "<img id='b' src='color.svg'></div>",
        "#root { width: 100%; height: 100%; }"
        "#a { width: 32px; height: 32px; color: #00ff00; }"
        "#b { width: 32px; height: 32px; color: #0000ff; }");
    QL_CHECK(doc != NULL);
    if (doc == NULL)
        return;
    QLUIRoot *ui = ql_ui_build(doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return;
    }
    ql_ui_layout(ui, 64, 64);

    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "color.svg";
    in.data = (const uint8_t *)kCurrentColor;
    in.len = strlen(kCurrentColor);
    in.format = QL_IMAGE_SRC_AUTO;
    QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);

    QLDispDriver *disp = ql_disp_mem_create(64, 64, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    if (disp != NULL) {
        QLCanvas cv;
        QL_CHECK_EQ(ql_canvas_from_disp(&cv, disp), QL_OK);
        ql_disp_mem_clear(disp, 0xFFFFFFu);
        ql_ui_render_canvas(ui, &cv);

        const QLUINodeRT *na = ql_ui_find_by_tag(ui, QL_UI_TAG_IMG, 0);
        const QLUINodeRT *nb = ql_ui_find_by_tag(ui, QL_UI_TAG_IMG, 1);
        QL_CHECK(na != NULL && nb != NULL);
        if (na != NULL && nb != NULL) {
            const QLRect ra = ql_ui_node_rect(na);
            const QLRect rb = ql_ui_node_rect(nb);
            const ql_color_t ca =
                ql_disp_mem_pixel(disp, ra.x + ra.w / 2, ra.y + ra.h / 2);
            const ql_color_t cb =
                ql_disp_mem_pixel(disp, rb.x + rb.w / 2, rb.y + rb.h / 2);
            QL_CHECK(QL_COLOR_GET_R(ca) < 0x40 && QL_COLOR_GET_G(ca) > 0xC0 &&
                     QL_COLOR_GET_B(ca) < 0x40);
            QL_CHECK(QL_COLOR_GET_R(cb) < 0x40 && QL_COLOR_GET_G(cb) < 0x40 &&
                     QL_COLOR_GET_B(cb) > 0xC0);
        }
        ql_disp_mem_destroy(disp);
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
}

#endif /* QL_FEAT_IMG_SVG */

QL_TEST_BEGIN("test_image_svg_scale")
{
#if QL_FEAT_IMG_SVG
    QLUIDoc *doc = NULL;
    QLUINodeRT *img = NULL;
    QLUIRoot *ui = mk_ui(&doc, &img);
    QL_CHECK(ui != NULL && img != NULL);
    if (ui != NULL && img != NULL) {
        QL_CHECK_EQ(register_svg(ui), QL_OK);

        /* 1) 128 视口、1x：盒子 64×64 → 光栅 64×64 */
        render_at(ui, PAGE_W, PAGE_H, QL_SCALE_ONE);
        const QLImage *r = ql_ui_node_image(ui, img);
        QL_CHECK(r != NULL);
        if (r != NULL) {
            QL_CHECK_EQ(r->w, 64);
            QL_CHECK_EQ(r->h, 64);
        }

        /* 固有尺寸仍然是 24（布局按 CSS 像素，不跟光栅走） */
        const QLRect rect = ql_ui_node_rect(&ui->nodes[1]);
        QL_CHECK_EQ((int)rect.w, 64);
        QL_CHECK_EQ((int)rect.h, 64);

        /* 2) DPR 2：同样的 CSS 盒子 → 光栅 128×128 */
        render_at(ui, PAGE_W * 2, PAGE_H * 2, 2 * QL_SCALE_ONE);
        r = ql_ui_node_image(ui, img);
        QL_CHECK(r != NULL);
        if (r != NULL) {
            QL_CHECK_EQ(r->w, 128);
            QL_CHECK_EQ(r->h, 128);
        }

        /* 3) 元素变大：256 视口、1x → 盒子 128×128 → 光栅 128×128 */
        ql_ui_layout(ui, PAGE_W * 2, PAGE_H * 2);
        render_at(ui, PAGE_W * 2, PAGE_H * 2, QL_SCALE_ONE);
        r = ql_ui_node_image(ui, img);
        QL_CHECK(r != NULL);
        if (r != NULL) {
            QL_CHECK_EQ(r->w, 128);
            QL_CHECK_EQ(r->h, 128);
        }

        /* 4) 尺寸只抖 2px（128 → 130）：±25% 带内，不重栅格化 */
        ql_ui_layout(ui, PAGE_W * 2 + 4, PAGE_H * 2);
        render_at(ui, PAGE_W * 2 + 4, PAGE_H * 2, QL_SCALE_ONE);
        r = ql_ui_node_image(ui, img);
        QL_CHECK(r != NULL);
        if (r != NULL) {
            QL_CHECK_EQ(r->w, 128);   /* 保持上次的光栅 */
            QL_CHECK_EQ(r->h, 128);
        }

        ql_ui_free(ui);
    }
    if (doc != NULL)
        ql_ui_doc_free(doc);

    test_mask_dpr();
    test_background_dpr();
    test_current_color();
#else
    /* 没有 SVG 的构建：注册明确失败，不会留下任何重栅格化状态 */
    QLUIDoc *doc = NULL;
    QLUINodeRT *img = NULL;
    QLUIRoot *ui = mk_ui(&doc, &img);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QL_CHECK_EQ(register_svg(ui), QL_ERR_UNSUPPORTED);
        ql_ui_free(ui);
    }
    if (doc != NULL)
        ql_ui_doc_free(doc);
#endif
}
QL_TEST_END()
