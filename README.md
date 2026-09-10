# Cyclone 2 firmware

A standalone SDK-based firmware project for the Gamesir Cyclone 2 (Jieli
BR23). This repository owns its application sources, configuration, startup
wrapper, build graph, and packaging. The Jieli SDK is a pinned, read-only
dependency supplying headers, libraries, and a small set of unmodified BR23
platform sources.

It does **not** patch, copy, or build inside the SDK's `apps/hid` project.

## Current behavior

- Home + Start at boot enters MaskROM `UBOOT1.00`, only when USB/VBUS is
  present.
- The same pre-SDK check executes following a soft-off wake/reset.
- M (PB7) shows magenta for one second and calls the SDK's
  `power_set_soft_poweroff()`.
- Home (PB11) is configured as the SDK soft-off wake pin.
- Solid red means the standalone application/control task is running.
- LED SPI baud is 2 for the SDK's 24 MHz clock. The earlier baud value 5 was
  calibrated in the approximately 48 MHz MaskROM/RAM-loader environment and
  likely explains the nearly-white result.

Bluetooth is not started by this baseline yet. Its purpose is to establish a
small, understandable SDK/FreeRTOS/application/power foundation before adding
our own HID implementation.

## SDK application dispatch

`REGISTER_APPLICATION(name)` places a `struct application` into the linker's
`.app` section. It is registration data, not a task and not a conventional
function call.

At runtime, `start_app(&intent)` searches that linker-built table using the
intent's name/action. The SDK app core then calls the selected application's
`state_machine` operation with transitions such as `APP_STA_CREATE` and
`APP_STA_START`. System key, Bluetooth, and device messages are delivered to
the active application's `event_handler` operation.

This project demonstrates that flow directly:

- `src/app_main.c` creates an intent and calls `start_app()`.
- `src/application.c` registers the `cyclone2` application and owns its state
  machine/event handler.
- `APP_STA_START` starts the `c2_control` task.
- `src/cyclone2_control.c` currently owns recovery, LEDs, and the sleep test.

The separation becomes useful once Bluetooth is added: application lifecycle
and event routing stay in `application.c`, while sampling/reporting can live in
dedicated tasks or modules.

## Build

Expected workspace dependencies:

- SDK: `/work/github.com-Jieli-Tech/fw-AC63_BT_SDK`, commit
  `fdc018de81629e21e594d677cf05f87d288be0f0`
- PI32 toolchain: `/work/jieli-linux-toolchains-20250324.1/pi32v2/bin`
- firmware tools: `/work/github.com-kagaimiq/jl-misctools`
- stock 3.52 container template: `/work/firmware/fw_3.52.bin_unpack`

Run:

```sh
./build.sh
```

Generated files remain under ignored `build/` and `dist/`. The build checks
the linked `_start` address, constructs the BR23 load image, packs it into the
known 3.52 container, unpacks it again, and compares the replacement and
preserved files.

## First hardware test

1. Flash `dist/cyclone2-sdk-fw.bin` using the tested update path.
2. A normal start should reach solid red.
3. Press M: LEDs should show magenta for one second and go dark in soft-off.
4. Press Home: the chip should wake/reset and return to solid red.
5. With USB connected, boot or wake while holding Home + Start: it should
   enumerate as `BR23UBOOT1.00`.

If USB is absent, Home + Start intentionally continues normal boot rather than
leaving MaskROM waiting invisibly for a cable. Keep pinhole reset and the JL
forced-USB recovery method available while testing new flash builds.
