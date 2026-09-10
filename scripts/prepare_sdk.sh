#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_sdk=${CYCLONE2_SDK:-/work/github.com-Jieli-Tech/fw-AC63_BT_SDK}
expected_commit=fdc018de81629e21e594d677cf05f87d288be0f0
sdk="$project/build/sdk"

actual_commit=$(git -C "$source_sdk" rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "unsupported SDK revision: $actual_commit" >&2
    echo "expected: $expected_commit" >&2
    exit 1
fi

mkdir -p "$project/build"
if [ -d "$sdk" ]; then
    find "$sdk" -depth -mindepth 1 -delete
    rmdir "$sdk"
fi
mkdir -p "$sdk"

# Archive HEAD so local generated files and uncommitted SDK changes cannot
# silently leak into a supposedly reproducible firmware build.
git -C "$source_sdk" archive "$expected_commit" | tar -x -C "$sdk"
cp "$project/src/cyclone2_control.c" "$sdk/apps/hid/cyclone2_control.c"
patch -d "$sdk" -p1 < "$project/patches/sdk.patch"

echo "prepared clean SDK $expected_commit"
