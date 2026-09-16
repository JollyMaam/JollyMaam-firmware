O := out
SDK := /work/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /work/jieli-linux-toolchains-20250324.1/pi32v2/bin
PYTHON := /work/bin/python3
FLASH_SIZE := 0x80000
# Keep the following 8-KiB VM region explicit in isd_config.  The profile
# modules and USB configuration console occupy one block beyond the earlier
# 0x40000 boundary; jlnewfw records this boundary in the packed image.
CODE_LIMIT := 0x41000

# Start from an exact, locally owned copy of the SDK HID application.  The
# "cyclone2" selection keeps the smaller experimental application available
# while the vendor baseline is reduced in controlled steps.
APPLICATION ?= vendor-hid

include sdk-br23.mk

HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES)
LFLAGS := $(SDK_LFLAGS)
SDK_ARCHIVES := $(SDK_ARCHIVES_FULL)

ifeq ($(APPLICATION),vendor-hid)
PROJECT_SOURCES := \
	src/vendor_hid/board/br23/board_ac6351d_keyboard.c \
	src/vendor_hid/board/br23/board_ac635n_demo.c \
	src/vendor_hid/config/lib_btctrler_config.c \
	src/vendor_hid/config/lib_btstack_config.c \
	src/vendor_hid/config/lib_driver_config.c \
	src/vendor_hid/config/lib_media_config.c \
	src/vendor_hid/config/lib_profile_config.c \
	src/vendor_hid/config/lib_system_config.c \
	src/vendor_hid/config/lib_update_config.c \
	src/vendor_hid/config/log_config.c \
	src/vendor_hid/examples/gamebox/app_gamebox.c \
	src/vendor_hid/examples/gamebox/gamebox.c \
	src/vendor_hid/examples/gamebox/key_mapping.c \
	src/vendor_hid/examples/idle/app_idle.c \
	src/vendor_hid/examples/keyboard/app_keyboard.c \
	src/vendor_hid/examples/keyfob/app_keyfob.c \
	src/vendor_hid/examples/keypage/app_keypage.c \
	src/vendor_hid/examples/mouse_dual/app_mouse_dual.c \
	src/vendor_hid/examples/mouse_single/app_mouse.c \
	src/vendor_hid/examples/standard_keyboard/app_standard_keyboard.c \
	src/vendor_hid/examples/standard_keyboard/usb_hid_devices.c \
	src/vendor_hid/examples/voice_remote_control/app_remote_control.c \
	src/vendor_hid/examples/voice_remote_control/audio_codec_demo.c \
	src/vendor_hid/modules/aec/br23/audio_aec.c \
	src/vendor_hid/modules/bt/app_comm_ble.c \
	src/vendor_hid/modules/bt/app_comm_edr.c \
	src/vendor_hid/modules/bt/ble_hogp.c \
	src/vendor_hid/modules/bt/edr_hid_user.c \
	src/vendor_hid/modules/misc.c \
	src/vendor_hid/modules/power/app_charge.c \
	src/vendor_hid/modules/power/app_chargestore.c \
	src/vendor_hid/modules/power/app_handshake.c \
	src/vendor_hid/modules/power/app_power_manage.c \
	src/vendor_hid/modules/rtc_alarm.c \
	src/vendor_hid/modules/user_cfg.c \
	src/vendor_hid/version.c \
	src/main.c \
	src/controller.c \
	src/input.c \
	src/profile.c \
	src/profile_generic.c \
	src/profile_switch.c \
	src/usb_control.c \
	src/led.c \
	src/rom.c
INCLUDES := \
	-Isrc \
	-Isrc/vendor_hid/include \
	-Isrc/vendor_hid/board/br23 \
	-Isrc/vendor_hid/modules/bt \
	-Isrc/vendor_hid \
	$(SDK_INCLUDES)
SDK_SOURCES := $(SDK_COMMON_SOURCES_FULL) $(SDK_SOURCES_FULL)
APP_CONFIG := src/vendor_hid/include/app_config.h
RESET_MAILBOX_LD := src/reset_mailbox.ld
APP_CONFIG_DEPS := \
	$(wildcard src/vendor_hid/include/*.h) \
	$(wildcard src/vendor_hid/board/br23/*.h) \
	src/vendor_hid/board/br23/app.ld \
	src/vendor_hid/board/br23/app_overlay.ld \
	$(RESET_MAILBOX_LD)
SDK_COMMON_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_COMMON_SOURCES_FULL:.c=.c.o))
SDK_CPU_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_SOURCES_FULL:.c=.c.o))
OBJECTS := $(SDK_COMMON_OBJECTS) $(addprefix $O/,$(PROJECT_SOURCES:.c=.c.o)) $(SDK_CPU_OBJECTS)
else ifeq ($(APPLICATION),cyclone2)
PROJECT_SOURCES := $(wildcard src/*.c config/*.c)
INCLUDES := -Isrc -Iconfig $(SDK_INCLUDES)
SDK_SOURCES := $(SDK_SOURCES_FULL)
APP_CONFIG := config/app_config.h
APP_CONFIG_DEPS := $(wildcard config/*.h config/*.ld)
SDK_OBJECTS := $(subst $(SDK),$O/sdk,$(SDK_SOURCES:.c=.c.o))
OBJECTS := $(addprefix $O/,$(PROJECT_SOURCES:.c=.c.o)) $(SDK_OBJECTS)
else
$(error unknown APPLICATION '$(APPLICATION)')
endif

DEPFILES := $(addsuffix .d,$(OBJECTS))

V?=0
ifneq ($(V),0)
QUIET=
else
QUIET=@
endif

.PHONY: all default clean

default all: $O/firmware.img $O/c2ctl

# regenerate stuff if makefile or config changes
$(OBJECTS) $O/sdk.ld $O/sdk_used_list.used: Makefile sdk-br23.mk $(APP_CONFIG_DEPS)

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
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$O/jlcfg: tools/jlcfg.c
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$O/c2ctl: tools/c2ctl.c
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) -Isrc $< -o $@

# minimal version of download.sh
$O/app.bin: $O/app.elf
	@echo [BIN] $@
	@mkdir -p $O/sections
	$(QUIET)for i in text data; do $(OBJCOPY) -O binary -j .$$i $< $O/sections/$$i.bin; done
	$(QUIET)for i in aec wav ape flac m4a amr dts fm mp3 wma; do $(OBJCOPY) -O binary -j .overlay_$$i $< $O/sections/$$i.bin; done
	$(QUIET)cd $O/sections && cat text.bin data.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin fm.bin mp3.bin wma.bin > ../app.bin

$O/app.elf: $O/sdk.ld $O/sdk_used_list.used $(OBJECTS)
	@mkdir -p $(@D)
	@echo [LD] $@
	$(QUIET)$(LD) -o $@ $(OBJECTS) -T$O/sdk.ld -M=$O/app.map $(SDK_LFLAGS) --plugin-opt=-used-symbol-file=$O/sdk_used_list.used --start-group $(SDK_ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

$O/sdk.ld: $(SDK)/cpu/br23/sdk_ld.c $(APP_CONFIG)
	$(compile_pre_build)
	# Reserve the profile-selection reset mailbox above the SDK heap and below
	# MaskROM's high workspace. Insert it after all normal RAM sections.
	$(QUIET)sed -i \
		-e '/^text_begin =/r $(RESET_MAILBOX_LD)' \
		-e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
		-e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
		$@

$O/sdk_used_list.used: $(SDK)/cpu/br23/sdk_used_list.c $(APP_CONFIG)
	$(compile_pre_build)

$O/isd_config.ini: $(SDK)/cpu/br23/tools/isd_config_rule.c $(APP_CONFIG)
	$(compile_pre_build)
	$(QUIET)sed -i -e /^$$/d "$@"

$O/src/%.c.o: src/%.c
	$(compile_c)

$O/src/vendor_hid/%.c.o: src/vendor_hid/%.c
	$(compile_c)

$O/config/%.c.o: config/%.c
	$(compile_c)

$O/sdk/%.c.o: $(SDK)/%.c
	$(compile_c)

clean:
	rm -rf out

-include $(DEPFILES)
