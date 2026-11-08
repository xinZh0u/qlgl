/**
 * @file    qlanim.c
 * @brief   帧时钟与补间动画核心。
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

/* ------------------------------ 帧时钟 ------------------------------ */

static uint32_t mono_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static ql_anim_time_fn s_time_fn = mono_now_ms;

uint32_t ql_anim_now_ms(void)
{
    return (s_time_fn != NULL) ? s_time_fn() : 0u;
}

void ql_anim_set_time_source(ql_anim_time_fn fn)
{
    s_time_fn = (fn != NULL) ? fn : mono_now_ms;
}

/* ------------------------------ 缓动 ------------------------------ */

/* 三次贝塞尔：P0=(0,0) P1=(x1,y1) P2=(x2,y2) P3=(1,1)，与 CSS cubic-bezier 同义 */
static float bezier_axis(float t, float a1, float a2)
{
    const float u = 1.0f - t;
    return 3.0f * u * u * t * a1 + 3.0f * u * t * t * a2 + t * t * t;
}

static float bezier_solve(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;

    /*
     * 先用牛顿迭代，不收敛再退回二分。
     * 次数固定，保证同一输入永远得到同一输出（可测试、可复现）。
     */
    float t = x;
    for (int i = 0; i < 8; i++) {
        float err = bezier_axis(t, x1, x2) - x;
        if (fabsf(err) < 1e-5f)
            break;
        float d = 3.0f * (1.0f - t) * (1.0f - t) * x1 +
                  6.0f * (1.0f - t) * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
        if (fabsf(d) < 1e-6f)
            break;
        t -= err / d;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
    }
    return bezier_axis(t, y1, y2);
}

float ql_ease_apply(uint8_t ease, uint8_t steps, float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;

    switch (ease) {
    case QL_EASE_IN:
        return bezier_solve(0.42f, 0.0f, 1.0f, 1.0f, t);
    case QL_EASE_OUT:
        return bezier_solve(0.0f, 0.0f, 0.58f, 1.0f, t);
    case QL_EASE_IN_OUT:
        return bezier_solve(0.42f, 0.0f, 0.58f, 1.0f, t);
    case QL_EASE_CSS:
        return bezier_solve(0.25f, 0.1f, 0.25f, 1.0f, t);
    case QL_EASE_STEPS: {
        /* steps(n, end)：先不动，跳到位 */
        uint8_t n = (steps > 0) ? steps : 1;
        float v = floorf(t * (float)n) / (float)n;
        return (v > 1.0f) ? 1.0f : v;
    }
    case QL_EASE_LINEAR:
    default:
        return t;
    }
}

/* ------------------------------ 动画槽 ------------------------------ */

typedef struct {
    QLAnimKey       *key;    /* 身份；NULL = 空槽（tick 时要回写 cur，不能是 const） */
    QLAnimValue      from;
    QLAnimValue      to;
    uint32_t         t0;
    uint32_t         dur;
    uint8_t          ease;
    uint8_t          steps;
    uint8_t          started;   /* 0 = 还没 apply 过起始值 */
    ql_anim_apply_fn apply;
    ql_anim_done_fn  done;
    void            *user;
} QLAnimSlot;

static QLAnimSlot s_slots[QL_CFG_ANIM_MAX];

static QLAnimSlot *find_slot(QLAnimKey *key)
{
    for (int i = 0; i < QL_CFG_ANIM_MAX; i++)
        if (s_slots[i].key == key)
            return &s_slots[i];
    return NULL;
}

static QLAnimSlot *alloc_slot(void)
{
    for (int i = 0; i < QL_CFG_ANIM_MAX; i++)
        if (s_slots[i].key == NULL)
            return &s_slots[i];
    return NULL;
}

void ql_anim_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
}

uint32_t ql_anim_active(void)
{
    uint32_t n = 0;
    for (int i = 0; i < QL_CFG_ANIM_MAX; i++)
        if (s_slots[i].key != NULL)
            n++;
    return n;
}

ql_err_t ql_anim_start(QLAnimKey *key, const QLAnimValue *to, uint32_t dur_ms, uint8_t ease,
                       uint8_t steps, ql_anim_apply_fn apply, ql_anim_done_fn done,
                       void *user)
{
    if (key == NULL || to == NULL || apply == NULL)
        return QL_ERR_INVAL;

    const uint32_t now = ql_anim_now_ms();

    /* 时长 0：直接落到终值，仍然走一次 apply + done（调用方逻辑统一） */
    if (dur_ms == 0) {
        QLAnimSlot *old = find_slot(key);
        if (old != NULL)
            old->key = NULL;

        key->cur = *to;
        apply(user, &key->cur);
        if (done != NULL)
            done(user);
        key->used = 0;
        return QL_OK;
    }

    QLAnimSlot *slot = find_slot(key);
    if (slot == NULL) {
        slot = alloc_slot();
        if (slot == NULL) {
            /* 槽位满：不排队（排队会让延迟无限累积），直接落终值 */
            key->cur = *to;
            apply(user, &key->cur);
            if (done != NULL)
                done(user);
            QL_LOGW("anim: slots full (%d), converging immediately", QL_CFG_ANIM_MAX);
            return QL_ERR_NOMEM;
        }
        memset(slot, 0, sizeof(*slot));
        slot->key = key;
        key->used = 1;
    }

    /*
     * **重基**：起点永远是 key->cur（当前插值结果），不是旧动画的 from。
     * 所以动画中途改目标不会有跳变——这是 transition 打断时的正确语义。
     */
    slot->from = key->cur;
    slot->to = *to;
    slot->t0 = now;
    slot->dur = dur_ms;
    slot->ease = ease;
    slot->steps = steps;
    slot->apply = apply;
    slot->done = done;
    slot->user = user;
    slot->started = 0;
    return QL_OK;
}

void ql_anim_cancel(QLAnimKey *key)
{
    if (key == NULL)
        return;
    QLAnimSlot *slot = find_slot(key);
    if (slot != NULL) {
        slot->key = NULL;
        memset(slot, 0, sizeof(*slot));
    }
    key->used = 0;
}

uint32_t ql_anim_tick(uint32_t now_ms)
{
    uint32_t active = 0;

    for (int i = 0; i < QL_CFG_ANIM_MAX; i++) {
        QLAnimSlot *s = &s_slots[i];
        if (s->key == NULL)
            continue;

        const uint32_t elapsed = now_ms - s->t0;   /* 无符号减法，时钟回绕也成立 */
        float t = (s->dur > 0) ? (float)elapsed / (float)s->dur : 1.0f;
        if (t > 1.0f)
            t = 1.0f;

        const float e = ql_ease_apply(s->ease, s->steps, t);
        QLAnimValue v;
        for (int k = 0; k < 4; k++)
            v.v[k] = s->from.v[k] + (s->to.v[k] - s->from.v[k]) * e;

        s->key->cur = v;
        s->apply(s->user, &v);
        s->started = 1;

        if (t >= 1.0f) {
            /* 收尾：确保落在精确终值上（避免浮点累计误差留一点点尾巴） */
            s->key->cur = s->to;
            s->apply(s->user, &s->to);
            if (s->done != NULL)
                s->done(s->user);
            s->key->used = 0;
            memset(s, 0, sizeof(*s));
        } else {
            active++;
        }
    }
    return active;
}
