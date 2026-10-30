# Built for and run on this machine: the image tool and the unit tests.

$(O)/host/jlnewfw: tools/jlnewfw.c $(O)/host/libjollymaam.a
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

# Every source with a UNITTEST block (#ifdef or #ifndef) and its own main.
TESTS := $(shell grep -rlE --include='*.c' 'if(n)?def UNITTEST' src | xargs grep -l '^int main')
TESTS := $(filter-out src/platform/telink/att_telink.c,$(TESTS))

# $(call host_tests,NAME): NAME-test builds the unit tests with NAME's -D flags
# into $(O)/host/test-NAME/ and runs them.
define host_tests
.PHONY: $(1)-test
$(1)-test: $(TESTS:src/%.c=$(O)/host/test-$(1)/%)
	$$(Q)for t in $$^; do $$$$t || exit 1; done

$(O)/host/test-$(1)/%: src/%.c $(wildcard src/*/*.h src/*/*/*.h src/*/*/*/*.h) $(O)/$(1)/defines
	@mkdir -p $$(@D)
	$$(Q)$(HOST_CC) $(HOST_CFLAGS) -DBOARD_PID=\"$($(1)_PID)\" $($(1)_DEFINES) $(EXTRA_DEFINES) -DUNITTEST $(SRC_INCLUDES) -iquote $($(1)_BOARD_DIR) $$< -o $$@ \
	  $$(if $$(findstring /btctrler/,$$@),-no-pie -Wl$$(comma)--unresolved-symbols=ignore-all)
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

# Portable libjollymaam core. Backends will be linked separately.
JOLLYMAAM_SOURCES := src/lib/jieli_image/ufw.c src/lib/jieli_image/key.c src/lib/jieli_image/crypto.c src/lib/jieli_image/entry.c
JOLLYMAAM_OBJECTS := $(JOLLYMAAM_SOURCES:src/lib/%.c=$(O)/host/libjollymaam/%.o)
.PHONY: libjollymaam
libjollymaam: $(O)/host/libjollymaam.a
$(O)/host/libjollymaam.a: $(JOLLYMAAM_OBJECTS)
	$(Q)ar rcs $@ $^
$(O)/host/libjollymaam/%.o: src/lib/%.c $(wildcard src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -c $< -o $@

.PHONY: libjollymaam-test
libjollymaam-test: $(O)/host/test-jollymaam-ufw $(O)/host/test-jollymaam-crypto $(O)/host/test-jollymaam-entry $(O)/host/test-jollymaam-key
	$(Q)$(O)/host/test-jollymaam-crypto
	$(Q)$(O)/host/test-jollymaam-entry
	$(Q)$(O)/host/test-jollymaam-key
	$(Q)$(O)/host/test-jollymaam-ufw
$(O)/host/test-jollymaam-crypto: src/lib/jieli_image/crypto.c src/lib/jieli_image/crypto.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< -o $@
$(O)/host/test-jollymaam-entry: src/lib/jieli_image/entry.c $(O)/host/libjollymaam/jieli_image/crypto.o $(wildcard src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST src/lib/jieli_image/entry.c $(O)/host/libjollymaam/jieli_image/crypto.o -o $@

$(O)/host/test-jollymaam-key: src/lib/jieli_image/key.c $(O)/host/libjollymaam/jieli_image/crypto.o $(wildcard src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam/jieli_image/crypto.o -o $@

$(O)/host/test-jollymaam-ufw: src/lib/jieli_image/ufw.c $(O)/host/libjollymaam/jieli_image/crypto.o $(wildcard src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam/jieli_image/crypto.o -o $@
