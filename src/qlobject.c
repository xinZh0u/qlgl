#include "qlobject.h"
#include <stdlib.h>
#include <string.h>
#include <qlclass.h>

struct QLObjectPrivate{
};

uint32_t ql_object_private_get_size(){
    return 0;
}

/* QLType */
static QLObjectClass *class_table[1024];
static QLType class_count = 1;

QLObjectClass *
ql_class_register(QLObjectClass *klass)
{
    if (klass->class_init)
        klass->class_init(klass);

    class_table[class_count] = klass;
    klass->type = class_count;
    class_count++;
    return klass;
}

QLObject *
ql_object_ref(QLObject *self)
{
    if (self)
        self->ref_count++;
    return self;
}

void
ql_object_unref(QLObject *self)
{
    if (!self) return;

    if (--self->ref_count == 0) {
        if (self->klass->deinit)
            self->klass->deinit(self);
        free(self);
    }
}

QLObjectClass * ql_type_get_class(QLType type){
    return class_table[type];
}

QLObject * ql_object_new(QLType type,...){
    QLObjectClass * klass = class_table[type];


    // 顺序初始化
    QLType klass_stack[QL_OBJECT_MAX_INHERIT_NUM];
    int klass_stack_pos = 0;
    QLType prev_type = type;
    int size = 0;
    for(;;){
        klass_stack[klass_stack_pos] = prev_type;
        if(prev_type==QL_OBJECT_TYPE)break;

        size += class_table[prev_type]->private_size();

        prev_type = class_table[prev_type]->parent_type();
        klass_stack_pos++;
    }

    uint8_t * mem = calloc(1,size + klass->instance_size);
    QLObject *self = (QLObject *)(mem+size);

    self->klass = klass;
    self->ref_count = 1;

    for(int i=klass_stack_pos;i>0;i--){
        klass = class_table[i];
        if (klass->init)
            klass->init(self);
    }

    return self;
}

QLType ql_object_get_type(){
    return 0;
}

/* 初始化*/
void ql_object_init(){
    QLObjectClass klass;
    class_table[QL_OBJECT_TYPE] = &klass;
    klass.type = QL_OBJECT_TYPE;
    klass.parent_type = NULL;
    klass.private_size = ql_object_private_get_size;
    klass.instance_size = sizeof(QLObject);
}

/* value系统 */
QLValue ql_value_int(int i){
    QLValue value={
        .type = QL_VALUE_INT,
        .i = i,
    };
    return value;
}

QLValue ql_value_ptr(void * p){
    QLValue value={
        .type = QL_VALUE_POINTER,
        .p = p,
    };
    return value;
}

/* 属性系统 */
void ql_object_set_property(QLObject * obj,ql_property_t property,QLValue value){
    QLType type = obj->klass->type;
    while(!(property>class_table[type]->property_start&&
               property<class_table[type]->property_end)){
        type = class_table[type]->parent_type();
        if(type==QL_OBJECT_TYPE)return;
    }

    if(class_table[type]->set_property)
        class_table[type]->set_property(obj,property,value);
}

QLValue ql_object_get_property(QLObject * obj,ql_property_t property){
    QLType type = obj->klass->type;
    while(!(property>class_table[type]->property_start&&
             property<class_table[type]->property_end)){
        type = class_table[type]->parent_type();
        if(type==QL_OBJECT_TYPE){
            QLValue value_null = {0};
            return value_null;
        }
    }

    if(class_table[type]->get_property == NULL){
        QLValue value_null = {0};
        return value_null;
    }
    return class_table[type]->get_property(obj,property);
}

/* 事件系统 */
static uint32_t ql_next_conn_id = 1;

int ql_event_new(QLObjectClass *klass,
                             const char    *name,
                             ql_event_t     id,
                             uint32_t       flags)
{
    if (!klass || !name) return 0;

    // 查重：同名事件不允许重复注册
    for (uint32_t i = 0; i < klass->desc_num; i++) {
        if (strcmp(klass->descs[i].name, name) == 0)
            return 0;   // 已存在
    }

    // 确保容量
    if (klass->descs == NULL) {
        klass->desc_capacity = QL_EVENT_DESC_DEFAULT_CAPACITY;
        klass->descs = calloc(klass->desc_capacity, sizeof(QLEventDesc));
        if (!klass->descs) return 0;
        klass->desc_num = 0;
    }
    else if (klass->desc_num >= klass->desc_capacity)
    {
        uint32_t new_cap = klass->desc_capacity * 2;
        QLEventDesc *new_descs = realloc(klass->descs,
                                         new_cap * sizeof(QLEventDesc));
        if (!new_descs) return 0;
        klass->descs = new_descs;
        klass->desc_capacity = new_cap;
    }

    // 配置事件描述符
    klass->descs[klass->desc_num++] = (QLEventDesc){
        .name  = strdup(name),   // 需要释放时 free
        .id    = id,
        .flags = flags
    };

    return 1;
}

int ql_connect(QLObject * obj,ql_event_t event,ql_event_cb_t cb,void * user_data){
    if(!obj)return 0;

    // 确保容量
    if (obj->conns == NULL) {
        obj->conn_capacity = QL_EVENT_CONN_DEFAULT_CAPACITY;
        obj->conns = calloc(obj->conn_capacity, sizeof(QLEventConn));
        if (!obj->conns) return 0;
        obj->conn_num = 0;
    }
    else if (obj->conn_num >= obj->conn_capacity)
    {
        uint32_t new_cap = obj->conn_capacity * 2;
        QLEventConn *new_conns = realloc(obj->conns,
                                         new_cap * sizeof(QLEventConn));
        if (!new_conns) return 0;
        obj->conns = new_conns;
        obj->conn_capacity = new_cap;
    }

    obj->conns[obj->conn_num++] = (QLEventConn){
        .conn_id = ql_next_conn_id++,
        .event = event,
        .cb = cb,
        .user_data = user_data,
        .removed = 0,
    };
    return ql_next_conn_id-1;
}

void ql_disconnect(QLObject *obj, uint32_t conn_id){
    if(!obj||!obj->conns)return;
    for(int i=0;i<obj->conn_num;i++){
        if(obj->conns[i].conn_id == conn_id && !obj->conns[i].removed){
            obj->conns[i].removed = 1;
        }
    }
}

int ql_emit(QLObject * obj,ql_event_t event,QLValue value){
    if(!obj)return 0;
    for(uint32_t i=0;i<obj->conn_num;i++){
        if(obj->conns[i].event == event&&!obj->conns[i].removed){
            QLEvent e = (QLEvent){
                .event = event,
                .target = obj,
                .current_target = obj,
                .value = value,
                .user_data = obj->conns[i].user_data,
            };
            if(obj->conns[i].cb)
                obj->conns[i].cb(&e);
        }
    }
    return 1;
}
