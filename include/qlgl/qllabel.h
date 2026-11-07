/**
 * @file    qllabel.h
 * @brief   文本标签控件：显示一段 UTF-8 文本。
 *
 * 文本由标签自己持有（QLStr），调用方传进来的指针用完即可释放。
 */
#ifndef QLGL_LABEL_H
#define QLGL_LABEL_H

#include "qlgl.h"
#include "qlwidget.h"

typedef struct QLLabel QLLabel;
typedef struct QLLabelClass QLLabelClass;

QL_PROPERTY_ENUM_START(QL_LABEL, QLLabel, QL_WIDGET)
    QL_LABEL_PROPERTY_TEXT,        /* STRING */
    QL_LABEL_PROPERTY_FONT,        /* POINTER（const QLFont *） */
    QL_LABEL_PROPERTY_TEXT_COLOR,  /* COLOR */
    QL_LABEL_PROPERTY_ALIGN,       /* INT（QLTextAlign） */
    QL_LABEL_PROPERTY_AUTO_SIZE    /* BOOL：文本/字体变化时自动改宽高 */
QL_PROPERTY_ENUM_END(QL_LABEL, 100)

QL_EVENT_ENUM_EMPTY(QL_LABEL, QLLabel, QL_WIDGET, 100)

QL_DECLARE(QLLabel, ql_label)

#define QL_LABEL_TYPE  ql_label_get_type()

struct QLLabel {
    QLWidget parent;
};

struct QLLabelClass {
    QLWidgetClass parent;
};

QLLabel *ql_label_new(const char *text);
QLLabel *ql_label_new_child(QLObject *parent, const char *text,
                            ql_coord_t x, ql_coord_t y);

/* 便捷函数 */
void            ql_label_set_text(QLObject *obj, const char *text);
const char     *ql_label_text(QLObject *obj);
void            ql_label_set_font(QLObject *obj, const QLFont *font);
/*
 * 按字号选字体（内部走 ql_font_default）：配置了矢量字体就得到该字号的
 * 动态字体（支持中文），否则回落到最接近的内置位图字体。
 * 返回实际使用的字体，便于调用方判断是不是回退了。
 */
const QLFont   *ql_label_set_font_size(QLObject *obj, int pixel_size);
const QLFont   *ql_label_font(QLObject *obj);
void            ql_label_set_text_color(QLObject *obj, ql_color_t color);
void            ql_label_set_align(QLObject *obj, QLTextAlign align);
/* 按当前文本与字体重新计算宽高（AUTO_SIZE 开启时会自动调用） */
void            ql_label_refit(QLObject *obj);

#endif /* QLGL_LABEL_H */
