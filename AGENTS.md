# Working on this repository

GameSir Cyclone 2 firmware: the Jieli BR23 controller SDK with a BlueKitchen
BTstack host port. The app is the code built from this repository and
`/work/btstack-jl-port`; the SDK includes the prebuilt Jieli archives. The
vendor `btstack.a` is replaced, but `btctrler.a` is still linked.

## Working rules

- The user operates the physical pad and console. Building, flashing and
  console commands are ours. Explain any requested physical action plainly.
- Use the shared `tools/padbus.py` daemon rather than opening serial directly.
  Keep the user informed during work. Restarting the daemon requires the user
  to reopen `tools/pad_tui.py`.
- Commit at a logical checkpoint.
- Say “the app” or “the SDK” when distinguishing code ownership.
- Prefer measured evidence. Hardware references live in [docs/](docs/README.md);
  distinguish board measurements, recovered binary behavior and assumptions.
- Do not add software plans or session diaries to the hardware documentation.

## Build and checks

    PATH=/work/bin:$PATH make -j32
    PATH=/work/bin:$PATH make config-test calibration-test wake-vectors wake-session-test aes128-test lights-test
    tools/flash.sh

`src/build_config.h` selects controller-archive patches. Patches apply to a
build copy, never the original SDK. `DEBUG_LOG=1` enables SDK tracing.
The compiler's stack-size warnings are a vendor lint; inspect actual task
budgets with `tools/stack_report.py` when changing call depth.

`CODE_LIMIT` fixes the VM boundary; moving it moves the settings store.
`config global.autocommit=false` keeps settings changes in RAM;
`config commit` persists them. A reboot loses uncommitted changes.

## Shared hardware session

    tools/padbus.py serve -p /dev/bwrap/ttyjl0 --ping-ms 500
    tools/pad_tui.py
    tools/padbus.py request codex
    tools/padbus.py send codex "status connection"
    tools/padbus.py release codex
    tools/padbus.py tail -n 40

The daemon rescans `/dev/bwrap/ttyjl*` after re-enumeration. For one command,
`tools/padbus.py cmd codex LINE` waits for the response and exits nonzero on
ERR. The byte-stream log may contain terminal control bytes; use its
`printable()` helper before rendering untrusted log bytes.

Console `help` lists commands; every namespace supports `help`, `-h`, `--help`.
Slots are A/B/X/Y or 0..3. `pair SLOT [--fresh]`, `connect SLOT`, `disconnect`,
`reboot`, `config`, `status`, and `debug` are the principal commands.
The user runs `tools/hostbt.py serve` with the necessary permissions on the
host when Linux Bluetooth testing is needed; the client uses localhost:47807.

## Measured constraints worth preserving

- Classic reconnect initiates from the pad. Change Grip/Order on the console
  instead initiates a new pairing. Opening outgoing HID before authentication
  caused reason-5 failures; retain the outgoing LEVEL_2 / incoming LEVEL_0
  distinction in `src/bt/btstack_jl_core.c`.
- The SDK advertises Read Encryption Key Size but never completes it; the
  port transport supplies the response. Persistent bonds belong to app VM.
- The SDK detach queue could retain the last received ACL packet and delay
  disconnect until supervision timeout. `patch_detach_drain_rx()` fixes it.
- The BTstack run loop must sleep on the port's wake semaphore
  (`jieli_btctrler_wait`). A one-tick `os_time_dly` held every wake to the
  10 ms system tick and capped Classic reports at 100 per second.
- Input reports, Classic and LE, go through a newest-wins slot with at most
  two packets in the controller (`BTSTACK_JL_MAX_IN_FLIGHT`).  Digital
  state (buttons, hat) is not newest-wins: `controller.c` queues every change
  and posts them in order, one per report the link takes.  Behind obstacles
  the radio retransmits for tens of ms and a newest-wins d-pad lost presses;
  the stock pad, which queues everything, did not.
  `status report.air.*` shows the slot wait and the hand-off-to-ack hold time.
- LE links must ask for 251-byte PDUs (`btstack_jl_le.c`); at 27 bytes a
  Switch 2 report took three connection events.
- `app_core` runs above `btstack`: the other way round, BTstack's ~1 ms of
  CPU per packet (it runs from flash) cost 19% of the 1 ms control polls.
- `uxTaskGetStackHighWaterMark()` is unwired and resets the pad. P33 `0x40`
  does not distinguish a deliberate reset from a fault.
- Raw Switch 1 wake access-code emission was measured; successful console
  wake and whitening were not. Switch 2 uses its own LE wake advertisement.
- No root or `/dev/bus/usb` here; use exposed `/dev/bwrap` devices. Do not
  attempt to leave the container.
