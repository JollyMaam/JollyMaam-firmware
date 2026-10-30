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
JOLLYMAAM_TESTS := crypto entry key ufw
JOLLYMAAM_TEST_BINS := $(JOLLYMAAM_TESTS:%=$(O)/host/test-jollymaam-%)
libjollymaam-test: $(JOLLYMAAM_TEST_BINS)
	$(Q)for t in $^; do $$t || exit 1; done
$(O)/host/test-jollymaam-%: src/lib/jieli_image/%.c $(O)/host/libjollymaam.a $(wildcard src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
test: libjollymaam-test
