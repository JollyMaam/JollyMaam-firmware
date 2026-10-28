#!/bin/sh
# Run a command with /dev rebuilt so /dev/input/eventN, /dev/input/jsN and
# /dev/hidrawN (and /dev/bus/usb/BBB/DDD) exist for the nodes in $DEV_ROOT,
# for programs that only look there (SDL).  A snapshot: nodes that come
# later appear only in $DEV_ROOT, which stays where it was (needs: unshare -rm).
set -e
src=${DEV_ROOT:?"withdev.sh: DEV_ROOT names the directory with the device nodes"}
D=$(mktemp -d)
mount -t tmpfs none "$D"
mkdir -p "$D/input" "$D/pts" "$D/shm"
for n in null zero urandom random tty full; do [ -e /dev/$n ] && touch "$D/$n" && mount --bind /dev/$n "$D/$n"; done
rel=${src#/dev/}
if [ "$rel" != "$src" ]; then mkdir -p "$D/$rel"; mount --bind "$src" "$D/$rel"; fi
for f in "$src"/*; do
  b=$(basename "$f")
  case "$b" in
    event*|js*) touch "$D/input/$b"; mount --bind "$f" "$D/input/$b";;
    hidraw*|sg*|uhid) touch "$D/$b"; mount --bind "$f" "$D/$b";;
    usb-*-*)     # raw USB device (usb-BUSNUM-DEVNUM), for libusb
      bus=${b#usb-}; dev=${bus#*-}; bus=${bus%-*}
      mkdir -p "$D/bus/usb/$bus"; touch "$D/bus/usb/$bus/$dev"; mount --bind "$f" "$D/bus/usb/$bus/$dev";;
  esac
done
mount --bind /dev/shm "$D/shm" 2>/dev/null || true
mount --rbind "$D" /dev
exec "$@"
