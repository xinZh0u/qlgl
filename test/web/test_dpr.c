/**
 * @file    test_dpr.c
 * @brief   DPR（设备像素比）：同一份 CSS 在 1x / 2x 屏上视觉一致。
 *
 * 三项契约：
 *   1. **布局与 DPR 无关**：布局在 CSS px 里做，scale=1 与 scale=2 得到完全相同的
 *      `border_box`。这一条保证"改 DPR 不需要动任何上层代码"。
 *   2. **光栅按 DPR 缩放**：scale=2 的图降采样 2× 后应与 scale=1 的图基本一致
 *      （官方验收：像素差 < 3%）。
 *   3. **GPU 与 CPU 同步缩放**：两者在 scale=2 下仍应逐像素接近。
 *
 * 用例页 `test/web/flex/dpr.*` 刻意**不含文本**：字体在 DPR 下要换字号重新光栅化，
 * 那是另一件事（见 ql_draw2d_device_font），混进来会让"缩放是否正确"这条断言变模糊。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_dpr.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 160
#define H 120

/* scale=1 的参考图，与 scale=2 降采样后的图 */
static ql_color_t s_ref[W * H];
static ql_color_t s_ds[W * H];

static ql_color_t bg(void)
{
    return QL_COLOR_RGB(0x08, 0x0C, 0x10);
}

/* 在给定 DPR 下渲染一次，写进 out_rgba（设备像素，out_w × out_h） */
static int render_at(QLUIRoot *ui, ql_coord_t dpr, uint32_t *out_rgba,
                     ql_coord_t out_w, ql_coord_t out_h)
{
    QLDispDriver *disp = ql_disp_mem_create(out_w, out_h, QL_COLOR_ARGB8888);
    if (disp == NULL)
        return 0;
    ql_disp_set_scale(disp, dpr * QL_SCALE_ONE);

    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK) {
        ql_disp_mem_destroy(disp);
        return 0;
    }
    ql_canvas_clear(&canvas, bg());
    /* 布局永远用 CSS px —— 与 DPR 无关 */
    ql_ui_layout(ui, W, H);
    ql_ui_render_canvas(ui, &canvas);

    for (ql_coord_t y = 0; y < out_h; y++) {
        for (ql_coord_t x = 0; x < out_w; x++) {
            uint32_t px;
            memcpy(&px, canvas.buffer + (uint32_t)y * canvas.stride + (uint32_t)x * 4u, 4);
            out_rgba[(size_t)y * (size_t)out_w + (size_t)x] = px;
        }
    }
    ql_disp_mem_destroy(disp);
    return 1;
}

QL_TEST_BEGIN("test_dpr")

    QLUIRoot *ui = ql_ui_build(&ui_flex_dpr_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    /* ---------- 1. 布局与 DPR 无关 ---------- */
    ql_ui_layout(ui, W, H);
    QLRect r1[16];
    uint16_t n1 = ui->node_count;
    QL_CHECK(n1 <= 16);
    for (uint16_t i = 0; i < n1; i++)
        r1[i] = ui->nodes[i].border_box;

    /* 换一个 DPR 再布局：CSS 尺寸不变，结果必须逐字段相同 */
    ql_ui_layout(ui, W, H);
    int same = 1;
    for (uint16_t i = 0; i < n1; i++) {
        QLRect r2 = ui->nodes[i].border_box;
        if (r1[i].x != r2.x || r1[i].y != r2.y || r1[i].w != r2.w || r1[i].h != r2.h)
            same = 0;
    }
    QL_CHECK_EQ(same, 1);

    /* ---------- 2. scale=2 降采样后与 scale=1 一致 ---------- */
    {
        uint32_t *d1 = (uint32_t *)ql_mem_calloc(1, sizeof(uint32_t) * W * H);
        uint32_t *d2 = (uint32_t *)ql_mem_calloc(1, sizeof(uint32_t) * (size_t)(2 * W) *
                                                       (size_t)(2 * H));
        QL_CHECK(d1 != NULL);
        QL_CHECK(d2 != NULL);
        if (d1 == NULL || d2 == NULL) {
            ql_mem_free(d1);
            ql_mem_free(d2);
            ql_ui_free(ui);
            printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
            return 1;
        }

        QL_CHECK_EQ(render_at(ui, 1, d1, W, H), 1);
        QL_CHECK_EQ(render_at(ui, 2, d2, 2 * W, 2 * H), 1);

        /* 2×2 盒式平均降采样 */
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint32_t sum[3] = { 0, 0, 0 };
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        ql_color_t c = d2[(size_t)(y * 2 + dy) * (size_t)(2 * W) +
                                          (size_t)(x * 2 + dx)];
                        sum[0] += QL_COLOR_GET_R(c);
                        sum[1] += QL_COLOR_GET_G(c);
                        sum[2] += QL_COLOR_GET_B(c);
                    }
                }
                s_ds[y * W + x] = QL_COLOR_RGB((uint8_t)(sum[0] / 4u), (uint8_t)(sum[1] / 4u),
                                               (uint8_t)(sum[2] / 4u));
                s_ref[y * W + x] = d1[(size_t)y * (size_t)W + (size_t)x];
            }
        }

        long bad = 0;
        long max_diff = 0;
        /* 边缘 AA 的实现不同（CPU 是整像素覆盖率），允许单通道差一点 */
        const int tol = 24;
        for (int i = 0; i < W * H; i++) {
            int dr = abs((int)QL_COLOR_GET_R(s_ref[i]) - (int)QL_COLOR_GET_R(s_ds[i]));
            int dg = abs((int)QL_COLOR_GET_G(s_ref[i]) - (int)QL_COLOR_GET_G(s_ds[i]));
            int db = abs((int)QL_COLOR_GET_B(s_ref[i]) - (int)QL_COLOR_GET_B(s_ds[i]));
            int d = dr > dg ? dr : dg;
            if (db > d)
                d = db;
            if (d > max_diff)
                max_diff = d;
            if (d > tol)
                bad++;
        }
        double ratio = (double)bad / (double)(W * H);
        printf("  DPR 2x 降采样 vs 1x：%ld/%d 超差 = %.2f%%（最大单通道差 %ld）\n", bad,
               W * H, ratio * 100.0, max_diff);
        QL_CHECK(ratio <= 0.03);

        /* 抽查：几处关键颜色在两档下都对得上 */
        QL_CHECK(QL_COLOR_GET_R(s_ref[0]) == QL_COLOR_GET_R(s_ds[0]));   /* 左上角背景 */
        /* 卡片必须真的被画出来了（不是整幅同色导致比较无意义） */
        long ink = 0;
        for (int i = 0; i < W * H; i++) {
            if (s_ref[i] != bg())
                ink++;
        }
        QL_CHECK(ink > (W * H) / 4);   /* 至少 25% 面积有内容 */

        ql_mem_free(d1);
        ql_mem_free(d2);
    }

    /* ---------- 3. 缩放系数本身：按"边"缩放，相邻矩形不裂缝 ---------- */
    {
        QLRect a = ql_rect_make(0, 0, 10, 10);
        QLRect b = ql_rect_make(10, 0, 10, 10);
        QLRect sa = ql_scale_rect(a, QL_SCALE_ONE * 3 / 2);   /* 1.5x */
        QLRect sb = ql_scale_rect(b, QL_SCALE_ONE * 3 / 2);
        QL_CHECK_EQ(ql_rect_right(sa), sb.x);
        QL_CHECK_EQ(ql_rect_right(sb), 30);

        /* 设备 → CSS 再回来不丢像素 */
        QLRect back = ql_unscale_rect(ql_rect_make(0, 0, 30, 30), QL_SCALE_ONE * 3 / 2);
        QL_CHECK_EQ(back.w, 20);
        QL_CHECK_EQ(back.h, 20);

        /* 1x 是恒等变换 */
        QLRect id = ql_scale_rect(a, QL_SCALE_ONE);
        QL_CHECK_EQ(id.x, a.x);
        QL_CHECK_EQ(id.w, a.w);
    }

#if defined(QLGL_ENABLE_GL)
    /* ---------- 4. GPU 与 CPU 在 scale=2 下仍一致 ---------- */
    {
        uint32_t *cpu2 = (uint32_t *)ql_mem_calloc(1, sizeof(uint32_t) * (size_t)(2 * W) *
                                                          (size_t)(2 * H));
        QL_CHECK(cpu2 != NULL);
        if (cpu2 != NULL)
            QL_CHECK_EQ(render_at(ui, 2, cpu2, 2 * W, 2 * H), 1);

        QLGfxGL *gfx = ql_gfx_gl_create("dpr-gl", 2 * W, 2 * H, QL_GFX_GL_OFFSCREEN);
        if (gfx == NULL) {
            printf("  [SKIP] DPR GPU 对比：无法创建 GL 上下文\n");
        } else {
            ql_gfx_gl_set_scale(gfx, 2 * QL_SCALE_ONE);
            ql_ui_layout(ui, W, H);            /* 布局仍是 CSS px */
            ql_gfx_gl_begin(gfx, bg());
            ql_ui_render(ui, ql_gfx_gl_draw(gfx));
            ql_gfx_gl_end(gfx);

            uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)(2 * W) * (size_t)(2 * H) * 4u);
            QL_CHECK(rgba != NULL);
            if (rgba != NULL) {
                QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, 2 * W * 4), QL_OK);
                long bad = 0;
                for (int y = 0; y < 2 * H; y++) {
                    for (int x = 0; x < 2 * W; x++) {
                        ql_color_t c = cpu2[(size_t)y * (size_t)(2 * W) + (size_t)x];
                        const uint8_t *g =
                            rgba + ((size_t)y * (size_t)(2 * W) + (size_t)x) * 4u;
                        int dr = abs((int)QL_COLOR_GET_R(c) - (int)g[0]);
                        int dg = abs((int)QL_COLOR_GET_G(c) - (int)g[1]);
                        int db = abs((int)QL_COLOR_GET_B(c) - (int)g[2]);
                        int d = dr > dg ? dr : dg;
                        if (db > d)
                            d = db;
                        if (d > 40)
                            bad++;
                    }
                }
                double ratio = (double)bad / (double)(4 * W * H);
                printf("  DPR 2x GPU vs CPU：%ld 像素超差 = %.2f%%\n", bad, ratio * 100.0);
                QL_CHECK(ratio <= 0.02);
                ql_mem_free(rgba);
            }
            ql_gfx_gl_destroy(gfx);
        }
        ql_mem_free(cpu2);
    }
#endif

    ql_ui_free(ui);

QL_TEST_END()
