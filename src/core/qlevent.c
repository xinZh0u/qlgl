/**
 * @file    qlevent.c
 * @brief   事件连接池与派发（捕获 → 目标 → 冒泡）。
 *
 * 运行期零 malloc：所有连接来自静态池，对象头只存链表首下标。
 */
#include <string.h>

#include <qlgl/qlgl.h>

typedef struct {
    QLObject     *obj;
    ql_event_cb_t cb;
    void         *user_data;
    ql_event_t    event;
    uint32_t      conn_id;      /* 单调递增，不复用，避免 ABA */
    uint16_t      next;         /* 同对象连接链的下一个池下标 */
    uint8_t       removed;
    uint8_t       in_use;
} QLEventConn;

static QLEventConn s_pool[QL_CFG_EVENT_CONN_POOL];
static uint16_t s_free_head = QL_CONN16_NONE;
static uint32_t s_next_conn_id = 1;
static uint32_t s_used;
static uint8_t s_emit_depth;

void ql_event_pool_init(void)
{
    s_free_head = QL_CONN16_NONE;
    for (int i = (int)QL_CFG_EVENT_CONN_POOL - 1; i >= 0; i--) {
        memset(&s_pool[i], 0, sizeof(s_pool[i]));
        s_pool[i].next = s_free_head;
        s_free_head = (uint16_t)i;
    }
    s_next_conn_id = 1;
    s_used = 0;
    s_emit_depth = 0;
}

uint32_t ql_event_pool_usage(void)
{
    return s_used;
}

uint32_t ql_event_pool_capacity(void)
{
    return (uint32_t)QL_CFG_EVENT_CONN_POOL;
}

static void pool_push(uint16_t index)
{
    QLEventConn *conn = &s_pool[index];
    conn->obj = NULL;
    conn->cb = NULL;
    conn->user_data = NULL;
    conn->event = 0;
    conn->conn_id = QL_CONN_ID_NONE;
    conn->removed = 0;
    conn->in_use = 0;
    conn->next = s_free_head;
    s_free_head = index;
    if (s_used > 0)
        s_used--;
}

/* 回收某对象链上所有已标记删除的连接 */
static void conn_gc(QLObject *obj)
{
    if (obj == NULL)
        return;

    uint16_t prev = QL_CONN16_NONE;
    uint16_t cur = (uint16_t)obj->conn_head;
    while (cur != QL_CONN16_NONE) {
        QLEventConn *conn = &s_pool[cur];
        uint16_t next = conn->next;
        if (conn->removed) {
            if (prev == QL_CONN16_NONE)
                obj->conn_head = next;
            else
                s_pool[prev].next = next;
            pool_push(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
    if (obj->conn_head == QL_CONN16_NONE)
        obj->conn_head = QL_CONN16_NONE;
}

uint32_t ql_connect(QLObject *obj, ql_event_t event, ql_event_cb_t cb, void *user_data)
{
    if (obj == NULL || cb == NULL)
        return QL_CONN_ID_NONE;
    if (s_free_head == QL_CONN16_NONE) {
        QL_LOGE("ql_connect: connection pool full (%d)", QL_CFG_EVENT_CONN_POOL);
        return QL_CONN_ID_NONE;
    }

    uint16_t index = s_free_head;
    s_free_head = s_pool[index].next;

    QLEventConn *conn = &s_pool[index];
    conn->obj = obj;
    conn->cb = cb;
    conn->user_data = user_data;
    conn->event = event;
    conn->conn_id = s_next_conn_id++;
    if (conn->conn_id == QL_CONN_ID_NONE)      /* 0 保留为"无效" */
        conn->conn_id = s_next_conn_id++;
    conn->next = (uint16_t)obj->conn_head;
    conn->removed = 0;
    conn->in_use = 1;

    obj->conn_head = (uint32_t)index;
    s_used++;

    return conn->conn_id;
}

ql_err_t ql_disconnect(QLObject *obj, uint32_t conn_id)
{
    if (obj == NULL || conn_id == QL_CONN_ID_NONE)
        return QL_ERR_INVAL;

    for (uint16_t i = (uint16_t)obj->conn_head; i != QL_CONN16_NONE; i = s_pool[i].next) {
        QLEventConn *conn = &s_pool[i];
        if (conn->in_use && conn->conn_id == conn_id) {
            conn->removed = 1;
            if (s_emit_depth == 0)
                conn_gc(obj);
            return QL_OK;
        }
    }
    return QL_ERR_INVAL;
}

int ql_disconnect_all(QLObject *obj)
{
    if (obj == NULL)
        return 0;

    int count = 0;
    uint16_t cur = (uint16_t)obj->conn_head;
    obj->conn_head = QL_CONN16_NONE;
    while (cur != QL_CONN16_NONE) {
        uint16_t next = s_pool[cur].next;
        pool_push(cur);
        count++;
        cur = next;
    }
    return count;
}

uint32_t ql_event_conn_count(QLObject *obj)
{
    if (obj == NULL)
        return 0;

    uint32_t count = 0;
    for (uint16_t i = (uint16_t)obj->conn_head; i != QL_CONN16_NONE; i = s_pool[i].next) {
        if (s_pool[i].in_use && !s_pool[i].removed)
            count++;
    }
    return count;
}

/* ----- 参数 ----- */

QLEventArgs *ql_args_reset(QLEventArgs *args)
{
    if (args != NULL)
        memset(args, 0, sizeof(*args));
    return args;
}

static QLEventArgs *args_push(QLEventArgs *args, QLValue value)
{
    if (args != NULL && args->count < QL_EVENT_ARG_MAX) {
        uint8_t index = args->count++;
        args->arg[index] = value;
        args->type[index] = value.type;
    }
    return args;
}

QLEventArgs *ql_args_int(QLEventArgs *args, int32_t value)
{
    return args_push(args, ql_value_int(value));
}

QLEventArgs *ql_args_uint(QLEventArgs *args, uint32_t value)
{
    return args_push(args, ql_value_uint(value));
}

QLEventArgs *ql_args_fixed(QLEventArgs *args, ql_fixed_t value)
{
    return args_push(args, ql_value_fixed(value));
}

QLEventArgs *ql_args_bool(QLEventArgs *args, int value)
{
    return args_push(args, ql_value_bool(value));
}

QLEventArgs *ql_args_string(QLEventArgs *args, const char *value)
{
    return args_push(args, ql_value_string(value));
}

QLEventArgs *ql_args_ptr(QLEventArgs *args, void *value)
{
    return args_push(args, ql_value_ptr(value));
}

QLEventArgs *ql_args_object(QLEventArgs *args, QLObject *value)
{
    return args_push(args, ql_value_object(value));
}

/* ----- 派发 ----- */

void ql_event_stop_propagation(QLEvent *event)
{
    if (event != NULL)
        event->stop_propagation = 1;
}

void ql_event_prevent_default(QLEvent *event)
{
    if (event != NULL)
        event->prevent_default = 1;
}

static const QLEventDesc *event_desc(QLObject *obj, ql_event_t event)
{
    for (QLObjectClass *klass = obj->klass; klass != NULL; klass = klass->parent) {
        for (uint16_t i = 0; i < klass->event_count; i++) {
            if (klass->events[i].id == event)
                return &klass->events[i];
        }
    }
    return NULL;
}

/* 未登记的事件默认"可冒泡 + 可停止" */
#define QL_EVENT_FLAG_DEFAULT_IMPL \
    (QL_EVENT_FLAG_BUBBLE | QL_EVENT_FLAG_STOPPABLE | QL_EVENT_FLAG_DEFAULT)

static int dispatch(QLObject *current, QLEvent *event, uint32_t flags)
{
    int count = 0;
    event->current_target = current;

    for (uint16_t i = (uint16_t)current->conn_head; i != QL_CONN16_NONE; ) {
        QLEventConn *conn = &s_pool[i];
        uint16_t next = conn->next;      /* 回调可能改动链表或标记删除 */
        if (conn->in_use && !conn->removed && conn->event == event->event) {
            event->user_data = conn->user_data;
            event->conn_id = conn->conn_id;
            conn->cb(event);
            count++;
            if (event->stop_propagation)
                return count;
        }
        i = next;
    }

    /* 默认处理只属于事件目标，且只有登记了 DEFAULT 标志的事件才会调用 */
    if ((flags & QL_EVENT_FLAG_DEFAULT) != 0 && current == event->target &&
        !event->prevent_default) {
        for (QLObjectClass *klass = current->klass; klass != NULL; klass = klass->parent) {
            if (klass->event != NULL) {
                klass->event(current, event);
                break;
            }
        }
    }

    return count;
}

int ql_emit(QLObject *obj, ql_event_t event, const QLEventArgs *args)
{
    if (obj == NULL)
        return 0;

    const QLEventDesc *desc = event_desc(obj, event);
    uint32_t flags = (desc != NULL) ? desc->flags : QL_EVENT_FLAG_DEFAULT_IMPL;

    /* 目标 → 根的路径，供捕获与冒泡复用 */
    QLObject *path[QL_CFG_EVENT_PATH_MAX];
    int depth = 0;
    for (QLObject *node = obj; node != NULL && depth < QL_CFG_EVENT_PATH_MAX;
         node = node->parent) {
        path[depth++] = node;
    }

    QLEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = event;
    ev.target = obj;
    ev.current_target = obj;
    ev.args = args;

    /*
     * 回调和默认处理里可能把路径上的任何对象 unref 掉，
     * 因此整条传播路径都要加守卫引用。
     */
    for (int i = 0; i < depth; i++)
        ql_object_ref(path[i]);
    s_emit_depth++;

    int count = 0;

    if ((flags & QL_EVENT_FLAG_CAPTURE) != 0) {
        for (int i = depth - 1; i > 0 && !ev.stop_propagation; i--)
            count += dispatch(path[i], &ev, flags);
    }

    if (!ev.stop_propagation)
        count += dispatch(obj, &ev, flags);

    if ((flags & QL_EVENT_FLAG_BUBBLE) != 0) {
        for (int i = 1; i < depth && !ev.stop_propagation; i++)
            count += dispatch(path[i], &ev, flags);
    }

    s_emit_depth--;

    if (s_emit_depth == 0) {
        for (int i = 0; i < depth; i++)
            conn_gc(path[i]);
        conn_gc(obj);
    }

    /* 守卫引用按"根 → 目标"的顺序释放，避免父对象先释放时把子对象带走 */
    for (int i = depth - 1; i >= 0; i--)
        ql_object_unref(path[i]);

    return count;
}

/* ----- 参数读取 ----- */

uint8_t ql_event_arg_count(const QLEvent *event)
{
    return (event != NULL && event->args != NULL) ? event->args->count : 0;
}

int32_t ql_event_arg_int(const QLEvent *event, uint8_t index, int32_t fallback)
{
    if (event == NULL || event->args == NULL || index >= event->args->count)
        return fallback;
    return ql_value_get_int(event->args->arg[index], fallback);
}

ql_fixed_t ql_event_arg_fixed(const QLEvent *event, uint8_t index, ql_fixed_t fallback)
{
    if (event == NULL || event->args == NULL || index >= event->args->count)
        return fallback;
    return ql_value_get_fixed(event->args->arg[index], fallback);
}

const char *ql_event_arg_string(const QLEvent *event, uint8_t index, const char *fallback)
{
    if (event == NULL || event->args == NULL || index >= event->args->count)
        return fallback;
    return ql_value_get_string(event->args->arg[index], fallback);
}

void *ql_event_arg_ptr(const QLEvent *event, uint8_t index, void *fallback)
{
    if (event == NULL || event->args == NULL || index >= event->args->count)
        return fallback;
    return ql_value_get_ptr(event->args->arg[index], fallback);
}
