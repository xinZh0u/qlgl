/**
 * @file    qlinput.c
 * @brief   输入队列、命中测试、hover/pressed/focus 状态机与事件合成。
 *
 * 语义尽量对齐 DOM：
 *   mousemove   → POINTER_MOVE（悬停目标变化时先发 LEAVE 再发 ENTER）
 *   mousedown   → POINTER_DOWN，记录"按下目标"
 *   mouseup     → POINTER_UP，若与按下目标相同则再发 CLICK
 *   wheel       → WHEEL
 *   keydown/up  → KEY_DOWN/KEY_UP（目标是焦点控件，事件冒泡到根）
 *   文本输入     → TEXT（目标是焦点控件）
 *
 * 状态位（QLWidgetPrivate::state）在命中变化时沿父链一起更新，
 * 与 CSS 的 :hover 会作用到祖先一致。
 */
#include <string.h>

#include <qlgl/qlgl.h>

/* 按下与抬起之间允许的最大位移，超过就不算点击（与常见 GUI 库一致） */
#define QL_CLICK_SLOP   4

static QLInputEvent s_queue[QL_CFG_INPUT_QUEUE];
static uint16_t     s_head;          /* 下一个写入位置 */
static uint16_t     s_tail;          /* 下一个读取位置 */
static uint16_t     s_count;

static QLObject *s_root;             /* 显式设置的输入根，可为 NULL */
static QLObject *s_hover;
static QLObject *s_pressed;
static QLObject *s_focus;

static int32_t  s_press_x;
static int32_t  s_press_y;
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
    s_root = NULL;
    s_hover = NULL;
    s_pressed = NULL;
    s_focus = NULL;
    s_press_x = 0;
    s_press_y = 0;
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

ql_err_t ql_input_push(const QLInputEvent *ev)
{
    if (ev == NULL)
        return QL_ERR_INVAL;

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
    ev.buttons = s_buttons;   /* 由 ql_input_handle 更新，这里只是快照 */
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

/* ------------------------------ 根与状态 ------------------------------ */

void ql_input_set_root(QLObject *root)
{
    if (s_root == root)
        return;
    /* 换根时旧路径上的状态位必须清掉，否则旧树会一直显示"悬停" */
    s_hover = NULL;
    s_pressed = NULL;
    s_focus = NULL;
    s_root = root;
}

QLObject *ql_input_root(void)
{
    return (s_root != NULL) ? s_root : ql_render_root();
}

QLObject *ql_input_hover(void)   { return s_hover; }
QLObject *ql_input_pressed(void) { return s_pressed; }
QLObject *ql_input_focus(void)   { return s_focus; }
uint8_t   ql_input_buttons(void) { return s_buttons; }
uint8_t   ql_input_mods(void)    { return s_mods; }
ql_coord_t ql_input_x(void)      { return s_x; }
ql_coord_t ql_input_y(void)      { return s_y; }

/* 沿父链设置/清除一个状态位，只在真正变化时打脏 */
static void set_state_chain(QLObject *obj, uint8_t mask, int on)
{
    for (QLObject *node = obj; node != NULL; node = ql_object_parent(node)) {
        if (!ql_object_is(node, QL_WIDGET_TYPE))
            continue;
        if (ql_widget_set_state(node, mask, on))
            ql_render_invalidate(node);
    }
}

/* ------------------------------ 命中测试 ------------------------------ */

static QLObject *hit_walk(QLObject *node, ql_coord_t x, ql_coord_t y, QLRect clip)
{
    if (node == NULL || (node->flags & QL_OBJ_FLAG_HIDDEN) != 0)
        return NULL;

    QLRect box = clip;
    int is_widget = ql_object_is(node, QL_WIDGET_TYPE);

    if (is_widget) {
        if (!ql_widget_is_visible(node))
            return NULL;
        box = ql_widget_abs_rect(node);
    }

    /* 子控件后画，因此后挂的兄弟优先；递归天然得到"最后一个命中" */
    QLObject *hit = NULL;
    for (QLObject *child = ql_object_first_child(node); child != NULL;
         child = ql_object_next_sibling(child)) {
        QLObject *found = hit_walk(child, x, y, box);
        if (found != NULL)
            hit = found;
    }
    if (hit != NULL)
        return hit;

    if (!is_widget)
        return NULL;
    if (!ql_widget_pointer_events(node))
        return NULL;
    if (x < box.x || x >= ql_rect_right(box) || y < box.y || y >= ql_rect_bottom(box))
        return NULL;

    /* 父控件裁剪子控件时，超出父盒子的部分不参与命中 */
    if (x < clip.x || x >= ql_rect_right(clip) || y < clip.y || y >= ql_rect_bottom(clip))
        return NULL;

    return node;
}

QLObject *ql_input_hit_test(QLObject *root, ql_coord_t x, ql_coord_t y)
{
    QLObject *start = (root != NULL) ? root : ql_input_root();
    if (start == NULL)
        return NULL;

    QLDispDriver *disp = ql_disp_get();
    QLRect screen = (disp != NULL) ? ql_rect_make(0, 0, disp->width, disp->height)
                                  : ql_rect_make(0, 0, 0x7FFFFFFF, 0x7FFFFFFF);
    return hit_walk(start, x, y, screen);
}

/* ------------------------------ 事件派发 ------------------------------ */

static void emit_pointer(QLObject *target, ql_event_t event, int32_t x, int32_t y,
                         int32_t extra)
{
    QL_EMIT_3(target, event, ql_args_int, x, ql_args_int, y, ql_args_int, extra);
}

static void update_hover(QLObject *target)
{
    if (s_hover == target)
        return;

    QLObject *old = s_hover;
    if (old != NULL)
        set_state_chain(old, QL_WIDGET_STATE_HOVER, 0);
    s_hover = target;
    if (target != NULL)
        set_state_chain(target, QL_WIDGET_STATE_HOVER, 1);

    if (old != NULL)
        emit_pointer(old, QL_WIDGET_EVENT_POINTER_LEAVE, s_x, s_y, 0);
    if (target != NULL)
        emit_pointer(target, QL_WIDGET_EVENT_POINTER_ENTER, s_x, s_y, 0);
}

ql_err_t ql_input_set_focus(QLObject *obj)
{
    if (obj != NULL && !ql_object_is(obj, QL_WIDGET_TYPE))
        return QL_ERR_INVAL;
    if (s_focus == obj)
        return QL_OK;

    QLObject *old = s_focus;
    s_focus = obj;
    if (old != NULL) {
        set_state_chain(old, QL_WIDGET_STATE_FOCUS, 0);
        QL_EMIT_0(old, QL_WIDGET_EVENT_FOCUS_OUT);
    }
    if (obj != NULL) {
        set_state_chain(obj, QL_WIDGET_STATE_FOCUS, 1);
        QL_EMIT_0(obj, QL_WIDGET_EVENT_FOCUS_IN);
    }
    return QL_OK;
}

static void handle_move(const QLInputEvent *ev)
{
    s_x = ev->x;
    s_y = ev->y;
    s_buttons = ev->buttons;

    QLObject *target = ql_input_hit_test(NULL, ev->x, ev->y);

    /*
     * 按住鼠标拖动时事件仍然送给"按下目标"（等价于 DOM 的隐式指针捕获），
     * 否则从按钮上拖出去再松手就收不到 UP。
     */
    if (s_pressed != NULL && (ev->buttons & QL_BUTTON_BIT_LEFT) != 0)
        target = s_pressed;

    update_hover(target);
    if (target != NULL)
        emit_pointer(target, QL_WIDGET_EVENT_POINTER_MOVE, ev->x, ev->y, ev->buttons);
}

static void handle_button(const QLInputEvent *ev, int down)
{
    s_x = ev->x;
    s_y = ev->y;
    if (ev->mods != 0)
        s_mods = ev->mods;

    uint8_t bit = (uint8_t)(1u << (ev->button & 7));
    if (down)
        s_buttons = (uint8_t)(s_buttons | bit);
    else
        s_buttons = (uint8_t)(s_buttons & ~bit);

    QLObject *target = ql_input_hit_test(NULL, ev->x, ev->y);

    if (down) {
        s_pressed = target;
        s_press_x = ev->x;
        s_press_y = ev->y;
        update_hover(target);
        if (target != NULL) {
            set_state_chain(target, QL_WIDGET_STATE_ACTIVE, 1);
            emit_pointer(target, QL_WIDGET_EVENT_POINTER_DOWN, ev->x, ev->y, ev->button);
        }
        return;
    }

    QLObject *pressed = s_pressed;
    s_pressed = NULL;
    if (pressed != NULL)
        set_state_chain(pressed, QL_WIDGET_STATE_ACTIVE, 0);

    if (target != NULL)
        emit_pointer(target, QL_WIDGET_EVENT_POINTER_UP, ev->x, ev->y, ev->button);

    /* 点击合成：同一目标 + 位移在阈值内 */
    if (pressed != NULL && pressed == target) {
        int32_t dx = ev->x - s_press_x;
        int32_t dy = ev->y - s_press_y;
        if (dx <= QL_CLICK_SLOP && dx >= -QL_CLICK_SLOP &&
            dy <= QL_CLICK_SLOP && dy >= -QL_CLICK_SLOP)
            emit_pointer(pressed, QL_WIDGET_EVENT_CLICK, ev->x, ev->y, ev->button);
    }
}

static void handle_key(const QLInputEvent *ev, int down)
{
    s_mods = ev->mods;

    QLObject *target = (s_focus != NULL) ? s_focus : ql_input_root();
    if (target == NULL)
        return;

    ql_event_t id = down ? QL_WIDGET_EVENT_KEY_DOWN : QL_WIDGET_EVENT_KEY_UP;
    QL_EMIT_2(target, id, ql_args_uint, ev->key, ql_args_int, (int32_t)ev->mods);
}

static void handle_text(const QLInputEvent *ev)
{
    if (s_focus == NULL || ev->text[0] == '\0')
        return;
    QL_EMIT_1(s_focus, QL_WIDGET_EVENT_TEXT, ql_args_string, ev->text);
}

void ql_input_handle(const QLInputEvent *ev)
{
    if (ev == NULL)
        return;

    switch (ev->type) {
    case QL_INPUT_POINTER_MOVE:
        handle_move(ev);
        break;

    case QL_INPUT_POINTER_DOWN:
        handle_button(ev, 1);
        break;

    case QL_INPUT_POINTER_UP:
        handle_button(ev, 0);
        break;

    case QL_INPUT_POINTER_LEAVE:
        if (s_pressed != NULL) {
            set_state_chain(s_pressed, QL_WIDGET_STATE_ACTIVE, 0);
            s_pressed = NULL;
        }
        update_hover(NULL);
        break;

    case QL_INPUT_WHEEL: {
        s_x = ev->x;
        s_y = ev->y;
        QLObject *target = ql_input_hit_test(NULL, ev->x, ev->y);
        if (target != NULL)
            QL_EMIT_3(target, QL_WIDGET_EVENT_WHEEL,
                      ql_args_int, ev->x, ql_args_int, ev->y, ql_args_int, ev->dy);
        break;
    }

    case QL_INPUT_KEY_DOWN:
        handle_key(ev, 1);
        break;

    case QL_INPUT_KEY_UP:
        handle_key(ev, 0);
        break;

    case QL_INPUT_TEXT:
        handle_text(ev);
        break;

    default:
        break;
    }
}

int ql_input_dispatch(void)
{
    int handled = 0;
    QLInputEvent ev;

    /* 回调里可能继续 push，因此按"取一条派发一条"的方式循环 */
    while (queue_pop(&ev)) {
        if (s_sink != NULL)
            s_sink(&ev, s_sink_user);
        else
            ql_input_handle(&ev);
        handled++;
    }
    return handled;
}
