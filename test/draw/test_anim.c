/**
 * @file    test_anim.c
 * @brief   帧时钟与补间动画：缓动、时间推进、重基、空闲时整帧跳过。
 *
 * 时间源被换成可控假时钟，所以所有断言都是确定的（不依赖真实耗时）。
 */
#include <math.h>
#include <string.h>

#include "../ql_test.h"

static uint32_t s_now;
static uint32_t fake_now(void)
{
    return s_now;
}

/* 记录最后一次 apply 的值与调用次数 */
static QLAnimValue s_last;
static int s_apply_count;
static int s_done_count;

static void on_apply(void *user, const QLAnimValue *v)
{
    QL_UNUSED(user);
    s_last = *v;
    s_apply_count++;
}

/* 推进到 t：时钟与 tick 参数必须一致（真实主循环里就是 ql_anim_now_ms()） */
static uint32_t tick_to(uint32_t t)
{
    s_now = t;
    return ql_anim_tick(t);
}

static void on_done(void *user)
{
    QL_UNUSED(user);
    s_done_count++;
}

/* 颜色过渡：r 通道从 0 到 255 */
static void color_target(QLAnimValue *v, float r, float g, float b, float a)
{
    v->v[0] = r;
    v->v[1] = g;
    v->v[2] = b;
    v->v[3] = a;
}

QL_TEST_BEGIN("test_anim")

    ql_anim_set_time_source(fake_now);

    /* ---------- 1. 线性插值按时间推进，结束回调只触发一次 ---------- */
    {
        QLAnimKey key;
        memset(&key, 0, sizeof(key));
        color_target(&key.cur, 0, 0, 0, 255);

        QLAnimValue to;
        color_target(&to, 100, 0, 0, 255);

        s_now = 1000;
        s_apply_count = 0;
        s_done_count = 0;
        QL_CHECK_EQ(ql_anim_start(&key, &to, 200, QL_EASE_LINEAR, 0, on_apply, on_done, NULL),
                    QL_OK);
        QL_CHECK_EQ(ql_anim_active(), 1);

        /* t = 0 */
        QL_CHECK_EQ(tick_to(1000), 1);
        QL_CHECK(fabsf(s_last.v[0] - 0.0f) < 0.01f);

        /* t = 一半 */
        QL_CHECK_EQ(tick_to(1100), 1);
        QL_CHECK(fabsf(s_last.v[0] - 50.0f) < 0.5f);

        /* t = 结束：落到精确终值 */
        QL_CHECK_EQ(tick_to(1200), 0);
        QL_CHECK(fabsf(s_last.v[0] - 100.0f) < 0.001f);
        QL_CHECK_EQ(s_done_count, 1);
        QL_CHECK_EQ(ql_anim_active(), 0);

        /* 结束后再 tick 不再 apply —— 这就是"没有动画就整帧跳过" */
        int before = s_apply_count;
        QL_CHECK_EQ(tick_to(1300), 0);
        QL_CHECK_EQ(s_apply_count, before);
        QL_CHECK_EQ(s_done_count, 1);
    }

    /* ---------- 2. 重基：中途改目标不跳变 ---------- */
    {
        QLAnimKey key;
        memset(&key, 0, sizeof(key));
        color_target(&key.cur, 0, 0, 0, 255);

        QLAnimValue to;
        color_target(&to, 200, 0, 0, 255);

        s_now = 0;
        ql_anim_start(&key, &to, 200, QL_EASE_LINEAR, 0, on_apply, NULL, NULL);
        QL_CHECK_EQ(tick_to(100), 1);          /* 走到 100 */
        float mid = s_last.v[0];
        QL_CHECK(fabsf(mid - 100.0f) < 0.5f);

        /* 此时改目标为 0，时长仍 200ms：起点必须是 100（当前值），不能跳回 0 或 200 */
        QLAnimValue back;
        color_target(&back, 0, 0, 0, 255);
        QL_CHECK_EQ(ql_anim_start(&key, &back, 200, QL_EASE_LINEAR, 0, on_apply, NULL, NULL),
                    QL_OK);
        QL_CHECK_EQ(tick_to(100), 1);          /* 立刻 tick 一次：值应仍是 100 */
        QL_CHECK(fabsf(s_last.v[0] - mid) < 0.5f);

        QL_CHECK_EQ(tick_to(200), 1);          /* 走了一半 → 50 */
        QL_CHECK(fabsf(s_last.v[0] - 50.0f) < 0.5f);

        QL_CHECK_EQ(tick_to(300), 0);          /* 到 0 */
        QL_CHECK(fabsf(s_last.v[0]) < 0.001f);
        ql_anim_reset();
    }

    /* ---------- 3. 缓动：端点固定、单调不减 ---------- */
    {
        for (int e = 0; e < QL_EASE_COUNT; e++) {
            QL_CHECK(fabsf(ql_ease_apply((uint8_t)e, 4, 0.0f)) < 1e-6f);
            QL_CHECK(fabsf(ql_ease_apply((uint8_t)e, 4, 1.0f) - 1.0f) < 1e-6f);

            float prev = -1.0f;
            int mono = 1;
            for (int i = 0; i <= 20; i++) {
                float t = (float)i / 20.0f;
                float v = ql_ease_apply((uint8_t)e, 4, t);
                if (v < prev - 1e-4f)
                    mono = 0;
                prev = v;
            }
            QL_CHECK_EQ(mono, 1);
        }

        /* ease-in 前段慢、ease-out 前段快 */
        QL_CHECK(ql_ease_apply(QL_EASE_IN, 0, 0.25f) < 0.25f);
        QL_CHECK(ql_ease_apply(QL_EASE_OUT, 0, 0.25f) > 0.25f);

        /* steps(4)：取值只能是 0 / 0.25 / 0.5 / 0.75 */
        QL_CHECK(fabsf(ql_ease_apply(QL_EASE_STEPS, 4, 0.10f) - 0.0f) < 1e-6f);
        QL_CHECK(fabsf(ql_ease_apply(QL_EASE_STEPS, 4, 0.30f) - 0.25f) < 1e-6f);
        QL_CHECK(fabsf(ql_ease_apply(QL_EASE_STEPS, 4, 0.60f) - 0.50f) < 1e-6f);
        QL_CHECK(fabsf(ql_ease_apply(QL_EASE_STEPS, 4, 0.90f) - 0.75f) < 1e-6f);
    }

    /* ---------- 4. 时长 0：立即收敛，仍然走 apply + done ---------- */
    {
        QLAnimKey key;
        memset(&key, 0, sizeof(key));
        color_target(&key.cur, 5, 0, 0, 0);
        QLAnimValue to;
        color_target(&to, 9, 0, 0, 0);

        s_apply_count = 0;
        s_done_count = 0;
        QL_CHECK_EQ(ql_anim_start(&key, &to, 0, QL_EASE_LINEAR, 0, on_apply, on_done, NULL),
                    QL_OK);
        QL_CHECK_EQ(s_apply_count, 1);
        QL_CHECK_EQ(s_done_count, 1);
        QL_CHECK(fabsf(s_last.v[0] - 9.0f) < 1e-6f);
        QL_CHECK_EQ(ql_anim_active(), 0);
    }

    /* ---------- 5. 取消后不再 apply ---------- */
    {
        QLAnimKey key;
        memset(&key, 0, sizeof(key));
        color_target(&key.cur, 0, 0, 0, 0);
        QLAnimValue to;
        color_target(&to, 100, 0, 0, 0);

        s_now = 0;
        ql_anim_start(&key, &to, 100, QL_EASE_LINEAR, 0, on_apply, NULL, NULL);
        QL_CHECK_EQ(tick_to(50), 1);
        ql_anim_cancel(&key);
        QL_CHECK_EQ(ql_anim_active(), 0);
        s_apply_count = 0;
        QL_CHECK_EQ(tick_to(80), 0);
        QL_CHECK_EQ(s_apply_count, 0);
    }

    /* ---------- 6. 槽位满：不排队，直接落终值 ---------- */
    {
        QLAnimKey keys[QL_CFG_ANIM_MAX + 2];
        QLAnimValue to;
        color_target(&to, 100, 100, 100, 255);
        memset(keys, 0, sizeof(keys));

        s_now = 0;
        int ok = 0;
        QLAnimValue over;
        color_target(&over, 1, 2, 3, 4);
        QLAnimKey extra;
        memset(&extra, 0, sizeof(extra));
        for (int i = 0; i < QL_CFG_ANIM_MAX; i++)
            if (ql_anim_start(&keys[i], &to, 1000, QL_EASE_LINEAR, 0, on_apply, NULL, NULL) ==
                QL_OK)
                ok++;
        QL_CHECK_EQ(ok, QL_CFG_ANIM_MAX);
        QL_CHECK_EQ((int)ql_anim_active(), QL_CFG_ANIM_MAX);

        s_apply_count = 0;
        QL_CHECK_EQ(ql_anim_start(&extra, &over, 1000, QL_EASE_LINEAR, 0, on_apply, NULL, NULL),
                    QL_ERR_NOMEM);
        QL_CHECK_EQ(s_apply_count, 1);      /* 立刻落到终值 */
        QL_CHECK(fabsf(s_last.v[0] - 1.0f) < 1e-6f);
        ql_anim_reset();
    }

    /* ---------- 7. 时钟回绕不炸（无符号相减） ---------- */
    {
        ql_anim_reset();
        QLAnimKey key;
        memset(&key, 0, sizeof(key));
        color_target(&key.cur, 0, 0, 0, 0);
        QLAnimValue to;
        color_target(&to, 100, 0, 0, 0);

        s_now = 0xFFFFFFF0u;                 /* 快到回绕点 */
        ql_anim_start(&key, &to, 100, QL_EASE_LINEAR, 0, on_apply, NULL, NULL);
        QL_CHECK_EQ(tick_to(0xFFFFFFF0u + 50u), 1);
        QL_CHECK(fabsf(s_last.v[0] - 50.0f) < 0.5f);
        QL_CHECK_EQ(tick_to(0xFFFFFFF0u + 100u), 0);
        ql_anim_reset();
    }

    ql_anim_set_time_source(NULL);

QL_TEST_END()
