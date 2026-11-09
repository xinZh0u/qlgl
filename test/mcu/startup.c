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

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler,   /* NMI */
    Default_Handler,   /* HardFault */
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
};
