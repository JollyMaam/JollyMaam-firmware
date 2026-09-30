#include <system/includes.h>

#include "gyro.h"
#include "imu.h"

/* The QMI8658A as imu.c sets it up (CTRL3 0x74): +-2048 dps full scale,
 * 16 LSB per degree per second. */
#define GYRO_LSB_PER_DPS        16

/* Which sensor axis turns the pointer, and which way.  Lying face up on a
 * table the accelerometer reads +1 g on z (measured 2026-09-29), so z is
 * yaw; turning right is clockwise seen from above, negative about +z. */
#define GYRO_YAW_AXIS           2
#define GYRO_YAW_SIGN           (-1)
#define GYRO_PITCH_AXIS         1
#define GYRO_PITCH_SIGN         (-1)

/* Offset learning.  Samples are looked at in windows; a window whose
 * readings stay within GYRO_STILL_SPREAD of each other on every axis is the
 * pad held still, and within GYRO_REST_SPREAD it is the pad lying on
 * something.  Lying down, the window's mean is the offset.  Held, hands
 * still shake and may be turning slowly on purpose, so after the first
 * offset only a small correction is taken, a quarter at a time. */
#define GYRO_WINDOW             32U     /* ~0.3 s at the IMU task's rate */
#define GYRO_REST_SPREAD        (10 * 16)                    /* 1/16 LSB */
#define GYRO_STILL_SPREAD       (3 * GYRO_LSB_PER_DPS * 16)  /* 3 dps */
#define GYRO_DRIFT_MAX          (1 * GYRO_LSB_PER_DPS * 16)  /* 1 dps */

/* Below this rate motion is scaled down, quadratically to zero: what is left
 * of the offset and of the hands' shake does not creep the pointer. */
#define GYRO_TIGHTEN            (GYRO_LSB_PER_DPS * 16 / 2)  /* 0.5 dps */

/* A sample more than this after the last one (the sensor was restarted, or
 * reads failed) is integrated as if it were this late. */
#define GYRO_MAX_DT_MS          50U

/* Everything in 1/16 LSB, so the offset can move by less than one LSB. */
static s32 bias[3];
static u8 calibrated;
static u32 rests;

static s32 window_sum[3];
static s32 window_min[3];
static s32 window_max[3];
static u8 window_count;

static u32 last_sample_ms;
static u8 have_last;
static s32 motion[2];           /* 1/16 LSB times milliseconds */
static s64 remainder[2];

void cyclone2_gyro_reset(void)
{
    have_last = 0U;
    window_count = 0U;
    motion[0] = motion[1] = 0;
    remainder[0] = remainder[1] = 0;
}

static s32 absolute(s32 value)
{
    return value < 0 ? -value : value;
}

static void learn_offset(const s32 rate[3])
{
    s32 mean[3];
    s32 spread = 0;
    int axis;

    for (axis = 0; axis < 3; axis++) {
        if (!window_count || rate[axis] < window_min[axis])
            window_min[axis] = rate[axis];
        if (!window_count || rate[axis] > window_max[axis])
            window_max[axis] = rate[axis];
        window_sum[axis] = (window_count ? window_sum[axis] : 0) + rate[axis];
    }
    if (++window_count < GYRO_WINDOW) return;
    window_count = 0U;

    for (axis = 0; axis < 3; axis++) {
        mean[axis] = window_sum[axis] / (s32)GYRO_WINDOW;
        if (window_max[axis] - window_min[axis] > spread)
            spread = window_max[axis] - window_min[axis];
    }
    if (spread > GYRO_STILL_SPREAD) return;
    if (spread <= GYRO_REST_SPREAD || !calibrated) {
        for (axis = 0; axis < 3; axis++) bias[axis] = mean[axis];
    } else {
        for (axis = 0; axis < 3; axis++)
            if (absolute(mean[axis] - bias[axis]) > GYRO_DRIFT_MAX) return;
        for (axis = 0; axis < 3; axis++)
            bias[axis] += (mean[axis] - bias[axis]) / 4;
    }
    calibrated = 1U;
    rests++;
}

static s32 tighten(s32 rate)
{
    if (absolute(rate) >= GYRO_TIGHTEN) return rate;
    return rate * absolute(rate) / GYRO_TIGHTEN;
}

static void take_sample(void)
{
    struct cyclone2_imu_sample sample;
    s32 rate[3];
    u32 dt;

    if (cyclone2_imu_latest(&sample) != 0) return;
    if (have_last && sample.timestamp_ms == last_sample_ms) return;
    dt = sample.timestamp_ms - last_sample_ms;
    if (dt > GYRO_MAX_DT_MS) dt = GYRO_MAX_DT_MS;
    last_sample_ms = sample.timestamp_ms;

    rate[0] = (s32)sample.gyro_x * 16;
    rate[1] = (s32)sample.gyro_y * 16;
    rate[2] = (s32)sample.gyro_z * 16;
    learn_offset(rate);
    if (!have_last) {
        have_last = 1U;
        return;
    }
    if (!calibrated) return;
    motion[0] += GYRO_YAW_SIGN *
                 tighten(rate[GYRO_YAW_AXIS] - bias[GYRO_YAW_AXIS]) * (s32)dt;
    motion[1] += GYRO_PITCH_SIGN *
                 tighten(rate[GYRO_PITCH_AXIS] - bias[GYRO_PITCH_AXIS]) *
                 (s32)dt;
}

void cyclone2_gyro_take(u16 per_degree, s32 *x, s32 *y)
{
    /* motion is 1/16 LSB * ms; a degree is GYRO_LSB_PER_DPS * 16 * 1000. */
    const s64 per_count = (s64)GYRO_LSB_PER_DPS * 16 * 1000;
    s32 *out[2];
    int axis;

    take_sample();
    out[0] = x;
    out[1] = y;
    for (axis = 0; axis < 2; axis++) {
        s64 total = (s64)motion[axis] * per_degree + remainder[axis];
        s32 counts = (s32)(total / per_count);

        remainder[axis] = total - (s64)counts * per_count;
        motion[axis] = 0;
        *out[axis] = counts;
    }
}

void cyclone2_gyro_get_status(struct cyclone2_gyro_status *status)
{
    int axis;

    status->calibrated = calibrated;
    for (axis = 0; axis < 3; axis++) status->bias[axis] = (s16)(bias[axis] / 16);
    status->rests = rests;
}
