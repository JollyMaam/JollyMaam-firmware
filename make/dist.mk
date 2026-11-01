# Release assembly belongs to Make. Nix is an optional dependency provider.
DIST_DIR ?= $(O)/dist
DIST_FIRMWARE ?= $(O)/firmware.zip
DIST_BOARDS := $(CYCLONE2_PAD) $(CYCLONE2_DONGLE) $(NOVA2LITE_PAD) $(NOVA2LITE_DONGLE) $(U2CW_PAD)

.PHONY: dist dist-firmware dist-linux dist-windows dist-desktop dist-android
dist: dist-$(JOLLYMAAM_PLATFORM)

# Build firmware with a native host compiler before cross-building Windows.
dist-firmware: $(DIST_BOARDS)
	$(Q)python3 tools/jollymaam/firmware_package.py --build-dir $(O) $(DIST_FIRMWARE)
	@mkdir -p $(DIST_DIR)
	$(Q)cp $(DIST_FIRMWARE) $(DIST_DIR)/firmware.zip

dist-linux: dist-firmware
	$(Q)$(MAKE) dist-desktop

# Reuse the common firmware ZIP when cross-compiling; do not execute a PE image
# as the host-side firmware packer. DIST_FIRMWARE may point at a native build.
dist-windows:
	@test '$(JOLLYMAAM_PLATFORM)' = windows || { echo 'dist-windows requires a MinGW compiler (HOST_CC or CC).' >&2; exit 1; }
	$(Q)$(MAKE) dist-desktop

dist-desktop:
	@test -f $(DIST_FIRMWARE) || { echo 'Build firmware first with native make dist-firmware, or set DIST_FIRMWARE.' >&2; exit 1; }
	$(Q)$(MAKE) JOLLYMAAM_STATIC=1 jollymaam-link-check
	$(Q)python3 -m tools.jollymaam.jollymaam_dist desktop --platform $(JOLLYMAAM_PLATFORM) --exe $(JOLLYMAAM_CLI) --firmware $(DIST_FIRMWARE) $(DIST_DIR)/jollymaam-$(JOLLYMAAM_PLATFORM).zip
	$(Q)rm -rf $(O)/dist-sdk-$(JOLLYMAAM_PLATFORM)
	$(Q)$(MAKE) JOLLYMAAM_STATIC=1 DESTDIR=$(abspath $(O)/dist-sdk-$(JOLLYMAAM_PLATFORM)) PREFIX=/ install
	$(Q)python3 -m tools.jollymaam.jollymaam_dist sdk --root $(O)/dist-sdk-$(JOLLYMAAM_PLATFORM) $(DIST_DIR)/jollymaam-$(JOLLYMAAM_PLATFORM)-sdk.zip

# Android needs a configured SDK/NDK and Java, independently of desktop deps.
dist-android: dist-firmware android
	@mkdir -p $(DIST_DIR)
	$(Q)cp $(O)/android/app/outputs/apk/debug/app-debug.apk $(DIST_DIR)/jollymaam-android-debug.apk
