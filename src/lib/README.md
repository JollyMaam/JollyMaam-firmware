# libjollymaam

This is an incremental extraction of host functionality from jlnewfw and the
Python tools. Public interfaces use the `jm_` prefix and C99. `jollymaam.h` collects
the portable interfaces; desktop adapters are opt-in and have separate headers.

Build and test from the repository root:

```
nix-shell --run 'make libjollymaam libjollymaam-test libjollymaam-linux-test jollymaam'
build/host/jollymaam inspect build/gamesir-cyclone-2-pad/firmware.img
```

`nix-build jollymaam-static.nix` produces a fully static musl executable and two
archives:

- `lib/libjollymaam.a` includes our core, HID and Linux SCSI adapters, static
  hidapi (libusb backend by default), libusb, zlib, and the required udev objects.
  Dependencies are merged as object members, not nested archives. Link consumers
  with this single archive and the matching target C runtime (`-static -pthread`);
  no separate `-lhidapi`, `-lusb-1.0`, `-lz` or `-ludev` is required.
- `lib/libjollymaam-core.a` contains only the portable protocol and image code,
  without desktop adapters or third-party dependencies.

Select the hidraw backend with `--argstr hidBackend hidraw`. Exactly one hidapi
backend is merged because their symbols overlap. Use `--arg withZlib false` to
omit zlib when it is not needed. The build links a dependency smoke test against
only the bundled archive and checks for shared-library dependencies; its version
queries do not open devices.

`nix-shell static-shell.nix` provides the matching musl compiler and static
libraries. These archives target Linux musl; do not mix them with the regular
glibc development shell or Android binaries. Android needs its own target build.
The regular Makefile `libjollymaam.a` remains the portable core archive, and its
CLI build links the desktop adapter and dynamic hidapi separately.

The CLI currently offers passive 8BitDo bootloader candidate enumeration (`scan`),
offline image inspection (`inspect IMAGE`), and pad bootloader upgrades
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
The legacy jlnewfw still owns full image orchestration, packing, flash sequencing,
RAM loader upload and device discovery. Those have not all moved into the library.

Custom-firmware support currently covers command construction, IDENTIFY decoding,
and clear MAIN update-image validation. Its full update state machine and desktop
framing are not implemented here yet. ZIP packaging, Windows SCSI access, the
libusb mass-storage backend, custom-device discovery, JNI integration and WASM
builds also remain to implement.

Module boundaries:

- `proto_*.c` encodes and validates packets or their image format; no device I/O.
- `upgrade_8bitdo.c` sequences requests, with no allocation or transport calls.
- `jieli_image/` transforms and validates caller buffers. Header comments describe
  whether a failed operation can leave a partially transformed buffer.
- `transport_*.c` adapts platform I/O. The CLI owns paths and presentation.

Run reference image comparisons with `tools/test_jollymaam_images.py`, supplying
separately built old and new jlnewfw binaries plus image paths. It compares list,
verify, and unpacked file contents without device access.
