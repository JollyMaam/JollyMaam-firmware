# Built for and run on this machine: the image tool and the unit tests.

$(O)/host/jlnewfw: tools/jlnewfw.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# Every source with a UNITTEST block.
TESTS := $(shell grep -l 'ifdef UNITTEST' src/*.c src/btctrler/*.c)

# $(call host_tests,NAME): NAME-test builds the unit tests with NAME's -D flags
# into $(O)/host/test-NAME/ and runs them.
define host_tests
.PHONY: $(1)-test
$(1)-test: $(TESTS:src/%.c=$(O)/host/test-$(1)/%)
	$$(Q)for t in $$^; do $$$$t || exit 1; done

$(O)/host/test-$(1)/%: src/%.c src/pad.h src/dongle.h src/test.h $(wildcard src/btctrler/*.h) $(O)/$(1)/defines
	@mkdir -p $$(@D)
	$$(Q)$(HOST_CC) $(HOST_CFLAGS) $($(1)_DEFINES) $(EXTRA_DEFINES) -DUNITTEST -Isrc -iquote src $$< -o $$@ \
	  $$(if $$(findstring /btctrler/,$$@),-no-pie -Wl$$(comma)--unresolved-symbols=ignore-all)
endef
