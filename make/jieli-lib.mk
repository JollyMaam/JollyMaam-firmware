# Jieli BR23 boards on lib.mk: the toolchain, shared source lists and the
# build steps as target types. Each board spells out its own targets in its
# rules.mk; nothing here depends on a board. The data (flags, defines, SDK
# sources) is in make/jieli.mk.

# The BR23 toolchain: NAME_toolchain := JIELI_ reads these.
JIELI_LINK := $(JIELI_LD)
JIELI_CPPFLAGS := $(JIELI_DEFINES)
JIELI_LDFLAGS := $(JIELI_LFLAGS)
JIELI_LDLIBS := --start-group $(JIELI_ARCHIVES) --end-group $(JIELI_RUNTIME_LIBS)

# Include directories after a board's own (SRC_INCLUDES, then its -iquote).
JIELI_BOARD_INCLUDES := -iquote src/platform/jieli/sdkconfig $(BTSTACK_INCLUDES) $(JIELI_INCLUDES)

# Sources a board's app.elf lists after its own: our controller, the SDK
# sources the app uses, BTstack.
JIELI_BTCTRLER_SOURCES := $(wildcard src/platform/jieli/btctrler/*.c)
JIELI_SDK_SOURCES := $(addprefix $(JIELI_SDK)/,$(JIELI_SDK_USED))
BTSTACK_SOURCES := $(addprefix $(BTSTACK)/,$(BTSTACK_USED))

# Preprocessing (sdk.ld, isd_config.ini) is a compile step with the board's
# flags: the "object" is the preprocessed text, and a type makes the output.
jieli-cpp = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -D__LD__ -E -P -MMD -MP -MF $(@:.o=.d) -MT $@ $< -o $@
lib-type-copy = cp $(lib_objects) $@

# The SDK's linker script (the target's first _deps) edited by the sed
# expressions in the target's jieli_sed:
#   JIELI_SDK_LD_SED                 every board: the heap ends below the
#                                    retained RAM, the controller's RAM stays
#                                    clear of the MaskROM boot token
#   $(call jieli_sdk_ld_main_at,OFF) an app this far above the app area's start
#   JIELI_SDK_LD_SED_UPDATER         an updater: its entry before the SDK's start-up
lib-type-jieli-sdk-ld = sed $(jieli_sed) $(firstword $(lib_deps)) > $@
define JIELI_SDK_LD_SED
-e 's/^_HEAP_END = RAM0_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^\([[:space:]]*\)_HEAP_END = RAM0_END;/\1_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^_HEAP_END = RAM_END;/_HEAP_END = RESET_MAILBOX_BEGIN;/'
-e 's/^PROVIDE(HEAP_END = RAM_END);/PROVIDE(HEAP_END = RESET_MAILBOX_BEGIN);/'
-e '/^    \.bss ALIGN(32) :$$/,/^        \*(\.bss)$$/s/^        \*(\.bss)$$/        btctrlermod_bss_base = ABSOLUTE(.);\n&/'
-e 's|^\([[:space:]]*\)#include "btctrler/btctler_lib_bss.ld"|\1. = MAX(., 0x10810 - btctrlermod_bss_base);\n&|'
endef
jieli_sdk_ld_main_at = -e 's/^\(\s*code0(rx)\s*:\s*ORIGIN = *0x1E00120\)$(comma)/\1 + $(1)$(comma)/' -e 's/^CODE_BEG = 0x1E00120;/CODE_BEG = 0x1E00120 + $(1);/'
define JIELI_SDK_LD_SED_UPDATER
-e 's/^\(\s*\)\*(\.startup\.text)/\1KEEP(*(.updater_entry))\n&/'
-e 's/^#include "sdk_used_list.c"$$/sdfile_vfs_ops/'
endef

# The SDK's download config, preprocessed, with the target's jieli_pid.
lib-type-jieli-isd = sed -e '/^$$/d' -e 's/^PID = .*;/PID = $(jieli_pid);/' $(lib_objects) > $@

# The BR23 image: the ELF's (first _deps) code and data, as the loader writes them.
define lib-type-jieli-bin
$(JIELI_OBJCOPY) -O binary -j .text $(firstword $(lib_deps)) $@.text &&
$(JIELI_OBJCOPY) -O binary -j .data $(firstword $(lib_deps)) $@.data &&
cat $@.text $@.data > $@ &&
rm -f $@.text $@.data
endef

# The flashable image, packed by jollymaam with the target's jieli_pack
# (sizes, config, loader, images; see jollymaam help).
lib-type-jieli-img = $(JOLLYMAAM_CLI) pack jieli -o $@ $(jieli_pack)
