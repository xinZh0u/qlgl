/**
 * @file    test_object_property.c
 * @brief   示例：属性 id 区间、闭区间边界、类型校验与继承派发。
 */
#include "../ql_test.h"

QL_PROPERTY_ENUM_START(QL_PROPA, QLPropA, QL_OBJECT)
    QL_PROPA_PROPERTY_X,
    QL_PROPA_PROPERTY_Y
QL_PROPERTY_ENUM_END(QL_PROPA, 10)

QL_DECLARE(QLPropA, ql_prop_a)

struct QLPropA        { QLObject parent; };
struct QLPropAClass  { QLObjectClass parent; };
struct QLPropAPrivate { int32_t x; int32_t y; };

QL_DEFINE_WITH_PRIVATE(QL_PROPA, QLPropA, ql_prop_a, ql_object)

static ql_err_t ql_prop_a_set(QLObject *obj, ql_property_t property, QLValue value)
{
    struct QLPropAPrivate *priv = ql_prop_a_get_private((QLPropA *)(void *)obj);
    switch (property) {
    case QL_PROPA_PROPERTY_X:
        if (value.type != QL_VALUE_INT) return QL_ERR_INVAL;
        priv->x = value.v.i;
        return QL_OK;
    case QL_PROPA_PROPERTY_Y:
        if (value.type != QL_VALUE_INT) return QL_ERR_INVAL;
        priv->y = value.v.i;
        return QL_OK;
    default:
        return QL_ERR_INVAL;
    }
}

static QLValue ql_prop_a_get(QLObject *obj, ql_property_t property)
{
    struct QLPropAPrivate *priv = ql_prop_a_get_private((QLPropA *)(void *)obj);
    switch (property) {
    case QL_PROPA_PROPERTY_X: return ql_value_int(priv->x);
    case QL_PROPA_PROPERTY_Y: return ql_value_int(priv->y);
    default:                  return QL_VALUE_NULL;
    }
}

static void ql_prop_a_class_init(QLPropAClass *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)klass;
    obj_class->set_property = ql_prop_a_set;
    obj_class->get_property = ql_prop_a_get;
}

static void ql_prop_a_init(QLPropA *self)   { QL_UNUSED(self); }
static void ql_prop_a_deinit(QLPropA *self) { QL_UNUSED(self); }

QL_TEST_BEGIN("test_object_property")

    QLType type = QL_REGISTER(ql_prop_a, QL_TYPE_OBJECT);
    QL_CHECK(type != QL_TYPE_NONE);

    /* 1. id 区间：根类 1..100，子类从 101 起，留足 10 个名额 */
    QL_CHECK_EQ(QL_OBJECT_PROPERTY_START, 1);
    QL_CHECK_EQ(QL_PROPA_PROPERTY_START, QL_OBJECT_PROPERTY_SPACE_END + 1);
    QL_CHECK_EQ(QL_PROPA_PROPERTY_END, QL_PROPA_PROPERTY_START + 9);
    QL_CHECK(QL_PROPA_PROPERTY_X >= QL_PROPA_PROPERTY_START);
    QL_CHECK(QL_PROPA_PROPERTY_Y <= QL_PROPA_PROPERTY_END);

    QLObject *obj = ql_object_new(type);
    QL_CHECK(obj != NULL);

    /* 2. 正常读写 */
    QL_CHECK_EQ(ql_object_set_property(obj, QL_PROPA_PROPERTY_X, ql_value_int(11)), QL_OK);
    QL_CHECK_EQ(ql_object_set_property(obj, QL_PROPA_PROPERTY_Y, ql_value_int(22)), QL_OK);
    QL_CHECK_EQ(ql_object_get_property(obj, QL_PROPA_PROPERTY_X).v.i, 11);
    QL_CHECK_EQ(ql_object_get_property(obj, QL_PROPA_PROPERTY_Y).v.i, 22);

    /* 3. START/END 是区间哨兵：落在区间内但没有属性绑定它们 */
    QL_CHECK_EQ(QL_PROPA_PROPERTY_X, QL_PROPA_PROPERTY_START + 1);
    QL_CHECK_EQ(QL_PROPA_PROPERTY_Y, QL_PROPA_PROPERTY_START + 2);
    QL_CHECK_EQ(ql_object_set_property(obj, QL_PROPA_PROPERTY_START, ql_value_int(1)),
                QL_ERR_INVAL);
    QL_CHECK_EQ(ql_object_set_property(obj, QL_PROPA_PROPERTY_END, ql_value_int(2)),
                QL_ERR_INVAL);

    /* 4. 类型不匹配：被 setter 拒绝 */
    QL_CHECK_EQ(ql_object_set_property(obj, QL_PROPA_PROPERTY_X, ql_value_string("x")),
                QL_ERR_INVAL);
    QL_CHECK_EQ(ql_object_get_property(obj, QL_PROPA_PROPERTY_X).v.i, 11);

    /* 5. 越界 id：根类区间没人处理、以及完全未知的 id */
    QL_CHECK_EQ(ql_object_set_property(obj, QL_OBJECT_PROPERTY_START, ql_value_int(0)),
                QL_ERR_INVAL);
    QL_CHECK_EQ(ql_object_set_property(obj, 99999u, ql_value_int(0)), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_object_get_property(obj, 99999u).type, QL_VALUE_NONE);

    /* 6. 归属查询 */
    QLType owner = QL_TYPE_NONE;
    QL_CHECK(ql_object_property_owner(obj, QL_PROPA_PROPERTY_Y, &owner));
    QL_CHECK_EQ(owner, type);
    QL_CHECK(ql_object_property_owner(obj, 99999u, &owner) == 0);

    /* 7. 值类型工具：定点与整型互转 */
    QL_CHECK_EQ(ql_fixed_from_int(3), 3 * QL_FIXED_ONE);
    QL_CHECK_EQ(ql_fixed_to_int(ql_fixed_from_int(3)), 3);
    QL_CHECK_EQ(ql_fixed_mul(ql_fixed_from_int(2), ql_fixed_from_int(3)), ql_fixed_from_int(6));
    QL_CHECK_EQ(ql_fixed_to_int(ql_fixed_div(ql_fixed_from_int(8), ql_fixed_from_int(4))), 2);
    QL_CHECK_EQ(ql_fixed_div(ql_fixed_from_int(1), 0), 0);

    /* 8. 读值容错：类型不符返回 fallback */
    QL_CHECK_EQ(ql_value_get_int(ql_value_string("abc"), -1), -1);
    QL_CHECK_EQ(ql_value_get_int(ql_value_int(5), 0), 5);
    QL_CHECK_STR(ql_value_get_string(ql_value_int(5), "fallback"), "fallback");
    QL_CHECK_STR(ql_value_type_name(QL_VALUE_FIXED), "fixed");

    ql_object_unref(obj);

QL_TEST_END()
