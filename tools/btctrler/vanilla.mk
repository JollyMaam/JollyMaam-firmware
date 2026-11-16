# Compile and audit both vanilla controller targets independently of the app.
VANILLA_Q32S := external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang
VANILLA_BD19_SOURCES := $(shell $(PYTHON) tools/controller.py sources)
VANILLA_BD19_OBJECTS := $(patsubst %.c,build/vanilla-bd19/%.o,$(VANILLA_BD19_SOURCES) src/btctrler/bd19/assert.c)
VANILLA_BR23_OBJECTS := $(patsubst %.c,build/vanilla-br23/%.o,$(wildcard src/btctrler/*.c))
VANILLA_BD19_FLAGS := $(filter-out -target pi32v2 -mcpu=r3,$(SDK_CFLAGS)) -target q32s -fno-builtin
VANILLA_BD19_DEFINES := $(filter-out -D__GCC_PI32V2__ -DCONFIG_CPU_BR23 -DCONFIG_MEDIA_DEVELOP_ENABLE -DCONFIG_SBC_CODEC_HW -DCONFIG_MSBC_CODEC_HW -DCONFIG_DNS_ENABLE,$(SDK_DEFINES)) -DCONFIG_CPU_BD19 -DCONFIG_NEW_BREDR_ENABLE -DCONFIG_MEDIA_NEW_ENABLE -D__GCC_Q32S__ -DCONFIG_CRYPTO_TOOLBOX_OSIZE_IN_MASKROM
VANILLA_BD19_INCLUDES := $(subst media_develop,media_new,$(subst br23,bd19,$(SDK_INCLUDES)))
VANILLA_BD19_TEST_SOURCES := $(shell $(PYTHON) -c 'from pathlib import Path; from tools.btctrler.controller import selected_sources; print(" ".join(s for s in selected_sources() if "ifdef UNITTEST" in Path(s).read_text()))')
VANILLA_BD19_TESTS := $(patsubst %.c,build/vanilla-bd19-test/%,$(VANILLA_BD19_TEST_SOURCES))

.PHONY: controller-objects controller-ir controller-layouts controller-tools-test controller-audit controller-test controller-compare
controller-objects: $(VANILLA_BD19_OBJECTS) $(VANILLA_BR23_OBJECTS)
build/vanilla-bd19/%.o: %.c $(wildcard src/btctrler/*.h)
	@mkdir -p $(@D)
	$(VANILLA_Q32S) $(VANILLA_BD19_FLAGS) $(VANILLA_BD19_DEFINES) -Werror=implicit-function-declaration -iquote src -iquote src/sdkconfig $(VANILLA_BD19_INCLUDES) -c $< -o $@
# -Os analysis objects: the SDK archive was built with -Os (optsize, no minsize),
# so these inline like the original and suit tools/btctrler/funccmp.py.
build/vanilla-bd19-os/%.o: %.c $(wildcard src/btctrler/*.h)
	@mkdir -p $(@D)
	$(VANILLA_Q32S) $(filter-out -Oz,$(VANILLA_BD19_FLAGS)) -Os $(VANILLA_BD19_DEFINES) -Werror=implicit-function-declaration -iquote src -iquote src/sdkconfig $(VANILLA_BD19_INCLUDES) -c $< -o $@
build/vanilla-br23/%.o: %.c $(wildcard src/btctrler/*.h)
	@mkdir -p $(@D)
	$(CC) $(SDK_CFLAGS) $(SDK_DEFINES) -Werror=implicit-function-declaration -iquote src -iquote src/sdkconfig $(SDK_INCLUDES) -c $< -o $@
build/vanilla-bd19-test/%: %.c src/test.h $(wildcard src/btctrler/*.h)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -DUNITTEST -DCONFIG_CPU_BD19 -iquote src -no-pie -Wl,--unresolved-symbols=ignore-all $< -lm -o $@
controller-test: $(VANILLA_BD19_TESTS)
	@set -e; for t in $^; do $$t; done
controller-ir:
	$(PYTHON) tools/controller.py inspect --chip br23
	$(PYTHON) tools/controller.py inspect --chip bd19
controller-layouts:
	$(CC) -target pi32v2 -std=gnu11 -fsyntax-only -x c src/btctrler/sdk_br23.h
	$(VANILLA_Q32S) -target q32s -std=gnu11 -fsyntax-only -x c src/btctrler/sdk_bd19.h
controller-tools-test:
	$(PYTHON) -m unittest discover -s tools/btctrler -p 'test_*.py'
controller-audit: $(VANILLA_BD19_OBJECTS)
	$(PYTHON) tools/controller.py audit --board vanilla-bd19
	$(PYTHON) tools/controller.py retained --output build/sdk-libs/bd19/btctrler.a
controller-compare: controller-ir $(VANILLA_BR23_OBJECTS)
	$(PYTHON) tools/controller.py compare --owned-board vanilla-br23
