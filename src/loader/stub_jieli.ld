/* The stub loader: code only, at the entry uboot.boot jumps to. */
ENTRY(_start)
SECTIONS
{
    .text 0x1e00120 :
    {
        *(.startup.text)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON) }
}
