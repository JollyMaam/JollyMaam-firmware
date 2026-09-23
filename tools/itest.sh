#!/bin/sh
#
# Integration test: run the RAM-only build against a real console and prove it
# never wrote the flash.
#
#   1. dump the whole flash                    -> <dir>/itest-<stamp>-before.bin
#   2. build and flash the RAM-only build (BOND_STORE_RAM=1)
#   3. pair the pad with a real console (human step, prompted)
#   4. read the whole flash back               -> <dir>/itest-<stamp>-after.bin
#   5. compare, ignoring the code span step 2 rewrote on purpose
#   6. if anything else changed, write the "before" dump back and fail
#
# Why this is the interesting test: BOND_STORE_RAM=1 keeps every bond in RAM,
# so a pairing must leave the persistent regions alone.  Those are the VM
# (syscfg, where the flash-backed link keys and peer addresses live), BTIF
# (RF trim / MAC) and the key-mac area.  All of them sit above the application
# image, so "everything from the end of the image onwards must be identical"
# is exactly the property under test.  A difference means the build wrote flash
# anyway, and the original dump is written back so the pad is not left with a
# half-written bond or a modified identity.
#
# The image itself is rewritten by step 2, so it is excluded from the compare.
# Use --compare-from 0 to include it; that always reports a difference and is
# only useful for exercising the restore path.
#
# Usage: tools/itest.sh [--image FILE] [--no-build] [--skip-pair]
#                       [--wait SECONDS] [--loader DEV] [--compare-from OFFSET]
#                       [--flash-size SIZE]

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

image=build-ram/firmware.img
flash_size=0x100000
wait_seconds=180
do_build=1
skip_pair=0
forced_loader=${JLUBOOT_DEVICE:-}
compare_from=
backup_dir=${FLASH_BACKUP_DIR:-/work/cyclone2-flash-backups}

jl_dir=${JLUBOOT_DIR:-/shared/github.com-kagaimiq/jl-uboot-tool}
jl_python=${JLUBOOT_PYTHON:-/work/bin/python}
jl_tool=$jl_dir/jluboottool.py
python=${ITEST_PYTHON:-python3}
c2ctl=$repo_root/build/c2ctl

usage()
{
    echo "usage: $0 [--image FILE] [--no-build] [--skip-pair] [--wait SECONDS]"
    echo "          [--loader DEV] [--compare-from OFFSET] [--flash-size SIZE]"
    echo
    echo "  --image FILE          RAM-only image to flash (default build-ram/firmware.img)"
    echo "  --no-build            do not run make first"
    echo "  --skip-pair           machinery test: do not wait for a real pairing"
    echo "  --wait SECONDS        how long to wait for the loader/console (default 180)"
    echo "  --loader DEV          use this loader device instead of detecting one"
    echo "  --compare-from OFFSET first flash offset to compare (default: end of image)"
    echo "  --flash-size SIZE     how much flash to dump (default 0x100000)"
}

die()
{
    echo "itest: $*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
    --image)
        [ $# -ge 2 ] || die "--image needs a file"
        image=$2
        shift 2
        ;;
    --no-build)
        do_build=0
        shift
        ;;
    --skip-pair)
        skip_pair=1
        shift
        ;;
    --wait)
        [ $# -ge 2 ] || die "--wait needs a number of seconds"
        wait_seconds=$2
        shift 2
        ;;
    --loader)
        [ $# -ge 2 ] || die "--loader needs a device path"
        forced_loader=$2
        shift 2
        ;;
    --compare-from)
        [ $# -ge 2 ] || die "--compare-from needs an offset"
        compare_from=$2
        shift 2
        ;;
    --flash-size)
        [ $# -ge 2 ] || die "--flash-size needs a size"
        flash_size=$2
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
    esac
done

# The loader is a JieLi USB "UBOOT"/"DEVICE"/"UDISK" disk.  Ask the vendor
# tool's own detection code instead of guessing from /dev names.
find_loader()
{
    JL_TOOL_DIR=$jl_dir "$jl_python" -c '
import os, sys
sys.path.insert(0, os.environ["JL_TOOL_DIR"])
from jldevfind import find_jl_devices
devices = find_jl_devices()
print(devices[0]["path"] if devices else "")
' 2>/dev/null || true
}

# First console node that answers, or nothing.
find_console()
{
    for candidate in /dev/bwrap/ttyjl* /dev/bwrap/ttyACM* /dev/ttyACM*; do
        if [ -c "$candidate" ] &&
            "$c2ctl" -p "$candidate" status >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 0
}

loader=

# Put the pad into UBOOT1.00 and remember the device path.  The path can change
# across a reset, so this is called again for every loader session.
enter_loader()
{
    if [ -n "$forced_loader" ]; then
        loader=$forced_loader
        echo "itest: loader at $loader"
        return 0
    fi
    loader=$(find_loader)
    if [ -z "$loader" ]; then
        console=$(find_console)
        if [ -n "$console" ]; then
            echo "itest: asking the firmware at $console to reboot into UBOOT1.00"
            "$c2ctl" -p "$console" uboot || true
        else
            echo "itest: no console and no loader yet; plug the pad in"
        fi
        deadline=$(( $(date +%s) + wait_seconds ))
        while [ -z "$loader" ]; do
            loader=$(find_loader)
            [ -n "$loader" ] && break
            if [ "$(date +%s)" -ge "$deadline" ]; then
                die "no JieLi UBOOT device after ${wait_seconds}s"
            fi
            sleep 1
        done
    fi
    echo "itest: loader at $loader"
}

# Run one loader session (several commands on one connection).
run_loader()
{
    "$jl_python" "$jl_tool" --device "$loader" "$@"
}

wait_for_console()
{
    deadline=$(( $(date +%s) + wait_seconds ))
    while :; do
        console=$(find_console)
        [ -n "$console" ] && { echo "$console"; return 0; }
        if [ "$(date +%s)" -ge "$deadline" ]; then
            return 1
        fi
        sleep 1
    done
}

connected_now()
{
    "$c2ctl" -p "$1" "connect status" 2>/dev/null |
        grep -q 'state=connected'
}

if [ "$do_build" = 1 ]; then
    echo "itest: building the RAM-only image (BOND_STORE_RAM=1 O=build-ram)"
    make BOND_STORE_RAM=1 O=build-ram >/dev/null
fi

[ -r "$image" ] || die "no image at $image"
[ -f "$jl_tool" ] || die "no loader tool at $jl_tool"
[ -x "$jl_python" ] || die "no python at $jl_python"

if [ ! -x "$c2ctl" ]; then
    echo "itest: building $c2ctl"
    make build/c2ctl >/dev/null || die "could not build the console client"
fi

if [ -x build/jlnewfw ]; then
    build/jlnewfw verify "$image" >/dev/null || die "$image does not verify"
fi

image_size=$(wc -c < "$image")
if [ -z "$compare_from" ]; then
    # Round up to a flash sector: the writer erases whole sectors.
    compare_from=$(( (image_size + 4095) / 4096 * 4096 ))
fi

mkdir -p "$backup_dir"
stamp=$(date +%Y%m%d-%H%M%S)
before=$backup_dir/itest-$stamp-before.bin
after=$backup_dir/itest-$stamp-after.bin
readback=$backup_dir/itest-$stamp-readback.bin

echo "itest: image $image ($image_size bytes); comparing flash above $compare_from"

# ---------------------------------------------------------------- 1. dump ---
enter_loader
echo "itest: dumping $flash_size bytes of flash to $before"
run_loader "read 0 $flash_size $before" >/dev/null
[ -s "$before" ] || die "the dump is empty"

# ------------------------------------------------- 2. flash the RAM build ---
size_hex=$(printf '0x%x' "$image_size")
echo "itest: writing $image"
run_loader "write 0 $image" "read 0 $size_hex $readback" reset >/dev/null

if ! cmp "$image" "$readback"; then
    die "readback differs from $image; the pad is left in the loader"
fi
echo "itest: image readback matches"

console=$(wait_for_console) ||
    die "the pad did not return to its console within ${wait_seconds}s"
echo "itest: console at $console"

# ---------------------- 3. commands that used to write flash ----------------
# Everything the firmware persists is RAM-only in this build, so none of these
# may leave a mark: a new identity, pairing windows, profile switches, and the
# report-period setting.  Rejections from a switch already in progress are
# expected and ignored.
echo "itest: exercising report/newaddr/pair/connect/disconnect"
for command in 'report 8' 'newaddr' 'pair 0' 'connect 1' 'disconnect' \
               'connect 0' 'newaddr' 'pair 3 fresh' 'connect status' \
               'status' 'report 10'; do
    "$c2ctl" -p "$console" "$command" >/dev/null 2>&1 || true
    sleep 0.4
done

# ------------------------------------------------------------- 4. pairing ---
if [ "$skip_pair" = 0 ]; then
    cat <<'MSG'

itest: pair the pad with a real console now.

  * put the console on its controller-pairing screen (Switch: Change Grip/Order)
    or wake the pad with the Home + face-button chord it is configured for;
  * let it pair and confirm the pad actually drives the console;
  * pair normally -- do not use "pair N fresh" or "unkey", which are meant to
    change the pad's own Bluetooth address and so do write flash.

Press Enter here once the console is connected and the pad is sending input.
MSG
    if [ -r /dev/tty ]; then
        read -r _ < /dev/tty || true
    else
        read -r _ || true
    fi

    echo "itest: waiting for state=connected"
    deadline=$(( $(date +%s) + wait_seconds ))
    while ! connected_now "$console"; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            die "the pad never reported state=connected; nothing was written to flash"
        fi
        sleep 2
    done
    echo "itest: pad is connected to the console"
else
    echo "itest: --skip-pair: not waiting for a real pairing"
fi

# ------------------------------------------------------- 5. read it back ---
enter_loader
echo "itest: reading $flash_size bytes back to $after"
# No reset here: if the comparison fails this loader session restores the dump.
run_loader "read 0 $flash_size $after" >/dev/null
[ -s "$after" ] || die "the readback is empty"

# ------------------------------------------------------------ 6. compare ---
set +e
"$python" - "$before" "$after" "$compare_from" "$flash_size" <<'PY'
import sys

before_path, after_path, start, end = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0)
before = open(before_path, 'rb').read()
after = open(after_path, 'rb').read()

if len(before) != len(after):
    print(f"itest: dump sizes differ: {len(before)} vs {len(after)}")
    sys.exit(1)

start = min(start, len(before))
end = min(end, len(before))
if start >= end:
    print(f"itest: nothing to compare ({start}..{end})")
    sys.exit(2)

first = []
last = -1
count = 0
for offset in range(start, end):
    if before[offset] != after[offset]:
        count += 1
        last = offset
        if len(first) < 8:
            first.append(offset)

if count == 0:
    print(f"itest: flash unchanged from 0x{start:x} to 0x{end:x} "
          f"({end - start} bytes)")
    sys.exit(0)

print(f"itest: {count} bytes changed between 0x{start:x} and 0x{end:x}")
for offset in first:
    print(f"  offset 0x{offset:x}: {before[offset]:02x} -> {after[offset]:02x}")
if count > len(first):
    print(f"  ... last change at 0x{last:x}")
sys.exit(1)
PY
compare_status=$?
set -e

case $compare_status in
0)
    echo "itest: resetting the pad"
    run_loader reset >/dev/null || true
    if [ "$skip_pair" = 1 ]; then
        echo "itest: PASS - flash unchanged above the image (no pairing was exercised)"
    else
        echo "itest: PASS - the RAM-only build paired and wrote nothing to flash"
    fi
    echo "itest: dumps kept at $before and $after"
    echo "itest: the pad is left running the RAM-only build"
    ;;
2)
    die "nothing to compare; check --compare-from"
    ;;
*)
    echo "itest: restoring the original flash from $before" >&2
    run_loader "write 0 $before" reset >/dev/null
    echo "itest: FAIL - the build wrote flash; the original dump was restored" >&2
    exit 1
    ;;
esac
