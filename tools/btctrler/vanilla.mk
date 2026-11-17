# Compile and audit both vanilla controller targets independently of the app.
# Toolchains and flags are the chips' from src/make/jieli.mk; tools/controller.py
# reads the SDK archives from $(JIELI_SDK) too.
PYTHON ?= python3
export JIELI_SDK
VANILLA_DIR := src/platform/jieli/btctrler
VANILLA_HEADERS := $(wildcard $(VANILLA_DIR)/*.h)
VANILLA_Q32S := $(JIELI_BD19_CC)
VANILLA_BD19_SOURCES := $(shell $(PYTHON) tools/controller.py sources)
VANILLA_BD19_OBJECTS := $(patsubst %.c,build/vanilla-bd19/%.o,$(VANILLA_BD19_SOURCES) $(VANILLA_DIR)/bd19/assert.c)
VANILLA_BR23_OBJECTS := $(patsubst %.c,build/vanilla-br23/%.o,$(wildcard $(VANILLA_DIR)/*.c))
VANILLA_BD19_FLAGS := $(JIELI_BD19_CFLAGS)
VANILLA_BD19_DEFINES := $(JIELI_BD19_CPPFLAGS)
VANILLA_BD19_INCLUDES := $(SRC_INCLUDES) $(JIELI_BD19_BOARD_INCLUDES)
VANILLA_BR23_COMPILE := $(JIELI_BR23_CC) $(JIELI_BR23_CFLAGS) $(JIELI_BR23_CPPFLAGS) -Werror=implicit-function-declaration $(SRC_INCLUDES) $(JIELI_BR23_BOARD_INCLUDES)
VANILLA_BD19_TEST_SOURCES := $(shell $(PYTHON) -c 'from pathlib import Path; from tools.btctrler.controller import selected_sources; print(" ".join(s for s in selected_sources() if "ifdef UNITTEST" in Path(s).read_text()))')
VANILLA_BD19_TESTS := $(patsubst %.c,build/vanilla-bd19-test/%,$(VANILLA_BD19_TEST_SOURCES))

.PHONY: controller-objects controller-ir controller-layouts controller-tools-test controller-audit controller-test controller-compare
controller-objects: $(VANILLA_BD19_OBJECTS) $(VANILLA_BR23_OBJECTS)
build/vanilla-bd19/%.o: %.c $(VANILLA_HEADERS)
	@mkdir -p $(@D)
	$(VANILLA_Q32S) $(VANILLA_BD19_FLAGS) $(VANILLA_BD19_DEFINES) -Werror=implicit-function-declaration $(VANILLA_BD19_INCLUDES) -c $< -o $@
# -Os analysis objects: the SDK archive was built with -Os (optsize, no minsize),
# so these inline like the original and suit tools/btctrler/funccmp.py.
build/vanilla-bd19-os/%.o: %.c $(VANILLA_HEADERS)
	@mkdir -p $(@D)
	$(VANILLA_Q32S) $(filter-out -Oz,$(VANILLA_BD19_FLAGS)) -Os $(VANILLA_BD19_DEFINES) -Werror=implicit-function-declaration $(VANILLA_BD19_INCLUDES) -c $< -o $@
# BD19 lmp.c.o is one translation unit that #includes its part files.
build/vanilla-bd19/$(VANILLA_DIR)/bd19/lmp/lmp.o build/vanilla-bd19-os/$(VANILLA_DIR)/bd19/lmp/lmp.o: \
	$(wildcard $(VANILLA_DIR)/bd19/lmp/*.c)
build/vanilla-br23/%.o: %.c $(VANILLA_HEADERS)
	@mkdir -p $(@D)
	$(VANILLA_BR23_COMPILE) -c $< -o $@
build/vanilla-bd19-test/%: %.c src/core/test.h $(VANILLA_HEADERS)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -DUNITTEST -DCONFIG_CPU_BD19 $(SRC_INCLUDES) -no-pie -Wl,--unresolved-symbols=ignore-all $< -lm -o $@
controller-test: $(VANILLA_BD19_TESTS)
	@set -e; for t in $^; do $$t; done
controller-ir:
	$(PYTHON) tools/controller.py inspect --chip br23
	$(PYTHON) tools/controller.py inspect --chip bd19
controller-layouts:
	$(JIELI_BR23_CC) -target pi32v2 -std=gnu11 -fsyntax-only -x c $(VANILLA_DIR)/sdk_br23.h
	$(VANILLA_Q32S) -target q32s -std=gnu11 -fsyntax-only -x c $(VANILLA_DIR)/sdk_bd19.h
controller-tools-test:
	$(PYTHON) -m unittest discover -s tools/btctrler -p 'test_*.py'
controller-audit: $(VANILLA_BD19_OBJECTS)
	$(PYTHON) tools/controller.py audit --board vanilla-bd19
	$(PYTHON) tools/controller.py complete
controller-compare: controller-ir $(VANILLA_BR23_OBJECTS)
	$(PYTHON) tools/controller.py compare --owned-board vanilla-br23
