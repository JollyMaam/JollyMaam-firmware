# GameSir Nova 2 Lite: the pad, a Jieli BR23 (stock PID AC695X_TRANS).
#   gamesir-nova-2-lite-flash        the pad: its updater and the app above
#                                    it; from stock firmware too: jlnewfw sends
#                                    it the HID command in docs/gamesir-nova-2-lite/
#   gamesir-nova-2-lite-pad          firmware.img in $(O)/<target>/: its
#                                    updater with the app above
#   gamesir-nova-2-lite-pad-updater  the pad's updater: part of -pad
#   gamesir-nova-2-lite-dongle       the dongle (a BD19): its updater with the
#                                    app above; -dongle-flash, -dongle-updater
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
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_config.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_playstation.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_switch.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_switch_wake.c
$(NOVA2LITE_PAD)_SOURCES += src/profiles/profile_xbox.c
$(NOVA2LITE_PAD)_SOURCES += src/core/remap.c
$(NOVA2LITE_PAD)_SOURCES += src/core/settings.c
$(NOVA2LITE_PAD)_SOURCES += src/core/settings_ux.c
$(NOVA2LITE_PAD)_SOURCES += src/core/usb.c
$(NOVA2LITE_PAD)_SOURCES += src/core/utils.c

# The pad's updater (src/updater/, docs/firmware/updater.md): in the app
# area, below stock's data at 0x64000; the app's region at flash 0x80000
# (0x7d000 above the app area), clear of it too.
NOVA2LITE_UPDATER := gamesir-nova-2-lite-pad-updater
$(NOVA2LITE_UPDATER)_FLASH_SIZE := $($(NOVA2LITE_PAD)_FLASH_SIZE)
$(NOVA2LITE_UPDATER)_PID := $($(NOVA2LITE_PAD)_PID)
$(NOVA2LITE_UPDATER)_BOARD_DIR := src/updater
$(NOVA2LITE_UPDATER)_UPDATER := 0x7d000
$(NOVA2LITE_UPDATER)_NO_FLASH := 1
$(NOVA2LITE_UPDATER)_DEFINES := -DBT_LE_ONLY -DUSB_TOOL_ONLY
$(NOVA2LITE_UPDATER)_BTSTACK := $($(CYCLONE2_UPDATER)_BTSTACK)
$(NOVA2LITE_UPDATER)_SOURCES := $(subst src/boards/gamesir-cyclone-2/board_pad.c,src/boards/gamesir-nova-2-lite/board_pad.c,$($(CYCLONE2_UPDATER)_SOURCES))

$(NOVA2LITE_PAD)_MAIN_AT := $($(NOVA2LITE_UPDATER)_UPDATER)
$(NOVA2LITE_PAD)_FRONT := $(NOVA2LITE_UPDATER)
$(NOVA2LITE_PAD)_KEEP := 0x64000:0x1c000       # stock's data: the image spans it

$(eval $(call jieli_board,$(NOVA2LITE_PAD)))$(eval $(call host_tests,$(NOVA2LITE_PAD)))
$(eval $(call jieli_board,$(NOVA2LITE_UPDATER)))

.PHONY: gamesir-nova-2-lite-all gamesir-nova-2-lite-test gamesir-nova-2-lite-flash
gamesir-nova-2-lite-all: $(NOVA2LITE_PAD)
gamesir-nova-2-lite-flash: $(NOVA2LITE_PAD)-flash
gamesir-nova-2-lite-test: $(NOVA2LITE_PAD)-test

# Nova dongle: BD19/AC632N (q32s), reusing the app's dongle link and USB profiles.
NOVA2LITE_DONGLE := gamesir-nova-2-lite-dongle
$(NOVA2LITE_DONGLE)_CHIP := bd19
$(NOVA2LITE_DONGLE)_FLASH_SIZE := 0x80000
$(NOVA2LITE_DONGLE)_PID := AC632N_TRANS
# One hub slot: each costs ~1.25 KB of the BD19's static RAM and an LE link's
# controller buffers from its heap.  With four (a 15 KB heap) the dongle hung
# at start-up and never came on USB; one leaves 19 KB.
$(NOVA2LITE_DONGLE)_DEFINES := -DBOARD_DONGLE -DHUB_SLOTS=1 -DLOG_LINES=16 -DTRACE=16 -DUSB_DMA_BUF_MAX_SIZE=512
# Stock's long-press reset: off (time 00).
$(NOVA2LITE_DONGLE)_DEFINES += -DCONFIG_RESET_PIN=PB02 -DCONFIG_RESET_TIME=00 -DCONFIG_RESET_LEVEL=0
$(NOVA2LITE_DONGLE)_BOARD_DIR := src/boards/gamesir-cyclone-2
$(NOVA2LITE_DONGLE)_LOADER_HINT := Hold the pairing button while connecting USB.
$(NOVA2LITE_DONGLE)_SOURCES := $(filter-out src/boards/gamesir-cyclone-2/board_dongle.c src/platform/jieli/platform_jieli_audio.c src/platform/jieli/btstack_qos_jieli.c,$($(CYCLONE2_DONGLE)_SOURCES))
$(NOVA2LITE_DONGLE)_SOURCES += src/boards/gamesir-nova-2-lite/board_dongle.c src/platform/jieli/platform_jieli_noaudio.c

# The dongle's updater: USB only, in the app area (64 KiB), the dongle's app
# above it.
NOVA2LITE_DONGLE_UPDATER := gamesir-nova-2-lite-dongle-updater
$(NOVA2LITE_DONGLE_UPDATER)_CHIP := bd19
$(NOVA2LITE_DONGLE_UPDATER)_FLASH_SIZE := $($(NOVA2LITE_DONGLE)_FLASH_SIZE)
$(NOVA2LITE_DONGLE_UPDATER)_PID := $($(NOVA2LITE_DONGLE)_PID)
$(NOVA2LITE_DONGLE_UPDATER)_BOARD_DIR := src/updater
$(NOVA2LITE_DONGLE_UPDATER)_UPDATER := 0x10000
$(NOVA2LITE_DONGLE_UPDATER)_NO_FLASH := 1
$(NOVA2LITE_DONGLE_UPDATER)_NO_BT := 1
$(NOVA2LITE_DONGLE_UPDATER)_DEFINES := $(filter-out -DHUB_SLOTS=%,$($(NOVA2LITE_DONGLE)_DEFINES)) -DUSB_TOOL_ONLY
$(NOVA2LITE_DONGLE_UPDATER)_SOURCES := $(subst src/boards/gamesir-cyclone-2/board_dongle.c,src/boards/gamesir-nova-2-lite/board_dongle.c,$($(CYCLONE2_DONGLE_UPDATER)_SOURCES))

$(NOVA2LITE_DONGLE)_MAIN_AT := $($(NOVA2LITE_DONGLE_UPDATER)_UPDATER)
$(NOVA2LITE_DONGLE)_FRONT := $(NOVA2LITE_DONGLE_UPDATER)
$(eval $(call jieli_board,$(NOVA2LITE_DONGLE)))$(eval $(call host_tests,$(NOVA2LITE_DONGLE)))
$(eval $(call jieli_board,$(NOVA2LITE_DONGLE_UPDATER)))
gamesir-nova-2-lite-all: $(NOVA2LITE_DONGLE)
gamesir-nova-2-lite-test: $(NOVA2LITE_DONGLE)-test
