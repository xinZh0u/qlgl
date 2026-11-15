/**
 * @file    startup.c
 * @brief   裸机启动：向量表 + Reset_Handler（Cortex-M4，QEMU mps2-an386 / netduinoplus2）。
 *
 * 只做三件事：把 .data 从 flash 搬到 RAM、把 .bss 清零、跳 main()。
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;)
        *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss;)
        *dst++ = 0;
    (void)main();
    for (;;)
        __asm volatile("wfi");
}

static void Default_Handler(void)
{
    for (;;)
        __asm volatile("wfi");
}

/*
 * SysTick（异常 15）：板级文件定义强符号 `void SysTick_Handler(void)` 覆盖，
 * 这里只是默认的"卡住"实现（例如基准程序根本不开 SysTick）。
 * 其余向量都直接指向 Default_Handler。
 */
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

/*
 * 向量表：索引 0 是初始 SP，之后一项对应一个异常：
 *   1 Reset / 2 NMI / 3 HardFault / 4 MemManage / 5 BusFault / 6 UsageFault
 *   7~10 保留 / 11 SVCall / 12 DebugMon / 13 保留 / 14 PendSV / 15 SysTick
 * 外部中断从第 16 项起（这个板子没接，不展开）。
 * 保留项必须是 0（ARM 架构要求）——填错会让中断直接跳到 0 地址（HardFault）。
 */
__attribute__((section(".isr_vector"), used))
void (*const g_vectors[16])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler,   /* NMI */
    Default_Handler,   /* HardFault */
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
    0, 0, 0, 0,        /* 7~10 保留 */
    Default_Handler,   /* 11 SVCall */
    0,                 /* 12 DebugMon（不用） */
    0,                 /* 13 保留 */
    Default_Handler,   /* 14 PendSV */
    SysTick_Handler,   /* 15 SysTick */
};
