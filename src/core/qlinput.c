/**
 * @file    qlinput.c
 * @brief   平台无关的输入事件队列 + 坐标/修饰键状态 + 一个事件汇（sink）。
 *
 * 这一层**只做两件事**：把后端产生的 `QLInputEvent` 排成队；排空时逐条
 * 交给装了的事件汇。命中测试、hover/active/focus 状态、click 合成全都在
 * 界面层（`ql_ui_input_event()` 那一侧）—— 因为只有界面层才知道
 * HTML/CSS 编译出来的节点树长什么样。
 *
 * 所以"后端认不认识控件"这个问题被彻底消掉了：后端给坐标和键码，
 * 界面层自己决定给谁。
 */
#include <string.h>

#include <qlgl/qlgl.h>

static QLInputEvent s_queue[QL_CFG_INPUT_QUEUE];
static uint16_t     s_head;          /* 下一个写入位置 */
static uint16_t     s_tail;          /* 下一个读取位置 */
static uint16_t     s_count;

static int32_t  s_x;
static int32_t  s_y;
static uint8_t  s_buttons;
static uint8_t  s_mods;

static ql_input_sink s_sink;
static void         *s_sink_user;

/* ------------------------------ 队列 ------------------------------ */

void ql_input_reset(void)
{
    memset(s_queue, 0, sizeof(s_queue));
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_x = 0;
    s_y = 0;
    s_buttons = 0;
    s_mods = 0;
    s_sink = NULL;
    s_sink_user = NULL;
}

void ql_input_set_sink(ql_input_sink sink, void *user)
{
    s_sink = sink;
    s_sink_user = user;
}

/* 事件一入队就把"当前状态"跟着更新：坐标、按住的按钮、修饰键。
 * 这样 push 系列的便捷函数不需要额外的 update 回调，也不会因为
 * 谁先谁后而拿到过期的快照。 */
static void track_state(const QLInputEvent *ev)
{
    switch (ev->type) {
    case QL_INPUT_POINTER_MOVE:
    case QL_INPUT_POINTER_DOWN:
    case QL_INPUT_POINTER_UP:
        s_x = ev->x;
        s_y = ev->y;
        break;
    case QL_INPUT_WHEEL:
        s_x = ev->x;
        s_y = ev->y;
        break;
    case QL_INPUT_KEY_DOWN:
    case QL_INPUT_KEY_UP:
        s_mods = ev->mods;
        break;
    case QL_INPUT_TEXT:
        s_mods = ev->mods;
        break;
    default:
        break;
    }

    if (ev->type == QL_INPUT_POINTER_DOWN)
        s_buttons = (uint8_t)(s_buttons | ev->button);
    else if (ev->type == QL_INPUT_POINTER_UP)
        s_buttons = (uint8_t)(s_buttons & (uint8_t)~ev->button);
}

ql_err_t ql_input_push(const QLInputEvent *ev)
{
    if (ev == NULL)
        return QL_ERR_INVAL;

    track_state(ev);

    ql_err_t err = QL_OK;
    if (s_count >= (uint16_t)QL_CFG_INPUT_QUEUE) {
        /* 丢最旧的：交互事件宁可丢历史也不能卡住输入 */
        s_tail = (uint16_t)((s_tail + 1) % QL_CFG_INPUT_QUEUE);
        s_count--;
        err = QL_ERR_FULL;
    }

    s_queue[s_head] = *ev;
    s_head = (uint16_t)((s_head + 1) % QL_CFG_INPUT_QUEUE);
    s_count++;
    return err;
}

uint32_t ql_input_pending(void)
{
    return (uint32_t)s_count;
}

static int queue_pop(QLInputEvent *out)
{
    if (s_count == 0)
        return 0;
    *out = s_queue[s_tail];
    s_tail = (uint16_t)((s_tail + 1) % QL_CFG_INPUT_QUEUE);
    s_count--;
    return 1;
}

/* ------------------------------ 构造入队 ------------------------------ */

static ql_err_t push_simple(uint8_t type, ql_coord_t x, ql_coord_t y)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.buttons = s_buttons;
    ev.mods = s_mods;
    return ql_input_push(&ev);
}

ql_err_t ql_input_push_move(ql_coord_t x, ql_coord_t y, uint8_t buttons)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_MOVE;
    ev.x = x;
    ev.y = y;
    ev.buttons = buttons;
    ev.mods = s_mods;
    return ql_input_push(&ev);
}

ql_err_t ql_input_push_button(ql_coord_t x, ql_coord_t y, uint8_t button, int down)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)(down ? QL_INPUT_POINTER_DOWN : QL_INPUT_POINTER_UP);
    ev.x = x;
    ev.y = y;
    ev.button = button;
    ev.buttons = s_buttons;
    ev.mods = s_mods;
    return ql_input_push(&ev);
}

ql_err_t ql_input_push_leave(void)
{
    return push_simple(QL_INPUT_POINTER_LEAVE, s_x, s_y);
}

ql_err_t ql_input_push_wheel(ql_coord_t x, ql_coord_t y, int32_t dx, int32_t dy)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_WHEEL;
    ev.x = x;
    ev.y = y;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = s_buttons;
    ev.mods = s_mods;
    return ql_input_push(&ev);
}

ql_err_t ql_input_push_key(uint32_t key, int down, uint8_t mods)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)(down ? QL_INPUT_KEY_DOWN : QL_INPUT_KEY_UP);
    ev.key = key;
    ev.mods = mods;
    ev.x = s_x;
    ev.y = s_y;
    ev.buttons = s_buttons;
    return ql_input_push(&ev);
}

ql_err_t ql_input_push_text(const char *utf8)
{
    if (utf8 == NULL)
        return QL_ERR_INVAL;
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TEXT;
    ev.mods = s_mods;
    ev.x = s_x;
    ev.y = s_y;
    ev.buttons = s_buttons;
    for (size_t i = 0; i + 1 < sizeof(ev.text) && utf8[i] != '\0'; i++)
        ev.text[i] = utf8[i];
    return ql_input_push(&ev);
}

/* ------------------------------ 状态查询 ------------------------------ */

uint8_t    ql_input_buttons(void) { return s_buttons; }
uint8_t    ql_input_mods(void)    { return s_mods; }
ql_coord_t ql_input_x(void)       { return s_x; }
ql_coord_t ql_input_y(void)       { return s_y; }

/* ------------------------------ 派发 ------------------------------ */

int ql_input_dispatch(void)
{
    int handled = 0;
    QLInputEvent ev;

    /* 回调里可能继续 push，因此按"取一条派发一条"的方式循环 */
    while (queue_pop(&ev)) {
        if (s_sink != NULL)
            s_sink(&ev, s_sink_user);
        handled++;
    }
    return handled;
}
