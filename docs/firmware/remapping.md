# Remapping — design notes

Remapping works on pad buttons, after the Home gestures (`src/core/settings_ux.c`),
so every shortcut still uses the physical keys and the same remap works for
every slot type. The engine is `src/core/remap.c`; the data is `struct set_remap`
in `src/core/pad.h`; the tools' text form is `tools/padtool/padremap.py`.

## Choosing a remap

Eight remap slots, south–right, and the base remap. Each connection slot's
`remap` setting names the one each connect starts with (default base).
Connected, Home+M (on release) enters quick settings: colours flow down the pad's
LEDs and the host sees a neutral pad, motion at rest. Releasing a
slot key picks that remap; releasing M or Home alone picks the base; 30 s
without a press leaves it as it was. Home's chords work there as anywhere
while connected: Home+Minus disconnects, which ends quick settings. A
picked remap lasts across that slot's own reconnects and ends with Home+Minus,
another slot or sleep. Its state (toggles, turbo phase) starts over whenever
the remap or any setting changes.

## The base remap

The base has every remap setting (store keys 30 and 31). A remap slot takes
each field it has not set from the base, live: changing the base changes every
slot that has not set that field. Two exceptions: a slot's name is its own,
and remap south keeps its "Swap faces" name, map and rules as its own defaults.
Resetting a slot's field makes it follow the base again. The base at its
defaults sends every key as itself and the sticks as calibrated.

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
  stick, the one pushed further wins. The base at its defaults gives the
  sticks exactly as calibrated: no curve, no deadzone.
- **triggers** (in the sticks record): per trigger a press point in
  percent of travel (default 5) and an analog mode. The press point alone decides
  the host's digital trigger (`PAD_L2`, `PAD_R2`, set after the remap; every
  profile with a trigger button reads them), letting go 3% short of it.
  Default leaves the value as calibrated; hair sends full while pressed and 0
  otherwise; rapid is hair that lets go once the trigger rises 5% from its
  deepest point and presses again once it goes 5% down from its highest,
  back to the press point only after it falls to the release point. Outside
  a connection nothing is pressed.
  Calibration holds only the sensor measurements.

Remap south defaults to "Swap faces" (`remap_south_default` in `src/core/settings.c`).

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

- **Analog as a digital input**: "LS up past N%" as extra input numbers with
  a threshold (with hysteresis) in the time byte. Every trigger and effect
  then works unchanged. The triggers' presses (`PAD_L2`, `PAD_R2`, bits 20
  and 21) are decided after the remap today; as remap inputs ("R2 pressed →
  tap south") they would move before it, with `REMAP_INPUTS` 22.
- **Digital to analog output**: "LS up full", "R2 full" as extra output bits;
  OR becomes "the largest wins" per axis, opposite directions cancel.
- **Axis to axis** ("left stick Y drives R2"; whole-stick routing and inversion are built, above): a
  different kind of rule, a route with a source axis, a transform (half-axis,
  invert, scale) and a destination, applied to the sticks and triggers rather
  than the button mask. Several routes to one axis need a stated combine rule
  (largest magnitude). This does not fit the 10-byte digital rule; it wants
  its own small table (about 4 bytes each) and its own GUI section, so it is
  kept separate from button rules.
