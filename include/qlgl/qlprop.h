/**
 * @file    qlprop.h
 * @brief   属性系统：按类的 id 区间派发，区间为闭区间。
 *
 * 每个类向父类要一块"足够大"的 id 区间（默认 100 个），
 * 子类从父类 SPACE_END 的下一个 id 起算，因此任意层的属性 id 全局唯一。
 */
#ifndef QLGL_PROP_H
#define QLGL_PROP_H

#include "qlgl.h"

typedef struct QLObject QLObject;

typedef uint32_t ql_property_t;

enum {
    QL_PROPERTY_FLAG_READ      = 1u << 0,
    QL_PROPERTY_FLAG_WRITE     = 1u << 1,
    QL_PROPERTY_FLAG_REDRAW    = 1u << 2,   /* 赋值后需要重绘 */
    QL_PROPERTY_FLAG_RELAYOUT  = 1u << 3,   /* 赋值后需要重新布局 */
    QL_PROPERTY_FLAG_INHERIT   = 1u << 4,   /* 样式可继承（供编译期工具使用） */
};

/* 属性描述表：可选，供脚本绑定与二进制工具使用，不参与派发 */
typedef struct {
    ql_property_t id;
    uint8_t       type;     /* QLValueType */
    uint8_t       flags;    /* QL_PROPERTY_FLAG_* */
    uint16_t      reserved;
    const char   *name;
} QLPropertyDesc;

/* 根类属性区间：1 ~ 1+capacity-1 */
typedef enum {
    QL_OBJECT_PROPERTY_START       = 1,
    QL_OBJECT_PROPERTY_END         = QL_OBJECT_PROPERTY_START + QL_CFG_PROPERTY_CAPACITY - 1,
    QL_OBJECT_PROPERTY_SPACE_END   = QL_OBJECT_PROPERTY_END,
} QLObjectProperty;

/*
 * 用法（注意：最后一个属性后面不带逗号）：
 *
 * QL_PROPERTY_ENUM_START(QL_WIDGET, QLWidget, QL_OBJECT)
 *     QL_WIDGET_PROPERTY_WIDTH,
 *     QL_WIDGET_PROPERTY_HEIGHT
 * QL_PROPERTY_ENUM_END(QL_WIDGET, 100)
 */
#define QL_PROPERTY_ENUM_START(NAME, Name, INHERIT)                          \
    enum Name##Property {                                                    \
        NAME##_PROPERTY_START = INHERIT##_PROPERTY_SPACE_END + 1,

#define QL_PROPERTY_ENUM_END(NAME, capacity)                                 \
        , NAME##_PROPERTY_END = NAME##_PROPERTY_START + (capacity) - 1       \
        , NAME##_PROPERTY_SPACE_END = NAME##_PROPERTY_END                    \
    };

/* 暂时没有属性的类用这个，等价于"只占位、不分配 id" */
#define QL_PROPERTY_ENUM_EMPTY(NAME, Name, INHERIT, capacity)                \
    enum Name##Property {                                                    \
        NAME##_PROPERTY_START = INHERIT##_PROPERTY_SPACE_END + 1,            \
        NAME##_PROPERTY_END = NAME##_PROPERTY_START + (capacity) - 1,        \
        NAME##_PROPERTY_SPACE_END = NAME##_PROPERTY_END                      \
    };

/* 派发：沿继承链找到拥有该 id 区间的类，调用它的 set/get */
ql_err_t ql_object_set_property(QLObject *obj, ql_property_t property, QLValue value);
QLValue  ql_object_get_property(QLObject *obj, ql_property_t property);

/* 判断某个类是否接管该属性 id（调试用） */
int ql_object_property_owner(QLObject *obj, ql_property_t property, QLType *owner_type);

#endif /* QLGL_PROP_H */
