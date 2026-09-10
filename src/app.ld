/* Project-owned application tail for the SDK's base linker script. */
SECTIONS
{
    .text : ALIGN(4)
    {
        gsensor_dev_begin = .; KEEP(*(.gsensor_dev)) gsensor_dev_end = .;
        codec_dev_begin = .; KEEP(*(.codec_dev)) codec_dev_end = .;
        hrsensor_dev_begin = .; KEEP(*(.hrsensor_dev)) hrsensor_dev_end = .;
        fm_dev_begin = .; KEEP(*(.fm_dev)) fm_dev_end = .;
        fm_emitter_dev_begin = .;
        KEEP(*(.fm_emitter_dev))
        fm_emitter_dev_end = .;
        adapter_idev_begin = .; KEEP(*(.adapter_idev)) adapter_idev_end = .;
        adapter_odev_begin = .; KEEP(*(.adapter_odev)) adapter_odev_end = .;
        adapter_enc_begin = .; KEEP(*(.adapter_enc)) adapter_enc_end = .;
        adapter_decoder_begin = .;
        KEEP(*(.adapter_decoder))
        adapter_decoder_end = .;
        adapter_encoder_begin = .;
        KEEP(*(.adapter_encoder))
        adapter_encoder_end = .;
        chargeIc_dev_begin = .; KEEP(*(.chargeIc_dev)) chargeIc_dev_end = .;

        *(.text*)
        *(.LOG_TAG_CONST*)
        *(.rodata*)
        . = ALIGN(4);
    } > code0

    .data : ALIGN(4)
    {
        *(.data*)
        *(.cvsd_data)
        dec_board_param_mem_begin = .;
        KEEP(*(.dec_board_param_mem))
        dec_board_param_mem_end = .;
        *(.sbc_eng_code)
        . = ALIGN(4);
    } > ram0

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.usb_h_dma) *(.usb_ep0) *(.dec_mix_buff)
        *(.sd0_var) *(.sd1_var) *(.dac_buff)
        *(.src_filt) *(.src_dma)
        . = ALIGN(32);
    } > ram0
}
