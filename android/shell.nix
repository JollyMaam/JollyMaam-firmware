{ pkgs ? import <nixpkgs> { config.allowUnfree = true; config.android_sdk.accept_license = true; }
, withEmulator ? false
}:
let sdk = (pkgs.androidenv.composeAndroidPackages {
  platformVersions = [ "35" ];
  buildToolsVersions = [ "34.0.0" ];
  includeEmulator = withEmulator;
  includeSources = false;
  includeSystemImages = withEmulator;
  systemImageTypes = [ "default" ];
  abiVersions = [ "x86_64" ];
}).androidsdk;
in pkgs.mkShell {
  packages = [ pkgs.jdk17 pkgs.android-tools ] ++ pkgs.lib.optionals withEmulator [ sdk pkgs.scrcpy pkgs.util-linux pkgs.psmisc pkgs.python3 ];
  JAVA_HOME = pkgs.jdk17.home;
  ANDROID_HOME = "${sdk}/libexec/android-sdk";
  GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/34.0.0/aapt2";
}
