# Keep project-local Gradle, Kotlin, and APK outputs under $(O)/android/.
.PHONY: android
android:
	nix-shell ./android/shell.nix --run './android/gradlew --project-dir "android" --project-cache-dir "$(OF)/android/.gradle" -Pkotlin.project.persistent.dir="$(OF)/android/.kotlin" -PcycloneOutputDir="$(OF)/android" :app:assembleDebug'
