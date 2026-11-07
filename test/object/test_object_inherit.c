/**
 * @file    test_object_inherit.c
 * @brief   示例：三层继承 —— 初始化顺序、私有区隔离、属性向父类派发。
 *
 * 继承链：QLObject → QLT1 → QLT2 → QLT3
 */
#include "../ql_test.h"

static char g_order[16];
static int  g_order_len;

static void order_push(char tag)
{
    if (g_order_len < (int)sizeof(g_order) - 1)
        g_order[g_order_len++] = tag;
    g_order[g_order_len] = '\0';
}

static void order_reset(void)
{
    g_order_len = 0;
    g_order[0] = '\0';
}

/* ------------------------------ QLT1 ------------------------------ */
QL_PROPERTY_ENUM_START(QL_T1, QLT1, QL_OBJECT)
    QL_T1_PROPERTY_VALUE
QL_PROPERTY_ENUM_END(QL_T1, 10)

QL_DECLARE(QLT1, ql_t1)

struct QLT1       { QLObject parent; };
struct QLT1Class  { QLObjectClass parent; };
struct QLT1Private{ int32_t value; };

QL_DEFINE_WITH_PRIVATE(QL_T1, QLT1, ql_t1, ql_object)

static ql_err_t ql_t1_set_property(QLObject *obj, ql_property_t property, QLValue value)
{
    struct QLT1Private *priv = ql_t1_get_private((QLT1 *)(void *)obj);
    switch (property) {
    case QL_T1_PROPERTY_VALUE:
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->value = value.v.i;
        return QL_OK;
    default:
        return QL_ERR_INVAL;
    }
}

static QLValue ql_t1_get_property(QLObject *obj, ql_property_t property)
{
    struct QLT1Private *priv = ql_t1_get_private((QLT1 *)(void *)obj);
    if (property == QL_T1_PROPERTY_VALUE)
        return ql_value_int(priv->value);
    return QL_VALUE_NULL;
}

static void ql_t1_class_init(QLT1Class *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)klass;
    obj_class->set_property = ql_t1_set_property;
    obj_class->get_property = ql_t1_get_property;
}

static void ql_t1_init(QLT1 *self)
{
    QL_UNUSED(self);
    order_push('1');
}

static void ql_t1_deinit(QLT1 *self)
{
    QL_UNUSED(self);
    order_push('1');
}

/* ------------------------------ QLT2 ------------------------------ */
QL_PROPERTY_ENUM_START(QL_T2, QLT2, QL_T1)
    QL_T2_PROPERTY_EXTRA
QL_PROPERTY_ENUM_END(QL_T2, 10)

QL_DECLARE(QLT2, ql_t2)

struct QLT2       { QLT1 parent; };
struct QLT2Class  { QLT1Class parent; };
struct QLT2Private{ int32_t extra; };

QL_DEFINE_WITH_PRIVATE(QL_T2, QLT2, ql_t2, ql_t1)

static void ql_t2_class_init(QLT2Class *klass) { QL_UNUSED(klass); }
static void ql_t2_init(QLT2 *self)             { QL_UNUSED(self); order_push('2'); }
static void ql_t2_deinit(QLT2 *self)           { QL_UNUSED(self); order_push('2'); }

/* ------------------------------ QLT3 ------------------------------ */
QL_PROPERTY_ENUM_START(QL_T3, QLT3, QL_T2)
    QL_T3_PROPERTY_FLAG
QL_PROPERTY_ENUM_END(QL_T3, 10)

QL_DECLARE(QLT3, ql_t3)

struct QLT3       { QLT2 parent; };
struct QLT3Class  { QLT2Class parent; };
struct QLT3Private{ int32_t flag; };

QL_DEFINE_WITH_PRIVATE(QL_T3, QLT3, ql_t3, ql_t2)

static void ql_t3_class_init(QLT3Class *klass) { QL_UNUSED(klass); }
static void ql_t3_init(QLT3 *self)             { QL_UNUSED(self); order_push('3'); }
static void ql_t3_deinit(QLT3 *self)           { QL_UNUSED(self); order_push('3'); }

QL_TEST_BEGIN("test_object_inherit")

    /* 1. 注册顺序：父类必须先注册 */
    QLType t1 = QL_REGISTER(ql_t1, QL_TYPE_OBJECT);
    QLType t2 = QL_REGISTER(ql_t2, t1);
    QLType t3 = QL_REGISTER(ql_t3, t2);
    QL_CHECK(t1 != QL_TYPE_NONE);
    QL_CHECK(t2 != QL_TYPE_NONE);
    QL_CHECK(t3 != QL_TYPE_NONE);
    QL_CHECK_EQ(ql_t3_get_type(), t3);

    /* 2. 私有区布局：每层 4 字节会被向上对齐到 max_align_t（通常 8/16） */
    const uint16_t align = (uint16_t)_Alignof(max_align_t);
    QL_CHECK_EQ(QL_CLASS_INFO(ql_t1).priv_offset, 0);
    QL_CHECK_EQ(QL_CLASS_INFO(ql_t2).priv_offset, align);
    QL_CHECK_EQ(QL_CLASS_INFO(ql_t3).priv_offset, align * 2);
    QL_CHECK_EQ(QL_CLASS_INFO(ql_t2).priv_total, align * 2);
    QL_CHECK_EQ(QL_CLASS_INFO(ql_t3).priv_total, align * 3);

    /* 3. 创建：分配量 = 实例 + 对齐后的私有区总量 */
    QLMemStats before;
    QLMemStats after;
    ql_mem_stats(&before);
    order_reset();
    QLT3 *obj = (QLT3 *)ql_object_new(t3);
    ql_mem_stats(&after);
    QL_CHECK(obj != NULL);
    QL_CHECK_EQ(after.bytes_live - before.bytes_live,
                (long)sizeof(QLT3) + (long)QL_CLASS_INFO(ql_t3).priv_total);

    /* 4. 初始化顺序：父 → 子 */
    QL_CHECK_STR(g_order, "123");

    /* 5. 私有区互不干扰，且地址符合公式 */
    struct QLT1Private *p1 = ql_t1_get_private((QLT1 *)(void *)obj);
    struct QLT2Private *p2 = ql_t2_get_private((QLT2 *)(void *)obj);
    struct QLT3Private *p3 = ql_t3_get_private((QLT3 *)(void *)obj);
    QL_CHECK((char *)p1 == (char *)obj - QL_CLASS_INFO(ql_t3).priv_total);
    QL_CHECK((char *)p2 == (char *)obj
             - (QL_CLASS_INFO(ql_t3).priv_total - QL_CLASS_INFO(ql_t2).priv_offset));
    QL_CHECK((char *)p3 == (char *)obj
             - (QL_CLASS_INFO(ql_t3).priv_total - QL_CLASS_INFO(ql_t3).priv_offset));

    /* 实例本身必须满足最大对齐要求，否则 UBSan 会报 misaligned access */
    QL_CHECK_EQ((uintptr_t)(void *)obj % _Alignof(max_align_t), 0);
    QL_CHECK_EQ((uintptr_t)(void *)p1 % _Alignof(max_align_t), 0);
    QL_CHECK_EQ((uintptr_t)(void *)p2 % _Alignof(max_align_t), 0);
    QL_CHECK_EQ((uintptr_t)(void *)p3 % _Alignof(max_align_t), 0);

    p1->value = 111;
    p2->extra = 222;
    p3->flag = 333;
    QL_CHECK_EQ(p1->value, 111);
    QL_CHECK_EQ(p2->extra, 222);
    QL_CHECK_EQ(p3->flag, 333);

    /* 6. 属性派发：QLT1 的 id 打在 QLT3 对象上仍由 QLT1 处理 */
    QL_CHECK_EQ(ql_object_set_property((QLObject *)obj, QL_T1_PROPERTY_VALUE,
                                       ql_value_int(7)), QL_OK);
    QL_CHECK_EQ(ql_object_get_property((QLObject *)obj, QL_T1_PROPERTY_VALUE).v.i, 7);

    QLType owner = QL_TYPE_NONE;
    QL_CHECK(ql_object_property_owner((QLObject *)obj, QL_T1_PROPERTY_VALUE, &owner));
    QL_CHECK_EQ(owner, t1);
    QL_CHECK(ql_object_property_owner((QLObject *)obj, QL_T3_PROPERTY_FLAG, &owner));
    QL_CHECK_EQ(owner, t3);

    /* 7. 析构顺序：子 → 父 */
    order_reset();
    ql_object_unref((QLObject *)obj);
    QL_CHECK_STR(g_order, "321");

    /* 8. 继承链深度检查：超过配置上限的类注册失败 */
    QL_CHECK(ql_object_is(NULL, QL_TYPE_OBJECT) == 0);

QL_TEST_END()
