/* BD19 has 64 KiB of main RAM; retain the small log below MaskROM workspace,
 * and below it the reset mailbox (app.ld's on the BR23), at an address the
 * updater and the app share.  The heap ends at the mailbox. */
RESET_MAILBOX_BEGIN = 0xf330;
RESET_MAILBOX_SIZE = 16;
BD19_RETAINED_BEGIN = 0xf340;  /* up to UPDATA_BEG 0xfb60 */
PROVIDE(text_code_begin = text_begin);
PROVIDE(text_code_end = text_end);
PROVIDE(_data_code_begin = data_code_addr);
PROVIDE(_data_code_end = data_code_addr + data_code_size);
SECTIONS {
    .reset_mailbox RESET_MAILBOX_BEGIN (NOLOAD) : ALIGN(4) { KEEP(*(.reset_mailbox)) } > ram0
    .retained BD19_RETAINED_BEGIN (NOLOAD) : ALIGN(4) { KEEP(*(.retained)) } > ram0
}
ASSERT(SIZEOF(.reset_mailbox) <= RESET_MAILBOX_SIZE, "soft-reset mailbox exceeds reserved SRAM")
ASSERT(BD19_RETAINED_BEGIN + SIZEOF(.retained) <= UPDATA_BEG, "BD19 retained RAM overflow")
ASSERT(_HEAP_BEGIN < RESET_MAILBOX_BEGIN, "BD19 RAM overflow")
