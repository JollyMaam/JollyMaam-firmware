SDK := /work/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /work/jieli-linux-toolchains-20250324.1/pi32v2/bin
PYTHON := /work/bin/python3

include sdk-br23.mk

HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES)
INCLUDES := -Isrc -Iconfig $(SDK_INCLUDES)
LFLAGS := $(SDK_LFLAGS)
SDK_SOURCES := $(SDK_SOURCES_FULL)
SDK_ARCHIVES := $(SDK_ARCHIVES_FULL)
SDK_OBJECTS := $(subst $(SDK),out/sdk,$(SDK_SOURCES:.c=.c.o))
DEPFILES := $(OBJECTS:.o=.c.d) $(SDK_OBJECTS:.o=.c.d)

SOURCES := $(wildcard src/*.c config/*.c)
OBJECTS := $(addprefix out/,$(SOURCES:.c=.c.o)) $(SDK_OBJECTS)

V?=0
ifneq ($(V),0)
QUIET=
else
QUIET=@
endif

define compile_c
	@mkdir -p $(@D)
	@echo [CC] $^
	$(QUIET)$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MP -MF $@.d -c $< -o $@
endef

define compile_pre_build
	@mkdir -p $(@D)
	@echo [PRECC] $@
	$(QUIET) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $< -o $@
endef

out/firmware.img: out/isd_config.ini out/app.bin out/jlnewfw
	@echo [FW] $@
	$(QUIET)out/jlnewfw pack -c out/isd_config.ini -u $(SDK_UBOOT) -o $@ out/app.bin $(SDK_CONFIG_TOOL)

out/jlnewfw: tools/jlnewfw.c
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

out/jlcfg: tools/jlcfg.c
	@echo [EXE] $@
	$(QUIET)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# does the same as download.sh
out/app.bin: out/app.elf
	@echo [BIN] $@
	@mkdir -p out/sections
	@for i in text data; do $(OBJCOPY) -O binary -j .$$i $< out/sections/$$i.bin; done
	@for i in aec wav ape flac m4a amr dts fm mp3 wma; do $(OBJCOPY) -O binary -j .overlay_$$i $< out/sections/$$i.bin; done
	@cd out/sections && cat text.bin data.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin fm.bin mp3.bin wma.bin > ../app.bin

out/app.elf: out/sdk.ld $(OBJECTS)
	@mkdir -p $(@D)
	@echo [LD] $@
	$(QUIET)$(LD) -o $@ $(OBJECTS) -Tout/sdk.ld -M=out/app.map $(SDK_LFLAGS) --start-group $(SDK_ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

out/sdk.ld: $(SDK)/cpu/br23/sdk_ld.c config/app_config.h
	$(compile_pre_build)

out/sdk_used_list.used: $(SDK)/cpu/br23/sdk_used_list.c config/app_config.h
	$(compile_pre_build)

out/download.sh: $(SDK)/cpu/br23/tools/download.c config/app_config.h
	$(compile_pre_build)

out/isd_config.ini: $(SDK)/cpu/br23/tools/isd_config_rule.c config/app_config.h
	$(compile_pre_build)
	$(QUIET)sed -i -e /^$$/d "$@"

out/src/%.c.o: src/%.c
	$(compile_c)

out/config/%.c.o: config/%.c
	$(compile_c)

out/sdk/%.c.o: $(SDK)/%.c
	$(compile_c)

clean:
	rm -rf out

-include $(DEPFILES)
