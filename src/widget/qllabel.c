/**
 * @file    qllabel.c
 * @brief   QLLabel 实现：文本持有、属性、绘制。
 */
#include <qlgl/qlgl.h>

struct QLLabelPrivate {
    QLStr           text;
    const QLFont   *font;
    ql_color_t      text_color;
    uint8_t         align;
    uint8_t         auto_size;
    uint8_t         reserved[2];
};

QL_DEFINE_WITH_PRIVATE(QL_LABEL, QLLabel, ql_label, ql_widget)

/* ------------------------------ 自动尺寸 ------------------------------ */

static void label_refit(QLLabel *self)
{
    struct QLLabelPrivate *priv = ql_label_get_private(self);
    if (!priv->auto_size)
        return;

    ql_coord_t w = 0;
    ql_coord_t h = 0;
    ql_font_measure_text(priv->font, ql_str_data(&priv->text), (int32_t)ql_str_len(&priv->text),
                         &w, &h);
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(w));
    ql_object_set_property((QLObject *)self, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(h));
}

/* ------------------------------ 属性 ------------------------------ */

static ql_err_t ql_label_set_property(QLObject *obj, ql_property_t property, QLValue value)
{
    QLLabel *self = (QLLabel *)(void *)obj;
    struct QLLabelPrivate *priv = ql_label_get_private(self);

    switch (property) {
    case QL_LABEL_PROPERTY_TEXT: {
        if (value.type != QL_VALUE_STRING && value.type != QL_VALUE_NONE)
            return QL_ERR_INVAL;
        const char *text = (value.type == QL_VALUE_STRING) ? value.v.s : NULL;
        if (text != NULL) {
            if (ql_str_set(&priv->text, text) != QL_OK)
                return QL_ERR_NOMEM;
        } else {
            ql_str_clear(&priv->text);
        }
        label_refit(self);
        ql_render_invalidate(obj);
        return QL_OK;
    }
    case QL_LABEL_PROPERTY_FONT: {
        const QLFont *font = (value.type == QL_VALUE_POINTER || value.type == QL_VALUE_NONE)
                                 ? (const QLFont *)value.v.p
                                 : ql_value_get_ptr(value, NULL);
        priv->font = (font != NULL) ? font : &ql_font_mono_12;
        label_refit(self);
        ql_render_invalidate(obj);
        return QL_OK;
    }
    case QL_LABEL_PROPERTY_TEXT_COLOR:
        if (value.type != QL_VALUE_COLOR && value.type != QL_VALUE_UINT &&
            value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->text_color = (ql_color_t)ql_value_get_uint(value, priv->text_color);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_LABEL_PROPERTY_ALIGN:
        if (value.type != QL_VALUE_INT && value.type != QL_VALUE_UINT)
            return QL_ERR_INVAL;
        priv->align = (uint8_t)(ql_value_get_int(value, 0) & 0xFF);
        ql_render_invalidate(obj);
        return QL_OK;
    case QL_LABEL_PROPERTY_AUTO_SIZE:
        if (value.type != QL_VALUE_BOOL && value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->auto_size = (uint8_t)(ql_value_get_int(value, 1) ? 1 : 0);
        label_refit(self);
        ql_render_invalidate(obj);
        return QL_OK;
    default:
        return QL_ERR_INVAL;
    }
}

static QLValue ql_label_get_property(QLObject *obj, ql_property_t property)
{
    struct QLLabelPrivate *priv = ql_label_get_private((QLLabel *)(void *)obj);

    switch (property) {
    case QL_LABEL_PROPERTY_TEXT:       return ql_value_string(ql_str_data(&priv->text));
    case QL_LABEL_PROPERTY_FONT:       return ql_value_ptr((void *)priv->font);
    case QL_LABEL_PROPERTY_TEXT_COLOR: return ql_value_color(priv->text_color);
    case QL_LABEL_PROPERTY_ALIGN:      return ql_value_int(priv->align);
    case QL_LABEL_PROPERTY_AUTO_SIZE:  return ql_value_bool(priv->auto_size);
    default:                           return QL_VALUE_NULL;
    }
}

/* ------------------------------ 绘制 ------------------------------ */

static void ql_label_draw(QLWidget *widget, QLCanvas *canvas)
{
    ql_widget_draw_default(widget, canvas);      /* 背景 + 边框 */

    QLLabel *self = (QLLabel *)(void *)widget;
    struct QLLabelPrivate *priv = ql_label_get_private(self);

    QLRect rect = ql_widget_abs_rect((QLObject *)self);
    if (ql_rect_is_empty(rect) || ql_str_len(&priv->text) == 0)
        return;

    ql_canvas_draw_text(canvas, priv->font, ql_str_data(&priv->text),
                        (int32_t)ql_str_len(&priv->text), rect, priv->text_color,
                        (QLTextAlign)priv->align);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void ql_label_class_init(QLLabelClass *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)(void *)klass;
    obj_class->set_property = ql_label_set_property;
    obj_class->get_property = ql_label_get_property;
    ql_widget_class_set_draw(obj_class->type, ql_label_draw);
}

static void ql_label_init(QLLabel *self)
{
    struct QLLabelPrivate *priv = ql_label_get_private(self);
    ql_str_init(&priv->text);
    priv->font = &ql_font_mono_12;
    priv->text_color = QL_COLOR_WHITE;
    priv->align = QL_TEXT_ALIGN_LEFT;
    priv->auto_size = 1;
}

static void ql_label_deinit(QLLabel *self)
{
    struct QLLabelPrivate *priv = ql_label_get_private(self);
    ql_str_free(&priv->text);
}

/* ------------------------------ 创建与便捷函数 ------------------------------ */

QLLabel *ql_label_new(const char *text)
{
    QLLabel *self = (QLLabel *)ql_object_new(QL_LABEL_TYPE);
    if (self == NULL)
        return NULL;
    if (text != NULL)
        ql_label_set_text((QLObject *)self, text);
    return self;
}

QLLabel *ql_label_new_child(QLObject *parent, const char *text,
                            ql_coord_t x, ql_coord_t y)
{
    if (parent == NULL)
        return NULL;
    QLLabel *self = ql_label_new(text);
    if (self == NULL)
        return NULL;
    ql_widget_set_pos((QLObject *)self, x, y);
    if (ql_object_add_child(parent, (QLObject *)self) != QL_OK) {
        ql_object_unref((QLObject *)self);
        return NULL;
    }
    return self;
}

void ql_label_set_text(QLObject *obj, const char *text)
{
    ql_object_set_property(obj, QL_LABEL_PROPERTY_TEXT, ql_value_string(text));
}

const char *ql_label_text(QLObject *obj)
{
    return ql_value_get_string(ql_object_get_property(obj, QL_LABEL_PROPERTY_TEXT), "");
}

const QLFont *ql_label_set_font_size(QLObject *obj, int pixel_size)
{
    const QLFont *font = ql_font_default(pixel_size);
    if (font != NULL && obj != NULL && ql_object_is(obj, QL_LABEL_TYPE))
        ql_object_set_property(obj, QL_LABEL_PROPERTY_FONT, ql_value_ptr((void *)font));
    return font;
}

void ql_label_set_font(QLObject *obj, const QLFont *font)
{
    ql_object_set_property(obj, QL_LABEL_PROPERTY_FONT, ql_value_ptr((void *)font));
}

const QLFont *ql_label_font(QLObject *obj)
{
    return (const QLFont *)ql_value_get_ptr(ql_object_get_property(obj, QL_LABEL_PROPERTY_FONT),
                                            NULL);
}

void ql_label_set_text_color(QLObject *obj, ql_color_t color)
{
    ql_object_set_property(obj, QL_LABEL_PROPERTY_TEXT_COLOR, ql_value_color(color));
}

void ql_label_set_align(QLObject *obj, QLTextAlign align)
{
    ql_object_set_property(obj, QL_LABEL_PROPERTY_ALIGN, ql_value_int((int32_t)align));
}

void ql_label_refit(QLObject *obj)
{
    if (obj == NULL || !ql_object_is(obj, QL_LABEL_TYPE))
        return;
    label_refit((QLLabel *)(void *)obj);
}
