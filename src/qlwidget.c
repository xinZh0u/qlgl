#include "qlwidget.h"
#include <string.h>

struct QLWidgetPrivate{
    int width,height;
    int x,y;
};

QL_DEFINE_WITH_PRIVATE(QL_WIDGET,QLWidget, ql_widget,ql_object)

void ql_widget_set_property(QLObject * obj,ql_property_t p,QLValue value){
    QL_PROPERTY_SET(QLWidget,ql_widget,obj,p)
        QL_SET_CASE(value,QL_WIDGET_PROPERTY_WIDTH,QL_VALUE_INT)
            private->width = value.i;
        QL_SET_CASE(value,QL_WIDGET_PROPERTY_HEIGHT,QL_VALUE_INT)
            private->height = value.i;
        QL_SET_CASE(value,QL_WIDGET_PROPERTY_X,QL_VALUE_INT)
            private->x = value.i;
        QL_SET_CASE(value,QL_WIDGET_PROPERTY_Y,QL_VALUE_INT)
            private->y = value.i;
    QL_PROPERTY_SET_END
}

QLValue ql_widget_get_property(QLObject * obj,ql_property_t p){
    QL_PROPERTY_GET(QLWidget,ql_widget,obj,p)
        QL_GET_CASE(QL_WIDGET_PROPERTY_WIDTH,QL_VALUE_INT)
            value.i = private->width;
        QL_GET_CASE(QL_WIDGET_PROPERTY_HEIGHT,QL_VALUE_INT)
            value.i = private->height;
        QL_GET_CASE(QL_WIDGET_PROPERTY_X,QL_VALUE_INT)
            value.i = private->x;
        QL_GET_CASE(QL_WIDGET_PROPERTY_Y,QL_VALUE_INT)
            value.i = private->y;
    QL_PROPERTY_GET_END
}

static void
ql_widget_class_init(QLWidgetClass *klass)
{
    QLObjectClass * obj_class = (QLObjectClass *)klass;
    obj_class->set_property = ql_widget_set_property;
    obj_class->get_property = ql_widget_get_property;
}

static void
ql_widget_init(QLWidget *self)
{
}

static void
ql_widget_deinit(QLWidget *self)
{

}

QLWidget *ql_widget_new(void) {
    return (QLWidget *)ql_object_new(QL_WIDGET_TYPE,NULL);
}

int ql_widget_get_pos(QLWidget * self,int * x,int * y) {
    QLWidgetPrivate * private = ql_widget_get_private(self);
    if(private==NULL)return 0;
    *x = private->x;
    *y = private->y;
    return 1;
}
