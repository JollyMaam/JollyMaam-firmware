HELP_TARGETS += c2
$(FIRMWARE_ZIP): $(O)/c2/pad/firmware.img $(O)/c2/dongle/firmware.img

firmware: c2
test: c2-test

c2: c2/pad c2/dongle
c2/pad: $(O)/c2/pad/firmware.img
c2/dongle: $(O)/c2/dongle/firmware.img
c2/pad-test: $(O)/host/test-c2/pad
c2/dongle-test: $(O)/host/test-c2/dongle
c2-test: c2/pad-test c2/dongle-test

.PHONY: c2 c2/pad c2/dongle c2-test c2/pad-test c2/dongle-test

BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_pad.c
BTSTACK_USERS += src/boards/gamesir-cyclone-2/bt_dongle.c
BTSTACK_USERS += src/updater/updater_ble.c
BTSTACK_USERS += src/updater/sm_none.c

# --- Pad updater: the app area's first 128 KiB

C2PU_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"GS_C2_ADC_DEVICE\" -DBT_LE_ONLY -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DMAIN_AT=0x20000 -DCODE_LIMIT=$(call jieli_code_limit,0x100000)
C2PU_CPPFLAGS := $(C2PU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BR23_BOARD_INCLUDES)

C2PU_SOURCES :=
C2PU_SOURCES += $(JIELI_UPDATER_SOURCES)
C2PU_SOURCES += $(JIELI_UPDATER_BLE_SOURCES)
C2PU_SOURCES += src/boards/gamesir-cyclone-2/board_pad.c
C2PU_SOURCES += $(JIELI_PLATFORM_SOURCES)
C2PU_SOURCES += $(JIELI_NOAUDIO_SOURCES)
C2PU_SOURCES += $(JIELI_BTSTACK_SOURCES)
C2PU_SOURCES += $(CORE_SOURCES)
# LE only, no security manager.
C2PU_BTSTACK := $(BTSTACK_CORE_SOURCES) $(BTSTACK_LE_SOURCES)

TARGETS += c2/pad-updater/sdk.ld
c2/pad-updater/sdk.ld_type := copy
c2/pad-updater/sdk.ld_toolchain := JIELI_BR23_
c2/pad-updater/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
c2/pad-updater/sdk.ld_cppflags := $(C2PU_CPPFLAGS) -DLD_UPDATER
c2/pad-updater/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += c2/pad-updater/app.elf
c2/pad-updater/app.elf_toolchain := JIELI_BR23_
c2/pad-updater/app.elf_sources := $(C2PU_SOURCES) $(JIELI_BR23_CONTROLLER_SOURCES) $(JIELI_BR23_SDK_SOURCES) $(C2PU_BTSTACK)
c2/pad-updater/app.elf_cppflags := $(C2PU_CPPFLAGS)
c2/pad-updater/app.elf_ldflags := -T$(O)/c2/pad-updater/sdk.ld -M=$(O)/c2/pad-updater/app.map
c2/pad-updater/app.elf_deps := $(O)/c2/pad-updater/sdk.ld $(JIELI_BR23_ARCHIVES)
c2/pad-updater/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,c2/pad-updater/app.elf,$(JIELI_BR23_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BR23_CONTROLLER_CFLAGS)
$(call lib-objs,c2/pad-updater/app.elf,$(C2PU_BTSTACK) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += c2/pad-updater/app.bin
c2/pad-updater/app.bin_type := jieli-br23-bin
c2/pad-updater/app.bin_deps := $(O)/c2/pad-updater/app.elf

# --- Pad: app 0x20000 above the updater

C2P_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"GS_C2_ADC_DEVICE\" $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
C2P_CPPFLAGS := $(C2P_FLAGS) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 $(JIELI_BR23_BOARD_INCLUDES)

C2P_SOURCES :=
C2P_SOURCES += src/boards/gamesir-cyclone-2/board_pad.c
C2P_SOURCES += src/boards/gamesir-cyclone-2/bt_pad.c
C2P_SOURCES += src/core/imu_i2c.c
C2P_SOURCES += $(PAD_SOURCES)
C2P_SOURCES += $(PROFILE_SOURCES)
C2P_SOURCES += $(PROFILE_PAD_SOURCES)
C2P_SOURCES += $(JIELI_PAD_WAKE_SOURCES)
C2P_SOURCES += $(CORE_SOURCES)
C2P_SOURCES += $(JIELI_PLATFORM_SOURCES)
C2P_SOURCES += $(JIELI_AUDIO_SOURCES)
C2P_SOURCES += $(JIELI_BTSTACK_SOURCES)

TARGETS += c2/pad/sdk.ld
c2/pad/sdk.ld_type := copy
c2/pad/sdk.ld_toolchain := JIELI_BR23_
c2/pad/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
c2/pad/sdk.ld_cppflags := $(C2P_CPPFLAGS) -DLD_CODE_AT=0x20000
c2/pad/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += c2/pad/isd_config.ini
c2/pad/isd_config.ini_type := jieli-br23-isd
c2/pad/isd_config.ini_toolchain := JIELI_BR23_
c2/pad/isd_config.ini_sources := $(JIELI_BR23_ISD_CONFIG)
c2/pad/isd_config.ini_cppflags := $(C2P_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
c2/pad/isd_config.ini_compile.c = $(jieli-cpp)
$(O)/c2/pad/isd_config.ini: private jieli_pid := GS_C2_ADC_DEVICE

TARGETS += c2/pad/app.elf
c2/pad/app.elf_toolchain := JIELI_BR23_
c2/pad/app.elf_sources := $(C2P_SOURCES) $(JIELI_BR23_CONTROLLER_SOURCES) $(JIELI_BR23_SDK_SOURCES) $(BTSTACK_SOURCES)
c2/pad/app.elf_cppflags := $(C2P_CPPFLAGS)
c2/pad/app.elf_ldflags := -T$(O)/c2/pad/sdk.ld -M=$(O)/c2/pad/app.map
c2/pad/app.elf_deps := $(O)/c2/pad/sdk.ld $(JIELI_BR23_ARCHIVES)
c2/pad/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,c2/pad/app.elf,$(JIELI_BR23_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BR23_CONTROLLER_CFLAGS)
$(call lib-objs,c2/pad/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += c2/pad/app.bin
c2/pad/app.bin_type := jieli-br23-bin
c2/pad/app.bin_deps := $(O)/c2/pad/app.elf

TARGETS += c2/pad/firmware.img
c2/pad/firmware.img_type := jieli-img
c2/pad/firmware.img_deps := $(O)/c2/pad-updater/app.bin $(O)/c2/pad/app.bin $(O)/c2/pad/isd_config.ini $(JIELI_BR23_UBOOT) $(JIELI_BR23_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(O)/c2/pad/firmware.img: private jieli_pack := --flash-size 0x100000 --code-limit $(call jieli_code_limit,0x100000) \
	-c $(O)/c2/pad/isd_config.ini -u $(JIELI_BR23_UBOOT) $(O)/c2/pad-updater/app.bin --main 0x20000:$(O)/c2/pad/app.bin $(JIELI_BR23_CONFIG_TOOL) \
	--main-out $(O)/c2/pad/update.bin

# --- Dongle updater: USB only, the app area's first 64 KiB

C2DU_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DNO_BT -DMAIN_AT=0x10000 -DCODE_LIMIT=$(call jieli_code_limit,0x80000)
C2DU_CPPFLAGS := $(C2DU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BR23_BOARD_INCLUDES)

C2DU_SOURCES :=
C2DU_SOURCES += $(JIELI_UPDATER_SOURCES)
C2DU_SOURCES += src/boards/gamesir-cyclone-2/board_dongle.c
C2DU_SOURCES += $(JIELI_PLATFORM_SOURCES)
C2DU_SOURCES += $(JIELI_NOAUDIO_SOURCES)
C2DU_SOURCES += $(CORE_SOURCES)

TARGETS += c2/dongle-updater/sdk.ld
c2/dongle-updater/sdk.ld_type := copy
c2/dongle-updater/sdk.ld_toolchain := JIELI_BR23_
c2/dongle-updater/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
c2/dongle-updater/sdk.ld_cppflags := $(C2DU_CPPFLAGS) -DLD_UPDATER
c2/dongle-updater/sdk.ld_compile.c = $(jieli-cpp)

# No Bluetooth: neither our controller nor BTstack.
TARGETS += c2/dongle-updater/app.elf
c2/dongle-updater/app.elf_toolchain := JIELI_BR23_
c2/dongle-updater/app.elf_sources := $(C2DU_SOURCES) $(JIELI_BR23_SDK_SOURCES)
c2/dongle-updater/app.elf_cppflags := $(C2DU_CPPFLAGS)
c2/dongle-updater/app.elf_ldflags := -T$(O)/c2/dongle-updater/sdk.ld -M=$(O)/c2/dongle-updater/app.map
c2/dongle-updater/app.elf_deps := $(O)/c2/dongle-updater/sdk.ld $(JIELI_BR23_ARCHIVES)
c2/dongle-updater/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += c2/dongle-updater/app.bin
c2/dongle-updater/app.bin_type := jieli-br23-bin
c2/dongle-updater/app.bin_deps := $(O)/c2/dongle-updater/app.elf

# --- Dongle: app 0x10000 above the updater

C2D_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
C2D_CPPFLAGS := $(C2D_FLAGS) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 $(JIELI_BR23_BOARD_INCLUDES)

C2D_SOURCES :=
C2D_SOURCES += src/boards/gamesir-cyclone-2/board_dongle.c
C2D_SOURCES += src/boards/gamesir-cyclone-2/dongle.c
C2D_SOURCES += src/boards/gamesir-cyclone-2/bt_dongle.c
C2D_SOURCES += src/boards/gamesir-cyclone-2/dongle_protocol.c
C2D_SOURCES += $(DONGLE_SOURCES)
C2D_SOURCES += $(PROFILE_SOURCES)
C2D_SOURCES += $(CORE_SOURCES)
C2D_SOURCES += $(JIELI_PLATFORM_SOURCES)
C2D_SOURCES += $(JIELI_AUDIO_SOURCES)
C2D_SOURCES += $(JIELI_BTSTACK_SOURCES)

TARGETS += c2/dongle/sdk.ld
c2/dongle/sdk.ld_type := copy
c2/dongle/sdk.ld_toolchain := JIELI_BR23_
c2/dongle/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
c2/dongle/sdk.ld_cppflags := $(C2D_CPPFLAGS) -DLD_CODE_AT=0x10000
c2/dongle/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += c2/dongle/isd_config.ini
c2/dongle/isd_config.ini_type := jieli-br23-isd
c2/dongle/isd_config.ini_toolchain := JIELI_BR23_
c2/dongle/isd_config.ini_sources := $(JIELI_BR23_ISD_CONFIG)
c2/dongle/isd_config.ini_cppflags := $(C2D_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
c2/dongle/isd_config.ini_compile.c = $(jieli-cpp)
$(O)/c2/dongle/isd_config.ini: private jieli_pid := GS_C2_Dongle

TARGETS += c2/dongle/app.elf
c2/dongle/app.elf_toolchain := JIELI_BR23_
c2/dongle/app.elf_sources := $(C2D_SOURCES) $(JIELI_BR23_CONTROLLER_SOURCES) $(JIELI_BR23_SDK_SOURCES) $(BTSTACK_SOURCES)
c2/dongle/app.elf_cppflags := $(C2D_CPPFLAGS)
c2/dongle/app.elf_ldflags := -T$(O)/c2/dongle/sdk.ld -M=$(O)/c2/dongle/app.map
c2/dongle/app.elf_deps := $(O)/c2/dongle/sdk.ld $(JIELI_BR23_ARCHIVES)
c2/dongle/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,c2/dongle/app.elf,$(JIELI_BR23_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BR23_CONTROLLER_CFLAGS)
$(call lib-objs,c2/dongle/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += c2/dongle/app.bin
c2/dongle/app.bin_type := jieli-br23-bin
c2/dongle/app.bin_deps := $(O)/c2/dongle/app.elf

TARGETS += c2/dongle/firmware.img
c2/dongle/firmware.img_type := jieli-img
c2/dongle/firmware.img_deps := $(O)/c2/dongle-updater/app.bin $(O)/c2/dongle/app.bin $(O)/c2/dongle/isd_config.ini $(JIELI_BR23_UBOOT) $(JIELI_BR23_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(O)/c2/dongle/firmware.img: private jieli_pack := --flash-size 0x80000 --code-limit $(call jieli_code_limit,0x80000) \
	-c $(O)/c2/dongle/isd_config.ini -u $(JIELI_BR23_UBOOT) $(O)/c2/dongle-updater/app.bin --main 0x10000:$(O)/c2/dongle/app.bin $(JIELI_BR23_CONFIG_TOOL) \
	--main-out $(O)/c2/dongle/update.bin

# --- Tests

TARGETS += host/test-c2/pad
host/test-c2/pad_type := host-tests
host/test-c2/pad_toolchain := HOST_
host/test-c2/pad_sources := $(HOST_TESTS)
host/test-c2/pad_cppflags := -DBOARD_PID=\"GS_C2_ADC_DEVICE\" $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2
host/test-c2/pad_compile.c = $(host-test-compile)
$(call lib-objs,host/test-c2/pad,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-c2/pad,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-c2/pad,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

TARGETS += host/test-c2/dongle
host/test-c2/dongle_type := host-tests
host/test-c2/dongle_toolchain := HOST_
host/test-c2/dongle_sources := $(HOST_TESTS)
host/test-c2/dongle_cppflags := -DBOARD_PID=\"GS_C2_Dongle\" -DBOARD_DONGLE $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2
host/test-c2/dongle_compile.c = $(host-test-compile)
$(call lib-objs,host/test-c2/dongle,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-c2/dongle,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-c2/dongle,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

