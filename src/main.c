#include <qlgl.h>
#include <qlwidget.h>
#include <qllabel.h>

void label_event(QLEvent * e){

    //printf("event:%d\n",e->value.i);

    struct {int _1;float _2;char * str;} *p = e->value.p;
    printf("event:%d %f\n",p->_1,p->_2);//,p->str);
}

int main(){
    QLWidget * widget = ql_widget_new();
    ql_object_init();
    QLLabel * label = ql_label_new();
    ql_object_set_property((QLObject*)label,QL_WIDGET_PROPERTY_X,ql_value_int(100));
    QLValue value = ql_object_get_property((QLObject*)label,QL_WIDGET_PROPERTY_X);
    printf("value:%d\n",value.i);

    char str[100] = "sad";
    int a = ql_connect((QLObject*)label,1,label_event,str);
    int b = ql_connect((QLObject*)label,1,label_event,str);
    QL_EMIT((QLObject*)label, 1, int, a, float, b);
    printf("\n");
    ql_disconnect((QLObject*)label,1);
    QL_EMIT((QLObject*)label, 1, int, a, float, b);


    return 0;
}
