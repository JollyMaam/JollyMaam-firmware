# GameSir Nova 2 Lite: the pad, a Jieli BR23 (stock PID AC695X_TRANS), and
# its dongle, a BD19 (AC632N). Each is the updater with the app above it.
#   gamesir-nova-2-lite-pad, -dongle     firmware.img (and update.bin) in
#                                        $(OBJDIR)/<board>/
#   gamesir-nova-2-lite-pad-updater, -dongle-updater   the updaters' app.bin
#   gamesir-nova-2-lite-pad-test, -dongle-test, gamesir-nova-2-lite-test
#   gamesir-nova-2-lite-all              both
# Until the shared parts move out of src/boards/gamesir-cyclone-2/, these
# boards use the Cyclone 2's bt_pad.c, dongle*.c and btstack_config.h.
# The flash is 1 MiB; stock declares 512 KiB and keeps its VM, BTIF (MAC),
# EXIF, USERIF and key_mac at 0x64000..0x80000.  Declaring 1 MiB puts ours
# in the empty upper half, so an image of under 0x64000 leaves stock's alone.
# make/jieli-lib.mk has the toolchains, the source lists and the types.

C2_DIR := src/boards/gamesir-cyclone-2
N2P := gamesir-nova-2-lite-pad
N2P_DIR := $(OBJDIR)/$(N2P)
N2D := gamesir-nova-2-lite-dongle
N2D_DIR := $(OBJDIR)/$(N2D)
N2PU := gamesir-nova-2-lite-pad-updater
N2PU_DIR := $(OBJDIR)/$(N2PU)
N2DU := gamesir-nova-2-lite-dongle-updater
N2DU_DIR := $(OBJDIR)/$(N2DU)

# --- The pad's updater (src/updater/): in the app area, below stock's data at
# 0x64000; the app's region at flash 0x80000 (0x7d000 above the app area),
# clear of it too --------------------------------------------------------------

N2PU_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"AC695X_TRANS\" -DBT_LE_ONLY -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DMAIN_AT=0x7d000 -DCODE_LIMIT=$(call jieli_code_limit,0x100000)
N2PU_CPPFLAGS := $(N2PU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BOARD_INCLUDES)

N2PU_SOURCES :=
N2PU_SOURCES += src/updater/updater.c
N2PU_SOURCES += src/updater/updater_jieli.c
N2PU_SOURCES += src/updater/updater_ble.c
N2PU_SOURCES += src/updater/sm_none.c
N2PU_SOURCES += $(self)/board_pad.c
N2PU_SOURCES += src/platform/jieli/platform_jieli.c
N2PU_SOURCES += src/platform/jieli/platform_jieli_sdk.c
N2PU_SOURCES += src/platform/jieli/platform_jieli_usb.c
N2PU_SOURCES += src/platform/jieli/platform_jieli_noaudio.c
N2PU_SOURCES += src/platform/jieli/btstack_hci_jieli.c
N2PU_SOURCES += src/platform/jieli/btstack_qos_jieli.c
N2PU_SOURCES += src/core/utils.c
N2PU_SOURCES += src/core/battery.c
# LE only, no security manager.
N2PU_BTSTACK := $(addprefix $(BTSTACK)/,$(filter-out src/classic/% src/btstack_hid_parser.c src/ble/sm.c src/btstack_crypto.c,$(BTSTACK_USED)))

TARGETS += $(N2PU)/sdk_ld.c
$(N2PU)/sdk_ld.c_type := jieli-sdk-ld
$(N2PU)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(N2PU_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(JIELI_SDK_LD_SED_UPDATER)

TARGETS += $(N2PU)/sdk.ld
$(N2PU)/sdk.ld_type := copy
$(N2PU)/sdk.ld_toolchain := JIELI_
$(N2PU)/sdk.ld_sources := $(N2PU_DIR)/sdk_ld.c
$(N2PU)/sdk.ld_cppflags := $(N2PU_CPPFLAGS)
$(N2PU)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(N2PU)/app.elf
$(N2PU)/app.elf_toolchain := JIELI_
$(N2PU)/app.elf_sources := $(N2PU_SOURCES) $(JIELI_BTCTRLER_SOURCES) $(JIELI_SDK_SOURCES) $(N2PU_BTSTACK)
$(N2PU)/app.elf_cppflags := $(N2PU_CPPFLAGS)
$(N2PU)/app.elf_ldflags := -T$(N2PU_DIR)/sdk.ld -M=$(N2PU_DIR)/app.map
$(N2PU)/app.elf_deps := $(N2PU_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(N2PU)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(N2PU)/app.elf,$(JIELI_BTCTRLER_SOURCES)): obj_cflags += -Werror=implicit-function-declaration
$(call lib-objs,$(N2PU)/app.elf,$(N2PU_BTSTACK) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(N2PU)/app.bin
$(N2PU)/app.bin_type := jieli-bin
$(N2PU)/app.bin_deps := $(N2PU_DIR)/app.elf

.PHONY: $(N2PU)
$(N2PU): $(N2PU_DIR)/app.bin

# --- The pad: its app 0x7d000 above the updater ------------------------------

N2P_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"AC695X_TRANS\" $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
N2P_CPPFLAGS := $(N2P_FLAGS) $(SRC_INCLUDES) -iquote $(C2_DIR) $(JIELI_BOARD_INCLUDES)

N2P_SOURCES :=
N2P_SOURCES += $(self)/board_pad.c
N2P_SOURCES += $(C2_DIR)/bt_pad.c
N2P_SOURCES += src/platform/jieli/btstack_hci_jieli.c
N2P_SOURCES += src/platform/jieli/btstack_qos_jieli.c
N2P_SOURCES += src/core/controller.c
N2P_SOURCES += src/core/input.c
N2P_SOURCES += src/core/input_calibration.c
N2P_SOURCES += src/core/motion.c
N2P_SOURCES += src/core/leds.c
N2P_SOURCES += src/platform/jieli/platform_jieli_audio.c
N2P_SOURCES += src/platform/jieli/platform_jieli.c
N2P_SOURCES += src/platform/jieli/platform_jieli_sdk.c
N2P_SOURCES += src/platform/jieli/platform_jieli_usb.c
N2P_SOURCES += src/profiles/profile_hid.c
N2P_SOURCES += src/profiles/profile_link.c
N2P_SOURCES += src/profiles/profile_config.c
N2P_SOURCES += src/profiles/profile_playstation.c
N2P_SOURCES += src/profiles/profile_switch.c
N2P_SOURCES += src/profiles/profile_switch_wake.c
N2P_SOURCES += src/profiles/profile_xbox.c
N2P_SOURCES += src/core/remap.c
N2P_SOURCES += src/core/settings.c
N2P_SOURCES += src/core/settings_ux.c
N2P_SOURCES += src/core/usb.c
N2P_SOURCES += src/core/utils.c
N2P_SOURCES += src/core/battery.c

TARGETS += $(N2P)/sdk_ld.c
$(N2P)/sdk_ld.c_type := jieli-sdk-ld
$(N2P)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(N2P_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(call jieli_sdk_ld_main_at,0x7d000)

TARGETS += $(N2P)/sdk.ld
$(N2P)/sdk.ld_type := copy
$(N2P)/sdk.ld_toolchain := JIELI_
$(N2P)/sdk.ld_sources := $(N2P_DIR)/sdk_ld.c
$(N2P)/sdk.ld_cppflags := $(N2P_CPPFLAGS)
$(N2P)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(N2P)/isd_config.ini
$(N2P)/isd_config.ini_type := jieli-isd
$(N2P)/isd_config.ini_toolchain := JIELI_
$(N2P)/isd_config.ini_sources := $(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c
$(N2P)/isd_config.ini_cppflags := $(N2P_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
$(N2P)/isd_config.ini_compile.c = $(jieli-cpp)
$(N2P_DIR)/isd_config.ini: private jieli_pid := AC695X_TRANS

TARGETS += $(N2P)/app.elf
$(N2P)/app.elf_toolchain := JIELI_
$(N2P)/app.elf_sources := $(N2P_SOURCES) $(JIELI_BTCTRLER_SOURCES) $(JIELI_SDK_SOURCES) $(BTSTACK_SOURCES)
$(N2P)/app.elf_cppflags := $(N2P_CPPFLAGS)
$(N2P)/app.elf_ldflags := -T$(N2P_DIR)/sdk.ld -M=$(N2P_DIR)/app.map
$(N2P)/app.elf_deps := $(N2P_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(N2P)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(N2P)/app.elf,$(JIELI_BTCTRLER_SOURCES)): obj_cflags += -Werror=implicit-function-declaration
$(call lib-objs,$(N2P)/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(N2P)/app.bin
$(N2P)/app.bin_type := jieli-bin
$(N2P)/app.bin_deps := $(N2P_DIR)/app.elf

TARGETS += $(N2P)/firmware.img
$(N2P)/firmware.img_type := jieli-img
$(N2P)/firmware.img_deps := $(N2PU_DIR)/app.bin $(N2P_DIR)/app.bin $(N2P_DIR)/isd_config.ini $(JIELI_UBOOT) $(JIELI_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(N2P_DIR)/firmware.img: private jieli_pack := --flash-size 0x100000 --code-limit $(call jieli_code_limit,0x100000) \
	-c $(N2P_DIR)/isd_config.ini -u $(JIELI_UBOOT) $(N2PU_DIR)/app.bin --main 0x7d000:$(N2P_DIR)/app.bin $(JIELI_CONFIG_TOOL) \
	--main-out $(N2P_DIR)/update.bin

.PHONY: $(N2P)
$(N2P): $(N2P_DIR)/firmware.img

# --- The dongle's updater (BD19): USB only, the app area's first 64 KiB ------

# The dongle's own defines, without its hub slots for the updater.
N2D_DEFINES := -DBOARD_DONGLE -DHUB_SLOTS=1 -DLOG_LINES=16 -DTRACE=16 -DUSB_DMA_BUF_MAX_SIZE=512
# Stock's long-press reset: off (time 00).
N2D_DEFINES += -DCONFIG_RESET_PIN=PB02 -DCONFIG_RESET_TIME=00 -DCONFIG_RESET_LEVEL=0

N2DU_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"AC632N_TRANS\" $(filter-out -DHUB_SLOTS=%,$(N2D_DEFINES)) -DUSB_TOOL_ONLY \
	$(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) -DNO_BT -DMAIN_AT=0x10000 -DCODE_LIMIT=$(call jieli_code_limit,0x80000)
N2DU_CPPFLAGS := $(N2DU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BD19_BOARD_INCLUDES)

N2DU_SOURCES :=
N2DU_SOURCES += src/updater/updater.c
N2DU_SOURCES += src/updater/updater_jieli.c
N2DU_SOURCES += $(self)/board_dongle.c
N2DU_SOURCES += src/platform/jieli/platform_jieli.c
N2DU_SOURCES += src/platform/jieli/platform_jieli_sdk.c
N2DU_SOURCES += src/platform/jieli/platform_jieli_usb.c
N2DU_SOURCES += src/platform/jieli/platform_jieli_noaudio.c
N2DU_SOURCES += src/core/utils.c
N2DU_SOURCES += src/core/battery.c

TARGETS += $(N2DU)/sdk_ld.c
$(N2DU)/sdk_ld.c_type := jieli-bd19-sdk-ld
$(N2DU)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/bd19/sdk_ld.c
$(N2DU_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(JIELI_SDK_LD_SED_UPDATER)

TARGETS += $(N2DU)/sdk.ld
$(N2DU)/sdk.ld_type := copy
$(N2DU)/sdk.ld_toolchain := JIELI_BD19_
$(N2DU)/sdk.ld_sources := $(N2DU_DIR)/sdk_ld.c
$(N2DU)/sdk.ld_cppflags := $(N2DU_CPPFLAGS)
$(N2DU)/sdk.ld_compile.c = $(jieli-cpp)

# No Bluetooth: no BTstack (and the BD19 never has our controller).
TARGETS += $(N2DU)/app.elf
$(N2DU)/app.elf_toolchain := JIELI_BD19_
$(N2DU)/app.elf_sources := $(N2DU_SOURCES) $(JIELI_BD19_SDK_SOURCES)
$(N2DU)/app.elf_cppflags := $(N2DU_CPPFLAGS)
$(N2DU)/app.elf_ldflags := -T$(N2DU_DIR)/sdk.ld -M=$(N2DU_DIR)/app.map
$(N2DU)/app.elf_deps := $(N2DU_DIR)/sdk.ld $(JIELI_BD19_ARCHIVES)
$(N2DU)/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += $(N2DU)/app.bin
$(N2DU)/app.bin_type := jieli-bd19-bin
$(N2DU)/app.bin_deps := $(N2DU_DIR)/app.elf

.PHONY: $(N2DU)
$(N2DU): $(N2DU_DIR)/app.bin

# --- The dongle (BD19): its app 0x10000 above the updater --------------------

N2D_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"AC632N_TRANS\" $(N2D_DEFINES) $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
N2D_CPPFLAGS := $(N2D_FLAGS) $(SRC_INCLUDES) -iquote $(C2_DIR) $(JIELI_BD19_BOARD_INCLUDES)

# The Cyclone 2 dongle's, with this board's file and without audio or QoS.
N2D_SOURCES :=
N2D_SOURCES += src/platform/jieli/platform_jieli.c
N2D_SOURCES += src/platform/jieli/platform_jieli_sdk.c
N2D_SOURCES += src/platform/jieli/platform_jieli_usb.c
N2D_SOURCES += $(C2_DIR)/dongle.c
N2D_SOURCES += $(C2_DIR)/bt_dongle.c
N2D_SOURCES += $(C2_DIR)/dongle_protocol.c
N2D_SOURCES += src/platform/jieli/btstack_hci_jieli.c
N2D_SOURCES += src/core/usb.c
N2D_SOURCES += src/core/utils.c
N2D_SOURCES += src/core/motion.c
N2D_SOURCES += src/profiles/profile_link.c
N2D_SOURCES += src/profiles/profile_switch.c
N2D_SOURCES += src/profiles/profile_xbox.c
N2D_SOURCES += src/profiles/profile_playstation.c
N2D_SOURCES += src/profiles/profile_hid.c
N2D_SOURCES += $(self)/board_dongle.c
N2D_SOURCES += src/platform/jieli/platform_jieli_noaudio.c
N2D_SOURCES += src/core/battery.c

TARGETS += $(N2D)/sdk_ld.c
$(N2D)/sdk_ld.c_type := jieli-bd19-sdk-ld
$(N2D)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/bd19/sdk_ld.c
$(N2D_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(call jieli_sdk_ld_main_at,0x10000)

TARGETS += $(N2D)/sdk.ld
$(N2D)/sdk.ld_type := copy
$(N2D)/sdk.ld_toolchain := JIELI_BD19_
$(N2D)/sdk.ld_sources := $(N2D_DIR)/sdk_ld.c
$(N2D)/sdk.ld_cppflags := $(N2D_CPPFLAGS)
$(N2D)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(N2D)/isd_config.ini
$(N2D)/isd_config.ini_type := jieli-bd19-isd
$(N2D)/isd_config.ini_toolchain := JIELI_BD19_
$(N2D)/isd_config.ini_sources := $(JIELI_SDK)/cpu/bd19/tools/isd_config_rule.c
$(N2D)/isd_config.ini_cppflags := $(N2D_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
$(N2D)/isd_config.ini_compile.c = $(jieli-cpp)
$(N2D_DIR)/isd_config.ini: private jieli_pid := AC632N_TRANS

TARGETS += $(N2D)/app.elf
$(N2D)/app.elf_toolchain := JIELI_BD19_
$(N2D)/app.elf_sources := $(N2D_SOURCES) $(JIELI_BD19_SDK_SOURCES) $(BTSTACK_SOURCES)
$(N2D)/app.elf_cppflags := $(N2D_CPPFLAGS)
$(N2D)/app.elf_ldflags := -T$(N2D_DIR)/sdk.ld -M=$(N2D_DIR)/app.map
$(N2D)/app.elf_deps := $(N2D_DIR)/sdk.ld $(JIELI_BD19_ARCHIVES)
$(N2D)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(N2D)/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(N2D)/app.bin
$(N2D)/app.bin_type := jieli-bd19-bin
$(N2D)/app.bin_deps := $(N2D_DIR)/app.elf

TARGETS += $(N2D)/firmware.img
$(N2D)/firmware.img_type := jieli-img
$(N2D)/firmware.img_deps := $(N2DU_DIR)/app.bin $(N2D_DIR)/app.bin $(N2D_DIR)/isd_config.ini $(JIELI_BD19_UBOOT) $(JIELI_BD19_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(N2D_DIR)/firmware.img: private jieli_pack := --flash-size 0x80000 --code-limit $(call jieli_code_limit,0x80000) \
	-c $(N2D_DIR)/isd_config.ini -u $(JIELI_BD19_UBOOT) $(N2DU_DIR)/app.bin --main 0x10000:$(N2D_DIR)/app.bin $(JIELI_BD19_CONFIG_TOOL) \
	--main-out $(N2D_DIR)/update.bin

.PHONY: $(N2D)
$(N2D): $(N2D_DIR)/firmware.img

# --- Unit tests (make/host-tests.mk), with the pad's and the dongle's defines

TARGETS += host/test-$(N2P)
host/test-$(N2P)_type := host-tests
host/test-$(N2P)_toolchain := HOST_
host/test-$(N2P)_sources := $(HOST_TESTS)
host/test-$(N2P)_cppflags := -DBOARD_PID=\"AC695X_TRANS\" $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote $(C2_DIR)
host/test-$(N2P)_compile.c = $(host-test-compile)
$(call lib-objs,host/test-$(N2P),$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(N2P),$(HOST_TESTS_LIBJOLLYMAAM)): $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(N2P),$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

TARGETS += host/test-$(N2D)
host/test-$(N2D)_type := host-tests
host/test-$(N2D)_toolchain := HOST_
host/test-$(N2D)_sources := $(HOST_TESTS)
host/test-$(N2D)_cppflags := -DBOARD_PID=\"AC632N_TRANS\" $(N2D_DEFINES) $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote $(C2_DIR)
host/test-$(N2D)_compile.c = $(host-test-compile)
$(call lib-objs,host/test-$(N2D),$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(N2D),$(HOST_TESTS_LIBJOLLYMAAM)): $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(N2D),$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

.PHONY: $(N2P)-test $(N2D)-test gamesir-nova-2-lite-test
$(N2P)-test: $(OBJDIR)/host/test-$(N2P)
$(N2D)-test: $(OBJDIR)/host/test-$(N2D)
gamesir-nova-2-lite-test: $(N2P)-test $(N2D)-test
test: gamesir-nova-2-lite-test

.PHONY: gamesir-nova-2-lite-all
gamesir-nova-2-lite-all: $(N2P) $(N2D)
firmware: gamesir-nova-2-lite-all
