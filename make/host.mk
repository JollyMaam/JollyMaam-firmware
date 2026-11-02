# Built for and run on this machine: the image tool and the unit tests.

# Every source with a UNITTEST block (#ifdef or #ifndef) and its own main.
TESTS := $(shell grep -rlE --include='*.c' 'if(n)?def UNITTEST' src | xargs grep -l '^int main')
TESTS := $(filter-out src/platform/telink/att_telink.c src/lib/%,$(TESTS))

# $(call host_tests,NAME): NAME-test builds the unit tests with NAME's -D flags
# into $(O)/host/test-NAME/ and runs them.
define host_tests
.PHONY: $(1)-test
$(1)-test: $(TESTS:src/%.c=$(O)/host/test-$(1)/%)
	$$(Q)for t in $$^; do $$$$t || exit 1; done

$(O)/host/test-$(1)/%: src/%.c $(wildcard src/*/*.h src/*/*/*.h src/*/*/*/*.h) $(O)/$(1)/defines
	@mkdir -p $$(@D)
	$$(Q)$(HOST_CC) $(HOST_CFLAGS) -DBOARD_PID=\"$($(1)_PID)\" $($(1)_DEFINES) $(EXTRA_DEFINES) -DUNITTEST $(if $(filter src/core/settings.c,$($(1)_SOURCES)),-DSETTINGS_SCHEMA -iquote $(O)/$(1)) $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) $$< $$(if $$(filter src/core/usb.c src/updater/updater.c,$$<),$(O)/host/libjollymaam.a) -o $$@ \
	  $$(if $$(findstring /btctrler/,$$@),-no-pie -Wl$$(comma)--unresolved-symbols=ignore-all)
# Protocol tests decode actual firmware replies with the portable host library.
$(O)/host/test-$(1)/core/usb $(O)/host/test-$(1)/updater/updater: $(O)/host/libjollymaam.a
.PHONY: $(1)-protocol-test
$(1)-protocol-test: $(O)/host/test-$(1)/core/usb $(O)/host/test-$(1)/updater/updater
	$$(Q)for t in $$^; do $$$$t || exit 1; done
endef

# The hub daemon (tools/padhub.c): the dongle's profiles, built for this PC.
PADHUB_SOURCES := tools/padhub.c src/profiles/profile_link.c src/profiles/profile_switch.c \
	src/profiles/profile_xbox.c src/profiles/profile_playstation.c src/profiles/profile_hid.c \
	src/core/motion.c src/core/utils.c

.PHONY: padhub
padhub: $(O)/host/padhub
$(O)/host/padhub: $(PADHUB_SOURCES) $(wildcard src/*/*.h src/boards/gamesir-cyclone-2/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DBOARD_DONGLE $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 $(PADHUB_SOURCES) -o $@

.PHONY: padhub-test
padhub-test: $(O)/host/test-padhub
	$(Q)$<
test: padhub-test
$(O)/host/test-padhub: tools/test_padhub.c $(PADHUB_SOURCES) $(wildcard src/*/*.h src/boards/gamesir-cyclone-2/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DBOARD_DONGLE $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2 tools/test_padhub.c $(filter-out tools/padhub.c,$(PADHUB_SOURCES)) -o $@

include make/libjollymaam.mk

# A host-compiled exporter uses the same tables and board defaults as firmware.
# Called by each board template only for targets which include settings.c.
define settings_schema
$(O)/host/settings-schema-$(1): tools/jollymaam/settings_schema.c src/core/settings.c src/lib/sha256.c src/lib/sha256.h src/platform/telink/store_telink.h $(patsubst %/board_pad.c,%/settings_defaults.inc,$(filter %/board_pad.c,$($(1)_SOURCES))) $(wildcard src/core/*.h src/lib/proto_jollymaam*.h) $(O)/$(1)/defines $(MAKE_FILES) $(O)/host/.jollymaam-config
	@mkdir -p $$(@D)
	$$(Q)$(HOST_CC) $(HOST_CFLAGS) $($(1)_DEFINES) $(EXTRA_DEFINES) $(if $(filter src/platform/telink/store_telink.c,$($(1)_SOURCES)),-DSETTINGS_STORE_TELINK) $(if $(filter src/platform/jieli/platform_jieli.c,$($(1)_SOURCES)),-DSETTINGS_STORE_JIELI_BYTES=$(JIELI_VM_KB)*1024 -DSETTINGS_STORE_JIELI_RESERVE=$(JIELI_VM_SETTINGS_RESERVE)) $(SRC_INCLUDES) -iquote $(dir $(filter %/board_pad.c,$($(1)_SOURCES))) -ffunction-sections -fdata-sections tools/jollymaam/settings_schema.c src/lib/sha256.c -Wl,--gc-sections -o $$@

$(O)/$(1)/settings_schema.h $(O)/$(1)/settings_schema.bin &: $(O)/host/settings-schema-$(1)
	$$(Q)$$< $(O)/$(1)/settings_schema.bin.tmp $(O)/$(1)/settings_schema.h.tmp
	$$(Q)cmp -s $(O)/$(1)/settings_schema.bin.tmp $(O)/$(1)/settings_schema.bin || mv $(O)/$(1)/settings_schema.bin.tmp $(O)/$(1)/settings_schema.bin
	$$(Q)cmp -s $(O)/$(1)/settings_schema.h.tmp $(O)/$(1)/settings_schema.h || mv $(O)/$(1)/settings_schema.h.tmp $(O)/$(1)/settings_schema.h
	$$(Q)rm -f $(O)/$(1)/settings_schema.bin.tmp $(O)/$(1)/settings_schema.h.tmp

$(O)/$(1)/src/core/usb.o $(O)/host/test-$(1)/core/usb: $(O)/$(1)/settings_schema.h
endef

.PHONY: settings-schema-test
settings-schema-test:
	$(Q)HOST_CC='$(HOST_CC)' python3 -m unittest tools/jollymaam/test_settings_schema.py
test: settings-schema-test
