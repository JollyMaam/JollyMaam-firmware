# Built for and run on this machine: the image tool and the unit tests.

$(O)/host/jlnewfw: tools/jlnewfw.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

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
