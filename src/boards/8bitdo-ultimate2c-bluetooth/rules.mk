HELP_TARGETS += u2cb
$(FIRMWARE_ZIP): $(O)/u2cb/pad/firmware.img

firmware: u2cb
test: u2cb-test

u2cb: $(O)/u2cb/pad/firmware.img
u2cb/hello: $(O)/u2cb/hello/app.bin
u2cb/pad-test: $(O)/host/test-u2cb/pad
u2cb-test: u2cb/pad-test

.PHONY: u2cb u2cb/hello u2cb-test u2cb/pad-test

# The app slot is 0x01018000..0x01060000 (flash_yc3121.h), above the boot
# loader and the version page at 0x01017000, which stay.  The updater has
# its first 32 KiB, then a window stays empty (updater_yc3121.c); the app's
# header is at 0x01028000 and the app after it.  Settings stay in the
# store's banks, stock's data pages from 0x0107c000.
#
# The Bluetooth core runs Yichip's program, as stock v1.01 carries it:
# src/platform/yc3121/radio_patch.c.

# The settings store (core/store.c): two 16 KiB banks above the app slot,
# below stock's data pages (flash_yc3121.h).
U2CB_STORE := -DSTORE_A=0x01074000 -DSTORE_B=0x01078000 -DSTORE_BANK=0x4000

# The app's region (its header) 0x10000 above the slot's base, the app
# JM_MAIN_HEAD_SIZE (0x120) further: the two go together.
U2CB_MAIN_AT := 0x10000
U2CB_APP_AT := 0x01028120

# --- Hello (bring-up app)

U2CB_HELLO_SOURCES := src/platform/yc3121/start.S src/platform/yc3121/platform_yc3121.c \
	src/platform/yc3121/usb_yc3121.c src/platform/yc3121/radio_yc3121.c src/platform/yc3121/radio_patch.c \
	src/platform/yc3121/sdp_yc3121.c src/platform/yc3121/flash_yc3121.c src/core/store.c \
	src/core/utils.c $(self)/hello.c

TARGETS += u2cb/hello/app.elf
u2cb/hello/app.elf_toolchain := YC3121_
u2cb/hello/app.elf_sources := $(U2CB_HELLO_SOURCES)
u2cb/hello/app.elf_cppflags := $(U2CB_STORE) -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) \
	-DRETAINED="__attribute__((section(\".noinit\")))" -DLOG_LINES=32 \
	-iquote src/core -iquote src/platform/yc3121
u2cb/hello/app.elf_ldflags := $(YC_CFLAGS) $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/hello/app.map
u2cb/hello/app.elf_deps := $(YC_LDSCRIPT)
u2cb/hello/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += u2cb/hello/app.bin
u2cb/hello/app.bin_type := yc3121-bin
u2cb/hello/app.bin_deps := $(O)/u2cb/hello/app.elf

# --- Updater: USB and LE; the core takes writes of 244 bytes at most (251
# fails and drops the link): UPDATE_DATA over LE in 224.  Such a write lost
# while an erase holds the CPU (the core answers ATT error 0x0e): BEGIN erases
# it all (seconds).  Its log as the app's,
# so the two keep one ring

U2CB_UPDATER_SOURCES :=
U2CB_UPDATER_SOURCES += src/updater/updater.c
U2CB_UPDATER_SOURCES += src/updater/updater_yc3121.c
U2CB_UPDATER_SOURCES += src/updater/updater_ble_yc3121.c
U2CB_UPDATER_SOURCES += src/boards/8bitdo-ultimate2c-bluetooth/board_pad.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/main_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/platform_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/usb_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/tool_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/flash_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/radio_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/sdp_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/bt_yc3121.c
U2CB_UPDATER_SOURCES += src/core/utils.c
U2CB_UPDATER_SOURCES += src/core/battery.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/start.S
U2CB_UPDATER_SOURCES += src/platform/yc3121/radio_patch.c

TARGETS += u2cb/updater/app.elf
u2cb/updater/app.elf_toolchain := YC3121_
u2cb/updater/app.elf_sources := $(U2CB_UPDATER_SOURCES)
u2cb/updater/app.elf_cppflags := -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) \
	-DUPDATER -DBT_WRITE_MAX=244 -DUPD_LE_DATA_MAX=224 -DUPD_ERASE_AT_BEGIN -DUPD_WRITE_BLOCK=32 -DMAIN_AT=$(U2CB_MAIN_AT) $(U2CB_STORE) -DLOG_LINES=32 $(EXTRA_DEFINES) \
	$(YC3121_SRC_INCLUDES) -iquote $(self)
u2cb/updater/app.elf_ldflags := $(YC_CFLAGS) -Wl,--defsym=TEXT_END=0x01020000 $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/updater/app.map
u2cb/updater/app.elf_deps := $(YC_LDSCRIPT)
u2cb/updater/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += u2cb/updater/app.bin
u2cb/updater/app.bin_type := yc3121-bin
u2cb/updater/app.bin_deps := $(O)/u2cb/updater/app.elf

# --- Pad: linked 0x10000 above the updater

# The log: 32 lines, as many as fit the RAM kept over a restart (app.ld KEPT).
# LE (bt_yc3121.c): the core's MTU stays 23, so config mode answers in
# 20-byte notifications.
U2CB_PAD_DEFINES := $(U2CB_STORE) -DLOG_LINES=32 -DCONFIG_REPLY_MAX=20 -DLEDS_MONO=1 -DSLOT_SWITCH2_WAKE=1

U2CB_PAD_SOURCES :=
U2CB_PAD_SOURCES += src/boards/8bitdo-ultimate2c-bluetooth/board_pad.c
U2CB_PAD_SOURCES += src/platform/yc3121/main_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/platform_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/usb_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/tool_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/radio_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/sdp_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/flash_yc3121.c
U2CB_PAD_SOURCES += src/platform/yc3121/bt_yc3121.c
U2CB_PAD_SOURCES += src/core/store.c
U2CB_PAD_SOURCES += src/core/controller.c
U2CB_PAD_SOURCES += src/core/input.c
U2CB_PAD_SOURCES += src/core/input_calibration.c
U2CB_PAD_SOURCES += src/core/motion.c
U2CB_PAD_SOURCES += src/core/imu_i2c.c
U2CB_PAD_SOURCES += src/core/leds_mono.c
U2CB_PAD_SOURCES += src/core/remap.c
U2CB_PAD_SOURCES += src/core/settings.c
U2CB_PAD_SOURCES += src/core/settings_ux.c
U2CB_PAD_SOURCES += src/core/usb.c
U2CB_PAD_SOURCES += src/core/utils.c
U2CB_PAD_SOURCES += src/core/battery.c
U2CB_PAD_SOURCES += src/profiles/profile_hid.c
U2CB_PAD_SOURCES += src/profiles/profile_link.c
U2CB_PAD_SOURCES += src/profiles/profile_config.c
U2CB_PAD_SOURCES += src/profiles/profile_playstation.c
U2CB_PAD_SOURCES += src/profiles/profile_switch.c
U2CB_PAD_SOURCES += src/profiles/profile_xbox.c
U2CB_PAD_SOURCES += src/platform/yc3121/start.S
U2CB_PAD_SOURCES += src/platform/yc3121/radio_patch.c

TARGETS += u2cb/pad/app.elf
u2cb/pad/app.elf_toolchain := YC3121_
u2cb/pad/app.elf_sources := $(U2CB_PAD_SOURCES)
u2cb/pad/app.elf_cppflags := -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) $(U2CB_PAD_DEFINES) $(EXTRA_DEFINES) $(YC3121_SRC_INCLUDES) -iquote $(self)
u2cb/pad/app.elf_ldflags := $(YC_CFLAGS) -Wl,--defsym=TEXT_AT=$(U2CB_APP_AT) $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/pad/app.map
u2cb/pad/app.elf_deps := $(YC_LDSCRIPT)
u2cb/pad/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += u2cb/pad/app.bin
u2cb/pad/app.bin_type := yc3121-bin
u2cb/pad/app.bin_deps := $(O)/u2cb/pad/app.elf

TARGETS += u2cb/pad/firmware.img
u2cb/pad/firmware.img_type := jollymaam-join
u2cb/pad/firmware.img_deps := $(O)/u2cb/updater/app.bin $(O)/u2cb/pad/app.bin $(JOLLYMAAM_CLI)
$(O)/u2cb/pad/firmware.img: private jollymaam_join := --pid U2C_BLUETOOTH --at $(U2CB_MAIN_AT) --entry $(U2CB_APP_AT) --main-out $(O)/u2cb/pad/update.bin $(O)/u2cb/updater/app.bin $(O)/u2cb/pad/app.bin

# --- Tests

TARGETS += host/test-u2cb/pad
host/test-u2cb/pad_type := host-tests
host/test-u2cb/pad_toolchain := HOST_
host/test-u2cb/pad_sources := $(HOST_TESTS)
host/test-u2cb/pad_cppflags := -DBOARD_PID=\"U2C_BLUETOOTH\" $(U2CB_PAD_DEFINES) $(EXTRA_DEFINES) -DUNITTEST -DSETTINGS_STORE \
	$(SRC_INCLUDES) -iquote $(self)
host/test-u2cb/pad_compile.c = $(host-test-compile)
$(call lib-objs,host/test-u2cb/pad,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-u2cb/pad,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-u2cb/pad,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

