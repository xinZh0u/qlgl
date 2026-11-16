/**
 * @file    test_clip_path.c
 * @brief   `clip-path: path()`：PlutoVG 解析 + 展平后接现有覆盖率遮罩。
 *
 * 只测"形状边界"（内/外/曲线），不比对抗锯齿逐像素：
 *   1. 直线三角形：内部有背景色、外部保持父背景；
 *   2. 圆弧（A 命令 → 三次贝塞尔展平）：圆心在内、四角在外；
 *   3. 多子路径 / 缺引号 / 少于 3 点：明确拒绝；
 *   4. 没有 SVG 的构建：`path()` 明确报"需要 QLGL_BUILD_SVG"。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 64
#define H 64
#define BG 0xFFFFFFu
#define FG 0xFF0000u

static QLDispDriver *s_disp;
static QLCanvas s_cv;

#if QL_FEAT_IMG_SVG
static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, (ql_coord_t)x, (ql_coord_t)y);
}
#endif

/* 解析 → 构建 → 布局 → 画一帧；返回 1 = 成功 */
static int render(const char *css)
{
    QLUIDoc *doc = ql_ui_load_text("<div id='a'></div>", css);
    if (doc == NULL)
        return 0;
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return 0;
    }
    ql_ui_layout(ui, W, H);
    ql_disp_mem_clear(s_disp, BG);
    ql_ui_render_canvas(ui, &s_cv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return 1;
}

#if QL_FEAT_IMG_SVG
static int is_fg(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 0xC0 && QL_COLOR_GET_G(c) < 0x40 &&
           QL_COLOR_GET_B(c) < 0x40;
}

static int is_bg(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 0xE0 && QL_COLOR_GET_G(c) > 0xE0 &&
           QL_COLOR_GET_B(c) > 0xE0;
}
#endif

QL_TEST_BEGIN("test_clip_path")
    s_disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    QL_CHECK(s_disp != NULL);
    if (s_disp != NULL) {
        QL_CHECK_EQ(ql_canvas_from_disp(&s_cv, s_disp), QL_OK);
    }

#if QL_FEAT_IMG_SVG
    /* ---- 1. 直角三角形 (0,0)-(64,0)-(0,64)：x+y < 64 在内部 ---- */
    QL_CHECK(render("#a { width: 64px; height: 64px; background-color: #ff0000;"
                    " clip-path: path(\"M0 0 L64 0 L0 64 Z\"); }"));
    QL_CHECK(is_fg(px(8, 8)));
    QL_CHECK(is_fg(px(40, 8)));
    QL_CHECK(is_fg(px(8, 40)));
    QL_CHECK(is_bg(px(56, 56)));
    QL_CHECK(is_bg(px(56, 20)));

    /* ---- 2. 圆弧：半径 28 的圆，A 命令会被展平成贝塞尔 ---- */
    QL_CHECK(render("#a { width: 64px; height: 64px; background-color: #ff0000;"
                    " clip-path: path(\"M32 4 A28 28 0 1 1 32 60 A28 28 0 1 1 32 4 Z\"); }"));
    QL_CHECK(is_fg(px(32, 32)));
    QL_CHECK(is_bg(px(2, 2)));
    QL_CHECK(is_bg(px(61, 61)));

    /* ---- 3. 拒绝路径：多子路径 / 无引号 / 2 个点 ---- */
    QL_CHECK(!render("#a { clip-path: path('M0 0 L10 0 L10 10 Z"
                     " M20 20 L30 20 L30 30 Z'); }"));
    QL_CHECK(strstr(ql_ui_text_last_error(), "子路径") != NULL);

    QL_CHECK(!render("#a { clip-path: path(M0 0 L10 0 L10 10 Z); }"));
    QL_CHECK(strstr(ql_ui_text_last_error(), "引号") != NULL);

    QL_CHECK(!render("#a { clip-path: path('M0 0 L10 10'); }"));
    QL_CHECK(strstr(ql_ui_text_last_error(), "3 个顶点") != NULL);
#else
    /* 没有 PlutoVG 时必须明确失败，并指出开关 */
    QL_CHECK(!render("#a { clip-path: path('M0 0 L10 0 L10 10 Z'); }"));
    QL_CHECK(strstr(ql_ui_text_last_error(), "QLGL_BUILD_SVG") != NULL);
#endif

    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
QL_TEST_END()
