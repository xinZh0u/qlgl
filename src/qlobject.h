#ifndef QLOBJECT_H
#define QLOBJECT_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <qlevent.h>

/* ----- 声明 ----- */
typedef struct QLObjectClass QLObjectClass;
typedef struct QLObject QLObject;

#define QL_OBJECT_MAX_INHERIT_NUM 100   // 创建object时候继承栈的大小

/* ----- 类别定义 -----*/
#define ql_property_t uint32_t
#define QLType unsigned long
#define QL_OBJECT_TYPE   ql_object_get_type()

/*  */
typedef enum {
    QL_VALUE_INT,
    QL_VALUE_FLOAT,
    QL_VALUE_DOUBLE,
    QL_VALUE_BOOL,
    QL_VALUE_STRING,
    QL_VALUE_POINTER,
    QL_VALUE_OBJECT,
} QLValueType;

typedef struct {
    QLValueType type;
    union{
        int      i;
        float    f;
        double   d;
        int      b;
        const char *s;
        QLObject *o;
        void * p;
    };
}QLValue;

/* ----- 事件 ----- */

typedef struct {
    ql_event_t  event;
    QLObject    *target;             // 最初收到事件的对象
    QLObject    *current_target;     // 当前正在处理事件的对象

    QLValue     value;
    void        *user_data;
}QLEvent;

typedef void (*ql_event_cb_t)(QLEvent * e);

typedef struct {
    uint32_t        conn_id;
    ql_event_t      event;       // 监听哪个事件
    ql_event_cb_t   cb;           // 回调函数
    void            *user_data;    // connect 时绑的上下文
    uint8_t         removed;  // : 1; // 延迟删除标记
} QLEventConn;

typedef struct {
    const char      *name;      // "clicked"
    ql_event_t      id;         // 合成 event_t
    uint32_t        flags;        // BUBBLE / STOPPABLE / PASSIVE
} QLEventDesc;

/* ----- 属性 ----- */
// 推荐的容量
#define QL_PROPERTY_DEFAULT_CAPACITY    100

typedef enum{
    QL_OBJECT_PROPERTY_START,

    QL_OBJECT_PROPERTY_END,
    QL_OBJECT_PROPERTY_SPACE_END = QL_OBJECT_PROPERTY_START + QL_PROPERTY_DEFAULT_CAPACITY,
}QLObjectProperty;

/* ----- 根类 ----- */

struct QLObject {
    QLObjectClass *klass;
    uint32_t ref_count;

    // 事件连接列表
    QLEventConn  *  conns;
    uint32_t       conn_num;
    uint32_t       conn_capacity;

    uint8_t        emitting : 1;
};

struct QLObjectClass {
    QLType type;
    uint32_t instance_size;

    // 事件描述符列表
    QLEventDesc *   descs;
    uint32_t        desc_num;
    uint32_t        desc_capacity;

    ql_property_t   property_start;
    ql_property_t   property_end;

    uint32_t (*private_size)();
    QLType (*parent_type)();

    // 虚函数
    void (*init)(QLObject *self);
    void (*deinit)(QLObject *self);

    void (*class_init)(QLObjectClass *klass);

    void (*set_property)(QLObject * object,ql_property_t property,QLValue value);
    QLValue (*get_property)(QLObject * object,ql_property_t property);
};

/* 生命周期 */
QLObject * ql_object_new(QLType type,...);
QLObject *ql_object_ref(QLObject *self);
void      ql_object_unref(QLObject *self);

/* 类型系统 */
QLObjectClass *ql_class_register(QLObjectClass *klass);
QLType ql_object_get_type();
uint32_t ql_object_private_get_size();

/* value系统 */
QLValue ql_value_int(int i);
QLValue ql_value_ptr(void * p);

/* 初始化 */
void ql_object_init();

/* 属性系统 */
void ql_object_set_property(QLObject * obj,ql_property_t property,QLValue value);
QLValue ql_object_get_property(QLObject * obj,ql_property_t property);

/* 事件系统 */
int ql_event_new(QLObjectClass *klass,const char *name,ql_event_t id,uint32_t flags);
int ql_connect(QLObject * obj,ql_event_t event,ql_event_cb_t cb,void * user_data);
void ql_disconnect(QLObject *obj, uint32_t conn_id);
int ql_emit(QLObject * obj,ql_event_t event,QLValue value);

#endif
