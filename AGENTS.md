# Working on this repository

GameSir Cyclone 2 firmware: the Jieli BR23 SDK runtime, an owned C Bluetooth
controller, and upstream BTstack (`/work/cyclone2-btstack`, unmodified `master`).

- Hardware and SoC reference: [docs/](docs/README.md). Reverse-engineering
  notes are `docs/re-*.md`.
- Module contracts: the section comments in `src/pad.h`.

## Working rules

- The user operates the physical pad and console. Building and flashing are
  ours. Say plainly when a physical action is needed.
- Commit at a logical checkpoint.
- Say "the app" or "the SDK" when distinguishing code ownership.
- `docs/` holds proven hardware facts only: no test results, dates or plans.
  Mark anything unproven as "not established".
- Source may refer to `docs/`. `docs/` does not refer to source.
- Keep the `src/pad.h` section comments in step with interface changes.
- No root here. Use the devices in `/dev/bwrap`; do not try to leave the container.

## Design

- **Event-driven loop.** `ctl_main()` sleeps until an event or a deadline, then
  does the work for what happened. It does not run on a period. The controller
  owns every decision and the pad's state.
- **Mostly stateless modules.** Modules do not call each other, except that a
  profile may send to the host (`host_send`, over the session's transport) and
  `bt.c` or the platform's USB asks the running profile to answer a host read.
- **Transports.** A slot's `transport` is Bluetooth, USB (the profile's
  `struct usb_identity`, built by `platform_jieli_usb.c`), USB while a USB host
  is there, or our dongle.  A profile builds the USB formats when
  `profile_session.usb` is set.  The dongle runs the same profiles on its USB
  port from the pad's state (`link.h`, `profile_link.c`, `dongle*.c`).
- **Two threads.** Control runs `ctl_main` and every module except `bt.c`.
  Bluetooth runs BTstack, `bt.c` and the profiles' `read()`. Control to
  Bluetooth: the input slot and the message queue. Bluetooth to control: the
  event queue (`bt_poll`). Host reads are answered on the Bluetooth thread, so
  `read()` returns only constant data.
- **Newest wins.** Input is never queued, buttons included. A lost report beats
  a late one.
- **Naming.** Public names carry the module prefix (`bt_`, `ux_`, `set_`, `in_`,
  `led_`, `util_`, `plat_`, `usb_`). Only `plat_` and `platform_jieli_sdk.c` use
  Jieli names. Words: slot, profile, session, link, host, report, write, read.
- **Configuration as data.** Tunable behaviour is a `const` table: Home gestures,
  timeouts and LED scenes (`settings_ux.c`), profile descriptors (`profile_*.c`),
  setting defaults (`settings.c`), pins (`platform_jieli.c`).
- **Tests.** A module with logic has an `#ifdef UNITTEST` block at its end.
  `make test` runs them on the host. Not covered: `controller.c`, `settings.c`,
  `bt.c`, `btstack_hci_jieli.c`, `platform_jieli*.c`.

## Build and test

    make -j32            # pad: build/pad/
    make test
    make pad-flash       # over USB, through the ROM loader
    make dongle          # build/dongle/
    make dongle-flash    # packed for the connected dongle's key
    make -j32 pad dongle # both boards

`make BOARD=dongle [flash|clean|test]` also works. Board differences (flash
size, code limit, PID) are at the top of the `Makefile`.

`padtool/padtool.py` talks to the running firmware over USB vendor commands
(`padtool --help`). `padtool --dongle` does the same for the dongle.  While a
pad or dongle is a game controller on USB, the vendor commands are gone:
`padtool tool` (feature report 0xf5 through hidraw, HID personalities only)
or ending the session brings the tool device back.  A test build that ends
USB sessions by itself: `make BOARD_DEFINES_pad=-DUSB_SESSION_TEST_MS=12000`
(a change of `BOARD_DEFINES` rebuilds everything).

`padtool/padgui.pyw` is the Tkinter GUI (`python padtool/padgui.pyw`), sharing the
padtool protocol and Linux/Windows SCSI transport. `padtool/padflash.py` backs up
and flashes prepared images in Python using the SDK BR23 RAM loader (or a packaged
copy beside the scripts). Device discovery runs once per second; only new or
changed devices reload settings. Edits stay pending across navigation until Apply.
Pad diagnostics live in Debug tools; dongle and ROM-loader actions live on their
main device pages. `tools/bootloop.py` is a development stress-test helper. Sensor
calibration is global; the USB trigger requires protocol 3+. Protocol 4 adds
a read-only defaults query for Reset and default-value indicators. Protocol 5
uses 15-point curves plus enable flags; presets and migration are shared in
`padtool/padcurves.py`. Protocol 6 adds the staged slot Unpair action (WRITE
id 0xfe, length 0): firmware clears the bond and renews the address at COMMIT.
GUI Unpair and `padtool unpair SLOT` share this action. Setup and host
tests: `padtool/README.md`.

`tools/sdlpad.c` shows what SDL makes of the gamepads it sees (build line at
its top).  In this container SDL needs the nodes from `/dev/bwrap` under
`/dev/input` and `/dev`: `SDL_JOYSTICK_DISABLE_UDEV=1 unshare -rm
tools/withdev.sh build/host/sdlpad 5`.

Recovering a pad that does not enumerate: hold Home and Start while plugging in
USB (ROM loader, `UBOOT1.00`), then `make pad-flash`. For the dongle, hold its
button while plugging it in, then `make dongle-flash`. Image layout and the
`jlnewfw` tool are in [docs/re-application-image.md](docs/re-application-image.md).

Tunable by hand: Home gestures, timeouts and LED scenes in `src/settings_ux.c`;
setting defaults and palette in `src/settings.c`. At run time: `padtool set`.

The controller in `src/btctrler/` links directly; no controller archive is
built. Recovery tooling is archived on the `btctrler-decompiled` branch. The
SDK is unmodified.
