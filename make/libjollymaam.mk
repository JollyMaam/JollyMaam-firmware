# Portable libjollymaam core. Backends will be linked separately.
JOLLYMAAM_SOURCES := src/lib/jieli_image/config.c src/lib/jieli_image/builder.c src/lib/session.c src/lib/manifest.c src/lib/sha256.c src/lib/package.c src/lib/flash_jieli.c src/lib/upgrade_jieli.c src/lib/upgrade_jollymaam.c src/lib/jieli_image/image.c src/lib/proto_jieli.c src/lib/proto_jollymaam.c src/lib/upgrade_8bitdo.c src/lib/proto_8bitdo.c src/lib/jieli_image/bankcb.c src/lib/jieli_image/ufw.c src/lib/jieli_image/key.c src/lib/jieli_image/crypto.c src/lib/jieli_image/entry.c
JOLLYMAAM_OBJECTS := $(JOLLYMAAM_SOURCES:src/lib/%.c=$(O)/host/libjollymaam/%.o)
.PHONY: libjollymaam
libjollymaam: $(O)/host/libjollymaam.a
$(O)/host/libjollymaam.a: $(JOLLYMAAM_OBJECTS)
	$(Q)ar rcs $@ $^
$(O)/host/libjollymaam/%.o: src/lib/%.c $(wildcard src/lib/jieli_image/*.h src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -c $< -o $@

.PHONY: libjollymaam-test
JOLLYMAAM_TESTS := builder image bankcb crypto entry key ufw
JOLLYMAAM_TEST_BINS := $(JOLLYMAAM_TESTS:%=$(O)/host/test-jollymaam-%)
libjollymaam-test: $(JOLLYMAAM_TEST_BINS)
	$(Q)for t in $^; do $$t || exit 1; done
$(O)/host/test-jollymaam-%: src/lib/jieli_image/%.c $(O)/host/libjollymaam.a $(wildcard src/lib/jieli_image/*.h src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
test: libjollymaam-test

$(O)/host/test-jollymaam-proto_8bitdo: src/lib/proto_8bitdo.c $(O)/host/libjollymaam.a src/lib/proto_8bitdo.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-proto_8bitdo

$(O)/host/test-jollymaam-upgrade: tools/test_jollymaam_upgrade.c $(O)/host/libjollymaam.a $(wildcard src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-upgrade

# Optional desktop backend; portable archive has no hidapi dependency.
HIDAPI_PKG ?= hidapi-hidraw
.PHONY: jollymaam
jollymaam: $(O)/host/jollymaam
JOLLYMAAM_PLATFORM_SOURCES := src/lib/platform.c src/lib/transport_hid.c src/lib/transport_custom.c src/lib/transport_linux.c src/lib/transport_jieli.c
JOLLYMAAM_PLATFORM_OBJECTS := $(JOLLYMAAM_PLATFORM_SOURCES:src/lib/%.c=$(O)/host/platform/%.o)
.PHONY: libjollymaam-platform
libjollymaam-platform: $(O)/host/libjollymaam-platform.a
$(O)/host/libjollymaam-platform.a: $(JOLLYMAAM_PLATFORM_OBJECTS)
	$(Q)ar rcs $@ $^
$(O)/host/platform/%.o: src/lib/%.c $(wildcard src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $$(pkg-config --cflags $(HIDAPI_PKG)) -c $< -o $@
$(O)/host/jollymaam: tools/jollymaam.c tools/jollymaam_update.h tools/jollymaam_update.c $(O)/host/libjollymaam-platform.a $(O)/host/libjollymaam.a
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) tools/jollymaam.c tools/jollymaam_update.c $(O)/host/libjollymaam-platform.a $(O)/host/libjollymaam.a $$(pkg-config --cflags --libs $(HIDAPI_PKG)) $(JOLLYMAAM_LDFLAGS) -o $@

$(O)/host/test-jollymaam-proto_jollymaam: src/lib/proto_jollymaam.c $(O)/host/libjollymaam.a src/lib/proto_jollymaam.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-proto_jollymaam

$(O)/host/test-jollymaam-proto_jieli: src/lib/proto_jieli.c $(O)/host/libjollymaam.a src/lib/proto_jieli.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-proto_jieli

$(O)/host/test-jollymaam-scsi-linux: src/lib/transport_linux.c src/lib/transport_scsi.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< -o $@
.PHONY: libjollymaam-linux-test
libjollymaam-linux-test: $(O)/host/test-jollymaam-scsi-linux
	$(Q)$<

$(O)/host/check-jollymaam-image: tools/test_jollymaam_image.c $(O)/host/libjollymaam.a $(wildcard src/lib/jieli_image/*.h src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a -o $@

$(O)/host/test-jollymaam-upgrade_jollymaam: src/lib/upgrade_jollymaam.c $(O)/host/libjollymaam.a $(wildcard src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-upgrade_jollymaam

$(O)/host/test-jollymaam-upgrade_jieli: src/lib/upgrade_jieli.c $(O)/host/libjollymaam.a $(wildcard src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-upgrade_jieli

$(O)/host/test-jollymaam-jieli-flash: tools/test_jollymaam_jieli.c $(O)/host/libjollymaam.a $(wildcard src/lib/*.h src/lib/jieli_image/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-jieli-flash

.PHONY: firmware-package
firmware-package:
	$(Q)python3 tools/firmware_package.py --build-dir $(O) $(O)/firmware.zip

$(O)/host/test-jollymaam-sha256: src/lib/sha256.c $(O)/host/libjollymaam.a src/lib/sha256.h
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-sha256

$(O)/host/test-jollymaam-session: tools/test_jollymaam_session.c $(O)/host/libjollymaam.a $(wildcard src/lib/*.h)
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a -o $@
libjollymaam-test: $(O)/host/test-jollymaam-session
