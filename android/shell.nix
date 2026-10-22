{ pkgs ? import <nixpkgs> { config.allowUnfree = true; config.android_sdk.accept_license = true; } }:
let sdk = (pkgs.androidenv.composeAndroidPackages {
  platformVersions = [ "35" ];
  buildToolsVersions = [ "34.0.0" ];
  includeEmulator = false;
  includeSources = false;
  includeSystemImages = false;
}).androidsdk;
in pkgs.mkShell {
  packages = [ pkgs.jdk17 pkgs.android-tools ];
  JAVA_HOME = pkgs.jdk17.home;
  ANDROID_HOME = "${sdk}/libexec/android-sdk";
  GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/34.0.0/aapt2";
}
