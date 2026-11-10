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
# The Bluetooth core runs Yichip's program, which this repository does not
# carry: make cuts it from a dump of the stock v1.01 flash (U2CB_STOCK_DUMP,
# from 0x01017000; the stock-firmware repository's) and checks its SHA-256.

U2CB_STOCK_DUMP ?= ../stock-firmware/8bitdo-ultimate2c-bluetooth/dumps/pad-v1.01-flash-0x01017000-0x01080000.bin
U2CB_BLOB_AT := 74517# 0x01029314 - 0x01017000, plus 1 for tail
U2CB_BLOB_LEN := 7596
U2CB_BLOB_SHA256 := 73d2a159a27681a20dd45ab2cae2923f2cfa634e6b95f20e60c4288d54974283
U2CB_BLOB := $(O)/yc3121/radio_blob.bin

define u2cb_blob_cmd
tail -c +$(U2CB_BLOB_AT) $< | head -c $(U2CB_BLOB_LEN) > $@.tmp &&
{ echo "$(U2CB_BLOB_SHA256)  $@.tmp" | sha256sum -c --quiet || { rm -f $@.tmp; echo "the radio blob from $< is not v1.01's"; exit 1; }; } &&
mv -f $@.tmp $@
endef
TRACKED += $(U2CB_BLOB)
$(U2CB_BLOB): $(U2CB_STOCK_DUMP) | $(O)/yc3121/
	$(call lib-run,$(u2cb_blob_cmd),BLOB)

# The settings store (core/store.c): two 16 KiB banks above the app slot,
# below stock's data pages (flash_yc3121.h).
U2CB_STORE := -DSTORE_A=0x01074000 -DSTORE_B=0x01078000 -DSTORE_BANK=0x4000

# The app's region (its header) 0x10000 above the slot's base, the app
# JM_MAIN_HEAD_SIZE (0x120) further: the two go together.
U2CB_MAIN_AT := 0x10000
U2CB_APP_AT := 0x01028120

# --- Hello (bring-up app)

U2CB_HELLO_SOURCES := src/platform/yc3121/start.S src/platform/yc3121/platform_yc3121.c \
	src/platform/yc3121/usb_yc3121.c src/platform/yc3121/radio_yc3121.c src/platform/yc3121/radio_blob.S \
	src/platform/yc3121/sdp_yc3121.c src/platform/yc3121/flash_yc3121.c src/core/store.c \
	src/core/utils.c $(self)/hello.c

TARGETS += u2cb/hello/app.elf
u2cb/hello/app.elf_toolchain := YC3121_
u2cb/hello/app.elf_sources := $(U2CB_HELLO_SOURCES)
u2cb/hello/app.elf_cppflags := $(U2CB_STORE) -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) \
	-DRETAINED="__attribute__((section(\".noinit\")))" -DLOG_LINES=32 -DRADIO_BLOB='"$(U2CB_BLOB)"' \
	-iquote src/core -iquote src/platform/yc3121
u2cb/hello/app.elf_ldflags := $(YC_CFLAGS) $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/hello/app.map
u2cb/hello/app.elf_deps := $(YC_LDSCRIPT)
u2cb/hello/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,u2cb/hello/app.elf,src/platform/yc3121/radio_blob.S): $(U2CB_BLOB)

TARGETS += u2cb/hello/app.bin
u2cb/hello/app.bin_type := yc3121-bin
u2cb/hello/app.bin_deps := $(O)/u2cb/hello/app.elf

# --- Updater: USB only; its log as the app's, so the two keep one ring

U2CB_UPDATER_SOURCES :=
U2CB_UPDATER_SOURCES += src/updater/updater.c
U2CB_UPDATER_SOURCES += src/updater/updater_yc3121.c
U2CB_UPDATER_SOURCES += src/boards/8bitdo-ultimate2c-bluetooth/board_pad.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/main_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/platform_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/usb_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/tool_yc3121.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/flash_yc3121.c
U2CB_UPDATER_SOURCES += src/core/utils.c
U2CB_UPDATER_SOURCES += src/core/battery.c
U2CB_UPDATER_SOURCES += src/platform/yc3121/start.S

TARGETS += u2cb/updater/app.elf
u2cb/updater/app.elf_toolchain := YC3121_
u2cb/updater/app.elf_sources := $(U2CB_UPDATER_SOURCES)
u2cb/updater/app.elf_cppflags := -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) \
	-DUPDATER -DNO_BT -DMAIN_AT=$(U2CB_MAIN_AT) $(U2CB_STORE) -DLOG_LINES=32 $(EXTRA_DEFINES) \
	$(YC3121_SRC_INCLUDES) -iquote $(self)
u2cb/updater/app.elf_ldflags := $(YC_CFLAGS) -Wl,--defsym=TEXT_END=0x01020000 $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/updater/app.map
u2cb/updater/app.elf_deps := $(YC_LDSCRIPT)
u2cb/updater/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,u2cb/updater/app.elf,src/platform/yc3121/start.S): obj_cppflags += -DRADIO_BLOB='"$(U2CB_BLOB)"'

TARGETS += u2cb/updater/app.bin
u2cb/updater/app.bin_type := yc3121-bin
u2cb/updater/app.bin_deps := $(O)/u2cb/updater/app.elf

# --- Pad: linked 0x10000 above the updater

# The log: 32 lines, as many as fit the RAM kept over a restart (app.ld KEPT).
# LE (bt_yc3121.c): the core's MTU stays 23, so config mode answers in
# 20-byte notifications.
U2CB_PAD_DEFINES := $(U2CB_STORE) -DLOG_LINES=32 -DCONFIG_REPLY_MAX=20

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
U2CB_PAD_SOURCES += src/core/leds.c
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
U2CB_PAD_SOURCES += src/platform/yc3121/radio_blob.S

TARGETS += u2cb/pad/app.elf
u2cb/pad/app.elf_toolchain := YC3121_
u2cb/pad/app.elf_sources := $(U2CB_PAD_SOURCES)
u2cb/pad/app.elf_cppflags := -DBOARD_PID=\"U2C_BLUETOOTH\" $(BUILD_ID_CPPFLAGS) $(U2CB_PAD_DEFINES) $(EXTRA_DEFINES) $(YC3121_SRC_INCLUDES) -iquote $(self)
u2cb/pad/app.elf_ldflags := $(YC_CFLAGS) -Wl,--defsym=TEXT_AT=$(U2CB_APP_AT) $(YC_LDFLAGS) -Wl,-Map,$(O)/u2cb/pad/app.map
u2cb/pad/app.elf_deps := $(YC_LDSCRIPT)
u2cb/pad/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,u2cb/pad/app.elf,$(filter %.S,$(U2CB_PAD_SOURCES))): obj_cppflags += -DRADIO_BLOB='"$(U2CB_BLOB)"'
$(call lib-objs,u2cb/pad/app.elf,src/platform/yc3121/radio_blob.S): $(U2CB_BLOB)

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

