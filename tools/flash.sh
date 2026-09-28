#!/bin/sh
#
# Flash the controller through its JieLi UBOOT1.00 USB loader.
#
#   tools/flash.sh [--image FILE] [--no-backup] [--erase-persistent]
#                  [--wait SECONDS] [--loader DEV]
#
# The controller normally runs the application firmware, which can reboot
# itself into the loader on command, so the usual case needs no button presses:
#
#   1. find the loader device; if it is not there, ask the running firmware to
#      reboot into it with the console `uboot` command, then wait for the
#      loader to enumerate (or for the user to plug the controller in)
#   2. optionally read the current 1 MiB flash to a backup file (with --backup)
#   3. write the image at offset 0; that is only the code span, so the VM,
#      BTIF and key/mac regions at 0x40000 and above are left alone
#   4. read the written span back and compare it with the image
#   5. reset the controller and wait for its USB console to return
#
# --backup saves the current 1 MiB flash before writing.
#
# --erase-persistent additionally erases 0x40000..0x7ffff, the VM, BTIF and
# key/mac regions.  That is a factory reset of everything the controller
# remembers: its Bluetooth address, its pairing records and its settings.  Use
# it when a host and the controller disagree about a link key, because bonding
# records live in the Bluetooth controller's own memory and survive a reflash.
#
# If the console does not answer and no loader is present, the script waits:
# unplug the controller and plug it back in, or power-cycle it.  A chip whose
# application does not start leaves its ROM loader on USB, which is what makes
# this recoverable.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

image=build/firmware.img
wait_seconds=120
take_backup=0
erase_persistent=0
loader=${JLUBOOT_DEVICE:-}
backup_dir=${FLASH_BACKUP_DIR:-/work/cyclone2-flash-backups}

jl_dir=${JLUBOOT_DIR:-/shared/github.com-kagaimiq/jl-uboot-tool}
jl_python=${JLUBOOT_PYTHON:-/work/bin/python}
jl_tool=$jl_dir/jluboottool.py
c2ctl=$repo_root/build/c2ctl
flash_size=0x100000

usage()
{
    echo "usage: $0 [--image FILE] [--backup] [--erase-persistent] [--wait SECONDS] [--loader DEV]"
    echo
    echo "  --image FILE       image to write (default build/firmware.img)"
    echo "  --backup           save the current flash before writing"
    echo "  --erase-persistent also erase VM/BTIF/key-mac (new address, no bonds)"
    echo "  --wait SECONDS     how long to wait for the loader or the console (default 120)"
    echo "  --loader DEV       use this loader device instead of detecting one"
}

die()
{
    echo "flash: $*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
    --image)
        [ $# -ge 2 ] || die "--image needs a file"
        image=$2
        shift 2
        ;;
    --backup)
        take_backup=1
        shift
        ;;
    --no-backup)
        # Keep the old spelling as a harmless compatibility option.
        take_backup=0
        shift
        ;;
    --erase-persistent)
        erase_persistent=1
        shift
        ;;
    --wait)
        [ $# -ge 2 ] || die "--wait needs a number of seconds"
        wait_seconds=$2
        shift 2
        ;;
    --loader)
        [ $# -ge 2 ] || die "--loader needs a device path"
        loader=$2
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
    for candidate in /dev/bwrap/ttyjl*; do
        if [ -c "$candidate" ] &&
            "$c2ctl" -p "$candidate" status >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 0
}

wait_for_loader()
{
    deadline=$(( $(date +%s) + wait_seconds ))

    while [ -z "$loader" ]; do
        loader=$(find_loader)
        [ -n "$loader" ] && break
        if [ "$(date +%s)" -ge "$deadline" ]; then
            die "no JieLi UBOOT device after ${wait_seconds}s; plug the controller in, or unplug and replug it"
        fi
        sleep 1
    done
}

[ -r "$image" ] || die "no image at $image (run make first)"
[ -f "$jl_tool" ] || die "no loader tool at $jl_tool"
[ -x "$jl_python" ] || die "no python at $jl_python"

# The console client is small; build it on demand rather than making the user
# remember that it is a make target.
if [ ! -x "$c2ctl" ]; then
    echo "building $c2ctl"
    make build/c2ctl || die "could not build the console client"
fi

# The packed image must be structurally sound before it goes near the device.
if [ -x build/jlnewfw ]; then
    build/jlnewfw verify "$image" >/dev/null || die "$image does not verify"
fi

if [ -z "$loader" ]; then
    loader=$(find_loader)
fi

if [ -z "$loader" ]; then
    console=$(find_console)
    if [ -n "$console" ]; then
        echo "console at $console; asking the firmware to reboot into UBOOT1.00"
        "$c2ctl" -p "$console" uboot || true
    else
        echo "no console and no loader yet"
        echo "  plug the controller in, or unplug and replug it so the ROM loader appears"
    fi
    wait_for_loader
fi

echo "loader at $loader"

workdir=$(mktemp -d)
readback=$workdir/readback.bin
backup=
trap 'rm -rf "$workdir"' EXIT INT TERM

size=$(wc -c < "$image")
size_hex=$(printf '0x%x' "$size")

if [ "$take_backup" = 1 ]; then
    mkdir -p "$backup_dir"
    backup=$backup_dir/flash-$(date +%Y%m%d-%H%M%S).bin
    echo "reading $flash_size bytes of current flash to $backup"
fi

erase_command=
if [ "$erase_persistent" = 1 ]; then
    # 1 MiB part: code ends at 0x40000, VM fills up to the BTIF at 0xfe000 and
    # the key/mac region sits above it.  Erasing only up to 0x7ffff (the old
    # 512 KiB layout) leaves the address and the bonds in place, so the
    # controller would come back with the same identity.
    erase_command="erase 0x40000 0xc0000"
    echo "erasing 0x40000..0xfffff: bonds, settings, address and key/mac"
fi

echo "writing $image ($size bytes) at offset 0"

# One loader session: optional backup, optional factory reset, write, read
# back, reset.  The write only covers the image, so the erased regions stay
# erased and the controller comes up as if it had never been paired.
set -- --device "$loader"
if [ -n "$backup" ]; then
    set -- "$@" "read 0 $flash_size $backup"
fi
if [ -n "$erase_command" ]; then
    set -- "$@" "$erase_command"
fi
set -- "$@" "write 0 $image" "read 0 $size_hex $readback" reset

"$jl_python" "$jl_tool" "$@"

if ! cmp "$image" "$readback"; then
    die "readback differs from $image; the controller is left in the loader"
fi
echo "readback matches the image"

deadline=$(( $(date +%s) + wait_seconds ))
console=
while :; do
    console=$(find_console)
    [ -n "$console" ] && break
    if [ "$(date +%s)" -ge "$deadline" ]; then
        die "the controller did not return to its console within ${wait_seconds}s"
    fi
    sleep 1
done

echo "controller is back at $console"
# With the padbus daemon running it owns the port and reads the boot counter
# after the re-enumeration itself; a second writer would only interleave.
if [ -S /tmp/padbus.sock ]; then
    echo "padbus is running: see its log for the boot counter"
else
    "$c2ctl" -p "$console" debug stat
    "$c2ctl" -p "$console" status list radio
fi

if [ -n "$backup" ]; then
    echo "previous flash saved as $backup"
fi
