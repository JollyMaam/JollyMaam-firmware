#include <system/includes.h>

#include "gyro.h"
#include "imu.h"

/* The QMI8658A as imu.c sets it up (CTRL3 0x74): +-2048 dps full scale,
 * 16 LSB per degree per second. */
#define GYRO_LSB_PER_DPS        16

/* Which sensor axis turns the pointer, and which way.  Lying face up on a
 * table the accelerometer reads +1 g on z (measured 2026-09-29), so z is
 * yaw; turning right is clockwise seen from above, negative about +z.
 * Pitch and the signs are as they felt right in use (2026-09-29). */
#define GYRO_YAW_AXIS           2
#define GYRO_YAW_SIGN           (-1)
#define GYRO_PITCH_AXIS         1
#define GYRO_PITCH_SIGN         (-1)

/* Offset learning.  Samples are looked at in windows; a window whose
 * readings stay within a spread of each other on every axis is the pad held
 * still, and its mean is the offset.  In three steps:
 *
 * - none yet: a short window held roughly still (GYRO_QUICK_*) gives a
 *   coarse offset at once, so pointing starts as the pad is picked up.
 *   Waiting for a quarter of a second within 3 dps took a long while in the
 *   hands (seen 2026-09-30).
 * - coarse: the next long window held still (GYRO_STILL_SPREAD) replaces it.
 * - fine: lying on something (GYRO_REST_SPREAD) still replaces it; held,
 *   hands shake and may be turning slowly on purpose, so only a small
 *   correction is taken, a quarter at a time. */
#define GYRO_QUICK_WINDOW       32U     /* ~64 ms at 500 samples a second */
#define GYRO_QUICK_SPREAD       (6 * GYRO_LSB_PER_DPS * 16)  /* 6 dps */
#define GYRO_WINDOW             128U    /* ~0.25 s */
#define GYRO_REST_SPREAD        (10 * 16)                    /* 1/16 LSB */
#define GYRO_STILL_SPREAD       (3 * GYRO_LSB_PER_DPS * 16)  /* 3 dps */
#define GYRO_DRIFT_MAX          (1 * GYRO_LSB_PER_DPS * 16)  /* 1 dps */

enum gyro_quality {
    GYRO_NONE,
    GYRO_COARSE,
    GYRO_FINE,
};

/* Below this rate motion is scaled down, quadratically to zero: what is left
 * of the offset and of the hands' shake does not creep the pointer. */
#define GYRO_TIGHTEN            (GYRO_LSB_PER_DPS * 16 / 2)  /* 0.5 dps */

/* Acceleration: turning slower than GYRO_ACCEL_SLOW moves the pointer at the
 * set speed, faster than GYRO_ACCEL_FAST at `accel` percent of it, and in
 * between in proportion.  Fine aim stays fine; a flick crosses the screen. */
#define GYRO_ACCEL_SLOW         (20 * GYRO_LSB_PER_DPS * 16)
#define GYRO_ACCEL_FAST         (150 * GYRO_LSB_PER_DPS * 16)

/* A sample more than this after the last one (the sensor was restarted, or
 * reads failed) is integrated as if it were this late. */
#define GYRO_MAX_DT_US          20000U

/* Rates in 1/16 LSB, so the offset can move by less than one LSB. */
static s32 bias[3];
static u8 quality;
static u32 rests;

static s32 window_sum[3];
static s32 window_min[3];
static s32 window_max[3];
static u16 window_count;

static u32 last_sequence;
static u32 last_us;
static u8 have_last;
/* Rate times microseconds times the acceleration percentage. */
static s64 motion[2];

void cyclone2_gyro_reset(void)
{
    have_last = 0U;
    window_count = 0U;
    motion[0] = motion[1] = 0;
}

static s32 absolute(s32 value)
{
    return value < 0 ? -value : value;
}

static void learn_offset(const s32 rate[3])
{
    u16 size = quality == GYRO_NONE ? GYRO_QUICK_WINDOW : GYRO_WINDOW;
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
    if (++window_count < size) return;
    window_count = 0U;

    for (axis = 0; axis < 3; axis++) {
        mean[axis] = window_sum[axis] / (s32)size;
        if (window_max[axis] - window_min[axis] > spread)
            spread = window_max[axis] - window_min[axis];
    }
    if (quality == GYRO_NONE) {
        if (spread > GYRO_QUICK_SPREAD) return;
        quality = GYRO_COARSE;
    } else if (spread > GYRO_STILL_SPREAD) {
        return;
    } else if (quality == GYRO_COARSE || spread <= GYRO_REST_SPREAD) {
        quality = GYRO_FINE;
    } else {
        for (axis = 0; axis < 3; axis++)
            if (absolute(mean[axis] - bias[axis]) > GYRO_DRIFT_MAX) return;
        for (axis = 0; axis < 3; axis++)
            bias[axis] += (mean[axis] - bias[axis]) / 4;
        rests++;
        return;
    }
    for (axis = 0; axis < 3; axis++) bias[axis] = mean[axis];
    rests++;
}

static s32 tighten(s32 rate)
{
    if (absolute(rate) >= GYRO_TIGHTEN) return rate;
    return rate * absolute(rate) / GYRO_TIGHTEN;
}

static s32 accel_percent(s32 yaw, s32 pitch, u16 accel)
{
    s32 a = absolute(yaw);
    s32 b = absolute(pitch);
    /* Within 12% of the length of (yaw, pitch), without a square root. */
    s32 speed = a > b ? a + b / 2 : b + a / 2;

    if (accel <= 100U || speed <= GYRO_ACCEL_SLOW) return 100;
    if (speed >= GYRO_ACCEL_FAST) return accel;
    return 100 + (s32)(accel - 100U) * (speed - GYRO_ACCEL_SLOW) /
                 (GYRO_ACCEL_FAST - GYRO_ACCEL_SLOW);
}

/* Each sample's rate is taken to hold from the one before it until itself. */
static void take_samples(u16 accel)
{
    struct cyclone2_imu_sample samples[8];
    int count;
    int index;

    if (!have_last) {
        if (cyclone2_imu_latest(&samples[0]) != 0) return;
        last_sequence = samples[0].sequence;
        last_us = samples[0].timestamp_us;
        have_last = 1U;
        return;
    }
    count = cyclone2_imu_since(last_sequence, samples, 8);
    for (index = 0; index < count; index++) {
        const struct cyclone2_imu_sample *sample = &samples[index];
        u32 dt = sample->timestamp_us - last_us;
        s32 rate[3];
        s32 yaw;
        s32 pitch;
        s32 percent;

        if (dt > GYRO_MAX_DT_US) dt = GYRO_MAX_DT_US;
        last_sequence = sample->sequence;
        last_us = sample->timestamp_us;
        rate[0] = (s32)sample->gyro_x * 16;
        rate[1] = (s32)sample->gyro_y * 16;
        rate[2] = (s32)sample->gyro_z * 16;
        learn_offset(rate);
        if (quality == GYRO_NONE) continue;
        yaw = GYRO_YAW_SIGN *
              tighten(rate[GYRO_YAW_AXIS] - bias[GYRO_YAW_AXIS]);
        pitch = GYRO_PITCH_SIGN *
                tighten(rate[GYRO_PITCH_AXIS] - bias[GYRO_PITCH_AXIS]);
        percent = accel_percent(yaw, pitch, accel);
        motion[0] += (s64)yaw * (s32)dt * percent;
        motion[1] += (s64)pitch * (s32)dt * percent;
    }
}

void cyclone2_gyro_take(u16 per_degree, u16 accel, s32 *x, s32 *y)
{
    /* motion is 1/16 LSB * us * percent; a degree is
     * GYRO_LSB_PER_DPS * 16 * 1000000 * 100 of it. */
    const s64 per_count = (s64)GYRO_LSB_PER_DPS * 16 * 1000000 * 100;
    s32 *out[2];
    int axis;

    take_samples(accel);
    out[0] = x;
    out[1] = y;
    for (axis = 0; axis < 2; axis++) {
        s64 total = motion[axis] * per_degree;
        s32 counts = (s32)(total / per_count);

        /* What is left under a count waits for the next take. */
        motion[axis] -= (s64)counts * per_count / per_degree;
        *out[axis] = counts;
    }
}

void cyclone2_gyro_get_status(struct cyclone2_gyro_status *status)
{
    int axis;

    status->offset = quality;
    for (axis = 0; axis < 3; axis++) status->bias[axis] = (s16)(bias[axis] / 16);
    status->rests = rests;
}
