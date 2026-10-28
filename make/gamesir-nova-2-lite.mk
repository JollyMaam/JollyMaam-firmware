# GameSir Nova 2 Lite: the pad, a Jieli BR23 (stock PID AC695X_TRANS).
#   gamesir-nova-2-lite-flash        the pad: its updater and the app above
#                                    it; from stock firmware too: jlnewfw sends
#                                    it the HID command in docs/gamesir-nova-2-lite/
#   gamesir-nova-2-lite-pad-split    firmware.img in $(O)/<target>/
#   gamesir-nova-2-lite-pad, -pad-updater  the app alone and its updater:
#                                    parts of -pad-split, not flashed alone
#   gamesir-nova-2-lite-pad-loader   into the ROM loader
#   gamesir-nova-2-lite-pad-test, gamesir-nova-2-lite-all
# Until the shared parts move out of src/boards/gamesir-cyclone-2/, this
# board uses the Cyclone 2's bt_pad.c and btstack_config.h.
# The flash is 1 MiB; stock declares 512 KiB and keeps its VM, BTIF (MAC),
# EXIF, USERIF and key_mac at 0x64000..0x80000.  Declaring 1 MiB puts ours
# in the empty upper half, so an image of under 0x64000 leaves stock's alone.

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c

NOVA2LITE_PAD := gamesir-nova-2-lite-pad
$(NOVA2LITE_PAD)_FLASH_SIZE := 0x100000
$(NOVA2LITE_PAD)_PID := AC695X_TRANS
$(NOVA2LITE_PAD)_NO_FLASH := 1
$(NOVA2LITE_PAD)_DEFINES :=
$(NOVA2LITE_PAD)_LOADER_HINT := Stock firmware: Xbox 360 mode (green).
$(NOVA2LITE_PAD)_BOARD_DIR := src/boards/gamesir-cyclone-2

$(NOVA2LITE_PAD)_SOURCES :=
$(NOVA2LITE_PAD)_SOURCES += src/boards/gamesir-nova-2-lite/board_pad.c
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

# The pad behind its updater (src/updater/, docs/firmware/updater.md): the
# updater in the app area, below stock's data at 0x64000; the app's region
# at flash 0x80000 (0x7d000 above the app area), clear of it too.
NOVA2LITE_UPDATER := gamesir-nova-2-lite-pad-updater
$(NOVA2LITE_UPDATER)_FLASH_SIZE := $($(NOVA2LITE_PAD)_FLASH_SIZE)
$(NOVA2LITE_UPDATER)_PID := $($(NOVA2LITE_PAD)_PID)
$(NOVA2LITE_UPDATER)_BOARD_DIR := src/updater
$(NOVA2LITE_UPDATER)_UPDATER := 0x7d000
$(NOVA2LITE_UPDATER)_NO_FLASH := 1
$(NOVA2LITE_UPDATER)_DEFINES := -DBT_LE_ONLY -DUSB_TOOL_ONLY
$(NOVA2LITE_UPDATER)_BTSTACK := $($(CYCLONE2_UPDATER)_BTSTACK)
$(NOVA2LITE_UPDATER)_SOURCES := $(subst src/boards/gamesir-cyclone-2/board_pad.c,src/boards/gamesir-nova-2-lite/board_pad.c,$($(CYCLONE2_UPDATER)_SOURCES))

NOVA2LITE_SPLIT := gamesir-nova-2-lite-pad-split
$(foreach v,FLASH_SIZE PID DEFINES LOADER_HINT BOARD_DIR SOURCES,$(eval $(NOVA2LITE_SPLIT)_$(v) := $$($(NOVA2LITE_PAD)_$(v))))
$(NOVA2LITE_SPLIT)_MAIN_AT := $($(NOVA2LITE_UPDATER)_UPDATER)
$(NOVA2LITE_SPLIT)_FRONT := $(NOVA2LITE_UPDATER)
$(NOVA2LITE_SPLIT)_DEFINES += -DUX_UPDATER=1   # offline Home+M: the updater
$(NOVA2LITE_SPLIT)_KEEP := 0x64000:0x1c000       # stock's data: the image spans it

$(eval $(call jieli_board,$(NOVA2LITE_PAD)))$(eval $(call host_tests,$(NOVA2LITE_PAD)))
$(foreach b,$(NOVA2LITE_UPDATER) $(NOVA2LITE_SPLIT),$(eval $(call jieli_board,$(b))))
$(eval $(call host_tests,$(NOVA2LITE_SPLIT)))

.PHONY: gamesir-nova-2-lite-all gamesir-nova-2-lite-test gamesir-nova-2-lite-flash
gamesir-nova-2-lite-all: $(NOVA2LITE_PAD) $(NOVA2LITE_SPLIT)
gamesir-nova-2-lite-flash: $(NOVA2LITE_SPLIT)-flash
gamesir-nova-2-lite-test: $(NOVA2LITE_PAD)-test

# Nova dongle: BD19/AC632N (q32s), reusing the app's dongle link and USB profiles.
NOVA2LITE_DONGLE := gamesir-nova-2-lite-dongle
$(NOVA2LITE_DONGLE)_CHIP := bd19
$(NOVA2LITE_DONGLE)_FLASH_SIZE := 0x80000
$(NOVA2LITE_DONGLE)_PID := AC632N_TRANS
$(NOVA2LITE_DONGLE)_DEFINES := -DBOARD_DONGLE -DLOG_LINES=16 -DTRACE=16 -DUSB_DMA_BUF_MAX_SIZE=512
# Stock's long-press reset: off (time 00).
$(NOVA2LITE_DONGLE)_DEFINES += -DCONFIG_RESET_PIN=PB02 -DCONFIG_RESET_TIME=00 -DCONFIG_RESET_LEVEL=0
$(NOVA2LITE_DONGLE)_BOARD_DIR := src/boards/gamesir-cyclone-2
$(NOVA2LITE_DONGLE)_LOADER_HINT := Hold the pairing button while connecting USB.
$(NOVA2LITE_DONGLE)_SOURCES := $(filter-out src/boards/gamesir-cyclone-2/board_dongle.c src/platform/jieli/platform_jieli_audio.c src/platform/jieli/btstack_qos_jieli.c,$($(CYCLONE2_DONGLE)_SOURCES))
$(NOVA2LITE_DONGLE)_SOURCES += src/boards/gamesir-nova-2-lite/board_dongle.c src/platform/jieli/platform_jieli_noaudio.c
$(eval $(call jieli_board,$(NOVA2LITE_DONGLE)))$(eval $(call host_tests,$(NOVA2LITE_DONGLE)))
gamesir-nova-2-lite-all: $(NOVA2LITE_DONGLE)
gamesir-nova-2-lite-test: $(NOVA2LITE_DONGLE)-test
