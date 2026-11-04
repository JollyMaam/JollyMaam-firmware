# The Jieli BR23: toolchain, SDK and the rules that build a board on it.
# The compiler ABI and library configuration are from the pinned SDK, mostly
# copied from apps/hid/board/br23/Makefile; we never include a vendor
# Makefile.  A product file sets a board's variables, then calls
# $(call jieli_board,NAME) (at the end of this file).

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
JIELI_LOADER := $(JIELI_SDK)

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
jieli_chip = $(or $($(1)_CHIP),br23)
jieli_tools = $(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_TOOLS),$(JIELI_TOOLS))

# $(call jieli_board,NAME): build NAME on the BR23, into $(O)/NAME/.  Set first:
#   NAME_FLASH_SIZE  the flash device's size
#   NAME_PID         the image's PID: the flasher writes only over the same PID;
#                    the firmware names itself by it (BOARD_PID)
#   NAME_SOURCES     the app's sources; our controller (btctrler/) and BTstack come with it
#   NAME_DEFINES     -D flags for this board (EXTRA_DEFINES adds to them)
#   NAME_BOARD_DIR   the board's headers (btstack_config.h)
#   NAME_LOADER_HINT how to enter the ROM loader by hand, where software cannot
#   NAME_NO_BT       optional: without our controller and BTstack
#   NAME_BTSTACK     optional: the BTstack sources, instead of BTSTACK_USED
#   NAME_UPDATER     optional: this is an updater (src/updater/) for an app this
#                    far above it (MAIN_AT); its entry goes before the SDK's start-up
#   NAME_MAIN_AT     optional: the app goes this far above the app area's start,
#   NAME_FRONT       and the app area holds board NAME_FRONT, its updater
#   NAME_KEEP        optional ADDR:LEN ranges NAME-flash writes back as the device held them
#   NAME_NO_FLASH    optional: no NAME-flash (a part of what another board flashes)
# Targets: NAME (firmware.img), NAME-flash (through the ROM loader),
# NAME-loader (into the ROM loader and wait there); with more than one
# device for the board, DEVICE= picks one: its USB port, as jollymaam jieli scan
# lists it.  Inside the template, $(1) is NAME, $(...) is expanded once when
# it is called, and $$(...) when the rule runs.
define jieli_board
$(1)_CC := $(call jieli_tools,$(1))/clang
$(1)_LD := $(call jieli_tools,$(1))/ld --plugin $(call jieli_tools,$(1))/LLVMgold.so
$(1)_OBJCOPY := $(call jieli_tools,$(1))/objcopy
$(1)_ARCHIVES := $(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_ARCHIVES),$(JIELI_ARCHIVES))
$(1)_LFLAGS := $(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_LFLAGS),$(JIELI_LFLAGS))
$(1)_RUNTIME := $(if $(filter bd19,$(call jieli_chip,$(1))),$(addprefix $(JIELI_BD19_TOOLS)/../lib/,libm.a libc.a libcompiler-rt.a),$(JIELI_RUNTIME_LIBS))
$(1)_UBOOT := $(JIELI_SDK)/cpu/$(call jieli_chip,$(1))/tools/uboot.boot
$(1)_CONFIG_TOOL := $(JIELI_SDK)/cpu/$(call jieli_chip,$(1))/tools/cfg_tool.bin
$(1)_CODE_LIMIT := $(call jieli_code_limit,$($(1)_FLASH_SIZE))
# With NAME_MAIN_AT the app area holds NAME_FRONT's app and this app sits above.
$(1)_FRONT_BIN := $(if $($(1)_MAIN_AT),$(O)/$($(1)_FRONT)/app.bin)
$(1)_PACKED := $(if $($(1)_MAIN_AT),$$($(1)_FRONT_BIN) --main $($(1)_MAIN_AT):$(O)/$(1)/app.bin,$(O)/$(1)/app.bin)
$(1)_OBJECTS := $(patsubst %.c,$(O)/$(1)/%.o,$($(1)_SOURCES) src/core/battery.c $(if $(filter bd19,$(call jieli_chip,$(1)))$($(1)_NO_BT),,$(wildcard src/platform/jieli/btctrler/*.c))) \
	$(patsubst %.c,$(O)/$(1)/sdk/%.o,$(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_SDK_USED),$(JIELI_SDK_USED))) $(if $($(1)_NO_BT),,$(patsubst %.c,$(O)/$(1)/btstack/%.o,$(or $($(1)_BTSTACK),$(BTSTACK_USED))))
$(1)_FLAGS := $(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_CFLAGS) $(JIELI_BD19_DEFINES),$(JIELI_CFLAGS) $(JIELI_DEFINES)) -DCONFIG_FLASH_SIZE=$($(1)_FLASH_SIZE) \
	-DBOARD_PID=\"$($(1)_PID)\" $($(1)_DEFINES) $(EXTRA_DEFINES) -DBUILD_ID=\"$(BUILD_ID)\" \
	$(if $($(1)_NO_BT),-DNO_BT) $(if $($(1)_UPDATER),-DMAIN_AT=$($(1)_UPDATER) -DCODE_LIMIT=$$($(1)_CODE_LIMIT))
$(1)_INCLUDES := $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) -iquote src/platform/jieli/sdkconfig $(BTSTACK_INCLUDES) $(if $(filter bd19,$(call jieli_chip,$(1))),$(JIELI_BD19_INCLUDES),$(JIELI_INCLUDES))

.PHONY: $(1) $(if $($(1)_NO_FLASH),,$(1)-flash) $(1)-loader
$(1): $(O)/$(1)/firmware.img

$(O)/$(1)/src/%.o: src/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$$($(1)_CC) $$($(1)_FLAGS) $$(OBJECT_FLAGS) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/sdk/%.o: $(JIELI_SDK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$$($(1)_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

$(O)/$(1)/btstack/%.o: $(BTSTACK)/%.c $(O)/$(1)/defines
	@mkdir -p $$(@D)
	@echo [CC $(1)] $$<
	$$(Q)$$($(1)_CC) $$($(1)_FLAGS) $(BTSTACK_RENAMES) $$($(1)_INCLUDES) -MMD -MP -c $$< -o $$@

# Our controller must not call undeclared functions, -w or not; our files
# that use BTstack see its renamed symbols.
$(O)/$(1)/src/platform/jieli/btctrler/%.o: OBJECT_FLAGS := -Werror=implicit-function-declaration
$(addprefix $(O)/$(1)/,$(BTSTACK_USERS:.c=.o)): OBJECT_FLAGS := $(BTSTACK_RENAMES)
# The build ID: rebuilt when git moves.
$(O)/$(1)/src/core/usb.o: $(wildcard $(BUILD_METADATA))

$(O)/$(1)/app.elf: $(O)/$(1)/sdk.ld $$($(1)_OBJECTS) $$($(1)_ARCHIVES)
	@echo [LD $(1)] $$@
	$$(Q)$$($(1)_LD) -o $$@ $$($(1)_OBJECTS) -T$(O)/$(1)/sdk.ld -M=$(O)/$(1)/app.map $$($(1)_LFLAGS) \
		--start-group $$($(1)_ARCHIVES) --end-group $$($(1)_RUNTIME)

$(O)/$(1)/app.bin: $(O)/$(1)/app.elf
	@echo [BIN] $$@
	$$(Q)$$($(1)_OBJCOPY) -O binary -j .text $$< $(O)/$(1)/text.bin
	$$(Q)$$($(1)_OBJCOPY) -O binary -j .data $$< $(O)/$(1)/data.bin
	$$(if $$(filter bd19,$$(call jieli_chip,$(1))),$$(Q)$$($(1)_OBJCOPY) -O binary -j .data_code $$< $(O)/$(1)/data_code.bin)
	$$(Q)cat $(O)/$(1)/text.bin $(O)/$(1)/data.bin $$(if $$(filter bd19,$$(call jieli_chip,$(1))),$(O)/$(1)/data_code.bin) > $$@

$(O)/$(1)/firmware.img: $(O)/$(1)/app.bin $$($(1)_FRONT_BIN) $(O)/$(1)/isd_config.ini $(O)/host/jollymaam-pack
	@echo [FW] $$@
	$$(Q)$(O)/host/jollymaam-pack jieli pack --flash-size $($(1)_FLASH_SIZE) --code-limit $$($(1)_CODE_LIMIT) \
		-c $(O)/$(1)/isd_config.ini -u $$($(1)_UBOOT) -o $$@ $$($(1)_PACKED) $$($(1)_CONFIG_TOOL) \
		$(if $($(1)_MAIN_AT),--main-out $(O)/$(1)/update.bin)

# Ask running firmware to enter the ROM loader (stock firmware without a
# command for it: by hand), then write the image, encrypted for the
# connected chip's key.
ifeq ($($(1)_NO_FLASH),)
$(1)-flash: $(O)/$(1)/app.bin $$($(1)_FRONT_BIN) $(O)/$(1)/isd_config.ini $(O)/host/jollymaam
	$$(Q)$(O)/host/jollymaam jieli flashpack --enter-loader --wait 30 --loader $(JIELI_LOADER) $$(if $$(DEVICE),--device $$(DEVICE)) \
		--flash-size $($(1)_FLASH_SIZE) --code-limit $$($(1)_CODE_LIMIT) \
		-c $(O)/$(1)/isd_config.ini -u $$($(1)_UBOOT) $$($(1)_PACKED) $$($(1)_CONFIG_TOOL) \
		$(foreach k,$($(1)_KEEP),--keep $(k))
endif

# Running firmware into the ROM loader, then wait for it there: the loader
# whose flash holds this board's PID.
$(1)-loader: $(O)/host/jollymaam
	$$(if $$($(1)_LOADER_HINT),@echo "$$($(1)_LOADER_HINT)")
	$$(Q)$(O)/host/jollymaam jieli loader --pid $($(1)_PID) --wait 30 --loader $(JIELI_LOADER) $$(if $$(DEVICE),--device $$(DEVICE))

# The SDK's linker script, with the heap ending below the retained RAM and the
# controller's RAM kept clear of the MaskROM boot token.
$(O)/$(1)/sdk_ld.c: $(JIELI_SDK)/cpu/$(call jieli_chip,$(1))/sdk_ld.c $(MAKE_FILES) $(wildcard src/platform/jieli/sdkconfig/*.h)
	@mkdir -p $$(@D)
	$$(Q)sed \
  -e 's/^_HEAP_END = RAM0_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
  -e 's/^\([[:space:]]*\)_HEAP_END = RAM0_END;/\1_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/' \
	  -e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/' \
	  $(if $($(1)_MAIN_AT),-e 's/^\(\s*code0(rx)\s*:\s*ORIGIN = *0x1E00120\)$(comma)/\1 + $($(1)_MAIN_AT)$(comma)/') \
	  $(if $($(1)_MAIN_AT),-e 's/^CODE_BEG = 0x1E00120;/CODE_BEG = 0x1E00120 + $($(1)_MAIN_AT);/') \
	  $(if $($(1)_UPDATER),-e 's/^\(\s*\)\*(\.startup\.text)/\1KEEP(*(.updater_entry))\n&/') \
	  $(if $($(1)_UPDATER),-e 's/^#include "sdk_used_list.c"$$$$/sdfile_vfs_ops/') \
	  -e '/^    \.bss ALIGN(32) :$$$$/,/^        \*(\.bss)$$$$/s/^        \*(\.bss)$$$$/        btctrlermod_bss_base = ABSOLUTE(.);\n&/' \
	  -e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - btctrlermod_bss_base);\n&|' \
		$$< > $$@

	$$(if $$(filter bd19,$$(call jieli_chip,$(1))),$$(Q)printf '\n#include "bd19_app.ld"\n' >> $$@)

$(O)/$(1)/sdk.ld: $(O)/$(1)/sdk_ld.c
	$$(Q)$$($(1)_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -D__LD__ -E -P $$< -o $$@

$(O)/$(1)/isd_config.ini: $(JIELI_SDK)/cpu/$(call jieli_chip,$(1))/tools/isd_config_rule.c $(wildcard src/platform/jieli/sdkconfig/*.h) $(MAKE_FILES)
	@mkdir -p $$(@D)
	$$(Q)$$($(1)_CC) $$($(1)_FLAGS) $$($(1)_INCLUDES) -D__LD__ -DCONFIG_VM_LEAST_SIZE=$(JIELI_VM_KB)K -E -P $$< -o $$@
	$$(Q)sed -i -e /^$$$$/d -e 's/^PID = .*;/PID = $($(1)_PID);/' $$@
	$$(if $$(filter bd19,$$(call jieli_chip,$(1))),$$(Q)printf '\n[JLNEWFW]\nUBOOT_FLAGS = 0x40;\nHEADER_FS = 11;\nRES = $(abspath $(JIELI_SDK))/cpu/bd19/tools/p11_code.bin:82;\n' >> $$@)

$$($(1)_OBJECTS) $(O)/$(1)/sdk.ld: $(MAKE_FILES) $(wildcard src/platform/jieli/sdkconfig/*)
$(call board_defines,$(1))
-include $$($(1)_OBJECTS:.o=.d)

endef
