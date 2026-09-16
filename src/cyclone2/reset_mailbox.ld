/*
 * One soft-reset handoff record lives in the gap between all known low-memory
 * bootloader use and MaskROM's high workspace.  The secondary uboot is linked
 * at 0x4000 in a 20-KiB window; MaskROM's low BSS ends near 0x11424, while its
 * high workspace starts at 0x2bdc0.  SDK startup does not clear this address.
 *
 * The Makefile also moves HEAP_END down to RESET_MAILBOX_BEGIN.  Keep the
 * reservation larger than the payload so the ABI can grow without moving its
 * address.
 */
RESET_MAILBOX_SIZE = 16;
RESET_MAILBOX_BEGIN = 0x20000;

SECTIONS
{
    .reset_mailbox RESET_MAILBOX_BEGIN (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.reset_mailbox))
        . = ALIGN(4);
    } > ram0
}

ASSERT(SIZEOF(.reset_mailbox) <= RESET_MAILBOX_SIZE,
       "soft-reset mailbox exceeds reserved SRAM")
ASSERT(RESET_MAILBOX_BEGIN >= RAM_USED,
       "normal RAM sections overlap soft-reset mailbox")
ASSERT(HEAP_END <= RESET_MAILBOX_BEGIN,
       "SDK heap overlaps soft-reset mailbox")
