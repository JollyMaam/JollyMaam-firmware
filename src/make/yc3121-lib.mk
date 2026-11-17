# Yichip YC3121 (Cortex-M0) boards on lib.mk: the compiler, its flags and
# the build steps as target types. Each board spells out its own targets in
# its rules.mk.
# The compiler is arm-none-eabi-gcc (flake.nix dev shell: gcc-arm-embedded).

YC_CROSS ?= arm-none-eabi-
# -fsigned-char: pad.h's s8 is a plain char, unsigned on ARM by default.
YC_CFLAGS := -mcpu=cortex-m0 -mthumb -fsigned-char -Os -g -std=gnu11 -Wall -Wextra -ffreestanding \
	-ffunction-sections -fdata-sections -fno-common
YC_LDSCRIPT := src/platform/yc3121/app.ld
YC_LDFLAGS := -nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-T,$(YC_LDSCRIPT)

# The toolchain: NAME_toolchain := YC3121_. The link is the compiler driver
# with the compile flags; a board writes its link flags in full (app.ld reads
# --defsym TEXT_AT / TEXT_END, which must come before the script):
#   NAME_ldflags := $(YC_CFLAGS) [-Wl,--defsym=TEXT_AT=...] $(YC_LDFLAGS) -Wl,-Map,DIR/app.map
YC3121_CC := $(YC_CROSS)gcc
YC3121_CFLAGS := $(YC_CFLAGS)
YC3121_LDLIBS := -lc -lgcc

# Our platform's headers, after a board's flags: SRC_INCLUDES without the
# Jieli platform, then this one (a board adds -iquote BOARD after).
YC3121_SRC_INCLUDES := $(subst -iquote src/platform/jieli,,$(SRC_INCLUDES)) -iquote src/platform/yc3121

# The ELF (first _deps) as the flash image, and its size.
define lib-type-yc3121-bin
$(YC_CROSS)objcopy -O binary $(firstword $(lib_deps)) $@ &&
$(YC_CROSS)size $(firstword $(lib_deps))
endef
