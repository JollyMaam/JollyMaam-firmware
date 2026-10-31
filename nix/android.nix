{ pkgs, withEmulator ? false }:
let sdk = (pkgs.androidenv.composeAndroidPackages {
  platformVersions = [ "35" ];
  buildToolsVersions = [ "34.0.0" ];
  includeNDK = true;
  ndkVersions = [ "27.2.12479018" ];
  cmakeVersions = [ "3.22.1" ];
  includeEmulator = withEmulator;
  includeSources = false;
  includeSystemImages = withEmulator;
  systemImageTypes = [ "default" ];
  abiVersions = [ "x86_64" ];
}).androidsdk;
in pkgs.mkShell {
  packages = [ pkgs.jdk17 pkgs.android-tools pkgs.cmake pkgs.ninja pkgs.gnumake pkgs.gcc pkgs.pkg-config pkgs.hidapi pkgs.python3 ] ++ pkgs.lib.optionals withEmulator [ sdk pkgs.scrcpy pkgs.util-linux pkgs.psmisc pkgs.python3 ];
  JOLLYMAAM_EMULATOR_SHELL = if withEmulator then "1" else "0";
  JAVA_HOME = pkgs.jdk17.home;
  ANDROID_NDK_HOME = "${sdk}/libexec/android-sdk/ndk/27.2.12479018";
  ANDROID_HOME = "${sdk}/libexec/android-sdk";
  GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/34.0.0/aapt2";
}
