# Keep project-local Gradle, Kotlin, and APK outputs under $(O)/android/.
# The APK always carries the firmware package: make firmware first, unless
# ANDROID_FIRMWARE_ZIP names one built elsewhere (CI).
ANDROID_FIRMWARE_ZIP ?= $(OF)/firmware.zip
# release: signed with the key in JOLLYMAAM_KEYSTORE (android/app/build.gradle.kts).
ANDROID_BUILD_TYPE ?= debug
ANDROID_APK := $(O)/android/app/outputs/apk/$(ANDROID_BUILD_TYPE)/app-$(ANDROID_BUILD_TYPE).apk
ANDROID_GRADLE := ./android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PoutputDir="$(OF)/android" -PfirmwareZip="$(ANDROID_FIRMWARE_ZIP)" -PappVersion="$(BUILD_ID)"
android android-check: $(if $(filter $(OF)/firmware.zip,$(ANDROID_FIRMWARE_ZIP)),firmware)
.PHONY: android
android:
	$(ANDROID_GRADLE) :app:assemble$(if $(filter release,$(ANDROID_BUILD_TYPE)),Release,Debug)

.PHONY: android-check
android-check:
	$(ANDROID_GRADLE) :app:assembleDebug :app:assembleDebugAndroidTest :app:testDebugUnitTest :app:lintDebug
