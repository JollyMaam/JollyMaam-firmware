# libjollymaam

This is an incremental extraction of host functionality from jlnewfw and the
Python tools. Public interfaces use the `jm_` prefix and C99. `jollymaam.h` collects
the portable interfaces; desktop adapters are opt-in and have separate headers.

Build and test from the repository root:

```
nix-shell --run 'make libjollymaam libjollymaam-test libjollymaam-linux-test jollymaam'
build/host/jollymaam inspect build/gamesir-cyclone-2-pad/firmware.img
```

`nix-build jollymaam-static.nix` builds a fully static musl executable and the
portable archive. Static libusb, both hidapi backends, zlib, and their required
udev implementation are available through `nix-shell static-shell.nix`. That
shell uses a matching musl compiler; do not mix its archives with the regular
glibc development shell. Use `pkg-config --static` when linking. The static Nix
build checks that all three dependencies can link and run without shared
libraries, using version queries that do not access devices.

The regular Makefile CLI build uses dynamic hidapi. The static
archive does not include desktop adapters or depend on hidapi.

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
