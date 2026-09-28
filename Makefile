O := build
.DEFAULT_GOAL := default
SDK := /shared/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /shared/jieli-linux-toolchains-20250324.1/pi32v2/bin
# Upstream BTstack checkout that carries the Jieli BR23 port.  Its host
# replaces the vendor cpu/br23/liba/btstack.a; the controller library
# (btctrler.a) stays.
BTSTACK_DIR := /work/btstack-jl-port
BTSTACK_PORT_DIR := $(BTSTACK_DIR)/port/jieli-br23
# Must match CONFIG_FLASH_SIZE in src/sdkconfig/board_config.h and the chip.
# The packer derives the reserved-region addresses from this: with 0x80000 the
# BTIF (MAC/RF trim) and key/mac areas land at 0x7e000/0x7f000, but the
# controller and the factory data use the 1 MiB positions 0xfe000/0xff000.
FLASH_SIZE := 0x100000
CODE_LIMIT := 0x50000
# The controller-archive patch mode lives in src/build_config.h, not here.
BUILD_CONFIG := src/build_config.h
# DEBUG_LOG=1 builds a copy of the SDK's debug.c that forwards the controller
# library's LMP/HCI tracing to the USB console.  See tools/patch_debug.py.
DEBUG_LOG ?= 0

include sdk-br23.mk

HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic
PYTHON ?= python3

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES)
SDK_ARCHIVES := $(SDK_ARCHIVES_MINIMAL)
SDK_SOURCES := $(SDK_SOURCES_MINIMAL) \
	$(SDK)/cpu/br23/mcpwm.c
SDK_COMMON_SOURCES := $(SDK_COMMON_SOURCES_MINIMAL)

SDK_COMMON_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_COMMON_SOURCES:.c=.c.o))
SDK_CPU_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_SOURCES:.c=.c.o))

# Always link a copy of the controller archive, patched as build_config.h
# says ("none" is an unmodified copy); the vendor SDK is never modified.
LLVM_AR := $(TOOL_DIR)/../../common/bin/llvm-ar
VENDOR_BTCTRLR := $(SDK)/cpu/br23/liba/btctrler.a
PATCHED_BTCTRLR := $O/sdk-libs/btctrler.a
SDK_ARCHIVES := $(subst $(VENDOR_BTCTRLR),$(PATCHED_BTCTRLR),$(SDK_ARCHIVES_MINIMAL))

ifneq ($(filter 1 verbose,$(DEBUG_LOG)),)
DEFINES += -DCYCLONE2_LOG_TAGS_VERBOSE=1
endif
ifeq ($(DEBUG_LOG),verbose)
DEFINES += -DCYCLONE2_LOG_TAGS_VERBOSE_VERBOSE=1
endif

INCLUDES := -iquote src -iquote src/bt -iquote src/sdkconfig -iquote src/btstack \
	-iquote $(O) \
	-I$(BTSTACK_DIR)/src -I$(BTSTACK_DIR)/src/classic -I$(BTSTACK_PORT_DIR) \
	$(SDK_INCLUDES)
PROJECT_SOURCES := $(wildcard src/*.c src/bt/*.c src/sdkconfig/*.c)
APP_CONFIG_DEPS := $(wildcard src/sdkconfig/*.h src/sdkconfig/*.ld)

# Upstream BTstack host sources.  The exclusions are the pieces that need the
# 3rd-party codecs (SBC/LC3/MD5) that this firmware does not build.
BTSTACK_SOURCES := \
	$(addprefix $(BTSTACK_DIR)/src/, \
		ad_parser.c btstack_hid_parser.c btstack_linked_list.c \
		btstack_memory.c btstack_memory_pool.c btstack_run_loop.c \
		btstack_crypto.c btstack_tlv.c btstack_util.c hci.c hci_cmd.c hci_event.c \
		hci_dump.c l2cap.c \
		l2cap_signaling.c) \
	$(addprefix $(BTSTACK_DIR)/src/classic/, \
		device_id_server.c hid_device.c sdp_server.c sdp_util.c) \
	$(addprefix $(BTSTACK_DIR)/src/ble/, \
		att_db.c att_dispatch.c att_server.c le_device_db_memory.c sm.c) \
	$(wildcard $(BTSTACK_PORT_DIR)/*.c)
BTSTACK_OBJECTS := $(subst $(BTSTACK_DIR),$O/btstack,$(BTSTACK_SOURCES:.c=.c.o))

# btctrler.a and the Jieli runtime already export a few names that upstream
# BTstack also defines (its own hci_send_cmd, the HCI event tables and a few
# byte helpers).  Rename BTstack's copies so both libraries can be linked; the
# controller keeps its own symbols for its internal users.
DROP_BTCTRLER_COMMANDS ?= 0
# L2CAP bring-up instrumentation in btstack's l2cap.c
L2CAP_TRACE ?= 0
# Controller packet/L2CAP bring-up tracing in the port's bridge
TRACE_PACKETS ?= 0
BTSTACK_DEBUG := -DJIELI_BTCTRLER_DROP_COMMANDS=$(DROP_BTCTRLER_COMMANDS) -DJIELI_BTCTRLER_L2CAP_TRACE=$(L2CAP_TRACE) -DJIELI_BTCTRLER_TRACE_PACKETS=$(TRACE_PACKETS)

BTSTACK_RENAMES := \
	-Dhci_send_cmd=btstack_hci_send_cmd \
	-Dhci_event_disconnection_complete=btstack_hci_event_disconnection_complete \
	-Dbig_endian_read_32=btstack_big_endian_read_32 \
	-Dbig_endian_store_32=btstack_big_endian_store_32 \
	-Dnibble_for_char=btstack_nibble_for_char

OBJECTS := $(SDK_COMMON_OBJECTS) $(addprefix $O/,$(PROJECT_SOURCES:.c=.c.o)) \
	$(SDK_CPU_OBJECTS) $(BTSTACK_OBJECTS)
DEPFILES := $(addsuffix .d,$(OBJECTS))

V?=0
ifneq ($(V),0)
QUIET=
else
QUIET=@
endif

.PHONY: all default clean flash test itest rebootwatch wake-vectors calibration-test config-test aes128-test

# Make must notice when the caller switches DEBUG_LOG; the value of a
# command-line variable alone is not part of a target's timestamp
# dependencies.
DEBUG_MODE_STAMP := $O/.debug_log_mode
.PHONY: FORCE
FORCE:

$(DEBUG_MODE_STAMP): FORCE
	@mkdir -p $(@D)
	@if [ ! -f "$@" ] || [ "$$(cat "$@")" != "$(DEBUG_LOG)" ]; then \
		printf '%s\n' "$(DEBUG_LOG)" > "$@"; \
	fi

default all: $O/firmware.img

# Write the packed image through the controller's UBOOT1.00 loader, keeping a
# full-flash backup.  See tools/flash.sh for the details and the recovery path.
flash: $O/firmware.img
	$(QUIET)tools/flash.sh

# Exercise the firmware that is running on the attached pad: profile switches
# across all four slots, the pairing -> disconnected -> connecting cycle, rapid
# bursts that land mid-transition, and a dwell past the connect timeout.  The
# test fails if the console stops answering or the chip's boot counter moves.
# It needs the pad plugged in, and it does not build or flash anything.
#   make test                       default run
#   make test TEST_ROUNDS=6         more repetitions
#   make test TEST_FLAGS=--quick    skip the 33 s timeout dwell
TEST_ROUNDS ?= 2
TEST_FLAGS ?=
test:
	$(QUIET)$(PYTHON) tools/stress.py --rounds $(TEST_ROUNDS) $(TEST_FLAGS)

# Integration test: dump the flash, flash the image, switch its store to RAM
# mode, pair with a real console, read the flash back and require it to be byte-identical above the
# application image.  If anything else moved, the original dump is written back
# and the test fails.  It prompts for the pairing, and it flashes the pad.
#   make itest                          full run
#   make itest ITEST_FLAGS=--skip-pair  machinery only, no console needed
ITEST_FLAGS ?=
itest:
	$(QUIET)tools/itest.sh $(ITEST_FLAGS)

# Watch the pad for chip resets and report the reset-source registers each
# time, so a reset under test can be told apart from a tooling artefact.
#   make rebootwatch
#   make rebootwatch WATCH_FLAGS="--seconds 600"
WATCH_FLAGS ?=
rebootwatch:
	$(QUIET)$(PYTHON) tools/rebootwatch.py $(WATCH_FLAGS)

# regenerate stuff if makefile or config changes
$(OBJECTS) $O/sdk.ld: Makefile sdk-br23.mk $(DEBUG_MODE_STAMP) $(APP_CONFIG_DEPS)

define compile_c
	@mkdir -p $(@D)
	@echo [CC] $<
	$(QUIET)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -MF $@.d -c $< -o $@
endef

define compile_btstack_c
	@mkdir -p $(@D)
	@echo [CC] $<
	$(QUIET)$(CC) $(CFLAGS) $(DEFINES) $(BTSTACK_RENAMES) $(BTSTACK_DEBUG) $(INCLUDES) -MMD -MP -MF $@.d -c $< -o $@
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

# The binary blobs in src/*.bin are the source of truth for the Switch protocol
# tables; bin2header only spells their bytes out as C arrays.
$O/bin2header: tools/bin2header.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# Host check for the wake-up access code and payload maths.  The two modules
# it covers are deliberately free of SDK includes so they can be built and run
# on the host; see docs/radio.md.
$O/wake_vectors: tools/wake_vectors.c src/profile_switch_wake_code.c src/profile_switch_wake_payload.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -Isrc $^ -o $@

wake-vectors: $O/wake_vectors
	$(QUIET)$O/wake_vectors

# Host check for the stick and trigger calibration maths, which is kept free
# of SDK includes for this.
$O/calibration_test: tools/calibration_test.c src/calibration_math.c src/calibration_math.h
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -Isrc tools/calibration_test.c src/calibration_math.c -lm -o $@

calibration-test: $O/calibration_test
	$(QUIET)$O/calibration_test

# Host check of src/aes128.c against FIPS-197 and BTstack's rijndael.c.
RIJNDAEL_DIR := $(BTSTACK_DIR)/3rd-party/rijndael
$O/aes128_test: tools/aes128_test.c src/aes128.c src/aes128.h $(RIJNDAEL_DIR)/rijndael.c
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -Isrc -I$(RIJNDAEL_DIR) tools/aes128_test.c src/aes128.c $(RIJNDAEL_DIR)/rijndael.c -o $@

aes128-test: $O/aes128_test
	$(QUIET)$O/aes128_test

# Host check for the settings engine against an in-memory syscfg; see
# docs/flash-map.md.
CONFIG_TEST_SOURCES := tools/config_test.c src/config.c src/config_schema.c
$O/config_test: $(CONFIG_TEST_SOURCES) src/config.h src/profile_types.h tools/host/config_host.h
	@mkdir -p $(@D)
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -DCONFIG_HOST_TEST -Itools/host -Isrc $(CONFIG_TEST_SOURCES) -o $@

config-test: $O/config_test
	$(QUIET)$O/config_test

$O/profile_switch_hid.h: $O/bin2header src/profile_switch_hid.bin
	@mkdir -p $(@D)
	@echo [GEN] $@
	$(QUIET)$O/bin2header src/profile_switch_hid.bin $@ \
		nintendo_switch_pro_hid_report_descriptor

$O/profile_switch_spi.h: $O/bin2header src/profile_switch_spi.bin
	@mkdir -p $(@D)
	@echo [GEN] $@
	$(QUIET)$O/bin2header src/profile_switch_spi.bin $@ \
		nintendo_switch_pro_spi_flash

# minimal version of download.sh
$O/app.bin: $O/app.elf
	@echo [BIN] $@
	@mkdir -p $(@D)/sections
	$(QUIET)for i in text data; do $(OBJCOPY) -O binary -j .$$i $< $O/sections/$$i.bin; done
	$(QUIET)for i in aec wav ape flac m4a amr dts fm mp3 wma; do $(OBJCOPY) -O binary -j .overlay_$$i $< $O/sections/$$i.bin; done
	$(QUIET)cd $O/sections && cat text.bin data.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin fm.bin mp3.bin wma.bin > ../app.bin

$O/app.elf: $O/sdk.ld $(OBJECTS) $(SDK_ARCHIVES)
	@mkdir -p $(@D)
	@echo [LD] $@
	$(QUIET)$(LD) -o $@ $(OBJECTS) -T$O/sdk.ld -M=$O/app.map $(SDK_LFLAGS) --start-group $(SDK_ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

$(PATCHED_BTCTRLR): $(VENDOR_BTCTRLR) tools/patch_btctrler.py $(BUILD_CONFIG)
	@mkdir -p $(@D)
	@echo [PATCH] $@
	$(QUIET)/work/bin/python3 tools/patch_btctrler.py $(VENDOR_BTCTRLR) $@ --clang $(CC) --ar $(LLVM_AR) --config $(BUILD_CONFIG)

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

# Upstream BTstack and the Jieli port are compiled straight from their
# checkout; btstack_config.h comes from src/btstack via -iquote.
$O/btstack/%.c.o: $(BTSTACK_DIR)/%.c
	$(compile_btstack_c)

# profile_switch.c uses the arrays generated from src/*.bin
$O/src/profile_switch.c.o: $O/profile_switch_hid.h $O/profile_switch_spi.h

$O/sdk/%.c.o: $(SDK)/%.c
	$(compile_c)

# Fix the SDK's one-way DTR/RTS latch and avoid resetting bulk endpoint DATA
# toggles whenever Linux opens/closes CDC. Patch a build copy only.
$O/cdc.c: $(SDK)/apps/common/device/usb/device/cdc.c tools/patch_cdc.py Makefile
	@mkdir -p $(@D)
	$(QUIET)/work/bin/python3 tools/patch_cdc.py $< $@

$O/sdk/apps/common/device/usb/device/cdc.c.o: $O/cdc.c
	$(compile_c)

# DEBUG_LOG=1: replace the SDK's no-op print stubs with ones that reach the
# USB console, so the controller library's LMP/HCI tracing is readable.
ifneq ($(filter 1 verbose,$(DEBUG_LOG)),)
$O/debug.c: $(SDK)/apps/common/debug/debug.c tools/patch_debug.py Makefile
	@mkdir -p $(@D)
	$(QUIET)/work/bin/python3 tools/patch_debug.py $< $@

$O/sdk/apps/common/debug/debug.c.o: $O/debug.c
	$(compile_c)
endif

clean:
	rm -rf "$O"

-include $(DEPFILES)

WAKE_SESSION_SOURCES := tools/wake_session_test.c src/profile_switch_wake.c \
	src/profile_switch_wake_code.c src/profile_switch_wake_payload.c
$O/wake_session_test: $(WAKE_SESSION_SOURCES) $(wildcard src/profile_switch_wake*.h) src/config.h tools/host/system/timer.h
	@mkdir -p $(@D)
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -Itools/host -Isrc $(WAKE_SESSION_SOURCES) -o $@

.PHONY: wake-session-test
wake-session-test: $O/wake_session_test
	$(QUIET)$O/wake_session_test

CONSOLE_TEST_SOURCES := tools/console_test.c src/console.c src/config.c src/config_schema.c
$O/console_test: $(CONSOLE_TEST_SOURCES) src/console.h src/config.h tools/host/system/includes.h
	@mkdir -p $(@D)
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -DCONFIG_HOST_TEST -Itools/host -Isrc $(CONSOLE_TEST_SOURCES) -o $@

.PHONY: console-test
console-test: $O/console_test
	$(QUIET)$O/console_test
