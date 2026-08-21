#ifndef QLWIDGET_H
#define QLWIDGET_H

#include <qlclass.h>

QL_PROPERTY_ENUM_START(QL_WIDGET,QLWidget,QL_OBJECT)
    QL_WIDGET_PROPERTY_WIDTH,
    QL_WIDGET_PROPERTY_HEIGHT,
    QL_WIDGET_PROPERTY_X,
    QL_WIDGET_PROPERTY_Y,
    QL_WIDGET_PROPERTY_END,
QL_PROPERTY_ENUM_END(QL_WIDGET,100)

QL_DECLARE(QLWidget, ql_widget)
#define QL_WIDGET_TYPE  ql_widget_get_type()

struct QLWidget{
    QLObject parent;
};

struct QLWidgetClass{
    QLObjectClass parent;
};

QLWidget *ql_widget_new(void);

int ql_widget_get_pos(QLWidget * self,int * x,int * y);

#endif
