# Stock battery reporting

The Ultimate 2C Wireless pad's factory v1.06 application samples battery
voltage, estimates a percentage, tracks low-battery state and builds a
software charging/full indication. These are separate decisions.

The full-decision and percentage instruction sequences are also present in
the downloadable v1.09 image, at `0xb450` and `0xb5a8` respectively. Its
initialized timeout is also 14,400. Addresses below are flash offsets in the
factory pad image. GPIO input
register `0x800518` is port D. Its bit 0 is PD0 (USB supply present), and
bit 1 is PD1. The battery routines read both pins; they do not drive them.

## Sampling and voltage

The background service at `0xb910` schedules `0xb748` after more than
16,000,000 system-timer ticks, approximately once a second. Special startup
paths can sample more often. A separate mode flag can suppress that service.

The sampling routine selects ADC channel 7, PB6. Its integer conversion is:

```
pin_mv = (adc_code * 1175) >> 13;
battery_mv = pin_mv * 3700 / 1000;
```

The intermediate truncation makes reported voltage advance in steps of about
3.7 mV, despite the finer ADC code resolution. Calibration of this conversion
against the physical cell voltage is not established.

## Percentage

`0xb650` computes a voltage estimate, approximately:

```
percent = clamp((battery_mv - 3499) * 100 / 601, 1, 100);
```

The low endpoint is handled before unsigned subtraction. Readings at or below
3498 mV return 1 immediately. Otherwise the result is kept between 1 and 100,
and a latched software-full flag overrides it to 100.

A cached estimate prevents the indicated percentage decreasing while USB is
present or increasing while USB is absent. A proposed change must accumulate
11 mismatching calculations before updating the cache. The counter tracks
mismatches, not a particular candidate value; calls returning the current
value reset it. This is a call count, not an independent elapsed-time timer:
status/report paths also call this function.

A second function, `0xb6f4` through `0xb738`, converts percentage into ten
battery levels. A report-building path at `0xee1e` uses that coarse value;
other paths use percentage directly. The report carrying the coarse value
also sets bit 4 of that byte.

## Software full indication

`0xb4f8` evaluates this condition:

```
(usb_present && (PD1_high || battery_mv >= 4100)) || usb_counter >= usb_limit
```

Each qualifying call increments a counter. The 11th qualifying call sets a
latched full flag at RAM `0x84342b`; the counter saturates at 11. A failed
condition returns the existing latch and does not itself reset the counter.
Thus qualifying calls need not be consecutive. Other battery-state paths
clear the latch and counter when USB is absent.

The USB counter at RAM `0x843428` increases once per battery-state update
while PD0 is high, up to `usb_limit`. Without USB it decreases one count per
update until zero. It is not a fresh timer started on every cable insertion.

The initialized limit at RAM `0x842e18` is 14,400, approximately four hours
at the normal one-second update period. Startup copies that value from
flash `0x18138`, within the initialized-data block starting at `0x18120`.
This provides a timeout path to software-full even without a voltage or PD1
criterion. It does not establish that the physical charger was stopped.

A charging/full state selector at `0xb5a4` returns these values:

| Value | Meaning in the battery indication path |
|---|---|
| 0 | USB absent, low-battery latch clear |
| 1 | USB absent, low-battery latch set |
| 2 | USB present, software-full latch clear |
| 3 | Software-full latch set |

The ordinary path at `0xb748` translates these values into indicator states.
A separate mode flag can override the indicated state. No charger-control
write occurs in the voltage/percentage/full-decision routines described here.

PD1 high is a sufficient full candidate while USB is present. This is
consistent with an active-low charging-status input, but the charger part,
its wiring, and the electrical meaning of PD1 during faults are not
established. The independent 4100 mV and timeout paths mean the software-full
flag cannot be treated as a definitive physical charge-termination signal.

## Low battery and shutdown

At or below 3499 mV, the low counter advances and saturates at 2; reaching 2
sets the low-battery latch. The above-threshold counter is cleared on each
low reading. It takes 120 above-threshold updates to clear the latch. The
low counter itself is not cleared by that recovery path.

Without USB, a separate counter advances at or below 3300 mV. The 30th such
update calls the power-off routine. An above-threshold reading clears this
counter. A separate mode flag can bypass this check. USB also bypasses it.

The startup check at `0xb80c` treats 3299 mV or lower as critically low,
performs a bounded indication/recheck sequence, and calls power-off if it
remains low. This differs from the running 30-update shutdown delay.
