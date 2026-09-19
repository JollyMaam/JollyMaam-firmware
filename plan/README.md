# Controller implementation plan

Source: `/work/PLANNED_ARCHITECTURE.md`. Keep this small: fixed buffers, explicit
ownership, existing SDK mechanisms, and one hardware-verified change at a time.
Implementation agents use `gpt-5.6-luna`, medium reasoning. The coordinating
agent owns integration, builds, flashing, and this progress record.

## Order and progress

| Step | Brief | State |
| --- | --- | --- |
| 1 | [USB testing access](01-usb-testing.md) | implementation present; review and hardware test pending |
| 2 | [Nonblocking acquisition](02-acquisition.md) | implementation present; hardware test pending |
| 3 | [Host test tools](03-host-tests.md) | implementation present; 10 ms live test passed; 5 ms host test awaits reconnect |
| 4 | [Controller scheduling and TX](04-controller-tx.md) | pending hardware baseline |
| 5 | [IMU and rumble](05-imu-rumble.md) | pending stable scheduling |

The starting controller is paired in Switch slot A (slot 0), address
`13:11:DB:58:28:18`. Preserve its syscfg/VM/BTIF and address derivation. Never
erase the whole chip during iteration. Existing UBOOT recovery is proven;
do not refactor that sequence as part of this work.

## Integration checks

1. Build; retain the previous known-working image outside the build output.
2. Query CDC `status`/`help`; request UBOOT; write only the packed image's code
   span, keeping reserved flash regions untouched; read back and compare.
3. Reset; wait for CDC; request `connect 0`; require an actual usable HID
   connection, not just an ACL connection. Check reports on the host input node.
4. Exercise repeated injected presses/releases, check missing/extra edges and
   latency. Repeat recovery after each meaningful integration milestone.
5. Record exact image hash, commands, outcomes, and unavailable hardware here.

Only the coordinator accesses hardware. Agents may build individual objects;
coordinate full builds to avoid racing generated files. Each brief defines file
ownership. No commits from agents; coordinator reviews and commits together.

## Design limits

Keep lifecycle/USB/flash writes in app_core. A future controller task owns
debounce, synthetic-input composition and report scheduling. Interrupts only
capture data and signal work. Do not call Bluetooth or log from an interrupt.
Use bounded queues only where events must be retained: UART bytes, button
transitions, protocol replies, and IMU history. Continuous axes use latest state.
Do not add a generic event bus, allocator, plugin system, or macro language.

## Validation log

- Initial checkout: `5abe596`, clean. At initial discovery only
  `/dev/bwrap/ttyACM0` exists; loader/input nodes may appear on enumeration.
  Host D-Bus is not exposed. Firmware-initiated reconnect avoids requiring it.

## Current handoff state (2026-09-18)

The plan was introduced in `4c0513b`. The reviewed implementation is committed
in `9288a03`, with the generated Python cache ignore rule in `1c0ec41`; the
worktree is clean. The implementation includes `src/controller.c`,
`src/controller.h`, `src/input.c`, `src/input.h`, `src/usb.c`, `tools/c2ctl.c`,
`tools/latency.py`, and their documentation.

The USB baseline agent reports successful object builds for
`build/src/controller.c.o` and `build/src/usb.c.o`; `git diff --check` passed.
The acquisition agent reports a successful `make V=1 build/src/input.c.o`.
Its implementation uses the SDK UART2 DMA ring on RX PA3, restores PA2 for
d-pad up, retains divisor 24, and adds parser/ring statistics. Live validation
of CRC frames, stick motion, d-pad up, overflow behavior, and report integrity
has not happened.

The host-tools agent exhausted its model budget. Treat `c2ctl.c`, `latency.py`,
and `docs/latency.md` as unfinished until reviewed and built. During manual
testing, opening CDC after a prior session required asserting DTR/RTS with
`TIOCMBIS` after raw-mode setup; this must be handled deliberately in the host
client, with a fallback for devices that reject modem control.

Hardware checks completed in this session:

- CDC answered `debug off`, `status`, and `report status`; the device was alive
  but had no active Bluetooth profile at that moment (`attempts=0`).
- CDC `uboot` was accepted and printed the expected MaskROM token. The device
  enumerated as `/dev/bwrap/sg1`, and the existing loader read the complete
  512 KiB flash.
- The readback is saved outside the repository as
  `/work/cyclone2-test-qjEkRO/baseline-flash.bin`, SHA-256
  `105a1775ff0f192529a2293f5e7e04bd981c6e759398a775788d8e1436af3807`.
  The pre-existing build image is saved as
  `/work/cyclone2-test-qjEkRO/initial-build.img`, SHA-256
  `4e3bf98dfd3daad9eaf68996713cb8da90b6fdccbcb097af210a168c4e11f49f`.
- No firmware image was flashed during this stage. The paired Switch device
  and its syscfg/VM/BTIF data remain the recovery baseline.

When resuming, review the next diff first, run a complete build, then test CDC
`help`, `connect status`, `connect 0`, and
`inject a 100 10000` while USB remains connected. Confirm the host sees the
saved Switch pairing before considering a flash. Keep the saved readback until
the new image has passed recovery and Bluetooth checks.

## Stage 1 validation update

The reviewed stage-1 image was built as `build/firmware.img`, verified by
`jlnewfw`, flashed through UBOOT1.00, and read back byte-for-byte. The full
image is 212992 bytes; the saved readback is
`/work/cyclone2-test-qjEkRO/stage3-readback.bin`. The existing reserved region
from offset `0x40000` through the end of the 512 KiB readback remained equal to
the pre-flash readback.

Live CDC and Bluetooth results:

- `connect 0` reconnected to the saved Switch bond without pairing. CDC then
  reported `state=connected`, address `13:11:db:58:28:18`, and USB present.
- `input status` showed valid stick frames and zero CRC failures. During a long
  run it reported two UART ring overflows and dropped bytes; axis data remained
  valid. This needs tuning before claiming the DMA path is lossless.
- `inject a 100 10000` produced 100 press and 100 release events on evdev code
  305 (`BTN_EAST`, the current Switch mapping for A), with no duplicates,
  `SYN_DROPPED`, or missing events. Device injection status reported 200
  accepted edges and zero schedule overruns. Host event spacing was about
  52 ms median for the alternating edges, consistent with a 100 ms cycle.
- The firmware was switched to a 5 ms report period, but the host did not
  automatically reconnect after the subsequent reboot, so no 5 ms evdev result
  is available. A host-side Bluetooth reconnect is required before continuing.

The generated CDC build copy suppresses an SDK bulk-endpoint reinitialization
on each CDC control-line change. Repeated CDC opens worked after this change;
the source SDK checkout is unchanged. The complete build also links the SDK
`cpu/br23/uart_dev.c` source required by the DMA acquisition implementation.

## Resume update (2026-09-19)

Trigger-stop microswitch support is committed in `9d031e2`. Static analysis
and a focused RAM diagnostic established that the switches use the two
active-low RTC-domain inputs rather than PA-PD GPIOs: left is SDK PR0
(physical/one-based PR1), right is SDK PR1 (physical/one-based PR2). The input
driver now configures both through the SDK RTC API and forces the corresponding
trigger report to `0xff` while a microswitch is closed, overriding the Hall ADC.

The resulting `build/firmware.img` has SHA-256
`18fefbb2e9add43ae223d580b08f8c2a2bcd2d1e2908e57b50443c64757d9dc5`.
It was verified by `jlnewfw`, flashed from offset zero through `0x33fff`, and
read back byte-for-byte to
`/work/cyclone2-test-qjEkRO/trigger-switch-readback.bin`. Reserved VM/BTIF data
was not written. The image boots and CDC works. `input status` after boot showed
64071 valid UART frames, zero dropped bytes, zero overflows, and four CRC misses.
The saved report period is 5 ms.

The remaining validation gate is host Bluetooth reconnection. Two CDC
`connect 0` attempts completed their reset path but eventually reported
`state=off`, with no host HID node. A host-side `bluetoothctl connect` (or an
equivalent host action) is needed before measuring 5 ms reports and verifying
the PR trigger overrides through evdev. Do not start step 4 until this baseline
is confirmed.

The user subsequently confirmed that `connect 0` did reconnect without a
host-side `bluetoothctl` command, although the elapsed time was not observed.
Commit `3cb6a79` adds an explicit `disconnect` console command. It stops
injection, requests the profile-level disconnect, waits 100 ms, exits the
Bluetooth stack, and deliberately leaves USB/CDC and the saved bond intact.
The corresponding image (SHA-256
`8938ae3a5b93ce1f2afa09e82bea5cd151e027bba3acf599f6ecfd5cbab6610f`)
was flashed and read back exactly. An automated `connect 0` observation still
reported `state=off` after 35 seconds, so live `disconnect` followed by
reconnect remains a user-assisted validation item.

## Step 4 progress

The user validated `connect 0`, `disconnect`, independent debounce, Home taps,
and USB-powered Home-hold disconnect behavior. Commit `1539abf` replaces the
whole-report debounce timer with independent per-button state and advances the
two-phase synthesized Home tap only after a report is accepted.

SDK `sys_timer_add()` is a synchronized timer: expiry is delivered back to the
task that registered it. `cyclone2_poll()` therefore runs in `app_core`
(priority 1), not in interrupt or `systimer` context; Bluetooth remains above
it (`btstack` priority 3, `btctrler` priority 4). A separate controller task is
not justified until acquisition or queue measurements show a scheduling issue.

Commit `6bdd0e2` classifies transmit results. Classic HID buffer-full and BLE
`APP_BLE_BUFF_FULL` are temporary busy results; deliberate BLE duplicate/CCC
deferral and unexpected errors have separate counters. Send attempts are now
counted only in the usable connected state. The image SHA-256 is
`7f29533ff7bf520b38e529f4a92d4d8e88e67a7e99f73ab9e477f272a7bc8809`;
it was flashed and read back exactly. Obtain `report status` during an active
5 ms connection to decide whether callback-driven retry work is warranted.
