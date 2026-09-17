OVERLAY : NOCROSSREFS AT(0x200000) SUBALIGN(4)
{
    .overlay_aec {
        LONG(0xFFFFFFFF);
        o_aec_end = .;
        *(.aec_mem) *(.msbc_enc)
    }
    .overlay_mp3 {
        LONG(0xFFFFFFFF);
        o_mp3_end = .;
        *(.mp3_mem) *(.mp3_ctrl_mem) *(.mp3pick_mem) *(.mp3pick_ctrl_mem)
    }
    .overlay_wma {
        LONG(0xFFFFFFFF);
        o_wma_end = .;
        *(.wma_mem) *(.wma_ctrl_mem) *(.wmapick_mem) *(.wmapick_ctrl_mem)
    }
    .overlay_wav {
        LONG(0xFFFFFFFF);
        *(.wav_mem) *(.wav_ctrl_mem)
    }
    .overlay_ape {
        LONG(0xFFFFFFFF);
        *(.ape_mem) *(.ape_ctrl_mem) *(.ape_bss) *(.ape_data)
    }
    .overlay_flac {
        LONG(0xFFFFFFFF);
        *(.flac_mem) *(.flac_ctrl_mem) *(.flac_dec_bss) *(.flac_bss)
    }
    .overlay_noise_reduction {
        LONG(0xFFFFFFFF);
        *(.noise_reduction)
    }
    .overlay_m4a {
        LONG(0xFFFFFFFF);
        *(.m4a_mem) *(.m4a_ctrl_mem) *(.m4a_dec_bss) *(.m4a_bss)
        *(.aac_ctrl_mem) *(.aac_bss) *(.alac_ctrl_mem) *(.alac_bss)
    }
    .overlay_amr {
        LONG(0xFFFFFFFF);
        *(.amr_mem) *(.amr_ctrl_mem) *(.amr_dec_bss)
    }
    .overlay_dts {
        LONG(0xFFFFFFFF);
        *(.dts_mem) *(.dts_ctrl_mem) *(.dts_dec_bss) *(.dts_bss)
    }
    .overlay_fm {
        LONG(0xFFFFFFFF);
        o_fm_end = .;
        *(.fm_mem) *(.linein_pcm_mem)
    }
} > ram0
