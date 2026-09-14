#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 /path/to/isd_download ARGS..." >&2
    exit 2
fi

isd_download=$1
shift

roots=$(nix --extra-experimental-features "nix-command flakes" path-info \
    nixpkgs#libsm \
    nixpkgs#libice \
    nixpkgs#libx11 \
    nixpkgs#libxext \
    nixpkgs#fontconfig \
    nixpkgs#freetype \
    nixpkgs#libxkbcommon \
    nixpkgs#glib \
    nixpkgs#dbus \
    nixpkgs#libxcb-cursor \
    nixpkgs#mesa \
    nixpkgs#libglvnd)

library_path=""
for path in $(nix-store -qR $roots); do
    if [ -d "$path/lib" ]; then
        library_path="${library_path:+$library_path:}$path/lib"
    fi
done

export LD_LIBRARY_PATH="${library_path}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM=offscreen

exec "$isd_download" "$@"
