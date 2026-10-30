{
  description = "JollyMaam controller firmware, host updater and Android development";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/1e93de5f6da447ce71322128505012dbfc2429d9";
  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; config.allowUnfree = true; config.android_sdk.accept_license = true; };
      linux = import ./nix/linux.nix { inherit pkgs; };
      windows = import ./nix/windows.nix { inherit pkgs; };
      python = pkgs.python3.withPackages (p: [ p.tkinter p.bleak ]);
    in {
      packages.${system} = { default = linux; inherit linux windows;
        linux-libusb = import ./nix/linux.nix { inherit pkgs; hidBackend = "libusb"; };
        linux-minimal = import ./nix/linux.nix { inherit pkgs; withZlib = false; }; };
      apps.${system}.default = { type = "app"; meta.description = "JollyMaam firmware updater"; program = "${linux}/bin/jollymaam"; };
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc gnumake binutils pkg-config hidapi libusb1 zlib python uv ];
        };
        static = (pkgs.mkShell.override { stdenv = pkgs.pkgsStatic.stdenv; }) {
          inputsFrom = [ linux ];
          nativeBuildInputs = with pkgs; [ gnumake pkg-config binutils python3 ];
        };
        windows = (pkgs.mkShell.override { stdenv = pkgs.pkgsCross.mingwW64.stdenv; }) {
          inputsFrom = [ windows ];
          nativeBuildInputs = with pkgs; [ gnumake pkg-config python3 ];
        };
        android = import ./nix/android.nix { inherit pkgs; };
        emulator = import ./nix/android.nix { inherit pkgs; withEmulator = true; };
      };
      checks.${system} = {
        core = linux;
        host = pkgs.stdenv.mkDerivation {
          name = "jollymaam-host-tests";
          src = self;
          nativeBuildInputs = [ pkgs.python3 pkgs.util-linux pkgs.procps ];
          buildInputs = [ pkgs.hidapi ];
          buildPhase = ''
            make libjollymaam-test libjollymaam-linux-test
            python3 -m unittest tools/test_firmware_package.py tools/test_jollymaam_app.py
            python3 -m unittest discover -s android/tools -p 'test_*.py'
          '';
          installPhase = "touch $out";
        };
      };
    };
}
