# Runtime shortcuts and remapping — design notes

**Status:** the remapping implementation and runtime shortcut settings have
been removed. The format and commands below describe the previous prototype
and a proposed future design, for iteration; they are not currently available.
The firmware now uses fixed M shortcuts in `src/settings_ux.c`.

These settings are global, so they also work before a slot is selected. Change
and commit them with `padtool/padtool.py set`; they persist and are included in
`backup-settings`, `restore-settings` and `factory-reset`. `ephemeral` can be
used to try settings without saving them. The current physical wake input is
still Home. Home wake connects the last slot and consumes that initial Home
press until release. Home+Start loader recovery remains reserved and physical.

## Defaults

| Input | Action | Hold |
|---|---|---|
| Home wake / Home tap while offline | Connect last slot | Immediate on boot / tap release |
| Home | Sleep (disconnect on USB) | 5 seconds |
| M+Minus | Disconnect | 1.5 seconds |
| M+A/B/X/Y | Select slots A/B/X/Y | 1.5 seconds |
| M+Up/Down/Left/Right | Select slots Up/Down/Left/Right | 1.5 seconds |
| Keep holding M+slot | Pair that slot | 5 seconds total |
| M+L3 or M+R3 | Calibrate | 3 seconds |

M waits 150 ms for a configured chord. With suppression enabled, it stays out
of reports during that window. If no configured chord arrives, M goes to the
host as a regular held key. Releasing it early produces a down report at the
next report opportunity and an up at the following opportunity. The deferred
down survives until a report is accepted by the Bluetooth input path; delivery
still depends on the link and the selected profile exposing the mapped key.
Several taps between report opportunities can merge, consistent with the
firmware's newest-input policy.

A chord can already be held when M goes down. Once captured, it cannot change
into a different chord: a change cancels it, and captured keys stay suppressed
until all are released. Unbound combinations do not capture keys. Each rule
fires once per press; rules for the same chord can fire at different times
(switch at 1500 ms, pair at 5000 ms). Hold time starts when the full chord is
recognized, and continues across slot/session changes. Matching is exact:
additional physical buttons or d-pad directions prevent a match. Stick and
trigger positions do not affect digital chord matching.

`shortcuts passthrough on` sends keys normally while still running shortcuts;
in that mode the grace window does not prevent late chords. The default is
`off`. A shortcut settings edit cancels the current gesture and drains held
keys; release them before trying the edited configuration.

## Examples

```sh
# Adjust the window; retain normal M behavior outside chords.
padtool/padtool.py set shortcuts grace_ms 250

# Send chord keys to the host too, or suppress them (default).
padtool/padtool.py set shortcuts passthrough on
padtool/padtool.py set shortcuts passthrough off

# Change the disconnect shortcut's hold time.
padtool/padtool.py set shortcuts rule8 'm+minus:disconnect:none:2000'

# Use the spare rule for another action.
padtool/padtool.py set shortcuts rule19 'm+plus:sleep:none:2000'

# Disable A-slot pairing while retaining its switch shortcut.
padtool/padtool.py set shortcuts rule11 none

# Change the modifier AND corresponding rules together (update all wanted rules).
padtool/padtool.py set shortcuts modifier l4 rule0 'l4+a:connect:A:1500'

# Remap a physical input to one or several virtual controls.
padtool/padtool.py set mapping a b b a m home l4 'l1+r1'
padtool/padtool.py set mapping screenshot none

# Swap stick axes, invert one, and swap triggers. '--' allows negative selectors.
padtool/padtool.py set -- mapping lx rx rx lx ly -ry ry ly l2 r2 r2 l2

# Disable standalone Home sleep; wake/recovery remain available.
padtool/padtool.py set shortcuts home_sleep_ms 0

# Inspect/reset configuration.
padtool/padtool.py get shortcuts
padtool/padtool.py get mapping
padtool/padtool.py reset shortcuts
padtool/padtool.py reset mapping
```

Shortcut matching uses **physical** controls, before output mapping. For
example, mapping M to Home makes an ordinary M press a host Home press, but
M+Minus still invokes the configured disconnect shortcut. A profile can only
send controls that its protocol supports; mapping does not add host keyboard
support to a gamepad-only profile.

All digital controls (15 buttons and four d-pad directions) can map to any
combination of those outputs, or none. Outputs from multiple inputs are ORed;
they remain held until all contributing inputs release. Opposite d-pad
directions cancel independently. Stick output axes can select any of LX, LY,
RX, RY, their inversions, or neutral. Trigger outputs select L2, R2, or neutral.
Sources are read from the original calibrated values, after the source stick
response curve; swaps cannot cascade. Mapping edits apply on the next input
pass, including held inputs.

## Encoding implemented now

The existing settings item format is `{u8 target, u8 id, u8 length, bytes}`;
zero length resets a field, and id zero resets the target. Writes are staged
and committed together with the existing USB settings protocol. Multibyte
integers are little-endian. The record envelope remains version 1; the new
target kinds and field flags are additive.

Digital masks use the button bit numbers in `src/pad.h`: A=0, B=1,
Screenshot=2, X=3, Y=4, M=5, L1=6, R1=7, L4=8, R4=9, Minus=10, Plus=11,
Home=12, L3=13, R3=14; Up=16, Right=17, Down=18, Left=19. Bit 15 and bits
20–31 are invalid. Key names are case-insensitive; `select`/`start` alias
`minus`/`plus`.

Target 10 (`shortcuts`, descriptor kind 3):

| Field id | Value |
|---|---|
| 1 | `u32 modifier` mask; zero disables modifier shortcuts |
| 2 | `u16 grace_ms`, 0–5000 |
| 3 | `u8 passthrough`, 0 off / 1 on |
| 4–23 | rule0–rule19, each `{u32 keys, u16 hold_ms, u8 action, u8 slot}` |
| 24 | `u16 home_sleep_ms`, 0 disables, maximum 60000 |

Rule actions: 0 disabled, 1 connect, 2 pair, 3 disconnect, 4 sleep,
5 calibrate. Slot is 0–7 or 254 for last; other actions conventionally use
255 (none). Rules must include the configured modifier to be reachable.
Changing the modifier does not rewrite rules. A disabled rule is eight zero
bytes. Host notation is `keys:action:slot:hold_ms`, or `none`.

Default rule indices: 0–7 switch A/B/X/Y/Up/Down/Left/Right, 8 disconnect,
9 L3 calibration, 10 R3 calibration, 11–18 pair in the same slot order,
19 disabled. When several rules become due in one pass, table order decides
which fires first; the others remain due for subsequent passes.

Target 11 (`mapping`, descriptor kind 4):

- Field id is physical input bit + 1, value `u32 output_mask` (identity by default).
  There is no field for unused bit 15.
- Fields 21–24 are output LX/LY/RX/RY source selectors: 0 neutral,
  1–4 LX/LY/RX/RY, 5–8 their inversions.
- Fields 25–26 are output L2/R2 source selectors: 0 neutral, 1 L2, 2 R2.

Descriptor flags: bit 0 developer setting (existing), bit 1 shortcut rule,
bit 2 digital mask. `padtool` renders/parses the latter two as names rather
than hexadecimal blobs. Firmware validates lengths, masks, actions, slot
arguments and selector ranges before staging. These targets fit in separate
settings records; they do not enlarge existing slot records.

## Extension design: typed routes and temporal bindings

The implemented format deliberately covers ordinary mapping and controller
shortcuts. It does not yet execute macros, layers, turbo, analog-to-digital
bindings, or arbitrary digital chords that emit other digital chords.
A versioned, declarative binding document can capture those without running
user code. A host tool would compile it into bounded route and binding tables,
validate it as a whole, and atomically install it in a separate mapping target.
Keep the version-1 masks as the convenient simple form.

A route has a **source**, optional **transforms**, a **destination**, and
optional **conditions**. A temporal binding adds trigger timing, consumption,
and one or more actions. Explicit signed/unipolar types make axis conversions
unambiguous. For example, this is a proposed authoring format, not an accepted
`padtool` command today:

```json
{
  "version": 2,
  "modifiers": {
    "m": {"source": "button.m", "grace_ms": 150, "consume": true,
          "tap": "button.home", "hold": "button.home"}
  },
  "bindings": [
    {"when": {"keys": ["m", "minus"], "exact": true},
     "trigger": {"hold_ms": 1500, "once": true},
     "consume": ["m", "minus"], "do": [{"disconnect": true}]},
    {"when": {"keys": ["m", "a"], "exact": true},
     "trigger": {"hold_ms": 1500, "once": true},
     "consume": ["m", "a"], "do": [{"connect_slot": "A"}]},
    {"source": "axis.ly", "transform": [{"half": "negative"}],
     "to": "axis.l2"},
    {"source": "axis.r2", "transform": [
       {"threshold": {"on": 0.25, "off": 0.20}},
       {"repeat": {"min_hz": 2, "max_hz": 20, "pulse_ms": 20,
                   "rate_from": "axis.r2"}}
     ], "to": "button.a"},
    {"when": {"layer": "desktop"}, "source": "stick.right",
     "transform": [{"deadzone": 0.10}, {"velocity": {"max_per_second": 1600}}],
     "to": "pointer.xy"}
  ]
}
```

Useful typed transforms:

- **Signed axis → trigger:** positive or negative half-wave, or full-range
  affine mapping. Half-wave leaves a centered stick at zero trigger; full-range
  maps center to half trigger and should always be an explicit choice.
- **Trigger → signed axis:** positive/negative half-axis, or full-range affine
  mapping. State the neutral/rest behavior in the conversion.
- **Analog → button:** threshold with separate press/release values
  (hysteresis), followed optionally by a pressure-to-repeat-rate curve. A
  scheduler emits a finite down pulse and an up, with deadlines between them;
  clamp frequency so both phases fit the host report rate. Never queue missed
  repeat pulses after a stalled link.
- **Digital → axis:** fixed signed value while held, optionally ramped;
  opposite directions cancel. **Axis → pointer:** velocity integration using
  elapsed time, preserving fractional counts.
- **Stick pair → stick pair:** rotation, radial deadzone and response curve.
  Keep pair transforms separate from per-axis operations.

Temporal triggers can be press, release, tap, hold, double-tap, or repeat;
actions can emit virtual controls, controller requests, momentary/toggled
layers, or bounded sequences. Every consuming modifier has its own grace
window and tap/hold fallback. A captured binding owns its controls until
release; modifier expiry commits to the regular mapping for that press.
Prefer explicit priorities for overlapping bindings, and reject ambiguous
rules at equal priority. Bindings read the physical snapshot and conditions;
outputs never feed back as physical inputs.

Digital destinations combine with OR; analog destinations need an explicit
policy such as maximum magnitude, saturating sum, or priority. Layer changes,
configuration replacement, disconnect and cancellation must release synthetic
outputs and cancel pending timers. All rules have fixed resource limits
(number of layers, active timers, macro steps and repeat frequency); no loops,
recursion or unbounded event queues. Protect the physical loader combination
and retain a tool-based reset path regardless of the installed mapping.

This gives a fairly broad authoring model while keeping firmware execution
small, deterministic and compatible with the event-driven controller loop.
