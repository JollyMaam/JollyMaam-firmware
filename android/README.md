# Android app

The pad settings page (`web/`, see `web/README.md`) in a WebView, with what a
WebView lacks supplied by the app: USB, Bluetooth, and a flasher for the boot
loaders a browser cannot reach. Android 8.0 (API 26) or newer. USB needs a
USB host-capable phone and an OTG adapter or USB-C cable; Bluetooth needs LE.

The page is the same one Chrome serves; in the app it gets its links from
`window.JollyMaamAndroid` instead of Web Bluetooth and WebHID
(`web/link-android.js`), and its home watches USB: a pad plugged in is asked
what it is and taken where it belongs (`web/flash.js`). The APK always carries the firmware package
(`build/firmware.zip`; `make android` builds it first); the page uses
it, and **Load package** on the home screen swaps in another until unloaded. `make dist-android` takes the one in
`build/dist/`.

- `MainActivity.kt`: the WebView. It serves `assets/web/` (copied from
  `web/` at build time) as `https://appassets.androidplatform.net/`, an https
  origin, so the page is a secure context. File inputs open Android's file
  picker; links to other sites open in the browser. Debug builds allow
  `chrome://inspect` from a desktop.
- `Bridge.kt`: what the page calls. Every call takes a call number and returns
  at once; the answer comes back as `jmAndroid.done(call, error, value)`, what a
  link hears as `jmAndroid.event(handle, type, value)`, bytes as base64.
- `HidLink.kt`: the tool device (16c0:05df, usage page 0xffc2) over the USB
  host API: feature reports out, input reports up, as WebHID gives them. The
  framing (`web/frames-hid.js`) stays in the page.
- `BleLink.kt`: a GATT link to a pad in config mode or its updater: frames
  written, notifications up, as Web Bluetooth gives them. Android runs one GATT
  operation at a time; they queue on one thread. It asks for a 517-byte MTU.
- The flasher: `NativeCore` (libjollymaam over JNI, `src/main/cpp/`) with
  `AndroidNativeUsb` for its USB hooks: our firmware or updater (kind 0), the
  8BitDo boot loader (1), the Jieli ROM loader, which is USB mass storage (2),
  and a pad on stock firmware (3: an identity in libjollymaam's table; each
  HID interface's report descriptor goes to the library, which picks the one
  taking its loader command). Backup, install, restore and reboot run as one
  pad session (`jm_pad`): the pad goes into its vendor loader once, and a
  Jieli RAM loader is uploaded once.

## Use

1. Open the app.
2. Home: **Load Package** (**i** lists the package's contents, and
   unloads a loaded one), then **Devices**: every pad on USB, stock ones
   too (plugging one in also offers the app), and, with the **Bluetooth**
   switch on, pads in config mode (hold Home+M for 3 s).
3. Tap a device for its page: what it is, and **Configure**, **Install**,
   **Upgrade**, **Backup** and **Restore**, each on when the pad and the
   package allow it (the reason shows underneath). Install wipes all
   settings and the flash (**Back up first**, on by default, saves the whole
   flash into Download/JollyMaam). Restore takes a backup of that pad or a
   vendor image. Over Bluetooth only Configure and Upgrade apply. A stock
   mode the app cannot restart into its loader says what to do by hand.

Back goes back a screen.

After a restart into the updater the pad is a new USB device: Android may ask
for USB access again.

## Quick UI iteration on Linux

Run this on the Linux host, outside the development container:

```sh
./android/run-emulator
```

The executable enters the top-level flake emulator shell and works from any directory with Nix
2.24 or newer. It supplies the SDK, emulator, AOSP Android 15 x86_64 system
image, JDK, adb and scrcpy. No Android Studio or Google account is needed. The first
invocation downloads the emulator/image; subsequent invocations reuse them.
An x86_64 Linux host with read/write access to `/dev/kvm` is expected.

The script builds the debug APK, creates a compact phone AVD if needed, boots
the normal emulator window, waits for Android, installs the APK and opens the
page. Use `--scrcpy` to run the emulator with `-no-window` and show
Android through scrcpy instead. The emulator has no pads: it shows the page
and the app's own errors (no pad on USB). Keep the script running. Press
**r + Enter** to rebuild/reinstall after
editing, keeping Android booted. Press **q + Enter** or **Ctrl-C** to stop the
session. Closing the emulator window (or scrcpy when selected) also ends the session. A failed reload
leaves the emulator available so you can fix the error and press r again.

Cleanup stops the build, scrcpy and their descendants, emulator and private adb server,
removes this run's temporary directory, then releases the launcher lock. The
supervisor waits for cleanup before returning. Saved AVD data, APK build
outputs and diagnostic logs are persistent and remain for the next run.

Useful options:

```sh
./android/run-emulator --no-build   # install the existing debug APK
./android/run-emulator --scrcpy     # use scrcpy instead of the emulator window
./android/run-emulator --cold-boot  # skip snapshot restore when starting
./android/run-emulator --headless   # no display window (for automation)
./android/run-emulator --software   # opt into slow CPU emulation without KVM
```

The dedicated emulator uses console port 5580, device port 5581 and serial
`emulator-5580`; set `PADTOOL_EMULATOR_PORT` to another even port from 5554 to
5584 if occupied. The private adb server uses emulator port + 1000 (6580 by
default), overridable with `PADTOOL_ADB_PORT`. The script stops only its own
server; an existing system adb server is untouched. scrcpy uses that same
server and the explicit emulator serial, with audio forwarding disabled.
All adb install/launch
commands target the emulator serial explicitly. An emulator already running
at the requested serial is rejected rather than adopted. State, AVD files,
`adb.log`, `emulator.log` and `scrcpy.log` live under `$XDG_STATE_HOME/cyclone-padtool/emulator`, falling
back to `~/.local/state/cyclone-padtool/emulator`. GPU rendering uses
SwiftShader, while KVM accelerates the CPU by default. `--software` also
disables CPU acceleration and gives first boot a longer timeout.

The normal emulator window uses `VirtioTablet` for native wheel events without
pointer capture. The script sets `QT_QPA_PLATFORM=xcb` for the bundled Qt,
overriding any inherited Wayland selection. With `--scrcpy`, the emulator's Qt window is disabled
and scrcpy forwards SDK pointer/wheel events. scrcpy stays in the Nix
dependencies whichever display mode is selected. `--headless` and `--scrcpy`
are mutually exclusive. Stop and restart the launcher to change display modes;
**r + Enter** reloads only the APK.

To keep the emulator floating in Sway, add to `~/.config/sway/config`:

```sway
for_window [title="^Android Emulator"] floating enable
```

Reload with `swaymsg reload`, then open the emulator.

If a launcher is still active, the lock error includes the lock path and
`fuser` process information. Stop the earlier launcher before retrying. Older
launcher versions could leak their lock descriptor into a child process;
stop that reported process once if it is still running. The updated
supervisor never passes the lock descriptor to its children. The empty
`launch.lock` file remains as a stable coordination file; an existing file by
itself does not mean a lock is held.

To stop/check Gradle on the host, from `android/`:

```sh
nix develop ..#android --command bash -c './gradlew --stop'
nix develop ..#android --command bash -c './gradlew --status'
pgrep -af '[G]radleDaemon|[K]otlinCompileDaemon'
```

`--stop` covers this Gradle version; `pgrep` checks processes across versions.
No `pgrep` output means neither named daemon is running. Entries marked
STOPPED in Gradle's registry are historical, not live processes.

Launcher lifecycle tests (Linux, Python 3 and util-linux; no emulator/USB):

```sh
python3 android/tools/test_launcher.py
```

These use fake build/emulator/adb processes with real process groups, signals,
locks, sockets and a terminal. They cover interrupted builds, Ctrl-C after
launch, Qt startup failure/retry, process descendants, temporary-directory
cleanup, terminal reload/quit and propagation of `QT_QPA_PLATFORM`.

## Build and install

With JDK 17, Android SDK platform 35, build-tools 34.0.0, NDK 27.2.12479018
and CMake 3.22.1 installed:

```sh
cd android
export ANDROID_HOME=/path/to/android-sdk
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
adb install -r ../build/android/app/outputs/apk/debug/app-debug.apk
```

Alternatively, from `android/` on Nix:

```sh
nix develop ..#android --command bash -c './gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug'
nix develop ..#android --command bash -c 'adb install -r ../build/android/app/outputs/apk/debug/app-debug.apk'
```

The shell uses the top-level flake.lock pin and accepts the SDK license for
the selected Android packages. Gradle 8.9 (including its download checksum),
AGP 8.7.3, Kotlin 2.0.21 and the Android SDK/build-tools versions are pinned in
the project. Android Studio can also open this directory directly.

The debug APK is signed with the local Android debug key and can be sideloaded.
Builds disable the persistent Gradle daemon by default and run the Kotlin
compiler in the build process. Gradle may fork a single-use JVM to satisfy its
memory settings; that process exits when the build finishes.
`assembleRelease` produces an optimized unsigned APK; production signing keys
are not included. There are no runtime dependencies beyond Kotlin's standard
library and Android. The app asks for USB device access and Bluetooth (scan and connect; before
Android 12, location, which scanning needed then); not storage or network.

## Checks and boundaries

`testDebugUnitTest` covers the USB Bulk-Only Transport framing (`BulkScsi`)
and the tool device's descriptor sizes (`ToolHid`). `make web-test` runs the
page's side of the bridge (`web/test/android.test.mjs`) against a fake app.

`nix develop .#android --command make android-check` (from the repository root)
builds the debug APK and instrumentation APK, runs JVM unit tests and lint. Make also works with an externally configured Java and Android
SDK/NDK environment; the Android targets do not invoke Nix.
`make dist-android` packages the firmware ZIP and debug APK in `build/dist/`.
`nix flake check` also exercises the real JNI bridge on a
host JVM with fake endpoint callbacks and `-Xcheck:jni`. Regenerate its synthetic instrumentation package with
`python3 android/tools/generate_native_fixture.py`. Android instrumentation
uses the same fake endpoints with the actual NDK library; it never accesses USB.
To run it on an already-running test emulator:

```sh
nix develop .#android --command adb -s emulator-5580 install -r build/android/app/outputs/apk/androidTest/debug/app-debug-androidTest.apk
nix develop .#android --command adb -s emulator-5580 shell am instrument -w dev.jollymaam.app.test/androidx.test.runner.AndroidJUnitRunner
```

Set `ADB_SERVER_SOCKET=tcp:127.0.0.1:6580` for the launcher's default private adb
server (adjust both ports when using a different emulator port). Install the main
APK first; `android/run-emulator` does that automatically. Phone USB behavior and
physical firmware writes still require hardware validation.

These checks touch no phone USB stack or radio: USB access, the GATT link
and flashing still need validation on real Android hardware.
