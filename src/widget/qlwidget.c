/**
 * @file    qlwidget.c
 * @brief   QLWidget 实现：属性、几何换算、默认绘制。
 */
#include <string.h>

#include <qlgl/qlgl.h>

struct QLWidgetPrivate {
    ql_coord_t x;
    ql_coord_t y;
    ql_coord_t w;
    ql_coord_t h;
    ql_color_t bg_color;
    ql_color_t border_color;
    ql_color_t state_bg[3];      /* 下标：0=HOVER 1=ACTIVE 2=FOCUS */
    uint8_t    bg_alpha;
    uint8_t    border_width;
    uint8_t    border_radius;
    uint8_t    clip_children;
    uint8_t    visible;
    uint8_t    state;            /* QL_WIDGET_STATE_* */
    uint8_t    pointer_events;
    uint8_t    focusable;
    uint8_t    state_bg_alpha[3];
};

QL_DEFINE_WITH_PRIVATE(QL_WIDGET, QLWidget, ql_widget, ql_object)

/* ------------------------------ 绘制表 ------------------------------ */

static ql_widget_draw_func s_draw_table[QL_CFG_MAX_CLASS];

void ql_widget_class_set_draw(QLType type, ql_widget_draw_func draw)
{
    if (type < QL_CFG_MAX_CLASS)
        s_draw_table[type] = draw;
}

ql_widget_draw_func ql_widget_class_get_draw(QLType type)
{
    return (type < QL_CFG_MAX_CLASS) ? s_draw_table[type] : NULL;
}

void ql_widget_reset(void)
{
    memset(s_draw_table, 0, sizeof(s_draw_table));
}

/* ------------------------------ 属性 ------------------------------ */

static ql_err_t ql_widget_set_property(QLObject *obj, ql_property_t property, QLValue value)
{
    struct QLWidgetPrivate *priv = ql_widget_get_private((QLWidget *)(void *)obj);

    switch (property) {
    case QL_WIDGET_PROPERTY_X:
    case QL_WIDGET_PROPERTY_Y:
    case QL_WIDGET_PROPERTY_WIDTH:
    case QL_WIDGET_PROPERTY_HEIGHT: {
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        int32_t v = value.v.i;
        if ((property == QL_WIDGET_PROPERTY_WIDTH || property == QL_WIDGET_PROPERTY_HEIGHT)
            && v < 0)
            return QL_ERR_INVAL;
        if (property == QL_WIDGET_PROPERTY_X)
            priv->x = (ql_coord_t)v;
        else if (property == QL_WIDGET_PROPERTY_Y)
            priv->y = (ql_coord_t)v;
        else if (property == QL_WIDGET_PROPERTY_WIDTH)
            priv->w = (ql_coord_t)v;
        else
            priv->h = (ql_coord_t)v;
        ql_render_invalidate(obj);
        return QL_OK;
    }
    case QL_WIDGET_PROPERTY_BG_COLOR:
        if (value.type != QL_VALUE_COLOR && value.type != QL_VALUE_UINT &&
            value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->bg_color = (ql_color_t)ql_value_get_uint(value, priv->bg_color);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_BG_ALPHA:
        if (value.type != QL_VALUE_INT && value.type != QL_VALUE_UINT)
            return QL_ERR_INVAL;
        priv->bg_alpha = (uint8_t)(ql_value_get_int(value, 0) & 0xFF);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_BORDER_WIDTH:
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->border_width = (uint8_t)(value.v.i < 0 ? 0 : value.v.i);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_BORDER_COLOR:
        if (value.type != QL_VALUE_COLOR && value.type != QL_VALUE_UINT &&
            value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->border_color = (ql_color_t)ql_value_get_uint(value, priv->border_color);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_BORDER_RADIUS:
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->border_radius = (uint8_t)(value.v.i < 0 ? 0 : value.v.i);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_CLIP_CHILDREN:
        if (value.type != QL_VALUE_BOOL && value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->clip_children = (uint8_t)(ql_value_get_int(value, 0) ? 1 : 0);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_VISIBLE:
        if (value.type != QL_VALUE_BOOL && value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->visible = (uint8_t)(ql_value_get_int(value, 1) ? 1 : 0);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_WIDGET_PROPERTY_POINTER_EVENTS:
        if (value.type != QL_VALUE_BOOL && value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->pointer_events = (uint8_t)(ql_value_get_int(value, 1) ? 1 : 0);
        return QL_OK;
    case QL_WIDGET_PROPERTY_FOCUSABLE:
        if (value.type != QL_VALUE_BOOL && value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->focusable = (uint8_t)(ql_value_get_int(value, 0) ? 1 : 0);
        return QL_OK;
    default:
        return QL_ERR_INVAL;
    }
}

static QLValue ql_widget_get_property(QLObject *obj, ql_property_t property)
{
    struct QLWidgetPrivate *priv = ql_widget_get_private((QLWidget *)(void *)obj);

    switch (property) {
    case QL_WIDGET_PROPERTY_X:             return ql_value_int(priv->x);
    case QL_WIDGET_PROPERTY_Y:             return ql_value_int(priv->y);
    case QL_WIDGET_PROPERTY_WIDTH:         return ql_value_int(priv->w);
    case QL_WIDGET_PROPERTY_HEIGHT:        return ql_value_int(priv->h);
    case QL_WIDGET_PROPERTY_BG_COLOR:      return ql_value_color(priv->bg_color);
    case QL_WIDGET_PROPERTY_BG_ALPHA:      return ql_value_int(priv->bg_alpha);
    case QL_WIDGET_PROPERTY_BORDER_WIDTH:  return ql_value_int(priv->border_width);
    case QL_WIDGET_PROPERTY_BORDER_COLOR:  return ql_value_color(priv->border_color);
    case QL_WIDGET_PROPERTY_BORDER_RADIUS: return ql_value_int(priv->border_radius);
    case QL_WIDGET_PROPERTY_CLIP_CHILDREN: return ql_value_bool(priv->clip_children);
    case QL_WIDGET_PROPERTY_VISIBLE:       return ql_value_bool(priv->visible);
    case QL_WIDGET_PROPERTY_POINTER_EVENTS: return ql_value_bool(priv->pointer_events);
    case QL_WIDGET_PROPERTY_FOCUSABLE:     return ql_value_bool(priv->focusable);
    default:                               return QL_VALUE_NULL;
    }
}

/* ------------------------------ 生命周期 ------------------------------ */

static void ql_widget_class_init(QLWidgetClass *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)klass;
    obj_class->set_property = ql_widget_set_property;
    obj_class->get_property = ql_widget_get_property;
    /* 默认绘制挂在 QLWidget 自己的类型 id 上，子类可覆盖 */
    ql_widget_class_set_draw(obj_class->type, ql_widget_draw_default);
}

static void ql_widget_init(QLWidget *self)
{
    struct QLWidgetPrivate *priv = ql_widget_get_private(self);
    priv->x = 0;
    priv->y = 0;
    priv->w = 0;
    priv->h = 0;
    priv->bg_color = QL_COLOR_WHITE;
    priv->border_color = QL_COLOR_BLACK;
    priv->bg_alpha = 0;          /* 默认透明，与 CSS 一致 */
    priv->border_width = 0;
    priv->border_radius = 0;
    priv->clip_children = 0;
    priv->visible = 1;
    priv->state = 0;
    priv->pointer_events = 1;
    priv->focusable = 0;
    for (int i = 0; i < 3; i++) {
        priv->state_bg[i] = QL_COLOR_WHITE;
        priv->state_bg_alpha[i] = 0;
    }
}

static void ql_widget_deinit(QLWidget *self)
{
    QL_UNUSED(self);
}

/* ------------------------------ 创建 ------------------------------ */

QLWidget *ql_widget_new(void)
{
    return (QLWidget *)ql_object_new(QL_WIDGET_TYPE);
}

QLWidget *ql_widget_new_ex(ql_coord_t x, ql_coord_t y, ql_coord_t w, ql_coord_t h)
{
    QLWidget *self = ql_widget_new();
    if (self == NULL)
        return NULL;
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_X, ql_value_int(x));
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_Y, ql_value_int(y));
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(w));
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(h));
    return self;
}

QLWidget *ql_widget_new_child(QLObject *parent, ql_coord_t x, ql_coord_t y,
                              ql_coord_t w, ql_coord_t h)
{
    if (parent == NULL)
        return NULL;
    QLWidget *self = ql_widget_new_ex(x, y, w, h);
    if (self == NULL)
        return NULL;
    if (ql_object_add_child(parent, (QLObject *)self) != QL_OK) {
        ql_object_unref((QLObject *)self);
        return NULL;
    }
    return self;
}

/* ------------------------------ 便捷访问 ------------------------------ */

static struct QLWidgetPrivate *priv_of(QLObject *obj)
{
    if (obj == NULL || !ql_object_is(obj, QL_WIDGET_TYPE))
        return NULL;
    return ql_widget_get_private((QLWidget *)(void *)obj);
}

void ql_widget_set_pos(QLObject *obj, ql_coord_t x, ql_coord_t y)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_X, ql_value_int(x));
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_Y, ql_value_int(y));
}

ql_coord_t ql_widget_x(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->x : 0;
}

ql_coord_t ql_widget_y(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->y : 0;
}

int ql_widget_is_visible(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->visible : 0;
}

int ql_widget_clips_children(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->clip_children : 0;
}

QLRect ql_widget_local_rect(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    if (priv == NULL)
        return ql_rect_make(0, 0, 0, 0);
    return ql_rect_make(priv->x, priv->y, priv->w, priv->h);
}

QLRect ql_widget_abs_rect(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    if (priv == NULL)
        return ql_rect_make(0, 0, 0, 0);

    ql_coord_t x = 0;
    ql_coord_t y = 0;
    for (QLObject *node = obj; node != NULL; node = ql_object_parent(node)) {
        struct QLWidgetPrivate *p = priv_of(node);
        if (p != NULL) {
            x += p->x;
            y += p->y;
        }
    }
    return ql_rect_make(x, y, priv->w, priv->h);
}

int ql_widget_contains(QLObject *obj, ql_coord_t abs_x, ql_coord_t abs_y)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    if (priv == NULL || !priv->visible)
        return 0;
    QLRect r = ql_widget_abs_rect(obj);
    return abs_x >= r.x && abs_x < ql_rect_right(r) && abs_y >= r.y &&
           abs_y < ql_rect_bottom(r);
}

ql_coord_t ql_widget_width(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->w : 0;
}

ql_coord_t ql_widget_height(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->h : 0;
}

void ql_widget_set_bg(QLObject *obj, ql_color_t color, uint8_t alpha)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_BG_COLOR, ql_value_color(color));
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_BG_ALPHA, ql_value_int(alpha));
}

void ql_widget_set_border(QLObject *obj, ql_coord_t width, ql_color_t color,
                          ql_coord_t radius)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_BORDER_WIDTH, ql_value_int(width));
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_BORDER_COLOR, ql_value_color(color));
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_BORDER_RADIUS, ql_value_int(radius));
}

void ql_widget_set_visible(QLObject *obj, int visible)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_VISIBLE, ql_value_bool(visible));
}

/* ------------------------------ 交互 ------------------------------ */

uint8_t ql_widget_state(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->state : 0;
}

int ql_widget_has_state(QLObject *obj, uint8_t mask)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) && (priv->state & mask) == mask;
}

int ql_widget_set_state(QLObject *obj, uint8_t mask, int on)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    if (priv == NULL)
        return 0;

    uint8_t old = priv->state;
    mask &= QL_WIDGET_STATE_ALL;
    priv->state = (uint8_t)(on ? (priv->state | mask) : (priv->state & ~mask));
    if (priv->state == old)
        return 0;

    /* 只有该状态配置了替代背景才需要重绘，否则白打一次脏 */
    for (int i = 0; i < 3; i++) {
        if ((mask & (1u << i)) != 0 && priv->state_bg_alpha[i] > 0) {
            ql_render_invalidate(obj);
            break;
        }
    }
    return 1;
}

int ql_widget_pointer_events(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->pointer_events : 0;
}

void ql_widget_set_pointer_events(QLObject *obj, int enabled)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_POINTER_EVENTS,
                           ql_value_bool(enabled ? 1 : 0));
}

int ql_widget_is_focusable(QLObject *obj)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    return (priv != NULL) ? priv->focusable : 0;
}

void ql_widget_set_focusable(QLObject *obj, int focusable)
{
    if (priv_of(obj) == NULL)
        return;
    ql_object_set_property(obj, QL_WIDGET_PROPERTY_FOCUSABLE,
                           ql_value_bool(focusable ? 1 : 0));
}

void ql_widget_set_state_bg(QLObject *obj, uint8_t state, ql_color_t color, uint8_t alpha)
{
    struct QLWidgetPrivate *priv = priv_of(obj);
    if (priv == NULL)
        return;

    if ((state & QL_WIDGET_STATE_HOVER) != 0) {
        priv->state_bg[0] = color;
        priv->state_bg_alpha[0] = alpha;
    }
    if ((state & QL_WIDGET_STATE_ACTIVE) != 0) {
        priv->state_bg[1] = color;
        priv->state_bg_alpha[1] = alpha;
    }
    if ((state & QL_WIDGET_STATE_FOCUS) != 0) {
        priv->state_bg[2] = color;
        priv->state_bg_alpha[2] = alpha;
    }
    ql_render_invalidate(obj);
}

/* ------------------------------ 绘制 ------------------------------ */

ql_widget_draw_func ql_widget_get_draw(QLObject *obj)
{
    if (obj == NULL)
        return NULL;

    for (QLObjectClass *klass = obj->klass; klass != NULL; klass = klass->parent) {
        ql_widget_draw_func draw = ql_widget_class_get_draw(klass->type);
        if (draw != NULL)
            return draw;
    }
    return NULL;
}

void ql_widget_draw_default(QLWidget *self, QLCanvas *canvas)
{
    if (self == NULL || canvas == NULL)
        return;

    struct QLWidgetPrivate *priv = ql_widget_get_private(self);
    QLRect rect = ql_widget_abs_rect((QLObject *)self);
    if (ql_rect_is_empty(rect))
        return;

    /* 状态背景：ACTIVE > FOCUS > HOVER，首个配置了 alpha 的生效 */
    ql_color_t bg_color = priv->bg_color;
    uint8_t    bg_alpha = priv->bg_alpha;
    static const uint8_t order[3] = { 1, 2, 0 };   /* ACTIVE, FOCUS, HOVER */
    for (int i = 0; i < 3; i++) {
        uint8_t idx = order[i];
        if ((priv->state & (1u << idx)) != 0 && priv->state_bg_alpha[idx] > 0) {
            bg_color = priv->state_bg[idx];
            bg_alpha = priv->state_bg_alpha[idx];
            break;
        }
    }

    if (bg_alpha > 0)
        ql_canvas_fill_round_rect_alpha(canvas, rect, priv->border_radius,
                                        bg_color, bg_alpha);

    if (priv->border_width > 0)
        ql_canvas_stroke_rect(canvas, rect, priv->border_radius,
                              priv->border_width, priv->border_color);
}
