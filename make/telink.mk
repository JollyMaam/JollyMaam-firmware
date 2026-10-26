# The Telink B80 (TLSR8208): toolchain, SDK and the rules that build a board
# on it.  The compiler ABI is the SDK's own (its Eclipse project's flags); we
# never include a vendor Makefile and build the SDK in place, unmodified.  A
# product file sets a board's variables, then calls $(call telink_board,NAME)
# (at the end of this file).

TELINK_SDK := external/telink-tc_ble_simple_sdk/b80_ble_sdk
TELINK_TOOLS := external/toolchains/tc32_gcc_v2.0/bin
TELINK_CC := $(TELINK_TOOLS)/tc32-elf-gcc
TELINK_LD := $(TELINK_TOOLS)/tc32-elf-ld
TELINK_OBJCOPY := $(TELINK_TOOLS)/tc32-elf-objcopy
TELINK_LIBGCC := $(shell $(TELINK_CC) -print-libgcc-file-name 2>/dev/null)
TELINK_IMAGE_TOOL := tools/8bitdoboot.py

# The SDK's sources build with -fpack-struct; ours do not, and see the SDK's
# headers only through src/platform/telink/sdk.h, which packs them the same
# way.  Everything else in the ABI (short enums, unsigned char) is shared.
TELINK_ABI :=
TELINK_ABI += -std=gnu99
TELINK_ABI += -fshort-enums
TELINK_ABI += -fshort-wchar
TELINK_ABI += -fms-extensions
TELINK_ABI += -ffunction-sections
TELINK_ABI += -fdata-sections
TELINK_ABI += -fcommon

# -O2, as the SDK builds, never -Os: at -Os this GCC (4.5 tc32) writes a dense
# switch's jump table as 16-bit offsets after the jump but loads 32-bit
# addresses from it, so the switch jumps anywhere.  Our char is signed
# (pad.h's s8); the SDK's stays unsigned, as it builds.
TELINK_SDK_CFLAGS := $(TELINK_ABI) -O2 -fpack-struct -finline-small-functions
TELINK_CFLAGS := $(TELINK_ABI) -O2 -fsigned-char -mstructure-size-boundary=8 -g -Wall -Wno-unused-function -Wno-missing-field-initializers \
	-Wframe-larger-than=256

TELINK_DEFINES :=
TELINK_DEFINES += -DMCU_CORE_B80=1
TELINK_DEFINES += -DMCU_STARTUP_FLASH
TELINK_DEFINES += -DMCU_STARTUP_B80=1
# No RAM survives a reset that the app could use for the log.
TELINK_DEFINES += -DRETAINED=

TELINK_INCLUDES :=
TELINK_INCLUDES += -I$(TELINK_SDK)
TELINK_INCLUDES += -I$(TELINK_SDK)/drivers
TELINK_INCLUDES += -I$(TELINK_SDK)/common
TELINK_INCLUDES += -I$(TELINK_SDK)/vendor/common

# Our SDK configuration (what vendor/*/app_config.h is to the SDK's samples),
# read before anything else by every SDK and app file.
TELINK_APP_CONFIG := src/platform/telink/sdkconfig/app_config.h

# The SDK's strncpy (common/string.c) copies n bytes whatever the string's
# length; libc_telink.c has the standard one.
TELINK_LDFLAGS := --wrap=strncpy

TELINK_ARCHIVES :=
TELINK_ARCHIVES += $(TELINK_SDK)/proj_lib/liblt_8208.a

# The SDK sources the app uses.
TELINK_SDK_USED :=
TELINK_SDK_USED += boot/cstartup_flash.S
TELINK_SDK_USED += common/sdk_version.c
TELINK_SDK_USED += common/string.c
TELINK_SDK_USED += common/utility.c
TELINK_SDK_USED += drivers/adc.c
TELINK_SDK_USED += drivers/analog.c
TELINK_SDK_USED += drivers/bsp.c
TELINK_SDK_USED += drivers/clock.c
TELINK_SDK_USED += drivers/driver_ext/ext_calibration.c
TELINK_SDK_USED += drivers/driver_ext/ext_misc.c
TELINK_SDK_USED += drivers/driver_ext/rf_pa.c
TELINK_SDK_USED += drivers/flash.c
TELINK_SDK_USED += drivers/flash/flash_common.c
TELINK_SDK_USED += drivers/flash/flash_mid114485.c
TELINK_SDK_USED += drivers/flash/flash_mid1160c8.c
TELINK_SDK_USED += drivers/flash/flash_mid1164c8.c
TELINK_SDK_USED += drivers/flash/flash_mid13325e.c
TELINK_SDK_USED += drivers/flash/flash_mid136085.c
TELINK_SDK_USED += drivers/flash/flash_mid1360c8.c
TELINK_SDK_USED += drivers/gpio.c
TELINK_SDK_USED += drivers/timer.c
TELINK_SDK_USED += drivers/usbhw.c
TELINK_SDK_USED += vendor/common/ble_flash.c
TELINK_SDK_USED += vendor/common/flash_prot.c

# $(call telink_board,NAME): build NAME on the B80, into $(O)/NAME/.  Set first:
#   NAME_FLASH_SIZE  the flash device's size
#   NAME_APP_START   where the boot loader starts the app (its image header)
#   NAME_APP_END     the first byte past what the app may write
#   NAME_PID         the firmware names itself by it (BOARD_PID); the flasher
#                    writes only over firmware that answers with the same one
#   NAME_STAMP       hex bytes for image header offsets 2..7 (the stock image's)
#   NAME_SOURCES     the app's sources
#   NAME_DEFINES     -D flags for this board (EXTRA_DEFINES adds to them)
#   NAME_BOARD_DIR   the board's headers
#   NAME_LDFLAGS     extra linker flags (--wrap=...)
#   NAME_LOADER_HINT how to enter the boot loader by hand
# Targets: NAME (firmware.bin), NAME-flash (through the 8BitDo boot loader),
# NAME-loader (into the boot loader and wait there).  Inside the template,
# $(1) is NAME, $(...) is expanded once when it is called, and $$(...) when
# the rule runs.
define telink_board
$(1)_OBJECTS := $(patsubst %.c,$(O)/$(1)/%.o,$($(1)_SOURCES)) \
	$(patsubst %,$(O)/$(1)/sdk/%.o,$(basename $(TELINK_SDK_USED))) \
	$(O)/$(1)/btstack/src/ble/att_db.o $(O)/$(1)/btstack/src/btstack_util.o
$(1)_FLAGS := $(TELINK_DEFINES) -DBOARD_PID=\"$($(1)_PID)\" -DAPP_START=$($(1)_APP_START) \
	$($(1)_DEFINES) $(EXTRA_DEFINES) -DBUILD_ID=\"$(BUILD_ID)\" -include $(TELINK_APP_CONFIG)
$(1)_INCLUDES := $(SRC_INCLUDES) -iquote src/platform/telink -iquote $($(1)_BOARD_DIR) $(TELINK_INCLUDES) $(BTSTACK_INCLUDES)

.PHONY: $(1) $(1)-flash $(1)-loader
$(1): $(O)/$(1)/firmware.bin

$(O)/$(1)/src/%.o: src/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(TELINK_CC) $(TELINK_CFLAGS) $$($(1)_FLAGS) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/sdk/%.o: $(TELINK_SDK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(TELINK_CC) $(TELINK_SDK_CFLAGS) $$($(1)_FLAGS) $(TELINK_INCLUDES) -w -MMD -MP -c $$< -o $$@

$(O)/$(1)/sdk/%.o: $(TELINK_SDK)/%.S $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [AS $(1)] $$<
	$$(Q)$(TELINK_CC) $(TELINK_SDK_CFLAGS) $$($(1)_FLAGS) $(TELINK_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/btstack/%.o: $(BTSTACK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(TELINK_CC) $(TELINK_CFLAGS) $$($(1)_FLAGS) $(BTSTACK_RENAMES) \
	  -I src/platform/telink $(BTSTACK_INCLUDES) -MMD -MP -c $$< -o $$@

# The build ID: rebuilt when git moves.
$(O)/$(1)/src/core/usb.o: $(wildcard $(BUILD_METADATA))

$(O)/$(1)/app.elf: $(O)/$(1)/boot.link $$($(1)_OBJECTS) $(TELINK_ARCHIVES)
	@echo [LD $(1)] $$@
	$$(Q)$(TELINK_LD) --gc-sections $(TELINK_LDFLAGS) $($(1)_LDFLAGS) -T $(O)/$(1)/boot.link -Map $(O)/$(1)/app.map -o $$@ \
		$$($(1)_OBJECTS) $(TELINK_ARCHIVES) $(TELINK_LIBGCC)

# The image header: the stock stamp in its reserved bytes, the size and the
# CRC the boot loader checks.
$(O)/$(1)/firmware.bin: $(O)/$(1)/app.elf $(TELINK_IMAGE_TOOL)
	@echo [FW] $$@
	$$(Q)$(TELINK_OBJCOPY) -O binary $$< $(O)/$(1)/app.bin
	$$(Q)python3 -I $(TELINK_IMAGE_TOOL) pack --stamp $($(1)_STAMP) \
		--max $$$$(( $($(1)_APP_END) - $($(1)_APP_START) )) -o $$@ $(O)/$(1)/app.bin

$(1)-flash: $(O)/$(1)/firmware.bin
	$$(Q)python3 -I $(TELINK_IMAGE_TOOL) flash --pid $($(1)_PID) --start $($(1)_APP_START) \
		--end $($(1)_APP_END) --hint "$($(1)_LOADER_HINT)" $$(if $$(DEVICE),--device $$(DEVICE)) $$<

$(1)-loader:
	$$(if $$($(1)_LOADER_HINT),@echo "$$($(1)_LOADER_HINT)")
	$$(Q)python3 -I $(TELINK_IMAGE_TOOL) loader --pid $($(1)_PID) $$(if $$(DEVICE),--device $$(DEVICE))

# The SDK's linker script, for a boot loader that copies the vectors and RAM
# code to SRAM but leaves the flash mapped at its physical addresses: flash
# code and constants link at the app's offset, their image offsets stay
# relative to its start. Keep an alias to the SDK connection validator
# before its symbol is redirected to the app's Switch 2 interval shim.
$(O)/$(1)/boot.link: $(TELINK_SDK)/project/tlsr_tc32/B80/boot.link $(MAKE_FILES)
	@mkdir -p $$(@D)
	$$(Q)sed \
	  -e 's/\. = _start_text_;/. = _start_text_ + $($(1)_APP_START);/' \
	  -e 's/\*(.text)/sdk_ll_procConnectReq = .; KEEP(*(.text.blc_ll_procConnectReq)) sdk_ll_push_tx_fifo = .; KEEP(*(.text.bls_ll_pushTxFifo)) *(.text)/' \
	  -e 's/PROVIDE(_code_size_ = \.);/PROVIDE(_code_size_ = . - $($(1)_APP_START));/' \
	  -e 's/AT ( _rstored_ )/AT ( _rstored_ - $($(1)_APP_START) )/' \
	  -e 's/AT ( _sstored_ )/AT ( _sstored_ - $($(1)_APP_START) )/' \
		$$< > $$@

$$($(1)_OBJECTS): $(MAKE_FILES) $(TELINK_APP_CONFIG)
$(call board_defines,$(1))
-include $$($(1)_OBJECTS:.o=.d)
endef
