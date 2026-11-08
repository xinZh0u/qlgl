/**
 * @file    qlanim.h
 * @brief   帧时钟 + 补间动画核心：缓动、重基、无动画时整帧跳过。
 *
 * 设计要点（对应 M5.5 计划的三条约束）：
 *
 *   1. **没有活动动画时 `ql_anim_tick()` 返回 0** —— 主循环据此跳过整帧
 *      （不重绘、不提交），这是省电与 MCU 帧预算的前提。
 *   2. **重基（retarget）从当前插值开始**，不是从旧目标开始：
 *      动画进行到一半改目标不会跳变。key 里存着 `cur`，新动画自动以它为起点。
 *   3. **动画只改"绘制期属性"**（颜色、alpha、绘制偏移），不进布局，
 *      所以过渡期间零重排。改 `width` 这类布局属性的动画本模块不负责。
 *
 * 用法：
 *
 *     static QLAnimKey s_key;
 *
 *     // 目标值变了才开始/改道；同一 key 重复调用会重基
 *     s_key.cur.v[0] = 当前值;
 *     ql_anim_start(&s_key, &target, 200, QL_EASE_OUT, 0, on_apply, on_done, user);
 *
 *     // 主循环
 *     if (ql_anim_tick(ql_anim_now_ms()) > 0)
 *         render();
 *
 * 值用 4 个 float 装，够覆盖颜色（r,g,b,a）与标量（只用 v[0]）。
 * 颜色约定为 0..255 的浮点，本模块只做线性插值，不理解语义。
 */
#ifndef QLGL_ANIM_H
#define QLGL_ANIM_H

#include "qlgl.h"

/* 同时活跃的动画上限；满了以后新动画直接落到终值（不排队，避免延迟累积） */
#ifndef QL_CFG_ANIM_MAX
#define QL_CFG_ANIM_MAX 64
#endif

typedef struct {
    float v[4];
} QLAnimValue;

/* 缓动曲线。前 5 个与 CSS 的 `linear/ease-in/ease-out/ease-in-out/ease` 一一对应 */
typedef enum {
    QL_EASE_LINEAR = 0,
    QL_EASE_IN = 1,      /* cubic-bezier(0.42, 0, 1, 1) */
    QL_EASE_OUT = 2,     /* cubic-bezier(0, 0, 0.58, 1) */
    QL_EASE_IN_OUT = 3,  /* cubic-bezier(0.42, 0, 0.58, 1) */
    QL_EASE_CSS = 4,     /* cubic-bezier(0.25, 0.1, 0.25, 1) */
    QL_EASE_STEPS = 5,   /* steps(steps, end)：阶跃，用 QLAnimKey.steps */
    QL_EASE_COUNT
} QLEasing;

/* 值写到目标：user 由调用方给，value 已插值好 */
typedef void (*ql_anim_apply_fn)(void *user, const QLAnimValue *value);
typedef void (*ql_anim_done_fn)(void *user);

/*
 * 一个动画槽的"身份"。**由调用方持有并零初始化**；同一个 key 重复 start 会重基。
 * `cur` 既是"新动画的起点"也是"当前值"，tick 时会更新——所以调用方想在
 * 动画之外改值时，直接写 `key->cur` 即可（下次 start 会从那里接着动）。
 */
typedef struct {
    QLAnimValue cur;
    uint8_t     used;    /* 内部：是否已占用槽位 */
    uint8_t     steps;   /* QL_EASE_STEPS 用：台阶数 */
    uint16_t    reserved;
} QLAnimKey;

/* 单调时钟（毫秒）。默认用 CLOCK_MONOTONIC；MCU 上可用 set_time_source 换掉 */
uint32_t ql_anim_now_ms(void);
typedef uint32_t (*ql_anim_time_fn)(void);
void     ql_anim_set_time_source(ql_anim_time_fn fn);

/*
 * 开始 / 重基一个动画。
 *   to       目标值
 *   dur_ms   时长；0 表示立即落到终值（仍会走一次 apply + done）
 *   ease     QLEasing
 *   steps    QL_EASE_STEPS 的台阶数（其它缓动忽略）
 * 起始值取 `key->cur`。返回 QL_OK / QL_ERR_INVAL / QL_ERR_NOMEM（槽位满）。
 */
ql_err_t ql_anim_start(QLAnimKey *key, const QLAnimValue *to, uint32_t dur_ms, uint8_t ease,
                       uint8_t steps, ql_anim_apply_fn apply, ql_anim_done_fn done,
                       void *user);

/* 取消：不再 apply，也不触发 done。key 可以立刻复用 */
void     ql_anim_cancel(QLAnimKey *key);

/*
 * 推进到 now_ms：对每个活跃动画算插值 → apply；到时间就 apply 终值 + done。
 * 返回**仍在进行中**的动画数：0 表示这一帧没有东西要重绘，主循环可以跳过。
 */
uint32_t ql_anim_tick(uint32_t now_ms);

/* 当前活跃动画数（不推进时间） */
uint32_t ql_anim_active(void);

/* 清空所有动画（qlgl_shutdown 调用） */
void     ql_anim_reset(void);

/* 缓动求值：t ∈ [0,1] → [0,1]。测试/复用用 */
float    ql_ease_apply(uint8_t ease, uint8_t steps, float t);

#endif /* QLGL_ANIM_H */
