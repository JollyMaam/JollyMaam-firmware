# Input

Three files turn the hardware into a `struct pad`.  The controller calls
them when their inputs change (see the events in [controller.md](controller.md)).

| File | Prefix | Does | Keeps state |
|---|---|---|---|
| `input.c` | `in_` | button and d-pad bits, stick and trigger scaling, activity test | none |
| `input_calibration.c` | `cal_` | applying calibration; the calibration routine and its maths | the routine's progress |
| `input_imu.c` | `imu_` | the IMU sensor, without blocking | the bus transfer, the gyro offset |

The hardware sampling (GPIO reads, the stick UART from the stick
microcontroller, the trigger ADC) is in `platform_jieli.c`, behind one call:

```c
struct in_raw {
    u16 buttons;          /* one bit per button, active high */
    u8  hat;              /* 0..7 clockwise from up, 8 neutral */
    u16 stick[4];         /* LX LY RX RY, raw sensor counts */
    u16 trigger[2];       /* L2 R2, raw ADC counts */
    u8  sticks_valid;
};
void plat_input(struct in_raw *raw);
```

Everything in the three input files is portable and testable; another board
only has to fill `struct in_raw`.

## `struct pad`

What the rest of the firmware sees:

```c
struct pad {
    u16 buttons;          /* PAD_A, PAD_B, ... PAD_HOME, PAD_M, ... */
    u8  hat;
    s16 lx, ly, rx, ry;   /* -32767..32767, calibrated, dead zones applied */
    u16 l2, r2;           /* 0..65535 */
    struct imu_sample imu[IMU_PER_REPORT]; /* samples new since the last report */
    u8  imu_count;
};
```

Button bit positions follow Linux's HID button order so SDL maps the generic
profile without a database entry.  Face buttons are named by the printed
labels (Xbox order: A south, B east, X west, Y north); profiles that want
Nintendo positions translate.

## `input.c`

```c
void in_map(const struct in_raw *raw, struct pad *pad);   /* bits as read */
int  in_moved(const struct pad *now, const struct pad *ref);   /* more than drift */
```

### Sampling and debounce

Buttons are sampled by a platform timer interrupt every 250 µs: it reads the
GPIO port registers (a few register reads), applies the debounce, and raises
`EV_BUTTONS` only when the debounced state changed.  Per-button pin
interrupts are not possible: the BR23's edge-detect inputs go through a few
wake-up channels (selected in `JL_IOMAP`), far fewer than the ~20 button and
d-pad lines.

At 250 µs a bounce (worst measured ~0.4 ms, `docs/peripherals.md`) is seen
nearly every time, so the eager debounce stays: a pin that changes is taken at
once, and then ignored for 2 ms while it bounces.  It adds no latency.  It
lives in the sampling interrupt, so the loop only ever sees clean changes.

The idle-sleep timer is the controller's: it keeps a reference `pad` and the
time of the last activity, and asks `in_moved()` whether the pad moved since.

## `input_calibration.c`

The calibration routine runs itself; the controller only starts it, feeds it
each time the inputs change, and cancels it.

```c
void cal_start(u32 now);
enum cal_result cal_step(const struct in_raw *raw, u32 now);   /* CAL_RUNNING, CAL_DONE, CAL_FAILED */
void cal_cancel(void);
void cal_result(struct set_cal *out);          /* after CAL_DONE */
int  cal_led(u32 now, u8 rgb[3]);              /* the routine's light; 0 if none */

void cal_apply(const struct set_cal *cal, const struct in_raw *raw, struct pad *pad);
```

The routine: hands off (settle), rest sampling, sweep the sticks and
triggers, the user presses A to finish (`cal_step` sees the buttons in
`raw`), result.  On `CAL_DONE` the controller stores the result as the
calibration record ([settings.md](settings.md)).  While it runs, the
controller keeps the link and sends the host no buttons.

`cal_apply()` is pure: the calibration it applies is passed in (the
controller gets it from `set_cal()`).  If the record is missing or from an
older format, defaults are used, and the lights and the USB status say
calibration is required.

The maths (stick and trigger mapping, the sanity checks) carries the unit
test.

## `input_imu.c`

```c
void imu_poll(u32 now, enum imu_mode mode, struct pad *pad, u32 *next_ms);
```

Called on `EV_IMU` and at the deadline it asks for (`next_ms`: when to start
the next read).  It never waits.  It advances the sensor's state
(set-up, sleeping, idle, reading) and puts the samples that arrived since the
last call into `pad->imu`.  The mode is the controller's (from the profile's
`begin()`): `IMU_MOTION` (accelerometer and gyro), `IMU_GYRO` (gyro only, the
mouse), `IMU_OFF` (sensor asleep).  This replaces the imu task, which ran at
the lowest priority and was starved (20 samples a second under load).

### Constraints

* **The bus moves one byte at a time.**  The BR23 hardware I2C has no DMA
  and no FIFO: the CPU has to act after every byte (about 22.5 µs at
  400 kHz).  A 12-byte sample is about 17 steps (start, address, register,
  restart, address, 12 bytes, stop), ~0.4 ms on the wire; the current driver
  busy-waits through it, measured 0.6 ms.
* **It can interrupt.**  The I2C block has a byte-done interrupt (CON0 bit 8)
  and an end-of-transfer interrupt (bit 10), vector `IRQ_IIC_IDX` (22), per
  the SDK's `iic_hw.h`.  The SDK uses them only in slave mode; master mode on
  interrupts is untested here.
* **Gaps cost nothing.**  The master drives the clock, so the sensor waits
  for each next byte however late it comes.  A delayed interrupt (the radio's
  have priority) stretches the transfer and corrupts nothing.
* **New data has to be detected.**  The QMI8658A samples on its own clock
  (~448 Hz, about every 2.2 ms).  Reading on our own timer drifts against it:
  it repeats a sample now and then and misses another, which today's driver
  does.  The sensor can signal data-ready on its INT1/INT2 pins, but no wiring
  from either to the BR23 has been found.
* **The bus is shared** with an EEPROM at 0x50 (unused): one transfer at a
  time.  The pull-up values are unknown; 400 kHz has been error-free.
* **Set-up needs a wait:** 20 ms after the soft reset.  It is a deadline the
  state machine checks, not a delay.

### Design

The transfer runs on the interrupt.  `platform_jieli.c` owns the I2C block
and offers:

```c
void plat_i2c_start(u8 dev, u8 reg, u8 *buf, u8 len);   /* read; returns at once */
int  plat_i2c_done(void);       /* 1 finished, 0 still going, <0 error (NACK, timeout) */
void plat_i2c_write(u8 dev, u8 reg, u8 value);          /* same, a one-register write */
```

The interrupt handler is a few instructions per byte, runs from RAM, and does
nothing but advance the transfer.

A read starts only when there is something to read.

1. **Data-ready pin**, if one turns out to be wired: enable data-ready on
   INT1 and INT2 and look for a GPIO that toggles at the sample rate.  If one
   does, its edge starts the read and the bus is idle otherwise.
2. **Otherwise, status and data in one read.**  The sensor auto-increments
   register addresses (enabled by the set-up), so one burst from STATUS0
   (`0x2E`) through the last gyro byte (`0x40`) returns the sensor's status
   and the sample together, 19 bytes, ~0.5 ms of bus time.  STATUS0 says
   whether the sample is new; an old one is dropped.  The burst is started
   about one sample period (~2.2 ms) after the last new sample, so most bursts
   find one.

The interrupt chains the transfer byte by byte and raises `EV_IMU` only when
a new sample is in.  The CPU cost is a few microseconds per byte; the ~0.5 ms
is bus time, during which the CPU does other work.

The 1-byte status register is the sensor's (QMI8658A STATUS0), read over the
bus like any register.  A single-register read costs about 5 bytes on the bus
(start, address, register, restart, address, the byte, stop), ~0.12 ms, which
is why it is folded into the data burst rather than read on its own.  The
I2C block's own "byte done" flag is a local chip register and costs nothing.

The sensor's FIFO is not used: reading it is also a transfer, and it only
pays off when several samples are fetched at once, which these rates do not
need.

(To check on hardware: the master-mode interrupt; whether a data-ready pin is
wired; the exact STATUS0 bits for "new gyro" and "new accelerometer".)

The sensor register sequence (recovered from the stock firmware) is specific
to the QMI8658A, not the chip.

The gyro offset (zero-rate bias) is learned whenever the pad is held still,
and removed from the samples before they go into `pad`.  It belongs to the
sensor, so it stays in this module across sessions.  Turning gyro motion into
pointer movement is the mouse profile's job: it sums the samples in its
reports.
