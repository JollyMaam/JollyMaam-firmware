# Power, sleep and wake

The chip's charger register, reset sources, retained memory and loader entry
are in [../jieli/power.md](../jieli/power.md).

## Charging

Not established: the external charger IC, its wiring, the charge termination
method, and the battery capacity.

## Sleep and wake inputs

| Pin | Function | Configuration |
|---|---|---|
| PB11 (Home) | wake | filtered falling-edge, pull-up |
| PC3 | wake | filtered rising-edge; kept as an input during soft power-off |

Whether USB insertion drives PC3 and wakes the pad is not established.

The low-power configuration is in [../jieli/soc.md](../jieli/soc.md). Sleep current has not been
measured.

Stock 3.52's soft-off GPIO callback, wake table and PMU shutdown path are
described in [re-stock-firmware.md](re-stock-firmware.md#soft-power-off-stock-352).
Its wake records use the same Home and PC3 edges but have their per-port
filter flags clear. The table above describes this firmware's configuration.

### USB presence signals

PC3 reads low while USB is attached during normal app operation. The OTG
state is `IDLE_MODE` and the PMU 5 V detector reads low in the same condition.
None of these signals distinguishes an attached cable from none. Software can
detect an active host from USB start-of-frame (SOF) activity instead.

A power-only USB source cannot currently be told apart from a disconnected
cable.

## Reset pin

PB1 is the configured reset input, active low. `CONFIG_RESET_TIME` is 8. This
is the configuration value. The resulting duration is not established.

