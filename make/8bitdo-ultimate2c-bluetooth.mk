# 8BitDo Ultimate 2C Bluetooth: the pad, a Yichip YC3121 (Cortex-M0).
#   8bitdo-ultimate2c-bluetooth-hello        bring-up app: $(O)/<target>/app.bin
#   8bitdo-ultimate2c-bluetooth-hello-flash  into the boot loader (by hand:
#                                            USB unplugged, hold L1+R1, tap Home, plug in)
# The app is a raw image for 0x01018000 (up to the data pages at 0x0107c000);
# the boot loader below it and the version page at 0x01017000 stay.
# The compiler is arm-none-eabi-gcc (flake.nix dev shell: gcc-arm-embedded).

YC_CROSS ?= arm-none-eabi-
YC_CFLAGS := -mcpu=cortex-m0 -mthumb -Os -g -std=gnu11 -Wall -Wextra -ffreestanding \
	-ffunction-sections -fdata-sections -fno-common
YC_LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,src/platform/yc3121/app.ld

U2CB_HELLO := 8bitdo-ultimate2c-bluetooth-hello
U2CB_HELLO_SRCS := src/platform/yc3121/start.S src/boards/8bitdo-ultimate2c-bluetooth/hello.c

$(O)/$(U2CB_HELLO)/app.elf: $(U2CB_HELLO_SRCS) src/platform/yc3121/yc3121.h src/platform/yc3121/app.ld $(MAKE_FILES)
	@mkdir -p $(@D)
	$(Q)$(YC_CROSS)gcc $(YC_CFLAGS) -iquote src/platform/yc3121 $(YC_LDFLAGS) -o $@ $(U2CB_HELLO_SRCS) -lgcc

$(O)/$(U2CB_HELLO)/app.bin: $(O)/$(U2CB_HELLO)/app.elf
	$(Q)$(YC_CROSS)objcopy -O binary $< $@
	$(Q)$(YC_CROSS)size $<

.PHONY: $(U2CB_HELLO) $(U2CB_HELLO)-flash
$(U2CB_HELLO): $(O)/$(U2CB_HELLO)/app.bin
$(U2CB_HELLO)-flash: $(O)/$(U2CB_HELLO)/app.bin
	python3 tools/8bitdoboot.py flash --pid U2C_BLUETOOTH $<
