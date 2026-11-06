# libjollymaam (no OS calls), built for this machine: linked into the
# jollymaam command, the host tests and the SDK.
TARGETS += host/libjollymaam.a
host/libjollymaam.a_type := archive
host/libjollymaam.a_toolchain := HOST_
host/libjollymaam.a_sources := $(wildcard $(self)/*.c $(self)/jieli_image/*.c)
host/libjollymaam.a_cflags := -std=c99

# Its unit tests (every module with a UNITTEST block) and the simulations
# (tools/jollymaam/test_jollymaam_*.c), each linked with the library
# (make/host-tests.mk).
JOLLYMAAM_UNIT := $(shell grep -lE '^#ifdef UNITTEST' $(host/libjollymaam.a_sources))
JOLLYMAAM_SIMS := $(addprefix tools/jollymaam/test_jollymaam_,upgrade.c jieli.c device.c)
TARGETS += host/libjollymaam-test
host/libjollymaam-test_type := host-tests
host/libjollymaam-test_toolchain := HOST_
host/libjollymaam-test_sources := $(JOLLYMAAM_UNIT) $(JOLLYMAAM_SIMS)
host/libjollymaam-test_cflags := -std=c99
host/libjollymaam-test_compile.c = $(host-test-compile)
$(call lib-objs,host/libjollymaam-test,$(JOLLYMAAM_UNIT)): obj_cppflags := -DUNITTEST
$(call lib-objs,host/libjollymaam-test,$(JOLLYMAAM_UNIT) $(JOLLYMAAM_SIMS)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/libjollymaam-test,$(JOLLYMAAM_UNIT) $(JOLLYMAAM_SIMS)): $(O)/host/libjollymaam.a

.PHONY: libjollymaam libjollymaam-test
libjollymaam: $(O)/host/libjollymaam.a
libjollymaam-test: $(O)/host/libjollymaam-test
test: libjollymaam-test
