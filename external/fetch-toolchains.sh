#!/usr/bin/env bash
# Fetch the compilers the build uses into external/toolchains/<archive name>/:
# download each pinned archive, check its sha256 and unpack it.  Already
# unpacked toolchains are left alone; delete a directory to fetch it again.
#   external/fetch-toolchains.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
dest=$here/toolchains
downloads=$dest/downloads

# url  sha256
toolchains=(
	"https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20250324.1.tar.xz f686586bcfb45e0f0bb27fd2b39c7a7f313cb4f0e88a66a14da621ffa8225958"
	"https://shyboy.oss-cn-shenzhen.aliyuncs.com/readonly/tc32_gcc_v2.0.tar.bz2 33b854be3e3db3dba4b4dacdda2cd4ea1c94dfd4d562864a095956de7991b430"
)

mkdir -p "$downloads"
for entry in "${toolchains[@]}"; do
	url= sum=
	read -r url sum <<<"$entry"
	archive=$downloads/${url##*/}
	dir=$dest/$(basename "$archive" | sed -E 's/\.tar\.[a-z0-9]+$//')
	if [ -e "$dir/.unpacked" ]; then
		echo "$dir"
		continue
	fi
	if [ ! -e "$archive" ] || ! echo "$sum  $archive" | sha256sum -c --status; then
		echo "downloading $url"
		curl -fL --retry 3 -o "$archive.part" "$url"
		mv "$archive.part" "$archive"
	fi
	if ! echo "$sum  $archive" | sha256sum -c --status; then
		echo "sha256 mismatch for $archive" >&2
		exit 1
	fi
	# Each archive holds one top directory; unpack its contents into $dir.
	rm -rf "$dir"
	mkdir -p "$dir"
	tar -xf "$archive" -C "$dir" --strip-components=1
	touch "$dir/.unpacked"
	echo "$dir"
done
