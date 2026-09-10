.DEFAULT_GOAL := firmware

PROJECT_ROOT := $(CURDIR)
SDK ?= /work/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR ?= /work/jieli-linux-toolchains-20250324.1/pi32v2/bin

# Local, versioned BR23 ABI/library configuration. The SDK contributes no
# Makefile targets and is never written by this build.
include mk/br23-sdk.mk

C2_BUILD := $(PROJECT_ROOT)/build
C2_OBJ_DIR := $(C2_BUILD)/obj
C2_ELF := $(C2_BUILD)/sdk.elf
C2_LD_SCRIPT := $(C2_BUILD)/sdk.ld
C2_USED_LIST := $(C2_BUILD)/sdk_used_list.used
C2_MAP := $(C2_BUILD)/sdk.map

C2_PROJECT_SRCS := \
	src/control.c \
	src/main.c \
	src/rom.c

# Unmodified SDK platform sources needed by this application. Application and
# board policy stay in this repository; the SDK is a read-only dependency.
C2_SDK_SRCS := \
	apps/common/device/key/iokey.c \
	apps/hid/modules/misc.c \
	cpu/br23/adc_api.c \
	cpu/br23/charge.c \
	cpu/br23/pwm_led.c

C2_PROJECT_OBJS := $(addprefix $(C2_OBJ_DIR)/project/,$(C2_PROJECT_SRCS:.c=.o))
C2_SDK_OBJS := $(addprefix $(C2_OBJ_DIR)/sdk/,$(C2_SDK_SRCS:.c=.o))
C2_SETUP_OBJ := $(C2_OBJ_DIR)/sdk/cpu/br23/setup.o
C2_OBJS := $(C2_PROJECT_OBJS) $(C2_SDK_OBJS) $(C2_SETUP_OBJ)

C2_LFLAGS := $(SDK_LFLAGS) --plugin-opt=-used-symbol-file=$(C2_USED_LIST) \
	-T$(C2_LD_SCRIPT) -M=$(C2_MAP)

.PHONY: firmware clean

firmware: $(C2_ELF)

$(C2_ELF): $(C2_OBJS) $(C2_LD_SCRIPT) $(C2_USED_LIST)
	@mkdir -p $(@D)
	$(LD) -o $@ $(C2_OBJS) $(C2_LFLAGS) --start-group \
		$(SDK_ARCHIVES) --end-group -L$(SYS_LIB_DIR) $(SDK_RUNTIME_LIBS)

$(C2_LD_SCRIPT): $(SDK)/cpu/br23/sdk_ld.c src/app_config.h src/board.h
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -D__LD__ -E -P $< -o $@

$(C2_USED_LIST): $(SDK)/cpu/br23/sdk_used_list.c src/app_config.h src/board.h
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -D__LD__ -E -P $< -o $@

$(C2_OBJ_DIR)/project/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -c $< -o $@

$(C2_OBJ_DIR)/sdk/%.o: $(SDK)/%.c
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -c $< -o $@

# Preserve the SDK startup implementation but give our wrapper ownership of
# the public setup_arch symbol.
$(C2_SETUP_OBJ): $(SDK)/cpu/br23/setup.c
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) \
		-Dsetup_arch=sdk_setup_arch -c $< -o $@

clean:
	./scripts/clean.sh
