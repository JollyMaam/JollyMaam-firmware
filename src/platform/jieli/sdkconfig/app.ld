RESET_MAILBOX_SIZE = 16;
RETAINED_BEGIN = 0x20010;
RETAINED_SIZE = 0x2100;
RESET_MAILBOX_BEGIN = 0x20000;
/* MaskROM's boot-state token, which `uboot` writes before its reset. */
MASKROM_TOKEN_BEGIN = 0x10800;
MASKROM_TOKEN_END = 0x10810;

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
     * sdk_ld_*.c also move HEAP_END down to RESET_MAILBOX_BEGIN.  Keep the
     * reservation larger than the payload so the ABI can grow without moving
     * its address.
     */
    .reset_mailbox RESET_MAILBOX_BEGIN (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.reset_mailbox))
        . = ALIGN(4);
    } > ram0

    .retained RETAINED_BEGIN (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.retained))
        . = ALIGN(4);
    } > ram0
}

ASSERT(SIZEOF(.reset_mailbox) <= RESET_MAILBOX_SIZE,
       "soft-reset mailbox exceeds reserved SRAM")
ASSERT(SIZEOF(.retained) <= RETAINED_SIZE,
       "retained RAM exceeds its reservation")
ASSERT(RESET_MAILBOX_BEGIN >= RAM_USED,
       "normal RAM sections overlap soft-reset mailbox")
/* The Bluetooth controller's RAM (its baseband buffers among it) must not
 * cover the token: the radio overwrote it and `uboot` rebooted into the app.
 * sdk_ld_br23.c starts that block at MASKROM_TOKEN_END at the earliest; the
 * space below it goes to the app's .bss or stays unused. */
ASSERT(btctler_bss_start >= MASKROM_TOKEN_END ||
       btctler_bss_end <= MASKROM_TOKEN_BEGIN,
       "Bluetooth controller RAM covers the MaskROM uboot token")
ASSERT(HEAP_END <= RESET_MAILBOX_BEGIN,
       "SDK heap overlaps soft-reset mailbox")
