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

One call per poll, and it never waits.  It collects a read the bus has
finished, puts the new samples into `pad->imu`, and starts the next read when
one is due: every 4 ms for `IMU_MOTION` (accelerometer and gyro), every 2 ms
for `IMU_GYRO` (gyro only, for the mouse).  `IMU_OFF` puts the sensor to
sleep.  The mode is the controller's (from the profile's `begin()`), passed
every call.

This replaces the imu task, which ran at the lowest priority and was starved
(20 samples a second under load).  A blocking 12-byte read measured 0.6 ms,
too long for the 1 ms loop, so the transfer runs on the I2C interrupt:
`plat_i2c_start(dev, reg, buf, len)` starts it and `plat_i2c_done()` says
when the bytes are in.  (To check: the BR23 I2C interrupt.  If it cannot be
used, the fallback is a read split over several polls.)

The sensor register sequence (QMI8658A, recovered from the stock firmware) is
specific to the sensor, not the chip.

The gyro offset (zero-rate bias) is learned whenever the pad is held still,
and is removed from the samples before they go into `pad`.  It belongs to the
sensor, so it stays in this module across sessions.  Turning gyro motion into
pointer movement is the mouse profile's job: it sums the samples in its
reports.
