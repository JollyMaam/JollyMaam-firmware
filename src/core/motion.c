#include <string.h>
#include "pad.h"

/* Motion samples, whatever sensor gives them: the newest few for the
 * reports, the gyro's zero-rate offset learned from them, and the gyro as a
 * pointer.  The board's sensor driver (imu_poll) hands each sample to
 * imu_store(), in the units of pad.h: gyro 16 LSB per degree per second. */

#define HISTORY     8

enum { Q_NONE, Q_COARSE, Q_FINE };        /* how well the gyro offset is known */

static struct {
    struct imu_sample ring[HISTORY];
    u32 seq;                     /* samples taken so far */
    s32 bias[3];                 /* gyro zero-rate offset, 1/16 LSB */
    u8 quality;
    s32 win_sum[3], win_min[3], win_max[3];
    u16 win_count;
} imu;

static void learn(const s16 g[3]);

void imu_store(const s16 accel[3], const s16 gyro[3])
{
    struct imu_sample *s = &imu.ring[imu.seq % HISTORY];
    u32 i;

    for (i = 0; i < 3; i++) {
        s->accel[i] = accel[i];
        s->gyro[i] = gyro[i];
    }
    s->us = plat_us();
    learn(s->gyro);
    imu.seq++;
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

int imu_gyro_bias(s32 out[3])
{
    memcpy(out, imu.bias, sizeof(imu.bias));
    return imu.quality != Q_NONE;
}

/* ---- gyro offset: learned whenever the pad is held still ---------------------
 * Windows of samples whose readings stay within a spread are the pad held
 * still; their mean is the offset.  A quick rough one first, so pointing
 * starts as the pad is picked up; then a still window; lying on something,
 * small corrections only. */

#define LSB_DPS        16        /* per degree per second (pad.h) */
#define QUICK_WINDOW   32
#define QUICK_SPREAD   (6 * LSB_DPS * 16)
#define WINDOW         128
#define REST_SPREAD    (10 * 16)
#define STILL_SPREAD   (3 * LSB_DPS * 16)
#define DRIFT_MAX      (1 * LSB_DPS * 16)

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

void imu_pointer(struct imu_pointer *p, const struct pad *pad, u16 per_degree, u16 accel,
                 s32 *x, s32 *y)
{
    const s64 per_count = (s64)LSB_DPS * 16 * 1000000 * 100;
    const struct imu_sample *s = pad->imu;
    s32 *out[2] = { x, y };
    int i, a;

    for (i = 0; i < pad->imu_count; i++) {
        u32 dt = p->have ? s[i].us - p->last_us : 0;
        s32 yaw, pitch, pct;

        p->have = 1;
        p->last_us = s[i].us;
        if (!pad->gyro_bias_known) continue;
        if (dt > MAX_DT_US) dt = MAX_DT_US;
        yaw = -tighten(s[i].gyro[YAW] * 16 - pad->gyro_bias[YAW]);
        pitch = -tighten(s[i].gyro[PITCH] * 16 - pad->gyro_bias[PITCH]);
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
u32 plat_us(void) { return fake_us; }

int main(void)
{
    static const s16 still[3] = { 0, 0, 16 };     /* gyro z = 16: 1 dps */
    struct imu_sample out[8];
    struct imu_pointer p;
    struct pad pad;
    u32 seq = 0;
    s32 x, y, sx = 0;
    int n, i;

    for (i = 0; i < 20; i++, fake_us += 4460) imu_store(still, still);
    n = imu_take(&seq, out, 8);
    CHECK(n == 8 && seq == imu.seq && out[7].gyro[2] == 16 && out[7].us == fake_us - 4460);
    CHECK(imu_take(&seq, out, 8) == 0);
    CHECK(imu.quality == Q_NONE);     /* a quick window is 32 samples */
    for (i = 0; i < 12; i++) imu_store(still, still);
    CHECK(imu.quality == Q_COARSE);
    for (i = 0; i < 128; i++) imu_store(still, still);
    CHECK(imu.quality == Q_FINE && imu.bias[2] == 16 * 16);
    memset(&p, 0, sizeof(p));
    memset(&pad, 0, sizeof(pad));
    pad.gyro_bias_known = (u8)imu_gyro_bias(pad.gyro_bias);
    CHECK(pad.gyro_bias_known && pad.gyro_bias[2] == 16 * 16);
    pad.imu_count = 1;
    for (i = 0; i < 100; i++) {
        pad.imu[0].gyro[2] = 16 + 160;     /* 10 dps right of the learned offset */
        pad.imu[0].us = i * 10000;
        imu_pointer(&p, &pad, 32, 100, &x, &y);
        sx += x;
    }
    CHECK(sx <= -310 && sx >= -320);  /* 10 dps for 0.99 s at 32 per degree */
    pad.gyro_bias_known = 0;          /* nothing learned: no pointer, no catching up later */
    imu_pointer(&p, &pad, 32, 100, &x, &y);
    CHECK(!x && !y);
    return TEST_DONE("motion");
}
#endif
