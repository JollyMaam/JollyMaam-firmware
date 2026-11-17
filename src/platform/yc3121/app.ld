/* A YC3121 program in the 8BitDo boot loader's slot, from 0x01018000: the
 * pad's updater there, its app at TEXT_AT above (behind its header); a build
 * without updater, or the bring-up app, at the slot's start.  The slot ends
 * at 0x01060000 (flash_yc3121.h: past it the ciphered flash would overlap
 * the settings store's raw banks).  make sets TEXT_AT and TEXT_END with
 * --defsym; without them, the whole slot.  SRAM: ours 0x20000..0x2e7ff, the
 * stack at its top; then what survives a restart through the boot loader
 * (KEPT, above its data, below its stack at 0x2ff00): the handover words
 * first (the same place in the updater and the app), the log, then the rest
 * of .noinit; the loader's mailbox at 0x2ffc0.  A chip reset clears all of
 * it. */
TEXT_AT = DEFINED(TEXT_AT) ? TEXT_AT : 0x01018000;
TEXT_END = DEFINED(TEXT_END) ? TEXT_END : 0x01060000;

MEMORY
{
    FLASH (rx) : ORIGIN = 0x01018000, LENGTH = 0x48000
    RAM (rwx)  : ORIGIN = 0x00020000, LENGTH = 0xe800
    KEPT (rw)  : ORIGIN = 0x0002e800, LENGTH = 0x1400
}

ENTRY(_entry)

SECTIONS
{
    .text TEXT_AT : {
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
    __text_end = __data_load + SIZEOF(.data);
    ASSERT(__text_end <= TEXT_END, "the program runs past its room in the slot")

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
        KEEP(*(.handover))
        . = ORIGIN(KEPT) + 16;
        *(.retained)
        *(.noinit .noinit.*)
        . = ALIGN(4);
    } > KEPT

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
