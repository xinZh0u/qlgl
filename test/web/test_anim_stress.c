/**
 * @file    test_anim_stress.c
 * @brief   M5.5 验收"10 个元素同时动画，60fps 不掉帧"的 CPU 侧量化。
 *
 * 验收拿窗口看帧率，但帧率不可复现（机器、驱动、vsync 都会变），所以这里量的是
 * **一帧要做的工作量**：`ql_ui_anim_tick` + 把脏区逐个 `ql_ui_render_rect`。
 * 这个数远小于 16.7 ms（60fps 的预算），窗口就一定能跑满帧。
 *
 * 同时也守两条语义：
 *   - 10 段动画各自独立（不是共用同一段帧表常数错乱）；
 *   - 静止之后 `ql_ui_anim_tick` 仍然每帧有活干（infinite），界面不会"停住"。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../ql_test.h"

#include "ui_flex_anim10.h"

#define W 320
#define H 320
#define BUDGET_MS 16.7        /* 60 fps 的一帧预算 */
#define LIMIT_MS  4.0         /* 只做绘制的工作量，留足余量给平台层 */

static uint32_t s_now;

static uint32_t fake_now(void)
{
    return s_now;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

QL_TEST_BEGIN("test_anim_stress")

    ql_anim_set_time_source(fake_now);
    s_now = 0;

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    QL_CHECK(disp != NULL);
    QLCanvas canvas;
    QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
    QLDraw2D d2d;
    QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &canvas), QL_OK);

    QLUIRoot *ui = ql_ui_build(&ui_flex_anim10_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* ---------- 1. 10 段动画各自独立 ---------- */
    QL_CHECK_EQ(ui_flex_anim10_doc.anim_count, 10);
    QL_CHECK(ui->runs != NULL);
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 0), 10);

    {
        /* 走 300ms（每段 1200ms，alternate infinite）后 10 个卡片的颜色应各不相同 */
        (void)ql_ui_anim_tick(ui, 300);
        uint32_t seen[10];
        int n = 0;
        for (uint16_t i = 0; i < ui->node_count && n < 10; i++) {
            const QLUIStyle *st = ql_ui_node_style(&ui->nodes[i]);
            if (st == NULL || st->anim == 0xFFFF)
                continue;
            seen[n++] = st->bg_color;
        }
        QL_CHECK_EQ(n, 10);
        int uniq = 0;
        for (int i = 0; i < n; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (seen[j] == seen[i])
                    dup = 1;
            if (!dup)
                uniq++;
        }
        QL_CHECK_EQ(uniq, 10);
    }

    /* ---------- 2. 一帧的工作量 ---------- */
    (void)ql_ui_consume_dirty(ui, (QLRect[16]){ { 0, 0, 0, 0 } }, 0);

    const int FRAMES = 600;
    long dirty_total = 0;
    double t0 = now_ms();
    for (int f = 0; f < FRAMES; f++) {
        s_now = 1000u + (uint32_t)f * 16u;
        uint32_t moving = ql_ui_anim_tick(ui, s_now);
        QL_CHECK(moving == 10);

        QLRect areas[16];
        int n = ql_ui_consume_dirty(ui, areas, 16);
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(ui, &d2d, areas[i]);
        dirty_total += n;
    }
    double per_frame = (now_ms() - t0) / (double)FRAMES;

    printf("  10 元素同时动画：%.4f ms/帧（平均 %.1f 块脏区，预算 %.1f ms）\n",
           per_frame, (double)dirty_total / (double)FRAMES, BUDGET_MS);
    QL_CHECK(per_frame < LIMIT_MS);

    /* ---------- 3. 纯求值成本单独看 ---------- */
    {
        double a = now_ms();
        for (int f = 0; f < FRAMES * 10; f++) {
            s_now = 20000u + (uint32_t)f * 16u;
            (void)ql_ui_anim_tick(ui, s_now);
        }
        double tick_only = (now_ms() - a) / (double)(FRAMES * 10);
        printf("  仅 ql_ui_anim_tick（10 段）：%.5f ms/帧\n", tick_only);
        QL_CHECK(tick_only < 0.5);
    }

    /* ---------- 4. 脏区确实只覆盖卡片，不是整屏 ---------- */
    {
        ql_disp_mem_clear(disp, 0x000000);
        QLRect areas[16];
        (void)ql_ui_consume_dirty(ui, areas, 16);
        s_now += 16;
        (void)ql_ui_anim_tick(ui, s_now);
        int n = ql_ui_consume_dirty(ui, areas, 16);
        QL_CHECK(n >= 1);
        long area_sum = 0;
        for (int i = 0; i < n; i++)
            area_sum += (long)areas[i].w * (long)areas[i].h;
        printf("  脏区总面积 %ld px²（整屏 %d px²）\n", area_sum, W * H);
        QL_CHECK(area_sum < (long)W * H);
    }

    ql_ui_free(ui);
    ql_disp_mem_destroy(disp);
    ql_anim_set_time_source(NULL);

QL_TEST_END()
