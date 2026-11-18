HELP_TARGETS += n2
$(FIRMWARE_ZIP): $(O)/n2/pad/firmware.img $(O)/n2/dongle/firmware.img

firmware: n2
test: n2-test

n2: n2/pad n2/dongle
n2/pad: $(O)/n2/pad/firmware.img
n2/dongle: $(O)/n2/dongle/firmware.img
n2/pad-test: $(O)/host/test-n2/pad
n2/dongle-test: $(O)/host/test-n2/dongle
n2-test: n2/pad-test n2/dongle-test

.PHONY: n2 n2/pad n2/dongle n2-test n2/pad-test n2/dongle-test

# Stock keeps its VM, BTIF (MAC), EXIF, USERIF and key_mac at
# 0x64000..0x80000 of the 1 MiB flash; declaring 1 MiB puts ours in the empty
# upper half, so an image under 0x64000 leaves stock's alone.

# --- Pad updater: below stock's data; the app's region at flash 0x80000, clear of it too

N2PU_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"AC695X_TRANS\" -DBT_LE_ONLY -DUSB_TOOL_ONLY $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS) -DMAIN_AT=0x7d000 -DCODE_LIMIT=$(call jieli_code_limit,0x100000)
N2PU_CPPFLAGS := $(N2PU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BR23_BOARD_INCLUDES)

N2PU_SOURCES :=
N2PU_SOURCES += $(JIELI_UPDATER_SOURCES)
N2PU_SOURCES += $(JIELI_UPDATER_BLE_SOURCES)
N2PU_SOURCES += src/boards/gamesir-nova-2-lite/board_pad.c
N2PU_SOURCES += $(JIELI_PLATFORM_SOURCES)
N2PU_SOURCES += $(JIELI_NOAUDIO_SOURCES)
N2PU_SOURCES += $(JIELI_BTSTACK_SOURCES)
N2PU_SOURCES += $(CORE_SOURCES)
# LE only, no security manager.
N2PU_BTSTACK := $(BTSTACK_CORE_SOURCES) $(BTSTACK_LE_SOURCES)

TARGETS += n2/pad-updater/sdk.ld
n2/pad-updater/sdk.ld_type := copy
n2/pad-updater/sdk.ld_toolchain := JIELI_BR23_
n2/pad-updater/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
n2/pad-updater/sdk.ld_cppflags := $(N2PU_CPPFLAGS) -DLD_UPDATER
n2/pad-updater/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += n2/pad-updater/app.elf
n2/pad-updater/app.elf_toolchain := JIELI_BR23_
n2/pad-updater/app.elf_sources := $(N2PU_SOURCES) $(JIELI_BR23_CONTROLLER_SOURCES) $(JIELI_BR23_SDK_SOURCES) $(N2PU_BTSTACK)
n2/pad-updater/app.elf_cppflags := $(N2PU_CPPFLAGS)
n2/pad-updater/app.elf_ldflags := -T$(O)/n2/pad-updater/sdk.ld -M=$(O)/n2/pad-updater/app.map
n2/pad-updater/app.elf_deps := $(O)/n2/pad-updater/sdk.ld $(JIELI_BR23_ARCHIVES)
n2/pad-updater/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,n2/pad-updater/app.elf,$(JIELI_BR23_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BR23_CONTROLLER_CFLAGS)
$(call lib-objs,n2/pad-updater/app.elf,$(N2PU_BTSTACK) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += n2/pad-updater/app.bin
n2/pad-updater/app.bin_type := jieli-br23-bin
n2/pad-updater/app.bin_deps := $(O)/n2/pad-updater/app.elf

# --- Pad: app 0x7d000 above the updater

N2P_FLAGS := -DCONFIG_FLASH_SIZE=0x100000 -DBOARD_PID=\"AC695X_TRANS\" $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS)
N2P_CPPFLAGS := $(N2P_FLAGS) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 $(JIELI_BR23_BOARD_INCLUDES)

N2P_SOURCES :=
N2P_SOURCES += src/boards/gamesir-nova-2-lite/board_pad.c
N2P_SOURCES += src/boards/gamesir-cyclone-2/bt_pad.c
N2P_SOURCES += $(PAD_SOURCES)
N2P_SOURCES += $(PROFILE_SOURCES)
N2P_SOURCES += $(PROFILE_PAD_SOURCES)
N2P_SOURCES += $(JIELI_PAD_WAKE_SOURCES)
N2P_SOURCES += $(CORE_SOURCES)
N2P_SOURCES += $(JIELI_PLATFORM_SOURCES)
N2P_SOURCES += $(JIELI_AUDIO_SOURCES)
N2P_SOURCES += $(JIELI_BTSTACK_SOURCES)

TARGETS += n2/pad/sdk.ld
n2/pad/sdk.ld_type := copy
n2/pad/sdk.ld_toolchain := JIELI_BR23_
n2/pad/sdk.ld_sources := $(JIELI_BR23_LINKER_SCRIPT)
n2/pad/sdk.ld_cppflags := $(N2P_CPPFLAGS) -DLD_CODE_AT=0x7d000
n2/pad/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += n2/pad/isd_config.ini
n2/pad/isd_config.ini_type := jieli-br23-isd
n2/pad/isd_config.ini_toolchain := JIELI_BR23_
n2/pad/isd_config.ini_sources := $(JIELI_BR23_ISD_CONFIG)
n2/pad/isd_config.ini_cppflags := $(N2P_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
n2/pad/isd_config.ini_compile.c = $(jieli-cpp)
$(O)/n2/pad/isd_config.ini: private jieli_pid := AC695X_TRANS

TARGETS += n2/pad/app.elf
n2/pad/app.elf_toolchain := JIELI_BR23_
n2/pad/app.elf_sources := $(N2P_SOURCES) $(JIELI_BR23_CONTROLLER_SOURCES) $(JIELI_BR23_SDK_SOURCES) $(BTSTACK_SOURCES)
n2/pad/app.elf_cppflags := $(N2P_CPPFLAGS)
n2/pad/app.elf_ldflags := -T$(O)/n2/pad/sdk.ld -M=$(O)/n2/pad/app.map
n2/pad/app.elf_deps := $(O)/n2/pad/sdk.ld $(JIELI_BR23_ARCHIVES)
n2/pad/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,n2/pad/app.elf,$(JIELI_BR23_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BR23_CONTROLLER_CFLAGS)
$(call lib-objs,n2/pad/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += n2/pad/app.bin
n2/pad/app.bin_type := jieli-br23-bin
n2/pad/app.bin_deps := $(O)/n2/pad/app.elf

TARGETS += n2/pad/firmware.img
n2/pad/firmware.img_type := jieli-img
n2/pad/firmware.img_deps := $(O)/n2/pad-updater/app.bin $(O)/n2/pad/app.bin $(O)/n2/pad/isd_config.ini $(JIELI_BR23_UBOOT) $(JIELI_BR23_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(O)/n2/pad/firmware.img: private jieli_pack := --flash-size 0x100000 --code-limit $(call jieli_code_limit,0x100000) \
	-c $(O)/n2/pad/isd_config.ini -u $(JIELI_BR23_UBOOT) $(O)/n2/pad-updater/app.bin --main 0x7d000:$(O)/n2/pad/app.bin $(JIELI_BR23_CONFIG_TOOL) \
	--main-out $(O)/n2/pad/update.bin

# --- Dongle updater (BD19): USB only, the app area's first 64 KiB

# Hub slots only in the app.
N2D_DEFINES := -DBOARD_DONGLE -DLOG_LINES=16 -DTRACE=16 -DUSB_DMA_BUF_MAX_SIZE=512
# Stock's long-press reset: off (time 00).
N2D_DEFINES += -DCONFIG_RESET_PIN=PB02 -DCONFIG_RESET_TIME=00 -DCONFIG_RESET_LEVEL=0
N2D_HUB_DEFINES := -DHUB_SLOTS=1

N2DU_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"AC632N_TRANS\" $(N2D_DEFINES) -DUSB_TOOL_ONLY \
	$(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) -DNO_BT -DMAIN_AT=0x10000 -DCODE_LIMIT=$(call jieli_code_limit,0x80000)
N2DU_CPPFLAGS := $(N2DU_FLAGS) $(SRC_INCLUDES) -iquote src/updater $(JIELI_BD19_BOARD_INCLUDES)

N2DU_SOURCES :=
N2DU_SOURCES += $(JIELI_UPDATER_SOURCES)
N2DU_SOURCES += src/boards/gamesir-nova-2-lite/board_dongle.c
N2DU_SOURCES += $(JIELI_PLATFORM_SOURCES)
N2DU_SOURCES += $(JIELI_NOAUDIO_SOURCES)
N2DU_SOURCES += $(CORE_SOURCES)

TARGETS += n2/dongle-updater/sdk.ld
n2/dongle-updater/sdk.ld_type := copy
n2/dongle-updater/sdk.ld_toolchain := JIELI_BD19_
n2/dongle-updater/sdk.ld_sources := $(JIELI_BD19_LINKER_SCRIPT)
n2/dongle-updater/sdk.ld_cppflags := $(N2DU_CPPFLAGS) -DLD_UPDATER
n2/dongle-updater/sdk.ld_compile.c = $(jieli-cpp)

# No Bluetooth: no BTstack, no controller but its version record (startup
# keeps it, as it did from btctrler.a).
TARGETS += n2/dongle-updater/app.elf
n2/dongle-updater/app.elf_toolchain := JIELI_BD19_
n2/dongle-updater/app.elf_sources := $(N2DU_SOURCES) $(JIELI_BD19_SDK_SOURCES) src/platform/jieli/btctrler/version.c
n2/dongle-updater/app.elf_cppflags := $(N2DU_CPPFLAGS)
n2/dongle-updater/app.elf_ldflags := -T$(O)/n2/dongle-updater/sdk.ld -M=$(O)/n2/dongle-updater/app.map
n2/dongle-updater/app.elf_deps := $(O)/n2/dongle-updater/sdk.ld $(JIELI_BD19_ARCHIVES)
n2/dongle-updater/app.elf_order_deps := $(BUILD_ID_H)

TARGETS += n2/dongle-updater/app.bin
n2/dongle-updater/app.bin_type := jieli-bd19-bin
n2/dongle-updater/app.bin_deps := $(O)/n2/dongle-updater/app.elf

# --- Dongle (BD19): app 0x10000 above the updater

N2D_FLAGS := -DCONFIG_FLASH_SIZE=0x80000 -DBOARD_PID=\"AC632N_TRANS\" $(N2D_DEFINES) $(N2D_HUB_DEFINES) $(EXTRA_DEFINES) \
	$(BUILD_ID_CPPFLAGS)
N2D_CPPFLAGS := $(N2D_FLAGS) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 $(JIELI_BD19_BOARD_INCLUDES)

# The Cyclone 2 dongle's, with this board's file, no audio and no QoS.
N2D_SOURCES :=
N2D_SOURCES += src/boards/gamesir-nova-2-lite/board_dongle.c
N2D_SOURCES += src/boards/gamesir-cyclone-2/dongle.c
N2D_SOURCES += src/boards/gamesir-cyclone-2/bt_dongle.c
N2D_SOURCES += src/boards/gamesir-cyclone-2/dongle_protocol.c
N2D_SOURCES += $(DONGLE_SOURCES)
N2D_SOURCES += $(PROFILE_SOURCES)
N2D_SOURCES += $(CORE_SOURCES)
N2D_SOURCES += $(JIELI_PLATFORM_SOURCES)
N2D_SOURCES += $(JIELI_NOAUDIO_SOURCES)
N2D_SOURCES += $(JIELI_BTSTACK_SOURCES)

TARGETS += n2/dongle/sdk.ld
n2/dongle/sdk.ld_type := copy
n2/dongle/sdk.ld_toolchain := JIELI_BD19_
n2/dongle/sdk.ld_sources := $(JIELI_BD19_LINKER_SCRIPT)
n2/dongle/sdk.ld_cppflags := $(N2D_CPPFLAGS) -DLD_CODE_AT=0x10000
n2/dongle/sdk.ld_compile.c = $(jieli-cpp)

TARGETS += n2/dongle/isd_config.ini
n2/dongle/isd_config.ini_type := jieli-bd19-isd
n2/dongle/isd_config.ini_toolchain := JIELI_BD19_
n2/dongle/isd_config.ini_sources := $(JIELI_BD19_ISD_CONFIG)
n2/dongle/isd_config.ini_cppflags := $(N2D_CPPFLAGS) -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K
n2/dongle/isd_config.ini_compile.c = $(jieli-cpp)
$(O)/n2/dongle/isd_config.ini: private jieli_pid := AC632N_TRANS

TARGETS += n2/dongle/app.elf
n2/dongle/app.elf_toolchain := JIELI_BD19_
n2/dongle/app.elf_sources := $(N2D_SOURCES) $(JIELI_BD19_CONTROLLER_SOURCES) $(JIELI_BD19_SDK_SOURCES) $(BTSTACK_SOURCES)
n2/dongle/app.elf_cppflags := $(N2D_CPPFLAGS)
n2/dongle/app.elf_ldflags := -T$(O)/n2/dongle/sdk.ld -M=$(O)/n2/dongle/app.map
n2/dongle/app.elf_deps := $(O)/n2/dongle/sdk.ld $(JIELI_BD19_ARCHIVES)
n2/dongle/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,n2/dongle/app.elf,$(JIELI_BD19_CONTROLLER_SOURCES)): obj_cflags += $(JIELI_BD19_CONTROLLER_CFLAGS)
$(call lib-objs,n2/dongle/app.elf,$(BTSTACK_SOURCES) $(BTSTACK_USERS)): obj_cppflags += $(BTSTACK_RENAMES)

TARGETS += n2/dongle/app.bin
n2/dongle/app.bin_type := jieli-bd19-bin
n2/dongle/app.bin_deps := $(O)/n2/dongle/app.elf

TARGETS += n2/dongle/firmware.img
n2/dongle/firmware.img_type := jieli-img
n2/dongle/firmware.img_deps := $(O)/n2/dongle-updater/app.bin $(O)/n2/dongle/app.bin $(O)/n2/dongle/isd_config.ini $(JIELI_BD19_UBOOT) $(JIELI_BD19_CONFIG_TOOL) $(JOLLYMAAM_CLI)
$(O)/n2/dongle/firmware.img: private jieli_pack := --flash-size 0x80000 --code-limit $(call jieli_code_limit,0x80000) \
	-c $(O)/n2/dongle/isd_config.ini -u $(JIELI_BD19_UBOOT) $(O)/n2/dongle-updater/app.bin --main 0x10000:$(O)/n2/dongle/app.bin $(JIELI_BD19_CONFIG_TOOL) \
	--main-out $(O)/n2/dongle/update.bin

# --- Tests

TARGETS += host/test-n2/pad
host/test-n2/pad_type := host-tests
host/test-n2/pad_toolchain := HOST_
host/test-n2/pad_sources := $(HOST_TESTS)
host/test-n2/pad_cppflags := -DBOARD_PID=\"AC695X_TRANS\" $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2
host/test-n2/pad_compile.c = $(host-test-compile)
$(call lib-objs,host/test-n2/pad,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-n2/pad,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-n2/pad,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

TARGETS += host/test-n2/dongle
host/test-n2/dongle_type := host-tests
host/test-n2/dongle_toolchain := HOST_
host/test-n2/dongle_sources := $(HOST_TESTS)
host/test-n2/dongle_cppflags := -DBOARD_PID=\"AC632N_TRANS\" $(N2D_DEFINES) $(N2D_HUB_DEFINES) $(EXTRA_DEFINES) -DUNITTEST $(JIELI_TEST_DEFINES) $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2
host/test-n2/dongle_compile.c = $(host-test-compile)
$(call lib-objs,host/test-n2/dongle,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-n2/dongle,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-n2/dongle,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

