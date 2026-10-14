# BOARD=pad (the Cyclone 2, the default) or BOARD=dongle (GameSir's dongle):
# the same platform with another board file and main loop.  Each board builds
# into $(O)/$(BOARD)/; host tools (built with HOST_CC, not the SDK) into $(O)/host/.
BOARDS := pad dongle
BOARD ?= pad
$(if $(filter $(BOARDS),$(BOARD)),,$(error BOARD must be one of: $(BOARDS)))

comma := ,
.DEFAULT_GOAL := all
SDK := /shared/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /shared/jieli-linux-toolchains-20250324.1/pi32v2/bin
HOST_CC := gcc
O := build
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers

# Cyclone 2 gamepad.
pad pad-% $(O)/pad/%: override BOARD := pad
FLASH_SIZE_pad := 0x100000
CODE_LIMIT_pad := 0x50000
PID_pad := GS_C2_ADC_DEVICE
APP_SOURCES_pad := $(filter-out src/board_%.c src/dongle%.c,$(wildcard src/*.c))

# GameSir dongle.
dongle dongle-% $(O)/dongle/%: override BOARD := dongle
FLASH_SIZE_dongle := 0x80000
CODE_LIMIT_dongle := 0x50000
PID_dongle := GS_C2_Dongle
BOARD_DEFINES_dongle := -DBOARD_DONGLE
APP_SOURCES_dongle := src/platform_jieli.c src/platform_jieli_sdk.c src/platform_jieli_usb.c \
	src/dongle.c src/dongle_bt.c src/dongle_protocol.c src/btstack_hci_jieli.c src/btstack_qos_jieli.c \
	src/usb.c src/utils.c src/input_imu.c \
	src/profile_link.c src/profile_switch.c src/profile_xbox.c src/profile_playstation.c src/profile_hid.c

FLASH_SIZE = $(FLASH_SIZE_$(BOARD))
# Note: changing CODE_LIMIT changes the location of settings/bonds storage
CODE_LIMIT = $(CODE_LIMIT_$(BOARD))
PID = $(PID_$(BOARD))
BOARD_DEFINES = $(BOARD_DEFINES_$(BOARD))

include sdk-br23.mk

ARCHIVES := $(SDK_ARCHIVES_MINIMAL)
CFLAGS := $(SDK_CFLAGS)
BUILD_ID := $(shell git describe --always --dirty 2>/dev/null)
BUILD_REF := $(shell git symbolic-ref -q HEAD)
BUILD_METADATA := $(shell git rev-parse --git-path HEAD --git-path index $(if $(BUILD_REF),--git-path $(BUILD_REF)))
DEFINES = $(SDK_DEFINES) -DCONFIG_FLASH_SIZE=$(FLASH_SIZE) $(BOARD_DEFINES) -DBUILD_ID=\"$(BUILD_ID)\"
INCLUDES = -iquote src -iquote src/sdkconfig $(BTSTACK_INCLUDES) $(SDK_INCLUDES)
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
	cpu/br23/mcpwm.c \
	cpu/br23/setup.c

# Upstream BTstack (master), unmodified. The owned controller exports a few
# of the same names; BTstack's copies are renamed.
BTSTACK := /work/cyclone2-btstack
BTSTACK_USED := \
	src/btstack_hid_parser.c src/btstack_linked_list.c src/btstack_memory.c \
	src/btstack_memory_pool.c src/btstack_run_loop.c src/btstack_run_loop_base.c \
	src/btstack_crypto.c src/btstack_tlv.c src/btstack_util.c src/hci.c src/hci_cmd.c \
	src/hci_event.c src/hci_dump.c src/l2cap.c src/l2cap_signaling.c src/ad_parser.c \
	src/classic/device_id_server.c src/classic/hid_device.c src/classic/sdp_server.c \
	src/classic/sdp_util.c src/ble/att_db.c src/ble/att_dispatch.c src/ble/att_server.c \
	src/ble/le_device_db_memory.c src/ble/sm.c
BTSTACK_RENAMES := -Dhci_send_cmd=btstack_hci_send_cmd \
	-Dhci_event_disconnection_complete=btstack_hci_event_disconnection_complete \
	-Dbig_endian_read_32=btstack_big_endian_read_32 \
	-Dbig_endian_store_32=btstack_big_endian_store_32 \
	-Dnibble_for_char=btstack_nibble_for_char
BTSTACK_INCLUDES := -I$(BTSTACK)/src -I$(BTSTACK)/src/classic

# Each output directory has its own dependency graph; recipes inherit BOARD
# from the target-specific assignments above, including when built directly.
OUTPUTS := $(foreach board,$(BOARDS),$(O)/$(board))
$(foreach board,$(BOARDS),$(eval APP_OBJECTS_$(board) := \
	$(patsubst %.c,$(O)/$(board)/%.o,src/board_$(board).c $(APP_SOURCES_$(board)) $(wildcard src/btctrler/*.c))))
APP_OBJECTS := $(foreach board,$(BOARDS),$(APP_OBJECTS_$(board)))
SDK_OBJECTS := $(foreach output,$(OUTPUTS),$(SDK_USED:%.c=$(output)/sdk/%.o))
BTSTACK_OBJECTS := $(foreach output,$(OUTPUTS),$(BTSTACK_USED:%.c=$(output)/btstack/%.o))
OBJECTS = $(APP_OBJECTS_$(BOARD)) $(SDK_USED:%.c=$(O)/$(BOARD)/sdk/%.o) $(BTSTACK_USED:%.c=$(O)/$(BOARD)/btstack/%.o)
ALL_OBJECTS := $(APP_OBJECTS) $(SDK_OBJECTS) $(BTSTACK_OBJECTS)
.SECONDEXPANSION:

ifeq ($(V),1)
Q :=
else
Q := @
endif

.PHONY: all clean flash test
all: $(BOARD)

clean:
	rm -rf $(O)

flash: $(BOARD)-flash
test: $(BOARD)-test

BOARD_ACTIONS := flash test
BOARD_TARGETS := $(foreach board,$(BOARDS),$(addprefix $(board)-,$(BOARD_ACTIONS)))
.PHONY: $(BOARDS) $(BOARD_TARGETS)
$(BOARDS): $$(O)/$$(BOARD)/firmware.img

# Ask running firmware to enter the ROM loader. For stock dongle firmware,
# hold its button while plugging in. flashpack uses the connected chip's key.
$(BOARDS:%=%-flash): $$(O)/$$(BOARD)/app.bin $$(O)/$$(BOARD)/isd_config.ini $(O)/host/jlnewfw
	$(Q)$(O)/host/jlnewfw flashpack --enter-loader --wait 30 --loader $(SDK_LOADER) \
		--flash-size $(FLASH_SIZE) --code-limit $(CODE_LIMIT) \
		-c $(O)/$(BOARD)/isd_config.ini -u $(SDK_UBOOT) $(O)/$(BOARD)/app.bin $(SDK_CONFIG_TOOL)

# The SDK flags include -w; owned controller C must not call undeclared
# functions all the same.
$(foreach output,$(OUTPUTS),$(eval $(output)/src/btctrler/%.o: CFLAGS += -Werror=implicit-function-declaration))
$(APP_OBJECTS): $$(patsubst $$(O)/$$(BOARD)/%.o,%.c,$$@)
	@mkdir -p $(@D)
	@echo [CC $(BOARD)] $<
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -c $< -o $@

$(SDK_OBJECTS): $$(patsubst $$(O)/$$(BOARD)/sdk/%.o,$$(SDK)/%.c,$$@)
	@mkdir -p $(@D)
	@echo [CC $(BOARD)] $<
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -c $< -o $@

$(BTSTACK_OBJECTS): $$(patsubst $$(O)/$$(BOARD)/btstack/%.o,$$(BTSTACK)/%.c,$$@)
	@mkdir -p $(@D)
	@echo [CC $(BOARD)] $<
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(BTSTACK_RENAMES) $(INCLUDES) -MMD -MP -c $< -o $@

$(foreach output,$(OUTPUTS),$(addprefix $(output)/src/,bt.o dongle_bt.o btstack_hci_jieli.o)): CFLAGS += $(BTSTACK_RENAMES)

$(OUTPUTS:%=%/app.elf): %/app.elf: %/sdk.ld $$(OBJECTS) $(ARCHIVES)
	@echo [LD $(BOARD)] $@
	$(Q)$(LD) -o $@ $(OBJECTS) -T$(O)/$(BOARD)/sdk.ld -M=$(O)/$(BOARD)/app.map $(SDK_LFLAGS) \
		--start-group $(ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

$(OUTPUTS:%=%/app.bin): %/app.bin: %/app.elf
	@echo [BIN] $@
	$(Q)$(OBJCOPY) -O binary -j .text $< $(O)/$(BOARD)/text.bin
	$(Q)$(OBJCOPY) -O binary -j .data $< $(O)/$(BOARD)/data.bin
	$(Q)cat $(O)/$(BOARD)/text.bin $(O)/$(BOARD)/data.bin > $@

$(OUTPUTS:%=%/firmware.img): %/firmware.img: %/app.bin %/isd_config.ini $(O)/host/jlnewfw
	@echo [FW] $@
	$(Q)$(O)/host/jlnewfw pack --flash-size $(FLASH_SIZE) --code-limit $(CODE_LIMIT) \
		-c $(O)/$(BOARD)/isd_config.ini -u $(SDK_UBOOT) -o $@ $(O)/$(BOARD)/app.bin $(SDK_CONFIG_TOOL)

$(O)/host/jlnewfw: tools/jlnewfw.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# The SDK's linker script, with the heap ending below the retained RAM and the
# controller's RAM kept clear of the MaskROM boot token.
$(OUTPUTS:%=%/sdk_ld.c): $(SDK)/cpu/br23/sdk_ld.c Makefile $(wildcard src/sdkconfig/*.h)
	@mkdir -p $(@D)
	$(Q)sed \
	  -e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
	  -e '/^    \.bss ALIGN(32) :$$/,/^        \*(\.bss)$$/s/^        \*(\.bss)$$/        cyclone2_bss_base = ABSOLUTE(.);\n&/' \
	  -e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - cyclone2_bss_base);\n&|' \
		$< > $@

$(OUTPUTS:%=%/sdk.ld): %/sdk.ld: %/sdk_ld.c
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@

# PID: what jlnewfw flash checks before writing (a gamepad or a dongle).
$(OUTPUTS:%=%/isd_config.ini): $(SDK)/cpu/br23/tools/isd_config_rule.c $(wildcard src/sdkconfig/*.h) Makefile
	@mkdir -p $(@D)
	$(Q)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@
	$(Q)sed -i -e /^$$/d -e 's/^PID = .*;/PID = $(PID);/' $@

# Host unit tests: every source with a UNITTEST block.
TESTS := $(shell grep -l 'ifdef UNITTEST' src/*.c src/btctrler/*.c)
TEST_BINARIES := $(foreach board,$(BOARDS),$(TESTS:src/%.c=$(O)/host/test-$(board)/%))
$(BOARDS:%=%-test): $$(patsubst src/%.c,$(O)/host/test-$$(BOARD)/%,$$(TESTS))
	$(Q)for t in $^; do $$t || exit 1; done

$(TEST_BINARIES): $$(patsubst $(O)/host/test-$$(BOARD)/%,src/%.c,$$@) src/pad.h src/dongle.h src/test.h $(wildcard src/btctrler/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(BOARD_DEFINES) -DUNITTEST -Isrc -iquote src $< -o $@ \
	  $(if $(findstring /btctrler/,$@),-no-pie -Wl$(comma)--unresolved-symbols=ignore-all)

$(OUTPUTS:%=%/src/usb.o): $(wildcard $(BUILD_METADATA))

$(ALL_OBJECTS) $(OUTPUTS:%=%/sdk.ld): Makefile sdk-br23.mk $(wildcard src/sdkconfig/*)

-include $(ALL_OBJECTS:.o=.d)
