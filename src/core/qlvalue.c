/**
 * @file    qlvalue.c
 * @brief   QLValue 构造与安全读取。
 */
#include <qlgl/qlgl.h>

ql_fixed_t ql_fixed_from_int(int32_t value)
{
    return (ql_fixed_t)(value * QL_FIXED_ONE);
}

ql_fixed_t ql_fixed_mul(ql_fixed_t a, ql_fixed_t b)
{
    /* 先扩到 64 位避免中间结果溢出；不使用浮点，保证 32/64 位一致 */
    return (ql_fixed_t)(((int64_t)a * (int64_t)b) >> QL_FIXED_SHIFT);
}

ql_fixed_t ql_fixed_div(ql_fixed_t a, ql_fixed_t b)
{
    if (b == 0)
        return 0;
    return (ql_fixed_t)(((int64_t)a << QL_FIXED_SHIFT) / (int64_t)b);
}

int32_t ql_fixed_to_int(ql_fixed_t value)
{
    /* 四舍五入到整数，负数同样向上进位 */
    if (value >= 0)
        return (int32_t)((value + (QL_FIXED_ONE / 2)) >> QL_FIXED_SHIFT);
    return -(int32_t)(((-value) + (QL_FIXED_ONE / 2)) >> QL_FIXED_SHIFT);
}

QLValue ql_value_none(void)
{
    return QL_VALUE_NULL;
}

QLValue ql_value_int(int32_t value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_INT;
    out.v.i = value;
    return out;
}

QLValue ql_value_uint(uint32_t value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_UINT;
    out.v.u = value;
    return out;
}

QLValue ql_value_fixed(ql_fixed_t value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_FIXED;
    out.v.f = value;
    return out;
}

QLValue ql_value_bool(int value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_BOOL;
    out.v.b = value ? 1 : 0;
    return out;
}

QLValue ql_value_string(const char *value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_STRING;
    out.v.s = value;
    return out;
}

QLValue ql_value_ptr(void *value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_POINTER;
    out.v.p = value;
    return out;
}

QLValue ql_value_object(QLObject *value)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_OBJECT;
    out.v.o = value;
    return out;
}

QLValue ql_value_color(uint32_t rgb)
{
    QLValue out = QL_VALUE_NULL;
    out.type = QL_VALUE_COLOR;
    out.v.u = rgb;
    return out;
}

int32_t ql_value_get_int(QLValue value, int32_t fallback)
{
    switch (value.type) {
    case QL_VALUE_INT:   return value.v.i;
    case QL_VALUE_UINT:  return (int32_t)value.v.u;
    case QL_VALUE_BOOL:  return value.v.b;
    case QL_VALUE_FIXED: return ql_fixed_to_int(value.v.f);
    case QL_VALUE_COLOR: return (int32_t)value.v.u;
    default:             return fallback;
    }
}

uint32_t ql_value_get_uint(QLValue value, uint32_t fallback)
{
    switch (value.type) {
    case QL_VALUE_UINT:  return value.v.u;
    case QL_VALUE_INT:   return (uint32_t)value.v.i;
    case QL_VALUE_BOOL:  return (uint32_t)value.v.b;
    case QL_VALUE_COLOR: return value.v.u;
    case QL_VALUE_FIXED: return (uint32_t)value.v.f;
    default:             return fallback;
    }
}

ql_fixed_t ql_value_get_fixed(QLValue value, ql_fixed_t fallback)
{
    switch (value.type) {
    case QL_VALUE_FIXED: return value.v.f;
    case QL_VALUE_INT:   return ql_fixed_from_int(value.v.i);
    case QL_VALUE_UINT:  return (ql_fixed_t)(value.v.u * QL_FIXED_ONE);
    default:             return fallback;
    }
}

const char *ql_value_get_string(QLValue value, const char *fallback)
{
    return value.type == QL_VALUE_STRING ? value.v.s : fallback;
}

void *ql_value_get_ptr(QLValue value, void *fallback)
{
    switch (value.type) {
    case QL_VALUE_POINTER: return value.v.p;
    case QL_VALUE_OBJECT:  return value.v.o;
    default:               return fallback;
    }
}

QLObject *ql_value_get_object(QLValue value, QLObject *fallback)
{
    return value.type == QL_VALUE_OBJECT ? value.v.o : fallback;
}

const char *ql_value_type_name(QLValueType type)
{
    static const char *names[QL_VALUE_TYPE_COUNT] = {
        "none", "int", "uint", "fixed", "bool", "string", "pointer", "object", "color",
    };
    if ((unsigned)type >= QL_VALUE_TYPE_COUNT)
        return "invalid";
    return names[type];
}
