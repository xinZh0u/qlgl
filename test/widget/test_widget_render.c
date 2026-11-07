/**
 * @file    test_widget_render.c
 * @brief   示例：把控件树渲染到内存显示，验证像素、层叠、裁剪、脏矩形与 PPM 输出。
 */
#include "../ql_test.h"

/* ------------------------- 子类：画一个对角叉 ------------------------- */
QL_PROPERTY_ENUM_EMPTY(QL_CROSS, QLCross, QL_WIDGET, 10)
QL_EVENT_ENUM_EMPTY(QL_CROSS, QLCross, QL_WIDGET, 10)

QL_DECLARE(QLCross, ql_cross)

struct QLCross        { QLWidget parent; };
struct QLCrossClass   { QLWidgetClass parent; };
struct QLCrossPrivate { ql_color_t line_color; };

QL_DEFINE_WITH_PRIVATE(QL_CROSS, QLCross, ql_cross, ql_widget)

static void ql_cross_draw(QLWidget *self, QLCanvas *canvas)
{
    ql_widget_draw_default(self, canvas);        /* 背景 + 边框 */

    struct QLCrossPrivate *priv = ql_cross_get_private((QLCross *)(void *)self);
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    ql_canvas_draw_line(canvas, r.x, r.y, ql_rect_right(r) - 1, ql_rect_bottom(r) - 1, 1,
                        priv->line_color);
    ql_canvas_draw_line(canvas, ql_rect_right(r) - 1, r.y, r.x, ql_rect_bottom(r) - 1, 1,
                        priv->line_color);
}

static void ql_cross_class_init(QLCrossClass *klass)
{
    /* 绘制函数登记到本类型 id 上，渲染器会优先用它 */
    ql_widget_class_set_draw(((QLObjectClass *)(void *)klass)->type, ql_cross_draw);
}

static void ql_cross_init(QLCross *self)
{
    struct QLCrossPrivate *priv = ql_cross_get_private(self);
    priv->line_color = QL_COLOR_RED;
}

static void ql_cross_deinit(QLCross *self) { QL_UNUSED(self); }

QL_TEST_BEGIN("test_widget_render")

    /* ---------------- 准备显示与根 ---------------- */
    QLDispDriver *disp = ql_disp_mem_create(80, 60, QL_COLOR_RGB888);
    QL_CHECK(disp != NULL);
    ql_disp_set_bg_color(QL_COLOR_BLACK);

    QLWidget *root = ql_widget_new_ex(0, 0, 80, 60);
    QL_CHECK(root != NULL);
    QL_CHECK_EQ(ql_render_set_root((QLObject *)root), QL_OK);
    QL_CHECK(ql_render_root() == (QLObject *)root);

    /* 1. 首帧：全屏脏 */
    QL_CHECK_EQ(ql_render_dirty_count(), 1);
    QL_CHECK_EQ(ql_render(), 1);
    QL_CHECK_EQ(ql_disp_mem_flush_count(disp), 1);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 40, 30), QL_COLOR_BLACK);
    QL_CHECK_EQ(ql_render_dirty_count(), 0);
    QL_CHECK_EQ(ql_render(), 0);          /* 无脏区不重绘 */

    /* 2. 父控件背景铺满 */
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x20, 0x40, 0x60), 255);
    QL_CHECK_EQ(ql_render_dirty_count(), 1);
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 0, 0), QL_COLOR_RGB(0x20, 0x40, 0x60));
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 79, 59), QL_COLOR_RGB(0x20, 0x40, 0x60));

    /* 3. 子控件覆盖父控件，且只脏自己那一块 */
    QLWidget *child = ql_widget_new_child((QLObject *)root, 10, 10, 20, 15);
    QL_CHECK(child != NULL);
    QL_CHECK_EQ(ql_object_parent((QLObject *)child), (QLObject *)root);
    ql_widget_set_bg((QLObject *)child, QL_COLOR_RED, 255);

    QL_CHECK_EQ(ql_render_dirty_count(), 1);
    QLRect dirty;
    ql_render_dirty_rect(0, &dirty);
    QL_CHECK_EQ(dirty.x, 10);
    QL_CHECK_EQ(dirty.y, 10);
    QL_CHECK_EQ(dirty.w, 20);
    QL_CHECK_EQ(dirty.h, 15);

    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, 10), QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 29, 24), QL_COLOR_RED);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 9, 10), QL_COLOR_RGB(0x20, 0x40, 0x60));
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 30, 24), QL_COLOR_RGB(0x20, 0x40, 0x60));

    /* 4. 边框 */
    ql_widget_set_border((QLObject *)child, 2, QL_COLOR_WHITE, 0);
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, 10), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 29, 10), QL_COLOR_WHITE);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 15, 15), QL_COLOR_RED);

    /* 5. 圆角：角上像素不画，中心照旧 */
    ql_widget_set_border((QLObject *)child, 0, QL_COLOR_WHITE, 6);
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 10, 10), QL_COLOR_RGB(0x20, 0x40, 0x60));
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 20, 17), QL_COLOR_RED);

    /* 6. 半透明背景与底层混合 */
    ql_widget_set_border((QLObject *)child, 0, QL_COLOR_WHITE, 0);
    ql_widget_set_bg((QLObject *)child, QL_COLOR_WHITE, 128);
    ql_render();
    ql_color_t blended = ql_disp_mem_pixel(disp, 20, 17);
    QL_CHECK(QL_COLOR_GET_R(blended) > 0x20 && QL_COLOR_GET_R(blended) < 0xFF);

    /* 7. 隐藏控件连同子控件都不绘制 */
    ql_widget_set_visible((QLObject *)child, 0);
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 20, 17), QL_COLOR_RGB(0x20, 0x40, 0x60));
    ql_widget_set_visible((QLObject *)child, 1);
    ql_render();

    /* 8. clip_children：超出父矩形的子控件被裁掉 */
    QLWidget *clipper = ql_widget_new_child((QLObject *)root, 40, 30, 20, 20);
    QL_CHECK(clipper != NULL);
    ql_object_set_property((QLObject *)clipper, QL_WIDGET_PROPERTY_CLIP_CHILDREN,
                           ql_value_bool(1));
    QLWidget *big = ql_widget_new_child((QLObject *)clipper, 10, 10, 40, 40);
    QL_CHECK(big != NULL);
    ql_widget_set_bg((QLObject *)big, QL_COLOR_GREEN, 255);
    ql_render();
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 55, 45), QL_COLOR_GREEN);   /* 父内 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, 65, 55), QL_COLOR_RGB(0x20, 0x40, 0x60));

    /* 9. 子类 draw：自定义控件画出对角叉 */
    QLType cross_type = QL_REGISTER(ql_cross, QL_WIDGET_TYPE);
    QL_CHECK(cross_type != QL_TYPE_NONE);
    QL_CHECK_EQ(ql_cross_get_type(), cross_type);

    QLCross *cross = (QLCross *)ql_object_new(cross_type);
    QL_CHECK(cross != NULL);
    ql_object_set_property((QLObject *)cross, QL_WIDGET_PROPERTY_X, ql_value_int(60));
    ql_object_set_property((QLObject *)cross, QL_WIDGET_PROPERTY_Y, ql_value_int(5));
    ql_object_set_property((QLObject *)cross, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(15));
    ql_object_set_property((QLObject *)cross, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(10));
    ql_object_add_child((QLObject *)root, (QLObject *)cross);

    ql_render();
    QLRect cr = ql_widget_abs_rect((QLObject *)cross);
    QL_CHECK_EQ(cr.x, 60);
    QL_CHECK_EQ(cr.y, 5);
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, cr.x, cr.y), QL_COLOR_RED);                 /* 起点 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(cr) - 1, ql_rect_bottom(cr) - 1),
                QL_COLOR_RED);                                                     /* 终点 */
    QL_CHECK_EQ(ql_disp_mem_pixel(disp, ql_rect_right(cr) - 1, cr.y), QL_COLOR_RED); /* 另一条 */

    /* 10. 脏矩形合并：分离 → 两块；相交 → 合并成一块 */
    ql_render_invalidate_all();
    ql_render();
    QL_CHECK_EQ(ql_render_dirty_count(), 0);
    ql_render_invalidate_rect(ql_rect_make(0, 0, 4, 4));
    ql_render_invalidate_rect(ql_rect_make(20, 20, 4, 4));
    QL_CHECK_EQ(ql_render_dirty_count(), 2);
    ql_render_invalidate_rect(ql_rect_make(2, 2, 20, 20));
    QL_CHECK_EQ(ql_render_dirty_count(), 1);

    /* 脏区超出屏幕会被裁掉 */
    ql_render();
    ql_render_invalidate_rect(ql_rect_make(-10, -10, 20, 20));
    QLRect clipped;
    ql_render_dirty_rect(0, &clipped);
    QL_CHECK_EQ(clipped.x, 0);
    QL_CHECK_EQ(clipped.y, 0);
    QL_CHECK_EQ(clipped.w, 10);
    QL_CHECK_EQ(clipped.h, 10);
    ql_render();

    /* 11. PPM 输出（可选，仅在本机存在临时目录时执行） */
    QL_CHECK_EQ(ql_disp_mem_save_ppm(disp, "/tmp/qlgl_widget_render.ppm"), QL_OK);

    /* 12. 无显示时应报错 */
    ql_render_invalidate_all();
    ql_disp_mem_destroy(disp);
    QL_CHECK_EQ(ql_render(), QL_ERR_NOT_INIT);

    ql_object_unref((QLObject *)root);

QL_TEST_END()
