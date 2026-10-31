/* A YC3121 app in the 8BitDo boot loader's slot: flash 0x01018000 up to the
 * data pages at 0x0107c000.  SRAM: ours 0x20000..0x2e7ff, the stack at its
 * top; then what survives a restart through the boot loader (.noinit, above
 * its data, below its stack at 0x2ff00); its mailbox at 0x2ffc0.  A chip
 * reset clears all of it. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x01018000, LENGTH = 0x64000
    RAM (rwx)  : ORIGIN = 0x00020000, LENGTH = 0xe800
    KEPT (rw)  : ORIGIN = 0x0002e800, LENGTH = 0x1400
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

    end = .;                            /* newlib's heap */
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /* Start-up neither loads nor clears it. */
    .noinit (NOLOAD) : {
        *(.noinit .noinit.*)
        . = ALIGN(4);
    } > KEPT

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
