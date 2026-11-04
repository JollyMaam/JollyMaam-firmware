# 8BitDo Ultimate 2C Bluetooth: the pad, a Yichip YC3121 (Cortex-M0).
#   8bitdo-ultimate2c-bluetooth-pad          the firmware (controller.c, Classic
#                                            Bluetooth through the radio core):
#                                            $(O)/<target>/app.bin
#   8bitdo-ultimate2c-bluetooth-hello        bring-up app (lights, keys, the tool
#                                            device on USB): $(O)/<target>/app.bin
# The app is a raw image for 0x01018000 (up to the data pages at 0x0107c000);
# the boot loader below it and the version page at 0x01017000 stay.
# The compiler is arm-none-eabi-gcc (flake.nix dev shell: gcc-arm-embedded).
#
# The Bluetooth core runs Yichip's program, which this repository does not
# carry: make cuts it from a dump of the stock v1.01 flash (U2CB_STOCK_DUMP,
# from 0x01017000; the stock-firmware repository's) and checks its SHA-256.

U2CB_STOCK_DUMP ?= ../stock-firmware/8bitdo-ultimate2c-bluetooth/dumps/pad-v1.01-flash-0x01017000-0x01080000.bin
U2CB_BLOB_AT := 74517                    # 0x01029314 - 0x01017000, plus 1 for tail
U2CB_BLOB_LEN := 7596
U2CB_BLOB_SHA256 := 73d2a159a27681a20dd45ab2cae2923f2cfa634e6b95f20e60c4288d54974283

YC_CROSS ?= arm-none-eabi-
# -fsigned-char: pad.h's s8 is a plain char, unsigned on ARM by default.
YC_CFLAGS := -mcpu=cortex-m0 -mthumb -fsigned-char -Os -g -std=gnu11 -Wall -Wextra -ffreestanding \
	-ffunction-sections -fdata-sections -fno-common
YC_LDFLAGS := -nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-T,src/platform/yc3121/app.ld

U2CB_HELLO := 8bitdo-ultimate2c-bluetooth-hello
U2CB_HELLO_SRCS := src/platform/yc3121/start.S src/platform/yc3121/platform_yc3121.c \
	src/platform/yc3121/usb_yc3121.c src/platform/yc3121/radio_yc3121.c src/platform/yc3121/radio_blob.S src/platform/yc3121/sdp_yc3121.c src/platform/yc3121/flash_yc3121.c src/core/store.c \
	src/core/utils.c src/boards/8bitdo-ultimate2c-bluetooth/hello.c
# The settings store (core/store.c): two 16 KiB banks at the top of the app
# slot, below stock's data pages (flash_yc3121.h).
U2CB_STORE := -DSTORE_A=0x01074000 -DSTORE_B=0x01078000 -DSTORE_BANK=0x4000
U2CB_HELLO_DEFINES := $(U2CB_STORE) -DBOARD_PID=\"U2C_BLUETOOTH\" -DBUILD_ID=\"$(BUILD_ID)\" -DRETAINED="__attribute__((section(\".noinit\")))" -DLOG_LINES=32

$(O)/yc3121/radio_blob.bin: $(U2CB_STOCK_DUMP) $(MAKE_FILES)
	@mkdir -p $(@D)
	$(Q)tail -c +$(strip $(U2CB_BLOB_AT)) $< | head -c $(U2CB_BLOB_LEN) > $@.tmp
	$(Q)echo "$(U2CB_BLOB_SHA256)  $@.tmp" | sha256sum -c --quiet || { rm -f $@.tmp; echo "the radio blob from $< is not v1.01's"; exit 1; }
	$(Q)mv $@.tmp $@

$(O)/$(U2CB_HELLO)/app.elf: $(U2CB_HELLO_SRCS) $(wildcard src/platform/yc3121/*.h) src/core/pad.h \
		src/platform/yc3121/app.ld $(O)/yc3121/radio_blob.bin $(MAKE_FILES) $(BUILD_METADATA)
	@mkdir -p $(@D)
	$(Q)$(YC_CROSS)gcc $(YC_CFLAGS) $(U2CB_HELLO_DEFINES) -DRADIO_BLOB='"$(O)/yc3121/radio_blob.bin"' \
		-iquote src/core -iquote src/platform/yc3121 \
		$(YC_LDFLAGS) -Wl,-Map,$(@D)/app.map -o $@ $(U2CB_HELLO_SRCS) -lc -lgcc

$(O)/$(U2CB_HELLO)/app.bin: $(O)/$(U2CB_HELLO)/app.elf
	$(Q)$(YC_CROSS)objcopy -O binary $< $@
	$(Q)$(YC_CROSS)size $<

.PHONY: $(U2CB_HELLO)
$(U2CB_HELLO): $(O)/$(U2CB_HELLO)/app.bin

# ---- the pad ----------------------------------------------------------------

U2CB_PAD := 8bitdo-ultimate2c-bluetooth-pad
$(U2CB_PAD)_PID := U2C_BLUETOOTH
# The log: 32 lines, as many as fit the RAM kept over a restart (app.ld KEPT).
$(U2CB_PAD)_DEFINES := $(U2CB_STORE) -DLOG_LINES=32
# LE (bt_yc3121.c): the core's MTU stays 23, so config mode answers in
# 20-byte notifications.
$(U2CB_PAD)_DEFINES += -DCONFIG_REPLY_MAX=20
$(U2CB_PAD)_SOURCES :=
$(U2CB_PAD)_SOURCES += src/boards/8bitdo-ultimate2c-bluetooth/board_pad.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/main_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/platform_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/usb_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/tool_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/radio_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/sdp_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/flash_yc3121.c
$(U2CB_PAD)_SOURCES += src/platform/yc3121/bt_yc3121.c
$(U2CB_PAD)_SOURCES += src/core/store.c
$(U2CB_PAD)_SOURCES += src/core/controller.c
$(U2CB_PAD)_SOURCES += src/core/input.c
$(U2CB_PAD)_SOURCES += src/core/input_calibration.c
$(U2CB_PAD)_SOURCES += src/core/motion.c
$(U2CB_PAD)_SOURCES += src/core/leds.c
$(U2CB_PAD)_SOURCES += src/core/remap.c
$(U2CB_PAD)_SOURCES += src/core/settings.c
$(U2CB_PAD)_SOURCES += src/core/settings_ux.c
$(U2CB_PAD)_SOURCES += src/core/usb.c
$(U2CB_PAD)_SOURCES += src/core/utils.c
$(U2CB_PAD)_SOURCES += src/core/battery.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_hid.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_link.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_config.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_playstation.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_switch.c
$(U2CB_PAD)_SOURCES += src/profiles/profile_xbox.c
$(U2CB_PAD)_ASM := src/platform/yc3121/start.S src/platform/yc3121/radio_blob.S
$(U2CB_PAD)_BOARD_DIR := src/boards/8bitdo-ultimate2c-bluetooth

U2CB_PAD_OBJECTS := $(patsubst %.c,$(O)/$(U2CB_PAD)/%.o,$($(U2CB_PAD)_SOURCES)) \
	$(patsubst %.S,$(O)/$(U2CB_PAD)/%.o,$($(U2CB_PAD)_ASM))
U2CB_PAD_FLAGS := $(YC_CFLAGS) -DBOARD_PID=\"$($(U2CB_PAD)_PID)\" -DBUILD_ID=\"$(BUILD_ID)\" \
	$($(U2CB_PAD)_DEFINES) $(EXTRA_DEFINES) \
	$(subst -iquote src/platform/jieli,,$(SRC_INCLUDES)) -iquote src/platform/yc3121 \
	-iquote $($(U2CB_PAD)_BOARD_DIR)

.PHONY: $(U2CB_PAD)
$(U2CB_PAD): $(O)/$(U2CB_PAD)/app.bin

$(O)/$(U2CB_PAD)/%.o: %.c $(O)/$(U2CB_PAD)/defines
	@mkdir -p $(@D)
	@echo [CC $(U2CB_PAD)] $<
	$(Q)$(YC_CROSS)gcc $(U2CB_PAD_FLAGS) -MMD -MP -c $< -o $@

$(O)/$(U2CB_PAD)/src/platform/yc3121/radio_blob.o: $(O)/yc3121/radio_blob.bin
$(O)/$(U2CB_PAD)/%.o: %.S $(O)/$(U2CB_PAD)/defines
	@mkdir -p $(@D)
	@echo [AS $(U2CB_PAD)] $<
	$(Q)$(YC_CROSS)gcc $(U2CB_PAD_FLAGS) -DRADIO_BLOB='"$(O)/yc3121/radio_blob.bin"' -c $< -o $@

$(O)/$(U2CB_PAD)/src/core/usb.o: $(wildcard $(BUILD_METADATA))
$(U2CB_PAD_OBJECTS): $(MAKE_FILES)

$(O)/$(U2CB_PAD)/app.elf: $(U2CB_PAD_OBJECTS) src/platform/yc3121/app.ld
	@echo [LD $(U2CB_PAD)] $@
	$(Q)$(YC_CROSS)gcc $(YC_CFLAGS) $(YC_LDFLAGS) -Wl,-Map,$(@D)/app.map -o $@ $(U2CB_PAD_OBJECTS) -lc -lgcc

$(O)/$(U2CB_PAD)/app.bin: $(O)/$(U2CB_PAD)/app.elf
	$(Q)$(YC_CROSS)objcopy -O binary $< $@
	$(Q)$(YC_CROSS)size $<


$(eval $(call board_defines,$(U2CB_PAD)))
-include $(U2CB_PAD_OBJECTS:.o=.d)
$(eval $(call host_tests,$(U2CB_PAD)))
