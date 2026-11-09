/**
 * @file    test_keyframes.c
 * @brief   CSS @keyframes：构建期编译成静态帧表，运行期按时间求值。
 *
 * 用可控假时钟，所以断言是确定的。六条契约：
 *   1. **构建期就落到 0% 帧**：第一次绘制不需要等 tick；
 *   2. 缓动只作用于"段内"，`iterations` / `direction` 按 CSS 语义；
 *   3. `fill: forwards` 播完停在末帧，`fill: none` 回落到基础样式；
 *   4. 隐式关键帧（只写 50%）的首末帧取元素自己的基础样式；
 *   5. 动画只标脏，脏区盖住该节点 → 主循环用 ql_ui_consume_dirty 局部重绘；
 *   6. 没有 @keyframes 的 doc 一个字节都不多占（`runs == NULL`，tick 恒 0）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_kf.h"
#include "ui_flex_trans.h"

#define W 120
#define H 260

/* 节点下标：0=#root，之后按文档顺序 */
enum {
    N_ROOT = 0,
    N_PULSE,
    N_REV,
    N_FWD,
    N_BLINK,
    N_ONCE,
    N_SLIDE,
    N_PLAIN,
    N_CLEAR,
    N_RADIAL,
    N_COUNT
};

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

#define END_MAX 16
static int s_end_calls;
static QLUIAnimEndKind s_end_kinds[END_MAX];
static QLUINodeRT *s_end_nodes[END_MAX];

static void on_anim_end(QLUIRoot *ui, QLUINodeRT *node, QLUIAnimEndKind kind,
                        void *user)
{
    (void)ui;
    (void)user;
    if (s_end_calls < END_MAX) {
        s_end_kinds[s_end_calls] = kind;
        s_end_nodes[s_end_calls] = node;
    }
    s_end_calls++;
}

static const QLUIStyle *style_of(QLUIRoot *ui, int index)
{
    return ql_ui_node_style(&ui->nodes[index]);
}

static int red_of(QLUIRoot *ui, int index)
{
    uint32_t c = style_of(ui, index)->bg_color;
    return (int)((c >> 16) & 0xFFu);
}

static int alpha_of(QLUIRoot *ui, int index)
{
    return (int)style_of(ui, index)->bg_alpha;
}

QL_TEST_BEGIN("test_keyframes")

    ql_anim_set_time_source(fake_now);
    s_now = 5000;

    QLUIRoot *ui = ql_ui_build(&ui_flex_kf_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* ---------- 1. 构建期生成的静态表 ---------- */
    QL_CHECK_EQ(ui->node_count, N_COUNT);
    QL_CHECK(ui->runs != NULL);
    QL_CHECK_EQ(ui_flex_kf_doc.anim_count, 8);
    QL_CHECK_EQ(ui_flex_kf_doc.keyframe_count, 11);

    /* 段表必须自洽：首帧 0、末帧 1000、offset 升序 */
    for (uint16_t a = 0; a < ui_flex_kf_doc.anim_count; a++) {
        const QLUIAnimation *def = &ui_flex_kf_doc.anims[a];
        const QLUIKeyframe *kf = &ui_flex_kf_doc.keyframes[def->first_frame];
        QL_CHECK(def->frame_count >= 2);
        QL_CHECK_EQ(kf[0].offset, 0);
        QL_CHECK_EQ(kf[def->frame_count - 1].offset, 1000);
        for (uint16_t k = 1; k < def->frame_count; k++)
            QL_CHECK(kf[k].offset > kf[k - 1].offset);
    }

    /* ---------- 2. 构建期就落到 0% 帧（还没 tick） ---------- */
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0x00);          /* pulse 0% = #000000 */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 0);             /* fade 0% = opacity 0 */
    QL_CHECK_EQ(style_of(ui, N_PLAIN)->bg_color, 0x0000FFu);

    /* ---------- 3. 第一次 tick 拿到起始时刻 ---------- */
    QL_CHECK_EQ(tick_at(ui, 5000), 8);               /* 8 段都在跑 */
    QL_CHECK_EQ(ql_ui_anim_running(ui), 8);
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0x00);

    /* 底色透明 + 只动 background-color：alpha 跟着颜色一起动 */
    QL_CHECK_EQ(ui->nodes[N_CLEAR].style->bg_alpha, 0);
    QL_CHECK_EQ(alpha_of(ui, N_CLEAR), 255);

    /* ---------- 4. 线性推进 + alternate / reverse ---------- */
    (void)tick_at(ui, 5250);                         /* t = 250 */
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0x40);          /* pulse u=0.25 → 63.75 → 64 */
    QL_CHECK_EQ(red_of(ui, N_REV), 0xBF);            /* reverse u=0.75 → 191 */
    QL_CHECK_EQ(red_of(ui, N_ONCE), 159);            /* once 250/400 = 0.625 → 159 */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 159);           /* fade 250/400 = 0.625 → 159 */
    QL_CHECK_EQ(alpha_of(ui, N_BLINK), 128);         /* 0%→50% 的中点 */

    (void)tick_at(ui, 5500);                         /* t = 500 */
    QL_CHECK_EQ(alpha_of(ui, N_BLINK), 0);           /* 正落在 50% 帧 */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 64);            /* 第二轮：100/400 = 0.25 */

    /* ---------- 5. 第二轮：alternate 反向 ---------- */
    (void)tick_at(ui, 5750);                         /* t = 750 */
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0xBF);          /* alternate 第二轮 u=0.75 */
    QL_CHECK_EQ(red_of(ui, N_REV), 0x40);            /* reverse 永远是 1-u */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 223);           /* 350/400 = 0.875 → 223 */

    /* ---------- 6. fill: forwards 停在末帧，fill: none 回落 ---------- */
    (void)tick_at(ui, 5800);                         /* fwd 跑满 2×400 */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 255);           /* 停在 100% 帧 */
    QL_CHECK_EQ(red_of(ui, N_FWD), 0x00);            /* 其它通道仍来自基础样式 */
    QL_CHECK_EQ(style_of(ui, N_ONCE)->bg_color, 0x00FF00u);   /* once 已回落 */

    (void)tick_at(ui, 6000);                         /* blink: u = 1.0 */
    QL_CHECK_EQ(alpha_of(ui, N_BLINK), 255);         /* 隐式 100% = 基础 alpha */
    QL_CHECK_EQ(ql_ui_anim_running(ui), 3);          /* pulse / rev / blink 是 infinite */
    QL_CHECK_EQ(alpha_of(ui, N_FWD), 255);           /* 停住的不会再变 */

    /* ---------- 7. 无限循环：永远返回非 0，静态判断不会误跳过 ---------- */
    for (uint32_t t = 7000; t < 9000; t += 250)
        QL_CHECK(tick_at(ui, t) >= 3);

    /* ---------- 8. plain 节点完全不受影响 ---------- */
    QL_CHECK_EQ(style_of(ui, N_PLAIN)->bg_color, 0x0000FFu);
    QL_CHECK_EQ(style_of(ui, N_PLAIN)->bg_alpha, 255);
    QL_CHECK(ql_ui_node_style(&ui->nodes[N_PLAIN]) == ui->nodes[N_PLAIN].style);

    /* ---------- 9. 重播：从头开始 ---------- */
    QL_CHECK_EQ(ql_ui_anim_restart(ui, &ui->nodes[N_PULSE]), QL_OK);
    QL_CHECK(tick_at(ui, 9000) >= 3);                /* pulse 重新加入在跑的队列 */
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0x00);
    (void)tick_at(ui, 9250);
    QL_CHECK_EQ(red_of(ui, N_PULSE), 0x40);

    /* 没有动画的节点：明确失败，不是静默什么都不做 */
    QL_CHECK_EQ(ql_ui_anim_restart(ui, &ui->nodes[N_PLAIN]), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_ui_anim_restart(ui, NULL), QL_ERR_INVAL);

    /* ---------- 9.5 transform：绘制期平移，命中测试要逆变换 ---------- */
    {
        ql_ui_free(ui);
        ui = ql_ui_build(&ui_flex_kf_doc);
        QL_CHECK(ui != NULL);
        ql_ui_layout(ui, W, H);
        (void)tick_at(ui, 5000);            /* 起始时刻 = 5000 */

        /* 静态 transform：布局不动，样式里的 tf 是 5/3，命中按平移后的位置 */
        QLRect pb = ql_ui_node_rect(&ui->nodes[N_PLAIN]);
        QL_CHECK_EQ(style_of(ui, N_PLAIN)->tf_x, 5);
        QL_CHECK_EQ(style_of(ui, N_PLAIN)->tf_y, 3);
        QL_CHECK(ql_ui_hit_test(ui, pb.x + pb.w / 2 + 5, pb.y + pb.h / 2 + 3) ==
                 &ui->nodes[N_PLAIN]);

        /* 动画：slide 第一帧 translate(-40px)，画在左边 40px 处 */
        QL_CHECK_EQ(style_of(ui, N_SLIDE)->tf_x, -40);
        QLRect sb = ql_ui_node_rect(&ui->nodes[N_SLIDE]);
        QL_CHECK(!ql_rect_is_empty(sb));

        /* paint_box 必须覆盖"旧位置"——否则滑入的第一帧会留残影 */
        QL_CHECK(ui->nodes[N_SLIDE].paint_box.x <= sb.x - 40);
        QL_CHECK(ql_rect_right(ui->nodes[N_SLIDE].paint_box) >= ql_rect_right(sb));

        /* 点画在左边的位置能命中，点原位不命中 */
        QL_CHECK(ql_ui_hit_test(ui, sb.x - 40 + sb.w / 2, sb.y + sb.h / 2) ==
                 &ui->nodes[N_SLIDE]);
        QL_CHECK(ql_ui_hit_test(ui, sb.x + sb.w - 2, sb.y + sb.h / 2) !=
                 &ui->nodes[N_SLIDE]);

        /* 走一半：translate 线性插值到 -20 */
        (void)tick_at(ui, 5200);
        QL_CHECK_EQ(style_of(ui, N_SLIDE)->tf_x, -20);
        QL_CHECK(ql_ui_hit_test(ui, sb.x - 20 + sb.w / 2, sb.y + sb.h / 2) ==
                 &ui->nodes[N_SLIDE]);

        /* 播完（fill: forwards）停在 0：回到原位，命中跟着回来 */
        (void)tick_at(ui, 5600);
        QL_CHECK_EQ(style_of(ui, N_SLIDE)->tf_x, 0);
        QL_CHECK(ql_ui_hit_test(ui, sb.x + sb.w / 2, sb.y + sb.h / 2) ==
                 &ui->nodes[N_SLIDE]);

        /* ---------- 像素级：平移真的画到别处去了 ---------- */
        {
            QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
            QLCanvas canvas;
            QLDraw2D d2d;
            QL_CHECK(disp != NULL);
            QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
            QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &canvas), QL_OK);

            /* 回到第一帧：slide 画在左边 40px */
            QL_CHECK_EQ(ql_ui_anim_restart(ui, &ui->nodes[N_SLIDE]), QL_OK);
            (void)tick_at(ui, 5000);
            QL_CHECK_EQ(style_of(ui, N_SLIDE)->tf_x, -40);

            ql_disp_mem_clear(disp, 0x000000);
            ql_ui_render(ui, &d2d);

            ql_coord_t cy = sb.y + sb.h / 2;
            /* 平移后的位置：背景色（#000000 不透明，而不是根节点的 #101010） */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, sb.x - 40 + 2, cy), 0x000000u);
            /* 原位：只剩根节点背景 */
            QL_CHECK_EQ(ql_disp_mem_pixel(disp, sb.x + sb.w - 2, cy), 0x101010u);

            ql_disp_mem_destroy(disp);
        }

        ql_ui_free(ui);
        ui = NULL;
    }

    /* ---------- 10. 脏区：覆盖被动画的节点 ---------- */
    ui = ql_ui_build(&ui_flex_kf_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL)
        return 1;
    ql_ui_layout(ui, W, H);
    (void)tick_at(ui, 5000);
    {
        QLRect areas[4];
        (void)ql_ui_consume_dirty(ui, areas, 4);     /* 清空 */

        QL_CHECK(tick_at(ui, 5500) > 0);
        int n = ql_ui_consume_dirty(ui, areas, 4);
        QL_CHECK(n > 0);

        QLRect box = ql_ui_node_rect(&ui->nodes[N_PULSE]);
        int covered = 0;
        for (int i = 0; i < n; i++) {
            if (areas[i].x <= box.x && areas[i].y <= box.y &&
                areas[i].x + areas[i].w >= box.x + box.w &&
                areas[i].y + areas[i].h >= box.y + box.h)
                covered = 1;
        }
        QL_CHECK(covered);
        /* 取过之后就干净了：不 tick 就不会再有脏区 */
        QL_CHECK_EQ(ql_ui_consume_dirty(ui, areas, 4), 0);
    }

    /* ---------- 9.8 圆角揭示 + 径向渐变 ---------- */
    {
        ql_ui_free(ui);
        ui = ql_ui_build(&ui_flex_kf_doc);
        QL_CHECK(ui != NULL);
        if (ui == NULL)
            return 1;
        ql_ui_layout(ui, W, H);

        /* 构造期就落在 0% 帧：border-radius 0 */
        QL_CHECK_EQ(style_of(ui, N_RADIAL)->bg_grad_dir, 3);   /* 3 = 径向 */
        QL_CHECK_EQ(style_of(ui, N_RADIAL)->radius[0], 0);

        (void)tick_at(ui, 5000);
        QL_CHECK_EQ(style_of(ui, N_RADIAL)->radius[0], 0);
        (void)tick_at(ui, 5300);                              /* 一半 */
        QL_CHECK_EQ(style_of(ui, N_RADIAL)->radius[0], 15);
        (void)tick_at(ui, 5600);                              /* 播完，fill: forwards */
        QL_CHECK_EQ(style_of(ui, N_RADIAL)->radius[0], 30);

        /* 像素：圆心是起始色（亮），四角是终止色（暗），且四角被圆角切掉 */
        QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
        QLCanvas canvas;
        QLDraw2D d2d;
        QL_CHECK(disp != NULL);
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &canvas), QL_OK);

        ql_disp_mem_clear(disp, 0x000000);
        ql_ui_render(ui, &d2d);

        QLRect rb = ql_ui_node_rect(&ui->nodes[N_RADIAL]);
        QL_CHECK(!ql_rect_is_empty(rb));
        int cx = rb.x + rb.w / 2;
        int cy = rb.y + rb.h / 2;

        ql_color_t mid = ql_disp_mem_pixel(disp, cx, cy);
        ql_color_t edge = ql_disp_mem_pixel(disp, cx, rb.y + rb.h - 2);
        /* 中心更亮（起色 #FFE08A 的 R=0xFF），靠近边缘更暗 */
        QL_CHECK((int)QL_COLOR_GET_R(mid) > (int)QL_COLOR_GET_R(edge) + 20);
        QL_CHECK((int)QL_COLOR_GET_R(mid) > 200);
        /* 圆角切掉了左上角：那里必须是底色 */
        /* 圆角切掉的部分露出**父节点**的背景（根节点 #101010），不是画布底色 */
        QL_CHECK_EQ(ql_disp_mem_pixel(disp, rb.x, rb.y), 0x101010u);
        /* 圆心处不是底色 */
        QL_CHECK(mid != 0x000000u);

        ql_disp_mem_destroy(disp);
    }

    /* ---------- 10.5 animationend：播完触发一次，重启不触发 ---------- */
    {
        ql_ui_free(ui);
        ui = ql_ui_build(&ui_flex_kf_doc);
        QL_CHECK(ui != NULL);
        if (ui == NULL)
            return 1;
        ql_ui_layout(ui, W, H);

        s_end_calls = 0;
        ql_ui_set_anim_end_cb(ui, on_anim_end, NULL);

        (void)tick_at(ui, 20000);          /* 起始时刻 */
        QL_CHECK_EQ(s_end_calls, 0);

        (void)tick_at(ui, 20400);          /* fwd(400ms×2) 还在第二轮；once 播完 */
        QL_CHECK(s_end_calls >= 1);
        int saw_kf = 0, saw_once = 0;
        for (int i = 0; i < s_end_calls && i < 8; i++) {
            if (s_end_kinds[i] == QL_UI_ANIM_END_KEYFRAMES)
                saw_kf = 1;
        }
        QL_CHECK(saw_kf);

        /* once 的节点确实收到过结束事件 */
        for (int i = 0; i < s_end_calls && i < 8; i++)
            if (s_end_nodes[i] == &ui->nodes[N_ONCE] &&
                s_end_kinds[i] == QL_UI_ANIM_END_KEYFRAMES)
                saw_once = 1;
        QL_CHECK(saw_once);

        /* 重播一个已播完的动画：不会立刻又推一条"结束" */
        int before = s_end_calls;
        QL_CHECK_EQ(ql_ui_anim_restart(ui, &ui->nodes[N_ONCE]), QL_OK);
        (void)tick_at(ui, 20500);
        QL_CHECK_EQ(s_end_calls, before);   /* 500ms 时才一半，还没结束 */
        (void)tick_at(ui, 20900);           /* 400ms 跑满（fwd 也在这期间走完） */
        QL_CHECK(s_end_calls > before);
        /* once 这次重播的结束事件确实来了 */
        {
            int got = 0;
            for (int i = before; i < s_end_calls && i < END_MAX; i++)
                if (s_end_nodes[i] == &ui->nodes[N_ONCE]) got = 1;
            QL_CHECK(got);
        }

        /* infinite 动画永远不推结束事件 */
        {
            int n = s_end_calls;
            for (uint32_t t = 21000; t < 23000; t += 100)
                (void)tick_at(ui, t);
            for (int i = n; i < s_end_calls; i++)
                QL_CHECK(s_end_nodes[i] != &ui->nodes[N_PULSE]);   /* pulse 是 infinite */
        }

        ql_ui_set_anim_end_cb(ui, NULL, NULL);
    }

    ql_ui_free(ui);

    /* ---------- 11. blob 往返：关键帧表跟着走，行为一致 ---------- */
    {
        size_t need = ql_ui_doc_encode(&ui_flex_kf_doc, NULL, 0);
        QL_CHECK(need > 0);
        uint8_t *buf = (uint8_t *)malloc(need);
        QL_CHECK(buf != NULL);
        if (buf != NULL) {
            QL_CHECK_EQ(ql_ui_doc_encode(&ui_flex_kf_doc, buf, need), need);

            QLUIDoc *copy = ql_ui_doc_load_mem(buf, need);
            QL_CHECK(copy != NULL);
            if (copy != NULL) {
                QL_CHECK_EQ(copy->anim_count, ui_flex_kf_doc.anim_count);
                QL_CHECK_EQ(copy->keyframe_count, ui_flex_kf_doc.keyframe_count);
                QL_CHECK(copy->anims != NULL && copy->keyframes != NULL);
                if (copy->anims != NULL && copy->keyframes != NULL) {
                    QL_CHECK_EQ(memcmp(copy->anims, ui_flex_kf_doc.anims,
                                       (size_t)copy->anim_count * sizeof(QLUIAnimation)), 0);
                    QL_CHECK_EQ(memcmp(copy->keyframes, ui_flex_kf_doc.keyframes,
                                       (size_t)copy->keyframe_count *
                                           sizeof(QLUIKeyframe)), 0);
                }

                QLUIRoot *from_blob = ql_ui_build(copy);
                QL_CHECK(from_blob != NULL);
                if (from_blob != NULL) {
                    ql_ui_layout(from_blob, W, H);
                    QL_CHECK_EQ(red_of(from_blob, N_PULSE), 0x00);
                    (void)tick_at(from_blob, 9000);
                    (void)tick_at(from_blob, 9250);
                    QL_CHECK_EQ(red_of(from_blob, N_PULSE), 0x40);
                    ql_ui_free(from_blob);
                }
                ql_ui_doc_free(copy);
            }
            free(buf);
        }
    }

    /* ---------- 12. 没有 @keyframes 的 doc：零成本 ---------- */
    {
        QLUIRoot *plain = ql_ui_build(&ui_flex_trans_doc);
        QL_CHECK(plain != NULL);
        if (plain != NULL) {
            QL_CHECK(plain->runs == NULL);
            ql_ui_layout(plain, 120, 60);
            s_now = 10000;
            QL_CHECK_EQ(ql_ui_anim_tick(plain, 10000), 0);
            QL_CHECK_EQ(ql_ui_anim_running(plain), 0);
            ql_ui_free(plain);
        }
    }

    ql_anim_set_time_source(NULL);

QL_TEST_END()
