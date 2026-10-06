# Working on this repository

GameSir Cyclone 2 firmware: the Jieli BR23 SDK's runtime and controller
library (`btctrler.a`) with upstream BTstack as the host
(`/work/cyclone2-btstack`, unmodified `master`).  The design is in [architecture/](architecture/README.md); hardware
references are in [docs/](docs/README.md).

## Working rules

- The user operates the physical pad and console.  Building and flashing are
  ours; explain any requested physical action plainly.
- Commit at a logical checkpoint.
- Say "the app" or "the SDK" when distinguishing code ownership.
- Prefer measured evidence.  In docs/, distinguish board measurements,
  recovered binary behaviour and assumptions.
- Do not add software plans or session diaries to the hardware documentation.
- Keep architecture/ in step with the code when an interface changes.

## Build, test, flash

    PATH=/work/bin:$PATH make -j32
    PATH=/work/bin:$PATH make test        # every src/*.c with an #ifdef UNITTEST block, host cc
    PATH=/work/bin:$PATH make flash       # over USB: jlnewfw flash --enter-loader

`tools/padtool.py` talks to the running pad over USB (vendor SCSI commands on
the mass-storage interface, `/dev/bwrap/sg*`): `identify`, `status`,
`log [-f]`, `stats [-w SECONDS]`, `reboot`, `loader`, and settings:
`fields`, `get`, `set`, `reset`, `backup-settings`, `restore-settings`,
`factory-reset`, `ephemeral`, `keep` (`padtool --help`).  The log is a RAM
ring that survives a reboot.

`tools/jlnewfw.c` (`build/jlnewfw`) is the image and flashing tool: `list`,
`unpack`, `pack`, and `flash`/`flashpack` through the chip's ROM loader with
the SDK's loader blob (`--loader $(SDK)/cpu/br23/tools/br23loader.bin`).  It
writes only onto a device whose flash holds the image's PID (our build:
`GS_C2_ADC_DEVICE`; the dongle: `GS_C2_Dongle`) and encrypted for the chip's
key; `--force-pid` for a blank device or a deliberate change.  A pad that
does not enumerate is recovered by holding Home+Start while plugging in USB
(ROM loader, UBOOT1.00), then `make flash`.

What can be tuned by hand: Home gestures, timeouts and LED scenes in
`src/settings_ux.c`; setting defaults (the field table) and the default
palette in `src/settings.c`.  At run time over USB with `padtool set`.

`tools/build_btctrler.py` and `tools/btctrler/` build the retained controller archive with owned
policy extraction and the tested fixes, applied to a build copy, never the SDK.  The linker's stack-size warnings are a vendor lint.
`CODE_LIMIT` fixes the VM boundary; moving it moves the settings store.

## Measured constraints worth preserving

- Classic reconnect initiates from the pad.  Opening outgoing HID before
  authentication caused reason-5 failures: keep outgoing LEVEL_2 / incoming
  LEVEL_0 (`src/bt.c`).
- The controller lists Read Encryption Key Size but never answers it; the
  transport answers (`src/btstack_hci_jieli.c`).  HCI Reset restores the
  factory address, so ours is reapplied after it.
- The BTstack run loop sleeps on a semaphore; a one-tick delay capped Classic
  reports at 100 per second.
- Input reports go through a newest-wins slot with at most two packets in the
  controller.  Nothing is queued, buttons included.
- `bt_max_pwr_set()` must be called at boot (`src/platform_jieli.c`); without
  it the radio ran 20 dB below the stock pad.
- LE links ask for 251-byte PDUs; at 27 bytes a Switch 2 report took three
  connection events.
- The control task runs above BTstack's: BTstack spends ~1 ms of CPU per
  packet.
- `REGISTER_LP_TARGET` keeps the SDK out of low-power sleep; without it USB
  did not enumerate.
- Switch 1 wake emission (`src/profile_switch_wake.c`, 1 s after a failed
  page) was measured on the air; a console actually waking and the whitening
  state were not.  Switch 2 wakes from a flag in its LE advertisement.
- No floating point.  The BR23's FPU is emulated (the SDK's `emu_fpu_*`):
  float instructions compile, but each traps into an emulator, and per-sample
  float maths in the Switch 2 motion code slowed the millisecond tick to 5-9 %
  of real time (everything lagged).  Use fixed point (Q30 with s64 products).
- No `__attribute__((packed))` in state the loop writes.  With packed stats
  structs the tick interrupt's millisecond count ran at 7 % speed: LTO merges
  the globals into one block, and the unaligned stores to the packed fields
  evidently clobbered the tick's byte.  Lay structs out without padding instead.
- No root or `/dev/bus/usb` here; use the devices in `/dev/bwrap`.  Do not
  attempt to leave the container.
