/* BD19 has 64 KiB of main RAM; retain the small log below MaskROM workspace. */
BD19_RETAINED_BEGIN = 0xf340;  /* up to UPDATA_BEG 0xfb60: the heap ends here */
PROVIDE(text_code_begin = text_begin);
PROVIDE(text_code_end = text_end);
PROVIDE(_data_code_begin = data_code_addr);
PROVIDE(_data_code_end = data_code_addr + data_code_size);
SECTIONS {
    .retained BD19_RETAINED_BEGIN (NOLOAD) : ALIGN(4) { KEEP(*(.retained)) } > ram0
}
ASSERT(BD19_RETAINED_BEGIN + SIZEOF(.retained) <= UPDATA_BEG, "BD19 retained RAM overflow")
ASSERT(_HEAP_BEGIN < BD19_RETAINED_BEGIN, "BD19 RAM overflow")
