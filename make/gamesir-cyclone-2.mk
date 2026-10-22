# GameSir Cyclone 2: the pad and GameSir's dongle, both on the Jieli BR23.
#   gamesir-cyclone-2-pad, -dongle       firmware.img in $(O)/<target>/
#   gamesir-cyclone-2-pad-flash, -dongle-flash
#   gamesir-cyclone-2-pad-loader, -dongle-loader   into the ROM loader
#   gamesir-cyclone-2-pad-test, -dongle-test, gamesir-cyclone-2-test
#   gamesir-cyclone-2-all                both boards and their tests' builds

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c
BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_dongle.c

CYCLONE2_PAD := gamesir-cyclone-2-pad
$(CYCLONE2_PAD)_FLASH_SIZE := 0x100000
$(CYCLONE2_PAD)_PID := GS_C2_ADC_DEVICE
$(CYCLONE2_PAD)_DEFINES :=
$(CYCLONE2_PAD)_LOADER_HINT := Stock firmware: Xbox 360 mode (green), or hold Home+Start while plugging in USB.
$(CYCLONE2_PAD)_BOARD_DIR := src/boards/gamesir-cyclone-2

$(CYCLONE2_PAD)_SOURCES :=
$(CYCLONE2_PAD)_SOURCES += src/boards/gamesir-cyclone-2/board_pad.c
$(CYCLONE2_PAD)_SOURCES += src/boards/gamesir-cyclone-2/bt_pad.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/btstack_hci_jieli.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/btstack_qos_jieli.c
$(CYCLONE2_PAD)_SOURCES += src/core/controller.c
$(CYCLONE2_PAD)_SOURCES += src/core/input.c
$(CYCLONE2_PAD)_SOURCES += src/core/input_calibration.c
$(CYCLONE2_PAD)_SOURCES += src/core/motion.c
$(CYCLONE2_PAD)_SOURCES += src/boards/gamesir-cyclone-2/imu_qmi8658a.c
$(CYCLONE2_PAD)_SOURCES += src/core/leds.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/platform_jieli_audio.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/platform_jieli.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/platform_jieli_sdk.c
$(CYCLONE2_PAD)_SOURCES += src/platform/jieli/platform_jieli_usb.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_hid.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_link.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_playstation.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_switch.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_switch_wake.c
$(CYCLONE2_PAD)_SOURCES += src/profiles/profile_xbox.c
$(CYCLONE2_PAD)_SOURCES += src/core/remap.c
$(CYCLONE2_PAD)_SOURCES += src/core/settings.c
$(CYCLONE2_PAD)_SOURCES += src/core/settings_ux.c
$(CYCLONE2_PAD)_SOURCES += src/core/usb.c
$(CYCLONE2_PAD)_SOURCES += src/core/utils.c

CYCLONE2_DONGLE := gamesir-cyclone-2-dongle
$(CYCLONE2_DONGLE)_FLASH_SIZE := 0x80000
$(CYCLONE2_DONGLE)_PID := GS_C2_Dongle
$(CYCLONE2_DONGLE)_DEFINES := -DBOARD_DONGLE
$(CYCLONE2_DONGLE)_LOADER_HINT := This firmware by hand: hold the button while plugging in USB.
$(CYCLONE2_DONGLE)_BOARD_DIR := src/boards/gamesir-cyclone-2
$(CYCLONE2_DONGLE)_SOURCES :=
$(CYCLONE2_DONGLE)_SOURCES += src/boards/gamesir-cyclone-2/board_dongle.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/platform_jieli.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/platform_jieli_sdk.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/platform_jieli_usb.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/platform_jieli_audio.c
$(CYCLONE2_DONGLE)_SOURCES += src/boards/gamesir-cyclone-2/dongle.c
$(CYCLONE2_DONGLE)_SOURCES += src/boards/gamesir-cyclone-2/bt_dongle.c
$(CYCLONE2_DONGLE)_SOURCES += src/boards/gamesir-cyclone-2/dongle_protocol.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/btstack_hci_jieli.c
$(CYCLONE2_DONGLE)_SOURCES += src/platform/jieli/btstack_qos_jieli.c
$(CYCLONE2_DONGLE)_SOURCES += src/core/usb.c
$(CYCLONE2_DONGLE)_SOURCES += src/core/utils.c
$(CYCLONE2_DONGLE)_SOURCES += src/core/motion.c
$(CYCLONE2_DONGLE)_SOURCES += src/profiles/profile_link.c
$(CYCLONE2_DONGLE)_SOURCES += src/profiles/profile_switch.c
$(CYCLONE2_DONGLE)_SOURCES += src/profiles/profile_xbox.c
$(CYCLONE2_DONGLE)_SOURCES += src/profiles/profile_playstation.c
$(CYCLONE2_DONGLE)_SOURCES += src/profiles/profile_hid.c

$(foreach b,$(CYCLONE2_PAD) $(CYCLONE2_DONGLE),$(eval $(call jieli_board,$(b)))$(eval $(call host_tests,$(b))))

.PHONY: gamesir-cyclone-2-all gamesir-cyclone-2-test
gamesir-cyclone-2-all: $(CYCLONE2_PAD) $(CYCLONE2_DONGLE)
gamesir-cyclone-2-test: $(CYCLONE2_PAD)-test $(CYCLONE2_DONGLE)-test
