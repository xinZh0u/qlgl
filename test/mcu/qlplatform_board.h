/**
 * @file    qlplatform_board.h
 * @brief   板级平台（Cortex-M4 + RGB565 面板）的对外小接口：模拟按键/触摸、
 *          读回"屏幕"内容与刷新统计。
 *
 * 这是 `test/mcu/` 里的板级示例，不是库的一部分：它演示 MCU 上如何用
 * `qlgl_platform_default()` 强符号覆盖桌面后端。真实板子把
 * `ql_board_push_event()` 换成"按键中断里入队"即可，接口保持一致。
 *
 * 触摸/按键走**中断 → 环形队列 → 主循环搬运**：不在 ISR 里直接
 * `ql_input_push()`（那样会和主循环抢队列，且 ISR 里不能分配/加锁）。
 */
#ifndef QLGL_TEST_MCU_PLATFORM_BOARD_H
#define QLGL_TEST_MCU_PLATFORM_BOARD_H

#include <stdint.h>

#include <qlgl/qlgl.h>

/* 面板尺寸（RGB565，静态帧缓冲） */
#define QL_BOARD_W 240
#define QL_BOARD_H 160

/*
 * 往"按键队列"里塞一个输入事件（模拟中断）。
 * 队列满时丢掉本事件并返回 0。
 */
int ql_board_push_event(const QLInputEvent *ev);

/* 便捷包装：pointer_down/up（触摸屏按下/抬起也走这个） */
void ql_board_touch(int32_t x, int32_t y, int down);
/* 便捷包装：键按下/抬起（键码用 QLKey） */
void ql_board_key(uint32_t key, int down);

/* "面板"内容（最近一次 flush 的结果，RGB565），用于断言画面 */
const uint16_t *ql_board_panel(void);
/* 累计 flush 次数 / 累计刷新像素数 / 是否整屏刷过 */
uint32_t ql_board_flush_count(void);
uint64_t ql_board_flush_pixels(void);
/* 清零统计（测新一轮之前调用） */
void ql_board_reset_stats(void);

/* 板级毫秒时钟（SysTick 中断累加），也是动画的时间源 */
uint32_t ql_board_now_ms(void);

#endif /* QLGL_TEST_MCU_PLATFORM_BOARD_H */
