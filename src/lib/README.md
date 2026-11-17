# libjollymaam

Our pads' and dongles' firmware, images and loaders, in C99 with no OS
calls. `jollymaam.h` is its interface:

- **Products** (`products.c`): every product's image PID, build directory,
  chip, 8BitDo loader model, flash the vendor loader keeps (the Nova pad's
  stock data) and how to enter the vendor loader by hand; and how stock
  firmware shows itself on USB and is restarted into its loader. Nothing
  else (make, the app, Android) keeps product knowledge.
- **Firmware** (`firmware.c`): a package is `manifest.txt` and the files it
  lists, in a folder or a zip (`package.c` reads stored zips). Its first
  line is `jollymaam-firmware 2`, then a line a file:
  `NAME ROLE PATH SIZE SHA256`, NAME a product's PID (ROLE `install`: what the
  vendor loader writes; `update`: update.bin for our updater) or a Jieli chip
  (ROLE `loader`: the SDK's RAM loader). Every file is checked on opening.
- **Devices** (`device.c`): finding devices by USB port; our firmware's tool
  commands and updater; the vendor loaders (Jieli ROM, 8BitDo Telink and
  Yichip); stock firmware's loader entry; and the operations on top: a pad
  step by step (`jm_pad`: ready, backup, install, restore, reboot, on one
  loader session), scan, and the one-call oemflash, update, loader, start,
  backup.
- The formats and protocols under them: `proto_*`, `upgrade_*`
  (state machines without I/O), `jieli_image/` (Jieli images).

The application supplies device access as hooks (`struct jm_hooks`): list
HID interfaces and USB disks with their ports, open and close, HID reports
(write, read, set feature), SCSI commands, sleep, and where messages go. The
jollymaam command has one table per platform (`tools/jollymaam/hooks_linux.c`,
`hooks_win32.c`, `hooks_hidapi.c` for anything else); the Android app's JNI
bridge is another (`android/app/src/main/cpp/jollymaam_jni.c`). Without
`scsi` (hidapi) there are no Jieli ROM loaders.

## The jollymaam command

`tools/jollymaam/`: `jollymaam help` lists its commands, `jollymaam help
COMMAND` explains one. `list`, `install`, `update`, `backup` and `reboot` act
on one pad (`--device PORT` names it); `package`, `inspect`, `pack` and
`unpack` work on files. The build uses `pack` for its images and `package`
for `$(O)/firmware.zip`.

```
nix develop -c make jollymaam libjollymaam-test
build/host/jollymaam list
```

`make dist` adds a static `jollymaam`, the library and its headers, and every
board's firmware as a zip. `nix build .#linux` / `.#windows` build the static
command alone.

## Settings storage check

Each board's settings unit test (`settings.c`, `make <board>-test`) checks
every persisted target with all fields explicitly set: 32-bit field masks,
each record's buffer limit and its one-byte length limit. On boards with
the core store (core/store.c: Telink, YC3121) it additionally checks record/key
limits and the complete compacted flash footprint against one bank
(STORE_BANK), reserving the receiver and optional last-slot records plus space
to append the largest replacement record. Both banks are needed for
compaction, so their capacities are not added together. This covers
the current build's supported records; obsolete records left by other firmware
are not included.

On Jieli the check uses one half of the configured 16 KiB VM region, a
4-byte area header, and 4 bytes per VM item without padding. It reserves space
for the largest overwrite and an explicit 1 KiB budget for SDK-owned/retained
items (`JIELI_VM_SETTINGS_RESERVE`). The SDK can erase both areas if a move
cannot fit the live records. The check rejects configurations over the budget;
it is not a full capacity guarantee for arbitrary SDK or obsolete records beyond
that reserve. Only the reviewed 8/16 KiB sector-mode layout is modeled.
