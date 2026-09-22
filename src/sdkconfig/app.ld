RESET_MAILBOX_SIZE = 16;
BRINGUP_LOG_BEGIN = 0x20010;
BRINGUP_LOG_SIZE = 1320;
RESET_MAILBOX_BEGIN = 0x20000;

SECTIONS
{
    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > code0

    .data : ALIGN(4)
    {
        *(.data*)
        . = ALIGN(4);
    } > ram0

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.usb_h_dma)
        *(.usb_ep0)
        *(.dec_mix_buff)
        *(.sd0_var)
        *(.sd1_var)
        *(.dac_buff)
        *(.src_filt)
        *(.src_dma)
        . = ALIGN(32);
    } > ram0

    /*
     * One soft-reset handoff record lives in the gap between all known
     * low-memory bootloader use and MaskROM's high workspace.  The secondary
     * uboot is linked at 0x4000 in a 20-KiB window; MaskROM's low BSS ends
     * near 0x11424, while its high workspace starts at 0x2bdc0.  SDK startup
     * does not clear this address.
     *
     * The Makefile also moves HEAP_END down to RESET_MAILBOX_BEGIN.  Keep the
     * reservation larger than the payload so the ABI can grow without moving
     * its address.
     */
    .reset_mailbox RESET_MAILBOX_BEGIN (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.reset_mailbox))
        . = ALIGN(4);
    } > ram0

    /*
     * Bring-up instrumentation (see src/main.c).  It lives right after the
     * mailbox so the mailbox itself keeps its fixed, preserved address.
     */
    .bringup_log BRINGUP_LOG_BEGIN (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.bringup_log))
        . = ALIGN(4);
    } > ram0
}

ASSERT(SIZEOF(.reset_mailbox) <= RESET_MAILBOX_SIZE,
       "soft-reset mailbox exceeds reserved SRAM")
ASSERT(SIZEOF(.bringup_log) <= BRINGUP_LOG_SIZE,
       "bring-up log exceeds reserved SRAM")
ASSERT(RESET_MAILBOX_BEGIN >= RAM_USED,
       "normal RAM sections overlap soft-reset mailbox")
ASSERT(HEAP_END <= RESET_MAILBOX_BEGIN,
       "SDK heap overlaps soft-reset mailbox")
