# Input

Three files turn the hardware into one `struct pad` per millisecond.  The
controller calls each of them once per poll.

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
    struct imu_sample imu[IMU_PER_POLL];  /* samples new since the last poll */
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

No debouncing: buttons are taken as read each millisecond.  The worst bounce
measured is ~0.4 ms (`docs/peripherals.md`), so a sample can occasionally land
inside one and show a press or release lasting a single poll.  If that shows
up (a doubled press, a Home tap that ends a chord early), debouncing comes back
here; it would also be needed if buttons were sampled faster.

The idle-sleep timer is the controller's: it keeps a reference `pad` and the
time of the last activity, and asks `in_moved()` whether the pad moved since.

## `input_calibration.c`

The calibration routine runs itself; the controller only starts it, feeds it
each poll, and cancels it.

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
void imu_poll(u32 now, enum imu_mode mode, struct pad *pad);
```

One call per poll, and it never waits.  It advances the sensor's state
(set-up, sleeping, idle, reading) and puts the samples that arrived since the
last poll into `pad->imu`.  The mode is the controller's (from the profile's
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

A read starts only when there is something to read.  In order of preference:

1. **Data-ready pin**, if one turns out to be wired: enable data-ready on
   INT1 and INT2 and look for a GPIO that toggles at the sample rate.  Then an
   edge (seen by `plat_input()` or its own interrupt) starts the read, and the
   bus is idle otherwise.
2. **Status, then data:** read the 1-byte status register (about 5 bytes on
   the bus, ~0.12 ms); if it says new data, read the sample.  Status checks
   start about one sample period after the last sample, so it is one or two
   checks per sample.
3. **The sensor's FIFO:** read its fill level, then exactly the samples in
   it.  Fewer transfers and no repeated or lost samples, at the cost of more
   set-up.

Either way the loop never waits: it starts a transfer and finds it finished on
a later poll.  (To check on hardware: the master-mode interrupt; whether a
data-ready pin is wired.)

The sensor register sequence (recovered from the stock firmware) is specific
to the QMI8658A, not the chip.

The gyro offset (zero-rate bias) is learned whenever the pad is held still,
and removed from the samples before they go into `pad`.  It belongs to the
sensor, so it stays in this module across sessions.  Turning gyro motion into
pointer movement is the mouse profile's job: it sums the samples in its
reports.
