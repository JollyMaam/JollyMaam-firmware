# Input

Three files turn the hardware into a `struct pad`.  The controller calls
them when their inputs change (see the events in [controller.md](controller.md)).

| File | Prefix | Does | Keeps state |
|---|---|---|---|
| `input.c` | `in_` | button and d-pad bits, stick response curves, activity test | none |
| `input_calibration.c` | `cal_` | applying calibration; the calibration routine and its maths | the routine's progress |
| `input_imu.c` | `imu_` | the IMU sensor, without blocking | the bus transfer, the gyro offset |

The hardware sampling (GPIO reads, the stick UART from the stick
microcontroller, the trigger ADC) is in `platform_jieli.c`, behind one call:

```c
struct in_raw {
    u16 buttons;          /* one bit per button, active high */
    u8  hat;              /* 0..7 clockwise from up, 8 neutral */
    u8  sticks_valid;
    u16 stick[4];         /* LX LY RX RY, raw sensor counts */
    u16 trigger[2];       /* L2 R2, raw counts */
    u8  trigger_stop[2];  /* the trigger's end-stop switch */
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
    u8  imu_count;        /* samples new since the last report, oldest first */
    struct imu_sample imu[IMU_PER_REPORT];
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
void in_response(const struct set_slot *slot, struct pad *pad);
```

### Stick response curves

After calibration and dead zones, `in_response()` shapes each stick's radial
magnitude, preserving its angle. The controller applies the current slot's
settings on each input sample, after activity detection and Home gestures.
All profiles, USB and Bluetooth, receive the shaped values. Calibration and
activity detection use the calibrated values before response shaping.

`left_curve` and `right_curve` independently select:

* `linear` (default): exactly the calibrated sample.
* `concave`: output = input squared, keeping more travel near the centre.
* `s-curve`: output = 2x(1-x) below halfway, and 1-2x(1-x) above halfway.
  Sensitivity is greatest at the centre and rim, lowest around halfway.
* `custom`: linearly interpolate seven output points at input magnitudes
  12.5%, 25%, 37.5%, 50%, 62.5%, 75%, 87.5%. Centre and rim are fixed
  at 0% and 100%. Outputs are bytes 0..255, in nondecreasing order;
  repeated values permit flat sections. Default points approximate linear.

The names follow the requested feel; these are app curves, not recovered
stock-firmware formulas. All calculations use integers, with no floating point.

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
void cal_cancel(u32 now);
int  cal_step(const struct in_raw *raw, u32 now, struct set_cal *cal);   /* CAL_IDLE, CAL_RUNNING, CAL_SAVE */
int  cal_led(u32 now, struct rgb *out);        /* the routine's light; 0 if none */

void cal_apply(const struct set_cal *cal, const struct in_raw *raw, struct pad *pad);
```

The routine: hands off (settle), rest sampling, sweep the sticks and
triggers, the user presses A or B to finish, or M to cancel (`cal_step` sees the buttons in
`raw`), result.  The controller passes a copy of the current calibration;
the routine fills it, and on `CAL_SAVE` the controller stores it as the
calibration record ([settings.md](settings.md)).  `CAL_IDLE` means the
routine is over (saved or cancelled).  While it runs, the
controller keeps the link and sends the host no buttons.

`cal_apply()` is pure: the calibration it applies is passed in (the
controller gets it from `set_cal()`).  If the record is missing or from an
older format, defaults are used, and the lights and the USB status say
calibration is required.

The maths (stick and trigger mapping, the sanity checks) carries the unit
test.

## `input_imu.c`

```c
u32 imu_poll(u32 now, u8 mode);   /* advances the sensor; returns its next deadline */
int imu_take(u32 *seq, struct imu_sample *out, int max);   /* samples after *seq */
void imu_pointer(struct imu_pointer *p, const struct imu_sample *s, int n,
                 u16 per_degree, u16 accel, s32 *x, s32 *y);
```

`imu_poll()` runs on every pass of the loop and asks to be called again at
the deadline it returns.  It never waits.  It advances the sensor's state
(reset, set-up, reading, asleep) and keeps the last 8 samples.  At each
report the controller takes the ones newer than its own sequence number with
`imu_take()` into `pad.imu`.  The mode is the controller's (from the profile's
`begin()`): `IMU_MOTION` (accelerometer and gyro), `IMU_GYRO` (gyro only, the
mouse), `IMU_OFF` (sensor asleep).  This replaces the imu task, which ran at
the lowest priority and was starved (20 samples a second under load).

### Constraints

* **The bus moves one byte at a time.**  The BR23 hardware I2C has no DMA
  and no FIFO: the CPU has to act after every byte (about 22.5 µs at
  400 kHz).  A 12-byte sample is about 17 steps (start, address, register,
  restart, address, 12 bytes, stop), ~0.4 ms on the wire; the old driver
  busy-waited through it, measured 0.6 ms.
* **It can interrupt.**  The I2C block has a byte-done interrupt (CON0 bit 8)
  and an end-of-transfer interrupt (bit 10), vector `IRQ_IIC_IDX` (22), per
  the SDK's `iic_hw.h`.  The SDK uses them only in slave mode; master mode on
  interrupts is untested here.
* **Gaps cost nothing.**  The master drives the clock, so the sensor waits
  for each next byte however late it comes.  A delayed interrupt (the radio's
  have priority) stretches the transfer and corrupts nothing.
* **New data has to be detected.**  The QMI8658A samples on its own clock
  (~448 Hz, about every 2.2 ms).  Reading on our own timer drifts against it:
  it repeats a sample now and then and misses another.  The sensor can signal data-ready on its INT1/INT2 pins, but no wiring
  from either to the BR23 has been found.
* **The bus is shared** with an EEPROM at 0x50 (unused): one transfer at a
  time.  The pull-up values are unknown; 400 kHz has been error-free.
* **Set-up needs a wait:** 20 ms after the soft reset.  It is a deadline the
  state machine checks, not a delay.

### How a transfer moves on the bus

The BR23 I2C block (`JL_IIC`: `CON0`, `CON1`, an 8-bit `BUF`, `BAUD`) holds
**one byte**.  There is no queue.  The CPU drives every step:

| Step | CPU does | Hardware does | Then |
|---|---|---|---|
| start | set `PRESET_START` | puts START on the bus with the next byte | |
| send a byte | write `BUF`, set `CONFIG_DONE` | clocks out 8 bits, reads the sensor's ACK bit (9th clock) | `PENDING`; `SENT_NACK` if the sensor did not ACK |
| receive a byte | choose ACK or NACK (`RECEIVE_NACK`), write `BUF`, set `CONFIG_DONE` | clocks in 8 bits, sends our ACK/NACK on the 9th clock | `PENDING`; the byte is in `BUF` |
| stop | set `PRESET_STOP` + `CONFIG_DONE` | puts STOP on the bus | `STOP_PENDING` |

So the hardware generates the ACK itself, but for **one byte at a time**: it
ACKs (or NACKs) the byte it was told to receive, then stops and waits for the
CPU to collect `BUF` and start the next byte.  The CPU decides ahead of each
byte whether it will be ACKed: every byte but the last of a read, which is
NACKed to tell the sensor to stop sending.  Between bytes the bus simply
waits, which is why a late interrupt does no harm.

Each `PENDING` is one interrupt when the byte-done interrupt is enabled.  So:

**interrupts per second ≈ samples per second × bus steps per sample**

| Read | Bus steps | Interrupts per sample |
|---|---|---|
| gyro only, `0x3B`–`0x40` (6 bytes) | address, register, address, 6 bytes, stop | 10 |
| accelerometer + gyro, `0x35`–`0x40` (12 bytes) | address, register, address, 12 bytes, stop | 16 |
| status + both, `0x2E`–`0x40` (19 bytes) | address, register, address, 19 bytes, stop | 23 |

The earlier "about 10 000 a second" was the worst row at the rate the sensor
is set to now: CTRL2 `0x14` and CTRL3 `0x74` set both accelerometer and gyro
to 448.4 Hz, and 448 × 23 ≈ 10 300.  Each interrupt is a few instructions from
RAM; the cost is small, but it is not free, and it is mostly avoidable.

### Keeping it down

The count is the sample rate times the bytes read.  So:

* **Read only what the profile uses.**  The mouse needs the gyro (6 bytes);
  the Switch and PlayStation profiles need both (12).
* **Run the sensor at the rate the profile needs, not 448 Hz.**  The
  QMI8658A offers 896.8, 448.4, 224.2, 112.1 Hz and lower.  A Switch report
  carries 3 samples every 15 ms (200 a second): 224.2 Hz.  PlayStation and the
  mouse: 224.2 Hz too, until something shows it needs more.  The mode the
  profile asks for in `begin()` carries the rate.
* **Status only if needed.**  Without a data-ready pin, the status bytes are
  how a read knows its sample is new.  With the rate known, reads can instead
  be timed just after each expected sample, and a repeated sample (same
  values) dropped; status costs 7 more steps per sample, so it is the
  fallback if timing proves unreliable.

At 224.2 Hz with both sensors and no status that is 224 × 16 ≈ 3 600
interrupts a second; gyro only, ≈ 2 200.  A data-ready pin, if wired, would
not change the count (the data still moves a byte at a time); it would make
every read find new data.

### Platform side

`platform_jieli.c` owns the I2C block and its interrupt:

```c
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);   /* starts; returns at once */
void plat_i2c_write(u8 dev, u8 reg, u8 value);         /* starts; returns at once */
int  plat_i2c_done(void);       /* 1 finished, 0 still going, <0 NACK or timeout */
```

The handler does one step per interrupt (the table above), from RAM, and
raises `EV_IMU` when the transfer ends.  The 250 µs tick also steps a
transfer whose interrupt has not come, and fails one stuck for 2 ms, so the
driver works even if the master-mode interrupt does not.

### Lifecycle

`input_imu.c` is a small state machine.  The interrupt advances a transfer;
`imu_poll()` (on `EV_IMU` or its deadline) advances the states.

```
            mode != OFF
   OFF ──────────────────> RESET ── write 0x60=0xB0 (soft reset)
    ^                        │ deadline: +20 ms
    │                        v
    │                    CONFIGURE ── write the set-up registers, one transfer each,
    │                        │        with this mode's rate and sensors (CTRL7)
    │                        v
    │   mode == OFF      RUNNING ── at each expected sample time: start a read;
    └── write CTRL7=0 ──     │      the interrupt chains its steps; done → EV_IMU
                             │      new sample → into pad, next deadline = +1 period
                             │ NACK, timeout, or a sensor that stops answering
                             v
                           RESET (after a back-off)
```

* `OFF`: the sensor is asleep (CTRL7 = 0) and the bus is idle.
* A mode change (motion ↔ gyro) resets the sensor and sets it up again.
* Register writes are transfers like reads (address, register, value, stop:
  4 interrupts each), so set-up never blocks either.
* Only one transfer is ever on the bus; the state machine starts the next one
  when the last has finished.

(To check on hardware: whether the master-mode interrupt fires or the tick
does the work; whether a data-ready pin is wired; that timed reads at
224.2 Hz stay in step with the sensor's clock.)

The sensor register sequence (recovered from the stock firmware) is specific
to the QMI8658A, not the chip.

The gyro offset (zero-rate bias) is learned whenever the pad is held still,
and removed from the samples before they go into `pad`.  It belongs to the
sensor, so it stays in this module across sessions.  `imu_pointer()` turns
gyro samples into pointer movement (with acceleration and the remainder
carried); the mouse profile calls it on the samples in each report.
