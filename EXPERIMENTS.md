# SDK initialization experiments

Run `make experiments` to produce all images under `dist/experiments/`.
Flash them in numeric order and record the visible behavior before proceeding.
The first three retain the Cyclone 2 early Home+Start recovery check. Image 04
is the completely unmodified SDK example and does **not** contain that check;
test it last and keep pinhole/forced-USB recovery available.

## 01 — baseline green

`01-baseline-green.bin` is the known-good project baseline. It bypasses the
vendor `debug_init()` and does not link `sdfile_new.c.o`. Expected result:
yellow briefly, followed by blinking green.

## 02 — vendor debug, blue early marker

`02-vendor-debug-blue-early.bin` changes exactly one important behavior from
the baseline: it restores `cpu.a(debug.c.o)`'s real `debug_init()`. VFS remains
disabled. Blue is the early marker; blinking green means the vendor debug-unit
configuration is valid with the rest of our current setup.

If this faults or resets while 01 works, the debug unit has an unmet platform
assumption independent of JLFS. Record every color in order because the custom
exception handler reports a cause followed by the saved-PC class.

## 03 — minimal configuration with VFS, cyan early marker

`03-minimal-vfs-cyan-early.bin` keeps the no-op `debug_init()` but restores
`VFS_ENABLE=1`, the real helpers from `sdfile_new.c.o`, and its early initcall.
Cyan means execution reached the point immediately before vendor early
initcalls; blinking green means the JLFS scan and mount unexpectedly succeeded.

This intentionally reproduces the suspect combination. A reset/stall after
cyan confirms the failure without mixing in the debug-unit experiment.

## 04 — exact vendor AC635N HID application

`04-exact-vendor-hid.bin` is built by the SDK's own
`apps/hid/board/br23/Makefile`, with its complete source list, reference-board
configuration, real `debug_init()`, and VFS. We stop before the broken Linux
post-build script and place its resulting ELF load image into the same verified
stock Cyclone 2 container used for the other tests.

There are no Cyclone 2 LED markers or custom recovery code in this image. Its
reference UART is PA0 (a controller button), and its reference LED wiring does
not match this board. Evidence of success is Bluetooth advertising/operation;
the compiled default name is `JL_HID_DEBUG`, although preserved BTIF settings
may override it with an existing controller name.

Interpretation:

- If 03 fails but 04 runs, the stock container is adequate and our missing
  SDK configuration/registration is the likely cause.
- If both 03 and 04 fail at startup, the retained stock JLFS/INI/`BOOT_INFO`
  layout becomes the leading suspect.
- If 02 fails independently, debug initialization is a second issue and must
  be isolated from the filesystem work.

## Results from the first batch

- 01: blinking green (known-good baseline).
- 02: blinking green. The real SDK `debug_init()` is working and is no longer
  a suspect.
- 03: solid cyan without resetting. Execution entered the vendor initcall
  sequence but did not reach the platform checkpoint.
- 04: advertised as `AC695X_1(BLE)` and paired successfully. The exact vendor
  SDK application therefore boots with the preserved stock Cyclone 2
  container, JLFS layout, `isd_config`, and BTIF data.

Together these results rule out a generally broken SDK and make the standalone
project's retained initialization/configuration the problem. They do not yet
prove precisely where `sdfile_init()` stops because experiment 03 also starts
the SPI/DMA LED transport immediately before it.

## 05 — VFS without the early LED transport

`05-vfs-no-early-led.bin` is identical in purpose to experiment 03, except it
does not initialize SPI1/DMA or transmit an LED frame before `sdfile_init()`.
It stays dark during early initialization. If all initcalls return, it then
initializes the LEDs and blinks green.

- Blinking green means the early LED diagnostic disturbed the flash/VFS path.
- Remaining dark or resetting means the VFS failure is independent of the
  early LED operation, and the next experiments should bracket or replace
  pieces of `sdfile_init()` rather than copying unrelated Bluetooth configs.

The vendor Linux post-build path is not authoritative as shipped: it invokes
the script from the board directory even though the script expects `sdk.elf`
in its current directory, then requires unavailable `/opt/utils/strip-ini`,
`/opt/utils/remove_tailing_zeros`, and `host-client`. The four overlay sections
that it conditionally trims are each only four bytes of `0xff` in this HID
build, so omission of that trimming does not change this particular load image.
