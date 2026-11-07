/**
 * @file    test_widget_basic.c
 * @brief   示例：QLWidget 的属性、几何换算、可见性与类型判断。
 */
#include "../ql_test.h"

QL_TEST_BEGIN("test_widget_basic")

    /* 1. 创建：QLWidget 已由 qlgl_init 注册 */
    QL_CHECK_EQ(ql_widget_get_type(), QL_TYPE_WIDGET);
    QL_CHECK(QL_TYPE_WIDGET < QL_TYPE_COUNT);

    QLWidget *w = ql_widget_new();
    QL_CHECK(w != NULL);
    QL_CHECK(ql_object_is((QLObject *)w, QL_TYPE_WIDGET));
    QL_CHECK(ql_object_is((QLObject *)w, QL_TYPE_OBJECT));   /* 含父类判断 */
    QL_CHECK_STR(ql_object_type_name((QLObject *)w), "QLWidget");

    /* 2. 默认值：透明背景、无边框、可见、尺寸 0 */
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_X).v.i, 0);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BG_ALPHA).v.i, 0);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BORDER_WIDTH).v.i, 0);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_VISIBLE).v.i, 1);
    QL_CHECK_EQ(ql_widget_width((QLObject *)w), 0);

    /* 3. 设置几何 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_X,
                                       ql_value_int(10)), QL_OK);
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_Y,
                                       ql_value_int(20)), QL_OK);
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_WIDTH,
                                       ql_value_int(100)), QL_OK);
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_HEIGHT,
                                       ql_value_int(50)), QL_OK);
    QL_CHECK_EQ(ql_widget_x((QLObject *)w), 10);
    QL_CHECK_EQ(ql_widget_y((QLObject *)w), 20);
    QL_CHECK_EQ(ql_widget_width((QLObject *)w), 100);
    QL_CHECK_EQ(ql_widget_height((QLObject *)w), 50);

    /* 4. 负尺寸被拒绝 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_WIDTH,
                                       ql_value_int(-1)), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_widget_width((QLObject *)w), 100);

    /* 5. 便捷函数 ql_widget_set_pos / set_bg / set_border */
    ql_widget_set_pos((QLObject *)w, 5, 6);
    QL_CHECK_EQ(ql_widget_x((QLObject *)w), 5);
    QL_CHECK_EQ(ql_widget_y((QLObject *)w), 6);
    ql_widget_set_bg((QLObject *)w, QL_COLOR_RED, 255);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BG_COLOR).v.u,
                QL_COLOR_RED);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BG_ALPHA).v.i, 255);
    ql_widget_set_border((QLObject *)w, 2, QL_COLOR_BLUE, 8);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BORDER_WIDTH).v.i, 2);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)w, QL_WIDGET_PROPERTY_BORDER_RADIUS).v.i, 8);

    /* 6. 局部 / 绝对矩形：子控件的坐标相对父控件 */
    QLWidget *child = ql_widget_new_ex(10, 10, 20, 20);
    QL_CHECK(child != NULL);
    QL_CHECK_EQ(ql_object_add_child((QLObject *)w, (QLObject *)child), QL_OK);

    QLRect local = ql_widget_local_rect((QLObject *)child);
    QL_CHECK_EQ(local.x, 10);
    QL_CHECK_EQ(local.y, 10);
    QL_CHECK_EQ(local.w, 20);
    QL_CHECK_EQ(local.h, 20);

    QLRect abs_child = ql_widget_abs_rect((QLObject *)child);
    QL_CHECK_EQ(abs_child.x, 15);       /* 父 x=5 + 子 x=10 */
    QL_CHECK_EQ(abs_child.y, 16);       /* 父 y=6 + 子 y=10 */
    QL_CHECK_EQ(abs_child.w, 20);

    /* 7. 命中测试用绝对坐标 */
    QL_CHECK(ql_widget_contains((QLObject *)child, 15, 16));
    QL_CHECK(ql_widget_contains((QLObject *)child, 34, 35));
    QL_CHECK(!ql_widget_contains((QLObject *)child, 14, 16));
    QL_CHECK(!ql_widget_contains((QLObject *)child, 35, 16));

    /* 8. 不可见时命中测试失败 */
    ql_widget_set_visible((QLObject *)child, 0);
    QL_CHECK_EQ(ql_widget_is_visible((QLObject *)child), 0);
    QL_CHECK(!ql_widget_contains((QLObject *)child, 20, 20));
    ql_widget_set_visible((QLObject *)child, 1);

    /* 9. 非控件对象上调用便捷函数安全无副作用 */
    QLObject *plain = ql_object_new(QL_TYPE_OBJECT);
    QL_CHECK(plain != NULL);
    QL_CHECK_EQ(ql_widget_width(plain), 0);
    ql_widget_set_pos(plain, 1, 2);                       /* 静默忽略 */
    QL_CHECK_EQ(ql_widget_x(plain), 0);
    QL_CHECK_EQ(ql_widget_local_rect(NULL).w, 0);
    ql_object_unref(plain);

    /* 10. 裁剪子控件开关 */
    QL_CHECK_EQ(ql_widget_clips_children((QLObject *)w), 0);
    QL_CHECK_EQ(ql_object_set_property((QLObject *)w, QL_WIDGET_PROPERTY_CLIP_CHILDREN,
                                       ql_value_bool(1)), QL_OK);
    QL_CHECK_EQ(ql_widget_clips_children((QLObject *)w), 1);

    /* 11. 绘制函数：默认实现存在 */
    QL_CHECK(ql_widget_get_draw((QLObject *)w) != NULL);

    ql_object_unref((QLObject *)w);   /* 级联销毁 child */

QL_TEST_END()
