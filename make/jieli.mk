# The Jieli BR23: toolchain, SDK and the rules that build a board on it.
# The compiler ABI and library configuration are from the pinned SDK, mostly
# copied from apps/hid/board/br23/Makefile; we never include a vendor
# Makefile.  A product file sets a board's variables, then calls
# $(call jieli_board,NAME) (at the end of this file).

JIELI_SDK := /shared/github.com-Jieli-Tech/fw-AC63_BT_SDK
JIELI_TOOLS := /shared/jieli-linux-toolchains-20250324.1/pi32v2/bin
JIELI_CC := $(JIELI_TOOLS)/clang
JIELI_LD := $(JIELI_TOOLS)/lto-wrapper
JIELI_OBJCOPY := $(JIELI_TOOLS)/objcopy

JIELI_UBOOT := $(JIELI_SDK)/cpu/br23/tools/uboot.boot
JIELI_CONFIG_TOOL := $(JIELI_SDK)/cpu/br23/tools/cfg_tool.bin
# jlnewfw finds each chip's RAM loader under the SDK's cpu/<chip>/tools/.
JIELI_LOADER := $(JIELI_SDK)

# VM (settings and bonds) is the VM_KB below BTIF (4 KiB) and key_mac (4 KiB)
# at the end of flash; code may use everything below it (the code limit).
# Changing VM_KB or the flash size moves VM.  16 KiB is the SDK's largest.
JIELI_VM_KB := 16
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
JIELI_SDK_USED += apps/common/device/usb/device/msd.c
JIELI_SDK_USED += apps/common/device/usb/device/usb_device.c
JIELI_SDK_USED += apps/common/device/usb/device/user_setup.c
JIELI_SDK_USED += apps/common/device/usb/usb_config.c
JIELI_SDK_USED += cpu/br23/adc_api.c
JIELI_SDK_USED += cpu/br23/charge.c
JIELI_SDK_USED += cpu/br23/clock_manager.c
JIELI_SDK_USED += cpu/br23/led_spi.c
JIELI_SDK_USED += cpu/br23/mcpwm.c
JIELI_SDK_USED += cpu/br23/setup.c

# $(call jieli_board,NAME): build NAME on the BR23, into $(O)/NAME/.  Set first:
#   NAME_FLASH_SIZE  the flash device's size
#   NAME_PID         the image's PID: the flasher writes only over the same PID;
#                    the firmware names itself by it (BOARD_PID)
#   NAME_SOURCES     the app's sources; our controller (btctrler/) and BTstack come with it
#   NAME_DEFINES     -D flags for this board (EXTRA_DEFINES adds to them)
#   NAME_BOARD_DIR   the board's headers (btstack_config.h)
#   NAME_LOADER_HINT how to enter the ROM loader by hand, where software cannot
# Targets: NAME (firmware.img), NAME-flash (through the ROM loader),
# NAME-loader (into the ROM loader and wait there); with more than one
# device for the board, DEVICE= picks one: its USB port, as jlnewfw scan
# lists it.  Inside the template, $(1) is NAME, $(...) is expanded once when
# it is called, and $$(...) when the rule runs.
define jieli_board
$(1)_CODE_LIMIT := $(call jieli_code_limit,$($(1)_FLASH_SIZE))
$(1)_OBJECTS := $(patsubst %.c,$(O)/$(1)/%.o,$($(1)_SOURCES) $(wildcard src/platform/jieli/btctrler/*.c)) \
	$(JIELI_SDK_USED:%.c=$(O)/$(1)/sdk/%.o) $(BTSTACK_USED:%.c=$(O)/$(1)/btstack/%.o)
$(1)_FLAGS := $(JIELI_CFLAGS) $(JIELI_DEFINES) -DCONFIG_FLASH_SIZE=$($(1)_FLASH_SIZE) \
	-DBOARD_PID=\"$($(1)_PID)\" $($(1)_DEFINES) $(EXTRA_DEFINES) -DBUILD_ID=\"$(BUILD_ID)\"
$(1)_INCLUDES := $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) -iquote src/platform/jieli/sdkconfig $(BTSTACK_INCLUDES) $(JIELI_INCLUDES)

.PHONY: $(1) $(1)-flash $(1)-loader
$(1): $(O)/$(1)/firmware.img

$(O)/$(1)/src/%.o: src/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(JIELI_CC) $$($(1)_FLAGS) $$(OBJECT_FLAGS) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/sdk/%.o: $(JIELI_SDK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(JIELI_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/btstack/%.o: $(BTSTACK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$(JIELI_CC) $$($(1)_FLAGS) $(BTSTACK_RENAMES) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

# Our controller must not call undeclared functions, -w or not; our files
# that use BTstack see its renamed symbols.
$(O)/$(1)/src/platform/jieli/btctrler/%.o: OBJECT_FLAGS := -Werror=implicit-function-declaration
$(addprefix $(O)/$(1)/,$(BTSTACK_USERS:.c=.o)): OBJECT_FLAGS := $(BTSTACK_RENAMES)
# The build ID: rebuilt when git moves.
$(O)/$(1)/src/core/usb.o: $(wildcard $(BUILD_METADATA))

$(O)/$(1)/app.elf: $(O)/$(1)/sdk.ld $$($(1)_OBJECTS) $(JIELI_ARCHIVES)
	@echo [LD $(1)] $$@
	$$(Q)$(JIELI_LD) -o $$@ $$($(1)_OBJECTS) -T$(O)/$(1)/sdk.ld -M=$(O)/$(1)/app.map $(JIELI_LFLAGS) \
		--start-group $(JIELI_ARCHIVES) --end-group $(JIELI_RUNTIME_LIBS)

$(O)/$(1)/app.bin: $(O)/$(1)/app.elf
	@echo [BIN] $$@
	$$(Q)$(JIELI_OBJCOPY) -O binary -j .text $$< $(O)/$(1)/text.bin
	$$(Q)$(JIELI_OBJCOPY) -O binary -j .data $$< $(O)/$(1)/data.bin
	$$(Q)cat $(O)/$(1)/text.bin $(O)/$(1)/data.bin > $$@

$(O)/$(1)/firmware.img: $(O)/$(1)/app.bin $(O)/$(1)/isd_config.ini $(O)/host/jlnewfw
	@echo [FW] $$@
	$$(Q)$(O)/host/jlnewfw pack --flash-size $($(1)_FLASH_SIZE) --code-limit $$($(1)_CODE_LIMIT) \
		-c $(O)/$(1)/isd_config.ini -u $(JIELI_UBOOT) -o $$@ $(O)/$(1)/app.bin $(JIELI_CONFIG_TOOL)

# Ask running firmware to enter the ROM loader (stock firmware without a
# command for it: by hand), then write the image, encrypted for the
# connected chip's key.
$(1)-flash: $(O)/$(1)/app.bin $(O)/$(1)/isd_config.ini $(O)/host/jlnewfw
	$$(Q)$(O)/host/jlnewfw flashpack --enter-loader --wait 30 --loader $(JIELI_LOADER) $$(if $$(DEVICE),--device $$(DEVICE)) \
		--flash-size $($(1)_FLASH_SIZE) --code-limit $$($(1)_CODE_LIMIT) \
		-c $(O)/$(1)/isd_config.ini -u $(JIELI_UBOOT) $(O)/$(1)/app.bin $(JIELI_CONFIG_TOOL)

# Running firmware into the ROM loader, then wait for it there: the loader
# whose flash holds this board's PID.
$(1)-loader: $(O)/host/jlnewfw
	$$(if $$($(1)_LOADER_HINT),@echo "$$($(1)_LOADER_HINT)")
	$$(Q)$(O)/host/jlnewfw loader --pid $($(1)_PID) --wait 30 --loader $(JIELI_LOADER) $$(if $$(DEVICE),--device $$(DEVICE))

# The SDK's linker script, with the heap ending below the retained RAM and the
# controller's RAM kept clear of the MaskROM boot token.
$(O)/$(1)/sdk_ld.c: $(JIELI_SDK)/cpu/br23/sdk_ld.c $(MAKE_FILES) $(wildcard src/platform/jieli/sdkconfig/*.h)
	@mkdir -p $$(@D)
	$$(Q)sed \
	  -e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
	  -e '/^    \.bss ALIGN(32) :$$$$/,/^        \*(\.bss)$$$$/s/^        \*(\.bss)$$$$/        btctrlermod_bss_base = ABSOLUTE(.);\n&/' \
	  -e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - btctrlermod_bss_base);\n&|' \
		$$< > $$@

$(O)/$(1)/sdk.ld: $(O)/$(1)/sdk_ld.c
	$$(Q)$(JIELI_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -D__LD__ -E -P $$< -o $$@

$(O)/$(1)/isd_config.ini: $(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c $(wildcard src/platform/jieli/sdkconfig/*.h) $(MAKE_FILES)
	@mkdir -p $$(@D)
	$$(Q)$(JIELI_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -D__LD__ -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K -E -P $$< -o $$@
	$$(Q)sed -i -e /^$$$$/d -e 's/^PID = .*;/PID = $($(1)_PID);/' $$@

$$($(1)_OBJECTS) $(O)/$(1)/sdk.ld: $(MAKE_FILES) $(wildcard src/platform/jieli/sdkconfig/*)
$(call board_defines,$(1))
-include $$($(1)_OBJECTS:.o=.d)
endef
