{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [ gcc gnumake binutils pkg-config hidapi libusb1 zlib python3 ];
}
