O := build
.DEFAULT_GOAL := all
SDK := /shared/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /shared/jieli-linux-toolchains-20250324.1/pi32v2/bin
# Must match CONFIG_FLASH_SIZE in src/sdkconfig/board_config.h.
FLASH_SIZE := 0x100000
# Fixes the VM boundary; moving it moves the settings store.
CODE_LIMIT := 0x50000
PYTHON := /work/bin/python3
HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function

include sdk-br23.mk

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES) -DBUILD_ID=\"$(shell git rev-parse --short HEAD 2>/dev/null)\"
INCLUDES := -iquote src -iquote src/sdkconfig $(SDK_INCLUDES)

SDK_USED := \
	apps/common/debug/debug.c \
	apps/common/device/usb/device/descriptor.c \
	apps/common/device/usb/device/msd.c \
	apps/common/device/usb/device/usb_device.c \
	apps/common/device/usb/device/user_setup.c \
	apps/common/device/usb/usb_config.c \
	cpu/br23/adc_api.c \
	cpu/br23/charge.c \
	cpu/br23/clock_manager.c \
	cpu/br23/led_spi.c \
	cpu/br23/setup.c

LLVM_AR := $(TOOL_DIR)/../../common/bin/llvm-ar
VENDOR_BTCTRLR := $(SDK)/cpu/br23/liba/btctrler.a
PATCHED_BTCTRLR := $O/sdk-libs/btctrler.a
ARCHIVES := $(subst $(VENDOR_BTCTRLR),$(PATCHED_BTCTRLR),$(SDK_ARCHIVES_MINIMAL))

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:%.c=$O/%.o) $(SDK_USED:%.c=$O/sdk/%.o)

ifeq ($(V),1)
Q :=
else
Q := @
endif

.PHONY: all clean flash test
all: $O/firmware.img

flash: $O/firmware.img
	$(Q)$(PYTHON) tools/padtool.py flash $<

clean:
	rm -rf $O

$(OBJECTS) $O/sdk.ld: Makefile sdk-br23.mk $(wildcard src/sdkconfig/*)

$O/src/%.o: src/%.c
	@mkdir -p $(@D)
	@echo [CC] $<
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -c $< -o $@

$O/sdk/%.o: $(SDK)/%.c
	@mkdir -p $(@D)
	@echo [CC] $<
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -c $< -o $@

$O/app.elf: $O/sdk.ld $(OBJECTS) $(ARCHIVES)
	@echo [LD] $@
	$(Q)$(LD) -o $@ $(OBJECTS) -T$O/sdk.ld -M=$O/app.map $(SDK_LFLAGS) \
		--start-group $(ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

$O/app.bin: $O/app.elf
	@echo [BIN] $@
	$(Q)$(OBJCOPY) -O binary -j .text $< $O/text.bin
	$(Q)$(OBJCOPY) -O binary -j .data $< $O/data.bin
	$(Q)cat $O/text.bin $O/data.bin > $@

$O/firmware.img: $O/app.bin $O/isd_config.ini $O/jlnewfw
	@echo [FW] $@
	$(Q)$O/jlnewfw pack --flash-size $(FLASH_SIZE) --code-limit $(CODE_LIMIT) \
		-c $O/isd_config.ini -u $(SDK_UBOOT) -o $@ $O/app.bin $(SDK_CONFIG_TOOL)

$O/jlnewfw: tools/jlnewfw.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(PATCHED_BTCTRLR): $(VENDOR_BTCTRLR) tools/patch_btctrler.py src/build_config.h
	@mkdir -p $(@D)
	@echo [PATCH] $@
	$(Q)$(PYTHON) tools/patch_btctrler.py $< $@ --clang $(CC) --ar $(LLVM_AR) --config src/build_config.h

# The SDK's linker script, with the heap ending below the retained RAM and the
# controller's RAM kept clear of the MaskROM boot token.
$O/sdk_ld.c: $(SDK)/cpu/br23/sdk_ld.c Makefile
	@mkdir -p $(@D)
	$(Q)sed \
	  -e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
	  -e '/^    \.bss ALIGN(32) :$$/,/^        \*(\.bss)$$/s/^        \*(\.bss)$$/        cyclone2_bss_base = ABSOLUTE(.);\n&/' \
	  -e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - cyclone2_bss_base);\n&|' \
		$< > $@

$O/sdk.ld: $O/sdk_ld.c
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@

$O/isd_config.ini: $(SDK)/cpu/br23/tools/isd_config_rule.c
	@mkdir -p $(@D)
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@
	$(Q)sed -i -e /^$$/d $@

# Host unit tests: every src/*.c with a UNITTEST block.
TESTS := $(shell grep -l 'ifdef UNITTEST' src/*.c)
test: $(TESTS:src/%.c=$O/test/%)
	$(Q)for t in $^; do $$t || exit 1; done

$O/test/%: src/%.c src/pad.h src/test.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DUNITTEST -Isrc $< -o $@

-include $(OBJECTS:.o=.d)
