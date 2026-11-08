/**
 * @file    test_effects.c
 * @brief   渐变与软阴影的像素级断言（CPU 路径 + GPU/CPU 一致性由 test_gl_vs_cpu 守）。
 */
#include "../ql_test.h"

#define W 120
#define H 120

static QLDispDriver *s_disp;
static QLCanvas s_canvas;

static void fresh(ql_color_t bg)
{
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, bg);
}

static ql_color_t px(ql_coord_t x, ql_coord_t y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

/* 单通道近似比较 */
static int near(ql_color_t a, ql_color_t b, int tol)
{
    int dr = (int)QL_COLOR_GET_R(a) - (int)QL_COLOR_GET_R(b);
    int dg = (int)QL_COLOR_GET_G(a) - (int)QL_COLOR_GET_G(b);
    int db = (int)QL_COLOR_GET_B(a) - (int)QL_COLOR_GET_B(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tol && dg <= tol && db <= tol;
}

QL_TEST_BEGIN("test_effects")

    const ql_color_t bg = QL_COLOR_RGB(0x00, 0x00, 0x00);
    const ql_color_t c0 = QL_COLOR_RGB(0x00, 0x00, 0xFF);   /* 蓝 */
    const ql_color_t c1 = QL_COLOR_RGB(0xFF, 0x00, 0x00);   /* 红 */

    /* ---------- 1. 垂直渐变：上蓝下红，中间是过渡 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 0,
                                       c0, c1, QL_GRADIENT_VERTICAL, 255);
    QL_CHECK(near(px(60, 10), c0, 8));
    QL_CHECK(near(px(60, 109), c1, 8));
    ql_color_t mid = px(60, 60);
    QL_CHECK(QL_COLOR_GET_R(mid) > 100 && QL_COLOR_GET_R(mid) < 160);   /* 半程 ≈ 128 */
    QL_CHECK(QL_COLOR_GET_B(mid) > 100 && QL_COLOR_GET_B(mid) < 160);
    /* 同 y 的整行颜色相同（垂直渐变的快路径特征） */
    QL_CHECK_EQ(px(11, 60), px(108, 60));

    /* ---------- 2. 水平渐变：左蓝右红，同列颜色相同 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 0,
                                       c0, c1, QL_GRADIENT_HORIZONTAL, 255);
    QL_CHECK(near(px(10, 60), c0, 8));
    QL_CHECK(near(px(109, 60), c1, 8));
    QL_CHECK_EQ(px(60, 11), px(60, 108));      /* 同一列相同 */

    /* ---------- 3. 渐变 + 圆角：角上不填 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(10, 10, 100, 100), 20,
                                       c0, c1, QL_GRADIENT_VERTICAL, 255);
    QL_CHECK_EQ(px(10, 10), bg);               /* 左上角外 */
    QL_CHECK_EQ(px(109, 10), bg);              /* 右上角外 */
    QL_CHECK(near(px(60, 11), c0, 10));        /* 顶边中点仍然填 */
    QL_CHECK(near(px(60, 108), c1, 10));       /* 底边中点仍然填 */

    /* ---------- 4. 渐变 alpha=0 是空操作 ---------- */
    fresh(bg);
    ql_canvas_fill_round_rect_gradient(&s_canvas, ql_rect_make(0, 0, W, H), 0,
                                       c0, c1, QL_GRADIENT_VERTICAL, 0);
    QL_CHECK_EQ(px(60, 60), bg);

    /* ---------- 5. 软阴影：紧贴边缘有暗晕，远处没有，内部不受影响 ---------- */
    const ql_color_t white = QL_COLOR_RGB(0xFF, 0xFF, 0xFF);
    fresh(white);
    QLRect body = ql_rect_make(30, 30, 60, 60);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 10, 0, 4, QL_COLOR_RGB(0, 0, 0), 255);
    ql_canvas_fill_round_rect(&s_canvas, body, 0, QL_COLOR_RGB(0x20, 0x20, 0x20));

    /* 正下方（偏移方向）应该有渐变暗晕 */
    ql_color_t s1 = px(60, 92);    /* 底边下 2px */
    ql_color_t s2 = px(60, 99);    /* 更远 */
    QL_CHECK(QL_COLOR_GET_R(s1) < 0xF0);       /* 明显变暗 */
    QL_CHECK(QL_COLOR_GET_R(s1) < QL_COLOR_GET_R(s2));   /* 越远越淡 */
    QL_CHECK(QL_COLOR_GET_R(s2) < 0xFF);
    /* 超出 blur 范围应完全恢复 */
    QL_CHECK_EQ(px(60, 110), white);
    /* 上方：偏移为 +4、模糊 10，阴影最远只到 y = 30+4-10 = 24，再往上必须干净 */
    QL_CHECK_EQ(px(60, 20), white);
    int r27 = QL_COLOR_GET_R(px(60, 27));          /* 27 落在 24..34 的衰减带内 */
    QL_CHECK(r27 > 140 && r27 < 230);
    /* 本体内部不受阴影影响 */
    QL_CHECK_EQ(px(60, 60), QL_COLOR_RGB(0x20, 0x20, 0x20));

    /* ---------- 6. blur = 0：硬边阴影 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 0, 0, 6, QL_COLOR_RGB(0, 0, 0), 255);
    ql_canvas_fill_round_rect(&s_canvas, body, 0, QL_COLOR_RGB(0x20, 0x20, 0x20));
    QL_CHECK_EQ(px(60, 93), QL_COLOR_RGB(0, 0, 0));   /* 偏移 6px 内是纯黑 */
    QL_CHECK_EQ(px(60, 97), white);                   /* 6px 外立即恢复 */

    /* ---------- 7. 圆角阴影：角外侧也有晕 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 16, 8, 0, 0, QL_COLOR_RGB(0, 0, 0), 255);
    QL_CHECK(QL_COLOR_GET_R(px(30, 30)) < 0xF0);      /* 圆角外沿 */
    QL_CHECK_EQ(px(3, 3), white);                     /* 远角处没有 */

    /* ---------- 8. shadow alpha = 0 是空操作 ---------- */
    fresh(white);
    ql_canvas_fill_shadow(&s_canvas, body, 0, 10, 0, 4, QL_COLOR_RGB(0, 0, 0), 0);
    QL_CHECK_EQ(px(60, 95), white);

    ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
