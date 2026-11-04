# libjollymaam

This library provides host protocol, image and update functionality shared with
the firmware tools. Public interfaces use the `jm_` prefix and C99. `jollymaam.h` collects
the portable interfaces; desktop adapters are opt-in and have separate headers.

`proto_jollymaam_wire.h` is the shared contract with the app firmware and
updater: command IDs, protocol version, battery flags, HID framing constants,
updater states/errors, response sizes/offsets and MAIN image constants. It contains constants only and
adds no firmware code or storage. Change these definitions here rather than
adding local copies. Firmware keeps its platform transport and flash operations;
the host library keeps its request scheduling and validation.

Firmware image builds use `make jollymaam-pack`, jollymaam's image commands
(`jieli pack`, `8bitdo pack` and `8bitdo join`) without USB. It links only the
portable archive and needs no hidapi or desktop transport dependencies. The
full CLI and flashing targets still require the desktop dependencies.

Each board has a `<board>-protocol-test` target, for example
`make gamesir-cyclone-2-pad-protocol-test`. These host tests link the app and
updater command handlers against libjollymaam's decoders and update state
machine. Only platform services and flash are simulated; the firmware produces
the replies. They also run as part of each board's regular tests.

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

Build and test from the repository root:

```
nix develop --command bash -c 'make libjollymaam libjollymaam-test libjollymaam-linux-test jollymaam'
build/host/jollymaam inspect build/gamesir-cyclone-2-pad/firmware.img
```

`libjollymaam.a` is the portable protocol and image library. It contains no
OS adapters, hidapi, libusb, or zlib. Applications supply their own transport or
link `libjollymaam-platform.a` for the optional desktop adapters. Applications
link hidapi and system dependencies separately; neither archive embeds them.

`nix build .#linux` produces both archives and a fully static
musl CLI. The CLI links hidapi and its platform dependencies separately. The default
HID backend is hidraw; select libusb with `nix build .#linux-libusb`.
`nix develop .#static` provides a matching musl compiler and static libusb,
hidapi and zlib. The build smoke-tests those dependencies without device access.
Use `nix build .#linux-minimal` to omit the unused zlib build dependency and its smoke
test. No zlib objects are merged into the core in either configuration.

The core accepts stored ZIP entries without compression. If deflate support is added,
zlib can be a dependency of that module; whether to bundle its objects or let
applications link their preferred zlib remains a separate decision.

The static Nix artifacts target Linux musl; do not mix them with the regular
glibc development shell or Android binaries. Android needs its own target build.
Make owns compilation, tests, installation and release packaging. Nix supplies
compilers and dependencies and invokes those Make targets. Without Nix, install
GNU Make, a C compiler, pkg-config, Python 3 and hidapi development files. Static
Linux builds additionally need static libc, hidapi and libudev archives (and
libusb when selecting that HID backend).

```
make jollymaam libjollymaam-test
make JOLLYMAAM_STATIC=1 jollymaam jollymaam-link-check
make HIDAPI_PKG=hidapi-libusb JOLLYMAAM_STATIC=1 jollymaam
make PREFIX=/usr/local DESTDIR=/tmp/jollymaam-install install
```

`HOST_CC` defaults to the supplied `CC`, or gcc; `HOST_AR` defaults to `AR`.
MinGW compilers select the Windows adapters and `.exe` extension automatically.
`HOST_CFLAGS`, `JOLLYMAAM_LDFLAGS`, `HIDAPI_PKG` and `O` are overridable.
Make rebuilds the host objects when switching compiler, platform or link mode
in the same output directory. The regular shell defaults to dynamic hidapi;
the static and Windows shells select static linking.

`make jollymaam-check` runs the core, platform, CLI, packaging and Android
host-side tests; the JNI tests require Java development tools. Windows test
executables can be built with `make jollymaam-windows-test-build`.

The CLI offers read-only discovery (`scan`, by USB port: our tool devices by
IDENTIFY, 8BitDo boot loaders and stock pads by the model they answer, Jieli
ROM loaders and firmware), offline image inspection (`inspect IMAGE`), the
`jieli` and `8bitdo` groups (`jollymaam help jieli`, `help 8bitdo`): images,
loader entry and flashing by port, as the build's flash targets use them.
`8bitdo flash --pid PID [--device PORT] IMAGE` enters the boot loader from our
or stock firmware and writes the Telink pad's firmware.img sector by sector or
the Yichip pad's raw app as one encrypted stream. The 8BitDo receiver takes
loader entry only. Linux Jieli flashing also has
`jieli install --yes DEVICE LOADER IMAGE`, with the chip's matching SDK RAM-loader
blob. The flash commands write hardware. Enumeration does not upload loaders or
reboot devices, and honors `DEV_ROOT`. The Jieli command verifies the INQUIRY
vendor/model before uploading its supplied loader, checks the installed product,
rekeys for the device and applies the Nova pad stock-data preservation range.
It returns after readback verification, leaving the device in its loader.

The portable 8BitDo operation accepts a full Telink pad image
(`jm_8bitdo_upgrade_init`) or a Yichip pad's raw app (`jm_8bitdo_yichip_init`)
and borrows its storage until completion. `jm_8bitdo_upgrade_timeout` gives the
pending request's reply time (the Yichip stream's erase is long). Call `jm_8bitdo_upgrade_next`, send the returned report, then
call `jm_8bitdo_upgrade_complete` with the reply. A stale read reply leaves the
request pending, so keep waiting without resending. Complete the final start
transfer without a reply. Android and browser callers can schedule this cycle
asynchronously; they own opening, permissions, timeouts and cancellation. The
hidapi adapter is a blocking desktop implementation of the same cycle.

On the Telink pad the operation checks image CRCs and model identity before
erasing, compares existing sectors, verifies erasure, programs and verifies each
changed sector, and writes the header sector last. On the Yichip pad it checks
the model, erases the slot for the whole stream (the app padded to 32 KiB
blocks), programs it encrypted (`jm_8bitdo_yichip_encrypt`, 8CryptDo's cipher,
MIT) and reads it all back. It has no rollback. Stop scheduling to cancel;
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
`jm_jieli_image_config` reconstructs a packable INI; the visitor also exports
MAIN application bytes as `_main.bin`. `jm_jieli_build` builds a new layout from
SDK or reconstructed INI and caller-provided file callbacks. It returns the full
image and optional MAIN update image in owned buffers, released using
`jm_jieli_build_free`. Relative input names resolve beside the INI's virtual
name. No filesystem access occurs, and errors are returned without process exit.
The `jollymaam` pack/list/verify/unpack commands use these core operations.

Jieli loader startup uses `jm_jieli_loader_init`, `jm_jieli_loader_next`, and
`jm_jieli_loader_complete`. The caller verifies the chip, supplies its SDK loader,
and drives yielded SCSI requests, honoring requested delays. Completion exposes
the flash ID, capacity, transfer limit and chip key. RAM writes and execute
commands are not retried after ambiguous failures; readiness polls are bounded.

The synchronous callback API also exposes `jm_session_jieli_open` for chip
identification and RAM-helper startup, `jm_session_jieli_read` for bounded flash
reads, `jm_session_jieli_backup` for streaming a full backup to an app-owned sink,
and `jm_session_jieli_start` for an explicit boot request. These work with an
external transport and do not require the platform archive. Backup leaves flash
unchanged and keeps the RAM helper running. The app owns output-file completion.

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

The libusb mass-storage backend and WASM builds remain to implement. JNI is
implemented in the Android app. Cross-chip interactive discovery of normal stock
firmware remains separate from the Jieli-specific command frontend. Windows SCSI and desktop HID
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

Run reference image comparisons with `tools/jollymaam/test_jollymaam_images.py`, supplying
a saved legacy image-tool binary and the current jollymaam binary plus image paths. It compares list,
verify, unpacked file contents and a byte-exact full rebuild without device access.

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
`jollymaam update --device DEVICE --product PRODUCT --yes ZIP` validates the
package and runs one update on that endpoint using the shared session runner.
Discovery or read-only probing identifies the protocol; the session checks live
identity before erasing. Stock bootloaders use the installation image and
matching RAM helper, while our app/updater uses the MAIN update image. Jieli
devices remain in their loader after verification; 8BitDo and custom updater
sessions request a firmware restart. Individual-image commands remain available.

Run `nix develop --command bash -c 'python3 -m unittest tools/jollymaam/test_firmware_package.py'` for the
builder and malformed-container tests. They compile the C reader into a temporary
shared library and compare its behavior with generated Python ZIP fixtures.

## Updater application

Launch `build/host/jollymaam update build/firmware.zip` inside the development
shell, or use the fully static binary from the Nix build outside it. This is a
terminal UI: it verifies the package, lists devices, accepts a selection and asks
for an explicit `yes` before each interactive operation. Blank input, EOF and any other
answer do not authorize updating. Use `r` to rescan after connecting a device or
changing modes, and `q` to quit. No automatic version comparison is implemented.

Selection options can appear before or after the ZIP filename:

```
build/host/jollymaam update --device DEVICE --product PRODUCT build/firmware.zip
build/host/jollymaam update --device DEVICE --product PRODUCT --yes build/firmware.zip
```

`--device` selects one endpoint and returns after one attempt. `--product`
selects a manifest product, filters incompatible devices and skips the Jieli
product chooser. Either can be supplied alone; remaining selection/confirmation
stays interactive. `--yes` skips confirmation and requires both options. A
selected product must exist in the validated package. Explicit paths not listed
by discovery can be probed when a product is supplied, including SCSI generic
nodes. No helper upload or flash write occurs during these probes. Invalid
options and unknown products return a nonzero status. Selected-device updates
also return nonzero for unavailable/incompatible devices, cancellation or update
failure. The separate package-flashing command has been consolidated into
`update`.

Discovery currently includes our HID tool devices (app and updater), the 8BitDo
pad bootloader with a verified model response, and Jieli UBOOT block candidates.
A running game-controller personality can hide our tool interface; ordinary stock
gamepad modes are not yet discovered. Jieli candidates require selecting a target
before confirmation, because product identification needs a RAM loader. The
flasher checks the installed product before erasing. Unknown products are refused.

For our firmware, the application uses update.bin, confirms the selected flash
UID and product, enters the updater when needed, reconnects by UID and drives the
portable update state machine over the tool device (commands as HID feature
reports, answers on input reports addressed to this program, so other programs
may share the device). It shows acknowledged progress and requests reboot only
after COMMIT verification. The desktop HID adapter recognizes our tool
descriptors: a 64- to 512-byte feature report and the 64-byte input report. On Linux,
HID paths honor DEV_ROOT as well as the Jieli block-device adapter.

For stock bootloaders it uses the installation image. Jieli remains in its loader
after verification; the 8BitDo starts its app. Real-device HID behavior is not yet
hardware-validated. Frontend tests use fake devices and exercise confirmation,
cancellation, rescanning, mismatched targets and failure handling without USB I/O.
Run `python3 -m unittest tools/jollymaam/test_jollymaam_app.py` in the development shell.

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

`session.h` defines typed SCSI, raw HID (input and output reports) and
feature-report (set only) callbacks. The caller
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
It does not need to link the desktop archive. The Android app implements this
adapter in `AndroidNativeUsb.kt`, with `jollymaam_jni.c` bridging the callback
table. Its package updater runs the same session as the desktop CLI; see
`android/README.md` for builds and instrumentation checks.
The existing per-protocol `next`/`complete` state machines remain available for
asynchronous scheduling; a complete asynchronous session wrapper remains future
work, particularly for browser event loops.

Session tests use callback-only devices and link just the core archive. They
exercise complete Jieli and 8BitDo sessions plus custom updater entry, framing,
reconnection, mismatched identities, failed transfers and dropped writes.

## Windows build and distribution

`nix build .#windows` cross-compiles the x86-64 Windows executable,
portable core archive and desktop platform archive. The executable statically
links hidapi and the compiler runtime and imports only Windows system DLLs.
The build also emits cross-compiled offline tests under share/jollymaam/tests;
they can run under Wine without flashing hardware. Windows USB behavior still
requires hardware validation. Raw Jieli access may require an Administrator
terminal; discovery itself uses disk-interface metadata without opening disks.

Release packaging uses Make:

```
nix develop .#static --command make -j32 dist
nix develop .#windows --command make dist-windows
nix develop .#android --command make dist-android
```

`make dist` builds all five firmware targets and packages the current desktop
platform. On Linux it produces `build/dist/firmware.zip`,
`jollymaam-linux.zip` (static executable, firmware ZIP and README) and
`jollymaam-linux-sdk.zip` (executable, archives and headers). The desktop ZIP
preserves the Linux executable permission. Static linkage is checked before
packaging.

Cross-build Windows after building the firmware natively; `make dist-windows`
reuses `build/firmware.zip` and produces the equivalent Windows updater and SDK
ZIPs. It does not execute the cross-built app to pack firmware. Override
`DIST_FIRMWARE` to use another native build's ZIP, `DIST_DIR` for release outputs,
and `O` for the build directory. `make dist-firmware` builds and packages only
the firmware. `make dist-desktop` packages only the current desktop build using
an existing firmware ZIP.

`make dist-android` builds firmware and a debug-signed APK, copying both into
the distribution directory. `make android` and `make android-check` use Java and
the SDK/NDK from the environment; they do not invoke Nix. The APK is a developer
build, not a signed store release. All targets work without Nix when their
compiler, SDK and development dependencies are supplied externally.

Extract the distribution ZIP and open `Update firmware.cmd` to launch the terminal
UI. The distribution contains the executable, firmware.zip, launcher and README;
no additional runtime DLLs are bundled. Fixed archive timestamps and ordering
make repeat builds with identical inputs reproducible.

## Stock-loader entry and stock restore

Our running firmware and updater both implement `REBOOT_LOADER` (`0x11`).
`jm_session_stock_loader` checks the selected UID, sends it once and returns
without discovering the new endpoint. A failed USB completion can accompany a
successful reboot; rescan before retrying. This operation writes no firmware and
requires no replacement updater build. The CLI exposes it as:

```
build/host/jollymaam enter-loader --yes HID_PATH
build/host/jollymaam scan
```

Restore uses the original bootloader endpoint after that mode change. Validate
an external vendor image before opening a device:

```
build/host/jollymaam jieli check-stock PRODUCT STOCK_IMAGE_OR_UFW
build/host/jollymaam 8bitdo check-stock 0x301b STOCK_BIN_OR_DAT
```

Explicit restore commands are:

```
build/host/jollymaam jieli restore --yes BLOCK_PATH BR23 PRODUCT BR23_LOADER STOCK_IMAGE_OR_UFW NEW_BACKUP_FILE
build/host/jollymaam 8bitdo restore --yes HID_PATH 0x301b STOCK_BIN_OR_DAT NEW_BACKUP_FILE
```

Use `BD19` and its matching helper for that chip. The CLI refuses existing
backup files, including aliases of the input. It saves, flushes, syncs and closes
a backup before the first erase. A write/close failure prevents erasure. The
`jm_restore_backup` callback structure makes that barrier explicit for external
transports and application-owned output streams. Verification failure prevents
the subsequent boot request. The CLI requests a restart after success; Jieli's
core restore operation leaves starting firmware to its caller.

Jieli accepts raw images and CRC-checked UFW wrappers. It checks the image,
INQUIRY chip, installed product, capacity and exact chip key. Stock application
check records are not rekeyed: choose the matching-key vendor image or a backup
from the original unit. All supplied image bytes are restored, with no Nova
preservation override. A full-flash backup includes settings; a partial vendor
image leaves sectors beyond its rounded end untouched. A raw backup carries no
unit identity, so product/key checks do not establish which individual unit it
came from. The local factory-cut Cyclone dongle 1.16 image fails validation;
validated vendor 1.19/1.21 images are available in the private collection.

8BitDo restoration accepts a CRC-checked packed pad app or vendor DAT. DAT
validation binds the model and load address and rejects receiver packages. An
extracted raw app has no model tag: `0x301b` is the caller's assertion about its
provenance. Live model checks still precede all writes. Factory flash dumps are
not application images. Restoration writes from `0x6000` and leaves the stock
loader and settings at/above `0x6f000` untouched. Its backup is the entire
`[0x6000,0x6f000)` application region, with file offset zero corresponding to
flash `0x6000`; a backup of our firmware can be reinstalled with `8bitdo flash`.

The private vendor files under `/work/stock-firmware` are external inputs. They
are not committed or added to the distributable firmware ZIP. Normal stock
firmware discovery/loader entry for the cross-chip interactive updater remains
separate. `jieli scan` and `loader` expose the migrated Jieli stock-mode table
and USB-port selection. Android currently exposes stock-loader entry; stock
restoration is available through the portable C API and desktop CLI.

## Jieli command frontend

The firmware updater and Jieli image/device commands share the `jollymaam`
executable. It provides the commands previously supplied by the separate image
tool:

```
build/host/jollymaam jieli pack -c isd_config.ini -u uboot.boot -o firmware.img app.bin
build/host/jollymaam jieli list firmware.img
build/host/jollymaam jieli verify firmware.img
build/host/jollymaam jieli unpack firmware.img unpacked
build/host/jollymaam jieli help
```

The `jieli` group supplies `flash`, `flashpack`, `backup`, `loader` and `run`,
including their existing `--pid`, `--device`, `--loader`, `--enter-loader`,
`--wait`, `--keep`, key and layout options. Board `make ...-flash` and
`make ...-loader` targets use these commands. Their existing explicit device
selection and force/dry-run behavior is retained. `jieli scan` lists Jieli USB
ports, our tool devices and the known stock HID modes without rebooting devices
or uploading helpers. The cross-chip top-level `scan` and package `update` commands retain
their existing behavior.

CLI argument handling and Jieli port/device policy live in
`tools/jollymaam/jollymaam_jieli.c`; image formats and transformations live in the portable
core. Windows links its device-configuration calls against the system cfgmgr32
library. Image inputs accept both Windows and POSIX path separators.
`JOLLYMAAM_LOADER` and `JOLLYMAAM_DEBUG` are the environment names for this
frontend; the former tool's environment names remain accepted aliases. Existing
`[JLNEWFW]` INI sections remain valid layout metadata for compatibility, and new
unpacked INIs name the `jollymaam` commands.

## Command groups and help

Protocol-specific commands live under `jollymaam jieli` and `jollymaam 8bitdo`.
Use `jollymaam help`, `jollymaam --help`, `jollymaam help jieli`,
`jollymaam jieli help` or `jollymaam 8bitdo --help` for usage. A group without a
subcommand also shows help. `--help` after a subcommand shows its group usage
before opening files or devices. Unknown commands return status 2; help returns
zero. The previous flat protocol command names are no longer accepted.

The shared `scan`, `update`, `inspect` and `enter-loader`
commands remain at the top level. `jollymaam jieli flash` retains the
option-based Jieli workflow; `jollymaam jieli install --yes DEVICE LOADER IMAGE`
is the direct installation command. The 8BitDo group provides `scan`, `flash`,
`check-stock` and `restore`.

Top-level help orders the entries as `scan`, `update`, `inspect`, `jieli` and
`8bitdo`, followed by `enter-loader` and global help. Protocol entries include
indented subcommand lists, without listing help as a protocol operation. Help
remains available at every level. `enter-loader --yes HID_PATH` is the name for
requesting stock-bootloader entry from our running app or updater.
