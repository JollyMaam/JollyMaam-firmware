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
