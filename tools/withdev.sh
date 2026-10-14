#!/bin/sh
# Run a command with /dev rebuilt so /dev/input/eventN, /dev/input/jsN and
# /dev/hidrawN exist for the nodes in /dev/bwrap (needs: unshare -rm).
set -e
D=$(mktemp -d)
mount -t tmpfs none "$D"
mkdir -p "$D/input" "$D/bwrap" "$D/pts" "$D/shm"
for n in null zero urandom random tty full; do [ -e /dev/$n ] && touch "$D/$n" && mount --bind /dev/$n "$D/$n"; done
mount --bind /dev/bwrap "$D/bwrap"
for f in /dev/bwrap/*; do
  b=$(basename "$f")
  case "$b" in
    event*|js*) touch "$D/input/$b"; mount --bind "$f" "$D/input/$b";;
    hidraw*|sg*) touch "$D/$b"; mount --bind "$f" "$D/$b";;
  esac
done
mount --bind /dev/shm "$D/shm" 2>/dev/null || true
mount --rbind "$D" /dev
exec "$@"
