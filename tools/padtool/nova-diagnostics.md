# Nova 2 Lite LED investigation

These are diagnostic tools, not a confirmed fix. The user is using the pad on
battery to see whether the failure recurs. Do not flash, reset, or run stock
before capturing a failed-state snapshot.

## Build and run

From the repository root:

```sh
nix develop -c make -j4 gamesir-nova-2-lite-pad EXTRA_DEFINES=-DNOVA_LED_PROBE
DEV_ROOT=/dev/bwrap /work/bin/python3 tools/padtool/nova_led_probe.py --device 5-2.2
```

Flashing needs user approval. The currently installed application already has
these diagnostics. The extra USB command (0x3e) is absent from normal builds.

The LED probe holds a raw full-brightness color on Home, bypassing normal
scenes. Enter or `n` advances through 11 timings and wraps; `p` goes back.
`w/r/g/b/d` selects white/red/green/blue/dark. `note TEXT` optionally records an
observation in `build/nova-led-probe/results.jsonl`. `q` clears the override.
Timing 0 uses the original C driver; 1 uses the same write counts in complete
assembly pulses. The other timings vary the counts. Counts are GPIO writes,
not calibrated microseconds. No setting or pairing changes are made.

Capture power state without changing the LED override or power configuration:

```sh
DEV_ROOT=/dev/bwrap /work/bin/python3 tools/padtool/nova_power_snapshot.py \
  --device 5-2.2 --output build/nova-led-probe/failed-power.json
```

Use a new output filename each time; existing snapshots are not overwritten.
The script reads P33 analog/keep registers, GPIO, IOMAP, identity and status.

## Handoff, 2026-11-02

- Initially normal scenes, LED demos, and all timing choices stayed dark.
- The saved original factory firmware lit Home red, green and blue. The user
  also observed constant rumble when plugging stock in; its cause is unknown.
- After returning to ours, LEDs worked. The application payload before and
  after the stock round trip was byte-identical.
- Restoring the previous updater while keeping the main application unchanged
  did not break LEDs. The user confirmed the blinking demo worked.
- LEDs survived an unplugged 30-second idle soft-off, USB wake, and repeated
  pinhole resets/reconnections. The wake log confirmed a fresh USB-wake boot.
- Finally the user confirmed all timing choices and w/r/g/b work, including
  operation on battery. The user recalls failure beginning after low battery.
  That trigger and the initialization mechanism remain unconfirmed.

The working snapshot is `build/nova-led-probe/working-power-baseline.json`.
Its P33 ANA_CON0..9 bytes are:

```
f7 00 d0 00 0f 54 0f 00 00 17
ANA_KEEP=40  VLD_KEEP=00
```

Other evidence is in `build/nova-led-probe/`: `before-stock-flash.bin` (full
backup), `stock-flash.log`, `updater-isolation.json`, and
`working-after-stock/` (decoded images and GPIO/IOMAP captures). These artifacts
are local build outputs, not committed firmware or fixtures.

If LEDs fail again, connect USB, capture power state and logs first, and compare
with the baseline. Stock recovery may destroy the interesting state. No true
battery-disconnected cold-start test has been done; the user does not want to
open the pad. Software soft-off and pinhole reset are not proven to remove
power from the LED circuitry.

Stock mode selection: Home+X worked to select Xbox mode (green). The stock
loader command works in that mode. Home+Menu and View+Menu did not work in this
session. The host's `/work/99-bwrap.rules` lacked stock Xbox VID:PID 3537:100f;
adding a hidraw export rule restored visibility. That host file is outside
this repository.
