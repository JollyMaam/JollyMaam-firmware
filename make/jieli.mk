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
JIELI_LOADER := $(JIELI_SDK)/cpu/br23/tools/br23loader.bin

# VM (settings and bonds) is the VM_KB below BTIF (4 KiB) and key_mac (4 KiB)
# at the end of flash; code may use everything below it (the code limit).
# Changing VM_KB or the flash size moves VM.  16 KiB is the SDK's largest.
JIELI_VM_KB := 16
jieli_code_limit = $(shell printf 0x%x $$(( $(1) - 0x2000 - $(JIELI_VM_KB) * 1024 )))

JIELI_CFLAGS := \
	-target pi32v2 \
	-mcpu=r3 \
	-integrated-as \
	-flto \
	-Oz \
	-g \
	-Wuninitialized \
	-Wno-invalid-noreturn \
	-fno-common \
	-fallow-pointer-null \
	-fprefer-gnu-section \
	-Wno-shift-negative-value \
	-Wundef \
	-Wframe-larger-than=256 \
	-Wincompatible-pointer-types \
	-Wreturn-type \
	-Wimplicit-function-declaration \
	-fms-extensions \
	-w \

JIELI_DEFINES := \
	-DAPP_ASYNC_POOL_NUM_CONFIG=0 \
	-DCONFIG_AEC_M=640 \
	-DCONFIG_AUDIO_ONCHIP \
	-DCONFIG_BTCTRLER_TASK_DEL_ENABLE \
	-DCONFIG_CPU_BR23 \
	-DCONFIG_DNS_ENABLE \
	-DCONFIG_EQ_SUPPORT_ASYNC \
	-DCONFIG_EVENT_KEY_MAP_ENABLE=0 \
	-DCONFIG_FREE_RTOS_ENABLE \
	-DCONFIG_HID_CASE_ENABLE \
	-DCONFIG_ITEM_FORMAT_VM \
	-DCONFIG_LINK_DISTURB_SCAN_ENABLE=0 \
	-DCONFIG_LITE_AEC_ENABLE=0 \
	-DCONFIG_LITE_AUDIO \
	-DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE \
	-DCONFIG_LMP_RESET_ENABLE \
	-DCONFIG_MEDIA_DEVELOP_ENABLE \
	-DCONFIG_MIXER_CYCLIC \
	-DCONFIG_MMU_ENABLE \
	-DCONFIG_MSBC_CODEC_HW \
	-DCONFIG_NEW_ECC_ENABLE \
	-DCONFIG_OTA_UPDATA_ENABLE \
	-DCONFIG_RELEASE_ENABLE \
	-DCONFIG_SBC_CODEC_HW \
	-DCONFIG_TRANSFER_ENABLE \
	-DCONFIG_TWS_ENABLE \
	-DCONFIG_UPDATA_ENABLE \
	-DEVENT_HANDLER_NUM_CONFIG=2 \
	-DEVENT_POOL_SIZE_CONFIG=256 \
	-DEVENT_TOUCH_ENABLE_CONFIG=0 \
	-D__GCC_PI32V2__ \
	-DSDFILE_VERSION=0x020000 \
	-DSDK_VERSION_CFG_DEFINE=0x240001 \
	-DSDK_VERSION_DATE_DEFINE=20241231 \
	-D__SHELL__ \
	-DSUPPORT_MS_EXTENSIONS \
	-DTCFG_APP_BT_EN=1 \
	-DTIMER_POOL_NUM_CONFIG=10 \
	-DUSB_PC_NO_APP_MODE \
	-DUSE_SDFILE_NEW=1 \
	-DVFS_ENABLE=1 \
	-DVFS_FILE_POOL_NUM_CONFIG=0 \
	-DVM_ITEM_MAX_NUM=256 \
	-DVM_MAX_SIZE_CONFIG=16*1024 \

JIELI_INCLUDES := \
	-I$(JIELI_SDK)/include_lib \
	-I$(JIELI_SDK)/include_lib/driver \
	-I$(JIELI_SDK)/include_lib/driver/device \
	-I$(JIELI_SDK)/include_lib/driver/cpu/br23 \
	-I$(JIELI_SDK)/include_lib/system \
	-I$(JIELI_SDK)/include_lib/system/generic \
	-I$(JIELI_SDK)/include_lib/system/device \
	-I$(JIELI_SDK)/include_lib/system/fs \
	-I$(JIELI_SDK)/include_lib/system/ui \
	-I$(JIELI_SDK)/include_lib/system/math/cpu/br23 \
	-I$(JIELI_SDK)/include_lib/btctrler \
	-I$(JIELI_SDK)/include_lib/btctrler/port/br23 \
	-I$(JIELI_SDK)/include_lib/update \
	-I$(JIELI_SDK)/include_lib/agreement \
	-I$(JIELI_SDK)/include_lib/btstack \
	-I$(JIELI_SDK)/include_lib/btstack/third_party/common \
	-I$(JIELI_SDK)/include_lib/btstack/third_party/rcsp \
	-I$(JIELI_SDK)/include_lib/media/media_develop \
	-I$(JIELI_SDK)/include_lib/media/media_develop/media \
	-I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23 \
	-I$(JIELI_SDK)/include_lib/media/media_develop/media/cpu/br23/asm \
	-I$(JIELI_SDK)/include_lib/ai_stack/JL_rcsp \
	-I$(JIELI_SDK)/include_lib/system/ex_mcu \
	-I$(JIELI_SDK)/apps/common \
	-I$(JIELI_SDK)/apps/common/include \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/gatt_common \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/online_db \
	-I$(JIELI_SDK)/apps/common/third_party_profile/jieli/trans_data_demo \
	-I$(JIELI_SDK)/apps/common/third_party_profile/common \
	-I$(JIELI_SDK)/apps/common/device \
	-I$(JIELI_SDK)/apps/common/device/usb \
	-I$(JIELI_SDK)/apps/common/device/usb/device \
	-I$(JIELI_SDK)/apps/common/device/usb/host \
	-I$(JIELI_SDK)/apps/common/audio \
	-I$(JIELI_SDK)/apps/common/code_switch/include \
	-I$(JIELI_SDK)/apps/common/device/optical_mouse_sensor/include \
	-I$(JIELI_SDK)/apps/common/device/touch_pad \
	-I$(JIELI_SDK)/cpu/br23 \
	-I$(JIELI_SDK)/cpu/br23/audio_dec \
	-I$(JIELI_SDK)/cpu/br23/audio_enc \
	-I$(JIELI_TOOLS)/../include \

JIELI_RUNTIME_LIBS := \
	$(JIELI_TOOLS)/../lib/r3/libm.a \
	$(JIELI_TOOLS)/../lib/r3/libc.a \
	$(JIELI_TOOLS)/../lib/r3/libcompiler-rt.a \

JIELI_LFLAGS := \
	--plugin-opt=-pi32v2-always-use-itblock=false \
	--plugin-opt=-enable-ipra=true \
	--plugin-opt=-pi32v2-merge-max-offset=4096 \
	--plugin-opt=-pi32v2-enable-simd=true \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-global-merge-on-const \
	--plugin-opt=-inline-threshold=5 \
	--plugin-opt=-inline-max-allocated-size=32 \
	--plugin-opt=-inline-normal-into-special-section=true \
	--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
	--plugin-opt=-pi32v2-enable-rep-memop \
	--plugin-opt=-warn-stack-size=256 \
	--sort-common \
	--dont-complain-call-overflow \
	--gc-sections \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-mattr=+fprev1 \

JIELI_ARCHIVES := \
	$(JIELI_SDK)/cpu/br23/liba/crypto_toolbox_Osize.a \
	$(JIELI_SDK)/cpu/br23/liba/cpu.a \
	$(JIELI_SDK)/cpu/br23/liba/system.a \
	$(JIELI_SDK)/cpu/br23/liba/update.a \
	$(JIELI_SDK)/cpu/br23/liba/media.a \

# The SDK sources the app uses.
JIELI_SDK_USED := \
	apps/common/debug/debug.c \
	apps/common/device/usb/device/descriptor.c \
	apps/common/device/usb/device/msd.c \
	apps/common/device/usb/device/usb_device.c \
	apps/common/device/usb/device/user_setup.c \
	apps/common/device/usb/usb_config.c \
	cpu/br23/adc_api.c \
	cpu/br23/charge.c \
	cpu/br23/clock_manager.c \
	cpu/br23/led_spi.c \
	cpu/br23/mcpwm.c \
	cpu/br23/setup.c

# $(call jieli_board,NAME): build NAME on the BR23, into $(O)/NAME/.  Set first:
#   NAME_FLASH_SIZE  the flash device's size
#   NAME_PID         the image's PID: the flasher writes only over the same PID
#   NAME_SOURCES     the app's sources; our controller (btctrler/) and BTstack come with it
#   NAME_DEFINES     -D flags for this board (EXTRA_DEFINES adds to them)
#   NAME_BOARD_DIR   the board's headers (btstack_config.h)
# Targets: NAME (firmware.img), NAME-flash (through the ROM loader; DEVICE=
# picks one of several).  Inside the template, $(1) is NAME, $(...) is
# expanded once when it is called, and $$(...) when the rule runs.
define jieli_board
$(1)_CODE_LIMIT := $(call jieli_code_limit,$($(1)_FLASH_SIZE))
$(1)_OBJECTS := $(patsubst %.c,$(O)/$(1)/%.o,$($(1)_SOURCES) $(wildcard src/platform/jieli/btctrler/*.c)) \
	$(JIELI_SDK_USED:%.c=$(O)/$(1)/sdk/%.o) $(BTSTACK_USED:%.c=$(O)/$(1)/btstack/%.o)
$(1)_FLAGS := $(JIELI_CFLAGS) $(JIELI_DEFINES) -DCONFIG_FLASH_SIZE=$($(1)_FLASH_SIZE) \
	$($(1)_DEFINES) $(EXTRA_DEFINES) -DBUILD_ID=\"$(BUILD_ID)\"
$(1)_INCLUDES := $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) -iquote src/platform/jieli/sdkconfig $(BTSTACK_INCLUDES) $(JIELI_INCLUDES)

.PHONY: $(1) $(1)-flash
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

# Ask running firmware to enter the ROM loader (stock firmware: enter it by
# hand), then write the image, encrypted for the connected chip's key.
$(1)-flash: $(O)/$(1)/app.bin $(O)/$(1)/isd_config.ini $(O)/host/jlnewfw
	$$(Q)$(O)/host/jlnewfw flashpack --enter-loader --wait 30 --loader $(JIELI_LOADER) $$(if $$(DEVICE),--device $$(DEVICE)) \
		--flash-size $($(1)_FLASH_SIZE) --code-limit $$($(1)_CODE_LIMIT) \
		-c $(O)/$(1)/isd_config.ini -u $(JIELI_UBOOT) $(O)/$(1)/app.bin $(JIELI_CONFIG_TOOL)

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
