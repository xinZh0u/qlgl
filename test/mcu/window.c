/**
 * @file    window.c
 * @brief   MCU 上跑 QLWindow：板级 QLPlatform + 静态 RGB565 面板 + 模拟按键。
 *
 * 这是 P2 的验收程序，验证"无 OS 也能用高层窗口 API"：
 *
 *   1. `qlgl_platform()` 拿到的是**板级**后端（强符号覆盖了桌面弱符号）；
 *   2. `ql_window_create()` / `bind_ui()` / `poll()` 能布局 + 光栅 + 刷屏；
 *   3. 触摸事件经环形队列 → `ql_input` → 界面层，点击回调触发、`:hover` 变色；
 *   4. 脏区重绘只刷一小块，`ql_window_render()` 才整屏；
 *   5. SysTick 毫秒时钟在跑（动画时间源）；
 *   6. `ql_window_resize()` 在固定面板上返回 UNSUPPORTED。
 *
 * 通过/失败用半主机 printf 打出来，`_exit` 让 QEMU 退出。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "qlplatform_board.h"
#include "ui_mcu_window.h"

/* ------------------------------ 断言 ------------------------------ */

static int s_checks;
static int s_fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        s_checks++;                                                            \
        if (!(cond)) {                                                         \
            s_fails++;                                                         \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_INT(expr, want)                                                  \
    do {                                                                       \
        const int v_ = (int)(expr);                                            \
        s_checks++;                                                            \
        if (v_ != (int)(want)) {                                               \
            s_fails++;                                                         \
            printf("  FAIL %s:%d  %s = %d (want %d)\n", __FILE__, __LINE__,    \
                   #expr, v_, (int)(want));                                    \
        }                                                                      \
    } while (0)

/* ------------------------------ 工具 ------------------------------ */

static uint16_t to565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t panel_px(int x, int y)
{
    return ql_board_panel()[(size_t)y * QL_BOARD_W + (size_t)x];
}

static int s_clicks;

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    s_clicks++;
}

/* 点：按钮区域的中心附近（见 window.css：20,50 起 200x40） */
#define BTN_X 120
#define BTN_Y 78

int main(void)
{
    printf("QLGL MCU window test\n");

    if (qlgl_init() != QL_OK) {
        printf("  qlgl_init failed\n");
        return 1;
    }

    /* 1) 后端应该是板级强符号，不是桌面默认 */
    const QLPlatform *plat = qlgl_platform();
    CHECK(plat != NULL);
    CHECK(plat != NULL && plat->name != NULL &&
          strcmp(plat->name, "mcu-board") == 0);
    printf("  platform = %s\n", (plat != NULL && plat->name != NULL)
                                    ? plat->name
                                    : "(null)");

    /* 2) 建窗口 + 绑界面 */
    QLUIRoot *ui = ql_ui_build(&ui_mcu_window_doc);
    CHECK(ui != NULL);
    if (ui == NULL) {
        printf("  ui build failed\n");
        return 1;
    }
    QLUINodeRT *btn = ql_ui_find_by_id(ui, "btn");
    CHECK(btn != NULL);
    CHECK_INT(ql_ui_set_node_click_cb(ui, btn, on_click, NULL), QL_OK);

    QLWindow *win = ql_window_create("mcu", QL_BOARD_W, QL_BOARD_H, QL_WINDOW_CPU);
    CHECK(win != NULL);
    if (win == NULL) {
        printf("  window create failed\n");
        return 1;
    }
    CHECK_INT(ql_window_bind_ui(win, ui), QL_OK);
    CHECK(ql_window_ui(win) == ui);

    ql_coord_t w = 0, h = 0;
    ql_window_size(win, &w, &h);
    CHECK_INT(w, QL_BOARD_W);
    CHECK_INT(h, QL_BOARD_H);

    /* 3) 首帧：整屏刷新，按钮区域是样式里的蓝色 */
    ql_board_reset_stats();
    CHECK_INT(ql_window_poll(win), 1);
    CHECK(ql_board_flush_count() > 0);
    CHECK_INT(ql_board_flush_pixels(), (uint64_t)QL_BOARD_W * QL_BOARD_H);
    for (int i = 0; i < 8; i++)
        (void)ql_window_poll(win);      /* 让悬停/动画等稳定 */
    const uint16_t idle_px = panel_px(BTN_X, BTN_Y);
    const uint16_t btn_px = to565(0x0969DAu);
    printf("  button pixel = 0x%04X (want 0x%04X)\n", idle_px, btn_px);
    CHECK_INT(idle_px, btn_px);

    /* 4) 触摸按下 → :hover/:active 变色，且只刷脏区 */
    ql_board_reset_stats();
    QLInputEvent mv;
    memset(&mv, 0, sizeof(mv));
    mv.type = QL_INPUT_POINTER_MOVE;
    mv.x = BTN_X;
    mv.y = BTN_Y;
    CHECK_INT(ql_board_push_event(&mv), 1);
    CHECK_INT(ql_window_poll(win), 1);

    ql_board_touch(BTN_X, BTN_Y, 1);
    CHECK_INT(ql_window_poll(win), 1);
    const uint16_t hover_px = panel_px(BTN_X, BTN_Y);
    printf("  hover pixel  = 0x%04X (want 0x%04X)\n", hover_px,
           to565(0x1F7FE8u));
    CHECK_INT(hover_px, to565(0x1F7FE8u));
    CHECK(ql_board_flush_pixels() > 0);
    CHECK(ql_board_flush_pixels() < (uint64_t)QL_BOARD_W * QL_BOARD_H);

    /* 5) 抬起 → 点击回调 */
    s_clicks = 0;
    ql_board_touch(BTN_X, BTN_Y, 0);
    CHECK_INT(ql_window_poll(win), 1);
    CHECK_INT(s_clicks, 1);
    printf("  clicks = %d\n", s_clicks);

    /* 6) 手动整屏重绘 */
    ql_board_reset_stats();
    ql_window_render(win);
    CHECK_INT(ql_board_flush_count(), 1);
    CHECK_INT(ql_board_flush_pixels(), (uint64_t)QL_BOARD_W * QL_BOARD_H);

    /* 7) SysTick 毫秒时钟在走（动画时间源；WFI 每 1ms 醒一次） */
    const uint32_t t0 = ql_board_now_ms();
    for (int i = 0; i < 64; i++)
        (void)ql_window_poll(win);
    const uint32_t t1 = ql_board_now_ms();
    printf("  systick: %u ms -> %u ms\n", (unsigned)t0, (unsigned)t1);
    CHECK(t1 > t0);

    /* 8) 固定面板不支持 resize */
    CHECK_INT(ql_window_resize(win, 120, 80), QL_ERR_UNSUPPORTED);

    /* 9) 退出 */
    ql_window_quit(win);
    CHECK_INT(ql_window_poll(win), 0);
    ql_window_destroy(win);
    ql_ui_free(ui);

    printf("%s  %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL",
           s_checks, s_fails);
    fflush(stdout);
    exit(s_fails == 0 ? 0 : 1);
}
