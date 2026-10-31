{
  description = "JollyMaam controller firmware, host updater and Android development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/1e93de5f6da447ce71322128505012dbfc2429d9";
  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; config.allowUnfree = true; config.android_sdk.accept_license = true; };
      # Make owns build/install/check rules; only dependencies and target selection
      # differ between the static Linux and MinGW packages.
      mkJollymaam = { platform ? "linux", hidBackend ? "hidraw", withZlib ? platform == "linux" }:
        assert builtins.elem platform [ "linux" "windows" ];
        assert builtins.elem hidBackend [ "hidraw" "libusb" ];
        let
          isWindows = platform == "windows";
          target = if isWindows then pkgs.pkgsCross.mingwW64 else pkgs.pkgsStatic;
          hidapi = if isWindows then target.hidapi.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or []) ++ [ "-DBUILD_SHARED_LIBS=OFF" ];
          }) else target.hidapi;
        in target.stdenv.mkDerivation {
          pname = "jollymaam" + pkgs.lib.optionalString isWindows "-windows";
          version = "0.1";
          src = pkgs.lib.cleanSource ./.;
          nativeBuildInputs = [ pkgs.pkg-config pkgs.python3 ];
          buildInputs = [ hidapi ] ++ pkgs.lib.optionals (!isWindows)
            ([ target.libusb1 target.libudev-zero ] ++ pkgs.lib.optional withZlib target.zlib);
          enableParallelBuilding = true;
          makeFlags = [ "JOLLYMAAM_STATIC=1" "JOLLYMAAM_PLATFORM=${platform}"
            "HIDAPI_PKG=${if isWindows then "hidapi" else "hidapi-${hidBackend}"}"
            "PREFIX=${placeholder "out"}" ];
          buildFlags = [ "jollymaam" ] ++ pkgs.lib.optionals isWindows
            [ "jollymaam-windows-test-build" "jollymaam-link-check" ];
          doCheck = !isWindows;
          checkTarget = "libjollymaam-test libjollymaam-linux-test jollymaam-link-check"
            + pkgs.lib.optionalString withZlib " jollymaam-static-deps-check";
          installTargets = [ "install" ] ++ pkgs.lib.optional isWindows "install-jollymaam-tests";
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
          packages = with pkgs; [ jdk17 android-tools cmake ninja gnumake gcc pkg-config hidapi python3 ]
            ++ lib.optionals withEmulator [ sdk scrcpy util-linux psmisc ];
          JOLLYMAAM_EMULATOR_SHELL = if withEmulator then "1" else "0";
          JAVA_HOME = pkgs.jdk17.home;
          ANDROID_NDK_HOME = "${sdk}/libexec/android-sdk/ndk/27.2.12479018";
          ANDROID_HOME = "${sdk}/libexec/android-sdk";
          GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/34.0.0/aapt2";
        };
      python = pkgs.python3.withPackages (p: [ p.tkinter p.bleak ]);
    in {
      packages.${system} = { default = linux; inherit linux windows;
        linux-libusb = mkJollymaam { hidBackend = "libusb"; };
        linux-minimal = mkJollymaam { withZlib = false; }; };
      apps.${system}.default = { type = "app"; meta.description = "JollyMaam firmware updater"; program = "${linux}/bin/jollymaam"; };
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc gnumake binutils pkg-config hidapi libusb1 zlib python uv gcc-arm-embedded ];
        };
        static = (pkgs.mkShell.override { stdenv = pkgs.pkgsStatic.stdenv; }) {
          inputsFrom = [ linux ];
          JOLLYMAAM_STATIC = "1";
          nativeBuildInputs = with pkgs; [ gnumake pkg-config binutils python3 ];
        };
        windows = (pkgs.mkShell.override { stdenv = pkgs.pkgsCross.mingwW64.stdenv; }) {
          inputsFrom = [ windows ];
          JOLLYMAAM_STATIC = "1";
          nativeBuildInputs = with pkgs; [ gnumake pkg-config python3 ];
        };
        android = mkAndroidShell {};
        emulator = mkAndroidShell { withEmulator = true; };
      };
      checks.${system} = {
        core = linux;
        host = pkgs.stdenv.mkDerivation {
          name = "jollymaam-host-tests";
          src = self;
          nativeBuildInputs = [ pkgs.python3 pkgs.util-linux pkgs.procps pkgs.pkg-config pkgs.jdk17 ];
          JAVA_HOME = pkgs.jdk17.home;
          buildInputs = [ pkgs.hidapi ];
          buildFlags = [ "jollymaam-check" ];
          installPhase = "touch $out";
        };
      };
    };
}
