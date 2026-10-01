#include <string.h>
#include "pad.h"

/* The QMI8658A motion sensor on I2C, without ever waiting: a state machine
 * the loop advances, with each transfer running on the I2C interrupt. */

#define DEV         0xd4
#define REG_RESET   0x60
#define REG_CTRL7   0x08
#define REG_ACCEL   0x35
#define REG_GYRO    0x3b
#define RESET_MS    20
#define RETRY_MS    1000
#define PERIOD_US   4460         /* 224.2 Hz, the rate set below */
#define HISTORY     8

/* Recovered from the stock firmware (docs/peripherals.md), with both rates
 * at 224.2 Hz instead of 448.4: CTRL2 0x15, CTRL3 0x75. */
static const u8 setup[][2] = {
    { 0x02, 0x60 }, { 0x03, 0x15 }, { 0x04, 0x75 }, { 0x06, 0x55 },
    { 0x0b, 0x01 }, { 0x0a, 0x12 }, { 0x0a, 0x00 }, { 0x08, 0x83 },
};

enum { S_OFF, S_RESET, S_RESET_WAIT, S_SETUP, S_RUN, S_READ, S_SLEEP, S_FAILED };

static struct {
    u8 state, mode, step, busy;
    u32 at_ms, next_us;
    u8 data[12];
    struct imu_sample ring[HISTORY];
    u32 seq;                     /* samples taken so far */
    s32 bias[3];                 /* gyro zero-rate offset, 1/16 LSB */
    u8 quality;
    s32 win_sum[3], win_min[3], win_max[3];
    u16 win_count;
} imu;

static void learn(const s16 g[3]);

static void enter(u8 state, u32 now)
{
    imu.state = state;
    imu.at_ms = now;
}

static void store(void)
{
    struct imu_sample *s = &imu.ring[imu.seq % HISTORY];
    const u8 *d = imu.mode == IMU_GYRO ? imu.data - 6 : imu.data;
    u32 i;

    for (i = 0; i < 3; i++) {
        s->accel[i] = imu.mode == IMU_GYRO ? 0 : (s16)(d[2 * i] | d[2 * i + 1] << 8);
        s->gyro[i] = (s16)(d[6 + 2 * i] | d[7 + 2 * i] << 8);
    }
    s->us = plat_us();
    learn(s->gyro);
    imu.seq++;
}

u32 imu_poll(u32 now, u8 mode)
{
    int done = plat_i2c_done();

    if (imu.busy && done == 0) return 0;
    if (imu.busy) {
        imu.busy = 0;
        if (done < 0) {
            util_log("imu: bus error in state %u", imu.state);
            enter(S_FAILED, now);
        }
    }
    if (mode != imu.mode && imu.state != S_FAILED) {
        imu.mode = mode;
        enter(mode == IMU_OFF ? S_SLEEP : S_RESET, now);
    }
    switch (imu.state) {
    case S_FAILED:
        if (now - imu.at_ms < RETRY_MS) return imu.at_ms + RETRY_MS;
        enter(imu.mode == IMU_OFF ? S_SLEEP : S_RESET, now);
        return now;
    case S_SLEEP:
        plat_i2c_write(DEV, REG_CTRL7, 0);
        imu.busy = 1;
        enter(S_OFF, now);
        return 0;
    case S_RESET:
        plat_i2c_write(DEV, REG_RESET, 0xb0);
        imu.busy = 1;
        enter(S_RESET_WAIT, now);
        return 0;
    case S_RESET_WAIT:
        if (now - imu.at_ms < RESET_MS) return imu.at_ms + RESET_MS;
        imu.step = 0;
        enter(S_SETUP, now);
        /* fall through */
    case S_SETUP:
        if (imu.step < sizeof(setup) / sizeof(setup[0])) {
            plat_i2c_write(DEV, setup[imu.step][0], setup[imu.step][1]);
            imu.step++;
            imu.busy = 1;
            return 0;
        }
        imu.next_us = plat_us();
        enter(S_RUN, now);
        /* fall through */
    case S_RUN:
        if ((s32)(plat_us() - imu.next_us) < 0) return now + (imu.next_us - plat_us()) / 1000 + 1;
        imu.next_us += PERIOD_US;
        if ((s32)(plat_us() - imu.next_us) > 0) imu.next_us = plat_us() + PERIOD_US;
        if (imu.mode == IMU_GYRO) plat_i2c_read(DEV, REG_GYRO, imu.data + 6, 6);
        else plat_i2c_read(DEV, REG_ACCEL, imu.data, 12);
        imu.busy = 1;
        imu.state = S_READ;
        return 0;
    case S_READ:
        store();
        imu.state = S_RUN;
        return now + 1;
    default:
        return 0;
    }
}

/* Samples after *seq, oldest first, at most max; *seq moves past them. */
int imu_take(u32 *seq, struct imu_sample *out, int max)
{
    u32 first = imu.seq > HISTORY ? imu.seq - HISTORY : 0;
    int n = 0;

    if (*seq < first) *seq = first;
    if (imu.seq - *seq > (u32)max) *seq = imu.seq - (u32)max;
    while (*seq < imu.seq) out[n++] = imu.ring[(*seq)++ % HISTORY];
    return n;
}

/* ---- gyro offset: learned whenever the pad is held still ---------------------
 * Windows of samples whose readings stay within a spread are the pad held
 * still; their mean is the offset.  A quick rough one first, so pointing
 * starts as the pad is picked up; then a still window; lying on something,
 * small corrections only. */

#define LSB_DPS        16        /* +-2048 dps full scale */
#define QUICK_WINDOW   32
#define QUICK_SPREAD   (6 * LSB_DPS * 16)
#define WINDOW         128
#define REST_SPREAD    (10 * 16)
#define STILL_SPREAD   (3 * LSB_DPS * 16)
#define DRIFT_MAX      (1 * LSB_DPS * 16)

enum { Q_NONE, Q_COARSE, Q_FINE };

static s32 absval(s32 v)
{
    return v < 0 ? -v : v;
}

static void learn(const s16 g[3])
{
    u16 size = imu.quality == Q_NONE ? QUICK_WINDOW : WINDOW;
    s32 mean[3], spread = 0;
    u32 a;

    for (a = 0; a < 3; a++) {
        s32 r = g[a] * 16;
        if (!imu.win_count || r < imu.win_min[a]) imu.win_min[a] = r;
        if (!imu.win_count || r > imu.win_max[a]) imu.win_max[a] = r;
        imu.win_sum[a] = (imu.win_count ? imu.win_sum[a] : 0) + r;
    }
    if (++imu.win_count < size) return;
    imu.win_count = 0;
    for (a = 0; a < 3; a++) {
        mean[a] = imu.win_sum[a] / (s32)size;
        if (imu.win_max[a] - imu.win_min[a] > spread) spread = imu.win_max[a] - imu.win_min[a];
    }
    if (imu.quality == Q_NONE) {
        if (spread > QUICK_SPREAD) return;
        imu.quality = Q_COARSE;
    } else if (spread > STILL_SPREAD) {
        return;
    } else if (imu.quality == Q_COARSE || spread <= REST_SPREAD) {
        imu.quality = Q_FINE;
    } else {
        for (a = 0; a < 3; a++)
            if (absval(mean[a] - imu.bias[a]) > DRIFT_MAX) return;
        for (a = 0; a < 3; a++) imu.bias[a] += (mean[a] - imu.bias[a]) / 4;
        return;
    }
    for (a = 0; a < 3; a++) imu.bias[a] = mean[a];
}

/* ---- the gyro as a pointer (mouse, PlayStation touchpad) ---------------------
 * Yaw is the sensor's z, pitch its y, as they felt right in use.  Slow
 * turns are scaled down to nothing (what is left of the offset and the hands'
 * shake); fast ones are sped up to `accel` percent. */

#define YAW            2
#define PITCH          1
#define TIGHTEN        (LSB_DPS * 16 / 2)
#define ACCEL_SLOW     (20 * LSB_DPS * 16)
#define ACCEL_FAST     (150 * LSB_DPS * 16)
#define MAX_DT_US      20000

static s32 tighten(s32 r)
{
    return absval(r) >= TIGHTEN ? r : r * absval(r) / TIGHTEN;
}

static s32 accel_percent(s32 yaw, s32 pitch, u16 accel)
{
    s32 a = absval(yaw), b = absval(pitch);
    s32 speed = a > b ? a + b / 2 : b + a / 2;

    if (accel <= 100 || speed <= ACCEL_SLOW) return 100;
    if (speed >= ACCEL_FAST) return accel;
    return 100 + (s32)(accel - 100) * (speed - ACCEL_SLOW) / (ACCEL_FAST - ACCEL_SLOW);
}

void imu_pointer(struct imu_pointer *p, const struct imu_sample *s, int n, u16 per_degree,
                 u16 accel, s32 *x, s32 *y)
{
    const s64 per_count = (s64)LSB_DPS * 16 * 1000000 * 100;
    s32 *out[2] = { x, y };
    int i, a;

    for (i = 0; i < n; i++) {
        u32 dt = p->have ? s[i].us - p->last_us : 0;
        s32 yaw, pitch, pct;

        p->have = 1;
        p->last_us = s[i].us;
        if (imu.quality == Q_NONE) continue;
        if (dt > MAX_DT_US) dt = MAX_DT_US;
        yaw = -tighten(s[i].gyro[YAW] * 16 - imu.bias[YAW]);
        pitch = -tighten(s[i].gyro[PITCH] * 16 - imu.bias[PITCH]);
        pct = accel_percent(yaw, pitch, accel);
        p->motion[0] += (s64)yaw * (s32)dt * pct;
        p->motion[1] += (s64)pitch * (s32)dt * pct;
    }
    for (a = 0; a < 2; a++) {
        s32 counts = (s32)(p->motion[a] * per_degree / per_count);

        p->motion[a] -= (s64)counts * per_count / (per_degree ? per_degree : 1);
        *out[a] = counts;
    }
}

#ifdef UNITTEST
static u32 fake_us;
static int pending;
static u8 last_reg, last_value;
static u8 *read_buf;
u32 plat_us(void) { return fake_us; }
void util_log(const char *fmt, ...) { (void)fmt; }
void plat_i2c_write(u8 dev, u8 reg, u8 value) { (void)dev; last_reg = reg; last_value = value; pending = 1; }
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    (void)dev;
    (void)reg;
    read_buf = buf;
    memset(buf, 0, len);
    buf[len - 2] = 0x10;         /* gyro z = 16: 1 dps */
    pending = 1;
}
int plat_i2c_done(void) { return pending ? (pending = 0, 1) : 1; }

int main(void)
{
    struct imu_sample out[8];
    struct imu_pointer p;
    u32 now, seq = 0, writes = 0;
    s32 x, y, sx = 0;
    int n, i;

    CHECK(imu_poll(0, IMU_OFF) == 0 && imu.state == S_OFF);
    imu_poll(1, IMU_MOTION);
    CHECK(last_reg == REG_RESET && imu.state == S_RESET_WAIT);
    CHECK(imu_poll(5, IMU_MOTION) == 21);
    for (now = 21; now < 40; now++) {
        imu_poll(now, IMU_MOTION);
        if (imu.state == S_SETUP) writes++;
    }
    CHECK(imu.state == S_RUN || imu.state == S_READ);
    CHECK(last_reg == 0x08 && last_value == 0x83);
    for (fake_us = 40000; fake_us < 140000; fake_us += 500) imu_poll(fake_us / 1000, IMU_MOTION);
    CHECK(imu.seq >= 20 && imu.seq <= 24);
    n = imu_take(&seq, out, 8);
    CHECK(n == 8 && seq == imu.seq && out[7].gyro[2] == 16);
    CHECK(imu_take(&seq, out, 8) == 0);
    CHECK(imu.quality == Q_NONE || imu.quality == Q_COARSE);
    for (fake_us = 140000; fake_us < 1140000; fake_us += 500) imu_poll(fake_us / 1000, IMU_MOTION);
    CHECK(imu.quality == Q_FINE && imu.bias[2] == 16 * 16);
    memset(&p, 0, sizeof(p));
    for (i = 0; i < 100; i++) {
        struct imu_sample s;
        memset(&s, 0, sizeof(s));
        s.gyro[2] = 16 + 160;     /* 10 dps right of the learned offset */
        s.us = i * 10000;
        imu_pointer(&p, &s, 1, 32, 100, &x, &y);
        sx += x;
    }
    CHECK(sx <= -310 && sx >= -320);  /* 10 dps for 0.99 s at 32 per degree */
    imu_poll(1200, IMU_OFF);
    CHECK(last_reg == REG_CTRL7 && last_value == 0);
    return TEST_DONE("input_imu");
}
#endif
