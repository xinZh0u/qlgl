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
 *
 * **多窗口**：队列/状态/汇都收在 `QLInput` 实例里，一窗口一个；后端把
 * 事件推进自己窗口的实例。文件末尾的全局函数等价于操作"进程默认实例"，
 * 只用于单窗口与测试（保持旧 API 不变）。
 */
#include <string.h>

#include <qlgl/qlgl.h>

struct QLInput {
    QLInputEvent queue[QL_CFG_INPUT_QUEUE];
    uint16_t     head;          /* 下一个写入位置 */
    uint16_t     tail;          /* 下一个读取位置 */
    uint16_t     count;

    int32_t  x;
    int32_t  y;
    uint8_t  buttons;
    uint8_t  mods;

    ql_input_sink sink;
    void         *sink_user;
};

/* 旧 API 的落脚点：单窗口程序/测试不需要显式创建实例 */
static QLInput s_default;

QLInput *ql_input_default(void)
{
    return &s_default;
}

/* 所有 `_ex` 接口：`in == NULL` 等价于进程默认实例（单窗口/测试便利） */
static QLInput *resolve_input(QLInput *in)
{
    return (in != NULL) ? in : &s_default;
}

static const QLInput *resolve_input_c(const QLInput *in)
{
    return (in != NULL) ? in : &s_default;
}

QLInput *ql_input_create(void)
{
    return (QLInput *)ql_mem_calloc(1, sizeof(QLInput));
}

void ql_input_destroy(QLInput *in)
{
    if (in == NULL || in == &s_default)
        return;
    ql_mem_free(in);
}

/* ------------------------------ 队列 ------------------------------ */

void ql_input_reset_ex(QLInput *in)
{
    in = resolve_input(in);
    memset(in->queue, 0, sizeof(in->queue));
    in->head = 0;
    in->tail = 0;
    in->count = 0;
    in->x = 0;
    in->y = 0;
    in->buttons = 0;
    in->mods = 0;
    in->sink = NULL;
    in->sink_user = NULL;
}

void ql_input_set_sink_ex(QLInput *in, ql_input_sink sink, void *user)
{
    in = resolve_input(in);
    in->sink = sink;
    in->sink_user = user;
}

/* 事件一入队就把"当前状态"跟着更新：坐标、按住的按钮、修饰键。
 * 这样 push 系列的便捷函数不需要额外的 update 回调，也不会因为
 * 谁先谁后而拿到过期的快照。 */
static void track_state(QLInput *in, const QLInputEvent *ev)
{
    switch (ev->type) {
    case QL_INPUT_POINTER_MOVE:
        in->x = ev->x;
        in->y = ev->y;
        /* 移动事件自带按钮快照，直接同步（push_button 的位运算只处理按下/抬起） */
        in->buttons = ev->buttons;
        break;
    case QL_INPUT_POINTER_DOWN:
    case QL_INPUT_POINTER_UP:
        in->x = ev->x;
        in->y = ev->y;
        break;
    case QL_INPUT_WHEEL:
    case QL_INPUT_TOUCH:
        in->x = ev->x;
        in->y = ev->y;
        break;
    case QL_INPUT_KEY_DOWN:
    case QL_INPUT_KEY_UP:
        in->mods = ev->mods;
        break;
    case QL_INPUT_TEXT:
        in->mods = ev->mods;
        break;
    default:
        break;
    }

    if (ev->type == QL_INPUT_POINTER_DOWN)
        in->buttons = (uint8_t)(in->buttons | ev->button);
    else if (ev->type == QL_INPUT_POINTER_UP)
        in->buttons = (uint8_t)(in->buttons & (uint8_t)~ev->button);
}

ql_err_t ql_input_push_ex(QLInput *in, const QLInputEvent *ev)
{
    if (ev == NULL)
        return QL_ERR_INVAL;
    in = resolve_input(in);

    track_state(in, ev);

    ql_err_t err = QL_OK;
    if (in->count >= (uint16_t)QL_CFG_INPUT_QUEUE) {
        /* 丢最旧的：交互事件宁可丢历史也不能卡住输入 */
        in->tail = (uint16_t)((in->tail + 1) % QL_CFG_INPUT_QUEUE);
        in->count--;
        err = QL_ERR_FULL;
    }

    in->queue[in->head] = *ev;
    in->head = (uint16_t)((in->head + 1) % QL_CFG_INPUT_QUEUE);
    in->count++;
    return err;
}

uint32_t ql_input_pending_ex(const QLInput *in)
{
    return (uint32_t)resolve_input_c(in)->count;
}

static int queue_pop(QLInput *in, QLInputEvent *out)
{
    if (in->count == 0)
        return 0;
    *out = in->queue[in->tail];
    in->tail = (uint16_t)((in->tail + 1) % QL_CFG_INPUT_QUEUE);
    in->count--;
    return 1;
}

/* ------------------------------ 构造入队 ------------------------------ */

static ql_err_t push_simple(QLInput *in, uint8_t type, ql_coord_t x, ql_coord_t y)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.buttons = (in != NULL) ? in->buttons : 0;
    ev.mods = (in != NULL) ? in->mods : 0;
    return ql_input_push_ex(in, &ev);
}

ql_err_t ql_input_push_move_ex(QLInput *in, ql_coord_t x, ql_coord_t y,
                               uint8_t buttons)
{
    in = resolve_input(in);
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_MOVE;
    ev.x = x;
    ev.y = y;
    ev.buttons = buttons;
    ev.mods = (in != NULL) ? in->mods : 0;
    return ql_input_push_ex(in, &ev);
}

ql_err_t ql_input_push_button_ex(QLInput *in, ql_coord_t x, ql_coord_t y,
                                 uint8_t button, int down)
{
    in = resolve_input(in);
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)(down ? QL_INPUT_POINTER_DOWN : QL_INPUT_POINTER_UP);
    ev.x = x;
    ev.y = y;
    ev.button = button;
    ev.buttons = (in != NULL) ? in->buttons : 0;
    ev.mods = (in != NULL) ? in->mods : 0;
    return ql_input_push_ex(in, &ev);
}

ql_err_t ql_input_push_leave_ex(QLInput *in)
{
    in = resolve_input(in);
    if (in == NULL)
        return QL_ERR_INVAL;
    return push_simple(in, QL_INPUT_POINTER_LEAVE, in->x, in->y);
}

ql_err_t ql_input_push_wheel_ex(QLInput *in, ql_coord_t x, ql_coord_t y,
                                int32_t dx, int32_t dy)
{
    in = resolve_input(in);
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_WHEEL;
    ev.x = x;
    ev.y = y;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = (in != NULL) ? in->buttons : 0;
    ev.mods = (in != NULL) ? in->mods : 0;
    return ql_input_push_ex(in, &ev);
}

/*
 * 触摸点：一次一根手指的状态变化。`touch_count` 由后端数（它看得见所有点），
 * 界面层靠它区分"单指拖动"和"双指捏合"。
 */
ql_err_t ql_input_push_touch_ex(QLInput *in, ql_coord_t x, ql_coord_t y,
                                uint8_t phase, uint8_t touch_id,
                                uint8_t touch_count)
{
    in = resolve_input(in);
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TOUCH;
    ev.x = x;
    ev.y = y;
    ev.button = phase;
    ev.touch_id = touch_id;
    ev.touch_count = touch_count;
    ev.buttons = (in != NULL) ? in->buttons : 0;
    ev.mods = (in != NULL) ? in->mods : 0;
    return ql_input_push_ex(in, &ev);
}

ql_err_t ql_input_push_key_ex(QLInput *in, uint32_t key, int down, uint8_t mods)
{
    in = resolve_input(in);
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)(down ? QL_INPUT_KEY_DOWN : QL_INPUT_KEY_UP);
    ev.key = key;
    ev.mods = mods;
    if (in != NULL) {
        ev.x = in->x;
        ev.y = in->y;
        ev.buttons = in->buttons;
    }
    return ql_input_push_ex(in, &ev);
}

ql_err_t ql_input_push_text_ex(QLInput *in, const char *utf8)
{
    in = resolve_input(in);
    if (utf8 == NULL)
        return QL_ERR_INVAL;
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TEXT;
    if (in != NULL) {
        ev.mods = in->mods;
        ev.x = in->x;
        ev.y = in->y;
        ev.buttons = in->buttons;
    }
    /* 按 UTF-8 码点边界截断：宁可不收最后一个不完整的字符 */
    size_t n = strlen(utf8);
    if (n > sizeof(ev.text) - 1u)
        n = ql_utf8_valid_len(utf8, sizeof(ev.text) - 1u);
    if (n > 0)
        memcpy(ev.text, utf8, n);
    ev.text[n] = '\0';
    return ql_input_push_ex(in, &ev);
}

/* ------------------------------ 状态查询 ------------------------------ */

uint8_t ql_input_buttons_ex(const QLInput *in) { return resolve_input_c(in)->buttons; }
uint8_t ql_input_mods_ex(const QLInput *in)    { return resolve_input_c(in)->mods; }
ql_coord_t ql_input_x_ex(const QLInput *in)    { return resolve_input_c(in)->x; }
ql_coord_t ql_input_y_ex(const QLInput *in)    { return resolve_input_c(in)->y; }

/* ------------------------------ 派发 ------------------------------ */

int ql_input_dispatch_ex(QLInput *in)
{
    in = resolve_input(in);

    int handled = 0;
    QLInputEvent ev;

    /* 回调里可能继续 push，因此按"取一条派发一条"的方式循环 */
    while (queue_pop(in, &ev)) {
        if (in->sink != NULL)
            in->sink(&ev, in->sink_user);
        handled++;
    }
    return handled;
}

/* ------------------------------ 旧 API：默认实例 ------------------------------ */

void ql_input_reset(void) { ql_input_reset_ex(&s_default); }

void ql_input_set_sink(ql_input_sink sink, void *user)
{
    ql_input_set_sink_ex(&s_default, sink, user);
}

ql_err_t ql_input_push(const QLInputEvent *ev) { return ql_input_push_ex(&s_default, ev); }

uint32_t ql_input_pending(void) { return ql_input_pending_ex(&s_default); }

int ql_input_dispatch(void) { return ql_input_dispatch_ex(&s_default); }

ql_err_t ql_input_push_move(ql_coord_t x, ql_coord_t y, uint8_t buttons)
{
    return ql_input_push_move_ex(&s_default, x, y, buttons);
}

ql_err_t ql_input_push_button(ql_coord_t x, ql_coord_t y, uint8_t button, int down)
{
    return ql_input_push_button_ex(&s_default, x, y, button, down);
}

ql_err_t ql_input_push_leave(void) { return ql_input_push_leave_ex(&s_default); }

ql_err_t ql_input_push_wheel(ql_coord_t x, ql_coord_t y, int32_t dx, int32_t dy)
{
    return ql_input_push_wheel_ex(&s_default, x, y, dx, dy);
}

ql_err_t ql_input_push_touch(ql_coord_t x, ql_coord_t y, uint8_t phase,
                             uint8_t touch_id, uint8_t touch_count)
{
    return ql_input_push_touch_ex(&s_default, x, y, phase, touch_id, touch_count);
}

ql_err_t ql_input_push_key(uint32_t key, int down, uint8_t mods)
{
    return ql_input_push_key_ex(&s_default, key, down, mods);
}

ql_err_t ql_input_push_text(const char *utf8)
{
    return ql_input_push_text_ex(&s_default, utf8);
}

uint8_t ql_input_buttons(void) { return ql_input_buttons_ex(&s_default); }
uint8_t ql_input_mods(void)    { return ql_input_mods_ex(&s_default); }
ql_coord_t ql_input_x(void)    { return ql_input_x_ex(&s_default); }
ql_coord_t ql_input_y(void)    { return ql_input_y_ex(&s_default); }
