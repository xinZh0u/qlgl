#include "qllabel.h"

struct QLLabelPrivate{
    char * label;
};

QL_DEFINE_WITH_PRIVATE(QL_LABEL,QLLabel,ql_label,ql_widget)

void ql_label_set_property(QLObject * obj,ql_property_t p,QLValue value){
    QL_PROPERTY_SET(QLLabel,ql_label,obj,p)
        QL_SET_CASE(value,QL_LABEL_PROPERTY_NAME,QL_VALUE_STRING)
            private->label = (char*)value.s;
    QL_PROPERTY_SET_END
}

QLValue ql_label_get_property(QLObject * obj,ql_property_t p){
    QL_PROPERTY_GET(QLLabel,ql_label,obj,p)
        QL_GET_CASE(QL_LABEL_PROPERTY_NAME,QL_VALUE_STRING)
        value.s =  private->label;
    QL_PROPERTY_GET_END
}

void ql_label_class_init(QLLabelClass * klass){
    QLObjectClass * obj_class = (QLObjectClass*)klass;
    obj_class->set_property = ql_label_set_property;
    obj_class->get_property = ql_label_get_property;

    ql_event_new(obj_class,"change",1,QL_EVENT_BUBBLE);
}

void ql_label_init(QLLabel * self){

}

void ql_label_deinit(QLLabel * self){

}

QLLabel *ql_label_new(void) {
    return (QLLabel *)ql_object_new(QL_LABEL_TYPE,NULL);
}
