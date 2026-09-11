.DEFAULT_GOAL := all

PROJECT_ROOT := $(CURDIR)
SDK ?= $(or $(CYCLONE2_SDK),/work/github.com-Jieli-Tech/fw-AC63_BT_SDK)
TOOL_DIR ?= $(or $(CYCLONE2_TOOLCHAIN),/work/jieli-linux-toolchains-20250324.1/pi32v2/bin)
STOCK_UNPACKED ?= $(or $(CYCLONE2_STOCK_UNPACKED),/work/firmware/fw_3.52.bin_unpack)
MISC_TOOLS ?= /work/github.com-kagaimiq/jl-misctools
PYTHON ?= /work/bin/python3

EXPECTED_SDK_REVISION := fdc018de81629e21e594d677cf05f87d288be0f0
ACTUAL_SDK_REVISION := $(shell git -C "$(SDK)" rev-parse HEAD 2>/dev/null)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(ACTUAL_SDK_REVISION),$(EXPECTED_SDK_REVISION))
$(error Unsupported SDK revision '$(ACTUAL_SDK_REVISION)' (expected $(EXPECTED_SDK_REVISION)))
endif
endif

# Freeze __DATE__/__TIME__ emitted by vendor sources so compiler output is
# repeatable. This is the timestamp of the pinned SDK commit.
export SOURCE_DATE_EPOCH := 1742302100

V?=0
ifneq ($(V),0)
QUIET=
else
QUIET=@
endif

# Local, versioned BR23 ABI/library configuration. The SDK contributes no
# Makefile targets and is never written by this build.
include mk/br23-sdk.mk

C2_BUILD := $(PROJECT_ROOT)/build
C2_OBJ_DIR := $(C2_BUILD)/obj
C2_ELF := $(C2_BUILD)/sdk.elf
C2_LISTING := $(C2_BUILD)/app-disasm.lst
C2_LD_SCRIPT := $(C2_BUILD)/sdk.ld
C2_USED_LIST := $(C2_BUILD)/sdk_used_list.used
C2_MAP := $(C2_BUILD)/sdk.map
C2_LINK_CHECK := $(C2_BUILD)/.linked-image-verified
C2_SECTIONS_DIR := $(C2_BUILD)/sections
C2_RAW_APP := $(C2_BUILD)/sdk-app.bin
C2_VERIFY_DIR := $(C2_BUILD)/verify
C2_VERIFY_STAMP := $(C2_VERIFY_DIR)/.verified

C2_DIST := $(PROJECT_ROOT)/dist
C2_PADDED_APP := $(C2_DIST)/app.bin
C2_CANDIDATE := $(C2_DIST)/cyclone2-sdk-fw.bin
C2_DIST_ELF := $(C2_DIST)/sdk.elf
C2_DIST_LISTING := $(C2_DIST)/app-disasm.lst
C2_LEGACY_LISTING := $(C2_DIST)/sdk.lst
C2_SECTION_REPORT := $(C2_DIST)/sdk-sections.txt
C2_HASHES := $(C2_DIST)/SHA256SUMS

C2_PACKER := $(MISC_TOOLS)/firmware/fwpack_newfw.py
C2_UNPACKER := $(MISC_TOOLS)/firmware/fwunpack_newfw.py
C2_IMAGE_BUILDER := tools/build_app_image.py
C2_STOCK_APP := $(STOCK_UNPACKED)/files/app.bin
C2_STOCK_CFG_TOOL := $(STOCK_UNPACKED)/files/cfg_tool.bin
C2_STOCK_DEPS := $(wildcard $(STOCK_UNPACKED)/*) \
	$(wildcard $(STOCK_UNPACKED)/files/*) \
	$(wildcard $(STOCK_UNPACKED)/top/*)

C2_SECTIONS := text data overlay_aec overlay_wav overlay_ape overlay_flac \
	overlay_m4a overlay_amr overlay_dts overlay_fm overlay_mp3 overlay_wma
C2_SECTION_FILES := $(addprefix $(C2_SECTIONS_DIR)/,$(addsuffix .bin,$(C2_SECTIONS)))

C2_PROJECT_SRCS := \
	src/config.c \
	src/control.c \
	src/main.c \
	src/rom.c

# Add individual, unmodified SDK implementation files here only when the
# firmware actually uses them. Configuration and board policy belong in src/.
C2_SDK_SRCS := \
	cpu/br23/charge.c \
	cpu/br23/led_spi.c

C2_PROJECT_OBJS := $(addprefix $(C2_OBJ_DIR)/project/,$(C2_PROJECT_SRCS:.c=.o))
C2_SDK_OBJS := $(addprefix $(C2_OBJ_DIR)/sdk/,$(C2_SDK_SRCS:.c=.o))
C2_SETUP_OBJ := $(C2_OBJ_DIR)/sdk/cpu/br23/setup.o
C2_OBJS := $(C2_PROJECT_OBJS) $(C2_SDK_OBJS) $(C2_SETUP_OBJ)
C2_DEPFILES := $(addsuffix .d,$(C2_OBJS))
C2_CONFIG_DEPS := Makefile mk/br23-sdk.mk src/app_config.h src/board.h

C2_LFLAGS := $(SDK_LFLAGS) --plugin-opt=-used-symbol-file=$(C2_USED_LIST) \
	--allow-multiple-definition -T$(C2_LD_SCRIPT) -M=$(C2_MAP)

.PHONY: all firmware elf analysis verify clean

all: firmware

# Compatibility target: unlike the old shell wrapper, this is the complete
# build, pack, analysis and round-trip verification pipeline.
firmware: $(C2_VERIFY_STAMP) $(C2_HASHES) $(C2_DIST_LISTING) \
	$(C2_LEGACY_LISTING) $(C2_SECTION_REPORT)
	@cat "$(C2_HASHES)"
	@echo "verified flash candidate: $(C2_CANDIDATE)"

elf: $(C2_ELF)

analysis: $(C2_LISTING) $(C2_DIST_LISTING) $(C2_LEGACY_LISTING) \
	$(C2_SECTION_REPORT)

verify: $(C2_VERIFY_STAMP)

# Compile-time policy affects every SDK translation unit, the generated linker
# script and the linker keep-list. Do not reuse objects built with stale flags.
$(C2_OBJS) $(C2_LD_SCRIPT) $(C2_USED_LIST): $(C2_CONFIG_DEPS)

$(C2_ELF): $(C2_OBJS) $(C2_LD_SCRIPT) $(C2_USED_LIST) Makefile
	@echo "ELF: $@"
	@mkdir -p $(@D)
	$(QUIET)$(LD) -o $@ $(C2_OBJS) $(C2_LFLAGS) --start-group \
		$(SDK_ARCHIVES) --end-group -L$(SYS_LIB_DIR) $(SDK_RUNTIME_LIBS)

# A searchable view of the complete linked application. It contains the full
# local/global symbol table plus labels and available source locations
# interleaved with the address-ordered disassembly.
$(C2_LISTING): $(C2_ELF) Makefile
	@echo "DISASM: $@"
	$(QUIET)$(OBJDUMP) -t -D -source -line-numbers -print-dbg \
		-address-mask=0x1ffffff $< > $@

# Validate assumptions that must hold before an ELF is turned into a flash
# image. Keeping this as a stamped target avoids repacking unchanged output.
$(C2_LINK_CHECK): $(C2_ELF)
	@start_address=`$(NM) -n "$<" | awk '$$3 == "_start" { print $$1; exit }'`; \
	if [ "$$start_address" != "01e00120" ]; then \
		echo "unexpected SDK _start address: $$start_address" >&2; exit 1; \
	fi
	@if $(NM) -n "$<" | awk '$$3 == "__initcall_sdfile_init" { found = 1 } END { exit !found }'; then \
		echo "error: disabled sdfile_init was linked into the firmware" >&2; exit 1; \
	fi
	@touch "$@"

# Extract loadable pieces and assemble them in the overlay order encoded by
# sdk.ld/download.c. The helper also extends the load image to the fixed stock
# app.bin slot size using the untouched tail of the known-good image.
$(C2_RAW_APP) $(C2_PADDED_APP) &: $(C2_ELF) $(C2_LINK_CHECK) \
		$(C2_IMAGE_BUILDER) $(C2_STOCK_APP)
	@echo "APP: $(C2_PADDED_APP)"
	@mkdir -p "$(C2_SECTIONS_DIR)" "$(C2_DIST)"
	@set -e; for section in $(C2_SECTIONS); do \
		$(OBJCOPY) -O binary -j ".$$section" "$<" \
			"$(C2_SECTIONS_DIR)/$$section.bin"; \
	done
	$(QUIET)$(PYTHON) "$(C2_IMAGE_BUILDER)" \
		--raw "$(C2_RAW_APP)" --stock "$(C2_STOCK_APP)" \
		--output "$(C2_PADDED_APP)" $(C2_SECTION_FILES)

$(C2_CANDIDATE): $(C2_PADDED_APP) $(C2_PACKER) $(C2_STOCK_DEPS)
	@echo "PACK: $@"
	@mkdir -p "$(@D)"
	$(QUIET)$(PYTHON) "$(C2_PACKER)" --experimental \
		--replace "app.bin=$(C2_PADDED_APP)" "$(STOCK_UNPACKED)" "$@"

# Unpack every new candidate and compare both the replacement and a preserved
# file. A candidate is not a successful firmware target until this completes.
$(C2_VERIFY_STAMP): $(C2_CANDIDATE) $(C2_PADDED_APP) $(C2_UNPACKER) \
		$(C2_STOCK_CFG_TOOL)
	@echo "VERIFY: $(C2_CANDIDATE)"
	@if [ -d "$(C2_VERIFY_DIR)" ]; then \
		find "$(C2_VERIFY_DIR)" -depth -mindepth 1 -delete; \
		rmdir "$(C2_VERIFY_DIR)"; \
	fi
	$(QUIET)$(PYTHON) "$(C2_UNPACKER)" --dirname "$(C2_VERIFY_DIR)" \
		"$(C2_CANDIDATE)"
	$(QUIET)cmp "$(C2_PADDED_APP)" "$(C2_VERIFY_DIR)/files/app.bin"
	$(QUIET)cmp "$(C2_STOCK_CFG_TOOL)" \
		"$(C2_VERIFY_DIR)/files/cfg_tool.bin"
	@touch "$@"

$(C2_DIST_ELF): $(C2_ELF)
	@mkdir -p "$(@D)"
	$(QUIET)cp "$<" "$@"

$(C2_DIST_LISTING): $(C2_LISTING)
	@mkdir -p "$(@D)"
	$(QUIET)cp "$<" "$@"

# Compatibility name retained for notes and tools written during bring-up.
$(C2_LEGACY_LISTING): $(C2_LISTING)
	@mkdir -p "$(@D)"
	$(QUIET)cp "$<" "$@"

$(C2_SECTION_REPORT): $(C2_ELF)
	@mkdir -p "$(@D)"
	$(QUIET)$(OBJDUMP) -h "$<" > "$@"

$(C2_HASHES): $(C2_CANDIDATE) $(C2_PADDED_APP) $(C2_DIST_ELF)
	@sha256sum "$(C2_CANDIDATE)" "$(C2_PADDED_APP)" \
		"$(C2_DIST_ELF)" > "$@"

$(C2_LD_SCRIPT): $(SDK)/cpu/br23/sdk_ld.c src/app_config.h src/board.h
	@echo "LD: $@"
	@mkdir -p $(@D)
	$(QUIET)$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -D__LD__ -E -P $< -o $@

$(C2_USED_LIST): $(SDK)/cpu/br23/sdk_used_list.c src/app_config.h src/board.h
	@echo "USED_LIST: $@"
	@mkdir -p $(@D)
	$(QUIET)$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) -D__LD__ -E -P $< -o $@

$(C2_OBJ_DIR)/project/%.o: %.c
	@echo "CC: $@"
	@mkdir -p $(@D)
	$(QUIET)$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) \
		-MMD -MP -MF $@.d -c $< -o $@

$(C2_OBJ_DIR)/sdk/%.o: $(SDK)/%.c
	@echo "CC: $@"
	@mkdir -p $(@D)
	$(QUIET)$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) \
		-MMD -MP -MF $@.d -c $< -o $@

# Preserve the SDK startup implementation but give our wrapper ownership of
# the public setup_arch symbol.
$(C2_SETUP_OBJ): $(SDK)/cpu/br23/setup.c
	@echo "SETUP: $@"
	@mkdir -p $(@D)
	$(QUIET)$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) $(SDK_INCLUDES) \
		-MMD -MP -MF $@.d -Dsetup_arch=sdk_setup_arch -c $< -o $@

-include $(C2_DEPFILES)

clean:
	rm -rf build dist
