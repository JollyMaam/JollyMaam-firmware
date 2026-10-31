/* A YC3121 app in the 8BitDo boot loader's slot: flash 0x01018000 up to the
 * data pages at 0x0107c000; SRAM 0x20000..0x2ffff, the stack at its top. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x01018000, LENGTH = 0x64000
    RAM (rwx)  : ORIGIN = 0x00020000, LENGTH = 0x10000
}

ENTRY(_entry)

SECTIONS
{
    .text : {
        KEEP(*(.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
