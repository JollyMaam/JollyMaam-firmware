# Charging and the battery

What this firmware does about charging, what the stock firmware did, and what
is still unknown.  Written after an incorrect claim that the pad "does not
charge" - see *A correction* at the end.

## What this firmware does: nothing

`board_init()` in `src/init.c` does:

    adc_vbg_init();
    adc_init();
    devices_init();

    /*close FAST CHARGE */
    CHARGE_EN(0);
    CHGBG_EN(0);

`CHARGE_EN` and `CHGBG_EN` are bits 0 and 1 of the PMU's charge register
(`P3_CHG_CON0`, register 0x15 in the P33 space), i.e. they control the **BR23's
own charge controller**.  Both are cleared, and nothing anywhere in the port
calls `charge_api_init()`, so the charge driver is never brought up.

Consequences:

* the pad has no charge management from this firmware at all - no charge
  current, no full-voltage setting, no charge state machine;
* `get_charge_online_flag()` reads 0 and `get_charge_poweron_en()` reads 0xff,
  because the driver's state is uninitialised - they say nothing about the
  actual board;
* `get_ldo5v_online_hw()`, `adc_check_vbat_lowpower()` and
  `get_charge_full_value()` **block** when called in this state, which hangs the
  console in the middle of a command.  The `battery` command deliberately
  avoids them for that reason.

The `battery` command therefore reports:

    battery vbat=4760 mV charge=0 poweron=255

Read that as "charge driver not running", not "the charger is off" - and note
the VBAT figure is `adc_get_voltage(AD_CH_VBAT) * 4` (the channel is vbat/4),
which lands at 4.76 V, above anything a single Li-ion cell can sit at.  It is
not a usable battery measurement.

## What the stock firmware did

The stock 3.52 firmware (`decompilation/`, cross-checked against the flash dump
`cyclone2-flash-backups/stock-3.52-20260921-141040.bin`) carries a real charge
and *pedestal* subsystem.  Its strings include:

    power on pedestal ad = %d          (an ADC reading - the pedestal/dock)
    pedestal status change : %d %d
    pedestal insert charge!            pedestal 5v insert reset
    open pedestal power                close pedestal power
    wire pc power on no charge         wire pc power on have charge1..5
    wire pc no charge enter sleep      wire pc fun config 0 start charge mode
    test charge!                       f_charge = %d
    charge enter sleep ps key check start
    user charge set enter pair mode soft reset
    pair charge soft reset             reconnect charge enter sleep mode soft reset
    STATUS_WIRELESS_MODE_CHARGE_NO_BT_RUN remove usb 5v enter sleep

So on the stock firmware charging state feeds into startup, sleep and pairing
decisions, and there is a charging dock ("pedestal") the firmware both detects
(via the ADC) and powers on and off.

The power-on path is `FUN_01e3a806`: it reads keys, prints `test charge!`,
computes `f_charge`, waits for a 4-byte status block, and branches on it into
the charge/mode state machine.  That much is visible in the Ghidra export.

## What the evidence does *not* settle

Whether the charge current is delivered by the BR23's internal controller or by
an external charger IC.  The pedestal strings above are almost all
*unreferenced* in the recovered functions, so the register writes that would
answer it are not in the export.  What can be said:

* for the internal controller, nothing in this port enables it, and the SoC's
  VBAT sense reads an impossible voltage - as if it were not connected to the
  cell;
* against the internal controller, the stock firmware clearly has an
  external-charger-shaped subsystem (a powered dock, an ADC-detect, explicit
  charge states) and the "close FAST CHARGE" call in our port looks like the
  deliberate disabling of the SoC charger that an external IC would require.

On balance an external charger is the better fit, but that is an inference, not
something the disassembly proves.

The strongest evidence remains the bench: **the pad runs fine unplugged, so its
cell is charged and healthy**, which means the board charges somehow without
this firmware's involvement.

## Settling it

In order of directness:

1. look at the board - a charger IC (BQ*/IP5*/ETA*/SY* marking) near the
   battery connector, next to a coil and a pair of resistors, is an external
   charger;
2. leave the pad plugged in for an hour and check whether the unplugged runtime
   improves, or put a meter on the USB port while it sits idle and charging;
3. scan the stock firmware for I2C traffic to a charger address, or for writes
   to P33 `P3_CHG_CON0..3` (0x15..0x1d), which would settle internal vs
   external.  The Ghidra project
   (`/work/analysis/cyclone2-fw-3.52-two-segment-v2.gpr`, not present in every
   checkout) is the right tool for that.

## A correction

An earlier commit added the `battery` command with a message claiming "this
firmware neither charges the battery nor can report its state".  The second half
is true; the first was an inference from *our own* `CHARGE_EN(0)` and was wrong
to state as fact, because disabling the internal charger is exactly what a
board with an external one would do.  The pad working unplugged is direct
evidence against it.

So: nothing in this firmware charges the battery or reports its level, and that
says nothing about whether the pad charges.  Do not use `battery` to decide
whether the pad is charging.
