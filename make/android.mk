# Keep project-local Gradle, Kotlin, and APK outputs under $(O)/android/.
.PHONY: android
android:
	mkdir -p "$(abspath $(O))/android"
	cd "$(abspath $(O))/android" && nix-shell "$(abspath android)/shell.nix" --run '"$(abspath android)/gradlew" --project-dir "$(abspath android)" --project-cache-dir "$(abspath $(O))/android/.gradle" -Pkotlin.project.persistent.dir="$(abspath $(O))/android/.kotlin" -PcycloneOutputDir="$(abspath $(O))/android" :app:assembleDebug'
