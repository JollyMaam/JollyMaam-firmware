# GameSir Cyclone 2: the pad and GameSir's dongle, both on the Jieli BR23.
#   gamesir-cyclone-2-pad, -dongle       firmware.img in $(O)/<target>/
#   gamesir-cyclone-2-pad-flash, -dongle-flash
#   gamesir-cyclone-2-pad-test, -dongle-test, gamesir-cyclone-2-test
#   gamesir-cyclone-2-all                both boards and their tests' builds

include make/btstack.mk
include make/jieli.mk

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c src/boards/gamesir-cyclone-2/bt_dongle.c

CYCLONE2_PAD := gamesir-cyclone-2-pad
$(CYCLONE2_PAD)_FLASH_SIZE := 0x100000
$(CYCLONE2_PAD)_PID := GS_C2_ADC_DEVICE
$(CYCLONE2_PAD)_DEFINES :=
$(CYCLONE2_PAD)_BOARD_DIR := src/boards/gamesir-cyclone-2
$(CYCLONE2_PAD)_SOURCES := src/boards/gamesir-cyclone-2/board_pad.c \
	src/boards/gamesir-cyclone-2/bt_pad.c src/platform/jieli/btstack_hci_jieli.c src/platform/jieli/btstack_qos_jieli.c \
	src/core/controller.c src/core/input.c src/core/input_calibration.c src/core/input_imu.c src/core/leds.c \
	src/platform/jieli/platform_jieli_audio.c src/platform/jieli/platform_jieli.c \
	src/platform/jieli/platform_jieli_sdk.c src/platform/jieli/platform_jieli_usb.c \
	src/profiles/profile_hid.c src/profiles/profile_link.c src/profiles/profile_playstation.c \
	src/profiles/profile_switch.c src/profiles/profile_switch_wake.c src/profiles/profile_xbox.c \
	src/core/remap.c src/core/settings.c src/core/settings_ux.c src/core/usb.c src/core/utils.c

CYCLONE2_DONGLE := gamesir-cyclone-2-dongle
$(CYCLONE2_DONGLE)_FLASH_SIZE := 0x80000
$(CYCLONE2_DONGLE)_PID := GS_C2_Dongle
$(CYCLONE2_DONGLE)_DEFINES := -DBOARD_DONGLE
$(CYCLONE2_DONGLE)_BOARD_DIR := src/boards/gamesir-cyclone-2
$(CYCLONE2_DONGLE)_SOURCES := src/boards/gamesir-cyclone-2/board_dongle.c \
	src/platform/jieli/platform_jieli.c src/platform/jieli/platform_jieli_sdk.c \
	src/platform/jieli/platform_jieli_usb.c src/platform/jieli/platform_jieli_audio.c \
	src/platform/gamesir/dongle/dongle.c src/boards/gamesir-cyclone-2/bt_dongle.c src/platform/gamesir/dongle/dongle_protocol.c \
	src/platform/jieli/btstack_hci_jieli.c src/platform/jieli/btstack_qos_jieli.c \
	src/core/usb.c src/core/utils.c src/core/input_imu.c \
	src/profiles/profile_link.c src/profiles/profile_switch.c src/profiles/profile_xbox.c \
	src/profiles/profile_playstation.c src/profiles/profile_hid.c

$(foreach b,$(CYCLONE2_PAD) $(CYCLONE2_DONGLE),$(eval $(call jieli_board,$(b)))$(eval $(call host_tests,$(b))))

.PHONY: gamesir-cyclone-2-all gamesir-cyclone-2-test
gamesir-cyclone-2-all: $(CYCLONE2_PAD) $(CYCLONE2_DONGLE)
gamesir-cyclone-2-test: $(CYCLONE2_PAD)-test $(CYCLONE2_DONGLE)-test
