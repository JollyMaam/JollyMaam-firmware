# Cyclone 2 firmware

A standalone SDK-based firmware project for the Gamesir Cyclone 2 (Jieli
BR23). This repository owns its application sources, configuration, startup
wrapper, build graph, and packaging. The Jieli SDK is a pinned, read-only
dependency supplying headers, libraries, and a small set of unmodified BR23
platform sources.

It does **not** include or build anything from the SDK's `apps/hid` project.
The SDK dependency is currently limited to public headers, `cpu.a`, `system.a`,
`cpu/br23/setup.c`, `cpu/br23/charge.c`, and `cpu/br23/led_spi.c`.

## Current behavior

- Home + Start at boot enters MaskROM `UBOOT1.00`, only when USB/VBUS is
  present.
- The same pre-SDK check executes following a soft-off wake/reset.
- M (PB7) shows magenta for one second and calls the SDK's
  `power_set_soft_poweroff()`.
- Home (PB11) is configured as the SDK soft-off wake pin.
- The stock initcall dispatcher is retained. Yellow marks the early-init phase;
  blinking green proves all retained early initcalls returned and execution
  reached the platform-init checkpoint. Runtime VFS/JLFS mounting is disabled:
  the prebuilt `sdfile_new.c.o` scanner did not accept this packed application
  layout. Three small helpers needed by VM/BTIF are supplied in `src/config.c`,
  which prevents that object and its automatic `sdfile_init` initcall from
  being linked. The build rejects any future image that restores that initcall.
- The project overrides the release SDK's otherwise non-reporting
  `exception_analyze()` while retaining its `startup.S` register-save entry.
  Exceptions remain visible as a cause color followed by a saved-PC class.
- During bring-up, the SDK's four-second watchdog is explicitly disabled after
  `sdk_setup_arch()` returns. Re-enable it only after the minimal idle path has
  been shown to clear it reliably.
- LEDs use the unmodified CPU-level SDK `cpu/br23/led_spi.c` driver. It derives
  its reset/data dividers from `clk_get("lsb")` for 1 MHz and 8 MHz wire rates;
  the application does not assume MaskROM's clock or program SPI registers.
- `src/board.h` is the central map of every button, trigger ADC,
  I2C peripheral, USB-sense input, and LED-chain pin identified so far. Runtime
  code uses the SDK GPIO API and its `IO_PORTx_xx` identifiers; only the
  pre-SDK recovery check accesses GPIO registers directly.

Bluetooth is not started by this baseline yet. Its purpose is to establish a
small, understandable SDK/FreeRTOS/application/power foundation before adding
our own HID implementation.

## How this SDK application boots

There are four distinct layers:

1. MaskROM and `uboot.boot` validate/load the packed `app.bin`, then enter the
   SDK `_start` symbol.
2. The startup code establishes RAM and calls `setup_arch()`. Our wrapper first
   performs the raw Home+Start recovery check, then calls the unmodified SDK
   setup for memory, clocks, interrupts, timers, and FreeRTOS.
3. The SDK starts its fixed tasks and calls our `app_main()`. We submit an
   `intent` named `cyclone2` to the SDK application dispatcher.
4. `start_app()` finds the `struct application` placed in the linker-built
   `.app` table. Its `APP_STA_START` callback creates `c2_control`, the ordinary
   FreeRTOS task that owns the current LED and M-button behavior.

Three project-owned configuration surfaces support that flow:

- `src/app_config.h` contains compile-time feature, clock, power, and flash
  layout policy used while compiling sources and preprocessing `sdk_ld.c`.
- `src/config.c` supplies the runtime global-symbol ABI expected by the
  prebuilt libraries: logging switches, assertions, VM sizing, and the BTIF
  syscfg table. The table currently keeps Bluetooth MAC/frequency calibration
  IDs in BTIF; unregistered writable IDs fall back to VM.
- `src/app.ld` and `src/app_overlay.ld` provide the application/overlay section
  layout that the generic BR23 linker script expects.

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

- `src/main.c` creates an intent, registers the `cyclone2` application, and
  owns its state machine/event handler.
- `APP_STA_START` starts the `c2_control` task.
- `src/control.c` owns LEDs and the sleep test; the pre-SDK recovery path is in
  `src/main.c`.

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
make
```

Generated files remain under ignored `build/` and `dist/`. The build checks
the linked `_start` address, constructs the BR23 load image, packs it into the
known 3.52 container, unpacks it again, and compares the replacement and
preserved files. It also generates `build/app-disasm.lst`: a complete,
searchable application disassembly with local/global symbols and available
source-line annotations. The analysis copy is `dist/app-disasm.lst`; the legacy
name `dist/sdk.lst` is retained for existing notes and tools. Useful narrower
targets are `make elf`, `make analysis`, and `make verify`.

## First hardware test

1. Flash `dist/cyclone2-sdk-fw.bin` using the tested update path.
2. A normal start should reach a dim red Home LED, with the other LEDs dark.
3. Press M: LEDs should show magenta for one second and go dark in soft-off.
4. Press Home: the chip should wake/reset and return to solid red.
5. With USB connected, boot or wake while holding Home + Start: it should
   enumerate as `BR23UBOOT1.00`.

If USB is absent, Home + Start intentionally continues normal boot rather than
leaving MaskROM waiting invisibly for a cable. Keep pinhole reset and the JL
forced-USB recovery method available while testing new flash builds.
