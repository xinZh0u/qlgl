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

static int s_end_calls;
static QLUIAnimEndKind s_end_kind;
static QLUINodeRT *s_end_node;

static void on_anim_end(QLUIRoot *ui, QLUINodeRT *node, QLUIAnimEndKind kind,
                        void *user)
{
    (void)ui;
    (void)user;
    s_end_calls++;
    s_end_kind = kind;
    s_end_node = node;
}

static void ql_ui_anim_end_cb_reset(void)
{
    s_end_calls = 0;
    s_end_node = NULL;
}

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

static uint32_t text_of(QLUIRoot *ui, uint16_t index)
{
    return ql_ui_node_style(&ui->nodes[index])->text_color;
}

static uint32_t border_of(QLUIRoot *ui, uint16_t index)
{
    return ql_ui_node_style(&ui->nodes[index])->border_color4[0];
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
    QL_CHECK_EQ(ui->node_count, 5);
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

    /* ---------- 3.5 文本色 / 边框色：与背景同一条过渡一起插值 ---------- */
    {
        s_now = 2400;
        /* .btn 的 transition: 200ms linear，props = all（背景+alpha+文本+边框） */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[1])->bg_alpha, 255);

        QL_CHECK_EQ(ql_ui_pointer_move(ui, 20, 20), 1);
        QL_CHECK_EQ(tick_at(ui, 2400), 1);
        /* 起点：三者都还是 base 色 */
        QL_CHECK_EQ(text_of(ui, 1), 0x102030u);
        QL_CHECK_EQ(border_of(ui, 1), 0x303030u);

        QL_CHECK_EQ(tick_at(ui, 2500), 1);
        /* 圆角也在这条过渡里（transition: all 含 border-radius）：4 → 12，一半 = 8 */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[1])->radius[0], 8);
        /* 一半：0x10 → 0xff 大约 0x87；0x30 → 0xaa 大约 0x6d */
        int tr = (int)((text_of(ui, 1) >> 16) & 0xFFu);
        int br = (int)((border_of(ui, 1) >> 16) & 0xFFu);
        QL_CHECK(tr > 0x70 && tr < 0x98);
        QL_CHECK(br > 0x58 && br < 0x80);

        QL_CHECK_EQ(tick_at(ui, 2600), 0);
        QL_CHECK_EQ(text_of(ui, 1), 0xffeeddu);
        QL_CHECK_EQ(border_of(ui, 1), 0xaabbccu);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[1])->radius[0], 12);

        /* .lnk 只过渡 color：400ms，中途背景色不该被碰 */
        s_now = 2700;
        QLRect lr = ql_ui_node_rect(&ui->nodes[2]);
        QL_CHECK_EQ(ql_ui_pointer_move(ui, lr.x + 3, lr.y + 3), 1);
        /* 同时离开 .btn（200ms 回去）+ 进入 .lnk（400ms）→ 两段过渡 */
        QL_CHECK_EQ(tick_at(ui, 2700), 2);
        QL_CHECK_EQ(text_of(ui, 2), 0x112233u);
        QL_CHECK_EQ(tick_at(ui, 2900), 1);        /* .btn 那段已经结束 */
        int lr16 = (int)((text_of(ui, 2) >> 16) & 0xFFu);
        QL_CHECK(lr16 > 0x70 && lr16 < 0xA0);     /* 一半：0x11 → 0xff ≈ 0x88 */
        QL_CHECK_EQ(tick_at(ui, 3100), 0);
        QL_CHECK_EQ(text_of(ui, 2), 0xFFFFFFu);

        /* 收尾：移开，等过渡跑完 */
        s_now = 3200;
        ql_ui_pointer_move(ui, 2, 2);
        (void)tick_at(ui, 4000);
    }

    /* ---------- 3.55 transform / clip-path 也能过渡 ---------- */
    {
        QLRect areas[4];
        const QLUIStyle *z = ql_ui_node_style(&ui->nodes[3]);
        QL_CHECK_EQ(z->sc_x, 1000);
        QL_CHECK_EQ(z->sc_y, 1000);
        QL_CHECK_EQ(z->tf_x, 0);
        /* transition-property: transform → 位 0x20（不含背景/圆角） */
        QL_CHECK_EQ(ui->nodes[3].style->trans_props, 32);

        /* paint_box 必须按"放大后的范围"撑开——它只看静态样式的话脏区会漏画 */
        {
            QLRect pb = ui->nodes[3].paint_box;
            QLRect b = ui->nodes[3].border_box;
            QL_CHECK(pb.x <= b.x + b.w / 2 - (b.w * 1500 / 1000) / 2);
            QL_CHECK(ql_rect_right(pb) >= b.x + b.w / 2 + (b.w * 1500 / 1000) / 2);
        }

        s_now = 3400;
        QL_CHECK_EQ(ql_ui_pointer_move(ui, 92, 25), 1);   /* 落在 .zoom 上 */
        QL_CHECK_EQ(tick_at(ui, 3400), 1);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 1000);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->tf_x, 0);

        QL_CHECK_EQ(tick_at(ui, 3500), 1);                /* 一半 */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 1250);   /* 1000 → 1500 */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->tf_x, 2);      /* 0 → 4 */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->tf_y, 1);      /* 0 → 2 */

        QL_CHECK_EQ(tick_at(ui, 3600), 0);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 1500);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->tf_x, 4);

        /* 放大后视觉范围变大，脏区必须跟着变大（重绘漏一点就留残影） */
        (void)ql_ui_consume_dirty(ui, areas, 4);
        s_now = 3700;
        QL_CHECK_EQ(ql_ui_pointer_move(ui, 2, 2), 1);      /* 移出 → 缩回去 */
        QL_CHECK_EQ(tick_at(ui, 3700), 1);
        QL_CHECK_EQ(tick_at(ui, 3800), 1);
        int na = ql_ui_consume_dirty(ui, areas, 4);
        QL_CHECK(na >= 1);
        if (na >= 1) {
            QLRect b = ui->nodes[3].border_box;
            ql_coord_t vis_w = (ql_coord_t)((int64_t)b.w * 1500 / 1000);
            ql_coord_t vx = b.x + b.w / 2 - vis_w / 2;
            QL_CHECK(areas[0].x <= vx);
            QL_CHECK(ql_rect_right(areas[0]) >= vx + vis_w);
        }
        QL_CHECK_EQ(tick_at(ui, 3900), 0);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[3])->sc_x, 1000);

        /* clip-path：半径在 20% → 100% 之间插值 */
        const QLUIStyle *m = ql_ui_node_style(&ui->nodes[4]);
        QL_CHECK_EQ(m->clip_kind, 1);
        QL_CHECK_EQ(m->clip_r, 200);

        s_now = 4000;
        QL_CHECK_EQ(ql_ui_pointer_move(ui, 92, 45), 1);    /* 落在 .mask 上 */
        QL_CHECK_EQ(tick_at(ui, 4000), 1);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->clip_r, 200);

        QL_CHECK_EQ(tick_at(ui, 4100), 1);                 /* 一半 → 600 */
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->clip_r, 600);
        QL_CHECK_EQ(tick_at(ui, 4200), 0);
        QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[4])->clip_r, 1000);

        s_now = 4300;
        ql_ui_pointer_move(ui, 2, 2);
        (void)tick_at(ui, 4500);
    }

    /* ---------- 3.6 animationend：过渡走到终点时回调一次 ---------- */
    {
        s_now = 5000;
        ql_ui_anim_end_cb_reset();
        ql_ui_set_anim_end_cb(ui, on_anim_end, NULL);

        QL_CHECK_EQ(ql_ui_pointer_move(ui, 20, 20), 1);   /* 起过渡 */
        QL_CHECK_EQ(tick_at(ui, 5000), 1);
        QL_CHECK_EQ(s_end_calls, 0);                      /* 还没结束 */
        QL_CHECK_EQ(tick_at(ui, 5200), 0);                /* 200ms 走完 */
        QL_CHECK_EQ(s_end_calls, 1);
        QL_CHECK_EQ(s_end_kind, QL_UI_ANIM_END_TRANSITION);
        QL_CHECK(s_end_node == &ui->nodes[1]);

        /* 再走一帧不会重复触发 */
        QL_CHECK_EQ(tick_at(ui, 5300), 0);
        QL_CHECK_EQ(s_end_calls, 1);

        /* 状态被撤掉（目标样式没有过渡）时不触发 */
        ql_ui_anim_end_cb_reset();
        ql_ui_pointer_move(ui, 2, 2);
        QL_CHECK_EQ(tick_at(ui, 5400), 1);
        ql_ui_pointer_move(ui, 20, 20);                   /* 半路又移回 */
        QL_CHECK_EQ(tick_at(ui, 5500), 1);
        QL_CHECK_EQ(s_end_calls, 0);                      /* 被打断的不是"结束" */
        (void)tick_at(ui, 5800);
        QL_CHECK_EQ(s_end_calls, 1);                      /* 新的这一段走完才触发 */

        ql_ui_set_anim_end_cb(ui, NULL, NULL);
    }

    /* ---------- 4. 销毁时不能把过期动画留在全局池里 ---------- */
    {
        s_now = 6000;
        ql_ui_pointer_move(ui, 2, 2);            /* 先移开 */
        (void)tick_at(ui, 7000);                 /* 让回去的过渡跑完 */
        QL_CHECK_EQ(ql_anim_active(), 0);

        ql_ui_pointer_move(ui, 20, 20);
        QL_CHECK_EQ(ql_anim_active(), 1);
        ql_ui_free(ui);                       /* 动画还在跑就销毁 */
        QL_CHECK_EQ(ql_anim_active(), 0);     /* 必须被摘干净 */
    }

    ql_anim_set_time_source(NULL);

QL_TEST_END()
