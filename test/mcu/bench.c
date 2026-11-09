/**
 * @file    bench.c
 * @brief   MCU 基准：在 QEMU 的 Cortex-M4 上量"一帧布局"和"一帧光栅"。
 *
 * 为什么单独做一个：PC 上的数字不能外推到 MCU。
 *   - PC 的布局/光栅有缓存、有宽流水、主频 3~4GHz；
 *   - MCU 是 100~200MHz、几十 KB RAM、无 FPU（或只有单精度）。
 *
 * 这里跑的是**和 PC 完全同一份源码**（src/ 下一行不改），差别只有三处：
 *   1. 文档是构建期静态数据（qltext cc 产物），运行期不解析 HTML/CSS；
 *   2. 帧缓冲是一块静态数组（RGB565），走 CPU 光栅；
 *   3. 时钟用 DWT 周期计数器（QEMU 的 `-icount` 下它按指令推进）。
 *
 * 量什么：
 *   layout  一个完整的 ql_ui_layout()（21 节点的小页面）
 *   render  一帧 ql_ui_render()（含清屏）
 *   idle    一帧"什么都没变"的渲染（脏区为空时的成本上限）
 *
 * 输出是**周期数**。换到时间需要乘主频，例如 200MHz 下 1ms = 200000 周期。
 * 用法见 test/mcu/README.md。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "ui_mcu_bench.h"

/* ------------------------------ 画布 ------------------------------ */

#define FB_W 240
#define FB_H 160

static uint16_t s_fb[FB_W * FB_H];      /* RGB565，38.4 KB */
static QLDispDriver s_disp;
static QLCanvas s_canvas;

/* ------------------------------ 计时 ------------------------------ */

/*
 * 计时：用 ARM 半主机的 `SYS_ELAPSED`（QEMU 下返回**虚拟纳秒**）。
 *
 * 为什么不用 DWT 的 CYCCNT：QEMU 对 M-profile 的 DWT 只实现了控制寄存器，
 * CYCCNT 恒为 0（实测）。半主机的 SYS_ELAPSED 是 QEMU 保证可用的，
 * 配合 `-icount shift=0`（每条指令推进 1ns）就等于"指令数"，
 * 也就是 1 IPC 假设下的周期数。
 *
 * 若宿主不需要可移植性，可以换成自己的 SysTick 实现 —— 接口就一个函数。
 */
static uint64_t semihost_elapsed(void)
{
    /* SYS_ELAPSED = 0x30：参数块里回填 64 位 tick 数 */
    uint32_t blk[2] = { 0, 0 };
    register int r0 __asm("r0") = 0x30;
    register uint32_t *r1 __asm("r1") = blk;
    __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return ((uint64_t)blk[1] << 32) | (uint64_t)blk[0];
}

static void cycles_init(void)
{
    /* 碰一下，确认半主机可用（不可用时 QEMU 会报错退出，不会静默给 0） */
    (void)semihost_elapsed();
}

static inline uint64_t cycles(void)
{
    return semihost_elapsed();
}

/* 反复跑，取最小值（排除中断/抖动的影响） */
static uint64_t bench_min(void (*fn)(void *), void *arg, int iters)
{
    uint64_t best = ~(uint64_t)0;
    for (int i = 0; i < iters; i++) {
        const uint64_t t0 = cycles();
        fn(arg);
        const uint64_t dt = cycles() - t0;
        if (dt < best)
            best = dt;
    }
    return best;
}

/* ------------------------------ 被测动作 ------------------------------ */

static QLUIRoot *s_ui;

static void do_layout(void *arg)
{
    (void)arg;
    ql_ui_layout(s_ui, FB_W, FB_H);
}

static void do_render(void *arg)
{
    (void)arg;
    ql_canvas_clear(&s_canvas, 0xFFFFFF);
    ql_ui_render_canvas(s_ui, &s_canvas);
}

static void do_idle(void *arg)
{
    (void)arg;
    /* 没有脏区时的"整屏重画"成本（保留式下会更低，这里保守量全量） */
    QLDraw2D d2d;
    if (ql_draw2d_canvas_init(&d2d, &s_canvas) == QL_OK)
        ql_ui_render_rect(s_ui, &d2d, ql_rect_make(0, 0, 1, 1));
}

int main(void)
{
    cycles_init();

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init failed\n");
        return 1;
    }

    memset(s_fb, 0, sizeof(s_fb));
    if (ql_disp_init_fb(&s_disp, s_fb, FB_W * 2u, FB_W, FB_H, QL_COLOR_RGB565,
                        NULL) != QL_OK) {
        printf("disp init failed\n");
        return 1;
    }
    ql_canvas_from_disp(&s_canvas, &s_disp);

    s_ui = ql_ui_build(&ui_mcu_bench_doc);
    if (s_ui == NULL) {
        printf("build failed\n");
        return 1;
    }

    /* 预热（首次布局会分配、字体首次光栅也会建缓存） */
    ql_ui_layout(s_ui, FB_W, FB_H);
    ql_canvas_clear(&s_canvas, 0xFFFFFF);
    ql_ui_render_canvas(s_ui, &s_canvas);

    const uint64_t c_layout = bench_min(do_layout, NULL, 10);
    const uint64_t c_render = bench_min(do_render, NULL, 10);
    const uint64_t c_idle = bench_min(do_idle, NULL, 20);

    printf("QLGL MCU bench  %dx%d RGB565  nodes=%u styles=%u\n", FB_W, FB_H,
           (unsigned)s_ui->node_count, (unsigned)ui_mcu_bench_doc.style_count);
    printf("  layout  %8u ns\n", (unsigned)c_layout);
    printf("  render  %8u ns  (含 clear)\n", (unsigned)c_render);
    printf("  idle    %8u ns  (1x1 脏区)\n", (unsigned)c_idle);
    /*
     * 换算成时间：cycles / 主频。用整数微秒算，避免拉进浮点 printf
     * （`nano.specs` 默认不带 %f，硬要用还得多链一个开关）。
     */
    {
        /* icount shift=0 → 1 条指令 = 1ns，所以"周期数"就是 ns */
        const uint64_t mhz = 200;
        const uint32_t lay_us = (uint32_t)(c_layout / mhz);
        const uint32_t ren_us = (uint32_t)(c_render / mhz);
        printf("  @%uMHz layout=%u.%03ums render=%u.%03ums\n", (unsigned)mhz,
               (unsigned)(lay_us / 1000), (unsigned)(lay_us % 1000),
               (unsigned)(ren_us / 1000), (unsigned)(ren_us % 1000));
        printf("  预算：30fps=33ms/frame，60fps=16ms/frame\n");
    }
    return 0;
}
