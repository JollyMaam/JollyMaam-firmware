# Built for and run on this machine: the image tool and the unit tests.

# Every source with a UNITTEST block (#ifdef or #ifndef) and its own main.
TESTS := $(shell grep -rlE --include='*.c' 'if(n)?def UNITTEST' src | xargs grep -l '^int main')
TESTS := $(filter-out src/platform/telink/att_telink.c src/lib/%,$(TESTS))

# $(call host_tests,NAME): NAME-test builds the unit tests with NAME's -D flags
# into $(O)/host/test-NAME/ and runs them.
# The settings store the settings unit test checks its records against:
# the Telink store, or the Jieli VM (size and the SDK's reserve).
settings_store_defines = $(if $(filter src/platform/telink/store_telink.c,$($(1)_SOURCES)),-DSETTINGS_STORE_TELINK) $(if $(filter src/platform/jieli/platform_jieli.c,$($(1)_SOURCES)),-DSETTINGS_STORE_JIELI_BYTES=$(JIELI_VM_KB)*1024 -DSETTINGS_STORE_JIELI_RESERVE=$(JIELI_VM_SETTINGS_RESERVE))

define host_tests
.PHONY: $(1)-test
$(1)-test: $(TESTS:src/%.c=$(O)/host/test-$(1)/%)
	$$(Q)for t in $$^; do $$$$t || exit 1; done

$(O)/host/test-$(1)/%: src/%.c $(wildcard src/*/*.h src/*/*/*.h src/*/*/*/*.h) $(O)/$(1)/defines
	@mkdir -p $$(@D)
	$$(Q)$(HOST_CC) $(HOST_CFLAGS) -DBOARD_PID=\"$($(1)_PID)\" $($(1)_DEFINES) $(EXTRA_DEFINES) -DUNITTEST $(call settings_store_defines,$(1)) $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) $$< $$(if $$(filter src/core/usb.c src/updater/updater.c,$$<),$(O)/host/libjollymaam.a) -o $$@ \
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

