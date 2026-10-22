# Working on this repository

GameSir Cyclone 2 firmware: the Jieli BR23 SDK runtime, an owned C Bluetooth
controller, and upstream BTstack (`/work/cyclone2-btstack`, unmodified `master`).

- Reference: [docs/](docs/README.md). `docs/jieli/` is the BR23 chip and SDK,
  `docs/gamesir-cyclone2/` the pad and dongle boards and stock firmware,
  `docs/firmware/` this firmware's own software. Reverse-engineering notes are
  `re-*.md`.
- Module contracts: the section comments in `src/core/pad.h`.
- Sources: `src/core/` (the controller loop, input, settings, Home gestures,
  LEDs, remap, the tools' protocol; no SDK or BTstack), `src/profiles/`,
  `src/platform/btstack/` (`bt.c`: the `bt_` interface on BTstack, for a chip
  with an HCI controller), `src/platform/gamesir/dongle/` (the dongle's loop),
  `src/platform/jieli/` (the BR23: platform, `board.h`, HCI glue, `sdkconfig/`,
  our controller in `btctrler/`), `src/boards/<product>/` (pins, board
  hooks and `btstack_config.h`, for one platform).

## Working rules

- The user operates the physical pad and console. Building and flashing are
  ours. Say plainly when a physical action is needed.
- Commit at a logical checkpoint.
- Say "the app" or "the SDK" when distinguishing code ownership.
- `docs/` holds proven facts only: no test results, dates or plans.
  Mark anything unproven as "not established".
- Source may refer to `docs/`. `docs/` does not refer to source.
- Keep the `src/core/pad.h` section comments in step with interface changes.
- No root here. Use the devices in `/dev/bwrap`; do not try to leave the container.

## Design

- **Event-driven loop.** `ctl_main()` sleeps until an event or a deadline, then
  does the work for what happened. It does not run on a period. The controller
  owns every decision and the pad's state.
- **Mostly stateless modules.** Modules do not call each other, except that a
  profile may send to the host (`host_send`, over the session's transport),
  `bt.c` or the platform's USB asks the running profile to answer a host read,
  and `usb.c` (the tools' commands) reads and writes settings itself.  What a
  profile needs from other modules (the gyro bias included) comes in
  `struct pad`, so the dongle can run the same profiles.
- **Transports.** A slot's `transport` is Bluetooth, USB (the profile's
  `struct usb_identity`, built by `platform_jieli_usb.c`), USB while a USB host
  is there (else Bluetooth), our dongle, or USB while a USB host is there (else
  the dongle).  A profile builds the USB formats when
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
  setting defaults (`settings.c`), pins (`board_pad.c`, `board_dongle.c`).
- **Tests.** A module with logic has an `#ifdef UNITTEST` block at its end.
  `make test` runs them on the host. Not covered: `controller.c`, `bt.c`,
  `btstack_hci_jieli.c`, `platform_jieli*.c`.

## Build and test

    make -j32                              # every product
    make test                              # every product's unit tests
    make -j32 gamesir-cyclone-2-pad        # build/gamesir-cyclone-2-pad/
    make gamesir-cyclone-2-pad-flash       # over USB, through the ROM loader
    make -j32 gamesir-cyclone-2-dongle     # build/gamesir-cyclone-2-dongle/
    make gamesir-cyclone-2-dongle-flash    # packed for the connected dongle's key
    make -j32 gamesir-cyclone-2-all        # both boards
    make gamesir-cyclone-2-pad-test        # one board's unit tests

The build is in `make/`: `common.mk` and `host.mk` are shared, `jieli.mk`
holds the BR23 toolchain, SDK and the board template (`jieli_board`),
`btstack.mk` BTstack, and each product's `make/<product>.mk` its boards
(flash size, PID, sources, defines) and targets.

`padtool/padtool.py` talks to the running firmware over USB vendor commands
(`padtool --help`). `padtool --dongle` does the same for the dongle.  While a
pad or dongle is a game controller on USB, the vendor commands are gone:
`padtool tool` (feature report 0xf5 through hidraw, HID personalities only)
or ending the session brings the tool device back.  A test build that ends
USB sessions by itself: `make gamesir-cyclone-2-pad EXTRA_DEFINES=-DUSB_SESSION_TEST_MS=12000`
(a change of a board's defines rebuilds that board).

`padtool/padgui.pyw` is the Tkinter GUI (`python padtool/padgui.pyw`), sharing the
padtool protocol and Linux/Windows SCSI transport. `padtool/padflash.py` backs up
and flashes prepared images in Python using the SDK BR23 RAM loader (or a packaged
copy beside the scripts). Device discovery runs once per second; only new or
changed devices reload settings. Edits stay pending across navigation until Apply.
Apply/Discard scope is all devices in the global toolbar, one pad on its main
page, and one target on slot/palette/calibration pages. Individual fields keep
a status button: blank is default, checkmark is saved custom, * is a staged
edit. Clicking a saved checkmark stages Default; clicking * or ! discards that
setting’s edits. The device overview uses the same buttons for slot types.
Reset all to defaults sits beside Apply/Discard and stages defaults within the
selected device or target scope. Hover explains the status/action.
Show console opens a per-device live log window (`padtool/padconsole.py`);
advanced diagnostics use the CLI. Slot Apply/Discard stay in the title header
while settings scroll. Dongle and ROM-loader actions live on their main pages. `tools/bootloop.py` is a development stress-test helper; `tools/buildcommits.sh [-q] [-o DIR] [-m MAKEARGS] [-c CMD] COMMIT|A..B...` builds each commit in a temporary worktree (output in `DIR/<date>-<hash>` with `-o`; `CMD` runs in the worktree with `$O` and `$COMMIT`). Sensor
calibration is global; the USB trigger requires protocol 3+. Protocol 4 adds
a read-only defaults query for Reset and default-value indicators. Protocol 5
uses 15-point curves plus enable flags; presets and migration are shared in
`padtool/padcurves.py`. Protocol 6 adds the staged slot Unpair action (WRITE
id 0xfe, length 0): firmware clears the bond and renews the address at COMMIT.
GUI Unpair and `padtool unpair SLOT` share this action. Protocol 7 adds LED
demos (`padtool led-demo`, the table in `src/core/settings_ux.c`). Address renewal hashes
the full previous address and tick, with no fixed prefix or generation counter.
Type changes renew identities (except through the dongle). Setup and host
tests: `padtool/README.md`.

`tools/sdlpad.c` shows what SDL makes of the gamepads it sees (build line at
its top).  In this container SDL needs the nodes from `/dev/bwrap` under
`/dev/input` and `/dev`: `SDL_JOYSTICK_DISABLE_UDEV=1 unshare -rm
tools/withdev.sh build/host/sdlpad 5`.

Recovering a pad that does not enumerate: hold Home and Start while plugging in
USB (ROM loader, `UBOOT1.00`), then `make gamesir-cyclone-2-pad-flash`. For the dongle, hold its
button while plugging it in, then `make gamesir-cyclone-2-dongle-flash`. Image layout and the
`jlnewfw` tool are in [docs/jieli/re-application-image.md](docs/jieli/re-application-image.md)
and [docs/firmware/image-tool.md](docs/firmware/image-tool.md).

Tunable by hand: Home gestures, timeouts and LED scenes in `src/core/settings_ux.c`;
setting defaults and palette in `src/core/settings.c`. At run time: `padtool set`.

The controller in `src/platform/jieli/btctrler/` links directly; no controller archive is
built. Recovery tooling is archived on the `btctrler-decompiled` branch. The
SDK is unmodified.
