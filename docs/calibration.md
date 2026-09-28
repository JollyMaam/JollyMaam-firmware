# Stick and trigger calibration

The pad measures the real range of its two sticks and two analog triggers and
maps it onto the full output range of whichever profile is running.  Nothing
here has been run on the pad yet (2026-09-27); the maths is checked on the host
with `make calibration-test`.

## Running it

On the pad: hold **Home+M for 3 seconds**.  Over USB: `cal start`.

| Home LED | phase | what to do |
| --- | --- | --- |
| steady blue, about 2.5 s | settle, then rest | hands off the sticks and triggers |
| blinking blue | sweep | circle each stick slowly along its edge about three times; press L2 and R2 all the way, trigger stops **off**; then press **A** (`cal done`) |
| green, 2 s | result | all four controls calibrated |
| yellow, 2 s | result | some calibrated; the others kept what they had |
| red, 2 s | result | nothing changed (cancelled with **B**/`cal cancel`, timed out after 2 minutes, or no control passed) |

While the routine runs the host gets a neutral report: centred sticks, no
buttons.  The console log says why a control was not calibrated
(`cal: R2 unchanged: trigger stop engaged`), and each control is judged on its
own, so it is fine to calibrate only the sticks.

The values are ordinary settings (`docs/config.md`): `config.cal` shows
what is in use, `config.cal.stick.0.deadzone_inner = 8` sets a dead zone in
percent, `unset config.cal` goes back to the built-in values, and
`export config.cal` saves it for a later `import`.
`status.cal.phase` says where the routine is and `debug.sensors` shows the raw
sensor values next to what they map to.

## Saving

The calibration is `config.cal` (syscfg item 49, the CAL group; only the
values that differ from the defaults are stored).  It is written like any
setting (`docs/config.md`): shortly after the routine finishes, unless a
pairing or a radio transition is under way, and not at all while
`config.autocommit` is off.  A stick whose stored values do not make sense
(centre outside its ends, a gate out of range) uses the defaults instead; a
trigger with `rest = 0` is uncalibrated and tracks its own rest position.

## The method

`src/calibration_math.c`, free of SDK includes so it builds on the host.

**Rest.**  For 1.5 s, the average of each axis is its centre and the spread
is its noise.  A stick that moves more than 0x300 counts or rests more than
0x1000 from the sensor's nominal 0x4000, or a trigger that moves more than 24
ADC counts, is rejected: it was not left alone.

**Sweep, triggers.**  The lowest ADC reading is fully pressed (the sensors fall
as a trigger goes down).  At least 96 counts of travel are required, and if the
trigger-stop microswitch closes the trigger is rejected: with the stop engaged
the Hall sensor only sees the first part of the travel.

**Sweep, sticks.**  Per axis, the lowest and highest readings are the ends of
travel (at least 0x1000 either side of the centre).  That alone maps the
cardinal directions right, but a gate is rarely a perfect circle in sensor
space: its diagonals fall short or reach past.  So the sweep also keeps, for
each of 16 directions, the farthest point seen.  Directions come from a
"pseudo-angle" (`cal_pseudo_angle()`), a monotonic stand-in for atan2 that
needs only one division and puts the cardinals and diagonals exactly on the
knots.  After the sweep each point is scaled with the final per-axis ranges and
its radius becomes the gate radius for its direction.  All 16 directions must
have been reached, which is what "circle the stick" is for.

**Mapping a stick** (`cal_stick_map()`):

1. each axis, piecewise linear: minimum..centre..maximum to -1..0..+1;
2. the radius is divided by the gate radius in that direction (interpolated
   between the two nearest knots), so the gate reads 1.0 everywhere;
3. radial dead zones: below the inner one the stick reads 0, above 1 - outer
   it reads 1, linear in between;
4. clamped to the unit circle, direction kept.

Dead zones are applied to the radius, not per axis, so a push slightly off
horizontal does not snap to horizontal.  The inner one is 3 % plus twice the
rest noise (at most 15 %); the outer one is 5 %, so that a normal push against
the gate reaches full scale.  Before any calibration the stick uses the
per-axis values measured on the first unit (`calibration.c`), no dead zones,
and the same radial clamp.

**Mapping a trigger** (`cal_trigger_map()`): linear from rest to full, with a
3 %-plus-noise inner and a 3 % outer dead zone.  Before any calibration the
old behaviour stays: the rest position is tracked as the highest reading seen
and three quarters of it is taken as the travel.  A closed trigger-stop switch
always reads fully pressed.

## Onto each profile

`struct cyclone2_input_report` carries sticks as -32767..+32767 (+y down) and
triggers as 0..65535; `input.h` has the conversions.

| profile | sticks | triggers |
| --- | --- | --- |
| Switch | 12 bit, **0x800 ± 0x600**, the range its virtual factory calibration (0x603d) declares; the host scales by that, so this is what reads 100 % | ZL/ZR past 1/8 |
| Switch 2 | 12 bit, full range around 0x800 (its flash calibration at 0x13080 is not decoded) | ZL/ZR past 1/8 |
| Xbox | 16 bit around 0x8000 | 10 bit |
| DualSense Edge | 8 bit around 0x80 | 8 bit |
| Generic BLE | 8 bit around 0x80 | 8 bit |

Before this, the Switch profile sent the whole 0..0xfff range, so on a Switch
the stick read 100 % at three quarters of its travel.

## Not done

* Whether the 24C64 EEPROM (`CYCLONE2_EEPROM_I2C_ADDRESS`) holds a factory
  calibration from the stock firmware has not been checked; it is not read.
* The Switch profile still answers with fixed factory calibration and leaves
  the user-calibration area erased; the pad does the correction itself.
