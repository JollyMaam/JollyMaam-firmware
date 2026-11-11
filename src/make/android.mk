# Keep project-local Gradle, Kotlin, and APK outputs under $(O)/android/.
# The APK takes the firmware package if it is built (make firmware).
ANDROID_FIRMWARE_ZIP ?= $(OF)/firmware.zip
.PHONY: android
android:
	./android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PoutputDir="$(OF)/android" -PfirmwareZip="$(ANDROID_FIRMWARE_ZIP)" :app:assembleDebug

.PHONY: android-check
android-check:
	./android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PoutputDir="$(OF)/android" -PfirmwareZip="$(ANDROID_FIRMWARE_ZIP)" :app:assembleDebug :app:assembleDebugAndroidTest :app:testDebugUnitTest :app:lintDebug
