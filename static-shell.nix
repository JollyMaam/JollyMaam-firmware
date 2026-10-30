{ pkgs ? import <nixpkgs> {} }:
# All target libraries and the compiler use musl. Do not mix these archives
# with the glibc compiler/libraries from the regular development shell.
(pkgs.mkShell.override { stdenv = pkgs.pkgsStatic.stdenv; }) {
  inputsFrom = [ (import ./jollymaam-static.nix { inherit pkgs; }) ];
  nativeBuildInputs = [ pkgs.gnumake pkgs.pkg-config pkgs.binutils pkgs.python3 ];
}
