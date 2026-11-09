/**
 * @file    test_input_queue.c
 * @brief   输入队列：入队/派发/溢出丢弃/状态跟踪/事件汇（sink）。
 *
 * 这一层**不做命中测试**（那是界面层的事，见 `test_input_edit` / `test_select`），
 * 它只负责"把后端产生的事件按顺序交出去"，所以这里断言的都是队列语义：
 *   - 排空顺序 = 入队顺序；
 *   - 队列满时**丢最旧的**（宁丢历史也不卡住输入）；
 *   - 坐标 / 按住的按钮 / 修饰键在**入队时**就更新（读到的不会是过期快照）；
 *   - 没装汇时事件被丢弃，但队列照常排空。
 */
#include "../ql_test.h"

/* 记下收到的事件，检查顺序与字段 */
typedef struct {
    int          count;
    QLInputType  type[8];
    int32_t      x[8], y[8];
    uint8_t      buttons[8];
} Recorder;

static Recorder s_rec;

static void record_sink(const QLInputEvent *ev, void *user)
{
    Recorder *r = (Recorder *)user;
    if (r == NULL || ev == NULL)
        return;
    if (r->count < (int)(sizeof(r->type) / sizeof(r->type[0]))) {
        r->type[r->count] = (QLInputType)ev->type;
        r->x[r->count] = ev->x;
        r->y[r->count] = ev->y;
        r->buttons[r->count] = ev->buttons;
    }
    r->count++;
}

/*
 * 汇里继续 push 一个事件：验证 dispatch 是"取一条派发一条"，
 * 新入队的事件在同一轮里会被派发，且不会无限循环（只补一次）。
 */
static int s_chain_done;

static void chain_sink(const QLInputEvent *ev, void *user)
{
    record_sink(ev, user);
    if (!s_chain_done) {
        s_chain_done = 1;
        ql_input_push_move(2, 2, 0);
    }
}

QL_TEST_BEGIN("test_input_queue")

    ql_input_reset();

    /* ---------- 1. 入队 → 派发：顺序不变，汇收到全部 ---------- */
    {
        memset(&s_rec, 0, sizeof(s_rec));
        ql_input_set_sink(record_sink, &s_rec);

        QL_CHECK_EQ(ql_input_pending(), 0u);
        QL_CHECK_EQ(ql_input_push_move(10, 20, 0), QL_OK);
        QL_CHECK_EQ(ql_input_push_button(10, 20, QL_BUTTON_BIT_LEFT, 1), QL_OK);
        QL_CHECK_EQ(ql_input_push_text("A"), QL_OK);
        QL_CHECK_EQ(ql_input_pending(), 3u);

        QL_CHECK_EQ(ql_input_dispatch(), 3);
        QL_CHECK_EQ(ql_input_pending(), 0u);
        QL_CHECK_EQ(s_rec.count, 3);
        QL_CHECK_EQ((int)s_rec.type[0], (int)QL_INPUT_POINTER_MOVE);
        QL_CHECK_EQ((int)s_rec.type[1], (int)QL_INPUT_POINTER_DOWN);
        QL_CHECK_EQ((int)s_rec.type[2], (int)QL_INPUT_TEXT);
        QL_CHECK_EQ((int)s_rec.x[0], 10);
        QL_CHECK_EQ((int)s_rec.y[0], 20);

        /* 再派发一次：队列已空，什么都不该发生 */
        QL_CHECK_EQ(ql_input_dispatch(), 0);
        QL_CHECK_EQ(s_rec.count, 3);
    }

    /* ---------- 2. 状态跟踪在**入队时**发生 ---------- */
    {
        ql_input_reset();
        memset(&s_rec, 0, sizeof(s_rec));
        ql_input_set_sink(record_sink, &s_rec);

        QL_CHECK_EQ(ql_input_push_move(33, 44, 0), QL_OK);
        /* 还没 dispatch，但坐标已经更新了 */
        QL_CHECK_EQ((int)ql_input_x(), 33);
        QL_CHECK_EQ((int)ql_input_y(), 44);

        QL_CHECK_EQ(ql_input_push_button(33, 44, QL_BUTTON_BIT_LEFT, 1), QL_OK);
        QL_CHECK_EQ((int)ql_input_buttons(), (int)QL_BUTTON_BIT_LEFT);
        QL_CHECK_EQ(ql_input_push_button(33, 44, QL_BUTTON_BIT_LEFT, 0), QL_OK);
        QL_CHECK_EQ((int)ql_input_buttons(), 0);

        /* 两个按钮分别按下/抬起，位图要能叠加 */
        QL_CHECK_EQ(ql_input_push_button(1, 1, QL_BUTTON_BIT_LEFT, 1), QL_OK);
        QL_CHECK_EQ(ql_input_push_button(1, 1, QL_BUTTON_BIT_RIGHT, 1), QL_OK);
        QL_CHECK_EQ((int)ql_input_buttons(),
                    (int)(QL_BUTTON_BIT_LEFT | QL_BUTTON_BIT_RIGHT));
        QL_CHECK_EQ(ql_input_push_button(1, 1, QL_BUTTON_BIT_LEFT, 0), QL_OK);
        QL_CHECK_EQ((int)ql_input_buttons(), (int)QL_BUTTON_BIT_RIGHT);

        QL_CHECK_EQ(ql_input_push_key(QL_KEY_ENTER, 1, 0x02), QL_OK);
        QL_CHECK_EQ((int)ql_input_mods(), 0x02);

        ql_input_dispatch();
    }

    /* ---------- 3. 队列满：丢最旧的，返回 FULL ---------- */
    {
        ql_input_reset();
        memset(&s_rec, 0, sizeof(s_rec));
        ql_input_set_sink(record_sink, &s_rec);

        int full_seen = 0;
        for (uint32_t i = 0; i < (uint32_t)QL_CFG_INPUT_QUEUE + 3u; i++) {
            const ql_err_t rc = ql_input_push_move((ql_coord_t)i, 0, 0);
            if (rc == QL_ERR_FULL)
                full_seen = 1;
        }
        QL_CHECK(full_seen);
        QL_CHECK_EQ(ql_input_pending(), (uint32_t)QL_CFG_INPUT_QUEUE);

        QL_CHECK_EQ(ql_input_dispatch(), (int)QL_CFG_INPUT_QUEUE);
        /* 保留下来的是**最后** QL_CFG_INPUT_QUEUE 条：第 3 条起 */
        QL_CHECK_EQ(s_rec.count, (int)QL_CFG_INPUT_QUEUE);
        if (s_rec.count >= (int)QL_CFG_INPUT_QUEUE)
            QL_CHECK_EQ((int)s_rec.x[0], 3);
    }

    /* ---------- 4. 没装汇：队列照样排空，事件被丢弃 ---------- */
    {
        ql_input_reset();
        ql_input_set_sink(NULL, NULL);
        QL_CHECK_EQ(ql_input_push_move(5, 5, 0), QL_OK);
        QL_CHECK_EQ(ql_input_push_key(QL_KEY_ESCAPE, 1, 0), QL_OK);
        QL_CHECK_EQ(ql_input_dispatch(), 2);   /* 计数照样返回 */
        QL_CHECK_EQ(ql_input_pending(), 0u);
    }

    /* ---------- 5. 汇里继续 push：同一轮内派发，不会死循环 ---------- */
    {
        ql_input_reset();
        memset(&s_rec, 0, sizeof(s_rec));
        s_chain_done = 0;
        ql_input_set_sink(chain_sink, &s_rec);
        QL_CHECK_EQ(ql_input_push_move(1, 1, 0), QL_OK);
        const int n = ql_input_dispatch();
        QL_CHECK_EQ(n, 2);            /* 原来那条 + 汇里补的一条 */
        QL_CHECK_EQ(s_rec.count, 2);
        QL_CHECK_EQ((int)s_rec.x[1], 2);
        ql_input_set_sink(NULL, NULL);
    }

    /* ---------- 6. reset 清空队列与状态 ---------- */
    {
        ql_input_reset();
        QL_CHECK_EQ(ql_input_push_button(7, 8, QL_BUTTON_BIT_LEFT, 1), QL_OK);
        ql_input_push_key(QL_KEY_TAB, 1, 0x01);
        ql_input_reset();
        QL_CHECK_EQ(ql_input_pending(), 0u);
        QL_CHECK_EQ((int)ql_input_buttons(), 0);
        QL_CHECK_EQ((int)ql_input_mods(), 0);
        QL_CHECK_EQ((int)ql_input_x(), 0);
        QL_CHECK_EQ((int)ql_input_y(), 0);
    }

QL_TEST_END()
