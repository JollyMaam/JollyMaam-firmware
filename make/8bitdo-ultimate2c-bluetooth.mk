# 8BitDo Ultimate 2C Bluetooth: the pad, a Yichip YC3121 (Cortex-M0).
#   8bitdo-ultimate2c-bluetooth-hello        bring-up app (lights, keys, the tool
#                                            device on USB): $(O)/<target>/app.bin
#   8bitdo-ultimate2c-bluetooth-hello-flash  into the boot loader (by hand:
#                                            USB unplugged, hold L1+R1, tap Home, plug in)
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
YC_CFLAGS := -mcpu=cortex-m0 -mthumb -Os -g -std=gnu11 -Wall -Wextra -ffreestanding \
	-ffunction-sections -fdata-sections -fno-common
YC_LDFLAGS := -nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-T,src/platform/yc3121/app.ld

U2CB_HELLO := 8bitdo-ultimate2c-bluetooth-hello
U2CB_HELLO_SRCS := src/platform/yc3121/start.S src/platform/yc3121/platform_yc3121.c \
	src/platform/yc3121/usb_yc3121.c src/platform/yc3121/radio_yc3121.c src/platform/yc3121/radio_blob.S \
	src/core/utils.c src/boards/8bitdo-ultimate2c-bluetooth/hello.c
U2CB_HELLO_DEFINES := -DBOARD_PID=\"U2C_BLUETOOTH\" -DBUILD_ID=\"$(BUILD_ID)\" -DRETAINED="__attribute__((section(\".noinit\")))" -DLOG_LINES=32

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

.PHONY: $(U2CB_HELLO) $(U2CB_HELLO)-flash
$(U2CB_HELLO): $(O)/$(U2CB_HELLO)/app.bin
$(U2CB_HELLO)-flash: $(O)/$(U2CB_HELLO)/app.bin
	python3 tools/8bitdoboot.py flash --pid U2C_BLUETOOTH $<
