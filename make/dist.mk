# make dist: into $(DIST_DIR) the static jollymaam for this platform (bin/),
# libjollymaam and its headers (lib/, include/), and, where jollymaam runs
# here, every board's firmware (firmware.zip).  A MinGW HOST_CC (nix develop
# .#windows) gives the Windows ones.  make dist-android: the Android app too.
DIST_DIR ?= $(O)/dist

.PHONY: dist dist-android
dist:
	$(Q)rm -rf $(DIST_DIR)
	$(Q)$(MAKE) --no-print-directory O=$(O)/dist-host JOLLYMAAM_STATIC=1 DESTDIR=$(abspath $(DIST_DIR)) PREFIX=/ install
ifneq ($(JOLLYMAAM_PLATFORM),windows)
	$(Q)$(MAKE) --no-print-directory all $(JOLLYMAAM_CLI)
	$(Q)$(JOLLYMAAM_CLI) package $(O) $(DIST_DIR)/firmware.zip
endif

dist-android: dist android
	$(Q)cp $(O)/android/app/outputs/apk/debug/app-debug.apk $(DIST_DIR)/jollymaam-android-debug.apk
