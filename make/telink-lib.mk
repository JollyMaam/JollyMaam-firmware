# Telink B80 boards on lib.mk: the toolchain, shared source lists and the
# build steps as target types. Each board spells out its own targets in its
# rules.mk. The data (flags, SDK sources) is in make/telink.mk.
#
# Our files, the SDK's and BTstack's build with different flags (the SDK's
# with its own ABI and only its headers). A board's target has the flags
# all three share; per-object variables add each group's:
#   $(call lib-objs,T,OUR_SOURCES): obj_cppflags += <our includes>
#   $(call lib-objs,T,OUR_SOURCES): obj_cflags += $(TELINK_CFLAGS)
#   $(call lib-objs,T,$(TELINK_SDK_SOURCES)): obj_cppflags += $(TELINK_INCLUDES)
#   $(call lib-objs,T,$(TELINK_SDK_C_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS) -w
#   $(call lib-objs,T,$(TELINK_SDK_S_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS)
#   $(call lib-objs,T,$(TELINK_BTSTACK_SOURCES)): obj_cppflags += $(BTSTACK_RENAMES) $(TELINK_BTSTACK_INCLUDES)
#   $(call lib-objs,T,$(TELINK_BTSTACK_SOURCES)): obj_cflags += $(TELINK_CFLAGS)

# The toolchain: NAME_toolchain := TELINK_B80_ (not TELINK_: TELINK_CFLAGS
# are our files' flags only).
TELINK_B80_CC := $(TELINK_CC)
TELINK_B80_LINK := $(TELINK_LD)
TELINK_B80_CPPFLAGS := $(TELINK_DEFINES)
TELINK_B80_LDFLAGS := --gc-sections $(TELINK_LDFLAGS)
TELINK_B80_LDLIBS := $(TELINK_ARCHIVES) $(TELINK_LIBGCC)

TELINK_SDK_SOURCES := $(addprefix $(TELINK_SDK)/,$(TELINK_SDK_USED))
TELINK_SDK_C_SOURCES := $(filter %.c,$(TELINK_SDK_SOURCES))
TELINK_SDK_S_SOURCES := $(filter %.S,$(TELINK_SDK_SOURCES))
# Our LE host uses BTstack's ATT database code only.
TELINK_BTSTACK_SOURCES := $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c
TELINK_BTSTACK_INCLUDES := -I src/platform/telink $(BTSTACK_INCLUDES)
# Our files' include directories, after the board's flags: SRC_INCLUDES
# without the Jieli platform, then this one and the board's (-iquote BOARD),
# then the SDK's and BTstack's.
TELINK_SRC_INCLUDES := $(subst -iquote src/platform/jieli,,$(SRC_INCLUDES)) -iquote src/platform/telink

# The SDK's linker script (first _deps), for a boot loader that copies the
# vectors and RAM code to SRAM but leaves the flash mapped at its physical
# addresses, linked at the target's telink_app_start.  The generic FSK
# driver's RAM code (1 KiB, genfsk_ll.o) goes to flash: we call it from the
# control loop only, never in an interrupt.  So does the 32k-RC sleep entry
# (1.2 KiB, ext_pm_32k_rc*.o): only deep sleep uses it, which never returns,
# and the step that stops the flash (sleep_start) stays in RAM.  Flash code
# and constants link at the app's offset, their image offsets stay relative
# to its start. Keep an alias to the SDK connection validator before its
# symbol is redirected to the app's Switch 2 interval shim.
define lib-type-telink-boot-link
sed
-e 's/\. = _start_text_;/. = _start_text_ + $(telink_app_start);/'
-e 's/\*(.text)/sdk_ll_procConnectReq = .; KEEP(*(.text.blc_ll_procConnectReq)) sdk_ll_push_tx_fifo = .; KEEP(*(.text.bls_ll_pushTxFifo)) *(.text) *liblt_8208*.a:genfsk_ll.o(.ram_code) *liblt_8208*.a:ext_pm_32k_rc*.o(.ram_code)/'
-e 's/^\( *\)\*(\.ram_code)$$/\1*(EXCLUDE_FILE(*liblt_8208*.a:genfsk_ll.o *liblt_8208*.a:ext_pm_32k_rc*.o) .ram_code)/'
-e 's/PROVIDE(_code_size_ = \.);/PROVIDE(_code_size_ = . - $(telink_app_start));/'
-e 's/AT ( _rstored_ )/AT ( _rstored_ - $(telink_app_start) )/'
-e 's/AT ( _sstored_ )/AT ( _sstored_ - $(telink_app_start) )/'
$(firstword $(lib_deps)) > $@
endef

# The ELF (first _deps) as the flash image.
lib-type-telink-bin = $(TELINK_OBJCOPY) -O binary $(firstword $(lib_deps)) $@

# The image (app.bin, first _deps) with its header: the stock stamp
# (telink_stamp) in its reserved bytes, the size and the CRC the boot loader
# checks; it may fill telink_app_start up to telink_app_end.
lib-type-telink-fw = $(JOLLYMAAM_CLI) pack telink --stamp $(telink_stamp) --max $$(( $(telink_app_end) - $(telink_app_start) )) -o $@ $(firstword $(lib_deps))
