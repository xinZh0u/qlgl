/**
 * @file    test_transition.c
 * @brief   CSS transition：状态位切换时颜色在时长内插值过去。
 *
 * 用可控假时钟，所以断言是确定的。三条契约：
 *   1. **过渡期间 `ql_ui_node_style()` 返回插值结果**（渲染层不需要知道有过渡）；
 *   2. **打断不跳变**：过渡到一半改目标，从当前插值接着走；
 *   3. **过渡只把节点标脏**，主循环用 `ql_ui_consume_dirty` 拿局部区域重绘；
 *      静态界面下 `ql_ui_anim_tick` 恒为 0（可以整帧跳过）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_trans.h"

#define W 120
#define H 60

static uint32_t s_now;

static uint32_t fake_now(void)
{
    return s_now;
}

/* 推进到 t：时钟与 tick 参数必须一致（真实主循环里就是 ql_anim_now_ms()） */
static uint32_t tick_at(QLUIRoot *ui, uint32_t t)
{
    s_now = t;
    return ql_ui_anim_tick(ui, t);
}

static QLUIRoot *layout(QLUIRoot *ui)
{
    ql_ui_layout(ui, W, H);
    return ui;
}

/* 节点当前"生效"背景色（会走过渡插值） */
static uint32_t bg_of(QLUIRoot *ui, uint16_t index)
{
    const QLUIStyle *s = ql_ui_node_style(&ui->nodes[index]);
    return (s != NULL) ? s->bg_color : 0;
}

/* 取 r 通道，方便看插值进度 */
static int red_of(uint32_t c)
{
    return (int)((c >> 16) & 0xFFu);
}

QL_TEST_BEGIN("test_transition")

    ql_anim_set_time_source(fake_now);
    s_now = 1000;

    QLUIRoot *ui = layout(ql_ui_build(&ui_flex_trans_doc));
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    /* .btn 是带过渡的那个节点（node 1） */
    QL_CHECK_EQ(ui->node_count, 2);
    QL_CHECK(ui->trans != NULL);          /* 有 transition → 过渡池已分配 */

    const uint32_t base_bg = bg_of(ui, 1);
    QL_CHECK_EQ(base_bg, 0x204060u);
    QL_CHECK_EQ(ql_anim_active(), 0);     /* 还没交互：没有动画 */

    /* ---------- 1. 移入：颜色在 200ms 内插值 ---------- */
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 20, 20), 1);      /* 落在 .btn 上 */
    QL_CHECK_EQ(ql_anim_active(), 1);

    QL_CHECK_EQ(tick_at(ui, 1000), 1);           /* t = 0 */
    QL_CHECK_EQ(red_of(bg_of(ui, 1)), 0x20);             /* 仍是起点色 */

    QL_CHECK_EQ(tick_at(ui, 1100), 1);           /* t = 一半 */
    {
        /* 线性：(0x20 + 0xff) / 2 ≈ 0x8F */
        int mid = red_of(bg_of(ui, 1));
        QL_CHECK(mid > 0x80 && mid < 0xA0);
    }

    QL_CHECK_EQ(tick_at(ui, 1200), 0);           /* 结束 */
    QL_CHECK_EQ(red_of(bg_of(ui, 1)), 0xFF);             /* 精确落到目标色 */
    QL_CHECK_EQ(bg_of(ui, 1), 0xFF8000u);

    /* 结束后不再有动画 —— 静态界面可以整帧跳过 */
    QL_CHECK_EQ(ql_anim_active(), 0);
    QL_CHECK_EQ(tick_at(ui, 1300), 0);

    /* ---------- 2. 移出：反向插值，且打断不跳变 ---------- */
    s_now = 1200;                                        /* 交互发生在这一刻 */
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 2, 2), 1);        /* 移到背景上 */
    QL_CHECK_EQ(ql_anim_active(), 1);

    QL_CHECK_EQ(tick_at(ui, 1200), 1);                   /* t = 0：从 0xFF 开始 */
    QL_CHECK_EQ(red_of(bg_of(ui, 1)), 0xFF);

    QL_CHECK_EQ(tick_at(ui, 1300), 1);                   /* t = 一半 */
    int mid_back = red_of(bg_of(ui, 1));
    QL_CHECK(mid_back > 0x80 && mid_back < 0xA0);

    /* 中途又移回去：起点必须是当前插值值（≈0x8F），不能跳回 0x20 或 0xFF */
    s_now = 1300;
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 20, 20), 1);
    QL_CHECK_EQ(tick_at(ui, 1300), 1);
    QL_CHECK(red_of(bg_of(ui, 1)) == mid_back);

    QL_CHECK_EQ(tick_at(ui, 1400), 1);                   /* 再走一半 → 向 0xFF 靠 */
    QL_CHECK(red_of(bg_of(ui, 1)) > mid_back);

    QL_CHECK_EQ(tick_at(ui, 1500), 0);
    QL_CHECK_EQ(bg_of(ui, 1), 0xFF8000u);

    /* ---------- 3. 过渡只标脏，脏区覆盖该节点 ---------- */
    {
        QLRect areas[4];
        /* 先清掉历史脏标记 */
        (void)ql_ui_consume_dirty(ui, areas, 4);

        s_now = 1600;
        ql_ui_pointer_move(ui, 2, 2);                    /* 再次移出 */
        QL_CHECK_EQ(tick_at(ui, 1600), 1);
        QL_CHECK_EQ(tick_at(ui, 1700), 1);

        int n = ql_ui_consume_dirty(ui, areas, 4);
        QL_CHECK(n >= 1);
        if (n >= 1) {
            QLRect b = ui->nodes[1].border_box;
            /* 脏区必须把节点的绘制包围盒包住（含阴影；这里没阴影，含边框盒即可） */
            QL_CHECK(areas[0].x <= b.x && areas[0].y <= b.y);
            QL_CHECK(ql_rect_right(areas[0]) >= ql_rect_right(b));
            QL_CHECK(ql_rect_bottom(areas[0]) >= ql_rect_bottom(b));
        }

        /* 取过之后就清了：同一帧不重复重绘 */
        QL_CHECK_EQ(ql_ui_consume_dirty(ui, areas, 4), 0);

        /* 收尾 */
        QL_CHECK_EQ(tick_at(ui, 1800), 0);
        QL_CHECK_EQ(bg_of(ui, 1), 0x204060u);
    }

    /* ---------- 4. 销毁时不能把过期动画留在全局池里 ---------- */
    {
        s_now = 2000;
        ql_ui_pointer_move(ui, 20, 20);
        QL_CHECK_EQ(ql_anim_active(), 1);
        ql_ui_free(ui);                       /* 动画还在跑就销毁 */
        QL_CHECK_EQ(ql_anim_active(), 0);     /* 必须被摘干净 */
    }

    ql_anim_set_time_source(NULL);

QL_TEST_END()
