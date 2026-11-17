# Unit tests, built for and run on this machine. A module with logic has an
# #ifdef UNITTEST block and its own main(); a board builds every such module
# with its own defines into one test target:
#
#   TARGETS += host/test-BOARD
#   host/test-BOARD_type := host-tests
#   host/test-BOARD_toolchain := HOST_
#   host/test-BOARD_sources := $(HOST_TESTS)
#   host/test-BOARD_cppflags := ...the board's -D flags... -DUNITTEST $(SRC_INCLUDES) -iquote BOARD_DIR
#   host/test-BOARD_compile.c = $(host-test-compile)
#   $(call lib-objs,host/test-BOARD,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
#   $(call lib-objs,host/test-BOARD,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
#   $(call lib-objs,host/test-BOARD,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)
#
# Each "object" is a test program (compiled and linked in one step); the
# target runs them all and is a stamp that they passed, so a test reruns when
# it or its command changed. A failing test prints its output.

# Every source with a UNITTEST block and its own main (libjollymaam's are
# src/lib/rules.mk's).
HOST_TESTS := $(shell grep -rlE --include='*.c' 'if(n)?def UNITTEST' src | xargs grep -l '^int main')
HOST_TESTS := $(sort $(filter-out src/platform/telink/att_telink.c src/lib/% src/platform/jieli/btctrler/bd19/%,$(HOST_TESTS)))
# Tests that decode real firmware replies with libjollymaam.
HOST_TESTS_LIBJOLLYMAAM := src/core/usb.c src/updater/updater.c
# Our controller's tests link without the rest of the controller; BD19-only
# sources are tested by controller-test (tools/btctrler/vanilla.mk).
HOST_TESTS_BTCTRLER := $(filter src/platform/jieli/btctrler/%,$(HOST_TESTS))
HOST_TESTS_BTCTRLER_LDFLAGS := -no-pie -Wl,--unresolved-symbols=ignore-all

host_test_libs :=
host_test_ldflags :=
host-test-compile = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -MMD -MP -MF $(@:.o=.d) -MT $@ \
	$< $(host_test_libs) -o $@ $(host_test_ldflags)
lib-type-host-tests = for t in $(lib_objects); do $(HOST_RUNNER) $$t > $$t.out 2>&1 || { cat $$t.out; exit 1; }; done && touch $@
