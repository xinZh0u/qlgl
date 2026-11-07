/**
 * @file    example_object.c
 * @brief   完整示例：自定义一个类（计数器），带属性、事件、父子关系。
 *
 * 运行：
 *   cmake --build build && ./build/example_object
 */
#include <stdio.h>

#include <qlgl/qlgl.h>

/* ------------------------- 1. 定义属性与事件 id ------------------------- */
QL_PROPERTY_ENUM_START(QL_COUNTER, QLCounter, QL_OBJECT)
    QL_COUNTER_PROPERTY_VALUE,
    QL_COUNTER_PROPERTY_STEP
QL_PROPERTY_ENUM_END(QL_COUNTER, 10)

QL_EVENT_ENUM_START(QL_COUNTER, QLCounter, QL_OBJECT)
    QL_COUNTER_EVENT_CHANGED,
    QL_COUNTER_EVENT_LIMIT
QL_EVENT_ENUM_END(QL_COUNTER, 10)

/* ------------------------- 2. 声明类 ------------------------- */
QL_DECLARE(QLCounter, ql_counter)

struct QLCounter        { QLObject parent; };
struct QLCounterClass   { QLObjectClass parent; };
struct QLCounterPrivate { int32_t value; int32_t step; };

QL_DEFINE_WITH_PRIVATE(QL_COUNTER, QLCounter, ql_counter, ql_object)

/* ------------------------- 3. 实现类 ------------------------- */
static const QLEventDesc ql_counter_events[] = {
    { "changed", QL_COUNTER_EVENT_CHANGED, QL_EVENT_FLAG_BUBBLE },
    { "limit",   QL_COUNTER_EVENT_LIMIT,   QL_EVENT_FLAG_BUBBLE | QL_EVENT_FLAG_STOPPABLE },
};

static ql_err_t ql_counter_set_property(QLObject *obj, ql_property_t property, QLValue value)
{
    QLCounter *self = (QLCounter *)(void *)obj;
    struct QLCounterPrivate *priv = ql_counter_get_private(self);

    switch (property) {
    case QL_COUNTER_PROPERTY_VALUE:
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->value = value.v.i;
        /* 属性变化直接产生事件：调用方不需要自己再发一次 */
        QL_EMIT_INT(obj, QL_COUNTER_EVENT_CHANGED, priv->value);
        return QL_OK;
    case QL_COUNTER_PROPERTY_STEP:
        if (value.type != QL_VALUE_INT)
            return QL_ERR_INVAL;
        priv->step = value.v.i;
        return QL_OK;
    default:
        return QL_ERR_INVAL;
    }
}

static QLValue ql_counter_get_property(QLObject *obj, ql_property_t property)
{
    struct QLCounterPrivate *priv = ql_counter_get_private((QLCounter *)(void *)obj);
    switch (property) {
    case QL_COUNTER_PROPERTY_VALUE: return ql_value_int(priv->value);
    case QL_COUNTER_PROPERTY_STEP:  return ql_value_int(priv->step);
    default:                        return QL_VALUE_NULL;
    }
}

static void ql_counter_class_init(QLCounterClass *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)klass;
    obj_class->set_property = ql_counter_set_property;
    obj_class->get_property = ql_counter_get_property;
    obj_class->events = ql_counter_events;
    obj_class->event_count = (uint16_t)QL_ARRAY_SIZE(ql_counter_events);
}

static void ql_counter_init(QLCounter *self)
{
    struct QLCounterPrivate *priv = ql_counter_get_private(self);
    priv->value = 0;
    priv->step = 1;
}

static void ql_counter_deinit(QLCounter *self) { QL_UNUSED(self); }

/* ------------------------- 4. 使用 ------------------------- */
static void on_changed(QLEvent *event)
{
    printf("  [changed] %s = %d（当前处理节点：%s）\n",
           ql_object_type_name(event->target),
           ql_event_arg_int(event, 0, -1),
           ql_object_type_name(event->current_target));
}

static void on_root_changed(QLEvent *event)
{
    printf("  [root 收到冒泡] 目标 id=%d\n", ql_event_arg_int(event, 0, -1));
}

int main(void)
{
    printf("== example_object ==\n");

    if (qlgl_init() != QL_OK) {
        printf("初始化失败\n");
        return 1;
    }

    QLType counter_type = QL_REGISTER(ql_counter, QL_TYPE_OBJECT);
    if (counter_type == QL_TYPE_NONE) {
        printf("注册 QLCounter 失败\n");
        return 1;
    }
    printf("QLCounter 已注册，type id = %u，私有区 %u 字节\n",
           (unsigned)counter_type, (unsigned)QL_CLASS_INFO(ql_counter).own_private_size);

    /* 根对象挂一个监听器，验证冒泡 */
    QLObject *root = ql_object_new(QL_TYPE_OBJECT);
    ql_connect(root, QL_COUNTER_EVENT_CHANGED, on_root_changed, NULL);

    QLCounter *counter = (QLCounter *)ql_object_new(counter_type);
    ql_object_set_parent((QLObject *)counter, root);   /* 保留我们自己的引用 */
    ql_connect((QLObject *)counter, QL_COUNTER_EVENT_CHANGED, on_changed, NULL);

    printf("-- 设置 VALUE = 1 --\n");
    ql_object_set_property((QLObject *)counter, QL_COUNTER_PROPERTY_VALUE, ql_value_int(1));

    printf("-- 设置 VALUE = 2（类型错误应被拒绝）--\n");
    ql_err_t err = ql_object_set_property((QLObject *)counter, QL_COUNTER_PROPERTY_VALUE,
                                          ql_value_string("bad"));
    printf("  返回值 = %d\n", (int)err);

    printf("-- 递增：value(%d) + step(%d) --\n",
           ql_object_get_property((QLObject *)counter, QL_COUNTER_PROPERTY_VALUE).v.i,
           ql_object_get_property((QLObject *)counter, QL_COUNTER_PROPERTY_STEP).v.i);
    int32_t value = ql_object_get_property((QLObject *)counter, QL_COUNTER_PROPERTY_VALUE).v.i;
    int32_t step = ql_object_get_property((QLObject *)counter, QL_COUNTER_PROPERTY_STEP).v.i;
    ql_object_set_property((QLObject *)counter, QL_COUNTER_PROPERTY_VALUE,
                           ql_value_int(value + step));

    /* 销毁：父对象会带走子对象，但这里我们自己也持有一份引用，先归还 */
    ql_object_unref((QLObject *)counter);
    ql_object_unref(root);

    QLMemStats stats;
    ql_mem_stats(&stats);
    printf("内存：分配 %zu 次 / 释放 %zu 次 / 存活 %zu 字节\n",
           stats.alloc_count, stats.free_count, stats.bytes_live);

    qlgl_shutdown();
    printf("== 结束 ==\n");
    return stats.bytes_live == 0 ? 0 : 1;
}
