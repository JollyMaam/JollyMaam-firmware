/* B80 (16 KiB SRAM) images started by a boot loader: the 8BitDo one for
 * the updater, the updater for the app.  It copies the image's first
 * (header word 0x0c, low half) x 16 bytes to SRAM at 0x840000 and restarts
 * the CPU there; the rest runs from flash at APP_START + its image offset
 * (--defsym=APP_START=...).  This replaces the SDK's boot.link and keeps
 * the symbol names cstartup_flash.S reads.
 *
 * SRAM, without alignment gaps:
 *   0x840000     vectors, RAM code, .data          copied by the boot loader
 *                .bss: irq_stk (its lowest word a guard), the rest
 *                (by alignment: no padding); .data_reload
 *   _ram_use_end_  the main stack, growing down    whatever is left
 *   _stack_top_  instruction cache: 256 B tags, 2 KiB lines, up to the end
 * The cache's place is any 256-byte boundary (register 0x80060c, set by
 * cstartup from _start_text_); at the end of SRAM that costs nothing.
 * cstartup starts main on a stack at the end of SRAM: platform_telink.c's
 * main moves it to _stack_top_ first.  tools/stackdepth.py checks the main
 * stack against _stack_top_ - _ram_use_end_ and the IRQ's against irq_stk. */

ENTRY(__start)

SRAM_BASE  = 0x840000;
SRAM_END   = 0x844000;
CACHE_SIZE = 0x900;
COPY_MAX   = 0x3200;             /* the 8BitDo boot loader's limit */
STACK_MIN  = 768;                /* a floor at link time; make u2cw-test runs
                                    tools/stackdepth.py for the real need */

SECTIONS
{
    /* Copied by the boot loader: code at its image offset, data at SRAM_BASE + it. */
    .vectors 0 :
    {
        KEEP(*(.vectors))
        *(.vectors.*)
    }
    /* The generic FSK driver (genfsk_ll.o, 1 KiB) runs from flash: only the
     * control loop calls it.  So does the 32k-RC sleep entry
     * (ext_pm_32k_rc*.o, 1.2 KiB): only deep sleep uses it, which never
     * returns; the step that stops the flash (sleep_start) stays in RAM. */
    .ram_code :
    {
        *(EXCLUDE_FILE(*liblt_8208*.a:genfsk_ll.o *liblt_8208*.a:ext_pm_32k_rc*.o) .ram_code)
        *(.ram_code.*)
    }
    . = ALIGN(4);
    _dstored_ = .;
    .data SRAM_BASE + _dstored_ : AT(_dstored_)
    {
        _start_data_ = .;
        *(SORT_BY_ALIGNMENT(.data) SORT_BY_ALIGNMENT(.data.*))
        . = ALIGN(4);
        _end_data_ = .;
    }
    _copy_size_ = _end_data_ - SRAM_BASE;
    /* The SDK's name for header word 0x0c's count, in 16-byte units. */
    _data_end_div_16_align_256_ = (_copy_size_ + 15) / 16;

    /* irq_stk (cstartup's .bss: nothing else, 16-byte aligned) first: its
     * lowest word is the guard an IRQ stack overflow hits. */
    .bss (NOLOAD) :
    {
        _start_bss_ = .;
        _irq_stk_guard_ = .;
        *cstartup_flash.S.o(.bss)
        *(SORT_BY_ALIGNMENT(.sbss) SORT_BY_ALIGNMENT(.sbss.*) SORT_BY_ALIGNMENT(.bss)
          SORT_BY_ALIGNMENT(.bss.*) SORT_BY_ALIGNMENT(COMMON))
        . = ALIGN(4);
        _end_bss_ = .;
    }
    /* Flash, after the copied part.  Keep aliases to the SDK's connection
     * validator and TX FIFO push before --defsym redirects their names to
     * the app's shims (rules.mk). */
    .text APP_START + _copy_size_ : AT(_copy_size_)
    {
        sdk_ll_procConnectReq = .;
        KEEP(*(.text.blc_ll_procConnectReq))
        sdk_ll_push_tx_fifo = .;
        KEEP(*(.text.bls_ll_pushTxFifo))
        *(.text)
        *liblt_8208*.a:genfsk_ll.o(.ram_code)
        *liblt_8208*.a:ext_pm_32k_rc*.o(.ram_code)
        *(.text.*)
    }
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    }
    _code_size_ = . - APP_START;
    _rstored_ = .;
    /* Copied by cstartup from flash (_rstored_), also after a retention wake. */
    .data_reload _end_bss_ : AT(_rstored_ - APP_START)
    {
        _data_reload_start_ = .;
        *(.data_reload)
        *(.data_reload.*)
        . = ALIGN(4);
        _data_reload_end_ = .;
    }
    _ram_use_end_ = .;
    _sstored_ = _rstored_ + SIZEOF(.data_reload);
    .sdk_version _sstored_ : AT(_sstored_ - APP_START)
    {
        . = ALIGN(4);
        KEEP(*(.sdk_version))
        KEEP(*(.sdk_version.*))
    }
    _bin_size_ = _code_size_ + SIZEOF(.data_reload) + SIZEOF(.sdk_version);
    _bin_size_div_16 = (_bin_size_ + 15) / 16;

    _stack_top_ = SRAM_END - CACHE_SIZE;
    _start_text_ = _stack_top_ - SRAM_BASE;  /* the SDK's name for the cache's offset */
    _ictag_start_ = _stack_top_;
    _ictag_end_ = _stack_top_ + 0x100;
}

ASSERT(_stack_top_ % 256 == 0, "the cache must start on a 256-byte boundary")
ASSERT(_copy_size_ <= COPY_MAX, "RAM code and .data: more than the boot loader copies")
ASSERT(_ram_use_end_ + STACK_MIN <= _stack_top_, "RAM: less than STACK_MIN left for the main stack")
