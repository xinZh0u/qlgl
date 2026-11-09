/**
 * @file    test_blur.c
 * @brief   backdrop-filter: blur()——毛玻璃。模糊的是"元素背后已经画好的内容"。
 *
 * 五条契约：
 *   1. 生成期把 `blur(Npx)` 编译进样式（运行期零解析）；
 *   2. **硬边被糊成渐变**：模糊区域里出现一长串"中间色"像素；
 *   3. **区域外一个像素都不动**（模糊只在元素自己的矩形里发生）；
 *   4. 元素自己的背景照常叠在模糊结果之上（半透明"玻璃"）；
 *   5. GPU 必须和 CPU 一样真的糊（用同一组性质断言，不做逐像素比较）；
 *   6. **跟随 transform**：玻璃自己平移之后，糊的是它现在盖住的那块内容，
 *      而不是它"没动之前"那块地方（这条曾经是错的，两条后端都错）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_blur.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 120
#define H 80
#define ROOT_BG 0x101010u

/* 一行上"既不是黑也不是白"的像素个数——模糊把硬边摊开，这个数会明显变大 */
static int is_ramp(int v)
{
    return v > 70 && v < 235;
}

static int ramp_count_cpu(QLDispDriver *d, int y, int x0, int x1)
{
    int n = 0;
    for (int x = x0; x < x1; x++)
        if (is_ramp((int)QL_COLOR_GET_R(ql_disp_mem_pixel(d, x, y))))
            n++;
    return n;
}

/* rgba: 每像素 4 字节、行距 stride 字节 */
static int ramp_count_rgba(const uint8_t *rgba, int stride, int y, int x0, int x1)
{
    int n = 0;
    for (int x = x0; x < x1; x++)
        if (is_ramp((int)rgba[(size_t)y * (size_t)stride + (size_t)x * 4u]))
            n++;
    return n;
}

QL_TEST_BEGIN("test_blur")

    QLUIRoot *ui = ql_ui_build(&ui_flex_blur_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* ---------- 1. 构建期就编译好了 ---------- */
    QL_CHECK_EQ(ui->node_count, 3);
    QL_CHECK_EQ(ui->nodes[1].style->backdrop_blur, 0);
    QL_CHECK_EQ(ui->nodes[2].style->backdrop_blur, 6);
    QL_CHECK_EQ(ui->nodes[2].style->bg_alpha, 20);      /* rgba(...,0.08) */

    QLRect glass = ql_ui_node_rect(&ui->nodes[2]);
    QL_CHECK_EQ(glass.x, 24);
    QL_CHECK_EQ(glass.w, 80);

    /* ---------- 2. CPU：硬边糊成渐变，区域外不动 ---------- */
    {
        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QL_CHECK(disp != NULL);
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, ROOT_BG);
        ql_ui_render_canvas(ui, &canvas);

        ql_coord_t my = glass.y + glass.h / 2;

        /* 区域外：硬边只是硬边（左边纯白、右边纯深灰） */
        /* 半透明填充会让像素带上 alpha，比较 RGB 时一律只取低 24 位 */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, my) & 0xFFFFFFu, 0xFFFFFFu);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 23, my) & 0xFFFFFFu, 0xFFFFFFu);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 110, my) & 0xFFFFFFu, ROOT_BG);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 5, 5) & 0xFFFFFFu, 0xFFFFFFu);
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, 100, 5) & 0xFFFFFFu, ROOT_BG);

        /* 区域外的那条硬边：中间色应该是 0 个 */
        int outside = ramp_count_cpu(disp, my, 0, 24);
        printf("  玻璃左侧（未模糊）的中间色像素：%d\n", outside);
        QL_CHECK_EQ(outside, 0);

        /* 玻璃里：硬边被摊开成一段渐变 */
        int inside = ramp_count_cpu(disp, my, (int)glass.x, (int)ql_rect_right(glass));
        printf("  玻璃内（模糊后）的中间色像素：%d\n", inside);
        QL_CHECK(inside >= 6);

        /*
         * 玻璃自己的白 8% 叠在模糊结果之上：左侧仍然很白（模糊把硬边摊开，
         * 靠边的地方会略微被压暗，所以判"接近白"而不是"纯白"）
         */
        {
            int lv = (int)QL_COLOR_GET_R(ql_disp_mem_pixel(disp, 30, my));
            printf("  玻璃左侧（模糊后）亮度 %d\n", lv);
            QL_CHECK(lv > 0xE0);
        }
        int right = (int)QL_COLOR_GET_R(ql_disp_mem_pixel(disp, 100, my));
        printf("  玻璃右侧底色被 8%% 白提亮到 %d（原 16）\n", right);
        QL_CHECK(right > 24 && right < 48);

        ql_disp_mem_destroy(disp);
    }

    /* ---------- 3. GPU：同一组性质断言 ---------- */
#if defined(QLGL_ENABLE_GL)
    {
        QLGfxGL *gfx = ql_gfx_gl_create("blur", W, H, QL_GFX_GL_OFFSCREEN);
        if (gfx == NULL) {
            printf("  [SKIP] GL：无法创建上下文\n");
        } else {
            ql_gfx_gl_begin(gfx, ROOT_BG);
            ql_ui_render(ui, ql_gfx_gl_draw(gfx));
            ql_gfx_gl_end(gfx);
            ql_gfx_gl_finish(gfx);

            uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4u);
            QL_CHECK(rgba != NULL);
            if (rgba != NULL) {
                QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);

                ql_coord_t my = glass.y + glass.h / 2;
                int ginside = ramp_count_rgba(rgba, W * 4, (int)my, (int)glass.x,
                                         (int)ql_rect_right(glass));
                int goutside = ramp_count_rgba(rgba, W * 4, (int)my, 0, 24);
                printf("  GPU：玻璃内中间色 %d 个，玻璃左侧 %d 个\n", ginside,
                       goutside);
                QL_CHECK(ginside >= 6);
                QL_CHECK_EQ(goutside, 0);

                /* 区域外必须原样：白还是白、底色还是底色 */
                {
                    const uint8_t *p10 = rgba + ((size_t)my * W + 10u) * 4u;
                    const uint8_t *p110 = rgba + ((size_t)my * W + 110u) * 4u;
                    QL_CHECK(p10[0] > 240 && p10[1] > 240 && p10[2] > 240);
                    QL_CHECK(p110[0] >= 8 && p110[0] <= 24);
                }
                ql_mem_free(rgba);
            }
            ql_gfx_gl_destroy(gfx);
        }
    }
#endif

    ql_ui_free(ui);

    /*
     * ---------- 4. 带 transform 的玻璃：糊的位置必须跟着元素走 ----------
     *
     * 场景：白条 0..40 与 80..120，中间是深色；玻璃在 20..80，动画平移 +40px
     * 之后落在 60..100。
     *
     *   正确：玻璃里只有 x=80 那条硬边被糊开 → 62..98 有中间色，42..58 没有；
     *   错误（按未平移的 20..80 采样）：玻璃里糊的是 x=40 那条边 → 正好反过来。
     */
    {
        const char *html = "<div id=r><div id=w></div><div id=w2></div>"
                           "<div id=g></div></div>";
        const char *css =
            "#r { width: 120px; height: 80px; background-color: #101010 }"
            "#w { position: absolute; left: 0px; top: 0px; width: 40px;"
            "     height: 80px; background-color: #FFFFFF }"
            "#w2 { position: absolute; left: 80px; top: 0px; width: 40px;"
            "      height: 80px; background-color: #FFFFFF }"
            "#g { position: absolute; left: 20px; top: 20px; width: 60px;"
            "     height: 40px; background-color: rgba(255,255,255,0.08);"
            "     backdrop-filter: blur(6px);"
            "     animation: mv 1000ms linear infinite alternate }"
            "@keyframes mv { from { transform: translate(0px, 0px); }"
            "                to   { transform: translate(40px, 0px); } }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *tu = ql_ui_build(doc);
            QL_CHECK(tu != NULL);
            if (tu != NULL) {
                ql_ui_layout(tu, W, H);
                (void)ql_ui_anim_tick(tu, 0);       /* 第一帧建立动画起点 */
                (void)ql_ui_anim_tick(tu, 1000);    /* 走完：平移 40px */

                QLUINodeRT *gn = NULL;
                for (uint16_t i = 0; i < tu->node_count; i++)
                    if (ql_ui_node_style(&tu->nodes[i])->backdrop_blur > 0)
                        gn = &tu->nodes[i];
                QL_CHECK(gn != NULL);
                if (gn != NULL) {
                    QL_CHECK_EQ(ql_ui_node_style(gn)->tf_x, 40);

                    const int my = 40;
                    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
                    QLCanvas canvas;
                    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
                    ql_disp_mem_clear(disp, ROOT_BG);
                    ql_ui_render_canvas(tu, &canvas);

                    /* 平移后的玻璃占 60..100：里面只有 x=80 那条边 */
                    const int inside = ramp_count_cpu(disp, my, 62, 98);
                    const int left = ramp_count_cpu(disp, my, 42, 58);
                    printf("  平移后 CPU：玻璃内中间色 %d，原位置(42..58) %d\n",
                           inside, left);
                    QL_CHECK(inside >= 6);
                    QL_CHECK_EQ(left, 0);

#if defined(QLGL_ENABLE_GL)
                    QLGfxGL *gfx = ql_gfx_gl_create("blur-mv", W, H,
                                                    QL_GFX_GL_OFFSCREEN);
                    if (gfx == NULL) {
                        printf("  [SKIP] GL：无法创建上下文\n");
                    } else {
                        ql_gfx_gl_begin(gfx, ROOT_BG);
                        ql_ui_render(tu, ql_gfx_gl_draw(gfx));
                        ql_gfx_gl_end(gfx);
                        ql_gfx_gl_finish(gfx);
                        uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4u);
                        QL_CHECK(rgba != NULL);
                        if (rgba != NULL) {
                            QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);
                            const int gin = ramp_count_rgba(rgba, W * 4, my, 62, 98);
                            const int gleft = ramp_count_rgba(rgba, W * 4, my, 42, 58);
                            printf("  平移后 GPU：玻璃内中间色 %d，原位置(42..58) %d\n",
                                   gin, gleft);
                            QL_CHECK(gin >= 6);
                            QL_CHECK_EQ(gleft, 0);
                            ql_mem_free(rgba);
                        }
                        ql_gfx_gl_destroy(gfx);
                    }
#endif
                    ql_disp_mem_destroy(disp);
                }
                ql_ui_free(tu);
            }
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
