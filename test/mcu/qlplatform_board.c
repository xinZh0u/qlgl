/**
 * @file    qlplatform_board.c
 * @brief   板级 `QLPlatform`：静态 RGB565 面板 + 按键队列 + SysTick 时钟。
 *
 * 这个文件就是"MCU 上落地平台层"的完整答案，只有两件事要自己写：
 *
 *   1. `window_open`：把一块静态帧缓冲包成 `QLDispDriver`（`ql_disp_init_fb`），
 *      从这里拿到 `QLDraw2D` 交给渲染器；
 *   2. `poll`/`wait`：把按键/触摸从环形队列搬进 `ql_input`，空闲时 `WFI`。
 *
 * 应用层完全不知道这些：它仍然只写 `qlgl_init()` + `ql_window_*`。
 *
 * 覆盖方式：定义**强符号** `qlgl_platform_default()`（库里那个是弱符号），
 * 链接时优先用这里。所以只要把这个文件（或你自己板子的同名文件）编进去即可。
 *
 * 面板尺寸是固定的，`ql_window_resize()` 在这里返回 `QL_ERR_UNSUPPORTED`。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "qlplatform_board.h"

/* ------------------------------ 面板 ------------------------------ */

static uint16_t s_fb[QL_BOARD_W * QL_BOARD_H];      /* 渲染目标，RGB565 */
static uint16_t s_panel[QL_BOARD_W * QL_BOARD_H];   /* "屏幕"：flush 后的样子 */

static uint32_t s_flush_count;
static uint64_t s_flush_pixels;
static int      s_flushed_full;

void ql_board_reset_stats(void)
{
    s_flush_count = 0;
    s_flush_pixels = 0;
    s_flushed_full = 0;
}

uint32_t ql_board_flush_count(void) { return s_flush_count; }
uint64_t ql_board_flush_pixels(void) { return s_flush_pixels; }
const uint16_t *ql_board_panel(void) { return s_panel; }

/*
 * 刷屏回调：真实板子在这里起 SPI/DMA 传输；QEMU 里只是把脏矩形拷进
 * 另一块内存（当作"屏幕上现在显示的内容"），顺便累计统计给测试断言用。
 */
static int board_flush(QLDispDriver *self, QLRect area)
{
    if (self == NULL || area.w <= 0 || area.h <= 0)
        return 0;

    const uint16_t *src = (const uint16_t *)self->framebuffer;
    ql_coord_t x0 = area.x, y0 = area.y;
    ql_coord_t x1 = area.x + area.w, y1 = area.y + area.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > self->width) x1 = self->width;
    if (y1 > self->height) y1 = self->height;

    for (ql_coord_t y = y0; y < y1; y++) {
        uint16_t *dst = &s_panel[(size_t)y * QL_BOARD_W];
        const uint16_t *row = &src[(size_t)y * QL_BOARD_W];
        for (ql_coord_t x = x0; x < x1; x++)
            dst[x] = row[x];
    }

    s_flush_count++;
    s_flush_pixels += (uint64_t)area.w * (uint64_t)area.h;
    if (area.w >= self->width && area.h >= self->height)
        s_flushed_full = 1;
    return 0;
}

/* ------------------------------ 时钟 ------------------------------ */

/*
 * SysTick：1ms 中断，累加毫秒数。
 *
 * 默认按 25MHz 内核时钟（mps2-an386 / 多数 M4 小板）。换板子改
 * `QL_BOARD_CORE_HZ`，或用 CubeMX 生成的 `HAL_GetTick()` 替掉这里。
 */
#ifndef QL_BOARD_CORE_HZ
#define QL_BOARD_CORE_HZ 25000000u
#endif

#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)

static volatile uint32_t s_ms;

/* 向量表第 15 项（startup.c 里已留好），强符号覆盖默认的死循环 handler */
void SysTick_Handler(void)
{
    s_ms++;
}

uint32_t ql_board_now_ms(void) { return s_ms; }

static void board_clock_init(void)
{
    SYST_RVR = (QL_BOARD_CORE_HZ / 1000u) - 1u;   /* 1ms */
    SYST_CVR = 0;
    SYST_CSR = 7u;                                 /* ENABLE | TICKINT | CLKSOURCE */
    __asm volatile("cpsie i" ::: "memory");        /* 开全局中断 */
}

/* ------------------------------ 按键队列 ------------------------------ */

#define BOARD_EVQ 16
static QLInputEvent s_evq[BOARD_EVQ];
static int s_evq_head;
static int s_evq_tail;

int ql_board_push_event(const QLInputEvent *ev)
{
    if (ev == NULL)
        return 0;
    const int next = (s_evq_tail + 1) % BOARD_EVQ;
    if (next == s_evq_head)
        return 0;                       /* 满：丢掉本事件 */
    s_evq[s_evq_tail] = *ev;
    s_evq_tail = next;
    return 1;
}

static int board_pop_event(QLInputEvent *out)
{
    if (s_evq_head == s_evq_tail)
        return 0;
    *out = s_evq[s_evq_head];
    s_evq_head = (s_evq_head + 1) % BOARD_EVQ;
    return 1;
}

void ql_board_touch(int32_t x, int32_t y, int down)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.x = x;
    ev.y = y;
    ev.type = down ? QL_INPUT_POINTER_DOWN : QL_INPUT_POINTER_UP;
    ev.button = QL_BUTTON_LEFT;
    ev.buttons = down ? QL_BUTTON_BIT_LEFT : 0;
    (void)ql_board_push_event(&ev);
}

void ql_board_key(uint32_t key, int down)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? QL_INPUT_KEY_DOWN : QL_INPUT_KEY_UP;
    ev.key = key;
    (void)ql_board_push_event(&ev);
}

/* ------------------------------ 窗口 ------------------------------ */

typedef struct {
    QLDispDriver disp;
    QLCanvas     canvas;
    QLDraw2D     d2d;
    QLRect       flush;
    int          full;
} BoardWindow;

static BoardWindow s_win;      /* 单屏：静态对象，零分配 */
static int         s_open;

static QLPlatformWindow *board_open(const char *title, ql_coord_t w, ql_coord_t h,
                                    int backend)
{
    (void)title;
    (void)backend;

    if (s_open)
        return NULL;               /* 一进程一屏（与输入汇的单例一致） */

    /*
     * 面板尺寸固定。请求比面板大就拒绝（应用应该按面板尺寸建窗口），
     * 比面板小则按请求的尺寸用——测试里正好验证"窗口小于面板"。
     */
    if (w <= 0 || h <= 0 || w > QL_BOARD_W || h > QL_BOARD_H) {
        QL_LOGE("board: window %dx%d does not fit panel %dx%d",
                (int)w, (int)h, QL_BOARD_W, QL_BOARD_H);
        return NULL;
    }

    BoardWindow *b = &s_win;
    memset(b, 0, sizeof(*b));

    if (ql_disp_init_fb(&b->disp, s_fb, (uint32_t)QL_BOARD_W * 2u, w, h,
                        QL_COLOR_RGB565, board_flush) != QL_OK) {
        printf("board: disp init failed\n");
        return NULL;
    }
    if (ql_canvas_from_disp(&b->canvas, &b->disp) != QL_OK) {
        printf("board: canvas failed\n");
        return NULL;
    }
    if (ql_draw2d_canvas_init(&b->d2d, &b->canvas) != QL_OK) {
        printf("board: draw2d failed\n");
        return NULL;
    }

    /* 动画时间源：裸机没有 clock_gettime，用 SysTick 的毫秒计数 */
    ql_anim_set_time_source(ql_board_now_ms);
    board_clock_init();

    s_evq_head = s_evq_tail = 0;
    ql_board_reset_stats();
    memset(s_panel, 0xFF, sizeof(s_panel));   /* 未刷新前当白屏 */
    s_open = 1;
    return (QLPlatformWindow *)b;
}

static void board_close(QLPlatformWindow *pw)
{
    if (pw == NULL)
        return;
    s_open = 0;
}

static int board_poll(QLPlatformWindow *pw)
{
    (void)pw;
    /* 中断里入队的事件，在主循环里搬进 ql_input —— 不在 ISR 里 push */
    QLInputEvent ev;
    while (board_pop_event(&ev))
        (void)ql_input_push(&ev);
    return 1;                       /* 没有"关窗"概念 */
}

static int board_wait(QLPlatformWindow *pw, int timeout_ms)
{
    (void)pw;
    (void)timeout_ms;
    /* SysTick 每 1ms 来一次中断：WFI 一定醒得过来，静止时也省电 */
    __asm volatile("wfi");
    return 1;
}

static void board_size(QLPlatformWindow *pw, ql_coord_t *w, ql_coord_t *h)
{
    BoardWindow *b = (BoardWindow *)pw;
    if (w != NULL) *w = ql_disp_css_width(&b->disp);
    if (h != NULL) *h = ql_disp_css_height(&b->disp);
}

static int board_begin(QLPlatformWindow *pw, ql_color_t bg, const QLRect *dirty,
                       int n)
{
    BoardWindow *b = (BoardWindow *)pw;
    b->full = 0;
    b->flush = ql_rect_make(0, 0, 0, 0);
    if (n <= 0) {
        ql_canvas_clear(&b->canvas, bg);
        b->full = 1;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (ql_rect_is_empty(dirty[i]))
            continue;
        ql_canvas_fill_rect(&b->canvas, dirty[i], bg);
        b->flush = (b->flush.w <= 0) ? dirty[i]
                                     : ql_rect_union(b->flush, dirty[i]);
    }
    return 0;
}

static QLDraw2D *board_draw(QLPlatformWindow *pw)
{
    return (pw != NULL) ? &((BoardWindow *)pw)->d2d : NULL;
}

static void board_end(QLPlatformWindow *pw)
{
    BoardWindow *b = (BoardWindow *)pw;
    if (b == NULL || b->disp.flush == NULL)
        return;
    const QLRect area = b->full
                            ? ql_rect_make(0, 0, b->disp.width, b->disp.height)
                            : b->flush;
    if (!ql_rect_is_empty(area))
        (void)b->disp.flush(&b->disp, area);
}

static QLErr board_resize(QLPlatformWindow *pw, ql_coord_t w, ql_coord_t h)
{
    (void)pw; (void)w; (void)h;
    return QL_ERR_UNSUPPORTED;      /* 面板尺寸固定 */
}

static const QLPlatform s_board = {
    .name = "mcu-board",
    .window_open = board_open,
    .window_close = board_close,
    .poll = board_poll,
    .wait = board_wait,
    .size = board_size,
    .resize = board_resize,
    .begin = board_begin,
    .draw = board_draw,
    .end = board_end,
    /* 剪贴板/IME/文件/性能统计：板子上没有，留 NULL */
};

/*
 * 强符号覆盖库里的弱符号 `qlgl_platform_default()`。
 * 应用层什么都不用改：`qlgl_init()` 之后 `ql_window_create()` 就用这个后端。
 */
const QLPlatform *qlgl_platform_default(void)
{
    return &s_board;
}
