/**
 * @file    qlvalue.h
 * @brief   属性/事件通用的值容器。整型宽度全部固定，禁止 long/float 直接入场。
 */
#ifndef QLGL_VALUE_H
#define QLGL_VALUE_H

#include "qlgl.h"

typedef struct QLObject QLObject;

typedef enum {
    QL_VALUE_NONE = 0,
    QL_VALUE_INT,       /* int32_t */
    QL_VALUE_UINT,      /* uint32_t */
    QL_VALUE_FIXED,     /* Q16.16 定点 */
    QL_VALUE_BOOL,
    QL_VALUE_STRING,    /* const char *，所有权不归值对象 */
    QL_VALUE_POINTER,   /* void *，所有权不归值对象 */
    QL_VALUE_OBJECT,    /* QLObject *，弱引用，不改变计数 */
    QL_VALUE_COLOR,     /* 0xRRGGBB 或 0xRGB565，由使用方约定 */
    QL_VALUE_TYPE_COUNT
} QLValueType;

/* Q16.16 定点：整数部分 16 位、小数部分 16 位，32/64 位机完全一致 */
typedef int32_t ql_fixed_t;

#define QL_FIXED_SHIFT      16
#define QL_FIXED_ONE        ((ql_fixed_t)1 << QL_FIXED_SHIFT)

ql_fixed_t ql_fixed_from_int(int32_t value);
ql_fixed_t ql_fixed_mul(ql_fixed_t a, ql_fixed_t b);
ql_fixed_t ql_fixed_div(ql_fixed_t a, ql_fixed_t b);
int32_t    ql_fixed_to_int(ql_fixed_t value);

typedef union {
    int32_t      i;
    uint32_t     u;
    ql_fixed_t   f;
    int32_t      b;
    const char  *s;
    void        *p;
    QLObject    *o;
} QLValuePayload;

typedef struct {
    uint8_t        type;    /* QLValueType */
    uint8_t        flags;   /* 预留：QL_VALUE_FLAG_* */
    uint16_t       aux;     /* 预留：长度/索引等附加信息 */
    QLValuePayload v;
} QLValue;

/* 空值常量：供宏与默认返回使用 */
#define QL_VALUE_NULL   ((QLValue){ .type = QL_VALUE_NONE, .flags = 0, .aux = 0, .v = { .i = 0 } })

QLValue ql_value_none(void);
QLValue ql_value_int(int32_t value);
QLValue ql_value_uint(uint32_t value);
QLValue ql_value_fixed(ql_fixed_t value);
QLValue ql_value_bool(int value);
QLValue ql_value_string(const char *value);
QLValue ql_value_ptr(void *value);
QLValue ql_value_object(QLObject *value);
QLValue ql_value_color(uint32_t rgb);

/* 读值：类型不匹配时返回 fallback，永不抛错 */
int32_t     ql_value_get_int(QLValue value, int32_t fallback);
uint32_t    ql_value_get_uint(QLValue value, uint32_t fallback);
ql_fixed_t  ql_value_get_fixed(QLValue value, ql_fixed_t fallback);
const char *ql_value_get_string(QLValue value, const char *fallback);
void       *ql_value_get_ptr(QLValue value, void *fallback);
QLObject   *ql_value_get_object(QLValue value, QLObject *fallback);

const char *ql_value_type_name(QLValueType type);

#endif /* QLGL_VALUE_H */
