# The Telink B80 (TLSR8208): toolchain, SDK, flags and the SDK sources the
# app uses.  The compiler ABI is the SDK's own (its Eclipse project's flags);
# we never include a vendor Makefile and build the SDK in place, unmodified.
# make/telink-lib.mk builds boards from this data.

TELINK_SDK := external/telink-tc_ble_simple_sdk/b80_ble_sdk
TELINK_TOOLS := external/toolchains/tc32_gcc_v2.0/bin
TELINK_CC := $(TELINK_TOOLS)/tc32-elf-gcc
TELINK_LD := $(TELINK_TOOLS)/tc32-elf-ld
TELINK_OBJCOPY := $(TELINK_TOOLS)/tc32-elf-objcopy
TELINK_LIBGCC := $(shell $(TELINK_CC) -print-libgcc-file-name 2>/dev/null)

# The SDK's sources build with -fpack-struct; ours do not, and see the SDK's
# headers only through src/platform/telink/sdk.h, which packs them the same
# way.  Everything else in the ABI (short enums, unsigned char) is shared.
TELINK_ABI := -std=gnu99 -fshort-enums -fshort-wchar -fms-extensions -ffunction-sections -fdata-sections -fcommon

# -O2, as the SDK builds, never -Os: at -Os this GCC (4.5 tc32) writes a dense
# switch's jump table as 16-bit offsets after the jump but loads 32-bit
# addresses from it, so the switch jumps anywhere.  Our char is signed
# (pad.h's s8); the SDK's stays unsigned, as it builds.
TELINK_SDK_CFLAGS := $(TELINK_ABI) -O2 -fpack-struct -finline-small-functions
TELINK_CFLAGS := $(TELINK_ABI) -O2 -fsigned-char -mstructure-size-boundary=8 -g -Wall -Wno-unused-function -Wno-missing-field-initializers -Wframe-larger-than=256

TELINK_DEFINES :=
TELINK_DEFINES += -DMCU_CORE_B80=1
TELINK_DEFINES += -DMCU_STARTUP_FLASH
TELINK_DEFINES += -DMCU_STARTUP_B80=1
# No RAM survives a reset that the app could use for the log.
TELINK_DEFINES += -DRETAINED=

TELINK_INCLUDES :=
TELINK_INCLUDES += -I$(TELINK_SDK)
TELINK_INCLUDES += -I$(TELINK_SDK)/drivers
TELINK_INCLUDES += -I$(TELINK_SDK)/common
TELINK_INCLUDES += -I$(TELINK_SDK)/vendor/common

# Our SDK configuration (what vendor/*/app_config.h is to the SDK's samples),
# read before anything else by every SDK and app file.
TELINK_APP_CONFIG := src/platform/telink/sdkconfig/app_config.h

# The SDK's strncpy (common/string.c) copies n bytes whatever the string's
# length; libc_telink.c has the standard one.
TELINK_LDFLAGS := --wrap=strncpy

TELINK_ARCHIVES :=
TELINK_ARCHIVES += $(TELINK_SDK)/proj_lib/liblt_8208.a

# The SDK sources the app uses.
TELINK_SDK_USED :=
TELINK_SDK_USED += boot/cstartup_flash.S
TELINK_SDK_USED += common/sdk_version.c
TELINK_SDK_USED += common/string.c
TELINK_SDK_USED += common/utility.c
TELINK_SDK_USED += drivers/adc.c
TELINK_SDK_USED += drivers/analog.c
TELINK_SDK_USED += drivers/bsp.c
TELINK_SDK_USED += drivers/clock.c
TELINK_SDK_USED += drivers/driver_ext/ext_calibration.c
TELINK_SDK_USED += drivers/driver_ext/ext_misc.c
TELINK_SDK_USED += drivers/driver_ext/rf_pa.c
TELINK_SDK_USED += drivers/driver_ext/rf_private_pa.c
TELINK_SDK_USED += drivers/flash.c
TELINK_SDK_USED += drivers/flash/flash_common.c
TELINK_SDK_USED += drivers/flash/flash_mid114485.c
TELINK_SDK_USED += drivers/flash/flash_mid1160c8.c
TELINK_SDK_USED += drivers/flash/flash_mid1164c8.c
TELINK_SDK_USED += drivers/flash/flash_mid13325e.c
TELINK_SDK_USED += drivers/flash/flash_mid136085.c
TELINK_SDK_USED += drivers/flash/flash_mid1360c8.c
TELINK_SDK_USED += drivers/gpio.c
TELINK_SDK_USED += drivers/timer.c
TELINK_SDK_USED += drivers/usbhw.c
TELINK_SDK_USED += vendor/common/ble_flash.c
TELINK_SDK_USED += vendor/common/flash_prot.c

TELINK_B80_CC := $(TELINK_CC)
TELINK_B80_LINK := $(TELINK_LD)
TELINK_B80_CPPFLAGS := $(TELINK_DEFINES)
TELINK_B80_LDFLAGS := --gc-sections $(TELINK_LDFLAGS)
TELINK_B80_LDLIBS := $(TELINK_ARCHIVES) $(TELINK_LIBGCC)

TELINK_SDK_SOURCES := $(addprefix $(TELINK_SDK)/,$(TELINK_SDK_USED))
TELINK_SDK_C_SOURCES := $(filter %.c,$(TELINK_SDK_SOURCES))
TELINK_SDK_S_SOURCES := $(filter %.S,$(TELINK_SDK_SOURCES))
TELINK_BTSTACK_SOURCES := $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c
TELINK_BTSTACK_INCLUDES := -I src/platform/telink $(BTSTACK_INCLUDES)
TELINK_SRC_INCLUDES := $(subst -iquote src/platform/jieli,,$(SRC_INCLUDES)) -iquote src/platform/telink

# Images link with src/platform/telink/b80.ld (its comment has the memory
# map) at their APP_START: TELINK_B80_LD plus --defsym=APP_START=....
TELINK_B80_LD := src/platform/telink/b80.ld

# The ELF (first _deps) as the flash image.
lib-type-telink-bin = $(TELINK_OBJCOPY) -O binary $(firstword $(lib_deps)) $@

# The image (app.bin, first _deps) with its header: the stock stamp
# (telink_stamp) in its reserved bytes, the size and the CRC the boot loader
# checks; it may fill telink_app_start up to telink_app_end.
lib-type-telink-fw = $(JOLLYMAAM_CLI) pack telink --stamp $(telink_stamp) --max $$(( $(telink_app_end) - $(telink_app_start) )) -o $@ $(firstword $(lib_deps))
