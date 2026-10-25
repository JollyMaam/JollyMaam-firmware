# 8BitDo Ultimate 2C Wireless: the pad, a Telink TLSR8208 (B80).
#   8bitdo-ultimate2c-wireless-pad          firmware.bin in $(O)/<target>/
#   8bitdo-ultimate2c-wireless-pad-flash    through the 8BitDo boot loader
#   8bitdo-ultimate2c-wireless-pad-loader   into the boot loader
#   8bitdo-ultimate2c-wireless-pad-test, 8bitdo-ultimate2c-wireless-all
# The boot loader is 0x0000-0x5fff and starts the app at 0x6000; stock
# keeps data in sectors from 0x73000 (docs/8bitdo-ultimate2c-wireless/),
# which nothing here writes.

U2CW_PAD := 8bitdo-ultimate2c-wireless-pad
$(U2CW_PAD)_FLASH_SIZE := 0x80000
$(U2CW_PAD)_APP_START := 0x6000
$(U2CW_PAD)_APP_END := 0x73000
$(U2CW_PAD)_PID := U2C_WIRELESS
$(U2CW_PAD)_STAMP := 6a0000005d02
$(U2CW_PAD)_LOADER_HINT := Stock firmware or no answer: USB unplugged, hold L1+R1, tap Home, plug in USB.
$(U2CW_PAD)_BOARD_DIR := src/boards/8bitdo-ultimate2c-wireless
$(U2CW_PAD)_LDFLAGS := --wrap=otp_read

# 16 KiB of SRAM: no settings store yet (controller_lite.c), small log and
# trace rings.  The tool device's serial number starts with this, as the
# V-USB ID 16c0:27df requires (platform_telink_usb.c).
$(U2CW_PAD)_DEFINES :=
$(U2CW_PAD)_DEFINES += -DNO_SETTINGS
$(U2CW_PAD)_DEFINES += -DLOG_LINES=4
$(U2CW_PAD)_DEFINES += -DTRACE=16
$(U2CW_PAD)_DEFINES += '-DTOOL_SERIAL_PREFIX="$(TOOL_SERIAL_PREFIX)"'

$(U2CW_PAD)_SOURCES :=
$(U2CW_PAD)_SOURCES += src/boards/8bitdo-ultimate2c-wireless/board_pad.c
$(U2CW_PAD)_SOURCES += src/platform/telink/platform_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/platform_telink_usb.c
$(U2CW_PAD)_SOURCES += src/platform/telink/libc_telink.c
$(U2CW_PAD)_SOURCES += src/core/controller_lite.c
$(U2CW_PAD)_SOURCES += src/core/input.c
$(U2CW_PAD)_SOURCES += src/core/input_calibration.c
$(U2CW_PAD)_SOURCES += src/core/usb.c
$(U2CW_PAD)_SOURCES += src/core/utils.c

$(eval $(call telink_board,$(U2CW_PAD)))$(eval $(call host_tests,$(U2CW_PAD)))

.PHONY: 8bitdo-ultimate2c-wireless-all 8bitdo-ultimate2c-wireless-test
8bitdo-ultimate2c-wireless-all: $(U2CW_PAD)
8bitdo-ultimate2c-wireless-test: $(U2CW_PAD)-test
