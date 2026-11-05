# The Jieli BR23 and BD19: toolchains, SDK, flags and the SDK sources the
# app uses.  The compiler ABI and library configuration are from the pinned
# SDK, mostly copied from apps/hid/board/br23/Makefile; we never include a
# vendor Makefile.  make/jieli-lib.mk builds boards from this data.

JIELI_SDK := external/jieli-fw-AC63_BT_SDK
JIELI_TOOLS := external/toolchains/jieli-linux-toolchains-20250324.1/pi32v2/bin
JIELI_CC := $(JIELI_TOOLS)/clang
# The toolchain's lto-wrapper is a Python script that only adds the LTO
# plugin (its argument-file and version-info options are not used here).
JIELI_LD := $(JIELI_TOOLS)/ld --plugin $(JIELI_TOOLS)/LLVMgold.so
JIELI_OBJCOPY := $(JIELI_TOOLS)/objcopy

JIELI_UBOOT := $(JIELI_SDK)/cpu/br23/tools/uboot.boot
JIELI_CONFIG_TOOL := $(JIELI_SDK)/cpu/br23/tools/cfg_tool.bin
# jollymaam finds each chip's RAM loader under the SDK's cpu/<chip>/tools/.

# VM (settings and bonds) is the VM_KB below BTIF (4 KiB) and key_mac (4 KiB)
# at the end of flash; code may use everything below it (the code limit).
# Changing VM_KB or the flash size moves VM.  16 KiB is the SDK's largest.
JIELI_VM_KB := 16
# Budget for SDK-owned and retained VM items, beyond our settings records.
JIELI_VM_SETTINGS_RESERVE := 1024
jieli_code_limit = $(shell printf 0x%x $$(( $(1) - 0x2000 - $(JIELI_VM_KB) * 1024 )))

JIELI_CFLAGS :=
JIELI_CFLAGS += -target pi32v2
JIELI_CFLAGS += -mcpu=r3
JIELI_CFLAGS += -integrated-as
JIELI_CFLAGS += -flto
JIELI_CFLAGS += -Oz
JIELI_CFLAGS += -g
JIELI_CFLAGS += -Wuninitialized
JIELI_CFLAGS += -Wno-invalid-noreturn
JIELI_CFLAGS += -fno-common
JIELI_CFLAGS += -fallow-pointer-null
JIELI_CFLAGS += -fprefer-gnu-section
JIELI_CFLAGS += -Wno-shift-negative-value
JIELI_CFLAGS += -Wundef
JIELI_CFLAGS += -Wframe-larger-than=256
JIELI_CFLAGS += -Wincompatible-pointer-types
JIELI_CFLAGS += -Wreturn-type
JIELI_CFLAGS += -Wimplicit-function-declaration
JIELI_CFLAGS += -fms-extensions
JIELI_CFLAGS += -w

JIELI_DEFINES :=
JIELI_DEFINES += -DAPP_ASYNC_POOL_NUM_CONFIG=0
JIELI_DEFINES += -DCONFIG_AEC_M=640
JIELI_DEFINES += -DCONFIG_AUDIO_ONCHIP
JIELI_DEFINES += -DCONFIG_BTCTRLER_TASK_DEL_ENABLE
JIELI_DEFINES += -DCONFIG_CPU_BR23
JIELI_DEFINES += -DCONFIG_DNS_ENABLE
JIELI_DEFINES += -DCONFIG_EQ_SUPPORT_ASYNC
JIELI_DEFINES += -DCONFIG_EVENT_KEY_MAP_ENABLE=0
JIELI_DEFINES += -DCONFIG_FREE_RTOS_ENABLE
JIELI_DEFINES += -DCONFIG_HID_CASE_ENABLE
JIELI_DEFINES += -DCONFIG_ITEM_FORMAT_VM
JIELI_DEFINES += -DCONFIG_LINK_DISTURB_SCAN_ENABLE=0
JIELI_DEFINES += -DCONFIG_LITE_AEC_ENABLE=0
JIELI_DEFINES += -DCONFIG_LITE_AUDIO
JIELI_DEFINES += -DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE
JIELI_DEFINES += -DCONFIG_LMP_RESET_ENABLE
JIELI_DEFINES += -DCONFIG_MEDIA_DEVELOP_ENABLE
JIELI_DEFINES += -DCONFIG_MIXER_CYCLIC
JIELI_DEFINES += -DCONFIG_MMU_ENABLE
JIELI_DEFINES += -DCONFIG_MSBC_CODEC_HW
JIELI_DEFINES += -DCONFIG_NEW_ECC_ENABLE
JIELI_DEFINES += -DCONFIG_OTA_UPDATA_ENABLE
JIELI_DEFINES += -DCONFIG_RELEASE_ENABLE
JIELI_DEFINES += -DCONFIG_SBC_CODEC_HW
JIELI_DEFINES += -DCONFIG_TRANSFER_ENABLE
JIELI_DEFINES += -DCONFIG_TWS_ENABLE
JIELI_DEFINES += -DCONFIG_UPDATA_ENABLE
JIELI_DEFINES += -DEVENT_HANDLER_NUM_CONFIG=2
JIELI_DEFINES += -DEVENT_POOL_SIZE_CONFIG=256
JIELI_DEFINES += -DEVENT_TOUCH_ENABLE_CONFIG=0
JIELI_DEFINES += -D__GCC_PI32V2__
JIELI_DEFINES += -DSDFILE_VERSION=0x020000
JIELI_DEFINES += -DSDK_VERSION_CFG_DEFINE=0x240001
JIELI_DEFINES += -DSDK_VERSION_DATE_DEFINE=20241231
JIELI_DEFINES += -D__SHELL__
JIELI_DEFINES += -DSUPPORT_MS_EXTENSIONS
JIELI_DEFINES += -DTCFG_APP_BT_EN=1
JIELI_DEFINES += -DTIMER_POOL_NUM_CONFIG=10
JIELI_DEFINES += -DUSB_PC_NO_APP_MODE
JIELI_DEFINES += -DUSE_SDFILE_NEW=1
JIELI_DEFINES += -DVFS_ENABLE=1
JIELI_DEFINES += -DVFS_FILE_POOL_NUM_CONFIG=0
JIELI_DEFINES += -DVM_ITEM_MAX_NUM=256
JIELI_DEFINES += -DVM_MAX_SIZE_CONFIG=16*1024

JIELI_INCLUDES :=
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/driver
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/driver/device
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/driver/cpu/br23
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/generic
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/device
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/fs
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/ui
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/math/cpu/br23
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/btctrler
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/btctrler/port/br23
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/update
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/agreement
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack/third_party/common
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/btstack/third_party/rcsp
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23/asm
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/ai_stack/JL_rcsp
JIELI_INCLUDES += -I$(JIELI_SDK)/include_lib/system/ex_mcu
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/include
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/gatt_common
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/online_db
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/jieli/trans_data_demo
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/third_party_profile/common
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb/device
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device/usb/host
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/audio
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/code_switch/include
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device/optical_mouse_sensor/include
JIELI_INCLUDES += -I$(JIELI_SDK)/apps/common/device/touch_pad
JIELI_INCLUDES += -I$(JIELI_SDK)/cpu/br23
JIELI_INCLUDES += -I$(JIELI_SDK)/cpu/br23/audio_dec
JIELI_INCLUDES += -I$(JIELI_SDK)/cpu/br23/audio_enc
JIELI_INCLUDES += -I$(JIELI_TOOLS)/../include

JIELI_RUNTIME_LIBS :=
JIELI_RUNTIME_LIBS += $(JIELI_TOOLS)/../lib/r3/libm.a
JIELI_RUNTIME_LIBS += $(JIELI_TOOLS)/../lib/r3/libc.a
JIELI_RUNTIME_LIBS += $(JIELI_TOOLS)/../lib/r3/libcompiler-rt.a

JIELI_LFLAGS :=
JIELI_LFLAGS += --plugin-opt=-pi32v2-always-use-itblock=false
JIELI_LFLAGS += --plugin-opt=-enable-ipra=true
JIELI_LFLAGS += --plugin-opt=-pi32v2-merge-max-offset=4096
JIELI_LFLAGS += --plugin-opt=-pi32v2-enable-simd=true
JIELI_LFLAGS += --plugin-opt=mcpu=r3
JIELI_LFLAGS += --plugin-opt=-global-merge-on-const
JIELI_LFLAGS += --plugin-opt=-inline-threshold=5
JIELI_LFLAGS += --plugin-opt=-inline-max-allocated-size=32
JIELI_LFLAGS += --plugin-opt=-inline-normal-into-special-section=true
JIELI_LFLAGS += --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar
JIELI_LFLAGS += --plugin-opt=-pi32v2-enable-rep-memop
JIELI_LFLAGS += --plugin-opt=-warn-stack-size=256
JIELI_LFLAGS += --sort-common
JIELI_LFLAGS += --dont-complain-call-overflow
JIELI_LFLAGS += --gc-sections
JIELI_LFLAGS += --plugin-opt=mcpu=r3
JIELI_LFLAGS += --plugin-opt=-mattr=+fprev1

JIELI_ARCHIVES :=
JIELI_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/crypto_toolbox_Osize.a
JIELI_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/cpu.a
JIELI_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/system.a
JIELI_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/update.a
JIELI_ARCHIVES += $(JIELI_SDK)/cpu/br23/liba/media.a

# The SDK sources the app uses.
JIELI_SDK_USED :=
JIELI_SDK_USED += apps/common/debug/debug.c
JIELI_SDK_USED += apps/common/device/usb/device/descriptor.c
JIELI_SDK_USED += apps/common/device/usb/device/usb_device.c
JIELI_SDK_USED += apps/common/device/usb/device/user_setup.c
JIELI_SDK_USED += apps/common/device/usb/usb_config.c
JIELI_SDK_USED += cpu/br23/adc_api.c
JIELI_SDK_USED += cpu/br23/charge.c
JIELI_SDK_USED += cpu/br23/clock_manager.c
JIELI_SDK_USED += cpu/br23/led_spi.c
JIELI_SDK_USED += cpu/br23/mcpwm.c
JIELI_SDK_USED += cpu/br23/setup.c

# BD19 uses q32s and the SDK controller; BR23 keeps the app's controller.
JIELI_BD19_TOOLS := external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin
JIELI_BD19_CFLAGS := $(filter-out -target pi32v2 -mcpu=r3,$(JIELI_CFLAGS)) -target q32s -fno-builtin
JIELI_BD19_DEFINES := $(filter-out -D__GCC_PI32V2__ -DCONFIG_CPU_BR23 -DCONFIG_MEDIA_DEVELOP_ENABLE -DCONFIG_SBC_CODEC_HW -DCONFIG_MSBC_CODEC_HW -DCONFIG_DNS_ENABLE,$(JIELI_DEFINES)) \
    -DCONFIG_CPU_BD19 -DCONFIG_NEW_BREDR_ENABLE -DCONFIG_MEDIA_NEW_ENABLE -D__GCC_Q32S__ -DCONFIG_CRYPTO_TOOLBOX_OSIZE_IN_MASKROM
JIELI_BD19_INCLUDES := $(subst media_develop,media_new,$(subst br23,bd19,$(JIELI_INCLUDES))) -I$(JIELI_BD19_TOOLS)/../include
JIELI_BD19_SDK_USED := $(subst br23,bd19,$(filter-out cpu/br23/clock_manager.c cpu/br23/led_spi.c,$(JIELI_SDK_USED)))
JIELI_BD19_ARCHIVES := $(addprefix $(JIELI_SDK)/cpu/bd19/liba/,cpu.a system.a update.a media.a btctrler.a)
JIELI_BD19_LFLAGS := --plugin-opt=-inline-threshold=5 --plugin-opt=-inline-normal-into-special-section=true \
    --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar --plugin-opt=-warn-stack-size=256 \
    --sort-common --dont-complain-call-overflow --gc-sections
