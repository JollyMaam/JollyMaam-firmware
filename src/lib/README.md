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
link `libjollymaam-platform.a` for the optional desktop adapters. Applications
link hidapi and system dependencies separately; neither archive embeds them.

`nix-build jollymaam-static.nix` produces both archives and a fully static
musl CLI. The CLI links hidapi and its platform dependencies separately. The default
HID backend is hidraw; select libusb with `--argstr hidBackend libusb`.
`nix-shell static-shell.nix` provides a matching musl compiler and static libusb,
hidapi and zlib. The build smoke-tests those dependencies without device access.
Use `--arg withZlib false` to omit the unused zlib build dependency and its smoke
test. No zlib objects are merged into the core in either configuration.

The core accepts stored ZIP entries without compression. If deflate support is added,
zlib can be a dependency of that module; whether to bundle its objects or let
applications link their preferred zlib remains a separate decision.

The static Nix artifacts target Linux musl; do not mix them with the regular
glibc development shell or Android binaries. Android needs its own target build.
The regular Makefile also builds the portable `libjollymaam.a`; its CLI links the
desktop platform archive and dynamic hidapi separately.

The CLI currently offers passive 8BitDo bootloader candidate enumeration (`scan`),
offline image inspection (`inspect IMAGE`), raw Jieli image rekeying
(`rekey-jieli INPUT OUTPUT KEY`), and pad bootloader upgrades
(`flash-8bitdo --yes HID_PATH IMAGE`). Linux Jieli flashing uses
`flash-jieli --yes DEVICE LOADER IMAGE`, with the chip's matching SDK RAM-loader
blob. Both flash commands write hardware: select
the intended path and obtain permission before invoking it. Enumeration does not
upload loaders or reboot devices. There is no automatic stock-firmware boot entry
or reconnection yet. The 8BitDo receiver is not accepted for upgrades. `scan` also lists Jieli UBOOT
block-device candidates using sysfs metadata and honors `DEV_ROOT`. It does not
open the block nodes or upload loaders. The Jieli command verifies the INQUIRY
vendor/model before uploading its supplied loader, checks the installed product,
rekeys for the device and applies the Nova pad stock-data preservation range.
It returns after readback verification, leaving the device in its loader.

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
jlnewfw still owns those layout-builder operations.

Jieli loader startup uses `jm_jieli_loader_init`, `jm_jieli_loader_next`, and
`jm_jieli_loader_complete`. The caller verifies the chip, supplies its SDK loader,
and drives yielded SCSI requests, honoring requested delays. Completion exposes
the flash ID, capacity, transfer limit and chip key. RAM writes and execute
commands are not retried after ambiguous failures; readiness polls are bounded.

Jieli flashing uses `jm_jieli_flash_init`, `jm_jieli_flash_next`, and
`jm_jieli_flash_complete`. Before init, read the installed product and rekey the
image if needed. Supply workspace and explicit board-specific keep ranges.
The operation reads those ranges before any erase, then erases, programs nonblank
chunks and verifies the complete written span. It leaves the device in its RAM
loader, with firmware start a separate explicit operation. The portable core session
composes these steps; Android/browser callers can drive the portable
state machines with their own transport. Keep ranges are trusted board policy
and must not cover firmware headers or code.

The custom updater is driven with `jm_update_init`, `jm_update_next`, and
`jm_update_complete`. Connect to the updater first: entering it from the app and
handling re-enumeration remain the caller's responsibility. Requests contain a
logical CDB and borrowed input/output buffers for the caller to frame over USB or
BLE. The operation checks the product, queries room and payload limits, sends
BEGIN and DATA, reconciles dropped writes through UPDATE_INFO with bounded
retries, checks COMMIT, and requests REBOOT. Errors stop the operation without
blind destructive retries. The caller owns timeouts and transport completion.

The libusb mass-storage backend, ordinary stock-gamepad discovery, JNI
integration and WASM builds remain to implement. Windows SCSI and desktop HID
adapters are implemented and cross-built, with hardware validation still pending.

Module boundaries:

- `proto_*.c` encodes and validates packets or their image format; no device I/O.
- `upgrade_*.c` and `flash_jieli.c` sequence requests without allocation or
  transport calls.
- `jieli_image/` transforms and validates caller buffers. Comments beside C definitions describe
  whether a failed operation can leave a partially transformed buffer. Function
  documentation lives beside definitions in the C files.
- `session.c` owns complete update orchestration, HID framing and reconnect policy.
- `platform.c` and `transport_*.c` implement desktop I/O and discovery.
- The CLI owns approval, opening/closing the connection and presentation.

Run reference image comparisons with `tools/test_jollymaam_images.py`, supplying
separately built old and new jlnewfw binaries plus image paths. It compares list,
verify, and unpacked file contents without device access.

`make build/host/check-jollymaam-image` builds the existing-image regression
runner. Pass a raw Jieli image path to check byte-exact repacking, rekeying back
to the original key, file visits and preservation of full-dump reserved tails.

`make build/host/test-jollymaam-jieli-flash` builds the flash-device simulation.
It runs a synthetic image by default; an optional raw image path exercises the
same scenarios on an existing firmware image, without device access.

## Firmware ZIP containers

`make firmware-package` packages existing builds into `build/firmware.zip` (or
`O/firmware.zip`). It requires every supported target's firmware.img and
update.bin plus the SDK BR23 and BD19 RAM loaders. It does not rebuild firmware.
The ZIP uses stored entries, fixed timestamps and sorted names for reproducible
output. manifest.json format version 1 describes products, chips, artifact roles,
sizes, SHA-256 digests, loaders and preservation ranges. Digests are corruption
checks, not signatures. Input artifacts are read before the output is opened.

`jm_package_open` validates the ZIP container and CRCs, and `jm_package_entry`
or `jm_package_find` returns borrowed name/data views. No allocation, filesystem
access or zlib is required. This initial format accepts ZIP32 stored entries only;
compressed/encrypted entries, data descriptors and multi-disk containers are
unsupported. Bounds are 128 entries and 32 MiB. Names cannot contain traversal
segments, absolute paths, backslashes or drive separators; duplicates fail.
The input buffer must remain unchanged while the package or entry views exist.

`jm_manifest_open` parses format version 1, checks every referenced artifact's
size and SHA-256, checks image products and restricts chip and preservation policy
to supported targets. Parsing is bounded to 512 tokens, depth 16 and 32 KiB;
strings are unescaped printable ASCII and numbers are unsigned 32-bit integers.
Duplicate keys/products and unknown format versions fail. SHA-256 remains an
integrity check, not authentication. The selected flash driver performs complete
image/protocol validation before destructive commands.

`jm_manifest_target` selects an exact product and returns borrowed install,
update and loader views. Keep the archive buffer and manifest alive through the
operation. No filesystem or OS dependencies enter the core library.

`jollymaam inspect build/firmware.zip` now validates the manifest and hashes too.
`jollymaam flash-package --yes DEVICE PRODUCT ZIP` verifies the package before
opening a device, selects that product's installation image and matching loader,
and drives the existing desktop adapter. The device protocol still checks actual
identity before erasing. Jieli devices remain in their loader after verification;
8BitDo starts the installed app. Custom-updater desktop transport is not yet wired
into this command. Individual-image commands remain available.

Run `nix-shell --run 'python3 -m unittest tools/test_firmware_package.py'` for the
builder and malformed-container tests. They compile the C reader into a temporary
shared library and compare its behavior with generated Python ZIP fixtures.

## Interactive updater application

Launch `build/host/jollymaam update build/firmware.zip` inside the development
shell, or use the fully static binary from the Nix build outside it. This is a
terminal UI: it verifies the package, lists devices, accepts a selection and asks
for an explicit `yes` before every operation. Blank input, EOF and any other
answer do not authorize updating. Use `r` to rescan after connecting a device or
changing modes, and `q` to quit. No automatic version comparison is implemented.

Discovery currently includes our HID tool devices (app and updater), the 8BitDo
pad bootloader with a verified model response, and Jieli UBOOT block candidates.
A running game-controller personality can hide our tool interface; ordinary stock
gamepad modes are not yet discovered. Jieli candidates require selecting a target
before confirmation, because product identification needs a RAM loader. The
flasher checks the installed product before erasing. Unknown products are refused.

For our firmware, the application uses update.bin, confirms the selected flash
UID and product, enters the updater when needed, reconnects by UID and drives the
portable update state machine over HID feature reports. It shows acknowledged
progress and requests reboot only after COMMIT verification. The desktop HID
adapter currently recognizes our 356- and 512-byte tool descriptors. On Linux,
HID paths honor DEV_ROOT as well as the Jieli block-device adapter.

For stock bootloaders it uses the installation image. Jieli remains in its loader
after verification; the 8BitDo starts its app. Real-device HID behavior is not yet
hardware-validated. Frontend tests use fake devices and exercise confirmation,
cancellation, rescanning, mismatched targets and failure handling without USB I/O.
Run `python3 -m unittest tools/test_jollymaam_app.py` in the development shell.

## Core and platform connection boundary

Both native and cross builds produce `libjollymaam.a` and
`libjollymaam-platform.a`. The core contains no OS handles, paths, hidapi calls,
sleeps or JNI dependencies. The desktop archive contains enumeration, handle
ownership, OS transfer adapters and compatibility wrappers for older entry points.
Link the desktop archive before the core archive, then hidapi/system libraries.
An application-supplied adapter can link the core alone.

The terminal application now follows this pattern after validating its package,
selecting a target and obtaining user approval:

```c
struct jm_platform_connection *connection = jm_platform_open(path, kind);
if (connection) {
    result = jm_session_run(target, kind, expected_uid,
                            jm_platform_io(), connection, progress, user);
    jm_platform_close(connection);
}
```

`session.h` defines typed SCSI, raw HID and feature-report callbacks. The caller
supplies a context pointer; the core borrows it and never frees or closes it.
Read/write callbacks report actual lengths; SCSI also reports sense and status.
Callback details and buffer lifetimes are documented beside the C definitions.
Only operations required by the selected protocol need implementations.

The shared session performs product/UID checks, loader startup, image rekeying,
preservation, updates, verification and custom-updater reconnect timing. A
reconnect callback makes one discovery/open attempt for a requested UID. The core
owns bounded retries and independently checks the returned UID, product and mode.
The compatibility desktop upgrade functions delegate to this same session code.

`jm_session_run` is a blocking convenience runner: callbacks execute on its
caller's thread. For Android, implement this table in a JNI adapter and use a
worker thread; Java/Kotlin owns USB permissions, transfers and device lifecycle.
It does not need to link the desktop archive. No Android adapter exists yet.
The existing per-protocol `next`/`complete` state machines remain available for
asynchronous scheduling; a complete asynchronous session wrapper remains future
work, particularly for browser event loops.

Session tests use callback-only devices and link just the core archive. They
exercise complete Jieli and 8BitDo sessions plus custom updater entry, framing,
reconnection, mismatched identities, failed transfers and dropped writes.
