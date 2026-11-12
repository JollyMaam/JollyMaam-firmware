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
- `src/boards/<product>/`: `board_pad.c` starts with the pad's description
  (its name, controls with their kind and printed label, base remap and other
  defaults), then pins and board hooks; the BTstack-facing `bt_*.c`,
  the dongle's own loop (`dongle*.c`), `btstack_config.h`.  The Nova reuses
  the Cyclone's `bt_pad.c`, `dongle*.c` and `btstack_config.h`.
- `src/updater/`: the updater every Jieli, Telink and YC3121 build boots
  through (rewrites the app in place over USB, or LE on the pads but the
  U2CB).
- `make/`: the build.  `make/lib.mk` (a copy of a separate project) builds
  every directory listed in the top `Makefile`'s `RULES` from its
  `rules.mk`; its header documents the target properties.  Each
  `src/boards/<product>/rules.mk` spells out its boards' targets (its header
  lists them) from source units: `make/sources.mk` (core and profiles),
  `make/btstack.mk`, and per platform `make/jieli.mk` (everything Jieli:
  `JIELI_COMMON_*` plus `JIELI_BR23_*` / `JIELI_BD19_*`, never filtered),
  `make/telink.mk` with `telink-lib.mk`, `make/yc3121-lib.mk`;
  `make/host-tests.mk` the unit tests.  libjollymaam, `jollymaam` and
  `padhub` have their own `rules.mk`.  `make` builds every board but the U2C
  Bluetooth (it needs `arm-none-eabi-gcc` and a stock dump); `make all`
  builds everything.
  `EXTRA_DEFINES=...` adds `-D` flags to a build (test and debug builds);
  a changed command line rebuilds exactly what it changes.  `O=dir` builds
  somewhere else.
- `tools/padtool/`: `padtool.py` (CLI; `--help` lists every command), `padgui.pyw`
  (Tk GUI), the transports.  `tools/padtool/README.md` for setup.
- `tools/jollymaam/`: `jollymaam` (built into `build/host/`; `jollymaam
  help`, `jollymaam help COMMAND`): images, packages, and every device
  operation (list, install, update, backup, reboot), on libjollymaam
  (`src/lib/`, which knows every product: `products.c`) with this platform's
  hooks.  `install` backs the whole flash up first; `--no-backup` skips it.
- `tools/`: `padhub.c` (Linux UHID bridge for the dongle's
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
- The SDKs stay unmodified.  The Jieli linker scripts are our copies of the
  SDK's (`src/platform/jieli/sdkconfig/sdk_ld_br23.c`, `sdk_ld_bd19.c`,
  changes marked "ours"); Telink's is patched at build time
  (`make/telink-lib.mk`).  BTstack stays upstream: bug-fix patches only, carried on
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
  `padtool --device 5-2.1`, `jollymaam list`.  Use `padtool --dongle` for
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
  `noinline` helper; `make u2cw/pad-stackcheck` checks (part of `make u2cw-test`).
  The check misses calls through pointers it has no rule for: the SDK's
  TX push (through a stub that looks like a call to itself) went unseen and
  an encrypted link overflowed into .bss by 80 bytes.  The profiles' session
  state shares one block (`PROFILE_STATE`, pad.h) for this room.
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
- 8BitDo Ultimate 2C Bluetooth (Yichip loader, model 1a30): write an app
  only as libjollymaam's `jm_8bitdo_yichip_init` does (erase at 0x01018000 with
  the image length as total, then the encrypted stream with that total).
  Writes without that erase are acknowledged but erase sectors that held
  data and program nothing; that lost the stock app once.  It programs only
  whole 512-byte blocks (a short last block is silently dropped), and an
  erase shorter than 32 KiB goes 32 bytes at a time (16 KiB outlasted the
  30 s timeout and kept the loader busy), so the image is padded to whole
  32 KiB blocks.
- Ultimate 2C Bluetooth (YC3121): the vectors are the ROM's.  HardFault
  goes through the ROM (which pushes r4, lr) and the loader's stub, which
  runs the loader's endless loop unless its mode register 0xf8564 is not 2
  (the app sets 1; the loader leaves 2 after flashing).  A fault that loops
  there needs a hard power-off (USB out, hold Home).  Into the boot loader
  without L1+R1: its mailbox and a jump, as `plat_restart` does; a chip
  reset clears RAM, mailbox included.  Disable and clear every NVIC
  interrupt before the jump: our USB interrupt left on and pending went to
  the loader's endless handler once it enabled interrupts (2 of 3 entries
  hung off USB until a power cycle; 25 of 25 with them cleared).  Not stock's second word 0x20210624
  while our radio runs: the loader then skips its radio start, takes the
  running core, and never came up on USB.
- Ultimate 2C Bluetooth USB: multi-packet OUT control transfers lose data
  (the controller takes the next packet while the last is in its FIFO, and
  flags bit 0 before a packet is all in), polled or from the interrupt.
  The tool device's report is therefore one 64-byte packet; answers go out
  in pieces (TOOL_MORE), on input reports per client as the other boards'.
- Ultimate 2C Bluetooth flash data pages (0x0107c000..): a CPU read of
  them (PEEK) resets the chip; read them raw through the ROM's QSPI read
  (flash_yc3121.c), as stock does.
- Ultimate 2C Bluetooth app slot: ciphered, 36 bytes of flash per 32-byte
  block, the blocks of each 32 KiB window shuffled over 0x9000 bytes of
  flash (ROM 0x503c; docs/firmware/updater.md).  CPU addresses are not
  flash offsets: the slot ends at 0x01060000, not where its raw neighbours
  start; a 4 KiB raw erase hits blocks all over a window; a block's
  bytes cannot be programmed in two goes.
- Ultimate 2C Bluetooth radio core: opcodes stock never sends are not
  safe to try: 0x27, and another from 0x29 up, wedged the pad until a
  power cycle, the second past the hang watch (a stalled bus, likely).
- Ultimate 2C Bluetooth radio core, LE: its attribute table (from
  0xc4975, bt_yc3121.c) has room for about 11 128-bit characteristics; past
  that the core stops answering commands.  Do not send the tool's `loader`
  while an LE host is linked: the pad dropped off USB until a power cycle
  (once).  Its LE features are encryption only (no data length extension,
  no 2M PHY).
- `s8` in `pad.h` is a plain `char`, which is unsigned on ARM: a `-1` in
  an `s8` table became 255 and saturated the U2C Bluetooth's stick axes.
  ARM builds pass `-fsigned-char`.
- Never search the whole filesystem (`/`, `/proc`, `/sys`, `/dev`): it has
  disturbed the host.
- BR23 VBAT: the SDK truncates its 16-sample average to an ADC code before
  converting voltage. It normally refreshes one sample about every 20 s;
  roughly 12 mV steps and minutes of smoothing do not imply a scaling error.
- Web Bluetooth on Linux (BlueZ): a notification subscription survives the
  pad's restart ("Notifying" stays true), and on the next link of the same
  device nothing arrives through it, although the pad sends (the updater
  answered bleak on a fresh link).  `web/link-web.js` stops notifications
  before UPDATER and stops/starts them on every connect.
- Web Bluetooth (Chrome on Linux), Connect right after Disconnect: the new
  link's first command can fail with "GATT Error Unknown" (BlueZ's 2 s
  disconnect delay, above); `web/app.js` retries on a fresh link for 8 s.
- Bluetooth config mode with several programs on one pad (BlueZ):
  every subscriber gets every notification, once per subscribing program
  (seen with `gdbus monitor`); under load the U2CB lost pieces, an even
  number at a time (a FIELDS answer filled with the next answer's pieces);
  BlueZ keeps one write pending per characteristic and one connect per
  device across programs ("In Progress"); acquired notifications (bleak's
  default) refuse every other subscriber; and Chrome's disconnect ends the
  link for all.  Hence the frames in profile_config.c (tag, answer number,
  piece index, length, refusals), the hosts' retries, StartNotify in
  padble.py, and no disconnect on a retry in web/.  Kill test browsers by
  name (`pkill -x chromium`), not their xvfb-run wrapper.
- Android USB: `UsbDeviceConnection.requestWait(timeout)` throws
  `TimeoutException` (no message) instead of returning null, and a request
  cancelled and freed after that timeout crashed the next wait natively
  (SIGSEGV in `usb_request_wait`, seen on a phone).  Use `bulkTransfer` with
  a timeout for interrupt endpoints too, as `HidLink` and `AndroidNativeUsb` do.
- Android JNI: a Kotlin hook that throws (a USB error while a pad restarts)
  leaves the exception pending, and the next JNI call from C (NewByteArray)
  aborts the app (SIGABRT "JNI DETECTED ERROR ... called with pending
  exception", seen on a phone).  `jollymaam_jni.c` clears it after every call
  (`threw`) and reports a failed transfer to libjollymaam.
