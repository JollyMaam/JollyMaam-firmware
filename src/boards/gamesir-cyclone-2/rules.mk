# GameSir Cyclone 2: the pad and GameSir's dongle, both on the Jieli BR23.
# Each is the updater with the app above it (docs/firmware/updater.md).
#   gamesir-cyclone-2-pad, -dongle       firmware.img (and update.bin) in
#                                        $(OBJDIR)/<board>/
#   gamesir-cyclone-2-pad-updater, -dongle-updater   the updaters' app.bin
#   gamesir-cyclone-2-all                both
#   gamesir-cyclone-2-pad-test, -dongle-test, gamesir-cyclone-2-test   unit tests
# A board's targets, all in $(OBJDIR)/<board>/: sdk_ld.c (the SDK's linker
# script, edited), sdk.ld (preprocessed), app.elf (and app.map), app.bin;
# for the pad and the dongle also isd_config.ini and firmware.img.
# make/jieli-lib.mk has the toolchain, the source lists and the types.

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c
BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_dongle.c
BTSTACK_USERS += src/updater/updater_ble.c
BTSTACK_USERS += src/updater/sm_none.c

C2P := gamesir-cyclone-2-pad
C2P_DIR := $(OBJDIR)/$(C2P)
C2D := gamesir-cyclone-2-dongle
C2D_DIR := $(OBJDIR)/$(C2D)
C2PU := gamesir-cyclone-2-pad-updater
C2PU_DIR := $(OBJDIR)/$(C2PU)
C2DU := gamesir-cyclone-2-dongle-updater
C2DU_DIR := $(OBJDIR)/$(C2DU)

# --- The pad's updater (src/updater/): the app area's first 128 KiB ---------

C2PU_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"GS_C2_ADC_DEVICE\" -DBT_LE_ONLY -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DMAIN_AT=0x20000 -DCODE_LIMIT=$(call jieli_code_limit,0x100000)
C2PU_CPPFLAGS := $(C2PU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BOARD_INCLUDES)

C2PU_SOURCES :=
C2PU_SOURCES += src/updater/updater.c
C2PU_SOURCES += src/updater/updater_jieli.c
C2PU_SOURCES += src/updater/updater_ble.c
C2PU_SOURCES += src/updater/sm_none.c
C2PU_SOURCES += $(self)/board_pad.c
C2PU_SOURCES += src/platform/jieli/platform_jieli.c
C2PU_SOURCES += src/platform/jieli/platform_jieli_sdk.c
C2PU_SOURCES += src/platform/jieli/platform_jieli_usb.c
C2PU_SOURCES += src/platform/jieli/platform_jieli_noaudio.c
C2PU_SOURCES += src/platform/jieli/btstack_hci_jieli.c
C2PU_SOURCES += src/platform/jieli/btstack_qos_jieli.c
C2PU_SOURCES += src/core/utils.c
C2PU_SOURCES += src/core/battery.c
# LE only, no security manager.
C2PU_BTSTACK := $(addprefix $(BTSTACK)/,$(filter-out src/classic/% src/btstack_hid_parser.c src/ble/sm.c src/btstack_crypto.c,$(BTSTACK_USED)))

TARGETS += $(C2PU)/sdk_ld.c
$(C2PU)/sdk_ld.c_type := jieli-sdk-ld
$(C2PU)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(C2PU_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(JIELI_SDK_LD_SED_UPDATER)

TARGETS += $(C2PU)/sdk.ld
$(C2PU)/sdk.ld_type := copy
$(C2PU)/sdk.ld_toolchain := JIELI_
$(C2PU)/sdk.ld_sources := $(C2PU_DIR)/sdk_ld.c
$(C2PU)/sdk.ld_cppflags := $(C2PU_CPPFLAGS)
$(C2PU)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(C2PU)/app.elf
$(C2PU)/app.elf_toolchain := JIELI_
$(C2PU)/app.elf_sources := $(C2PU_SOURCES) $(JIELI_BTCTRLER_SOURCES) $(JIELI_SDK_SOURCES) $(C2PU_BTSTACK)
$(C2PU)/app.elf_cppflags := $(C2PU_CPPFLAGS)
$(C2PU)/app.elf_ldflags := -T$(C2PU_DIR)/sdk.ld -M=$(C2PU_DIR)/app.map
$(C2PU)/app.elf_deps := $(C2PU_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(C2PU)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(C2PU)/app.elf,$(JIELI_BTCTRLER_SOURCES)): obj_cflags += -Werror=implicit-function-declaration
$(call lib-objs,$(C2PU)/app.elf,$(C2PU_BTSTACK) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(C2PU)/app.bin
$(C2PU)/app.bin_type := jieli-bin
$(C2PU)/app.bin_deps := $(C2PU_DIR)/app.elf

.PHONY: $(C2PU)
$(C2PU): $(C2PU_DIR)/app.bin

# --- The pad: its app 0x20000 above the updater -----------------------------

C2P_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"GS_C2_ADC_DEVICE\" $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
C2P_CPPFLAGS := $(C2P_FLAGS) $(SRC_INCLUDES) -iquote $(self) $(JIELI_BOARD_INCLUDES)

C2P_SOURCES :=
C2P_SOURCES += $(self)/board_pad.c
C2P_SOURCES += $(self)/bt_pad.c
C2P_SOURCES += src/platform/jieli/btstack_hci_jieli.c
C2P_SOURCES += src/platform/jieli/btstack_qos_jieli.c
C2P_SOURCES += src/core/controller.c
C2P_SOURCES += src/core/input.c
C2P_SOURCES += src/core/input_calibration.c
C2P_SOURCES += src/core/motion.c
C2P_SOURCES += src/core/imu_i2c.c
C2P_SOURCES += src/core/leds.c
C2P_SOURCES += src/platform/jieli/platform_jieli_audio.c
C2P_SOURCES += src/platform/jieli/platform_jieli.c
C2P_SOURCES += src/platform/jieli/platform_jieli_sdk.c
C2P_SOURCES += src/platform/jieli/platform_jieli_usb.c
C2P_SOURCES += src/profiles/profile_hid.c
C2P_SOURCES += src/profiles/profile_link.c
C2P_SOURCES += src/profiles/profile_config.c
C2P_SOURCES += src/profiles/profile_playstation.c
C2P_SOURCES += src/profiles/profile_switch.c
C2P_SOURCES += src/profiles/profile_switch_wake.c
C2P_SOURCES += src/profiles/profile_xbox.c
C2P_SOURCES += src/core/remap.c
C2P_SOURCES += src/core/settings.c
C2P_SOURCES += src/core/settings_ux.c
C2P_SOURCES += src/core/usb.c
C2P_SOURCES += src/core/utils.c
C2P_SOURCES += src/core/battery.c

TARGETS += $(C2P)/sdk_ld.c
$(C2P)/sdk_ld.c_type := jieli-sdk-ld
$(C2P)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(C2P_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(call jieli_sdk_ld_main_at,0x20000)

TARGETS += $(C2P)/sdk.ld
$(C2P)/sdk.ld_type := copy
$(C2P)/sdk.ld_toolchain := JIELI_
$(C2P)/sdk.ld_sources := $(C2P_DIR)/sdk_ld.c
$(C2P)/sdk.ld_cppflags := $(C2P_CPPFLAGS)
$(C2P)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(C2P)/isd_config.ini
$(C2P)/isd_config.ini_type := jieli-isd
$(C2P)/isd_config.ini_toolchain := JIELI_
$(C2P)/isd_config.ini_sources := $(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c
$(C2P)/isd_config.ini_cppflags := $(C2P_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
$(C2P)/isd_config.ini_compile.c = $(jieli-cpp)
$(C2P_DIR)/isd_config.ini: private jieli_pid := GS_C2_ADC_DEVICE

TARGETS += $(C2P)/app.elf
$(C2P)/app.elf_toolchain := JIELI_
$(C2P)/app.elf_sources := $(C2P_SOURCES) $(JIELI_BTCTRLER_SOURCES) $(JIELI_SDK_SOURCES) $(BTSTACK_SOURCES)
$(C2P)/app.elf_cppflags := $(C2P_CPPFLAGS)
$(C2P)/app.elf_ldflags := -T$(C2P_DIR)/sdk.ld -M=$(C2P_DIR)/app.map
$(C2P)/app.elf_deps := $(C2P_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(C2P)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(C2P)/app.elf,$(JIELI_BTCTRLER_SOURCES)): obj_cflags += -Werror=implicit-function-declaration
$(call lib-objs,$(C2P)/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(C2P)/app.bin
$(C2P)/app.bin_type := jieli-bin
$(C2P)/app.bin_deps := $(C2P_DIR)/app.elf

TARGETS += $(C2P)/firmware.img
$(C2P)/firmware.img_type := jieli-img
$(C2P)/firmware.img_deps := $(C2PU_DIR)/app.bin $(C2P_DIR)/app.bin $(C2P_DIR)/isd_config.ini $(JIELI_UBOOT) $(JIELI_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(C2P_DIR)/firmware.img: private jieli_pack := --flash-size 0x100000 --code-limit $(call jieli_code_limit,0x100000) \
	-c $(C2P_DIR)/isd_config.ini -u $(JIELI_UBOOT) $(C2PU_DIR)/app.bin --main 0x20000:$(C2P_DIR)/app.bin $(JIELI_CONFIG_TOOL) \
	--main-out $(C2P_DIR)/update.bin

.PHONY: $(C2P)
$(C2P): $(C2P_DIR)/firmware.img

# --- The dongle's updater: USB only, the app area's first 64 KiB -------------

C2DU_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DNO_BT -DMAIN_AT=0x10000 -DCODE_LIMIT=$(call jieli_code_limit,0x80000)
C2DU_CPPFLAGS := $(C2DU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BOARD_INCLUDES)

C2DU_SOURCES :=
C2DU_SOURCES += src/updater/updater.c
C2DU_SOURCES += src/updater/updater_jieli.c
C2DU_SOURCES += $(self)/board_dongle.c
C2DU_SOURCES += src/platform/jieli/platform_jieli.c
C2DU_SOURCES += src/platform/jieli/platform_jieli_sdk.c
C2DU_SOURCES += src/platform/jieli/platform_jieli_usb.c
C2DU_SOURCES += src/platform/jieli/platform_jieli_noaudio.c
C2DU_SOURCES += src/core/utils.c
C2DU_SOURCES += src/core/battery.c

TARGETS += $(C2DU)/sdk_ld.c
$(C2DU)/sdk_ld.c_type := jieli-sdk-ld
$(C2DU)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(C2DU_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(JIELI_SDK_LD_SED_UPDATER)

TARGETS += $(C2DU)/sdk.ld
$(C2DU)/sdk.ld_type := copy
$(C2DU)/sdk.ld_toolchain := JIELI_
$(C2DU)/sdk.ld_sources := $(C2DU_DIR)/sdk_ld.c
$(C2DU)/sdk.ld_cppflags := $(C2DU_CPPFLAGS)
$(C2DU)/sdk.ld_compile.c = $(jieli-cpp)

# No Bluetooth: neither our controller nor BTstack.
TARGETS += $(C2DU)/app.elf
$(C2DU)/app.elf_toolchain := JIELI_
$(C2DU)/app.elf_sources := $(C2DU_SOURCES) $(JIELI_SDK_SOURCES)
$(C2DU)/app.elf_cppflags := $(C2DU_CPPFLAGS)
$(C2DU)/app.elf_ldflags := -T$(C2DU_DIR)/sdk.ld -M=$(C2DU_DIR)/app.map
$(C2DU)/app.elf_deps := $(C2DU_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(C2DU)/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += $(C2DU)/app.bin
$(C2DU)/app.bin_type := jieli-bin
$(C2DU)/app.bin_deps := $(C2DU_DIR)/app.elf

.PHONY: $(C2DU)
$(C2DU): $(C2DU_DIR)/app.bin

# --- The dongle: its app 0x10000 above the updater --------------------------

C2D_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
C2D_CPPFLAGS := $(C2D_FLAGS) $(SRC_INCLUDES) -iquote $(self) $(JIELI_BOARD_INCLUDES)

C2D_SOURCES :=
C2D_SOURCES += $(self)/board_dongle.c
C2D_SOURCES += src/platform/jieli/platform_jieli.c
C2D_SOURCES += src/platform/jieli/platform_jieli_sdk.c
C2D_SOURCES += src/platform/jieli/platform_jieli_usb.c
C2D_SOURCES += src/platform/jieli/platform_jieli_audio.c
C2D_SOURCES += $(self)/dongle.c
C2D_SOURCES += $(self)/bt_dongle.c
C2D_SOURCES += $(self)/dongle_protocol.c
C2D_SOURCES += src/platform/jieli/btstack_hci_jieli.c
C2D_SOURCES += src/platform/jieli/btstack_qos_jieli.c
C2D_SOURCES += src/core/usb.c
C2D_SOURCES += src/core/utils.c
C2D_SOURCES += src/core/motion.c
C2D_SOURCES += src/profiles/profile_link.c
C2D_SOURCES += src/profiles/profile_switch.c
C2D_SOURCES += src/profiles/profile_xbox.c
C2D_SOURCES += src/profiles/profile_playstation.c
C2D_SOURCES += src/profiles/profile_hid.c
C2D_SOURCES += src/core/battery.c

TARGETS += $(C2D)/sdk_ld.c
$(C2D)/sdk_ld.c_type := jieli-sdk-ld
$(C2D)/sdk_ld.c_deps := $(JIELI_SDK)/cpu/br23/sdk_ld.c
$(C2D_DIR)/sdk_ld.c: private jieli_sed := $(JIELI_SDK_LD_SED) $(call jieli_sdk_ld_main_at,0x10000)

TARGETS += $(C2D)/sdk.ld
$(C2D)/sdk.ld_type := copy
$(C2D)/sdk.ld_toolchain := JIELI_
$(C2D)/sdk.ld_sources := $(C2D_DIR)/sdk_ld.c
$(C2D)/sdk.ld_cppflags := $(C2D_CPPFLAGS)
$(C2D)/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += $(C2D)/isd_config.ini
$(C2D)/isd_config.ini_type := jieli-isd
$(C2D)/isd_config.ini_toolchain := JIELI_
$(C2D)/isd_config.ini_sources := $(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c
$(C2D)/isd_config.ini_cppflags := $(C2D_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
$(C2D)/isd_config.ini_compile.c = $(jieli-cpp)
$(C2D_DIR)/isd_config.ini: private jieli_pid := GS_C2_Dongle

TARGETS += $(C2D)/app.elf
$(C2D)/app.elf_toolchain := JIELI_
$(C2D)/app.elf_sources := $(C2D_SOURCES) $(JIELI_BTCTRLER_SOURCES) $(JIELI_SDK_SOURCES) $(BTSTACK_SOURCES)
$(C2D)/app.elf_cppflags := $(C2D_CPPFLAGS)
$(C2D)/app.elf_ldflags := -T$(C2D_DIR)/sdk.ld -M=$(C2D_DIR)/app.map
$(C2D)/app.elf_deps := $(C2D_DIR)/sdk.ld $(JIELI_ARCHIVES)
$(C2D)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(C2D)/app.elf,$(JIELI_BTCTRLER_SOURCES)): obj_cflags += -Werror=implicit-function-declaration
$(call lib-objs,$(C2D)/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += $(C2D)/app.bin
$(C2D)/app.bin_type := jieli-bin
$(C2D)/app.bin_deps := $(C2D_DIR)/app.elf

TARGETS += $(C2D)/firmware.img
$(C2D)/firmware.img_type := jieli-img
$(C2D)/firmware.img_deps := $(C2DU_DIR)/app.bin $(C2D_DIR)/app.bin $(C2D_DIR)/isd_config.ini $(JIELI_UBOOT) $(JIELI_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(C2D_DIR)/firmware.img: private jieli_pack := --flash-size 0x80000 --code-limit $(call jieli_code_limit,0x80000) \
	-c $(C2D_DIR)/isd_config.ini -u $(JIELI_UBOOT) $(C2DU_DIR)/app.bin --main 0x10000:$(C2D_DIR)/app.bin $(JIELI_CONFIG_TOOL) \
	--main-out $(C2D_DIR)/update.bin

.PHONY: $(C2D)
$(C2D): $(C2D_DIR)/firmware.img

# --- Unit tests (make/host-tests.mk), with the pad's and the dongle's defines

TARGETS += host/test-$(C2P)
host/test-$(C2P)_type := host-tests
host/test-$(C2P)_toolchain := HOST_
host/test-$(C2P)_sources := $(HOST_TESTS)
host/test-$(C2P)_cppflags := -DBOARD_PID=\"GS_C2_ADC_DEVICE\" $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote $(self)
host/test-$(C2P)_compile.c = $(host-test-compile)
$(call lib-objs,host/test-$(C2P),$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(C2P),$(HOST_TESTS_LIBJOLLYMAAM)): $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(C2P),$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

TARGETS += host/test-$(C2D)
host/test-$(C2D)_type := host-tests
host/test-$(C2D)_toolchain := HOST_
host/test-$(C2D)_sources := $(HOST_TESTS)
host/test-$(C2D)_cppflags := -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote $(self)
host/test-$(C2D)_compile.c = $(host-test-compile)
$(call lib-objs,host/test-$(C2D),$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(C2D),$(HOST_TESTS_LIBJOLLYMAAM)): $(OBJDIR)/host/libjollymaam.a
$(call lib-objs,host/test-$(C2D),$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

.PHONY: $(C2P)-test $(C2D)-test gamesir-cyclone-2-test
$(C2P)-test: $(OBJDIR)/host/test-$(C2P)
$(C2D)-test: $(OBJDIR)/host/test-$(C2D)
gamesir-cyclone-2-test: $(C2P)-test $(C2D)-test
test: gamesir-cyclone-2-test

.PHONY: gamesir-cyclone-2-all
gamesir-cyclone-2-all: $(C2P) $(C2D)
firmware: gamesir-cyclone-2-all
