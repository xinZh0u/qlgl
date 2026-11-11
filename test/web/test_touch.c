/**
 * @file    test_touch.c
 * @brief   触摸手势：单指点按 / 单指拖动滚动 / 双指捏合。
 *
 * 后端只上报 `QL_INPUT_TOUCH`（哪根手指、几点、还有几根），"算点按还是算拖动、
 * 双指是不是捏合"全在界面层认。所以这里完全不碰窗口/合成器，喂合成事件即可 ——
 * 这也是唯一能在 CI 上验证触摸逻辑的办法（CI 没有触摸屏）。
 *
 * 验的四件事：
 *   1. 单指按下抬起 → 合成一次 click；
 *   2. 单指在可滚动容器里移动超过 slop → 拖内容滚动，且**不**合成 click；
 *   3. 第二根手指落下 → 那次按下作废（没有 click），捏合回调收到
 *      BEGIN / UPDATE / END，倍数是"相对手势开始"的；
 *   4. 捏合结束后剩下的手指不当鼠标（不会在落点误触发点击）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 200

static const char *k_html =
    "<div id=root>"
    "<div class=pad id=pad>tap</div>"
    "<div class=sc id=sc>"
    "<div class=row>1</div><div class=row>2</div><div class=row>3</div>"
    "<div class=row>4</div><div class=row>5</div><div class=row>6</div>"
    "</div></div>";

static const char *k_css =
    "#root { width: 200px; height: 200px; }"
    ".pad { height: 40px; background-color: #334455; }"
    ".sc { height: 60px; overflow-y: auto; }"
    ".row { height: 40px; background-color: #EEEEEE; }";

/* ---------- 捏合回调记录 ---------- */
static int        s_pinch_n;
static int        s_pinch_phase[8];
static ql_coord_t s_pinch_scale[8];
static ql_coord_t s_pinch_cx, s_pinch_cy;

static void on_pinch(QLUIRoot *ui, ql_coord_t cx, ql_coord_t cy, ql_coord_t scale_fx,
                     int phase, void *user)
{
    (void)ui; (void)user;
    s_pinch_cx = cx;
    s_pinch_cy = cy;
    if (s_pinch_n < 8) {
        s_pinch_phase[s_pinch_n] = phase;
        s_pinch_scale[s_pinch_n] = scale_fx;
        s_pinch_n++;
    }
}

static int s_clicks;
static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui; (void)node; (void)user;
    s_clicks++;
}

/* 一次触摸事件走完整链路：入队 → 派发 → 界面层 */
static void touch(ql_coord_t x, ql_coord_t y, uint8_t phase, uint8_t id, uint8_t count)
{
    (void)ql_input_push_touch(x, y, phase, id, count);
    (void)ql_input_dispatch();
}

static QLUIRoot *mk(QLUIDoc **out_doc, QLUINodeRT **out_sc)
{
    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, W, H);
    /* 节点顺序：0=root, 1=pad, 2=sc, 3..8=row */
    *out_sc = &ui->nodes[2];
    *out_doc = doc;
    return ui;
}

QL_TEST_BEGIN("test_touch")

    ql_input_reset();
    s_clicks = 0;
    s_pinch_n = 0;

    /* ---------- 1. 单指点按 = 一次 click ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUINodeRT *sc = NULL;
        QLUIRoot *ui = mk(&doc, &sc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_set_click_cb(ui, on_click, NULL);
            ql_ui_attach_input(ui);

            touch(100, 20, QL_TOUCH_DOWN, 0, 1);
            touch(100, 20, QL_TOUCH_UP, 0, 0);
            QL_CHECK_EQ(s_clicks, 1);
            QL_CHECK_EQ((int)ui->touch_count, 0);   /* 手抬完了，状态归零 */

            /* 抖 3px 仍算点按（小于 slop 8px），第二次点击要算上 */
            touch(100, 20, QL_TOUCH_DOWN, 1, 1);
            touch(101, 22, QL_TOUCH_MOVE, 1, 1);
            touch(100, 21, QL_TOUCH_UP, 1, 0);
            QL_CHECK_EQ(s_clicks, 2);

            /* 超过 slop 的移动不算点击 */
            touch(100, 20, QL_TOUCH_DOWN, 2, 1);
            touch(100, 40, QL_TOUCH_MOVE, 2, 1);
            touch(100, 40, QL_TOUCH_UP, 2, 0);
            QL_CHECK_EQ(s_clicks, 2);

            ql_ui_detach_input();
            ql_ui_set_click_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 单指拖列表 → 滚动，且不算点击 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUINodeRT *sc = NULL;
        QLUIRoot *ui = mk(&doc, &sc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            s_clicks = 0;
            ql_ui_set_click_cb(ui, on_click, NULL);
            ql_ui_attach_input(ui);
            QL_CHECK(ql_ui_scrollable(sc));
            const ql_coord_t max_y = sc->scroll_max_y;
            QL_CHECK(max_y > 0);

            /* 列表占 y=40..100（pad 40 高、列表 60 高）：在列表里按下，往上推 50px */
            touch(100, 90, QL_TOUCH_DOWN, 0, 1);
            for (int i = 1; i <= 5; i++)
                touch(100, (ql_coord_t)(90 - i * 10), QL_TOUCH_MOVE, 0, 1);
            QL_CHECK(sc->scroll_y > 0);
            QL_CHECK_EQ((int)sc->scroll_y, 50);     /* 手指走多少，内容走多少 */
            touch(100, 40, QL_TOUCH_UP, 0, 0);
            QL_CHECK_EQ(s_clicks, 0);               /* 拖动不算点击 */

            /* 拖到超过上限：夹在 scroll_max_y，不会越界 */
            touch(100, 95, QL_TOUCH_DOWN, 1, 1);
            for (int i = 1; i <= 10; i++)
                touch(100, (ql_coord_t)(95 - i * 20), QL_TOUCH_MOVE, 1, 1);
            QL_CHECK_EQ((int)sc->scroll_y, (int)max_y);
            touch(100, -105, QL_TOUCH_UP, 1, 0);

            ql_ui_detach_input();
            ql_ui_set_click_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 双指捏合：作废按下 + 回调倍数 + 结束后不误点 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUINodeRT *sc = NULL;
        QLUIRoot *ui = mk(&doc, &sc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            s_clicks = 0;
            s_pinch_n = 0;
            ql_ui_set_click_cb(ui, on_click, NULL);
            ql_ui_set_pinch_cb(ui, on_pinch, NULL);
            ql_ui_attach_input(ui);

            /* 第一根手指按在 pad 上，第二根落下 → pad 那次按下作废 */
            touch(60, 20, QL_TOUCH_DOWN, 0, 1);
            touch(140, 20, QL_TOUCH_DOWN, 1, 2);
            QL_CHECK_EQ(s_pinch_n, 1);
            QL_CHECK_EQ(s_pinch_phase[0], QL_UI_PINCH_BEGIN);
            QL_CHECK_EQ((int)s_pinch_scale[0], (int)QL_SCALE_ONE);
            QL_CHECK_EQ((int)s_pinch_cx, 100);      /* 两指中心 */
            QL_CHECK_EQ((int)s_pinch_cy, 20);

            /* 两指拉开到 160px：倍数 160/80 = 2.0 */
            touch(40, 20, QL_TOUCH_MOVE, 0, 2);
            touch(200, 20, QL_TOUCH_MOVE, 1, 2);
            QL_CHECK(s_pinch_n >= 2);
            QL_CHECK_EQ(s_pinch_phase[s_pinch_n - 1], QL_UI_PINCH_UPDATE);
            QL_CHECK_EQ((int)s_pinch_scale[s_pinch_n - 1], (int)(QL_SCALE_ONE * 2));

            /* 抬起一根：捏合结束 */
            touch(40, 20, QL_TOUCH_UP, 0, 1);
            QL_CHECK_EQ(s_pinch_phase[s_pinch_n - 1], QL_UI_PINCH_END);
            QL_CHECK_EQ((int)ui->pinch_active, 0);

            /*
             * 剩下的那根手指抬起：**不能**变成点击（否则缩放完一松手就误点）。
             */
            touch(200, 20, QL_TOUCH_UP, 1, 0);
            QL_CHECK_EQ(s_clicks, 0);
            QL_CHECK_EQ((int)ui->touch_count, 0);
            QL_CHECK_EQ((int)ui->touch_ignore, 0);  /* 全抬完状态复位 */

            ql_ui_detach_input();
            ql_ui_set_pinch_cb(ui, NULL, NULL);
            ql_ui_set_click_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 合成器取消（CANCEL）：当抬起处理，不算点击 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUINodeRT *sc = NULL;
        QLUIRoot *ui = mk(&doc, &sc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            s_clicks = 0;
            ql_ui_set_click_cb(ui, on_click, NULL);
            ql_ui_attach_input(ui);

            touch(100, 20, QL_TOUCH_DOWN, 0, 1);
            touch(100, 20, QL_TOUCH_CANCEL, 0, 0);
            QL_CHECK_EQ(s_clicks, 0);
            QL_CHECK_EQ((int)ui->touch_count, 0);

            /* 取消之后状态干净：下一次正常点按仍然算点击 */
            touch(100, 20, QL_TOUCH_DOWN, 1, 1);
            touch(100, 20, QL_TOUCH_UP, 1, 0);
            QL_CHECK_EQ(s_clicks, 1);

            ql_ui_detach_input();
            ql_ui_set_click_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
