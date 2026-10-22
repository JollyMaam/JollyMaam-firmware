# Native Android padtool

A small Kotlin app using Android's built-in widgets and USB host API. Android
8.0 (API 26) or newer, a USB host-capable phone, and a data cable/OTG adapter are
required. Configuration uses this firmware's USB tool interface, with protocol
1 as defined in `src/core/usb.c`. Bluetooth configuration is not implemented.

The desktop CLI and Tkinter GUI are independent and remain available.

## Quick UI iteration on Linux

Run this on the Linux host, outside the development container:

```sh
/work/cyclone2-android/android/run-emulator
```

The executable has a Nix shell shebang and works from any directory with Nix
2.24 or newer. It supplies the SDK, emulator, AOSP Android 15 x86_64 system
image, JDK and adb. No Android Studio or Google account is needed. The first
invocation downloads the emulator/image; subsequent invocations reuse them.
An x86_64 Linux host with read/write access to `/dev/kvm` is expected.

The script builds the debug APK, creates a compact phone AVD if needed, boots
it, waits for Android, installs the APK and opens the **device list**. Select
**Simulated pad (no USB)** to explore the app. Keep the script running. Press
**r + Enter** to rebuild/reinstall after
editing, keeping Android booted. Press **q + Enter** or **Ctrl-C** to stop the
session. Closing the emulator window also ends the session. A failed reload
leaves the emulator available so you can fix the error and press r again.

Cleanup stops the build and its descendants, emulator and private adb server,
removes this run's temporary directory, then releases the launcher lock. The
supervisor waits for cleanup before returning. Saved AVD data, simulated
settings, APK build outputs and diagnostic logs are persistent and remain for
the next run. The overview's **Reset simulated pad** resets the demo settings.

Useful options:

```sh
./android/run-emulator --no-build   # install the existing debug APK
./android/run-emulator --cold-boot  # skip snapshot restore when starting
./android/run-emulator --headless   # start without a window
./android/run-emulator --software   # opt into slow CPU emulation without KVM
```

The dedicated emulator uses console port 5580, device port 5581 and serial
`emulator-5580`; set `PADTOOL_EMULATOR_PORT` to another even port from 5554 to
5584 if occupied. The private adb server uses emulator port + 1000 (6580 by
default), overridable with `PADTOOL_ADB_PORT`. The script stops only its own
server; an existing system adb server is untouched. All adb install/launch
commands target the emulator serial explicitly. An emulator already running
at the requested serial is rejected rather than adopted. State, AVD files,
`adb.log` and `emulator.log` live under `$XDG_STATE_HOME/cyclone-padtool/emulator`, falling
back to `~/.local/state/cyclone-padtool/emulator`. GPU rendering uses
SwiftShader, while KVM accelerates the CPU by default. `--software` also
disables CPU acceleration and gives first boot a longer timeout.

The launcher enables `VirtioTablet` and disables the alternative mouse devices
to expose native wheel input without pointer capture. Otherwise the emulator
can translate wheel ticks into animated touchscreen swipes, which also trigger
Android's touch flinging. After updating an older launcher, stop it and run
`./android/run-emulator --cold-boot` once so the new input hardware is used.
Reloading the APK with **r + Enter** does not change emulator hardware options.

`QT_QPA_PLATFORM` is passed through. The launcher defaults to `xcb` when
`DISPLAY` is available. To explicitly use X11/XWayland:

```sh
QT_QPA_PLATFORM=xcb ./android/run-emulator --no-build
```

If a launcher is still active, the lock error includes the lock path and
`fuser` process information. Stop the earlier launcher before retrying. Older
launcher versions could leak their lock descriptor into a child process;
stop that reported process once if it is still running. The updated
supervisor never passes the lock descriptor to its children. The empty
`launch.lock` file remains as a stable coordination file; an existing file by
itself does not mean a lock is held.

To stop/check Gradle on the host, from `android/`:

```sh
nix-shell --run './gradlew --stop'
nix-shell --run './gradlew --status'
pgrep -af '[G]radleDaemon|[K]otlinCompileDaemon'
```

`--stop` covers this Gradle version; `pgrep` checks processes across versions.
No `pgrep` output means neither named daemon is running. Entries marked
STOPPED in Gradle's registry are historical, not live processes.

The app always starts on the device list, including when launched by the
emulator script. In a debug APK, select **Simulated pad (no USB)**. The simulator
uses the normal field decoder, settings reads, WRITE/RESULT/COMMIT flow, draft
handling, editors and JSON files. It includes all firmware fields/defaults,
some saved sample remap settings, a synthetic bond for Unpair, moving input,
and logs for pairing, connect, rumble and LED actions. It models remap-base
inheritance, range checks and the 1024-byte staging limit. Calibration writes
synthetic readings immediately. It is a UI/protocol development aid, not an
emulation of controller timing, radios or sensors; flashing is disabled.

Simulation is explicitly labelled and cannot open the USB transport. Its
fixture is packaged only in debug builds; release builds expose neither the
simulation button nor its fixture. Real device operation still uses
the Android USB transport and needs separate phone/hardware validation.

Regenerate the checked-in simulation fixture when the firmware field table or
defaults change (requires Python 3 and a host C compiler):

```sh
python3 android/tools/generate_simulation.py
```

The exporter invokes the firmware's actual `set_describe`, `set_read` and
`set_read_defaults` with its host-test storage stubs; it does not access USB or
the SDK. Ordinary app builds and emulator launches use the checked-in fixture.

Launcher lifecycle tests (Linux, Python 3 and util-linux; no emulator/USB):

```sh
python3 android/tools/test_launcher.py
```

These use fake build/emulator/adb processes with real process groups, signals,
locks, sockets and a terminal. They cover interrupted builds, Ctrl-C after
launch, Qt startup failure/retry, process descendants, temporary-directory
cleanup, terminal reload/quit and propagation of `QT_QPA_PLATFORM`.

## Build and install

With JDK 17, Android SDK platform 35 and build-tools 34.0.0 installed:

```sh
cd android
export ANDROID_HOME=/path/to/android-sdk
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Alternatively, from `android/` on Nix:

```sh
nix-shell --run './gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug'
nix-shell --run 'adb install -r app/build/outputs/apk/debug/app-debug.apk'
```

The shell uses the configured Nixpkgs channel and accepts the SDK license for
the selected Android packages. Gradle 8.9 (including its download checksum),
AGP 8.7.3, Kotlin 2.0.21 and the Android SDK/build-tools versions are pinned in
the project. Android Studio can also open this directory directly.

The debug APK is signed with the local Android debug key and can be sideloaded.
Builds disable the persistent Gradle daemon by default and run the Kotlin
compiler in the build process. Gradle may fork a single-use JVM to satisfy its
memory settings; that process exits when the build finishes.
`assembleRelease` produces an optimized unsigned APK; production signing keys
are not included. There are no runtime dependencies beyond Kotlin's standard
library and Android. The app requests USB device access, not storage, network,
location or Bluetooth permissions.

## Use

1. End the controller's USB gamepad session, then connect it to the phone.
2. Select its device entry and allow Android's USB access prompt.
3. Open a slot, palette, calibration or remap page. Tap a value to edit it.
4. Stage changes, then Apply. Discard uses the cached originals. Defaults stages
   the firmware defaults, including pairing fields within the selected scope.

Apply, Discard, Defaults and Rescan stay in the fixed toolbar at the very top.
On the pad's main page the setting actions affect that pad; on a detailed page
they affect only that target. Rescan enumerates USB devices and returns to the
device list; selecting a device reads its settings. Back returns from a
settings page to the pad, and from the pad to the device list.

The single status line below the toolbar includes the page, simulation marker,
pending changes, operation/progress and device state. Long press it for full
details, including build/protocol/flash UID and any truncated message. Long
press a setting's value or default label for its help; there are no separate
help buttons. Pages retain their scroll position through edits and recreation.
Mouse-wheel scrolling takes precedence over an earlier button's focus reveal,
so later layouts do not pull the list back to that button. Vertical wheel input
scrolls directly and cancels earlier scrolling animations; touch dragging and
flinging keep their native behaviour.

Drafts survive navigation,
activity recreation and app restarts, and are keyed to the firmware build and
flash UID. Reading a reconnected device reloads its saved values and restores
compatible drafts. A `*` button discards the field's pending edit; a `✓` stages
its default. Developer fields are collapsed by default. Unpair is staged and
only takes effect on Apply.

The firmware's staging area holds 1024 bytes. An Apply larger than that is
rejected before WRITE; use page-level Apply for large imports. Each Apply
checks the device identity, drops stale firmware staging, checks every WRITE
result, and sends one COMMIT. A failed/ambiguous commit requires Rescan and
reopening the device before another Apply. Check the loaded values and discard any already-saved edits
before retrying an action such as Unpair. Successfully acknowledged commits
clear drafts before the subsequent settings read, so a failed refresh cannot
re-stage a completed action.

Key mappings use a selector per key. Rules accept the desktop CLI's text, with
syntax/examples in the editor. Stick curves have draggable points, a value
editor and the same Linear, Concave and S-curve presets as Python padtool.
Choosing Linear disables the curve; other edits enable it. The enable field
can also be edited independently.

Settings backups and remap presets use the desktop format-1 JSON files.
Settings backup reads saved device values, excluding drafts. Restore validates
the file and stages edits, including calibration and pairing; Apply saves them.
Preset export includes the selected remap's pending values. Android's system
file picker handles import and export.

The device overview provides firmware console, live input, short rumble tests,
LED demos and restart/loader entry. Console/input reads are serialized with all
other USB operations and pause when leaving the page or backgrounding the app;
use Start to resume. The console retains up to 32,000 characters. Dongles have
Pair, target-address and tool-only controls.

## Firmware backup and flashing

On a running device, Enter flash loader requests the reboot. Select the
returning UBOOT device after Rescan and grant USB access again. Alternatively,
connect the pad while holding Home + Start, or the dongle while holding its
button.

Select the SDK's `cpu/br23/tools/br23loader.bin` once using **Select RAM
helper**. This proprietary helper is kept in private app storage; it is not
bundled in the APK or repository. Discovery never uploads code. Read product ID,
full flash backup and flashing explicitly upload the helper into the selected
ROM loader's RAM.

Flash firmware accepts a prepared `firmware.img` or a CRC-checked UFW. The app
checks product ID, capacity, chip key and any required key conversion before
showing the final flash choice, then checks again on the selected loader before
erasing. The source file is unchanged. Key conversion supports our generated
images; unsupported stock layouts are rejected. If requested, a full backup is
written and its destination closed before any erase. Every flash verifies the
written bytes before requesting a restart. Progress reports upload, backup,
erase, write and verification stages. Keep the app in the foreground and USB
connected during the operation.

Flashing replaces settings in erased sectors. Save a separate settings backup
if needed. A standalone full backup leaves the device in its loader; reconnect
USB afterwards. A failed write/verification leaves the device in the loader
for recovery.

## Checks and boundaries

`testDebugUnitTest` includes:

- USB Bulk-Only Transport framing, tags, residue, short transfers, reset on
  failure and no automatic command retry;
- complete WRITE result checks, ordering, identity, capacity, refusal and lost
  COMMIT replies;
- Python-generated map/rule/curve vectors and byte-exact firmware rekey output;
- simulated RAM upload, flash capacity/key/PID, erase/write/readback and refusing
  to restart on verification failure;
- backup/preset compatibility and native activity launch/recreation on Android
  9 and 15, plus staging a mapping and retaining its draft.
- device-list startup, toolbar/navigation, long-press help, single-line status,
  scroll retention through edits/recreation, wheel versus deferred focus and
  active flings, wheel direction changes/limits, and subsequent touch flinging.

Regenerate interoperability fixtures after changing Python formats:

```sh
python3 android/tools/generate_vectors.py
```

These checks do not establish compatibility with a physical phone's USB stack.
USB access, the controller's actual BOT behaviour and full flash/recovery still
need validation on real Android hardware. No physical device is touched by the
tests. Advanced CLI tools such as CPU profiling, memory peek, trace analysis and
the ephemeral journal are outside this app.

`Protocol.kt`, `Remap.kt`, `FirmwareImage.kt` and `BulkScsi.kt` contain the wire
formats and logic without Android dependencies. `UsbTransport.kt` adapts Android
USB bulk endpoints. `AppModel.kt` owns the single USB worker, drafts and retained
state; `MainActivity.kt` and `CurveView.kt` provide the UI. `Br23Loader.kt` ports
the Python loader and `SettingsFiles.kt` keeps JSON compatibility.
