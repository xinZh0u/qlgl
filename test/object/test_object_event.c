/**
 * @file    test_object_event.c
 * @brief   示例：事件连接、参数打包、捕获/冒泡、停止传播、连接池。
 */
#include "../ql_test.h"

static char g_trace[32];
static int  g_trace_len;
static int  g_default_count;
static int  g_stop_at_b;
static int  g_prevent_default;

static void trace_reset(void) { g_trace_len = 0; g_trace[0] = '\0'; }
static void trace_push(char tag)
{
    if (g_trace_len < (int)sizeof(g_trace) - 1)
        g_trace[g_trace_len++] = tag;
    g_trace[g_trace_len] = '\0';
}

/* ------------------------------ 事件类 ------------------------------ */
QL_PROPERTY_ENUM_EMPTY(QL_EVT, QLEvt, QL_OBJECT, 10)

QL_EVENT_ENUM_START(QL_EVT, QLEvt, QL_OBJECT)
    QL_EVT_EVENT_PING,
    QL_EVT_EVENT_DATA
QL_EVENT_ENUM_END(QL_EVT, 10)

QL_DECLARE(QLEvt, ql_evt)

struct QLEvt        { QLObject parent; };
struct QLEvtClass   { QLObjectClass parent; };
struct QLEvtPrivate { int32_t hits; };

QL_DEFINE_WITH_PRIVATE(QL_EVT, QLEvt, ql_evt, ql_object)

static ql_err_t ql_evt_default(QLObject *self, QLEvent *event)
{
    QL_UNUSED(self);
    QL_UNUSED(event);
    g_default_count++;
    return QL_OK;
}

static const QLEventDesc ql_evt_events[] = {
    { "ping", QL_EVT_EVENT_PING,
      QL_EVENT_FLAG_BUBBLE | QL_EVENT_FLAG_STOPPABLE | QL_EVENT_FLAG_DEFAULT },
    { "data", QL_EVT_EVENT_DATA, QL_EVENT_FLAG_BUBBLE },
};

static void ql_evt_class_init(QLEvtClass *klass)
{
    QLObjectClass *obj_class = (QLObjectClass *)klass;
    obj_class->events = ql_evt_events;
    obj_class->event_count = (uint16_t)QL_ARRAY_SIZE(ql_evt_events);
    obj_class->event = ql_evt_default;
}

static void ql_evt_init(QLEvt *self)   { QL_UNUSED(self); }
static void ql_evt_deinit(QLEvt *self) { QL_UNUSED(self); }

/* ------------------------------ 回调 ------------------------------ */
static QLObject *g_a;
static QLObject *g_b;
static QLObject *g_c;

static void cb_a(QLEvent *event) { QL_UNUSED(event); trace_push('a'); }
static void cb_b(QLEvent *event)
{
    QL_UNUSED(event);
    trace_push('b');
    if (g_stop_at_b)
        ql_event_stop_propagation(event);
}
static void cb_c(QLEvent *event)
{
    QL_UNUSED(event);
    trace_push('c');
    if (g_prevent_default)
        ql_event_prevent_default(event);
}

static QLObject *g_victim;
static void cb_drop_target(QLEvent *event)
{
    /* 故意在回调里释放目标自身的全部引用 */
    ql_object_unref(event->target);
    g_victim = (QLObject *)0;
}

static int g_arg_hits;
static void cb_args(QLEvent *event)
{
    g_arg_hits++;
    QL_CHECK_EQ(ql_event_arg_count(event), 2);
    QL_CHECK_EQ(ql_event_arg_int(event, 0, -1), 42);
    QL_CHECK_STR(ql_event_arg_string(event, 1, ""), "hi");
    QL_CHECK_EQ(ql_event_arg_int(event, 9, -7), -7);      /* 越界取 fallback */
    QL_CHECK(event->target == g_c);
    QL_CHECK(event->current_target == g_c);
}

QL_TEST_BEGIN("test_object_event")

    QLType type = QL_REGISTER(ql_evt, QL_TYPE_OBJECT);
    QL_CHECK(type != QL_TYPE_NONE);

    g_a = ql_object_new(type);
    g_b = ql_object_new(type);
    g_c = ql_object_new(type);
    QL_CHECK(g_a && g_b && g_c);
    QL_CHECK_EQ(ql_object_set_parent(g_b, g_a), QL_OK);
    QL_CHECK_EQ(ql_object_set_parent(g_c, g_b), QL_OK);

    /* 1. 连接返回非零且互不相同的 id */
    uint32_t conn_a = ql_connect(g_a, QL_EVT_EVENT_PING, cb_a, NULL);
    uint32_t conn_b = ql_connect(g_b, QL_EVT_EVENT_PING, cb_b, NULL);
    uint32_t conn_c = ql_connect(g_c, QL_EVT_EVENT_PING, cb_c, NULL);
    QL_CHECK(conn_a != QL_CONN_ID_NONE);
    QL_CHECK(conn_b != QL_CONN_ID_NONE);
    QL_CHECK(conn_c != QL_CONN_ID_NONE);
    QL_CHECK(conn_a != conn_b && conn_b != conn_c && conn_a != conn_c);
    QL_CHECK_EQ(ql_event_conn_count(g_c), 1);

    /* 三个回调同时监听 DATA 事件，用于验证"未登记 DEFAULT 的事件" */
    QL_CHECK(ql_connect(g_a, QL_EVT_EVENT_DATA, cb_a, NULL) != QL_CONN_ID_NONE);
    QL_CHECK(ql_connect(g_b, QL_EVT_EVENT_DATA, cb_b, NULL) != QL_CONN_ID_NONE);
    QL_CHECK(ql_connect(g_c, QL_EVT_EVENT_DATA, cb_c, NULL) != QL_CONN_ID_NONE);

    /* 2. 冒泡顺序：目标 → 父 → 根；默认处理只在目标上跑一次 */
    trace_reset();
    g_default_count = 0;
    QL_CHECK_EQ(ql_emit(g_c, QL_EVT_EVENT_PING, NULL), 3);
    QL_CHECK_STR(g_trace, "cba");
    QL_CHECK_EQ(g_default_count, 1);

    /* 3. 阻止默认处理 */
    trace_reset();
    g_default_count = 0;
    g_prevent_default = 1;
    QL_CHECK_EQ(ql_emit(g_c, QL_EVT_EVENT_PING, NULL), 3);
    QL_CHECK_STR(g_trace, "cba");
    QL_CHECK_EQ(g_default_count, 0);
    g_prevent_default = 0;

    /* 4. 停止传播：b 处截断，a 收不到 */
    trace_reset();
    g_default_count = 0;
    g_stop_at_b = 1;
    QL_CHECK_EQ(ql_emit(g_c, QL_EVT_EVENT_PING, NULL), 2);
    QL_CHECK_STR(g_trace, "cb");
    QL_CHECK_EQ(g_default_count, 1);
    g_stop_at_b = 0;

    /* 5. 未登记 DEFAULT 的事件不会触发默认处理，但仍可冒泡 */
    trace_reset();
    g_default_count = 0;
    QL_CHECK_EQ(ql_emit(g_c, QL_EVT_EVENT_DATA, NULL), 3);
    QL_CHECK_STR(g_trace, "cba");
    QL_CHECK_EQ(g_default_count, 0);

    /* 6. 参数打包：整型 + 字符串，回调里按索引取 */
    g_arg_hits = 0;
    uint32_t conn_args = ql_connect(g_c, QL_EVT_EVENT_DATA, cb_args, NULL);
    QL_CHECK(conn_args != QL_CONN_ID_NONE);
    trace_reset();
    QL_EMIT_2(g_c, QL_EVT_EVENT_DATA, ql_args_int, 42, ql_args_string, "hi");
    QL_CHECK_EQ(g_arg_hits, 1);
    QL_CHECK_STR(g_trace, "cba");

    /* 7. 按 id 断开 */
    QL_CHECK(ql_disconnect(g_a, conn_a) == QL_OK);
    QL_CHECK(ql_disconnect(g_a, conn_a) == QL_ERR_INVAL);   /* 重复断开 */
    QL_CHECK_EQ(ql_event_conn_count(g_a), 1);               /* DATA 那条还在 */

    /* 8. 池：能装满，满后返回无效 id，清空后恢复 */
    QLObject *pool_obj = ql_object_new(type);
    QL_CHECK(pool_obj != NULL);
    uint32_t used = ql_event_pool_usage();
    uint32_t capacity = ql_event_pool_capacity();
    uint32_t successes = 0;
    while (ql_connect(pool_obj, QL_EVT_EVENT_DATA, cb_a, NULL) != QL_CONN_ID_NONE)
        successes++;
    QL_CHECK_EQ(successes, capacity - used);
    QL_CHECK_EQ(ql_event_pool_usage(), capacity);
    QL_CHECK_EQ(ql_disconnect_all(pool_obj), (int)successes);
    QL_CHECK_EQ(ql_event_pool_usage(), used);
    ql_object_unref(pool_obj);

    /* 9. 回调里把目标对象本身 unref 掉也不能崩（emit 自带守卫引用） */
    {
        QLObject *victim = ql_object_new(type);
        QL_CHECK(victim != NULL);
        g_victim = victim;
        QL_CHECK(ql_connect(victim, QL_EVT_EVENT_DATA, cb_drop_target, NULL) != QL_CONN_ID_NONE);
        QL_CHECK(ql_object_ref_count(victim) == 1);
        QL_EMIT_0(victim, QL_EVT_EVENT_DATA);   /* 回调内把引用降到 0 */
        QL_CHECK_EQ(g_victim, NULL);
    }

    /* 10. 销毁对象时自动归还连接 */
    QL_CHECK_EQ(ql_disconnect_all(g_c), 3);      /* PING + DATA + 参数回调 */
    QL_CHECK_EQ(ql_disconnect_all(g_b), 2);      /* PING + DATA */
    QL_CHECK_EQ(ql_disconnect_all(g_a), 1);      /* DATA */
    QL_CHECK_EQ(ql_event_pool_usage(), 0);

    ql_object_unref(g_c);
    ql_object_unref(g_b);
    ql_object_unref(g_a);

QL_TEST_END()
