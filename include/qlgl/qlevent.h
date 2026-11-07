/**
 * @file    qlevent.h
 * @brief   事件系统：参数打包、连接池、捕获/冒泡传播。
 */
#ifndef QLGL_EVENT_H
#define QLGL_EVENT_H

#include "qlgl.h"

typedef struct QLObject QLObject;

/* 事件 id 一律 32 位，段内按类预留 100 个（见 qlconfig.h） */
typedef uint32_t ql_event_t;

/* 事件参数：定长数组，随事件一起传值，不分布局、不分配堆 */
#define QL_EVENT_ARG_MAX    QL_CFG_EVENT_ARG_MAX

typedef struct {
    uint8_t  count;
    uint8_t  type[QL_CFG_EVENT_ARG_MAX];
    QLValue  arg[QL_CFG_EVENT_ARG_MAX];
} QLEventArgs;

/* 事件参数构造器：链式调用，超限静默丢弃（返回同一指针） */
QLEventArgs *ql_args_reset(QLEventArgs *args);
QLEventArgs *ql_args_int(QLEventArgs *args, int32_t value);
QLEventArgs *ql_args_uint(QLEventArgs *args, uint32_t value);
QLEventArgs *ql_args_fixed(QLEventArgs *args, ql_fixed_t value);
QLEventArgs *ql_args_bool(QLEventArgs *args, int value);
QLEventArgs *ql_args_string(QLEventArgs *args, const char *value);
QLEventArgs *ql_args_ptr(QLEventArgs *args, void *value);
QLEventArgs *ql_args_object(QLEventArgs *args, QLObject *value);

typedef struct QLEvent {
    ql_event_t          event;           /* 事件 id */
    QLObject           *target;          /* 最初收到事件的对象 */
    QLObject           *current_target;  /* 当前正在处理事件的对象 */
    const QLEventArgs  *args;            /* 可为 NULL */
    void               *user_data;       /* ql_connect 时绑定的上下文 */
    uint32_t            conn_id;         /* 当前连接 id */
    uint8_t             stop_propagation;/* 非 0：不再向后续节点传播 */
    uint8_t             prevent_default; /* 非 0：跳过类的默认事件处理 */
} QLEvent;

typedef void (*ql_event_cb_t)(QLEvent *event);

/* 事件描述表：静态常量，注册时挂在类上 */
typedef struct {
    const char *name;    /* QL_CFG_PROPERTY_CAPACITY 那套裁剪暂未覆盖事件名 */
    ql_event_t  id;
    uint32_t    flags;
} QLEventDesc;

enum {
    QL_EVENT_FLAG_BUBBLE  = 1u << 0,   /* 向父对象冒泡 */
    QL_EVENT_FLAG_CAPTURE = 1u << 1,   /* 先由根向目标捕获 */
    QL_EVENT_FLAG_STOPPABLE = 1u << 2, /* 允许 stop_propagation 生效 */
    QL_EVENT_FLAG_DEFAULT = 1u << 3,   /* 类有默认处理函数可调用 */
};

/* 根类事件区间：1 ~ 1+capacity-1，子类从 SPACE_END+1 接续 */
typedef enum {
    QL_OBJECT_EVENT_START       = 1,
    QL_OBJECT_EVENT_END         = QL_OBJECT_EVENT_START + QL_CFG_EVENT_CAPACITY - 1,
    QL_OBJECT_EVENT_SPACE_END   = QL_OBJECT_EVENT_END,
} QLObjectEvent;

#define QL_EVENT_ENUM_START(NAME, Name, INHERIT)                            \
    enum Name##Event {                                                      \
        NAME##_EVENT_START = INHERIT##_EVENT_SPACE_END + 1,

#define QL_EVENT_ENUM_END(NAME, capacity)                                   \
        , NAME##_EVENT_END = NAME##_EVENT_START + (capacity) - 1            \
        , NAME##_EVENT_SPACE_END = NAME##_EVENT_END                         \
    };

/* 暂时没有事件的类用这个 */
#define QL_EVENT_ENUM_EMPTY(NAME, Name, INHERIT, capacity)                  \
    enum Name##Event {                                                      \
        NAME##_EVENT_START = INHERIT##_EVENT_SPACE_END + 1,                 \
        NAME##_EVENT_END = NAME##_EVENT_START + (capacity) - 1,             \
        NAME##_EVENT_SPACE_END = NAME##_EVENT_END                           \
    };

/* ----- 连接 ----- */
uint32_t ql_connect(QLObject *obj, ql_event_t event, ql_event_cb_t cb, void *user_data);
ql_err_t ql_disconnect(QLObject *obj, uint32_t conn_id);
int      ql_disconnect_all(QLObject *obj);
uint32_t ql_event_conn_count(QLObject *obj);

/* ----- 派发 ----- */
int ql_emit(QLObject *obj, ql_event_t event, const QLEventArgs *args);

/* 回调内可调 */
void ql_event_stop_propagation(QLEvent *event);
void ql_event_prevent_default(QLEvent *event);

/* 读取参数 */
int32_t     ql_event_arg_int(const QLEvent *event, uint8_t index, int32_t fallback);
ql_fixed_t  ql_event_arg_fixed(const QLEvent *event, uint8_t index, ql_fixed_t fallback);
const char *ql_event_arg_string(const QLEvent *event, uint8_t index, const char *fallback);
void       *ql_event_arg_ptr(const QLEvent *event, uint8_t index, void *fallback);
uint8_t     ql_event_arg_count(const QLEvent *event);

/* ----- 池状态（调试/测试用） ----- */
uint32_t ql_event_pool_usage(void);
uint32_t ql_event_pool_capacity(void);
void     ql_event_pool_init(void);

/*
 * 事件发射宏：显式写出每个参数的类型构造器，不做 token 计数魔法。
 * 0 参数：QL_EMIT_0(obj, event)
 * 1 参数：QL_EMIT_1(obj, event, ql_args_int, 42)
 * 2 参数：QL_EMIT_2(obj, event, ql_args_int, 1, ql_args_string, "hi")
 */
#define QL_EMIT_0(obj, event)   ql_emit((obj), (event), NULL)

#define QL_EMIT_1(obj, event, push1, a1)                                    \
    do {                                                                    \
        QLEventArgs _ql_args;                                               \
        ql_args_reset(&_ql_args);                                           \
        push1(&_ql_args, (a1));                                             \
        ql_emit((obj), (event), &_ql_args);                                 \
    } while (0)

#define QL_EMIT_2(obj, event, push1, a1, push2, a2)                         \
    do {                                                                    \
        QLEventArgs _ql_args;                                               \
        ql_args_reset(&_ql_args);                                           \
        push1(&_ql_args, (a1));                                             \
        push2(&_ql_args, (a2));                                             \
        ql_emit((obj), (event), &_ql_args);                                 \
    } while (0)

#define QL_EMIT_3(obj, event, push1, a1, push2, a2, push3, a3)              \
    do {                                                                    \
        QLEventArgs _ql_args;                                               \
        ql_args_reset(&_ql_args);                                           \
        push1(&_ql_args, (a1));                                             \
        push2(&_ql_args, (a2));                                             \
        push3(&_ql_args, (a3));                                             \
        ql_emit((obj), (event), &_ql_args);                                 \
    } while (0)

#define QL_EMIT_4(obj, event, push1, a1, push2, a2, push3, a3, push4, a4)   \
    do {                                                                    \
        QLEventArgs _ql_args;                                               \
        ql_args_reset(&_ql_args);                                           \
        push1(&_ql_args, (a1));                                             \
        push2(&_ql_args, (a2));                                             \
        push3(&_ql_args, (a3));                                             \
        push4(&_ql_args, (a4));                                             \
        ql_emit((obj), (event), &_ql_args);                                 \
    } while (0)

/* 最常用的一元整型事件 */
#define QL_EMIT_INT(obj, event, value)  QL_EMIT_1((obj), (event), ql_args_int, (value))

#endif /* QLGL_EVENT_H */
