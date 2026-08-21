#ifndef QLEVENT_H
#define QLEVENT_H

/* 0 参数 */
#define QL_EMIT_IMPL_0(obj, code) \
do { ql_emit((obj), (code), QL_VALUE_NULL); } while(0)

/* 1 参数 */
#define QL_EMIT_IMPL_1(obj, code, value) \
    ql_emit((obj), (code), value);

/* 2 参数 */
#define QL_EMIT_IMPL_2(obj, code, t1, a1) \
do { struct { t1 _1; } _p = { a1 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 4 参数 */
#define QL_EMIT_IMPL_4(obj, code, t1, a1, t2, a2) \
do { struct { t1 _1; t2 _2; } _p = { a1, a2 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 6 参数 */
#define QL_EMIT_IMPL_6(obj, code, t1,a1,t2,a2,t3,a3) \
do { struct { t1 _1; t2 _2; t3 _3; } _p = { a1, a2, a3 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 8 参数 */
#define QL_EMIT_IMPL_8(obj, code, t1,a1,t2,a2,t3,a3,t4,a4) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; } _p = { a1, a2, a3, a4 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 10 参数 */
#define QL_EMIT_IMPL_10(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; } _p = { a1, a2, a3, a4, a5 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 12 参数 */
#define QL_EMIT_IMPL_12(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5,t6,a6) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; t6 _6; } _p = { a1, a2, a3, a4, a5, a6 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 14 参数 */
#define QL_EMIT_IMPL_14(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5,t6,a6,t7,a7) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; t6 _6; t7 _7; } _p = { a1, a2, a3, a4, a5, a6, a7 }; \
        ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 16 参数 */
#define QL_EMIT_IMPL_16(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5,t6,a6,t7,a7,t8,a8) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; t6 _6; t7 _7; t8 _8; } _p = { a1, a2, a3, a4, a5, a6, a7, a8 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 18 参数 */
#define QL_EMIT_IMPL_18(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5,t6,a6,t7,a7,t8,a8,t9,a9) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; t6 _6; t7 _7; t8 _8; t9 _9; } _p = { a1, a2, a3, a4, a5, a6, a7, a8, a9 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)

/* 20 参数 */
#define QL_EMIT_IMPL_20(obj, code, t1,a1,t2,a2,t3,a3,t4,a4,t5,a5,t6,a6,t7,a7,t8,a8,t9,a9,t10,a10) \
do { struct { t1 _1; t2 _2; t3 _3; t4 _4; t5 _5; t6 _6; t7 _7; t8 _8; t9 _9; t10 _10; } _p = { a1, a2, a3, a4, a5, a6, a7, a8, a9, a10 }; \
    ql_emit((obj), (code), ql_value_ptr(&_p)); } while(0)


/* 事件宏 */
#define QL_EMIT_N_ARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,N,...) N

#define QL_EMIT_N_ARGS(...) \
QL_EMIT_N_ARGS_IMPL(__VA_ARGS__, 10,9,8,7,6,5,4,3,2,1,0)

/* 让 N 先算成数字再拼接 */
#define QL_EMIT_IMPL_N_(obj, code, N, ...) \
QL_EMIT_IMPL_##N(obj, code, __VA_ARGS__)

#define QL_EMIT_IMPL_N(obj, code, N, ...) \
    QL_EMIT_IMPL_N_(obj, code, N, __VA_ARGS__)

#define QL_EMIT(obj, code, ...) \
    QL_EMIT_IMPL_N(obj, code, QL_EMIT_N_ARGS(__VA_ARGS__), __VA_ARGS__)

/* 类属性宏定义 */
#define QL_EVENT_ENUM_START(NAME,Name,INHERIT)                                  \
typedef enum Name##EVENT Name##EVENT;                                           \
    enum Name##Property {                                                       \
        NAME##_EVENT_START = INHERIT##_EVENT_SPACE_END,                         \

#define QL_EVENT_ENUM_END(NAME,capacity)                                        \
        NAME##_EVENT_SPACE_END = NAME##_EVENT_END+capacity,                     \
};


#define QL_EVENT_DESC_DEFAULT_CAPACITY  4
#define QL_EVENT_CONN_DEFAULT_CAPACITY  4

#define ql_event_t  uint32_t

typedef enum {
    /* 传播行为 */
    QL_EVENT_BUBBLE    = 1 << 0,  /* 默认向 parent 冒泡 */
    QL_EVENT_CAPTURE   = 1 << 1,  /* 捕获阶段（从根到目标），高级可不要 */
    QL_EVENT_STOPPABLE = 1 << 2,  /* 允许回调调 stop_processing */
} QLEventFlags;



#endif // QLEVENT_H
