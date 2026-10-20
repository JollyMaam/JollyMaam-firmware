# Remapping — design notes

Remapping works on pad buttons, after the Home gestures (`src/settings_ux.c`),
so every shortcut still uses the physical keys and the same remap works for
every slot type. The engine is `src/remap.c`; the data is `struct set_remap`
in `src/pad.h`; the tools' text form is `padtool/padremap.py`.

## Choosing a remap

Eight remap slots, A–right. Each connection slot's `remap` setting names the
one each connect starts with (default none). Connected, Home+M (on release)
enters config mode: the grips flash the slot's connect colour and the host
sees a neutral pad, motion at rest. Releasing a slot key picks that remap;
releasing Home or M picks none; 30 s without a press leaves it as it was. A
picked remap lasts across that slot's own reconnects and ends with Home+Minus,
another slot or sleep. Its state (toggles, turbo phase) starts over whenever
the remap or any setting changes.

## A remap slot

- **name**: UTF-8, 24 bytes.
- **map**: per input key, a "held: hold KEY" row (bit + 1, itself included),
  "held: nothing" (0xff) or no row (0).
- **rules**: 16 × 10 bytes: trigger, input, time in 10 ms, effect, a 32-bit
  output mask, turbo rate, a reserved byte (zero; room for a condition key).
- **sticks** (a second store record, keys 22–29): per physical stick the
  host stick it maps to (itself, the other, nothing), X and Y inversion, a
  15-point curve and its enable flag, inner and outer deadzone in percent.
  Order: deadzone, curve, inversion, then the routing; two sticks on one host
  stick, the one pushed further wins. A slot with no
  remap gets the sticks exactly as calibrated: no curve, no deadzone.
  Calibration holds only the sensor measurements.

Remap A defaults to "Swap A/B X/Y" (`remap_defaults` in `src/settings.c`).

A key nothing mentions sends itself. A key with a map entry or rules sends
only what they make, all of it together; there is no precedence. The map is
only compact storage for the commonest row, so a full swap leaves the 16
rules free; the GUI shows both as one list. Rules read inputs, never other
rules' outputs; outputs OR together, and opposite d-pad directions cancel
afterwards.

| Trigger | Fires | Effects |
|---|---|---|
| held | while down | hold, turbo, turbo modifier, nothing |
| held for N ms | from N ms until release | hold, turbo, turbo modifier, nothing |
| pressed / released | at the edge | tap, toggle hold, toggle turbo, toggle turbo modifier |
| tapped under N ms | on a release before N ms | same |
| double-tapped within N ms | on a press N ms or less after a release | same |

Nothing waits to see what comes next, so no input is delayed by a rule it
does not match. A tap and a long press on one key means the tap only goes out
at release; a single-tap rule on a key also fires on a double tap's first tap.

A turbo modifier's keys are inputs: while it is on, what each of them sends
while held (its base row and hold rules) pulses at the modifier's rate, from
the later of the key's press and the modifier coming on. Other keys are left
alone; the first modifier on for a key wins.

Tap is one pulse of `ux_timeouts.tap_ms`. Turbo is 1–30 Hz, starting down,
timed by the clock; after a stall it does not catch up. Rate 0 (max) flips the
keys on every report handed over, starting down.

## Not yet

- **Macros**: an effect "run macro N" with bounded steps (keys, time) in a
  second record per remap slot; same clock rule as turbo.
- **Analog**: see below.
- **Conditions/layers**: "only while KEY held", in the reserved byte.

## Analog inputs and outputs (evaluated, not built)

The rule shape fits analog as long as inputs and outputs stay typed:

- **Analog as a digital input**: "LS up past N%", "R2 past N%" as extra input
  numbers with a threshold (with hysteresis) in the time byte. Every trigger
  and effect then works unchanged, e.g. R2 pressed → tap A.
- **Digital to analog output**: "LS up full", "R2 full" as extra output bits;
  OR becomes "the largest wins" per axis, opposite directions cancel.
- **Axis to axis** ("left stick Y drives R2"; whole-stick routing and inversion are built, above): a
  different kind of rule, a route with a source axis, a transform (half-axis,
  invert, scale) and a destination, applied to the sticks and triggers rather
  than the button mask. Several routes to one axis need a stated combine rule
  (largest magnitude). This does not fit the 10-byte digital rule; it wants
  its own small table (about 4 bytes each) and its own GUI section, so it is
  kept separate from button rules.
