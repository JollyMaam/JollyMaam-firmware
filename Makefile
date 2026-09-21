O := build
.DEFAULT_GOAL := default
SDK := /shared/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /shared/jieli-linux-toolchains-20250324.1/pi32v2/bin
FLASH_SIZE := 0x80000
CODE_LIMIT := 0x40000
# Use the broad controller-library workaround only when explicitly requested.
PATCH_BTCTLER ?= 0

include sdk-br23.mk

HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES)
SDK_ARCHIVES := $(SDK_ARCHIVES_MINIMAL)
SDK_SOURCES := $(SDK_SOURCES_MINIMAL) \
	$(SDK)/cpu/br23/mcpwm.c \
	$(SDK)/cpu/br23/uart_dev.c
SDK_COMMON_SOURCES := $(SDK_COMMON_SOURCES_MINIMAL)

SDK_COMMON_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_COMMON_SOURCES:.c=.c.o))
SDK_CPU_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_SOURCES:.c=.c.o))

ifeq ($(PATCH_BTCTLER),1)
# Build a patched copy without modifying the vendor SDK installation.
LLVM_AR := $(TOOL_DIR)/../../common/bin/llvm-ar
VENDOR_BTCTRLR := $(SDK)/cpu/br23/liba/btctrler.a
PATCHED_BTCTRLR := $O/sdk-libs/btctrler.a
SDK_ARCHIVES := $(subst $(VENDOR_BTCTRLR),$(PATCHED_BTCTRLR),$(SDK_ARCHIVES_MINIMAL))
endif

INCLUDES := -iquote src -iquote src/bt -iquote src/sdkconfig -iquote $(O) $(SDK_INCLUDES)
PROJECT_SOURCES := $(wildcard src/*.c src/bt/*.c src/sdkconfig/*.c)
APP_CONFIG_DEPS := $(wildcard src/sdkconfig/*.h src/sdkconfig/*.ld)

OBJECTS := $(SDK_COMMON_OBJECTS) $(addprefix $O/,$(PROJECT_SOURCES:.c=.c.o)) $(SDK_CPU_OBJECTS)
DEPFILES := $(addsuffix .d,$(OBJECTS))

V?=0
ifneq ($(V),0)
QUIET=
else
QUIET=@
endif

.PHONY: all default clean flash

# Make must notice when the caller switches between the standard and patched
# controller-library variants; the value of a command-line variable alone is
# not part of a target's timestamp dependencies.
PATCH_MODE_STAMP := $O/.patch_btctler_mode
.PHONY: FORCE
FORCE:

$(PATCH_MODE_STAMP): FORCE
	@mkdir -p $(@D)
	@if [ ! -f "$@" ] || [ "$$(cat "$@")" != "$(PATCH_BTCTLER)" ]; then \
		printf '%s\n' "$(PATCH_BTCTLER)" > "$@"; \
	fi

default all: $O/firmware.img

# Write the packed image through the controller's UBOOT1.00 loader, keeping a
# full-flash backup.  See tools/flash.sh for the details and the recovery path.
flash: $O/firmware.img
	$(QUIET)tools/flash.sh

# regenerate stuff if makefile or config changes
$(OBJECTS) $O/sdk.ld: Makefile sdk-br23.mk $(APP_CONFIG_DEPS)

define compile_c
	@mkdir -p $(@D)
	@echo [CC] $<
	$(QUIET)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -MF $@.d -c $< -o $@
endef

define compile_pre_build
	@mkdir -p $(@D)
	@echo [PRECC] $@
	$(QUIET) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@
endef

$O/firmware.img: $O/isd_config.ini $O/app.bin $O/jlnewfw
	@echo [FW] $@
	$(QUIET)$O/jlnewfw pack --flash-size $(FLASH_SIZE) --code-limit $(CODE_LIMIT) -c $O/isd_config.ini -u $(SDK_UBOOT) -o $@ $O/app.bin $(SDK_CONFIG_TOOL)

$O/jlnewfw: tools/jlnewfw.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$O/jlcfg: tools/jlcfg.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$O/c2ctl: tools/c2ctl.c
	@mkdir -p $(@D)
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# The binary blobs in data/ are the source of truth for the Switch protocol
# tables; bin2header only spells their bytes out as C arrays.
$O/bin2header: tools/bin2header.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$O/nintendo_switch_pro_hid.h: $O/bin2header data/nintendo_switch_pro_hid.bin
	@mkdir -p $(@D)
	@echo [GEN] $@
	$(QUIET)$O/bin2header data/nintendo_switch_pro_hid.bin $@ \
		nintendo_switch_pro_hid_report_descriptor

$O/nintendo_switch_pro_spi.h: $O/bin2header data/nintendo_switch_pro_spi.bin
	@mkdir -p $(@D)
	@echo [GEN] $@
	$(QUIET)$O/bin2header data/nintendo_switch_pro_spi.bin $@ \
		nintendo_switch_pro_spi_flash

# minimal version of download.sh
$O/app.bin: $O/app.elf
	@echo [BIN] $@
	@mkdir -p $(@D)/sections
	$(QUIET)for i in text data; do $(OBJCOPY) -O binary -j .$$i $< $O/sections/$$i.bin; done
	$(QUIET)for i in aec wav ape flac m4a amr dts fm mp3 wma; do $(OBJCOPY) -O binary -j .overlay_$$i $< $O/sections/$$i.bin; done
	$(QUIET)cd $O/sections && cat text.bin data.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin fm.bin mp3.bin wma.bin > ../app.bin

$O/app.elf: $O/sdk.ld $(OBJECTS) $(SDK_ARCHIVES) $(PATCH_MODE_STAMP)
	@mkdir -p $(@D)
	@echo [LD] $@
	$(QUIET)$(LD) -o $@ $(OBJECTS) -T$O/sdk.ld -M=$O/app.map $(SDK_LFLAGS) --start-group $(SDK_ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

ifeq ($(PATCH_BTCTLER),1)
$(PATCHED_BTCTRLR): $(VENDOR_BTCTRLR) tools/patch_btctrler.py
	@mkdir -p $(@D)
	@echo [PATCH] $@
	$(QUIET)/work/bin/python3 tools/patch_btctrler.py $(VENDOR_BTCTRLR) $@ --clang $(CC) --ar $(LLVM_AR)
endif

$O/sdk_ld.c: $(SDK)/cpu/br23/sdk_ld.c
	@mkdir -p $(@D)
	$(QUIET)sed \
	  -e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
		$< > $@

$O/sdk.ld: $O/sdk_ld.c $(APP_CONFIG_DEPS)
	$(compile_pre_build)

$O/sdk_used_list.used: $(SDK)/cpu/br23/sdk_used_list.c $(APP_CONFIG)
	$(compile_pre_build)

$O/isd_config.ini: $(SDK)/cpu/br23/tools/isd_config_rule.c $(APP_CONFIG)
	$(compile_pre_build)
	$(QUIET)sed -i -e /^$$/d "$@"

$O/src/%.c.o: src/%.c
	$(compile_c)

# profile_switch.c uses the arrays generated from data/*.bin
$O/src/profile_switch.c.o: $O/nintendo_switch_pro_hid.h $O/nintendo_switch_pro_spi.h

$O/sdk/%.c.o: $(SDK)/%.c
	$(compile_c)

# Fix the SDK's one-way DTR/RTS latch and avoid resetting bulk endpoint DATA
# toggles whenever Linux opens/closes CDC. Patch a build copy only.
$O/cdc.c: $(SDK)/apps/common/device/usb/device/cdc.c tools/patch_cdc.py Makefile
	@mkdir -p $(@D)
	$(QUIET)/work/bin/python3 tools/patch_cdc.py $< $@

$O/sdk/apps/common/device/usb/device/cdc.c.o: $O/cdc.c
	$(compile_c)

clean:
	rm -rf "$O"

-include $(DEPFILES)
