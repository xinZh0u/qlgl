/**
 * @file    test_label.c
 * @brief   示例：QLLabel —— 文本所有权、自动尺寸、字体切换、对齐与像素级绘制。
 */
#include "../ql_test.h"

/* 统计画布上某个颜色的像素数，用于判断"画上去了没有" */
static int count_color(QLDispDriver *d, ql_color_t color)
{
    int n = 0;
    for (ql_coord_t y = 0; y < d->height; y++)
        for (ql_coord_t x = 0; x < d->width; x++)
            if (ql_disp_mem_pixel(d, x, y) == color)
                n++;
    return n;
}

QL_TEST_BEGIN("test_label")

    QLDispDriver *disp = ql_disp_mem_create(260, 120, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    ql_disp_set_bg_color(QL_COLOR_BLACK);

    QLWidget *root = ql_widget_new_ex(0, 0, 260, 120);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_BLACK, 255);
    ql_render_set_root((QLObject *)root);

    /* 1. 创建与默认值 */
    QL_CHECK_EQ(ql_label_get_type(), QL_TYPE_LABEL);
    QL_CHECK(ql_object_is((QLObject *)root, QL_TYPE_OBJECT));

    QLLabel *label = ql_label_new("Hello");
    QL_CHECK(label != NULL);
    QL_CHECK(ql_object_is((QLObject *)label, QL_TYPE_LABEL));
    QL_CHECK(ql_object_is((QLObject *)label, QL_TYPE_WIDGET));
    QL_CHECK_STR(ql_object_type_name((QLObject *)label), "QLLabel");
    QL_CHECK_STR(ql_label_text((QLObject *)label), "Hello");
    QL_CHECK(ql_label_font((QLObject *)label) == &ql_font_mono_12);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)label, QL_WIDGET_PROPERTY_BG_ALPHA).v.i, 0);

    /* 2. 自动尺寸：文本变化后宽高跟着变 */
    ql_coord_t w12 = 0;
    ql_coord_t h12 = 0;
    ql_font_measure_text(&ql_font_mono_12, "Hello", -1, &w12, &h12);
    QL_CHECK_EQ(ql_widget_width((QLObject *)label), w12);
    QL_CHECK_EQ(ql_widget_height((QLObject *)label), h12);
    QL_CHECK(w12 > 0 && h12 > 0);

    ql_label_set_text((QLObject *)label, "Hello, world");
    QL_CHECK(ql_widget_width((QLObject *)label) > w12);

    /* 3. 文本所有权：调用方缓冲区被改写后，标签内容不受影响 */
    {
        char buffer[16];
        strcpy(buffer, "owned");
        ql_label_set_text((QLObject *)label, buffer);
        memset(buffer, 'X', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        QL_CHECK_STR(ql_label_text((QLObject *)label), "owned");
    }

    /* 4. 切换字体 */
    ql_label_set_font((QLObject *)label, &ql_font_mono_16);
    QL_CHECK(ql_label_font((QLObject *)label) == &ql_font_mono_16);
    QL_CHECK_EQ(ql_widget_height((QLObject *)label), ql_font_mono_16.line_height);
    ql_coord_t w16 = 0;
    ql_font_measure_text(&ql_font_mono_16, "owned", -1, &w16, NULL);
    QL_CHECK_EQ(ql_widget_width((QLObject *)label), w16);
    QL_CHECK(w16 > 0);

    /* 5. 关闭自动尺寸后由调用方决定几何 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)label, QL_LABEL_PROPERTY_AUTO_SIZE,
                                       ql_value_bool(0)), QL_OK);
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(200));
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(40));
    ql_label_set_text((QLObject *)label, "ABCD");
    QL_CHECK_EQ(ql_widget_width((QLObject *)label), 200);
    QL_CHECK_EQ(ql_widget_height((QLObject *)label), 40);

    /* 6. "ABCD" 在 mono_16 下是 4 个同宽字形 */
    const QLGlyph *g = ql_font_find_glyph(&ql_font_mono_16, 'A');
    QL_CHECK(g != NULL);

    /* 7. 左对齐：第一个字形从左边界开始画 */
    ql_label_set_align((QLObject *)label, QL_TEXT_ALIGN_LEFT);
    ql_label_set_text_color((QLObject *)label, QL_COLOR_WHITE);
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_X, ql_value_int(0));
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_Y, ql_value_int(0));
    ql_object_add_child((QLObject *)root, (QLObject *)label);
    ql_render_invalidate_all();
    ql_render();

    int white_left = count_color(disp, QL_COLOR_WHITE);
    QL_CHECK(white_left > 0);                       /* 确实画出了字 */

    /* 左对齐时 x=200-1 附近不该有文字（4 字符约 40px 宽） */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 190, 8), QL_COLOR_BLACK);

    /* 8. 右对齐：同样的文本会跑到右边 */
    ql_label_set_align((QLObject *)label, QL_TEXT_ALIGN_RIGHT);
    ql_render_invalidate_all();
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 20, 8), QL_COLOR_BLACK);   /* 左边空了 */
    int white_right = count_color(disp, QL_COLOR_WHITE);
    QL_CHECK(white_right > 0);
    QL_CHECK_EQ(white_left, white_right);            /* 文字总量不变，只是位置变了 */

    /* 9. 居中对齐 */
    ql_label_set_align((QLObject *)label, QL_TEXT_ALIGN_CENTER);
    ql_render_invalidate_all();
    ql_render();
    QL_CHECK_EQ(count_color(disp, QL_COLOR_WHITE), white_left);

    /* 10. 多行：高度按行数增长，换行处额外多一行 */
    ql_object_set_property((QLObject *)label, QL_LABEL_PROPERTY_AUTO_SIZE, ql_value_bool(1));
    ql_label_set_text((QLObject *)label, "AB\nCD\nEF");
    QL_CHECK_EQ(ql_widget_height((QLObject *)label), ql_font_mono_16.line_height * 3);
    QL_CHECK_EQ(ql_widget_width((QLObject *)label), g->adv_w * 2);

    /* 11. 颜色变化 */
    ql_label_set_text_color((QLObject *)label, QL_COLOR_RED);
    ql_render_invalidate_all();
    ql_render();
    QL_CHECK_EQ(count_color(disp, QL_COLOR_WHITE), 0);
    QL_CHECK(count_color(disp, QL_COLOR_RED) > 0);

    /* 12. 空文本不占地方也不崩 */
    ql_label_set_text((QLObject *)label, "");
    QL_CHECK_EQ(ql_widget_width((QLObject *)label), 0);
    QL_CHECK_EQ(ql_widget_height((QLObject *)label), ql_font_mono_16.line_height);
    ql_render();
    ql_label_set_text((QLObject *)label, NULL);
    QL_CHECK_STR(ql_label_text((QLObject *)label), "");

    /* 13. 类型校验：TEXT 只接受字符串 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)label, QL_LABEL_PROPERTY_TEXT,
                                       ql_value_int(1)), QL_ERR_INVAL);
    /* FONT 接受任意指针，NULL 会退回默认字体 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)label, QL_LABEL_PROPERTY_FONT,
                                       ql_value_ptr(NULL)), QL_OK);
    QL_CHECK(ql_label_font((QLObject *)label) == &ql_font_mono_12);

    /* 14. 非控件对象上调用便捷函数不崩 */
    QLObject *plain = ql_object_new(QL_TYPE_OBJECT);
    QL_CHECK(plain != NULL);
    QL_CHECK_STR(ql_label_text(plain), "");
    ql_label_set_text(plain, "ignored");
    ql_label_refit(plain);
    ql_object_unref(plain);

    /* 15. 用 new_child 创建，父对象接管引用 */
    QLLabel *child = ql_label_new_child((QLObject *)root, "child", 4, 60);
    QL_CHECK(child != NULL);
    QL_CHECK_EQ(ql_object_parent((QLObject *)child), (QLObject *)root);
    QL_CHECK_EQ(ql_object_ref_count((QLObject *)child), 1);
    ql_label_set_text((QLObject *)child, "child2");
    QL_CHECK_STR(ql_label_text((QLObject *)child), "child2");

    ql_object_unref((QLObject *)root);
    ql_disp_mem_destroy(disp);

QL_TEST_END()
