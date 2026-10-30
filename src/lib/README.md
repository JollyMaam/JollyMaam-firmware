# libjollymaam

This is an incremental extraction of host functionality from jlnewfw and the
Python tools. Public interfaces use the `jm_` prefix and C99. `jollymaam.h` collects
the portable interfaces; desktop adapters are opt-in and have separate headers.

Build and test from the repository root:

```
nix-shell --run 'make libjollymaam libjollymaam-test libjollymaam-linux-test jollymaam'
build/host/jollymaam inspect build/gamesir-cyclone-2-pad/firmware.img
```

`libjollymaam.a` is the portable protocol and image library. It contains no
OS adapters, hidapi, libusb, or zlib. Applications supply their own transport or
compile the optional desktop adapters and link the platform libraries they need.

`nix-build jollymaam-static.nix` produces this core archive and a fully static
musl CLI. The CLI links hidapi and its platform dependencies separately. Select
the hidraw backend with `--argstr hidBackend hidraw`; the default is libusb.
`nix-shell static-shell.nix` provides a matching musl compiler and static libusb,
hidapi and zlib. The build smoke-tests those dependencies without device access.
Use `--arg withZlib false` to omit the unused zlib build dependency and its smoke
test. No zlib objects are merged into the core in either configuration.

The core does not currently use compression. When ZIP support is implemented,
zlib can be a dependency of that module; whether to bundle its objects or let
applications link their preferred zlib remains a separate decision.

The static Nix artifacts target Linux musl; do not mix them with the regular
glibc development shell or Android binaries. Android needs its own target build.
The regular Makefile also builds the portable `libjollymaam.a`; its CLI links the
desktop adapter and dynamic hidapi separately.

The CLI currently offers passive 8BitDo bootloader candidate enumeration (`scan`),
offline image inspection (`inspect IMAGE`), raw Jieli image rekeying
(`rekey-jieli INPUT OUTPUT KEY`), and pad bootloader upgrades
(`flash-8bitdo --yes HID_PATH IMAGE`). The last command writes hardware: select
the intended path and obtain permission before invoking it. Enumeration does not
upload loaders or reboot devices. There is no automatic stock-firmware boot entry
or reconnection yet. The 8BitDo receiver is not accepted for upgrades.

The portable 8BitDo operation accepts a full pad image and borrows its storage
until completion. Call `jm_8bitdo_upgrade_next`, send the returned report, then
call `jm_8bitdo_upgrade_complete` with the reply. A stale read reply leaves the
request pending, so keep waiting without resending. Complete the final start
transfer without a reply. Android and browser callers can schedule this cycle
asynchronously; they own opening, permissions, timeouts and cancellation. The
hidapi adapter is a blocking desktop implementation of the same cycle.

The operation checks image CRCs and model identity before erasing, compares
existing sectors, verifies erasure, programs and verifies each changed sector,
and writes the header sector last. It has no rollback. Stop scheduling to cancel;
after erasing, an interrupted operation can require recovery. Transport failures
are not automatically retried. Device compatibility still requires hardware
validation; current upgrade tests simulate the device.

Jieli modules provide image CRC/encryption, JLFS entries and flash-base detection,
chip-key records, BANKCB transforms, UFW views, and command encoding/echo checks.
The Jieli CDB helpers are transport-independent. `transport_scsi.h` defines an
optional synchronous callback contract and a Linux SG_IO function for an
application-owned descriptor. That backend returns status, sense and actual
length; a successful ioctl alone does not imply a successful protocol operation.
`jm_jieli_image_open` validates a raw image into caller-owned workspace and
`jm_jieli_image_visit` exposes decoded files without filesystem access. The
configuration file is the binary stream, rather than a generated INI.
`jm_jieli_image_pack` preserves the existing layout and can rekey it, leaving
reserved data beyond the code region unchanged. Same-key repacking is byte-exact.
This API does not support resizing files or rebuilding a new layout; the legacy
jlnewfw still owns those operations, flash sequencing, RAM loader startup and
device discovery.

The custom updater is driven with `jm_update_init`, `jm_update_next`, and
`jm_update_complete`. Connect to the updater first: entering it from the app and
handling re-enumeration remain the caller's responsibility. Requests contain a
logical CDB and borrowed input/output buffers for the caller to frame over USB or
BLE. The operation checks the product, queries room and payload limits, sends
BEGIN and DATA, reconciles dropped writes through UPDATE_INFO with bounded
retries, checks COMMIT, and requests REBOOT. Errors stop the operation without
blind destructive retries. The caller owns timeouts and transport completion.

Desktop framing for the custom updater, ZIP packaging, Windows SCSI access, the
libusb mass-storage backend, custom-device discovery, JNI integration and WASM
builds remain to implement.

Module boundaries:

- `proto_*.c` encodes and validates packets or their image format; no device I/O.
- `upgrade_*.c` sequences requests, with no allocation or transport calls.
- `jieli_image/` transforms and validates caller buffers. Header comments describe
  whether a failed operation can leave a partially transformed buffer. Function
  documentation lives beside definitions in the C files.
- `transport_*.c` adapts platform I/O. The CLI owns paths and presentation.

Run reference image comparisons with `tools/test_jollymaam_images.py`, supplying
separately built old and new jlnewfw binaries plus image paths. It compares list,
verify, and unpacked file contents without device access.

`make build/host/check-jollymaam-image` builds the existing-image regression
runner. Pass a raw Jieli image path to check byte-exact repacking, rekeying back
to the original key, file visits and preservation of full-dump reserved tails.
