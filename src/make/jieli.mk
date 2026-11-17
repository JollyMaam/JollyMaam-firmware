# Jieli boards on lib.mk: the SDK, the toolchains, flags, the SDK's and our
# Jieli sources as units, and the build steps as target types.  The compiler
# ABI and library configuration are the pinned SDK's (apps/hid/board/br23/
# Makefile and its BD19 sibling); we never include a vendor Makefile.  Each
# board spells out its own targets in its rules.mk; nothing here depends on
# a board.
#
# Every list is JIELI_COMMON_* (every chip) and JIELI_<CHIP>_* (that chip's
# own), and a chip's toolchain (NAME_toolchain := JIELI_BR23_ or
# JIELI_BD19_) is the two together.  A unit is added, never filtered.
#   JIELI_BR23  AC635N (Cyclone 2 pad and dongle, Nova 2 Lite pad): pi32v2,
#               our controller (src/platform/jieli/btctrler/)
#   JIELI_BD19  AC632N (Nova 2 Lite dongle): q32s, the SDK's controller

JIELI_SDK := external/jieli-fw-AC63_BT_SDK
JIELI_TOOLCHAINS := external/toolchains/jieli-linux-toolchains-20250324.1

# VM (settings and bonds) is the VM_KB below BTIF (4 KiB) and key_mac (4 KiB)
# at the end of flash; code may use everything below it (the code limit).
# Changing VM_KB or the flash size moves VM.  16 KiB is the SDK's largest.
JIELI_VM_KB := 16
# Budget for SDK-owned and retained VM items, beyond our settings records.
JIELI_VM_SETTINGS_RESERVE := 1024
jieli_code_limit = $(shell printf 0x%x $$(( $(1) - 0x2000 - $(JIELI_VM_KB) * 1024 )))

# For a Jieli board's host tests: the settings test checks its records
# against the Jieli VM (its size and the SDK's reserve).
JIELI_TEST_DEFINES := -DSETTINGS_STORE_JIELI_BYTES=$(JIELI_VM_KB)*1024 -DSETTINGS_STORE_JIELI_RESERVE=$(JIELI_VM_SETTINGS_RESERVE)

# --- Every chip ---------------------------------------------------------------

JIELI_COMMON_CFLAGS :=
JIELI_COMMON_CFLAGS += -integrated-as
JIELI_COMMON_CFLAGS += -flto
JIELI_COMMON_CFLAGS += -Oz
JIELI_COMMON_CFLAGS += -g
JIELI_COMMON_CFLAGS += -Wuninitialized
JIELI_COMMON_CFLAGS += -Wno-invalid-noreturn
JIELI_COMMON_CFLAGS += -fno-common
JIELI_COMMON_CFLAGS += -fallow-pointer-null
JIELI_COMMON_CFLAGS += -fprefer-gnu-section
JIELI_COMMON_CFLAGS += -Wno-shift-negative-value
JIELI_COMMON_CFLAGS += -Wundef
JIELI_COMMON_CFLAGS += -Wframe-larger-than=256
JIELI_COMMON_CFLAGS += -Wincompatible-pointer-types
JIELI_COMMON_CFLAGS += -Wreturn-type
JIELI_COMMON_CFLAGS += -Wimplicit-function-declaration
JIELI_COMMON_CFLAGS += -fms-extensions
JIELI_COMMON_CFLAGS += -w

JIELI_COMMON_DEFINES :=
JIELI_COMMON_DEFINES += -DAPP_ASYNC_POOL_NUM_CONFIG=0
JIELI_COMMON_DEFINES += -DCONFIG_AEC_M=640
JIELI_COMMON_DEFINES += -DCONFIG_AUDIO_ONCHIP
JIELI_COMMON_DEFINES += -DCONFIG_BTCTRLER_TASK_DEL_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_EQ_SUPPORT_ASYNC
JIELI_COMMON_DEFINES += -DCONFIG_EVENT_KEY_MAP_ENABLE=0
JIELI_COMMON_DEFINES += -DCONFIG_FREE_RTOS_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_HID_CASE_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_ITEM_FORMAT_VM
JIELI_COMMON_DEFINES += -DCONFIG_LINK_DISTURB_SCAN_ENABLE=0
JIELI_COMMON_DEFINES += -DCONFIG_LITE_AEC_ENABLE=0
JIELI_COMMON_DEFINES += -DCONFIG_LITE_AUDIO
JIELI_COMMON_DEFINES += -DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_LMP_RESET_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_MIXER_CYCLIC
JIELI_COMMON_DEFINES += -DCONFIG_MMU_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_NEW_ECC_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_OTA_UPDATA_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_RELEASE_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_TRANSFER_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_TWS_ENABLE
JIELI_COMMON_DEFINES += -DCONFIG_UPDATA_ENABLE
JIELI_COMMON_DEFINES += -DEVENT_HANDLER_NUM_CONFIG=2
JIELI_COMMON_DEFINES += -DEVENT_POOL_SIZE_CONFIG=256
JIELI_COMMON_DEFINES += -DEVENT_TOUCH_ENABLE_CONFIG=0
JIELI_COMMON_DEFINES += -DSDFILE_VERSION=0x020000
JIELI_COMMON_DEFINES += -DSDK_VERSION_CFG_DEFINE=0x240001
JIELI_COMMON_DEFINES += -DSDK_VERSION_DATE_DEFINE=20241231
JIELI_COMMON_DEFINES += -D__SHELL__
JIELI_COMMON_DEFINES += -DSUPPORT_MS_EXTENSIONS
JIELI_COMMON_DEFINES += -DTCFG_APP_BT_EN=1
JIELI_COMMON_DEFINES += -DTIMER_POOL_NUM_CONFIG=10
JIELI_COMMON_DEFINES += -DUSB_PC_NO_APP_MODE
JIELI_COMMON_DEFINES += -DUSE_SDFILE_NEW=1
JIELI_COMMON_DEFINES += -DVFS_ENABLE=1
JIELI_COMMON_DEFINES += -DVFS_FILE_POOL_NUM_CONFIG=0
JIELI_COMMON_DEFINES += -DVM_ITEM_MAX_NUM=256
JIELI_COMMON_DEFINES += -DVM_MAX_SIZE_CONFIG=16*1024

# The SDK's include directories (a chip's go after these), after a board's
# own (SRC_INCLUDES, then its -iquote) and our SDK configuration's.
JIELI_COMMON_INCLUDES :=
JIELI_COMMON_INCLUDES += -iquote src/platform/jieli/sdkconfig
JIELI_COMMON_INCLUDES += $(BTSTACK_INCLUDES)
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/driver
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/driver/device
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system/generic
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system/device
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system/fs
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system/ui
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/btctrler
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/update
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/agreement
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack/third_party/common
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack/third_party/rcsp
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/ai_stack/JL_rcsp
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/include_lib/system/ex_mcu
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/include
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/gatt_common
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/online_db
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/trans_data_demo
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/common
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb/device
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb/host
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/audio
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/code_switch/include
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device/optical_mouse_sensor/include
JIELI_COMMON_INCLUDES += -I$(JIELI_SDK)/apps/common/device/touch_pad

JIELI_COMMON_LDFLAGS :=
JIELI_COMMON_LDFLAGS += --plugin-opt=-inline-threshold=5
JIELI_COMMON_LDFLAGS += --plugin-opt=-inline-normal-into-special-section=true
JIELI_COMMON_LDFLAGS += --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar
JIELI_COMMON_LDFLAGS += --plugin-opt=-warn-stack-size=256
JIELI_COMMON_LDFLAGS += --sort-common
JIELI_COMMON_LDFLAGS += --dont-complain-call-overflow
JIELI_COMMON_LDFLAGS += --gc-sections

# The SDK sources the app uses (a chip's cpu/ ones go after these).
JIELI_COMMON_SDK_USED :=
JIELI_COMMON_SDK_USED += apps/common/debug/debug.c
JIELI_COMMON_SDK_USED += apps/common/device/usb/device/descriptor.c
JIELI_COMMON_SDK_USED += apps/common/device/usb/device/usb_device.c
JIELI_COMMON_SDK_USED += apps/common/device/usb/device/user_setup.c
JIELI_COMMON_SDK_USED += apps/common/device/usb/usb_config.c

# --- BR23 (AC635N) ------------------------------------------------------------

JIELI_BR23_TOOLS := $(JIELI_TOOLCHAINS)/pi32v2/bin
JIELI_BR23_CC := $(JIELI_BR23_TOOLS)/clang
# The toolchain's lto-wrapper is a Python script that only adds the LTO
# plugin (its argument-file and version-info options are not used here).
JIELI_BR23_LINK := $(JIELI_BR23_TOOLS)/ld --plugin $(JIELI_BR23_TOOLS)/LLVMgold.so
JIELI_BR23_OBJCOPY := $(JIELI_BR23_TOOLS)/objcopy

JIELI_BR23_CFLAGS := -target pi32v2 -mcpu=r3 $(JIELI_COMMON_CFLAGS)

JIELI_BR23_DEFINES :=
JIELI_BR23_DEFINES += -DCONFIG_CPU_BR23
JIELI_BR23_DEFINES += -D__GCC_PI32V2__
JIELI_BR23_DEFINES += -DCONFIG_DNS_ENABLE
JIELI_BR23_DEFINES += -DCONFIG_MEDIA_DEVELOP_ENABLE
JIELI_BR23_DEFINES += -DCONFIG_MSBC_CODEC_HW
JIELI_BR23_DEFINES += -DCONFIG_SBC_CODEC_HW

JIELI_BR23_INCLUDES :=
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/driver/cpu/br23
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/system/math/cpu/br23
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/btctrler/port/br23
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23/asm
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/cpu/br23
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/cpu/br23/audio_dec
JIELI_BR23_INCLUDES += -I$(JIELI_SDK)/cpu/br23/audio_enc
JIELI_BR23_INCLUDES += -I$(JIELI_TOOLCHAINS)/pi32v2/include

JIELI_BR23_LDFLAGS := $(JIELI_COMMON_LDFLAGS)
JIELI_BR23_LDFLAGS += --plugin-opt=mcpu=r3
JIELI_BR23_LDFLAGS += --plugin-opt=-mattr=+fprev1
JIELI_BR23_LDFLAGS += --plugin-opt=-pi32v2-always-use-itblock=false
JIELI_BR23_LDFLAGS += --plugin-opt=-enable-ipra=true
JIELI_BR23_LDFLAGS += --plugin-opt=-pi32v2-merge-max-offset=4096
JIELI_BR23_LDFLAGS += --plugin-opt=-pi32v2-enable-simd=true
JIELI_BR23_LDFLAGS += --plugin-opt=-global-merge-on-const
JIELI_BR23_LDFLAGS += --plugin-opt=-inline-max-allocated-size=32
JIELI_BR23_LDFLAGS += --plugin-opt=-pi32v2-enable-rep-memop

JIELI_BR23_ARCHIVES :=
JIELI_BR23_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/crypto_toolbox_Osize.a
JIELI_BR23_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/cpu.a
JIELI_BR23_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/system.a
JIELI_BR23_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/update.a
JIELI_BR23_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/media.a

JIELI_BR23_RUNTIME_LIBS :=
JIELI_BR23_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/pi32v2/lib/r3/libm.a
JIELI_BR23_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/pi32v2/lib/r3/libc.a
JIELI_BR23_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/pi32v2/lib/r3/libcompiler-rt.a

JIELI_BR23_SDK_USED := $(JIELI_COMMON_SDK_USED)
JIELI_BR23_SDK_USED += cpu/br23/adc_api.c
JIELI_BR23_SDK_USED += cpu/br23/charge.c
JIELI_BR23_SDK_USED += cpu/br23/clock_manager.c
JIELI_BR23_SDK_USED += cpu/br23/led_spi.c
JIELI_BR23_SDK_USED += cpu/br23/mcpwm.c
JIELI_BR23_SDK_USED += cpu/br23/setup.c

JIELI_BR23_LINKER_SCRIPT := src/platform/jieli/sdkconfig/sdk_ld_br23.c
JIELI_BR23_ISD_CONFIG := $(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c
JIELI_BR23_UBOOT := $(JIELI_SDK)/cpu/br23/tools/uboot.boot
JIELI_BR23_CONFIG_TOOL := $(JIELI_SDK)/cpu/br23/tools/cfg_tool.bin
# jollymaam finds each chip's RAM loader under the SDK's cpu/<chip>/tools/.

# The toolchain (NAME_toolchain := JIELI_BR23_).
JIELI_BR23_CPPFLAGS := $(JIELI_COMMON_DEFINES) $(JIELI_BR23_DEFINES)
JIELI_BR23_LDLIBS := --start-group $(JIELI_BR23_ARCHIVES) --end-group $(JIELI_BR23_RUNTIME_LIBS)
# A board's include directories, after its own (SRC_INCLUDES, then -iquote).
JIELI_BR23_BOARD_INCLUDES := $(JIELI_COMMON_INCLUDES) $(JIELI_BR23_INCLUDES)
JIELI_BR23_SDK_SOURCES := $(addprefix $(JIELI_SDK)/,$(JIELI_BR23_SDK_USED))

# --- BD19 (AC632N) ------------------------------------------------------------
# It links the SDK's controller (btctrler.a), not ours.

JIELI_BD19_TOOLS := $(JIELI_TOOLCHAINS)/q32s/bin
JIELI_BD19_CC := $(JIELI_BD19_TOOLS)/clang
JIELI_BD19_LINK := $(JIELI_BD19_TOOLS)/ld --plugin $(JIELI_BD19_TOOLS)/LLVMgold.so
JIELI_BD19_OBJCOPY := $(JIELI_BD19_TOOLS)/objcopy

JIELI_BD19_CFLAGS := -target q32s -fno-builtin $(JIELI_COMMON_CFLAGS)

JIELI_BD19_DEFINES :=
JIELI_BD19_DEFINES += -DCONFIG_CPU_BD19
JIELI_BD19_DEFINES += -D__GCC_Q32S__
JIELI_BD19_DEFINES += -DCONFIG_CRYPTO_TOOLBOX_OSIZE_IN_MASKROM
JIELI_BD19_DEFINES += -DCONFIG_MEDIA_NEW_ENABLE
JIELI_BD19_DEFINES += -DCONFIG_NEW_BREDR_ENABLE

JIELI_BD19_INCLUDES :=
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/driver/cpu/bd19
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/system/math/cpu/bd19
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/btctrler/port/bd19
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_new
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_new/media
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_new/media/cpu/bd19
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_new/media/cpu/bd19/asm
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/cpu/bd19
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/cpu/bd19/audio_dec
JIELI_BD19_INCLUDES += -I$(JIELI_SDK)/cpu/bd19/audio_enc
JIELI_BD19_INCLUDES += -I$(JIELI_TOOLCHAINS)/q32s/include

JIELI_BD19_LDFLAGS := $(JIELI_COMMON_LDFLAGS)

JIELI_BD19_ARCHIVES :=
JIELI_BD19_ARCHIVES += $(JIELI_SDK)/cpu/bd19/liba/cpu.a
JIELI_BD19_ARCHIVES += $(JIELI_SDK)/cpu/bd19/liba/system.a
JIELI_BD19_ARCHIVES += $(JIELI_SDK)/cpu/bd19/liba/update.a
JIELI_BD19_ARCHIVES += $(JIELI_SDK)/cpu/bd19/liba/media.a
# The SDK's controller.
JIELI_BD19_ARCHIVES += $(JIELI_SDK)/cpu/bd19/liba/btctrler.a

JIELI_BD19_RUNTIME_LIBS :=
JIELI_BD19_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/q32s/lib/libm.a
JIELI_BD19_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/q32s/lib/libc.a
JIELI_BD19_RUNTIME_LIBS += $(JIELI_TOOLCHAINS)/q32s/lib/libcompiler-rt.a

JIELI_BD19_SDK_USED := $(JIELI_COMMON_SDK_USED)
JIELI_BD19_SDK_USED += cpu/bd19/adc_api.c
JIELI_BD19_SDK_USED += cpu/bd19/charge.c
JIELI_BD19_SDK_USED += cpu/bd19/mcpwm.c
JIELI_BD19_SDK_USED += cpu/bd19/setup.c

JIELI_BD19_LINKER_SCRIPT := src/platform/jieli/sdkconfig/sdk_ld_bd19.c
JIELI_BD19_ISD_CONFIG := $(JIELI_SDK)/cpu/bd19/tools/isd_config_rule.c
JIELI_BD19_UBOOT := $(JIELI_SDK)/cpu/bd19/tools/uboot.boot
JIELI_BD19_CONFIG_TOOL := $(JIELI_SDK)/cpu/bd19/tools/cfg_tool.bin

# The toolchain (NAME_toolchain := JIELI_BD19_).
JIELI_BD19_CPPFLAGS := $(JIELI_COMMON_DEFINES) $(JIELI_BD19_DEFINES)
JIELI_BD19_LDLIBS := --start-group $(JIELI_BD19_ARCHIVES) --end-group $(JIELI_BD19_RUNTIME_LIBS)
JIELI_BD19_BOARD_INCLUDES := $(JIELI_COMMON_INCLUDES) $(JIELI_BD19_INCLUDES)
JIELI_BD19_SDK_SOURCES := $(addprefix $(JIELI_SDK)/,$(JIELI_BD19_SDK_USED))

# --- Our Jieli sources, as units ------------------------------------------------

# Every image (the SDK's glue, flash, USB).
JIELI_PLATFORM_SOURCES :=
JIELI_PLATFORM_SOURCES += src/platform/jieli/platform_jieli.c
JIELI_PLATFORM_SOURCES += src/platform/jieli/platform_jieli_sdk.c
JIELI_PLATFORM_SOURCES += src/platform/jieli/platform_jieli_usb.c
JIELI_PAD_WAKE_SOURCES := src/platform/jieli/bt_switch_wake_jieli.c

# One of the two: USB audio, or its stubs.
JIELI_AUDIO_SOURCES := src/platform/jieli/platform_jieli_audio.c
JIELI_NOAUDIO_SOURCES := src/platform/jieli/platform_jieli_noaudio.c
# BTstack on the controller (with BTSTACK_RENAMES: BTSTACK_USERS).
JIELI_BTSTACK_SOURCES := src/platform/jieli/btstack_hci_jieli.c src/platform/jieli/hci_queue.c
# The updater (src/updater/), and its LE service for an image with Bluetooth.
JIELI_UPDATER_SOURCES := src/updater/updater.c src/updater/updater_jieli.c
JIELI_UPDATER_BLE_SOURCES := src/updater/updater_ble.c src/updater/sm_none.c

# BR23: our controller (src/platform/jieli/btctrler/) by layer, and BTstack's
# QoS on it.  An image links every layer (--gc-sections keeps what it uses);
# each source builds with JIELI_BR23_CONTROLLER_CFLAGS.
JIELI_BR23_CONTROLLER_CFLAGS := -Werror=implicit-function-declaration
# The rest: HCI, tasks, RF, crypto, memory and the like.
JIELI_BTCTRLER_CORE :=
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/RF.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/address.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/aes128.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/aes_hw.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/analog.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/authentication.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/btagc.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/btcontroller_heap.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/btcontroller_util.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/btencry_task.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/device_manager.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/direct_test_dut.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_dispatch.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_ll.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_ll_5_cmds.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_ll_cmds.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_misc.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hci_transport.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/hw_dma.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/lbuf.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/link_bulk.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/link_control.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/link_layer.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/link_policy.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/link_task.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/local_media_sync.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/misc_hw.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/multilink_schedule.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/nv_memory.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/sys_timer.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/task_dispatch.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/task_lifecycle.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/task_queue.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/thread.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/vendor_manager.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/version.c
JIELI_BTCTRLER_CORE += src/platform/jieli/btctrler/vsprintf.c
# LE: the link layer (ll_*).
JIELI_BTCTRLER_LE :=
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/RF_ble.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/RF_ble5.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/analog_ble.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/le_low_power.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_adv.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_afh.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_bis.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_bis_master.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_cis.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_common_iso.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_data_extension.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_events.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_ext_adv.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_ext_init.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_ext_scan.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_extended_advertising.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_init.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_keys.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_master.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_multiple_phys.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_pc.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_periodic_advertising.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_resolve_list.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_scan.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_slave.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_vendor.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_white_list.c
JIELI_BTCTRLER_LE += src/platform/jieli/btctrler/ll_wifi_detect.c
# BR/EDR: the baseband and LMP (bredr_*, lmp_*).
JIELI_BTCTRLER_BREDR :=
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_dut.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_fcc.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_frame.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_link.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_low_power.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_slot_timer.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/bredr_utils.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/classic_baseband.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_acl.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_acl_tx.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_afh.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_clock_setup.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_commands.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_connections.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_control_tx.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_encryption.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_escape3.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_esco.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_events.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_hci_control.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_hci_pairing.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_hci_procedures.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_hci_settings.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_info.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_inquiry_page.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_link_events.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_master.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_name_req.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_packets.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_pairing_support.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_power.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_queries.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_receive.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_role_switch.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_rx.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_slave.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_sniff.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_sniff_support.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_ssp.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_state.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_suspend.c
JIELI_BTCTRLER_BREDR += src/platform/jieli/btctrler/lmp_utils.c
# TWS (tws_*).
JIELI_BTCTRLER_TWS :=
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_ble_link.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_data_forwarding.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_event.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_le_acc.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_le_link.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_link.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_ll_adv.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_ll_scan.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_media_sync.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_pair.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_power_balance.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_sniff.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_sync_call.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_task.c
JIELI_BTCTRLER_TWS += src/platform/jieli/btctrler/tws_tx_sync.c
JIELI_BR23_CONTROLLER_SOURCES := $(JIELI_BTCTRLER_CORE) $(JIELI_BTCTRLER_LE) $(JIELI_BTCTRLER_BREDR) $(JIELI_BTCTRLER_TWS)
JIELI_BR23_CONTROLLER_SOURCES += src/platform/jieli/btstack_qos_jieli.c

# --- Target types ---------------------------------------------------------------
# Each is per chip (jieli-br23-..., jieli-bd19-...), from one definition.

# Preprocessing (sdk.ld, isd_config.ini) is a compile step with the board's
# flags: the "object" is the preprocessed text, and a type makes the output.
# sdk.ld is $(JIELI_<CHIP>_LINKER_SCRIPT) with -DLD_CODE_AT=... for an app
# above the updater, -DLD_UPDATER for an updater (see its header).
jieli-cpp = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -D__LD__ -E -P -MMD -MP -MF $(@:.o=.d) -MT $@ $< -o $@
lib-type-copy = cp $(lib_objects) $@

# The SDK's download config, preprocessed, with the target's jieli_pid; the
# BD19's has the new firmware format's header after it.
jieli-isd = sed -e '/^$$/d' -e 's/^PID = .*;/PID = $(jieli_pid);/' $(lib_objects) > $@
JIELI_BD19_ISD_TAIL := \n[JLNEWFW]\nUBOOT_FLAGS = 0x40;\nHEADER_FS = 11;\nRES = $(abspath $(JIELI_SDK))/cpu/bd19/tools/p11_code.bin:82;\n
lib-type-jieli-br23-isd = $(jieli-isd)
lib-type-jieli-bd19-isd = $(jieli-isd) && printf '$(JIELI_BD19_ISD_TAIL)' >> $@

# The image: the ELF's (first _deps) sections, as the loader writes them.
JIELI_BR23_BIN_SECTIONS := .text .data
# The BD19 also has code that runs from RAM.
JIELI_BD19_BIN_SECTIONS := .text .data .data_code
define jieli-bin
$(foreach s,$(JIELI_$(1)_BIN_SECTIONS),$(JIELI_$(1)_OBJCOPY) -O binary -j $(s) $(firstword $(lib_deps)) $@$(s) &&)
cat $(addprefix $@,$(JIELI_$(1)_BIN_SECTIONS)) > $@ &&
rm -f $(addprefix $@,$(JIELI_$(1)_BIN_SECTIONS))
endef
lib-type-jieli-br23-bin = $(call jieli-bin,BR23)
lib-type-jieli-bd19-bin = $(call jieli-bin,BD19)

# The flashable image, packed by jollymaam with the target's jieli_pack
# (sizes, config, loader, images; see jollymaam help).
lib-type-jieli-img = $(JOLLYMAAM_CLI) pack jieli -o $@ $(jieli_pack)
