/**
 * @file    test_input.c
 * @brief   输入系统：命中测试、hover/pressed/focus 状态、点击合成、冒泡。
 *
 * 全部走"构造事件 → 入队 → dispatch"的真实路径，不直接调用内部派发函数。
 */
#include "../ql_test.h"

static int s_clicks;
static int s_enters;
static int s_leaves;
static int s_bubbled_clicks;
static int s_key_events;
static int s_text_events;
static char s_last_text[16];

static void on_click(QLEvent *ev)
{
    QL_UNUSED(ev);
    s_clicks++;
}

static void on_enter(QLEvent *ev)
{
    QL_UNUSED(ev);
    s_enters++;
}

static void on_leave(QLEvent *ev)
{
    QL_UNUSED(ev);
    s_leaves++;
}

static void on_parent_click(QLEvent *ev)
{
    /* 冒泡到父控件时 target 仍是子控件 */
    if (ev->target != ev->current_target)
        s_bubbled_clicks++;
}

static void on_key(QLEvent *ev)
{
    s_key_events++;
    QL_CHECK_EQ(ql_event_arg_int(ev, 0, -1), (int32_t)QL_KEY_ENTER);
    QL_CHECK_EQ(ql_event_arg_int(ev, 1, -1), QL_MOD_SHIFT);
}

static void on_text(QLEvent *ev)
{
    s_text_events++;
    const char *t = ql_event_arg_string(ev, 0, "");
    strncpy(s_last_text, t, sizeof(s_last_text) - 1);
}

QL_TEST_BEGIN("test_input")

    QLWidget *root = ql_widget_new_ex(0, 0, 320, 240);
    QLWidget *a = ql_widget_new_child((QLObject *)root, 10, 10, 100, 50);
    QLWidget *b = ql_widget_new_child((QLObject *)root, 50, 20, 100, 50);  /* 与 a 重叠 */
    QLWidget *far = ql_widget_new_child((QLObject *)root, 200, 200, 20, 20);
    QL_CHECK(root != NULL && a != NULL && b != NULL && far != NULL);
    ql_input_set_root((QLObject *)root);

    /* 1. 命中测试：后挂的兄弟在上层 */
    QL_CHECK(ql_input_hit_test(NULL, 60, 30) == (QLObject *)b);
    QL_CHECK(ql_input_hit_test(NULL, 15, 15) == (QLObject *)a);    /* 只有 a 覆盖 */
    QL_CHECK(ql_input_hit_test(NULL, 205, 205) == (QLObject *)far);
    QL_CHECK(ql_input_hit_test(NULL, 190, 5) == (QLObject *)root); /* 空白处命中最外层 */
    QL_CHECK(ql_input_hit_test(NULL, 400, 400) == NULL);

    /* 2. pointer-events: none 时穿透到下层 */
    ql_widget_set_pointer_events((QLObject *)b, 0);
    QL_CHECK(ql_input_hit_test(NULL, 60, 30) == (QLObject *)a);
    QL_CHECK_EQ(ql_widget_pointer_events((QLObject *)b), 0);
    ql_widget_set_pointer_events((QLObject *)b, 1);
    QL_CHECK(ql_input_hit_test(NULL, 60, 30) == (QLObject *)b);

    /* 3. 隐藏控件不参与命中 */
    ql_widget_set_visible((QLObject *)b, 0);
    QL_CHECK(ql_input_hit_test(NULL, 60, 30) == (QLObject *)a);
    ql_widget_set_visible((QLObject *)b, 1);
    QL_CHECK(ql_input_hit_test(NULL, 60, 30) == (QLObject *)b);

    /* 4. 父控件裁剪子控件：越界部分不命中 */
    QLWidget *clip_box = ql_widget_new_child((QLObject *)root, 0, 100, 100, 40);
    QLWidget *big = ql_widget_new_child((QLObject *)clip_box, 0, 0, 200, 40);
    ql_object_set_property((QLObject *)clip_box, QL_WIDGET_PROPERTY_CLIP_CHILDREN,
                           ql_value_bool(1));
    QL_CHECK(ql_input_hit_test(NULL, 50, 120) == (QLObject *)big);
    QL_CHECK(ql_input_hit_test(NULL, 150, 120) == (QLObject *)root);

    /* 5. hover：ENTER/LEAVE + 状态位沿父链设置 */
    ql_connect((QLObject *)a, QL_WIDGET_EVENT_POINTER_ENTER, on_enter, NULL);
    ql_connect((QLObject *)a, QL_WIDGET_EVENT_POINTER_LEAVE, on_leave, NULL);

    QL_CHECK_EQ(ql_input_push_move(15, 15, 0), QL_OK);
    QL_CHECK_EQ(ql_input_pending(), 1);
    QL_CHECK_EQ(ql_input_dispatch(), 1);
    QL_CHECK(ql_input_hover() == (QLObject *)a);
    QL_CHECK_EQ(s_enters, 1);
    QL_CHECK(ql_widget_has_state((QLObject *)a, QL_WIDGET_STATE_HOVER));
    QL_CHECK(ql_widget_has_state((QLObject *)root, QL_WIDGET_STATE_HOVER));

    QL_CHECK_EQ(ql_input_push_move(16, 16, 0), QL_OK);   /* 同一目标：不重复 ENTER */
    ql_input_dispatch();
    QL_CHECK_EQ(s_enters, 1);

    QL_CHECK_EQ(ql_input_push_move(205, 205, 0), QL_OK);
    ql_input_dispatch();
    QL_CHECK_EQ(s_leaves, 1);
    QL_CHECK(ql_input_hover() == (QLObject *)far);
    QL_CHECK(!ql_widget_has_state((QLObject *)a, QL_WIDGET_STATE_HOVER));
    /* far 也在 root 下，root 仍然是 hover */
    QL_CHECK(ql_widget_has_state((QLObject *)root, QL_WIDGET_STATE_HOVER));

    /* 6. 离开表面：清空 hover */
    QL_CHECK_EQ(ql_input_push_leave(), QL_OK);
    ql_input_dispatch();
    QL_CHECK(ql_input_hover() == NULL);
    QL_CHECK(!ql_widget_has_state((QLObject *)root, QL_WIDGET_STATE_HOVER));

    /* 7. 点击合成：同一目标 + 位移在阈值内 */
    ql_connect((QLObject *)b, QL_WIDGET_EVENT_CLICK, on_click, NULL);
    ql_connect((QLObject *)root, QL_WIDGET_EVENT_CLICK, on_parent_click, NULL);

    ql_input_push_move(60, 30, 0);
    ql_input_push_button(60, 30, QL_BUTTON_LEFT, 1);
    ql_input_push_button(62, 31, QL_BUTTON_LEFT, 0);
    ql_input_dispatch();
    QL_CHECK_EQ(s_clicks, 1);
    QL_CHECK_EQ(s_bubbled_clicks, 1);          /* 冒泡到 root，target 仍是 b */
    QL_CHECK_EQ(ql_input_pressed(), NULL);
    QL_CHECK_EQ(ql_input_buttons(), 0);
    QL_CHECK(!ql_widget_has_state((QLObject *)b, QL_WIDGET_STATE_ACTIVE));

    /* 8. 按下后拖到别处松手：不算点击 */
    ql_input_push_button(60, 30, QL_BUTTON_LEFT, 1);
    ql_input_push_move(205, 205, QL_BUTTON_BIT_LEFT);
    ql_input_push_button(205, 205, QL_BUTTON_LEFT, 0);
    ql_input_dispatch();
    QL_CHECK_EQ(s_clicks, 1);

    /* 9. 按住期间保持 ACTIVE 与 pressed 目标，松手清除 */
    ql_input_push_button(60, 30, QL_BUTTON_LEFT, 1);
    ql_input_dispatch();
    QL_CHECK(ql_input_pressed() == (QLObject *)b);
    QL_CHECK(ql_widget_has_state((QLObject *)b, QL_WIDGET_STATE_ACTIVE));
    QL_CHECK_EQ(ql_input_buttons(), QL_BUTTON_BIT_LEFT);
    ql_input_push_button(60, 30, QL_BUTTON_LEFT, 0);
    ql_input_dispatch();
    QL_CHECK_EQ(ql_input_pressed(), NULL);

    /* 10. 状态背景色只影响绘制，不改变状态位 */
    ql_widget_set_state_bg((QLObject *)a, QL_WIDGET_STATE_HOVER, QL_COLOR_RED, 200);

    /* 11. 焦点：状态位 + 键盘事件走焦点目标并冒泡 */
    ql_widget_set_focusable((QLObject *)a, 1);
    QL_CHECK_EQ(ql_widget_is_focusable((QLObject *)a), 1);
    QL_CHECK_EQ(ql_input_set_focus((QLObject *)a), QL_OK);
    QL_CHECK(ql_input_focus() == (QLObject *)a);
    QL_CHECK(ql_widget_has_state((QLObject *)a, QL_WIDGET_STATE_FOCUS));
    QL_CHECK(ql_widget_has_state((QLObject *)root, QL_WIDGET_STATE_FOCUS));

    ql_connect((QLObject *)a, QL_WIDGET_EVENT_KEY_DOWN, on_key, NULL);
    QL_CHECK_EQ(ql_input_push_key(QL_KEY_ENTER, 1, QL_MOD_SHIFT), QL_OK);
    ql_input_dispatch();
    QL_CHECK_EQ(s_key_events, 1);
    QL_CHECK_EQ(ql_input_mods(), QL_MOD_SHIFT);

    /* 12. 文本输入送到焦点控件 */
    ql_connect((QLObject *)a, QL_WIDGET_EVENT_TEXT, on_text, NULL);
    QL_CHECK_EQ(ql_input_push_text("A"), QL_OK);
    ql_input_dispatch();
    QL_CHECK_EQ(s_text_events, 1);
    QL_CHECK_STR(s_last_text, "A");

    /* 13. 清除焦点 */
    QL_CHECK_EQ(ql_input_set_focus(NULL), QL_OK);
    QL_CHECK(ql_input_focus() == NULL);
    QL_CHECK(!ql_widget_has_state((QLObject *)a, QL_WIDGET_STATE_FOCUS));

    /* 14. 队列溢出丢最旧的，不阻塞输入 */
    for (int i = 0; i < QL_CFG_INPUT_QUEUE; i++)
        QL_CHECK_EQ(ql_input_push_move((ql_coord_t)i, 0, 0), QL_OK);
    QL_CHECK_EQ(ql_input_pending(), QL_CFG_INPUT_QUEUE);
    QL_CHECK_EQ(ql_input_push_move(999, 0, 0), QL_ERR_FULL);
    QL_CHECK_EQ(ql_input_pending(), QL_CFG_INPUT_QUEUE);
    ql_input_dispatch();
    QL_CHECK_EQ(ql_input_pending(), 0);
    QL_CHECK_EQ(ql_input_x(), 999);       /* 最后一条生效 */

    /* 15. reset 清空一切 */
    ql_input_push_move(1, 1, 0);
    ql_input_set_focus((QLObject *)a);
    ql_input_reset();
    QL_CHECK_EQ(ql_input_pending(), 0);
    QL_CHECK(ql_input_hover() == NULL);
    QL_CHECK(ql_input_focus() == NULL);
    QL_CHECK(ql_input_root() == NULL);

    ql_object_unref((QLObject *)root);

QL_TEST_END()
