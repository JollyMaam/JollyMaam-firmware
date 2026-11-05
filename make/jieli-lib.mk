# Jieli BR23 boards on lib.mk: the toolchain, the build steps as target types,
# and $(call jieli-board,NAME), which only declares a board's targets and
# their properties (lib.mk makes the rules). The data (flags, defines, SDK
# sources) is in make/jieli.mk.
#
# A board sets, before $(eval $(call jieli-board,NAME)):
#   NAME_FLASH_SIZE  the flash device's size
#   NAME_PID         the image's PID (BOARD_PID; libjollymaam's products.c knows it)
#   NAME_SOURCES     the app's sources; our controller (btctrler/) and BTstack come with it
#   NAME_DEFINES     -D flags for this board (EXTRA_DEFINES adds to them)
#   NAME_BOARD_DIR   the board's headers (btstack_config.h)
#   NAME_NO_BT       optional: without our controller and BTstack
#   NAME_BTSTACK     optional: the BTstack sources, instead of BTSTACK_USED
#   NAME_UPDATER     optional: this is an updater (src/updater/) for an app this
#                    far above it (MAIN_AT); its entry goes before the SDK's start-up
#   NAME_MAIN_AT     optional: the app goes this far above the app area's start,
#   NAME_FRONT       and the app area holds board NAME_FRONT, its updater
# Targets, all in $(OBJDIR)/NAME/: sdk_ld.c, sdk.ld, isd_config.ini, app.elf
# (and app.map), app.bin, firmware.img (and update.bin with NAME_MAIN_AT).
# NAME alone builds firmware.img.

# The toolchain: NAME_toolchain := JIELI_ reads these.
JIELI_LINK := $(JIELI_LD)
JIELI_CPPFLAGS := $(JIELI_DEFINES)
JIELI_LDFLAGS := $(JIELI_LFLAGS)
JIELI_LDLIBS := --start-group $(JIELI_ARCHIVES) --end-group $(JIELI_RUNTIME_LIBS)

# Preprocessing (sdk.ld, isd_config.ini) is a compile step with the board's
# flags; the "object" is the preprocessed text and lib-type-copy or
# lib-type-jieli-isd makes the output from it.
jieli-cpp = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -D__LD__ -E -P -MMD -MP -MF $(@:.o=.d) -MT $@ $< -o $@
lib-type-copy = cp $(lib_objects) $@

# The SDK's linker script, with the heap ending below the retained RAM and the
# controller's RAM kept clear of the MaskROM boot token; for an app above an
# updater (jieli_main_at) moved up, for an updater (jieli_updater) with its
# entry first.
define lib-type-jieli-sdk-ld
sed
-e 's/^_HEAP_END = RAM0_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^\([[:space:]]*\)_HEAP_END = RAM0_END;/\1_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/'
$(if $(jieli_main_at),-e 's/^\(\s*code0(rx)\s*:\s*ORIGIN = *0x1E00120\)$(comma)/\1 + $(jieli_main_at)$(comma)/')
$(if $(jieli_main_at),-e 's/^CODE_BEG = 0x1E00120;/CODE_BEG = 0x1E00120 + $(jieli_main_at);/')
$(if $(jieli_updater),-e 's/^\(\s*\)\*(\.startup\.text)/\1KEEP(*(.updater_entry))\n&/')
$(if $(jieli_updater),-e 's/^#include "sdk_used_list.c"$$/sdfile_vfs_ops/')
-e '/^    \.bss ALIGN(32) :$$/,/^        \*(\.bss)$$/s/^        \*(\.bss)$$/        btctrlermod_bss_base = ABSOLUTE(.);\n&/'
-e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - btctrlermod_bss_base);\n&|'
$(firstword $(lib_deps)) > $@
endef

# The SDK's download config, preprocessed, with the board's PID.
lib-type-jieli-isd = sed -e '/^$$/d' -e 's/^PID = .*;/PID = $(jieli_pid);/' $(lib_objects) > $@

# The image's code and data sections, as the loader writes them.
define lib-type-jieli-bin
$(JIELI_OBJCOPY) -O binary -j .text $(firstword $(lib_deps)) $@.text &&
$(JIELI_OBJCOPY) -O binary -j .data $(firstword $(lib_deps)) $@.data &&
cat $@.text $@.data > $@ &&
rm -f $@.text $@.data
endef

# The flashable image; with an updater in front, also update.bin (the app
# alone, for the updater).
define lib-type-jieli-img
$(JOLLYMAAM_CLI) pack jieli --flash-size $(jieli_flash_size) --code-limit $(jieli_code_limit)
-c $(jieli_isd) -u $(jieli_uboot) -o $@ $(jieli_packed) $(jieli_config_tool)
$(if $(jieli_main_at),--main-out $(dir $@)update.bin)
endef

# Inside the template, $(1) is NAME and $(...) is expanded when it is
# called; $$(...) is left for $(eval), which reads each value once.
define jieli-board
$(1)_DIR := $(OBJDIR)/$(1)
$(1)_CODE_LIMIT := $$(call jieli_code_limit,$$($(1)_FLASH_SIZE))
$(1)_FLAGS := -DCONFIG_FLASH_SIZE=$$($(1)_FLASH_SIZE) -DBOARD_PID=\"$$($(1)_PID)\" $$($(1)_DEFINES) $$(EXTRA_DEFINES) -DBUILD_ID=\"$$(BUILD_ID)\" $$(if $$($(1)_NO_BT),-DNO_BT) $$(if $$($(1)_UPDATER),-DMAIN_AT=$$($(1)_UPDATER) -DCODE_LIMIT=$$($(1)_CODE_LIMIT))
$(1)_INCLUDES := $$(SRC_INCLUDES) -iquote $$($(1)_BOARD_DIR) -iquote src/platform/jieli/sdkconfig $$(BTSTACK_INCLUDES) $$(JIELI_INCLUDES)

TARGETS += $(1)/sdk_ld.c
$(1)/sdk_ld.c_type := jieli-sdk-ld
$(1)/sdk_ld.c_deps := $$(JIELI_SDK)/cpu/br23/sdk_ld.c
$$($(1)_DIR)/sdk_ld.c: private jieli_main_at := $$($(1)_MAIN_AT)
$$($(1)_DIR)/sdk_ld.c: private jieli_updater := $$($(1)_UPDATER)

TARGETS += $(1)/sdk.ld
$(1)/sdk.ld_type := copy
$(1)/sdk.ld_toolchain := JIELI_
$(1)/sdk.ld_sources := $$($(1)_DIR)/sdk_ld.c
$(1)/sdk.ld_cppflags := $$($(1)_FLAGS) $$($(1)_INCLUDES)
$(1)/sdk.ld_compile.c = $$(jieli-cpp)

TARGETS += $(1)/isd_config.ini
$(1)/isd_config.ini_type := jieli-isd
$(1)/isd_config.ini_toolchain := JIELI_
$(1)/isd_config.ini_sources := $$(JIELI_SDK)/cpu/br23/tools/isd_config_rule.c
$(1)/isd_config.ini_cppflags := $$($(1)_FLAGS) $$($(1)_INCLUDES) -DCONFIG_VM_LEAST_SIZE=$$(JIELI_VM_KB)K
$(1)/isd_config.ini_compile.c = $$(jieli-cpp)
$$($(1)_DIR)/isd_config.ini: private jieli_pid := $$($(1)_PID)

TARGETS += $(1)/app.elf
$(1)/app.elf_toolchain := JIELI_
$(1)/app.elf_sources := $$($(1)_SOURCES) src/core/battery.c \
	$$(if $$($(1)_NO_BT),,$$(wildcard src/platform/jieli/btctrler/*.c)) \
	$$(addprefix $$(JIELI_SDK)/,$$(JIELI_SDK_USED)) \
	$$(if $$($(1)_NO_BT),,$$(addprefix $$(BTSTACK)/,$$(or $$($(1)_BTSTACK),$$(BTSTACK_USED))))
$(1)/app.elf_cppflags := $$($(1)_FLAGS) $$($(1)_INCLUDES)
$(1)/app.elf_ldflags := -T$$($(1)_DIR)/sdk.ld -M=$$($(1)_DIR)/app.map
$(1)/app.elf_deps := $$($(1)_DIR)/sdk.ld $$(JIELI_ARCHIVES)
# Our controller must not call undeclared functions, -w or not; BTstack and
# our files that use it see its renamed symbols.
$$(call lib-objs,$(1)/app.elf,$$(filter src/platform/jieli/btctrler/%,$$($(1)/app.elf_sources))): obj_cflags += -Werror=implicit-function-declaration
$$(call lib-objs,$(1)/app.elf,$$(filter $$(BTSTACK)/% $$(BTSTACK_USERS),$$($(1)/app.elf_sources))): obj_cppflags += $$(BTSTACK_RENAMES)

TARGETS += $(1)/app.bin
$(1)/app.bin_type := jieli-bin
$(1)/app.bin_deps := $$($(1)_DIR)/app.elf

TARGETS += $(1)/firmware.img
$(1)/firmware.img_type := jieli-img
$(1)/firmware.img_deps := $$($(1)_DIR)/app.bin $$(if $$($(1)_MAIN_AT),$$(OBJDIR)/$$($(1)_FRONT)/app.bin) \
	$$($(1)_DIR)/isd_config.ini $$(JIELI_UBOOT) $$(JIELI_CONFIG_TOOL) $$(JOLLYMAAM_CLI)
$$($(1)_DIR)/firmware.img: private jieli_flash_size := $$($(1)_FLASH_SIZE)
$$($(1)_DIR)/firmware.img: private jieli_code_limit := $$($(1)_CODE_LIMIT)
$$($(1)_DIR)/firmware.img: private jieli_isd := $$($(1)_DIR)/isd_config.ini
$$($(1)_DIR)/firmware.img: private jieli_uboot := $$(JIELI_UBOOT)
$$($(1)_DIR)/firmware.img: private jieli_config_tool := $$(JIELI_CONFIG_TOOL)
$$($(1)_DIR)/firmware.img: private jieli_main_at := $$($(1)_MAIN_AT)
$$($(1)_DIR)/firmware.img: private jieli_packed := $$(if $$($(1)_MAIN_AT),$$(OBJDIR)/$$($(1)_FRONT)/app.bin --main $$($(1)_MAIN_AT):$$($(1)_DIR)/app.bin,$$($(1)_DIR)/app.bin)

.PHONY: $(1)
$(1): $$($(1)_DIR)/firmware.img
endef
