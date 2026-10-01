# Input

Three files turn the hardware into one `struct pad` per millisecond:

| File | Prefix | Does |
|---|---|---|
| `input.c` | `in_` | debounce, d-pad, stick and trigger mapping, activity detection |
| `input_calibration.c` | `cal_` | applying calibration; the calibration routine and its maths |
| `input_imu.c` | `imu_` | the IMU sensor over I2C; gyro as a pointing device |

The hardware sampling itself (GPIO reads, the stick UART from the stick
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

So everything in the three input files is portable and testable; another
board only has to fill `struct in_raw`.

## `struct pad`

What the rest of the firmware sees:

```c
struct pad {
    u16 buttons;          /* PAD_A, PAD_B, ... PAD_HOME, PAD_M, ... */
    u8  hat;
    s16 lx, ly, rx, ry;   /* -32767..32767, calibrated, dead zones applied */
    u16 l2, r2;           /* 0..65535 */
    struct imu_sample imu[IMU_HISTORY];  /* newest last; count in imu_count */
    u8  imu_count;
};
```

Button bit positions follow Linux's HID button order so SDL maps the generic
profile without a database entry.  Face buttons are named by the printed
labels (Xbox order: A south, B east, X west, Y north); profiles that want
Nintendo positions translate.

## `input.c`

```c
void in_init(void);
void in_read(struct pad *pad, u32 now_us);   /* debounced digital + mapped analog */
int  in_active(const struct pad *pad);       /* moved more than drift: resets idle sleep */
```

Debounce is eager: a contact that changes is taken at once, then held 2 ms
while it bounces (worst bounce measured ~0.4 ms).  Waiting for it to settle
first cost every press 10–20 ms.

## `input_calibration.c`

```c
void cal_apply(const struct in_raw *raw, struct pad *pad);
int  cal_start(void);
void cal_finish(void);
void cal_cancel(void);
void cal_step(u32 now, u16 buttons);   /* drives the routine from the poll */
int  cal_running(void);
int  cal_led(u32 now, u8 rgb[3]);      /* the routine's light, 0 if none */
```

The routine: hands off (settle), rest sampling, sweep the sticks and triggers,
result.  The result is stored as the calibration record
([settings.md](settings.md)); if that record is missing or from an older
format, defaults are used and the console and lights say calibration is
required.

The maths (`cal_stick_map`, `cal_trigger_map`, the sanity checks) is pure and
carries the unit test.

## `input_imu.c`

```c
void imu_task(void);                       /* the imu thread's body */
void imu_tick(void);                       /* from ctl_poll: paces sampling */
void imu_mode(enum imu_mode m);            /* IMU_OFF, IMU_MOTION (4 ms), IMU_GYRO (2 ms) */
int  imu_since(u32 seq, struct imu_sample *out, int max);
void imu_pointer(u16 per_degree, u16 accel, s32 *x, s32 *y);   /* gyro mouse */
void imu_pointer_reset(void);
```

Profiles choose the mode in `begin()` (PlayStation and Switch: motion; gyro
mouse: gyro only; others: off).  The sensor register sequence is specific to
the sensor, not the chip; the bus is `plat_i2c_read()`/`plat_i2c_write()`.
The pointer learns the gyro's zero-rate offset whenever the pad is held still.
