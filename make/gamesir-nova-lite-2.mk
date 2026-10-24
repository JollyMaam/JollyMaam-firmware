# GameSir Nova 2 Lite: the pad, a Jieli BR23 (stock PID AC695X_TRANS).
#   gamesir-nova-lite-2-pad          firmware.img in $(O)/<target>/
#   gamesir-nova-lite-2-pad-flash    from stock firmware too: jlnewfw sends
#                                    it the HID command in docs/gamesir-nova-lite-2/
#   gamesir-nova-lite-2-pad-loader   into the ROM loader
#   gamesir-nova-lite-2-pad-test, gamesir-nova-lite-2-all
# Until the shared parts move out of src/boards/gamesir-cyclone-2/, this
# board uses the Cyclone 2's bt_pad.c and btstack_config.h.
# The flash is 1 MiB; stock declares 512 KiB and keeps its VM, BTIF (MAC),
# EXIF, USERIF and key_mac at 0x64000..0x80000.  Declaring 1 MiB puts ours
# in the empty upper half, so an image of under 0x64000 leaves stock's alone.

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c

NOVA2LITE_PAD := gamesir-nova-lite-2-pad
$(NOVA2LITE_PAD)_FLASH_SIZE := 0x100000
$(NOVA2LITE_PAD)_PID := AC695X_TRANS
$(NOVA2LITE_PAD)_DEFINES := -DLED_DEFAULT_BRIGHTNESS_SCALE=4
$(NOVA2LITE_PAD)_LOADER_HINT := Stock firmware: Xbox 360 mode (green).
$(NOVA2LITE_PAD)_BOARD_DIR := src/boards/gamesir-cyclone-2

$(NOVA2LITE_PAD)_SOURCES :=
$(NOVA2LITE_PAD)_SOURCES += src/boards/gamesir-nova-lite-2/board_pad.c
$(NOVA2LITE_PAD)_SOURCES += src/boards/gamesir-cyclone-2/bt_pad.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/btstack_hci_jieli.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/btstack_qos_jieli.c
$(NOVA2LITE_PAD)_SOURCES += src/core/controller.c
$(NOVA2LITE_PAD)_SOURCES += src/core/input.c
$(NOVA2LITE_PAD)_SOURCES += src/core/input_calibration.c
$(NOVA2LITE_PAD)_SOURCES += src/core/motion.c
$(NOVA2LITE_PAD)_SOURCES += src/core/leds.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/platform_jieli_audio.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/platform_jieli.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/platform_jieli_sdk.c
$(NOVA2LITE_PAD)_SOURCES += src/platform/jieli/platform_jieli_usb.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_hid.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_link.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_playstation.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_switch.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_switch_wake.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_xbox.c
$(NOVA2LITE_PAD)_SOURCES += src/core/remap.c
$(NOVA2LITE_PAD)_SOURCES += src/core/settings.c
$(NOVA2LITE_PAD)_SOURCES += src/core/settings_ux.c
$(NOVA2LITE_PAD)_SOURCES += src/core/usb.c
$(NOVA2LITE_PAD)_SOURCES += src/core/utils.c

$(eval $(call jieli_board,$(NOVA2LITE_PAD)))$(eval $(call host_tests,$(NOVA2LITE_PAD)))

.PHONY: gamesir-nova-lite-2-all gamesir-nova-lite-2-test
gamesir-nova-lite-2-all: $(NOVA2LITE_PAD)
gamesir-nova-lite-2-test: $(NOVA2LITE_PAD)-test
