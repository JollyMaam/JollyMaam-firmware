# Working on this repository

Open-source firmware for three game controllers and their dongles, sharing
one core:

| Product | Chip | Notes |
|---|---|---|
| GameSir Cyclone 2 pad and dongle | Jieli BR23 | the main target; IMU, audio |
| GameSir Nova 2 Lite pad | Jieli BR23 | no IMU, no audio |
| GameSir Nova 2 Lite dongle | Jieli BD19 (AC632N) | 64 KiB RAM, very tight |
| 8BitDo Ultimate 2C Wireless | Telink TLSR8208 (B80) | 16 KiB SRAM; `controller_lite.c` |

The Jieli boards run the unmodified Jieli SDK runtime, our own C Bluetooth
controller (`src/platform/jieli/btctrler/`; the BD19 links the SDK's
controller library instead) and upstream BTstack.  The 8BitDo runs on the
unmodified Telink B80 SDK and its link layer, with our own LE host on top
(BTstack's ATT database code only).

## Where things are

- `src/core/`: the controller loop (`controller.c`; `controller_lite.c` on
  the 8BitDo), input, calibration, motion, settings, Home gestures and LED
  scenes (`settings_ux.c`), remap, LEDs, the tools' USB protocol (`usb.c`).
  No SDK or BTstack here.  `pad.h` is the contract between modules: its
  section comments describe every interface.  `link.h` is the pad–dongle
  link protocol.
- `src/profiles/`: what the pad pretends to be (Switch, Switch 2, PlayStation,
  Xbox, generic HID, the dongle link).  One `struct profile` each.
- `src/platform/jieli/`, `src/platform/telink/`: the chip layer (`plat_*`),
  USB device, HCI glue, linker-script additions (`sdkconfig/`).
- `src/boards/<product>/`: pins and board hooks, the BTstack-facing `bt_*.c`,
  the dongle's own loop (`dongle*.c`), `btstack_config.h`.  The Nova reuses
  the Cyclone's `bt_pad.c`, `dongle*.c` and `btstack_config.h`.
- `src/updater/`: the updater every Jieli and Telink build boots through
  (rewrites the app in place over USB, or LE on the pads).
- `make/`: one `make/<product>.mk` per product.  Its header comment lists the
  targets; `jieli.mk` and `telink.mk` document the per-board variables of
  their board templates.  The top `Makefile` lists the global targets.
  `EXTRA_DEFINES=...` adds `-D` flags to a build (test and debug builds).
  `O=dir` builds somewhere else; per-board variables can be overridden on the
  command line (e.g. an empty `NAME_MAIN_AT` for a build without updater).
- `tools/padtool/`: `padtool.py` (CLI; `--help` lists every command), `padgui.pyw`
  (Tk GUI), the transports.  `tools/padtool/README.md` for setup.
- `tools/jollymaam/`: `jollymaam.c` (host updater, Jieli image packer and flasher, built into
  `build/host/`), its tests and release packaging helpers.
- `tools/`: `8bitdoboot.py` (the 8BitDo boot loader), `upgrade.py`
  (every connected device), `padhub.c` (Linux UHID bridge for the dongle's
  hub, `tools/padhub.md`), `sdlpad.c` (what SDL sees), `linkbench.py`,
  `buildcommits.sh` (build a range of commits, for bisecting),
  `stackdepth.py` (the 8BitDo's deepest stack use, from its disassembly),
  `tools/padtool/padtool_dist.py` (`make padtool-dist`, make/padtool.mk: the Windows and Linux zip of padtool, bleak,
  the RAM loaders and every board's firmware; `tools/padtool/padinstall.py` is its
  first-install flasher).
- `external/`: the SDKs and BTstack as submodules, `fetch-toolchains.sh` for
  the compilers.  BTstack is our fork of upstream plus bug fixes only.
- `docs/`: hardware and firmware reference, see `docs/README.md`.
  `docs/firmware/` is this firmware's own design (build, image tool, updater,
  runtime RAM and flash, remapping).  Reverse-engineering notes are `re-*.md`.

## Architecture

- **Event-driven loop.**  `ctl_main()` sleeps until an event or a deadline,
  handles what happened, and sleeps again.  It does not poll on a period.
  The controller owns every decision and the pad's state.
- **Two threads on the Jieli pads.**  Control runs `ctl_main` and every module
  except `bt_pad.c`.  Bluetooth runs BTstack, `bt_pad.c` and the profiles'
  `read()`.  Control to Bluetooth: the input slot and the message queue.
  Bluetooth to control: the event queue (`bt_poll`).  Host reads are answered
  on the Bluetooth thread, so `read()` returns only constant data.
- **Modules do not call each other.**  Exceptions: a profile sends to the
  host (`host_send`), the transport asks the running profile to answer a host
  read, and `usb.c` reads and writes settings.  Everything a profile needs
  arrives in `struct pad`, so the dongle can run the same profiles on its USB
  port from the pad's state.
- **Slots and transports.**  A slot is a saved connection: a profile type,
  a transport (Bluetooth, USB, our dongle, or USB-else-wireless), a bond and
  an address.  Remap slots are separate and apply on top of any connection.
- **Newest wins.**  Input is never queued, buttons included.  A dropped report
  beats a late one.  Do not add queues for reports or button changes.
- **Configuration as data.**  Tunable behaviour lives in readable `const`
  tables in C: Home gestures (`gestures[]`, `struct gesture` in
  `src/core/settings_ux.c`), timeouts and LED scenes (same file), setting
  defaults and the palette (`settings.c`), profile descriptors
  (`profile_*.c`), pins (`board_*.c`).  No code generation; a generator, if
  ever needed, is host-compiled C.  Settings changes apply on the next
  connect.
- **Console wake only from Home.**  A pad wakes a console only from a Home
  gesture (or `padtool connect`), never from another button or power-on.
- **Tool device.**  Outside a game-controller session every board is a HID
  "tool device" (V-USB's shared ID 16c0:05df, manufacturer "JollyMaam"),
  which `padtool` and `jollymaam` talk to.  The protocol is documented at the
  top of `src/core/usb.c`.  Our own IDs are in `pad.h`
  (`USB_OWN_*`); the generic gamepad is 16c0:27dc, the Xbox 360 personality
  Microsoft's 045e:028e.
- **Updater.**  Every pad and dongle build is the updater with the app above
  it (`docs/firmware/updater.md`).  The updater starts the app unless the app
  asked for it (reset mailbox) or the app's header is missing.  On the Jieli
  chips it also stays after three app starts that never reached the control
  thread.

## Code style

- C99 for everything shared: the Telink compiler is GCC 4.5.  `pad.h`
  provides `_Static_assert`.
- Public names carry the module prefix: `bt_`, `ux_`, `set_`, `in_`, `led_`,
  `util_`, `plat_`, `usb_`, `upd_`.  Only `plat_*` and `platform_jieli_sdk.c`
  use Jieli SDK names.  Our additions inside the controller carry
  `btctrlermod_`.  Words used consistently: slot, profile, session, link,
  host, report, write, read.
- Comments are short plain sentences saying what a thing is or why, at about
  the density of the surrounding code.  Match the neighbouring code's naming
  and idiom rather than introducing a new style.
- Tests: a module with logic has an `#ifdef UNITTEST` block at its end, run
  on the host by the `-test` targets.  Not covered: `controller.c`,
  `bt_pad.c`, `btstack_hci_jieli.c`, `platform_jieli*.c`.  Python tools have
  `test_*.py` beside them (the GUI tests need Tk: use a Nix shell with
  `tkinter`, and `xvfb-run` for a display).
- The SDKs stay unmodified (their linker scripts are patched at build time
  in `jieli.mk`).  BTstack stays upstream: bug-fix patches only, carried on
  the fork.
- Keep `pad.h`'s section comments in step with interface changes.
- Commit messages: `Area: what changed` as the subject, then prose on why
  and what was tested on which hardware.

## Docs

- What an agent writes in `docs/` is proven fact only: no test results,
  dates or plans.  Mark an unproven point "not established".  Humans may
  keep other files there (plans, discussion); leave those to them.
- Source may refer to `docs/`; `docs/` does not refer to source.
- A chip or SDK fact goes in `docs/jieli/`, a board fact in that board's
  folder, a choice this firmware makes in `docs/firmware/`.
- How to put a stock pad into its loader and install this firmware belongs in
  the per-board docs, written by a human.  Do not write it.

## Hardware and testing

- The user handles the physical devices: plugging, buttons, consoles, jigs.
  Building and running the tools are ours.  When a physical action is
  needed, ask plainly, as numbered steps.
- **Ask before flashing any device, and before changing anything it keeps
  over a reboot** (settings, pairing, calibration, a factory reset).  Say
  which device and what will change.  Reading (status, log, settings) and
  ephemeral changes need no permission.  `padtool update` is flashing too.
  Never run two flashers at once.
- Device nodes: the tools look in `/dev` (`/dev/input` for evdev), or, when
  `DEV_ROOT` is set, for every node in that one directory (a container that
  exports its nodes there).  Programs that only look in the standard places
  (SDL) run under `tools/withdev.sh`, which rebuilds `/dev` from `DEV_ROOT`
  once at start: nodes that appear later are not in it.
- Several devices of one kind are told apart by USB port:
  `padtool --device 5-2.1`, `jollymaam jieli scan`.  Use `padtool --dongle` for
  dongles.
- **Ephemeral mode for tests.**  Before changing settings or pairing just to
  test something, put every device involved in `padtool ephemeral` (and
  `padtool --dongle ephemeral`; check that each command succeeded): nothing
  is saved until the next boot.  **When the test is done, reboot every
  device you made ephemeral** (`padtool reboot`) before reporting, so they
  are back on their saved settings.  Only save changes the user wants kept.
- While a pad or dongle is a game controller on USB, its tool device is
  gone.  A HID personality comes back with `padtool tool HIDRAW_PATH` (it
  takes paths, not `--device`; without one it tries every hidraw).  The Xbox
  360 personality is not HID: end the session on the pad (the disconnect
  gesture in `gestures[]`) or replug.  A dongle with a pad linked shows that
  pad's personality.
- Logs: `padtool log` reads the firmware's log ring, which survives a soft
  reset (and the updater shows the app's lines).  A hang watch soft-resets
  after about 3 s and logs where it was.  `padtool stats` gives loop, report
  and link timing.
- Debug switches (via `EXTRA_DEFINES`): `DEBUG_UART_DP` (and `DEBUG_NO_USB`)
  puts a UART on USB D+ for the BD19 bring-up jig; `HCI_LOG`, `BT_SMP_LOG`,
  `PATH_STATS`, `LOG_LINES`/`LOG_TEXT` (log size), `USB_SESSION_TEST_MS`,
  `STAY_ON_TEST`, `JACK_FLIP_TEST`, `BOOT_HANG_TEST` (8BitDo: a start-up
  hang for the updater's start count).  Grep for each to see what it does.
  The 8BitDo's test builds take make variables instead: `U2CW_PROFILE=1`
  (`padtool profile`) or `U2CW_PROBE_EDGES=6` (`padtool latency`), one at a
  time.
- A device that does not come up after flashing: the ROM loader is the way
  back (per-board docs).  Before asking the user for a loader cycle, prepare
  the next experiment so one cycle answers the question.

## Pitfalls found the hard way

When something costs real effort to find out (a chip quirk, a tool trap, a
wrong assumption), add it here in a line or two.

- BR23 LE TX slots (`RF_ble.c`, comment above `tx_staged`): an empty PDU
  sent from a completed current slot does not move TXTOG when acknowledged,
  data written into that slot while it waits is dropped, and a NACKed empty
  PDU resends the slot's stale descriptor.  Breaking any of the three rules
  there gave input stalls with MIC failures under duplex traffic.

- RAM is the limit on the BD19 dongle: the linker's "NVRAM space overflow"
  is static RAM, and a heap too small for the controller makes the app hang
  at start-up without ever enumerating.  Check `_HEAP_BEGIN`/`_HEAP_END` in
  `app.map` after adding static data there.  `HUB_SLOTS` is per board for
  this reason.
- The Classic local name (a profile's `.name`) fits 63 bytes in the BR23
  controller (`local_name[64]`; the SDK had 32).  A longer name asserts
  "local name length" when the session starts.
- LTO deletes stores to a global nothing reads (a reset mailbox, say) unless
  its address escapes into a call or an asm input.
- Never build the Telink board with `-Os`: the compiler miscompiles switch
  tables.
- The 8BitDo's stack has under 100 bytes to spare.  A function's frame holds
  the locals of all its branches, so a big array in one `case` costs every
  case (a 192-byte one in `usb_command` overflowed into .bss).  Keep it in a
  `noinline` helper; `make 8bitdo-ultimate2c-wireless-pad-stack` checks.
- 8BitDo, 2.4 GHz (rf8_telink.c): the SDK's BLE radio set-up leaves
  0x800401 bit 3 set, and generic FSK then never hears a reply.  The FSK
  set-up in turn changes registers BLE's never writes again (the RF
  interrupt mask, packet format, state machine, the radio's RX DMA): save
  and restore them, or BLE advertises but no host can connect.
- padtool latency's clock mapping assumes symmetric USB round trips: a
  command answered only at the end of a control-loop pass skews it by up to
  half the round trip (the 8BitDo answers CLOCK at once since).
- Shell scripts: initialise every option variable empty up front; the
  environment can leak names like `out` into a script.
- The 8BitDo app keeps 2 log lines (`LOG_LINES`); a debug build can take
  `EXTRA_DEFINES=-DLOG_LINES=5`, 12 overflows RAM.  Its updater's launch of
  the app clears the watchdog, so arming it in the updater does nothing.
- BlueZ (Linux) waits 2 s before it disconnects an LE link on request.
  When the pad ended an LE link instead, the kernel sometimes held the next
  scan or connection for 20 s (cause not found).  padble therefore leaves
  config mode's link up between runs, on Linux only: bleak's WinRT backend
  keeps a connection (maintain_connection), and Windows then connects to
  anything that comes up on that address, so it never appears in a scan.
- 8BitDo: work polled from the wait loop advances once per pass, and with
  a busy link (Switch 2, 5 ms interval) passes come up to ~15 ms apart.  A
  key-matrix scan done one row step per pass took 64 ms and the debounce
  dropped quick taps; anything with several steps should finish in one go.
- Never search the whole filesystem (`/`, `/proc`, `/sys`, `/dev`): it has
  disturbed the host.
- BR23 VBAT: the SDK truncates its 16-sample average to an ADC code before
  converting voltage. It normally refreshes one sample about every 20 s;
  roughly 12 mV steps and minutes of smoothing do not imply a scaling error.
