# Stock battery measurement and indications

Evidence from the stock Nova 2 Lite pad application, PID `AC695X_TRANS`, VID
`N2LT`. Address mapping is in [re-stock-power.md](re-stock-power.md).

## Voltage conversion

The VBAT channel is `0x0050000f`, the BR23's internal quarter-battery input.
The stock conversion at `0x01e3f794` uses a bandgap reference of 1168 mV,
adjusted by the chip's trim in steps of 3.2 mV. It multiplies the battery
ADC code by the trimmed reference and divides by the reference ADC code.
The caller at `0x01e410be..0x01e410cc` multiplies the result by four.
The path at `0x01e43dec..0x01e43e4c` also multiplies by four and divides by
ten for an internal value in 10 mV units. This division is a change of
units, not an additional battery divider.

There is no additional board-specific voltage multiplier in these paths.
Calibration against physical cell voltage is not established.

Stock sampling differs from the SDK's background VBAT average. The getter at
`0x01e3bbb4` takes eight VBAT readings, discards the minimum and maximum,
and divides the remaining sum by six. The application also has voltage
smoothing and charging-dependent adjustment before its percentage and state
decisions. Those indication values are separate from a measured voltage.

## Percentage estimate

The routine at `0x01e3f92e` uses piecewise linear interpolation through:

| Voltage | Estimated charge |
|---|---|
| 3550 mV | 0% |
| 3750 mV | 15% |
| 3900 mV | 55% |
| 4130 mV | 100% |

Lower and higher voltages are clamped to 0 and 100. Interpolation uses the
signed integer helper at `0x01e360a4` and truncates the division.

After uptime reaches 5001 ms, a nonzero cached percentage prevents a decrease
while PA0 indicates USB and prevents an increase without USB. A zero cache is
initialized from the current estimate. The cache is SRAM `0x6b92`. The caller
at `0x01e413dc..0x01e4140e` additionally forces 100%
when the software charge-state routine returns state 4.

This is a firmware estimate, not a measured capacity or independently
validated state-of-charge curve.

## USB and charging indication

The startup sequence at `0x01e25fce..0x01e25fda` configures PA0 as an input
without pulls, and GPIO 56 (`IO_PORT_PR_00`, PR0) as a pulled-up input.
The common input helper is `0x01e366e2`. For PR0 it selects P33 `0x02`
bits 0..2 = 1, sets the RTC port direction to input, enables its digital
input, disables its pull-down and enables its pull-up. The RTC pin helpers
at `0x01e3fbc6`, `0x01e3fbec`, `0x01e3fc24` and `0x01e3fc5c` are SDK
library matches.

The ordinary USB-present branch at `0x01e3f7e6..0x01e3f80e` reads PR0 and
returns state 3 when PR0 is low and voltage is below 4150 mV. It returns
state 4 when PR0 is high or voltage is at least 4150 mV. With USB absent it
normally returns 0. Other mode and transition branches can override this.

The caller identifies state 3 as charging and state 4 as software-full by
choosing the corresponding battery indication and forcing the latter's
percentage to 100. PR0 is therefore the stock charging-status input, with
low selecting active charging. The charger part, its fault signalling, and
whether PR0 high alone proves physical charge completion are not established.
The voltage-based full override is also not proof of charge termination.

## Low-voltage indication

The application classifies voltage above 3750 mV as state 1, 3551..3750 mV
as state 5, and 3550 mV or below as state 6 when its charging/full branches
do not apply (`0x01e41400`, `0x01e414b2`, `0x01e416f2`). State 6 selects
zero percent. The state dispatcher at `0x01e416f6..0x01e4173a` notifies its
registered callbacks after three subsequent matching classifications; a
changed classification first resets that counter.

The subscriber at `0x01e44b92`, registered at `0x01e40f4a..0x01e40f5a`,
identifies states 5 and 6 as `BAT_EVT_LOW_POWER` and `BAT_EVT_POWER_PROTECT`.
The state-6 branch at `0x01e44be4..0x01e44bf4` calls `0x01e35ad0`. That
routine selects application power state 2 without USB and state 1 with USB.
The main loop handles state 2 at `0x01e427ae` and follows its shutdown path
to `0x01e4296e`, which calls the soft-off wrapper `0x01e4064c`.

Thus the ordinary battery protection path uses the 3550 mV boundary on the
application's filtered/adjusted voltage, with repeated-state confirmation.
A separate emergency cutoff on raw instantaneous ADC voltage, and all mode
exceptions to the ordinary path, are not established.
