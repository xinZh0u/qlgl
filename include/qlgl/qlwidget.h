/**
 * @file    qlwidget.h
 * @brief   控件基类 QLWidget：几何、背景、边框、子控件裁剪与绘制虚函数。
 *
 * 所有控件都继承 QLWidget。渲染器遍历对象树，按树的顺序（父先子后）绘制，
 * 因此后挂的子控件覆盖先挂的。
 */
#ifndef QLGL_WIDGET_H
#define QLGL_WIDGET_H

#include "qlgl.h"
#include "qlcanvas.h"

typedef struct QLWidget QLWidget;
typedef struct QLWidgetClass QLWidgetClass;

/* ----- 属性 id ----- */
QL_PROPERTY_ENUM_START(QL_WIDGET, QLWidget, QL_OBJECT)
    QL_WIDGET_PROPERTY_X,
    QL_WIDGET_PROPERTY_Y,
    QL_WIDGET_PROPERTY_WIDTH,
    QL_WIDGET_PROPERTY_HEIGHT,
    QL_WIDGET_PROPERTY_BG_COLOR,
    QL_WIDGET_PROPERTY_BG_ALPHA,
    QL_WIDGET_PROPERTY_BORDER_WIDTH,
    QL_WIDGET_PROPERTY_BORDER_COLOR,
    QL_WIDGET_PROPERTY_BORDER_RADIUS,
    QL_WIDGET_PROPERTY_CLIP_CHILDREN,
    QL_WIDGET_PROPERTY_VISIBLE
QL_PROPERTY_ENUM_END(QL_WIDGET, 100)

/* ----- 事件 id（v0.1 暂无事件，占位保留区间） ----- */
QL_EVENT_ENUM_EMPTY(QL_WIDGET, QLWidget, QL_OBJECT, 100)

QL_DECLARE(QLWidget, ql_widget)

/* 类型 id 快捷宏：每个新类在自己头文件里加一行同名宏即可 */
#define QL_WIDGET_TYPE  ql_widget_get_type()

struct QLWidget {
    QLObject parent;
};

/* 绘制函数的类型：self 提供属性，canvas 提供裁剪与原语 */
typedef void (*ql_widget_draw_func)(QLWidget *self, QLCanvas *canvas);

/*
 * 类结构统一为 { 父类 parent; }，字段布局必须与 QLObjectClass 完全一致，
 * 所以绘制函数不放进类结构体，而是登记在"类型 id → 绘制函数"表里。
 * 原因：QLObjectClass 是 QLWidgetClass 的首成员，若派生类再加字段，
 * 从 QLObjectClass* 反查派生字段的偏移就不确定了。
 */
struct QLWidgetClass {
    QLObjectClass parent;
};

/* ----- 创建 ----- */
QLWidget *ql_widget_new(void);
QLWidget *ql_widget_new_ex(ql_coord_t x, ql_coord_t y, ql_coord_t w, ql_coord_t h);
/* 创建后立即挂到 parent 上（等价于 add_child），失败返回 NULL */
QLWidget *ql_widget_new_child(QLObject *parent, ql_coord_t x, ql_coord_t y,
                              ql_coord_t w, ql_coord_t h);

/* ----- 便捷访问（等价于 set/get property，命中失败返回原值/fallback） ----- */
void       ql_widget_set_pos(QLObject *obj, ql_coord_t x, ql_coord_t y);
ql_coord_t ql_widget_x(QLObject *obj);
ql_coord_t ql_widget_y(QLObject *obj);
int        ql_widget_is_visible(QLObject *obj);
int        ql_widget_clips_children(QLObject *obj);
QLRect     ql_widget_local_rect(QLObject *obj);       /* 相对父控件的内容区坐标 */
QLRect     ql_widget_abs_rect(QLObject *obj);         /* 相对屏幕的绝对坐标 */
int        ql_widget_contains(QLObject *obj, ql_coord_t abs_x, ql_coord_t abs_y);
ql_coord_t ql_widget_width(QLObject *obj);
ql_coord_t ql_widget_height(QLObject *obj);
void       ql_widget_set_bg(QLObject *obj, ql_color_t color, uint8_t alpha);
void       ql_widget_set_border(QLObject *obj, ql_coord_t width, ql_color_t color,
                                ql_coord_t radius);
void       ql_widget_set_visible(QLObject *obj, int visible);

/* ----- 绘制 ----- */
/* 登记/查询某类型的绘制函数；在 class_init 里调用 set */
void       ql_widget_class_set_draw(QLType type, ql_widget_draw_func draw);
ql_widget_draw_func ql_widget_class_get_draw(QLType type);
/* 清空绘制表（qlgl_init / shutdown 内部调用） */
void       ql_widget_reset(void);

/* 返回该类链上最近一个实现了 draw 的函数；没有则返回 NULL */
ql_widget_draw_func ql_widget_get_draw(QLObject *obj);
/* 默认绘制：背景 + 边框（子类可以直接调用它） */
void ql_widget_draw_default(QLWidget *self, QLCanvas *canvas);

#endif /* QLGL_WIDGET_H */
