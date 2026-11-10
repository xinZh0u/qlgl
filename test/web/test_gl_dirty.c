/**
 * @file    test_gl_dirty.c
 * @brief   保留式脏区（FBO 画布）的正确性：局部重绘 == 全量重绘。
 *
 * 为什么需要这块 FBO：窗口是双缓冲，`eglSwapBuffers` 之后后缓冲内容**没有任何保证**，
 * 所以"只重画脏区"在窗口上会把其余部分清成底色（曾经真的发生过：动画一停，
 * 卡片、标题、提示文字全都不见了）。保留模式把画布放在自己的 FBO 里，帧末整块 blit。
 *
 * 这个测试守三条：
 *   1. FBO 刚建的那一帧 `ql_gfx_gl_begin_rects()` 必须返回 1（要求整屏重画）；
 *   2. 之后只清 + 重画脏区，结果与"整屏重画"**逐像素相同**；
 *   3. 动画跑一段之后（多帧累积局部重绘），结果仍与整屏重画一致——累积不漂移。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"
#include "ui_flex_anim.h"

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("== test_gl_dirty ==\n  跳过：未编译 GL 后端\n  0 项检查，0 项失败\n");
    ql_test_checks = 0;
    ql_test_failures = 0;
    return 0;
}
#else

#define W 360
#define H 260
#define BG QL_COLOR_RGB(0x08, 0x0A, 0x10)

static uint32_t s_now;

static uint32_t fake_now(void)
{
    return s_now;
}

/* 整屏重画一遍，返回参考图（RGBA，自上而下） */
static void render_full(QLGfxGL *g, QLUIRoot *ui, uint8_t *out)
{
    QLDraw2D *d2d = ql_gfx_gl_draw(g);
    ql_gfx_gl_begin(g, BG);
    ql_ui_render(ui, d2d);
    ql_gfx_gl_end(g);
    ql_gfx_gl_finish(g);
    QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, out, W * 4), QL_OK);
}

static long diff_pixels(const uint8_t *a, const uint8_t *b)
{
    long n = 0;
    for (long i = 0; i < (long)W * H; i++) {
        const uint8_t *p = a + i * 4;
        const uint8_t *q = b + i * 4;
        if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2])
            n++;
    }
    return n;
}

QL_TEST_BEGIN("test_gl_dirty")

    ql_anim_set_time_source(fake_now);

    QLGfxGL *g = ql_gfx_gl_create("gl-dirty", W, H, QL_GFX_GL_OFFSCREEN);
    if (g == NULL) {
        QL_TEST_GL_SKIP("无法创建 GL 上下文（无 EGL/X11？）");
        qlgl_shutdown();
        return ql_test_gl_required() ? 1 : 0;
    }

    QL_CHECK_EQ(ql_gfx_gl_set_retained(g, 1), QL_OK);

    uint8_t *ref = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
    uint8_t *got = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
    QL_CHECK(ref != NULL && got != NULL);

    s_now = 0;
    QLUIRoot *ui = ql_ui_build(&ui_flex_anim_doc);
    QL_CHECK(ui != NULL);
    ql_ui_layout(ui, W, H);
    QLDraw2D *d2d = ql_gfx_gl_draw(g);

    /* ---------- 1. FBO 刚建：必须要求整屏重画 ---------- */
    {
        QLRect areas[8];
        s_now = 1000;
        (void)ql_ui_anim_tick(ui, s_now);
        int n = ql_ui_consume_dirty(ui, areas, 8);
        QL_CHECK(n > 0);
        QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, areas, n), 1);
        ql_ui_render(ui, d2d);
        ql_gfx_gl_end(g);
        ql_gfx_gl_finish(g);
    }

    /* ---------- 2. 之后只画脏区：与整屏重画逐像素相同 ---------- */
    for (int step = 1; step <= 12; step++) {
        s_now = 1000u + (uint32_t)step * 40u;
        (void)ql_ui_anim_tick(ui, s_now);

        QLRect areas[8];
        int n = ql_ui_consume_dirty(ui, areas, 8);
        if (n == 0)
            continue;

        QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, areas, n), 0);
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(ui, d2d, areas[i]);
        ql_gfx_gl_end(g);
        ql_gfx_gl_finish(g);
        QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, got, W * 4), QL_OK);

        render_full(g, ui, ref);
        long bad = diff_pixels(ref, got);
        if (bad != 0)
            printf("  step %d：局部重绘与整屏重画差 %ld 像素\n", step, bad);
        QL_CHECK_EQ(bad, 0);
    }

    /* ---------- 3. 动画跨过终点（fill: forwards 停在末帧）后仍然一致 ---------- */
    for (int step = 0; step < 4; step++) {
        s_now = 2000u + (uint32_t)step * 100u;
        (void)ql_ui_anim_tick(ui, s_now);
        QLRect areas[8];
        int n = ql_ui_consume_dirty(ui, areas, 8);
        if (n == 0)
            continue;
        QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, areas, n), 0);
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(ui, d2d, areas[i]);
        ql_gfx_gl_end(g);
        ql_gfx_gl_finish(g);
        QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, got, W * 4), QL_OK);

        render_full(g, ui, ref);
        QL_CHECK_EQ(diff_pixels(ref, got), 0);
    }

    /* ---------- 4. 关掉保留模式：退回每帧整屏清，不残留 ---------- */
    QL_CHECK_EQ(ql_gfx_gl_set_retained(g, 0), QL_OK);
    {
        QLRect areas[1] = { ql_rect_make(10, 10, 20, 20) };
        QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, areas, 1), 1);
        ql_gfx_gl_end(g);
        ql_gfx_gl_finish(g);
        QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, got, W * 4), QL_OK);
        QL_CHECK_EQ(got[(5 * W + 5) * 4 + 0], 8);      /* 清成 BG */
        QL_CHECK_EQ(got[(200 * W + 300) * 4 + 0], 8);
    }

    /*
     * ---------- 5. 脏区重绘 + 混合模式图层 ----------
     *
     * 守一个**只在脏区重绘时才出现**的 bug：`glBlitFramebuffer` 也受 scissor 影响，
     * 而混合模式要先把"图层背后那一块"拷进一张纹理；那张纹理的目标矩形在 (0,0)，
     * 脏区重绘时 scissor 是"屏幕上那一小块"，两块不重叠 → 整次拷贝被**静默丢弃**，
     * 于是片元读到的是**上一次**抓的背景（或者全黑）：multiply/overlay/darken
     * 全黑、screen/difference 退化成原色。
     *
     * 整屏重绘时 scissor 恰好覆盖 (0,0)，所以只测整屏的用例永远是绿的 ——
     * 这里必须让"背后的颜色变了、而我只重画这一小块"：
     * 先整屏画一张红底（把背景纹理灌成红），再只重画黄底那一块。
     */
    QL_CHECK_EQ(ql_gfx_gl_set_retained(g, 1), QL_OK);
    {
        const char *html = "<div id=r><div id=b></div></div>";
        const char *css_red =
            "#r{width:360px;height:260px;background-color:#FF0000}"
            "#b{position:absolute;left:40px;top:40px;width:80px;height:50px;"
            "background-color:#3366FF;mix-blend-mode:multiply}";
        const char *css_yellow =
            "#r{width:360px;height:260px;background-color:#FFCC00}"
            "#b{position:absolute;left:40px;top:40px;width:80px;height:50px;"
            "background-color:#3366FF;mix-blend-mode:multiply}";
        QLUIDoc *dr = ql_ui_load_text(html, css_red);
        QLUIDoc *dy = ql_ui_load_text(html, css_yellow);
        QL_CHECK(dr != NULL && dy != NULL);
        if (dr != NULL && dy != NULL) {
            QLUIRoot *ur = ql_ui_build(dr);
            QLUIRoot *uy = ql_ui_build(dy);
            QL_CHECK(ur != NULL && uy != NULL);
            if (ur != NULL && uy != NULL) {
                ql_ui_layout(ur, W, H);
                ql_ui_layout(uy, W, H);
                uint8_t *buf = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4);
                QL_CHECK(buf != NULL);
                if (buf != NULL) {
                    /* 红底整屏一遍：背景纹理里现在是红 */
                    QLRect all = ql_rect_make(0, 0, W, H);
                    QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, &all, 1), 1);
                    ql_ui_render(ur, d2d);
                    ql_gfx_gl_end(g);
                    ql_gfx_gl_finish(g);

                    /* 换成黄底，只重画包住那个元素的一小块 */
                    QLRect one = ql_rect_make(36, 36, 100, 60);
                    QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, BG, &one, 1), 0);
                    ql_ui_render_rect(uy, d2d, one);
                    ql_gfx_gl_end(g);
                    ql_gfx_gl_finish(g);
                    QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, buf, W * 4), QL_OK);

                    /* multiply：#3366FF × #FFCC00 = #335200（红底会得到 #330000） */
                    const uint8_t *p = buf + ((size_t)(40 + 20) * W + (40 + 20)) * 4u;
                    printf("  脏区重绘里的 multiply：%02X%02X%02X（期望 335200）\n", p[0],
                           p[1], p[2]);
                    QL_CHECK(p[2] < 0x20);
                    QL_CHECK(p[1] > 0x40 && p[1] < 0x60);
                    ql_mem_free(buf);
                }
                ql_ui_free(uy);
                ql_ui_free(ur);
            }
            ql_ui_doc_free(dy);
            ql_ui_doc_free(dr);
        }
    }

    ql_mem_free(got);
    ql_mem_free(ref);
    ql_ui_free(ui);
    ql_gfx_gl_destroy(g);
    ql_anim_set_time_source(NULL);

QL_TEST_END()
#endif
