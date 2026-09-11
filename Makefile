SDK := /work/github.com-Jieli-Tech/fw-AC63_BT_SDK
TOOL_DIR := /work/jieli-linux-toolchains-20250324.1/pi32v2/bin

include sdk-br23.mk

CFLAGS := $(SDK_CFLAGS)
DEFINES := $(SDK_DEFINES)
INCLUDES := -Isrc -Isrc/config $(SDK_INCLUDES)
LFLAGS := $(SDK_LFLAGS)
SDK_SOURCES := $(SDK_SOURCES_FULL)
SDK_ARCHIVES := $(SDK_ARCHIVES_FULL)
SDK_OBJECTS := $(subst $(SDK),out/sdk,$(SDK_SOURCES:.c=.c.o))
SDK_PREBUILD := out/sdk.ld out/sdk_used_list.used out/isd_config.ini
DEPFILES := $(OBJECTS:.o=.c.d) $(SDK_OBJECTS:.o=.c.d)

SOURCES := $(wildcard src/*.c src/config/*.c)
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

app: out/app.elf

out/app.elf: out/sdk.ld $(OBJECTS)
	@mkdir -p $(@D)
	@echo [LD] $@
	$(QUIET)$(LD) -o $@ $(OBJECTS) -Tout/sdk.ld -M=out/app.map $(SDK_LFLAGS) --start-group $(SDK_ARCHIVES) --end-group $(SDK_RUNTIME_LIBS)

out/sdk.ld: $(SDK)/cpu/br23/sdk_ld.c
	$(compile_pre_build)

out/sdk_used_list.used: $(SDK)/cpu/br23/sdk_used_list.c
	$(compile_pre_build)

out/download.sh: $(SDK)/cpu/br23/tools/download.c
	$(compile_pre_build)

out/isd_config.ini: $(SDK)/cpu/br23/tools/isd_config_rule.c
	$(compile_pre_build)

out/src/%.c.o: src/%.c
	$(compile_c)

out/sdk/%.c.o: $(SDK)/%.c
	$(compile_c)

clean:
	rm -rf out

-include $(DEPFILES)
