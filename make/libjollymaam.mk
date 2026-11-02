# Portable core and optional desktop adapters. All host build rules live here;
# Nix supplies compilers/dependencies and invokes these same targets.
HOST_TRIPLE := $(shell $(HOST_CC) -dumpmachine)
JOLLYMAAM_PLATFORM ?= $(if $(findstring mingw,$(HOST_TRIPLE)),windows,linux)
JOLLYMAAM_STATIC ?= 0
JOLLYMAAM_EXEEXT := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),.exe)
HIDAPI_PKG ?= $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),hidapi,hidapi-hidraw)
JOLLYMAAM_PKGS := $(HIDAPI_PKG)
ifeq ($(JOLLYMAAM_STATIC),1)
JOLLYMAAM_PKG_OPTIONS := --static
JOLLYMAAM_STATIC_LDFLAGS := -static $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),-static-libgcc,-pthread)
ifeq ($(JOLLYMAAM_PLATFORM),linux)
# hidapi-hidraw.pc does not always declare its private udev dependency.
JOLLYMAAM_PKGS += $(if $(filter hidapi-libusb,$(HIDAPI_PKG)),libusb-1.0) libudev
endif
endif
ifeq ($(JOLLYMAAM_PLATFORM),windows)
JOLLYMAAM_SYSTEM_LIBS := -lsetupapi -lhid -lcfgmgr32
endif
JOLLYMAAM_LINK_FLAGS = $(JOLLYMAAM_STATIC_LDFLAGS) $(JOLLYMAAM_LDFLAGS)
JOLLYMAAM_CLI := $(O)/host/jollymaam$(JOLLYMAAM_EXEEXT)
JOLLYMAAM_HEADERS := $(wildcard src/lib/*.h src/lib/jieli_image/*.h)
JOLLYMAAM_SOURCES := $(wildcard src/lib/jieli_image/*.c src/lib/proto_*.c src/lib/upgrade_*.c) src/lib/restore.c src/lib/flash_jieli.c src/lib/package.c src/lib/sha256.c src/lib/manifest.c src/lib/session.c
JOLLYMAAM_OBJECTS := $(JOLLYMAAM_SOURCES:src/lib/%.c=$(O)/host/libjollymaam/%.o)
JOLLYMAAM_PLATFORM_SOURCES := src/lib/platform.c src/lib/transport_hid.c src/lib/transport_custom.c src/lib/transport_jieli.c $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),src/lib/transport_win32.c,src/lib/transport_linux.c)
JOLLYMAAM_PLATFORM_OBJECTS := $(JOLLYMAAM_PLATFORM_SOURCES:src/lib/%.c=$(O)/host/platform/%.o)
JOLLYMAAM_APP_SOURCES := tools/jollymaam/jollymaam.c tools/jollymaam/jollymaam_jieli.c tools/jollymaam/jollymaam_update.c tools/jollymaam/jollymaam_restore.c
JOLLYMAAM_APP_HEADERS := $(wildcard tools/jollymaam/jollymaam*.h)

# Rebuild host objects when changing compiler/ABI, dependency environment or
# build flags in the same output directory. Compare before replacing the stamp.
$(O)/host/.jollymaam-config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(HOST_CC)' '$(shell command -v $(firstword $(HOST_CC)))' '$(HOST_TRIPLE)' '$(HOST_AR)' '$(HOST_CFLAGS)' '$(JOLLYMAAM_PLATFORM)' '$(JOLLYMAAM_STATIC)' '$(HIDAPI_PKG)' '$(JOLLYMAAM_LDFLAGS)' '$(PKG_CONFIG_PATH)' '$(PKG_CONFIG_LIBDIR)' '$(NIX_CFLAGS_COMPILE)' '$(NIX_LDFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

.PHONY: libjollymaam libjollymaam-platform jollymaam
libjollymaam: $(O)/host/libjollymaam.a
libjollymaam-platform: $(O)/host/libjollymaam-platform.a
jollymaam: $(JOLLYMAAM_CLI)
$(O)/host/libjollymaam.a: $(JOLLYMAAM_OBJECTS) $(MAKE_FILES)
	$(Q)rm -f $@
	$(Q)$(HOST_AR) rcs $@ $(JOLLYMAAM_OBJECTS)
$(O)/host/libjollymaam/%.o: src/lib/%.c $(JOLLYMAAM_HEADERS) $(O)/host/.jollymaam-config
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -c $< -o $@
$(O)/host/libjollymaam-platform.a: $(JOLLYMAAM_PLATFORM_OBJECTS) $(MAKE_FILES)
	$(Q)rm -f $@
	$(Q)$(HOST_AR) rcs $@ $(JOLLYMAAM_PLATFORM_OBJECTS)
$(O)/host/platform/%.o: src/lib/%.c $(JOLLYMAAM_HEADERS) $(O)/host/.jollymaam-config
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $$(pkg-config --cflags $(HIDAPI_PKG)) -c $< -o $@
$(JOLLYMAAM_CLI): $(JOLLYMAAM_APP_SOURCES) $(JOLLYMAAM_APP_HEADERS) $(O)/host/libjollymaam-platform.a $(O)/host/libjollymaam.a $(MAKE_FILES)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(JOLLYMAAM_APP_SOURCES) $(O)/host/libjollymaam-platform.a $(O)/host/libjollymaam.a $$(pkg-config $(JOLLYMAAM_PKG_OPTIONS) --cflags --libs $(JOLLYMAAM_PKGS)) $(JOLLYMAAM_LINK_FLAGS) $(JOLLYMAAM_SYSTEM_LIBS) -o $@

# Offline firmware image packing; no desktop archive or USB dependencies.
.PHONY: jollymaam-jlpack
jollymaam-jlpack: $(O)/host/jollymaam-jlpack$(JOLLYMAAM_EXEEXT)
$(O)/host/jollymaam-jlpack$(JOLLYMAAM_EXEEXT): tools/jollymaam/jollymaam_jieli.c tools/jollymaam/jollymaam_jieli.h $(O)/host/libjollymaam.a $(MAKE_FILES)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DJOLLYMAAM_PACK_ONLY $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@

JOLLYMAAM_IMAGE_TESTS := builder image bankcb crypto entry key ufw
JOLLYMAAM_MODULE_TESTS := restore proto_8bitdo proto_jollymaam proto_jieli upgrade_jollymaam upgrade_jieli sha256
JOLLYMAAM_IMAGE_TEST_BINS := $(JOLLYMAAM_IMAGE_TESTS:%=$(O)/host/test-jollymaam-%$(JOLLYMAAM_EXEEXT))
JOLLYMAAM_MODULE_TEST_BINS := $(JOLLYMAAM_MODULE_TESTS:%=$(O)/host/test-jollymaam-%$(JOLLYMAAM_EXEEXT))
JOLLYMAAM_SIM_TEST_BINS := $(addprefix $(O)/host/test-jollymaam-,$(addsuffix $(JOLLYMAAM_EXEEXT),upgrade jieli-flash session))
JOLLYMAAM_TEST_BINS := $(JOLLYMAAM_IMAGE_TEST_BINS) $(JOLLYMAAM_MODULE_TEST_BINS) $(JOLLYMAAM_SIM_TEST_BINS)
$(JOLLYMAAM_IMAGE_TEST_BINS): $(O)/host/test-jollymaam-%$(JOLLYMAAM_EXEEXT): src/lib/jieli_image/%.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
$(JOLLYMAAM_MODULE_TEST_BINS): $(O)/host/test-jollymaam-%$(JOLLYMAAM_EXEEXT): src/lib/%.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
$(O)/host/test-jollymaam-upgrade$(JOLLYMAAM_EXEEXT): tools/jollymaam/test_jollymaam_upgrade.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
$(O)/host/test-jollymaam-jieli-flash$(JOLLYMAAM_EXEEXT): tools/jollymaam/test_jollymaam_jieli.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
$(O)/host/test-jollymaam-session$(JOLLYMAAM_EXEEXT): tools/jollymaam/test_jollymaam_session.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
.PHONY: libjollymaam-test-build libjollymaam-test
libjollymaam-test-build: $(JOLLYMAAM_TEST_BINS)
libjollymaam-test: libjollymaam-test-build
	$(Q)for t in $(JOLLYMAAM_TEST_BINS); do $(HOST_RUNNER) $$t || exit 1; done
test: libjollymaam-test

$(O)/host/test-jollymaam-scsi-linux: src/lib/transport_linux.c src/lib/transport_scsi.h $(O)/host/.jollymaam-config
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(JOLLYMAAM_LINK_FLAGS) -o $@
.PHONY: libjollymaam-linux-test
libjollymaam-linux-test: $(O)/host/test-jollymaam-scsi-linux
	$(Q)$(HOST_RUNNER) $<
$(O)/host/test-jollymaam-scsi-win32.exe: tools/jollymaam/test_jollymaam_win32.c src/lib/transport_win32.c $(O)/host/.jollymaam-config
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(JOLLYMAAM_LINK_FLAGS) -lsetupapi -o $@
$(O)/host/app-stubs.c: tools/jollymaam/test_jollymaam_app.py
	@mkdir -p $(@D)
	$(Q)python3 -c 'from tools.jollymaam.test_jollymaam_app import STUBS; print(STUBS)' > $@
$(O)/host/app-mock$(JOLLYMAAM_EXEEXT): $(O)/host/app-stubs.c tools/jollymaam/jollymaam_update.c $(O)/host/libjollymaam.a $(JOLLYMAAM_APP_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -I. $< tools/jollymaam/jollymaam_update.c $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@
.PHONY: jollymaam-windows-test-build
jollymaam-windows-test-build: libjollymaam-test-build $(O)/host/test-jollymaam-scsi-win32.exe $(O)/host/app-mock.exe

$(O)/host/check-jollymaam-image$(JOLLYMAAM_EXEEXT): tools/jollymaam/test_jollymaam_image.c $(O)/host/libjollymaam.a $(JOLLYMAAM_HEADERS)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) -o $@

# Dependency and linkage checks are Make targets too, usable without Nix.
$(O)/host/test-application-deps: tools/jollymaam/test_static_dependencies.c $(O)/host/.jollymaam-config
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $< $$(pkg-config --static --cflags --libs $(HIDAPI_PKG) libusb-1.0 zlib libudev) -static -pthread -o $@
.PHONY: jollymaam-static-deps-check jollymaam-link-check
jollymaam-static-deps-check: $(O)/host/test-application-deps
	$(Q)$<
	$(Q)python3 tools/jollymaam/check_host_binary.py --platform linux --readelf $(HOST_READELF) $<
jollymaam-link-check: $(JOLLYMAAM_CLI)
	$(Q)python3 tools/jollymaam/check_host_binary.py --platform $(JOLLYMAAM_PLATFORM) --readelf $(HOST_READELF) --objdump $(HOST_OBJDUMP) $<

PREFIX ?= /usr/local
.PHONY: install install-jollymaam install-jollymaam-tests
install: install-jollymaam
install-jollymaam: jollymaam
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image
	$(Q)install -m755 $(JOLLYMAAM_CLI) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m644 $(O)/host/libjollymaam.a $(O)/host/libjollymaam-platform.a $(DESTDIR)$(PREFIX)/lib/
	$(Q)install -m644 src/lib/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/
	$(Q)install -m644 src/lib/jieli_image/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image/
install-jollymaam-tests: jollymaam-windows-test-build
	$(Q)install -d $(DESTDIR)$(PREFIX)/share/jollymaam/tests
	$(Q)install -m755 $(JOLLYMAAM_TEST_BINS) $(O)/host/test-jollymaam-scsi-win32.exe $(O)/host/app-mock.exe $(DESTDIR)$(PREFIX)/share/jollymaam/tests/

.PHONY: firmware-package
firmware-package:
	$(Q)python3 tools/jollymaam/firmware_package.py --build-dir $(O) $(O)/firmware.zip

.PHONY: jollymaam-check
jollymaam-check: jollymaam libjollymaam-test libjollymaam-linux-test
	$(Q)python3 -m unittest tools/jollymaam/test_firmware_package.py tools/jollymaam/test_jollymaam_app.py tools/jollymaam/test_jollymaam_jni.py tools/jollymaam/test_jollymaam_restore.py tools/jollymaam/test_jollymaam_jieli.py tools/jollymaam/test_jollymaam_dist.py
	$(Q)python3 -m unittest discover -s android/tools -p 'test_*.py'
