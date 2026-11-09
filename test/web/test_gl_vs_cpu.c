/**
 * @file    test_gl_vs_cpu.c
 * @brief   GPU 与 CPU 后端的 golden image 对比（离屏，不弹窗）。
 *
 * 同一份 HTML/CSS 编译出的界面，分别用 CPU 光栅与 GL(ES3/EGL) 渲染，
 * 逐像素比较。抗锯齿实现不同，因此允许小容差。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"
#include "ui_demo.h"
#include "ui_flex_anim.h"
#include "ui_flex_grad.h"

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("== test_gl_vs_cpu ==\n  跳过：未编译 GL 后端\n  0 项检查，0 项失败\n");
    ql_test_checks = 0;
    ql_test_failures = 0;
    return 0;
}
#else

#define W 320
#define H 240
#define TOLERANCE 40          /* 单通道允许误差 */
/*
 * 允许不同的像素占比：差异几乎都在字形边缘——GPU 走纹理双线性插值，
 * CPU 是逐像素覆盖率，两者对边缘的处理略有不同。
 *
 * 这个阈值是**防退化**用的：曾经 GPU 侧把字形 quad 也外扩了 1px 做抗锯齿，
 * 结果每个字被拉宽 (w+2)/w（8px 的字就是 25%），字距被吃掉、笔画连成一片，
 * 这里量到 5.72%。修好后是 0.73%，所以 2% 的阈值能守住这个回归。
 */
#define MAX_BAD_RATIO 0.02

QL_TEST_BEGIN("test_gl_vs_cpu")

    /* ---- CPU 参考图 ---- */
    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    QLCanvas canvas;
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
    ql_canvas_clear(&canvas, QL_COLOR_RGB(0x10, 0x14, 0x1C));

    QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
    QL_CHECK(ui != NULL);
    ql_ui_layout(ui, W, H);
    ql_ui_render_canvas(ui, &canvas);

    /* ---- GPU 图 ---- */
    QLGfxGL *gfx = ql_gfx_gl_create("gl-vs-cpu", W, H, QL_GFX_GL_OFFSCREEN);
    if (gfx == NULL) {
        printf("  [SKIP] 无法创建 GL 上下文（无 EGL/X11？）\n");
        ql_ui_free(ui);
        ql_disp_mem_destroy(disp);
        qlgl_shutdown();
        return 0;
    }

    ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x10, 0x14, 0x1C));
    ql_ui_render(ui, ql_gfx_gl_draw(gfx));
    ql_gfx_gl_end(gfx);

    QL_CHECK_EQ(ql_gfx_gl_frame_count(gfx), 1);
    QL_CHECK(ql_gfx_gl_vertex_count(gfx) > 0);
    QL_CHECK_EQ(ql_gfx_gl_draw_calls(gfx), 1);      /* 单批次 */

    uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
    QL_CHECK(rgba != NULL);
    QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);

    /* ---- 逐像素比较 ---- */
    long total = 0, bad = 0;
    long max_diff = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            ql_color_t c = ql_disp_mem_pixel(disp, x, y);
            const uint8_t *g = rgba + ((size_t)y * (size_t)W + (size_t)x) * 4u;
            int dr = abs((int)QL_COLOR_GET_R(c) - (int)g[0]);
            int dg = abs((int)QL_COLOR_GET_G(c) - (int)g[1]);
            int db = abs((int)QL_COLOR_GET_B(c) - (int)g[2]);
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d > max_diff) max_diff = d;
            if (d > TOLERANCE) bad++;
            total++;
        }
    }

    double ratio = (double)bad / (double)total;
    printf("  像素差异：%ld/%ld = %.2f%%（最大单通道差 %ld）\n", bad, total, ratio * 100, max_diff);
    QL_CHECK(ratio <= MAX_BAD_RATIO);

    /*
     * ---- 位图搬运：CPU 与 GPU 必须一致 ----
     *
     * GPU 侧的图片是"单独一个批次"（整帧共用一个纹理，而图片是另一张），
     * 所以这里除了比对像素，还确认两件事：能整除成两个批次、纹理不串色。
     */
    {
        static uint16_t raw565[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const int left = x < 4, top = y < 4;
                raw565[y * 8 + x] =
                    (uint16_t)(top ? (left ? 0xF800 : 0x07E0) : (left ? 0x001F : 0xFFFF));
            }
        static uint8_t raw_rgba[4 * 4 * 4];
        for (int i = 0; i < 16; i++) {
            raw_rgba[i * 4 + 0] = 0x20;
            raw_rgba[i * 4 + 1] = 0x80;
            raw_rgba[i * 4 + 2] = 0xE0;
            raw_rgba[i * 4 + 3] = (uint8_t)(i < 8 ? 255 : 96);   /* 一半半透明 */
        }

        const char *html =
            "<div id=r><img src=a><img src=b class=cover></div>";
        const char *css = "#r { width: 64px; height: 64px; background-color: #000000 }"
                          "img { width: 24px; height: 24px }"
                          "img.cover { object-fit: cover; border-radius: 12px }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *iu = ql_ui_build(doc);
            QL_CHECK(iu != NULL);
            if (iu != NULL) {
                QLUIImageInput i1;
                memset(&i1, 0, sizeof(i1));
                i1.name = "a";
                i1.data = (const uint8_t *)raw565;
                i1.len = sizeof(raw565);
                i1.w = 8;
                i1.h = 8;
                i1.format = QL_IMAGE_SRC_RAW;
                i1.pixel_format = QL_IMAGE_RGB565;
                QL_CHECK_EQ(ql_ui_register_image(iu, &i1), QL_OK);

                QLUIImageInput i2;
                memset(&i2, 0, sizeof(i2));
                i2.name = "b";
                i2.data = raw_rgba;
                i2.len = sizeof(raw_rgba);
                i2.w = 4;
                i2.h = 4;
                i2.format = QL_IMAGE_SRC_RAW;
                i2.pixel_format = QL_IMAGE_RGBA8888;
                QL_CHECK_EQ(ql_ui_register_image(iu, &i2), QL_OK);

                ql_ui_layout(iu, 64, 64);

                /* CPU */
                QLDispDriver *id = ql_disp_mem_create(64, 64, QL_COLOR_RGB888);
                QLCanvas ic;
                QL_CHECK_EQ(ql_canvas_from_disp(&ic, id), QL_OK);
                ql_canvas_clear(&ic, 0x000000);
                ql_ui_render_canvas(iu, &ic);

                /* GPU */
                QLGfxGL *ig = ql_gfx_gl_create("img-vs-cpu", 64, 64, QL_GFX_GL_OFFSCREEN);
                QL_CHECK(ig != NULL);
                if (ig != NULL) {
                    ql_gfx_gl_begin(ig, 0x000000);
                    ql_ui_render(iu, ql_gfx_gl_draw(ig));
                    ql_gfx_gl_end(ig);

                    /* 两张图 = 两个批次（每个图片各自 flush 一次） */
                    QL_CHECK(ql_gfx_gl_draw_calls(ig) >= 2);

                    uint8_t *irgba = (uint8_t *)ql_mem_calloc(1, 64u * 64u * 4u);
                    QL_CHECK(irgba != NULL);
                    if (irgba != NULL) {
                        QL_CHECK_EQ(ql_gfx_gl_read_pixels(ig, irgba, 64 * 4), QL_OK);
                        long bad2 = 0, tot2 = 0;
                        for (int y = 0; y < 64; y++)
                            for (int x = 0; x < 64; x++) {
                                ql_color_t c = ql_disp_mem_pixel(id, (ql_coord_t)x,
                                                                 (ql_coord_t)y);
                                const uint8_t *g2 =
                                    irgba + ((size_t)y * 64u + (size_t)x) * 4u;
                                int dr = abs((int)QL_COLOR_GET_R(c) - (int)g2[0]);
                                int dg2 = abs((int)QL_COLOR_GET_G(c) - (int)g2[1]);
                                int db = abs((int)QL_COLOR_GET_B(c) - (int)g2[2]);
                                int dmax = dr > dg2 ? dr : dg2;
                                if (db > dmax) dmax = db;
                                if (dmax > TOLERANCE) bad2++;
                                tot2++;
                            }
                        const double r2 = (double)bad2 / (double)tot2;
                        printf("  位图 CPU vs GPU：%ld/%ld = %.2f%%\n", bad2, tot2,
                               r2 * 100.0);
                        QL_CHECK(r2 <= MAX_BAD_RATIO);

                        /* 关键点抽查：左上红、右上绿、左下蓝、右下白 */
                        QLRect a0 = ql_ui_node_rect(&iu->nodes[1]);
                        const int mx[4] = { a0.x + 6, a0.x + 18, a0.x + 6, a0.x + 18 };
                        const int my[4] = { a0.y + 6, a0.y + 6, a0.y + 18, a0.y + 18 };
                        for (int k = 0; k < 4; k++) {
                            const uint8_t *g3 =
                                irgba + ((size_t)my[k] * 64u + (size_t)mx[k]) * 4u;
                            ql_color_t c = ql_disp_mem_pixel(id, (ql_coord_t)mx[k],
                                                             (ql_coord_t)my[k]);
                            QL_CHECK(abs((int)QL_COLOR_GET_R(c) - (int)g3[0]) <= TOLERANCE);
                            QL_CHECK(abs((int)QL_COLOR_GET_G(c) - (int)g3[1]) <= TOLERANCE);
                            QL_CHECK(abs((int)QL_COLOR_GET_B(c) - (int)g3[2]) <= TOLERANCE);
                        }
                        ql_mem_free(irgba);
                    }
                    ql_gfx_gl_destroy(ig);
                }
                ql_disp_mem_destroy(id);
                ql_ui_free(iu);
            }
            ql_ui_doc_free(doc);
        }
    }

    /*
     * ---- 毛玻璃的圆角裁剪：两条后端都不许把模糊漏到圆角缺口 ----
     *
     * 模糊是方框运算，元素是圆角的。漏出去的表现是"圆角缺口里渗出方形模糊块"，
     * 肉眼一看就是错的，但它只在有 `border-radius` 的元素上出现，容易被漏测。
     * 场景：红色硬边竖条 + 圆角玻璃卡，缺口处必须还是**纯红硬边**。
     */
    {
        const char *html = "<div id=r><div id=s></div><div id=g></div></div>";
        const char *css =
            "#r { width: 160px; height: 120px; background-color: #FFFFFF }"
            "#s { position: absolute; left: 0px; top: 0px; width: 40px; height: 120px;"
            "     background-color: #FF0000 }"
            "#g { position: absolute; left: 30px; top: 20px; width: 100px; height: 80px;"
            "     border-radius: 16px; background-color: rgba(255,255,255,0.15);"
            "     backdrop-filter: blur(6px) }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *bu = ql_ui_build(doc);
            QL_CHECK(bu != NULL);
            if (bu != NULL) {
                ql_ui_layout(bu, 160, 120);

                QLDispDriver *bd = ql_disp_mem_create(160, 120, QL_COLOR_RGB888);
                QLCanvas bc;
                QL_CHECK_EQ(ql_canvas_from_disp(&bc, bd), QL_OK);
                ql_canvas_clear(&bc, QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
                ql_ui_render_canvas(bu, &bc);

                QLGfxGL *bg = ql_gfx_gl_create("blur-round", 160, 120,
                                               QL_GFX_GL_OFFSCREEN);
                uint8_t *brgba = (uint8_t *)ql_mem_calloc(1, 160u * 120u * 4u);
                QL_CHECK(bg != NULL && brgba != NULL);
                if (bg != NULL && brgba != NULL) {
                    ql_gfx_gl_begin(bg, QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
                    ql_ui_render(bu, ql_gfx_gl_draw(bg));
                    ql_gfx_gl_end(bg);
                    QL_CHECK_EQ(ql_gfx_gl_read_pixels(bg, brgba, 160 * 4), QL_OK);

                    /* (31,21)：左上圆角缺口内、红条之外 —— 必须是纯红（没被糊进去） */
                    ql_color_t cc = ql_disp_mem_pixel(bd, 31, 21);
                    const uint8_t *gp = brgba + ((size_t)21 * 160u + 31u) * 4u;
                    QL_CHECK(QL_COLOR_GET_G(cc) <= 8 && QL_COLOR_GET_B(cc) <= 8);
                    QL_CHECK(gp[1] <= 8 && gp[2] <= 8);
                    /* (42,55)：红白硬边在玻璃里 —— 两边都应被糊成粉色 */
                    ql_color_t cb = ql_disp_mem_pixel(bd, 42, 55);
                    const uint8_t *gb2 = brgba + ((size_t)55 * 160u + 42u) * 4u;
                    QL_CHECK(QL_COLOR_GET_G(cb) > 60);
                    QL_CHECK(gb2[1] > 60);
                    ql_mem_free(brgba);
                }
                if (bg != NULL)
                    ql_gfx_gl_destroy(bg);
                ql_disp_mem_destroy(bd);
                ql_ui_free(bu);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---- 关键位置抽查：背景、标题条、面板 ---- */
    QL_CHECK(rgba[((size_t)2 * (size_t)W + 2u) * 4u + 0u] < 40);          /* 左上角深色 */

    /* ---- 字形几何：GPU 画的字符串不能比 CPU 宽/高 ---- */
    {
        QLUIRoot *au = ql_ui_build(&ui_flex_anim_doc);
        QL_CHECK(au != NULL);
        if (au != NULL) {
            ql_ui_layout(au, 360, 260);

            /* 找到 .hint（一行中文 + 英文混排，最考验字距） */
            QLUINodeRT *hint = NULL;
            for (uint16_t i = 0; i < au->node_count; i++) {
                const char *t = ql_ui_node_text(&au->nodes[i]);
                if (t != NULL && strstr(t, "Esc") != NULL) {
                    hint = &au->nodes[i];
                    break;
                }
            }
            QL_CHECK(hint != NULL);

            QLDispDriver *d3 = ql_disp_mem_create(360, 260, QL_COLOR_RGB888);
            QLCanvas c3;
            QL_CHECK_EQ(ql_canvas_from_disp(&c3, d3), QL_OK);
            ql_canvas_clear(&c3, QL_COLOR_RGB(0x00, 0x00, 0x00));
            ql_ui_render_canvas(au, &c3);

            QLGfxGL *g3 = ql_gfx_gl_create("glyph-geom", 360, 260, QL_GFX_GL_OFFSCREEN);
            uint8_t *rgba3 = (uint8_t *)ql_mem_calloc(1, 360u * 260u * 4u);
            QL_CHECK(g3 != NULL && rgba3 != NULL);
            if (g3 != NULL && rgba3 != NULL && hint != NULL) {
                ql_gfx_gl_begin(g3, QL_COLOR_RGB(0x00, 0x00, 0x00));
                ql_ui_render(au, ql_gfx_gl_draw(g3));
                ql_gfx_gl_end(g3);
                QL_CHECK_EQ(ql_gfx_gl_read_pixels(g3, rgba3, 360 * 4), QL_OK);

                /* 文字所在的横向带状区域（避开卡片等实心块） */
                QLRect box = ql_ui_node_rect(hint);
                int y0 = box.y, y1 = box.y + box.h;
                if (y0 < 0) y0 = 0;
                if (y1 > 260) y1 = 260;

                int cpu_l = 1 << 20, cpu_r = -1;
                int gl_l = 1 << 20, gl_r = -1;
                for (int y = y0; y < y1; y++) {
                    for (int x = 0; x < 360; x++) {
                        ql_color_t cc = ql_disp_mem_pixel(d3, x, y);
                        if (QL_COLOR_GET_R(cc) + QL_COLOR_GET_G(cc) +
                                QL_COLOR_GET_B(cc) > 90) {
                            if (x < cpu_l) cpu_l = x;
                            if (x > cpu_r) cpu_r = x;
                        }
                        const uint8_t *gp =
                            rgba3 + ((size_t)y * 360u + (size_t)x) * 4u;
                        if ((int)gp[0] + (int)gp[1] + (int)gp[2] > 90) {
                            if (x < gl_l) gl_l = x;
                            if (x > gl_r) gl_r = x;
                        }
                    }
                }
                QL_CHECK(cpu_r > cpu_l);
                QL_CHECK(gl_r > gl_l);

                int cpu_w = cpu_r - cpu_l + 1;
                int gl_w = gl_r - gl_l + 1;
                printf("  .hint 墨迹宽度：CPU %d px / GPU %d px\n", cpu_w, gl_w);
                /*
                 * 外扩 1px 的拉伸会让墨迹左右各长 1px（共 2px）；
                 * 正常差异来自抗锯齿的最后一像素，最多 1px。
                 */
                QL_CHECK(abs(gl_w - cpu_w) <= 1);
                QL_CHECK(abs(gl_l - cpu_l) <= 1);

                ql_mem_free(rgba3);
            } else {
                ql_mem_free(rgba3);
            }
            if (g3 != NULL)
                ql_gfx_gl_destroy(g3);
            ql_ui_free(au);
            ql_disp_mem_destroy(d3);
        }
    }

    /*
     * ---- 渐变/圆角专项：没有文字，所以阈值可以收紧 ----
     * 覆盖径向渐变（第二色走 a_uv.x 打包）与圆角描边。
     */
    {
        QLUIRoot *gu = ql_ui_build(&ui_flex_grad_doc);
        QL_CHECK(gu != NULL);
        if (gu != NULL) {
            ql_ui_layout(gu, 120, 120);

            QLDispDriver *gd = ql_disp_mem_create(120, 120, QL_COLOR_RGB888);
            QLCanvas gc;
            QL_CHECK_EQ(ql_canvas_from_disp(&gc, gd), QL_OK);
            ql_canvas_clear(&gc, QL_COLOR_RGB(0, 0, 0));
            ql_ui_render_canvas(gu, &gc);

            QLGfxGL *gg = ql_gfx_gl_create("grad", 120, 120, QL_GFX_GL_OFFSCREEN);
            if (gg == NULL) {
                printf("  [SKIP] 渐变 GL 对比：无法创建上下文\n");
            } else {
                ql_gfx_gl_begin(gg, QL_COLOR_RGB(0, 0, 0));
                ql_ui_render(gu, ql_gfx_gl_draw(gg));
                ql_gfx_gl_end(gg);

                uint8_t *gr = (uint8_t *)ql_mem_calloc(1, 120u * 120u * 4u);
                QL_CHECK(gr != NULL);
                if (gr != NULL) {
                    QL_CHECK_EQ(ql_gfx_gl_read_pixels(gg, gr, 120 * 4), QL_OK);
                    long gbad = 0;
                    for (int y = 0; y < 120; y++) {
                        for (int x = 0; x < 120; x++) {
                            ql_color_t c = ql_disp_mem_pixel(gd, x, y);
                            const uint8_t *p =
                                gr + ((size_t)y * 120u + (size_t)x) * 4u;
                            int dr = abs((int)QL_COLOR_GET_R(c) - (int)p[0]);
                            int dg2 = abs((int)QL_COLOR_GET_G(c) - (int)p[1]);
                            int db = abs((int)QL_COLOR_GET_B(c) - (int)p[2]);
                            int m = dr > dg2 ? dr : dg2;
                            if (db > m) m = db;
                            if (m > 24) gbad++;
                        }
                    }
                    double gratio = (double)gbad / (double)(120 * 120);
                    printf("  渐变/圆角 CPU vs GPU：%ld/%d = %.2f%%\n", gbad,
                           120 * 120, gratio * 100.0);
                    /* 没有文字，差异只应来自 SDF 边缘的 AA */
                    QL_CHECK(gratio <= 0.02);

                    /*
                     * 结构性断言：`.rad2` 是"径向渐变 + clip-path"。
                     * 光看比例抓不住这个 bug——GPU 整块丢掉遮罩时比例才 3.00%，
                     * 刚好卡在阈值上。所以直接看角落：遮罩半径 75% 盖不到角，必须是底色。
                     */
                    {
                        QLRect b = ql_ui_node_rect(&gu->nodes[3]);
                        const uint8_t *corner =
                            gr + ((size_t)b.y * 120u + (size_t)b.x) * 4u;
                        printf("  遮罩角落 GPU 像素：%02X%02X%02X\n", corner[0],
                               corner[1], corner[2]);
                        QL_CHECK(corner[0] < 24 && corner[1] < 24 && corner[2] < 24);
                        /* 而中心必须真的画出了渐变（不能为了过断言整块不画） */
                        const uint8_t *mid =
                            gr + ((size_t)(b.y + b.h / 2) * 120u +
                                  (size_t)(b.x + b.w / 2)) * 4u;
                        QL_CHECK(mid[0] + mid[1] + mid[2] > 120);
                    }
                    ql_mem_free(gr);
                }
                ql_gfx_gl_destroy(gg);
            }
            ql_ui_free(gu);
            ql_disp_mem_destroy(gd);
        }
    }

    ql_mem_free(rgba);
    ql_gfx_gl_destroy(gfx);
    ql_ui_free(ui);
    ql_disp_mem_destroy(disp);

    /*
     * ---- QLDraw2D 原语本身：GL 与 CPU 必须一致 ----
     *
     * 这里不走任何"节点树"，直接按顺序调 draw2d 原语画同一张图，两个后端
     * 逐像素比较。涵盖实心矩形、圆角填充、圆角描边、文本，以及 draw_line
     * （直线是公共 Bresenham 实现，两个后端同源，本不该有任何差异）。
     *
     * 不覆盖的部分：布局（那由 test_render / test_flex 守）。
     */
    {
        /* 同一段绘制代码，对两个后端各跑一遍 */
        #define DRAW_SCENE(d2d)                                                    \
            do {                                                                   \
                (d2d)->fill_rect((d2d), ql_rect_make(0, 0, 120, 80),               \
                                 QL_COLOR_RGB(0x2A, 0x3A, 0x50), 255);             \
                (d2d)->fill_round_rect((d2d), ql_rect_make(8, 90, 60, 24), 8,      \
                                       QL_COLOR_RGB(0x50, 0x30, 0x40), 200);       \
                (d2d)->stroke_round_rect((d2d), ql_rect_make(70, 90, 60, 24), 6,\
                                         2, QL_COLOR_RGB(0x88, 0xAA, 0xCC));       \
                (d2d)->draw_line((d2d), 4, 4, ql_rect_right(                       \
                                     ql_rect_make(0, 0, 120, 80)) - 5, 75, 3,      \
                                 QL_COLOR_RGB(0x90, 0xE0, 0x90));                  \
                (d2d)->draw_line((d2d), 115, 4, 4, 75, 1,                          \
                                 QL_COLOR_RGB(0x90, 0xE0, 0x90));                  \
            } while (0)

        /* CPU 参考 */
        QLDispDriver *d2 = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QLCanvas c2;
        QL_CHECK_EQ(ql_canvas_from_disp(&c2, d2), QL_OK);
        ql_canvas_clear(&c2, QL_COLOR_RGB(0x08, 0x0C, 0x10));
        QLDraw2D cd2d;
        QL_CHECK_EQ(ql_draw2d_canvas_init(&cd2d, &c2), QL_OK);
        DRAW_SCENE(&cd2d);
        if (cd2d.flush != NULL)
            cd2d.flush(&cd2d);

        /* 直线用公共 Bresenham（方形笔刷），起点处应有确定的一格 */
        QL_CHECK_EQ(ql_disp_mem_pixel(d2, 4, 4), QL_COLOR_RGB(0x90, 0xE0, 0x90));
        /* 矩形内部 */
        QL_CHECK_EQ(ql_disp_mem_pixel(d2, 10, 10), QL_COLOR_RGB(0x2A, 0x3A, 0x50));

        /* GPU */
        QLGfxGL *g2 = ql_gfx_gl_create("draw2d-gl", W, H, QL_GFX_GL_OFFSCREEN);
        if (g2 == NULL) {
            printf("  [SKIP] draw2d 原语 GL 对比：无法创建上下文\n");
        } else {
            ql_gfx_gl_begin(g2, QL_COLOR_RGB(0x08, 0x0C, 0x10));
            QLDraw2D *gd2d = ql_gfx_gl_draw(g2);
            DRAW_SCENE(gd2d);
            if (gd2d->flush != NULL)
                gd2d->flush(gd2d);
            ql_gfx_gl_end(g2);

            uint8_t *rgba2 = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
            QL_CHECK(rgba2 != NULL);
            QL_CHECK_EQ(ql_gfx_gl_read_pixels(g2, rgba2, W * 4), QL_OK);

            long bad2 = 0;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    ql_color_t c = ql_disp_mem_pixel(d2, x, y);
                    const uint8_t *g = rgba2 + ((size_t)y * (size_t)W + (size_t)x) * 4u;
                    int dr = abs((int)QL_COLOR_GET_R(c) - (int)g[0]);
                    int dg = abs((int)QL_COLOR_GET_G(c) - (int)g[1]);
                    int db = abs((int)QL_COLOR_GET_B(c) - (int)g[2]);
                    int d = dr > dg ? dr : dg;
                    if (db > d) d = db;
                    if (d > TOLERANCE) bad2++;
                }
            }
            double ratio2 = (double)bad2 / ((double)W * (double)H);
            printf("  draw2d 原语像素差异：%ld/%d = %.2f%%\n", bad2, W * H, ratio2 * 100);
            QL_CHECK(ratio2 <= 0.02);
            ql_mem_free(rgba2);
            ql_gfx_gl_destroy(g2);
        }

        #undef DRAW_SCENE
        ql_disp_mem_destroy(d2);
    }

QL_TEST_END()
#endif
