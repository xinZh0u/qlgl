/* mps2-an386 / netduinoplus2：Flash 0x00000000 起，RAM 0x20000000 起。
 *
 * mps2-an386 的 QEMU 模型里 0x00000000 是可执行 SRAM（ZBT），
 * 0x20000000 也有 SRAM。这里把代码放 0x00000000，数据与栈放 0x20000000。
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text : {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(8);
    end = .;

    _estack = ORIGIN(RAM) + LENGTH(RAM);
}
