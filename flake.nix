{
  description = "JollyMaam controller firmware, host updater and Android development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/1e93de5f6da447ce71322128505012dbfc2429d9";
  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; config.allowUnfree = true; config.android_sdk.accept_license = true; };
      # Make owns build/install/check rules; Nix picks the static toolchain.
      mkJollymaam = { platform ? "linux" }:
        let target = if platform == "windows" then pkgs.pkgsCross.mingwW64 else pkgs.pkgsStatic;
        in target.stdenv.mkDerivation {
          pname = "jollymaam" + pkgs.lib.optionalString (platform == "windows") "-windows";
          version = "0.1";
          src = pkgs.lib.cleanSource ./.;
          enableParallelBuilding = true;
          makeFlags = [ "JOLLYMAAM_STATIC=1" "JOLLYMAAM_PLATFORM=${platform}" "PREFIX=${placeholder "out"}" ];
          buildFlags = [ "jollymaam" ];
          doCheck = platform != "windows";
          checkTarget = "libjollymaam-test";
          installTargets = [ "install" ];
        };
      linux = mkJollymaam {};
      windows = mkJollymaam { platform = "windows"; };
      mkAndroidShell = { withEmulator ? false }:
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
          packages = with pkgs; [ jdk17 android-tools cmake ninja gnumake gcc pkg-config python3 ]
            ++ lib.optionals withEmulator [ sdk scrcpy util-linux psmisc ];
          JOLLYMAAM_EMULATOR_SHELL = if withEmulator then "1" else "0";
          JAVA_HOME = pkgs.jdk17.home;
          ANDROID_NDK_HOME = "${sdk}/libexec/android-sdk/ndk/27.2.12479018";
          ANDROID_HOME = "${sdk}/libexec/android-sdk";
          GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/34.0.0/aapt2";
        };
      python = pkgs.python3.withPackages (p: [ p.tkinter p.bleak ]);
    in {
      packages.${system} = { default = linux; inherit linux windows; };
      apps.${system}.default = { type = "app"; meta.description = "JollyMaam firmware updater"; program = "${linux}/bin/jollymaam"; };
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc gnumake binutils pkg-config python uv gcc-arm-embedded nodejs ];
          # make dist links jollymaam statically against these (not on the
          # path: everything else links as usual).
          JOLLYMAAM_LIBC_STATIC = "${pkgs.glibc.static}/lib";
        };
        windows = (pkgs.mkShell.override { stdenv = pkgs.pkgsCross.mingwW64.stdenv; }) {
          JOLLYMAAM_STATIC = "1";
          nativeBuildInputs = with pkgs; [ gnumake ];
        };
        android = mkAndroidShell {};
        emulator = mkAndroidShell { withEmulator = true; };
      };
      checks.${system}.core = linux;
    };
}
