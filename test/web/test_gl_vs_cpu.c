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

/* 最小自定义控件：只画两条直线，用来验证 d2d->draw_line 的 GL/CPU 一致性 */
QL_PROPERTY_ENUM_EMPTY(QL_XGL, QLXgl, QL_WIDGET, 20)
QL_EVENT_ENUM_EMPTY(QL_XGL, QLXgl, QL_WIDGET, 20)
QL_DECLARE(QLXgl, ql_xgl)
struct QLXgl        { QLWidget parent; };
struct QLXglClass   { QLWidgetClass parent; };
struct QLXglPrivate { int unused; };
QL_DEFINE_WITH_PRIVATE(QL_XGL, QLXgl, ql_xgl, ql_widget)

static void ql_xgl_draw(QLWidget *self, QLDraw2D *d2d)
{
    ql_widget_draw_default(self, d2d);
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    const ql_color_t col = QL_COLOR_RGB(0x90, 0xE0, 0x90);
    d2d->draw_line(d2d, r.x + 4, r.y + 4, ql_rect_right(r) - 5, ql_rect_bottom(r) - 5, 3, col);
    d2d->draw_line(d2d, ql_rect_right(r) - 5, r.y + 4, r.x + 4, ql_rect_bottom(r) - 5, 1, col);
}

static void ql_xgl_class_init(QLXglClass *klass)
{
    ql_widget_class_set_draw(((QLObjectClass *)(void *)klass)->type, ql_xgl_draw);
}

static void ql_xgl_init(QLXgl *self)
{
    struct QLXglPrivate *priv = ql_xgl_get_private(self);
    priv->unused = 0;
}

static void ql_xgl_deinit(QLXgl *self) { QL_UNUSED(self); }

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
     * ---- 控件层也走 QLDraw2D：GL 与 CPU 必须一致 ----
     *
     * 这一段验证 ql_render_draw2d()：同一棵 QLWidget 树分别画到 CPU 画布和
     * GL 离屏帧缓冲，逐像素比较。涵盖背景、圆角边框、文本、以及自定义控件
     * 用 d2d->draw_line 画直线（直线是公共 Bresenham 实现，不应有差异）。
     */
    {
        QLWidget *wroot = ql_widget_new_ex(0, 0, W, H);
        QL_CHECK(wroot != NULL);
        ql_widget_set_bg((QLObject *)wroot, QL_COLOR_RGB(0x18, 0x20, 0x2A), 255);

        QLWidget *card = ql_widget_new_child((QLObject *)wroot, 20, 20, 120, 70);
        ql_widget_set_bg((QLObject *)card, QL_COLOR_RGB(0x2A, 0x3A, 0x50), 255);
        ql_widget_set_border((QLObject *)card, 2, QL_COLOR_RGB(0x88, 0xAA, 0xCC), 8);

        QLWidget *hib = ql_widget_new_child((QLObject *)wroot, 60, 130, 80, 40);
        ql_widget_set_bg((QLObject *)hib, QL_COLOR_RGB(0x50, 0x30, 0x40), 200);

        QLLabel *lab = ql_label_new_child((QLObject *)wroot, "QLGL", 24, 100);
        ql_label_set_text_color((QLObject *)lab, QL_COLOR_RGB(0xE8, 0xF0, 0xFF));

        QLType xtype = QL_REGISTER(ql_xgl, QL_WIDGET_TYPE);
        QL_CHECK(xtype != QL_TYPE_NONE);
        QLWidget *barc = (QLWidget *)ql_object_new(xtype);
        QL_CHECK(barc != NULL);
        ql_object_add_child((QLObject *)wroot, (QLObject *)barc);
        ql_widget_set_pos((QLObject *)barc, 200, 30);
        ql_widget_set_bg((QLObject *)barc, QL_COLOR_RGB(0x1E, 0x2A, 0x1E), 255);
        ql_widget_set_border((QLObject *)barc, 1, QL_COLOR_RGB(0x40, 0x60, 0x40), 0);
        ql_object_set_property((QLObject *)barc, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(90));
        ql_object_set_property((QLObject *)barc, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(90));

        ql_render_set_root((QLObject *)wroot);
        ql_render_invalidate_all();

        /* CPU 参考 */
        QLDispDriver *d2 = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QLCanvas c2;
        QL_CHECK_EQ(ql_canvas_from_disp(&c2, d2), QL_OK);
        ql_canvas_clear(&c2, QL_COLOR_RGB(0x08, 0x0C, 0x10));
        QLDraw2D cd2d;
        QL_CHECK_EQ(ql_draw2d_canvas_init(&cd2d, &c2), QL_OK);
        QL_CHECK(ql_render_draw2d(&cd2d, W, H) > 0);

        /* 自定义控件确实画出来了（直线起点处的方形笔刷） */
        QL_CHECK_EQ(ql_disp_mem_pixel(d2, 204, 34), QL_COLOR_RGB(0x90, 0xE0, 0x90));
        QL_CHECK_EQ(ql_disp_mem_pixel(d2, 210, 32), QL_COLOR_RGB(0x1E, 0x2A, 0x1E));

        /* GPU */
        QLGfxGL *g2 = ql_gfx_gl_create("widget-gl", W, H, QL_GFX_GL_OFFSCREEN);
        if (g2 == NULL) {
            printf("  [SKIP] 控件层 GL 对比：无法创建上下文\n");
        } else {
            ql_gfx_gl_begin(g2, QL_COLOR_RGB(0x08, 0x0C, 0x10));
            QL_CHECK(ql_render_draw2d(ql_gfx_gl_draw(g2), W, H) > 0);
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
            printf("  控件层像素差异：%ld/%d = %.2f%%\n", bad2, W * H, ratio2 * 100);
            QL_CHECK(ratio2 <= 0.02);
            ql_mem_free(rgba2);
            ql_gfx_gl_destroy(g2);
        }

        ql_disp_mem_destroy(d2);
        ql_object_unref((QLObject *)wroot);
        ql_render_set_root(NULL);
    }

QL_TEST_END()
#endif
