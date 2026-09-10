#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CYCLONE2_TOOLCHAIN:-/work/jieli-linux-toolchains-20250324.1/pi32v2/bin}
sdk="$project/build/sdk"
board="$sdk/apps/hid/board/br23"
elf="$sdk/cpu/br23/tools/sdk.elf"
raw="$project/build/sdk-app.bin"
stock_unpacked=${CYCLONE2_STOCK_UNPACKED:-/work/firmware/fw_3.52.bin_unpack}
stock_app="$stock_unpacked/files/app.bin"
packer=/work/github.com-kagaimiq/jl-misctools/firmware/fwpack_newfw.py
unpacker=/work/github.com-kagaimiq/jl-misctools/firmware/fwunpack_newfw.py
python=/work/bin/python3
dist="$project/dist"
candidate="$dist/cyclone2-sdk-fw.bin"
verify="$project/build/verify"

# Freeze __DATE__/__TIME__ emitted by vendor sources so clean builds are
# byte-for-byte repeatable. This is the timestamp of the pinned SDK commit.
export SOURCE_DATE_EPOCH=1742302100

"$project/scripts/prepare_sdk.sh"

# Generate the preprocessed linker script and used-symbol list first, then
# build only the ELF target. This deliberately skips the vendor post-build
# step, which depends on unavailable proprietary host utilities.
make -C "$board" -f Makefile TOOL_DIR="$tool" pre_build
make -C "$board" -f Makefile TOOL_DIR="$tool" \
    ../../../../cpu/br23/tools/sdk.elf

start_address=$("$tool/nm" -n "$elf" | awk '$3 == "_start" { print $1; exit }')
if [ "$start_address" != "01e00120" ]; then
    echo "unexpected SDK _start address: $start_address" >&2
    exit 1
fi

mkdir -p "$project/build/sections" "$dist"
sections="$project/build/sections"
for section in text data overlay_aec overlay_wav overlay_ape overlay_flac \
    overlay_m4a overlay_amr overlay_dts overlay_fm overlay_mp3 overlay_wma
do
    "$tool/objcopy" -O binary -j ".$section" "$elf" "$sections/$section.bin"
done

# BR23's app.bin is the load image: flash text, RAM initializer data, then the
# mutually-exclusive overlays in the order encoded by sdk.ld/download.c.
"$python" - "$sections" "$raw" <<'PY'
import sys
from pathlib import Path

directory = Path(sys.argv[1])
output = Path(sys.argv[2])
order = (
    "text", "data", "overlay_aec", "overlay_wav", "overlay_ape",
    "overlay_flac", "overlay_m4a", "overlay_amr", "overlay_dts",
    "overlay_fm", "overlay_mp3", "overlay_wma",
)
output.write_bytes(b"".join((directory / f"{name}.bin").read_bytes()
                            for name in order))
PY

# The experimental repacker currently performs fixed-size replacement. Keep
# the unused tail from the known-good stock slot; execution never reaches it.
"$python" - "$raw" "$stock_app" "$dist/app.bin" <<'PY'
import sys
from pathlib import Path

payload = Path(sys.argv[1]).read_bytes()
stock = Path(sys.argv[2]).read_bytes()
if len(payload) > len(stock):
    raise SystemExit(f"SDK image is {len(payload)} bytes; slot is {len(stock)}")
Path(sys.argv[3]).write_bytes(payload + stock[len(payload):])
print(f"SDK load image: {len(payload)} bytes; padded slot: {len(stock)} bytes")
PY

"$python" "$packer" --experimental --replace "app.bin=$dist/app.bin" \
    "$stock_unpacked" "$candidate"

if [ -d "$verify" ]; then
    find "$verify" -depth -mindepth 1 -delete
    rmdir "$verify"
fi
"$python" "$unpacker" --dirname "$verify" "$candidate"
cmp "$dist/app.bin" "$verify/files/app.bin"
cmp "$stock_unpacked/files/cfg_tool.bin" "$verify/files/cfg_tool.bin"

"$tool/objdump" -h "$elf" > "$dist/sdk-sections.txt"
"$tool/objdump" -D -address-mask=0x1ffffff -print-dbg "$elf" \
    > "$dist/sdk.lst"
cp "$elf" "$dist/sdk.elf"

sha256sum "$candidate" "$dist/app.bin" "$dist/sdk.elf"
echo "verified flash candidate: $candidate"
