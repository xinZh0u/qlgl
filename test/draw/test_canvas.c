/**
 * @file    test_canvas.c
 * @brief   示例：画布原语 —— 填充、圆角、描边、裁剪、混合、多格式。
 */
#include "../ql_test.h"

/* 颜色近似比较：RGB565 / L8 有量化误差 */
static int color_close(ql_color_t a, ql_color_t b, int tolerance)
{
    int dr = (int)QL_COLOR_GET_R(a) - (int)QL_COLOR_GET_R(b);
    int dg = (int)QL_COLOR_GET_G(a) - (int)QL_COLOR_GET_G(b);
    int db = (int)QL_COLOR_GET_B(a) - (int)QL_COLOR_GET_B(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tolerance && dg <= tolerance && db <= tolerance;
}

QL_TEST_BEGIN("test_canvas")

    /* ---------------- RGB888：无量化误差，可做精确断言 ---------------- */
    QLDispDriver *disp = ql_disp_mem_create(64, 48, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    QL_CHECK_EQ(disp->width, 64);
    QL_CHECK_EQ(disp->height, 48);
    QL_CHECK_STR(ql_disp_format_name(disp->format), "RGB888");
    QL_CHECK_EQ(ql_disp_format_bytes(QL_COLOR_RGB888), 3);
    QL_CHECK_EQ(ql_disp_buffer_size(QL_COLOR_RGB565, 10, 10), 200);
    QL_CHECK_EQ(ql_disp_buffer_size(QL_COLOR_L8, 10, 10), 100);
    QL_CHECK_EQ(ql_disp_buffer_size((QLColorFormat)99, 10, 10), 0);

    QLCanvas canvas;
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
    QL_CHECK_EQ(canvas.width, 64);
    QL_CHECK_EQ(canvas.clip.w, 64);

    ql_canvas_clear(&canvas, QL_COLOR_BLACK);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 0, 0), QL_COLOR_BLACK);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 63, 47), QL_COLOR_BLACK);

    /* 1. 直角填充 */
    ql_canvas_fill_rect(&canvas, ql_rect_make(4, 4, 10, 6), QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 4, 4), QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 13, 9), QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 3, 4), QL_COLOR_BLACK);    /* 左边界外 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 14, 9), QL_COLOR_BLACK);   /* 右边界外 */

    /* 2. 裁剪：超出画布的部分不写入 */
    ql_canvas_fill_rect(&canvas, ql_rect_make(60, 44, 20, 20), QL_COLOR_GREEN);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 63, 47), QL_COLOR_GREEN);
    ql_canvas_set_clip(&canvas, ql_rect_make(0, 0, 30, 30));
    QL_CHECK_EQ(canvas.clip.w, 30);
    ql_canvas_fill_rect(&canvas, ql_rect_make(25, 25, 20, 20), QL_COLOR_BLUE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 29, 29), QL_COLOR_BLUE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 30, 30), QL_COLOR_BLACK);  /* 裁剪区外 */
    ql_canvas_set_clip(&canvas, ql_rect_make(0, 0, 64, 48));

    /* 3. 圆角填充：四角被切掉，中心仍是实心 */
    ql_canvas_fill_rect(&canvas, ql_rect_make(0, 0, 64, 48), QL_COLOR_BLACK);
    QLRect rr = ql_rect_make(10, 10, 20, 20);
    ql_canvas_fill_round_rect(&canvas, rr, 6, QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 20, 20), QL_COLOR_WHITE);   /* 中心 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, 10), QL_COLOR_BLACK);   /* 左上角被切 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 29, 29), QL_COLOR_BLACK);   /* 右下角被切 */
    QL_CHECK(ql_rect_contains_round(rr, 20, 20, 6));
    QL_CHECK(!ql_rect_contains_round(rr, 10, 10, 6));
    /* 半径超过短边一半会被裁到一半，不应越界写 */
    ql_canvas_fill_round_rect(&canvas, ql_rect_make(40, 10, 10, 10), 100, QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 45, 15), QL_COLOR_RED);

    /* 4. 描边：四边有颜色，中心保持原色 */
    ql_canvas_fill_rect(&canvas, ql_rect_make(0, 0, 64, 48), QL_COLOR_BLACK);
    ql_canvas_stroke_rect(&canvas, ql_rect_make(4, 4, 20, 20), 0, 2, QL_COLOR_GREEN);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 5, 4), QL_COLOR_GREEN);     /* 上边 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 5, 23), QL_COLOR_GREEN);    /* 下边 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 4, 10), QL_COLOR_GREEN);    /* 左边 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 23, 10), QL_COLOR_GREEN);   /* 右边 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 14, 14), QL_COLOR_BLACK);   /* 内部 */

    /* 5. 圆角描边：角上不应出现直角像素 */
    ql_canvas_fill_rect(&canvas, ql_rect_make(0, 0, 64, 48), QL_COLOR_BLACK);
    ql_canvas_stroke_rect(&canvas, ql_rect_make(30, 10, 24, 24), 8, 2, QL_COLOR_BLUE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 30, 10), QL_COLOR_BLACK);   /* 角被切 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 42, 10), QL_COLOR_BLUE);    /* 上边中点 */

    /* 6. 线条 */
    ql_canvas_clear(&canvas, QL_COLOR_BLACK);
    ql_canvas_draw_hline(&canvas, 2, 20, 5, 1, QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 2, 5), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 20, 5), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 21, 5), QL_COLOR_BLACK);
    ql_canvas_draw_vline(&canvas, 40, 2, 10, 1, QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 40, 10), QL_COLOR_WHITE);
    ql_canvas_draw_line(&canvas, 0, 20, 10, 30, 1, QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 0, 20), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 5, 25), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, 30), QL_COLOR_WHITE);

    /* 7. alpha 混合：0 透明、255 覆盖、128 半透 */
    ql_canvas_clear(&canvas, QL_COLOR_BLACK);
    ql_canvas_fill_rect_alpha(&canvas, ql_rect_make(0, 0, 10, 10), QL_COLOR_WHITE, 0);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 1, 1), QL_COLOR_BLACK);
    ql_canvas_fill_rect_alpha(&canvas, ql_rect_make(0, 0, 10, 10), QL_COLOR_WHITE, 255);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 1, 1), QL_COLOR_WHITE);
    ql_canvas_clear(&canvas, QL_COLOR_BLACK);
    ql_canvas_fill_rect_alpha(&canvas, ql_rect_make(0, 0, 10, 10), QL_COLOR_WHITE, 128);
    ql_color_t mid = ql_disp_mem_pixel(disp, 1, 1);
    QL_CHECK(QL_COLOR_GET_R(mid) > 100 && QL_COLOR_GET_R(mid) < 155);

    /* 8. 像素读写 */
    ql_canvas_write_pixel(&canvas, 1, 1, QL_COLOR_RED, 255);
    QL_CHECK_EQ(ql_canvas_read_pixel(&canvas, 1, 1), QL_COLOR_RED);
    QL_CHECK_EQ(ql_canvas_read_pixel(&canvas, -1, 0), 0);          /* 越界返回 0 */
    QL_CHECK_EQ(ql_canvas_read_pixel(&canvas, 999, 0), 0);

    ql_disp_mem_destroy(disp);

    /* ---------------- RGB565：量化后比较 ---------------- */
    disp = ql_disp_mem_create(32, 24, QL_COLOR_RGB565);
    QL_CHECK(disp != NULL);
    QL_CHECK_EQ(disp->width, 32);
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);

    ql_canvas_clear(&canvas, QL_COLOR_RED);
    QL_CHECK(color_close(ql_disp_mem_pixel(disp, 1, 1), QL_COLOR_RED, 8));
    QL_CHECK(color_close(ql_disp_mem_pixel(disp, 16, 12), QL_COLOR_RED, 8));
    ql_color_t quantized = ql_canvas_quantize(&canvas, QL_COLOR_RGB(0x12, 0x34, 0x56));
    QL_CHECK(color_close(quantized, QL_COLOR_RGB(0x12, 0x34, 0x56), 8));
    QL_CHECK(quantized != QL_COLOR_RGB(0x12, 0x34, 0x56));   /* 确实发生了量化 */

    /* ---------------- L8：灰度 ---------------- */
    QLDispDriver *l8 = ql_disp_mem_create(16, 16, QL_COLOR_L8);
    QL_CHECK(l8 != NULL);
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, l8), QL_OK);
    ql_canvas_clear(&canvas, QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(l8, 1, 1), QL_COLOR_WHITE);
    ql_canvas_clear(&canvas, QL_COLOR_BLACK);
    QL_CHECK_EQ(ql_disp_mem_pixel(l8, 1, 1), QL_COLOR_BLACK);
    ql_disp_mem_destroy(l8);

    /* ---------------- ARGB8888 ---------------- */
    QLDispDriver *argb = ql_disp_mem_create(8, 8, QL_COLOR_ARGB8888);
    QL_CHECK(argb != NULL);
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, argb), QL_OK);
    ql_canvas_clear(&canvas, QL_COLOR_SKY);
    QL_CHECK_EQ(ql_disp_mem_pixel(argb, 3, 3), QL_COLOR_SKY);
    ql_disp_mem_destroy(argb);

    ql_disp_mem_destroy(disp);

QL_TEST_END()
