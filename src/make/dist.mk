DIST_DIR ?= $(O)/dist

.PHONY: dist dist-android
dist:
	$(Q)rm -rf $(DIST_DIR)
	$(Q)$(MAKE) --no-print-directory O=$(O)/dist-host JOLLYMAAM_STATIC=1 DESTDIR=$(abspath $(DIST_DIR)) PREFIX=/ install
ifneq ($(JOLLYMAAM_PLATFORM),windows)
	$(Q)$(MAKE) --no-print-directory all $(JOLLYMAAM_CLI)
	$(Q)$(JOLLYMAAM_CLI) package $(O) $(DIST_DIR)/firmware.zip
endif

dist-android: ANDROID_FIRMWARE_ZIP = $(abspath $(DIST_DIR))/firmware.zip
dist-android: dist android
	$(Q)cp $(ANDROID_APK) $(DIST_DIR)/jollymaam-android-$(ANDROID_BUILD_TYPE).apk
