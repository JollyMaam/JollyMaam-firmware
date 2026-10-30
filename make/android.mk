# Keep project-local Gradle, Kotlin, and APK outputs under $(O)/android/.
.PHONY: android
android:
	nix develop .#android --command bash -c './android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PoutputDir="$(OF)/android" :app:assembleDebug'

.PHONY: android-check
android-check:
	nix develop .#android --command bash -c './android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PoutputDir="$(OF)/android" :app:assembleDebug :app:assembleDebugAndroidTest :app:testDebugUnitTest :app:lintDebug'
