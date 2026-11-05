#include <string.h>
#include "pad.h"
#include "i2c.h"

/* A motion sensor on I2C (the Cyclone 2's QMI8658A, the U2CB's ICM-42670),
 * without ever waiting: a state machine the loop advances, with each
 * transfer running in the board's I2C (i2c.h).  Samples go to motion.c
 * (imu_store) in its units, 16 LSB per degree per second and 8192 per g,
 * and in the Cyclone's axes: a board whose sensor sits otherwise defines
 * imu_axis.  What differs between the chips is in struct imu_chip. */

#define RETRY_MS    1000
#define PERIOD_US   4460         /* 224.2 Hz, about the chips' rates set below */

/* QMI8658A: recovered from the Cyclone's stock firmware
 * (docs/gamesir-cyclone2/peripherals.md), with both rates at 224.2 Hz
 * instead of 448.4: CTRL2 0x15 (+-4 g), CTRL3 0x75 (+-2048 dps). */
static const u8 qmi8658a_setup[][2] = {
    { 0x02, 0x60 }, { 0x03, 0x15 }, { 0x04, 0x75 }, { 0x06, 0x55 },
    { 0x0b, 0x01 }, { 0x0a, 0x12 }, { 0x0a, 0x00 }, { 0x08, 0x83 },
};
const struct imu_chip imu_qmi8658a = {
    .reset = { 0x60, 0xb0 }, .reset_ms = 20,
    .setup = qmi8658a_setup, .setups = sizeof(qmi8658a_setup) / sizeof(qmi8658a_setup[0]),
    .sleep = { 0x08, 0x00 }, .data = 0x35, .gyro_scale = 1024,
};

/* ICM-42670 (TDK; the U2CB's answers 0x63 at WHO_AM_I 0x75, stock treats it
 * as its 0x67): soft reset in SIGNAL_PATH_RESET; GYRO_CONFIG0 +-2000 dps
 * 400 Hz, ACCEL_CONFIG0 +-4 g 400 Hz (stock: +-8 g, 800 Hz, filters as
 * they come), GYRO_CONFIG1 and ACCEL_CONFIG1 filters at 53 Hz, then
 * PWR_MGMT0 both in low noise mode, last (no write for 200 us after it).
 * Data from 0x0b: accelerometer then gyro, big-endian; the gyro 16.4 LSB per
 * dps, scaled to 16. */
static const u8 icm42670_setup[][2] = {
    { 0x20, 0x07 }, { 0x21, 0x47 }, { 0x23, 0x34 }, { 0x24, 0x44 }, { 0x1f, 0x0f },
};
const struct imu_chip imu_icm42670 = {
    .reset = { 0x02, 0x10 }, .reset_ms = 2,
    .setup = icm42670_setup, .setups = sizeof(icm42670_setup) / sizeof(icm42670_setup[0]),
    .sleep = { 0x1f, 0x00 }, .data = 0x0b, .big_endian = 1, .gyro_scale = 999,   /* 1024 * 16 / 16.4 */
};

/* The board's sensor and its address (8-bit): the Cyclone's unless the
 * board sets others before the first imu_poll. */
const struct imu_chip *imu_chip = &imu_qmi8658a;
u8 imu_dev = 0xd4;
#define DEV imu_dev

/* For each of our axes x, y, z, the sensor's axis it is (1 x, 2 y, 3 z),
 * negative for the opposite direction; the same for the accelerometer and
 * the gyro.  As the Cyclone's sensor sits. */
__attribute__((weak)) const s8 imu_axis[3] = { 1, 2, 3 };

enum { S_OFF, S_RESET, S_RESET_WAIT, S_SETUP, S_RUN, S_READ, S_SLEEP, S_FAILED };

static struct {
    u8 state, mode, step, busy;
    u8 failing;                  /* the last transfer failed: retries are not logged */
    u32 at_ms, next_us;
    u8 data[12];
} imu;

static void enter(u8 state, u32 now)
{
    imu.state = state;
    imu.at_ms = now;
}

static s16 value(const u8 *b)
{
    return imu_chip->big_endian ? (s16)(b[0] << 8 | b[1]) : (s16)(b[0] | b[1] << 8);
}

static void store(void)
{
    const u8 *d = imu.data;
    s16 accel[3], gyro[3];
    u32 i;

    for (i = 0; i < 3; i++) {
        s8 k = imu_axis[i];
        u32 a = (u32)(k < 0 ? -k : k) - 1;
        s16 ac = imu.mode == IMU_GYRO ? 0 : value(d + 2 * a);
        s16 gy = (s16)(value(d + 6 + 2 * a) * imu_chip->gyro_scale / 1024);

        /* -(-32768) stays -32768 as s16: full scale, never reached at rest */
        accel[i] = k < 0 ? (s16)-ac : ac;
        gyro[i] = k < 0 ? (s16)-gy : gy;
    }
    imu_store(accel, gyro);
}

u32 imu_poll(u32 now, u8 mode)
{
    int done = plat_i2c_done();

    if (imu.busy && done == 0) return 0;
    if (imu.busy) {
        imu.busy = 0;
        if (done < 0) {
            if (!imu.failing) util_log("imu: bus error in state %u (retried every %u ms, unlogged)",
                                       imu.state, (u32)RETRY_MS);
            imu.failing = 1;
            enter(S_FAILED, now);
        } else {
            imu.failing = 0;
            if (imu.state == S_READ) {
                store();         /* and on to the next read's time below */
                imu.state = S_RUN;
            }
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
        plat_i2c_write(DEV, imu_chip->sleep[0], imu_chip->sleep[1]);
        imu.busy = 1;
        enter(S_OFF, now);
        return 0;
    case S_RESET:
        plat_i2c_write(DEV, imu_chip->reset[0], imu_chip->reset[1]);
        imu.busy = 1;
        enter(S_RESET_WAIT, now);
        return 0;
    case S_RESET_WAIT:
        if (now - imu.at_ms < imu_chip->reset_ms) return imu.at_ms + imu_chip->reset_ms;
        imu.step = 0;
        enter(S_SETUP, now);
        /* fall through */
    case S_SETUP:
        if (imu.step < imu_chip->setups) {
            plat_i2c_write(DEV, imu_chip->setup[imu.step][0], imu_chip->setup[imu.step][1]);
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
        if (imu.mode == IMU_GYRO) plat_i2c_read(DEV, (u8)(imu_chip->data + 6), imu.data + 6, 6);
        else plat_i2c_read(DEV, imu_chip->data, imu.data, 12);
        imu.busy = 1;
        imu.state = S_READ;
        return 0;
    default:
        return 0;
    }
}

#ifdef UNITTEST
static u32 fake_us;
static int pending;
static u8 last_reg, last_value;
static u32 stored;
static s16 last_gyro_z;
u32 plat_us(void) { return fake_us; }
void util_log(const char *fmt, ...) { (void)fmt; }
void imu_store(const s16 accel[3], const s16 gyro[3]) { (void)accel; stored++; last_gyro_z = gyro[2]; }
void plat_i2c_write(u8 dev, u8 reg, u8 value) { (void)dev; last_reg = reg; last_value = value; pending = 1; }
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    (void)dev;
    (void)reg;
    memset(buf, 0, len);
    buf[len - 2] = 0x10;         /* gyro z = 16: 1 dps */
    pending = 1;
}
int plat_i2c_done(void) { return pending ? (pending = 0, 1) : 1; }

int main(void)
{
    u32 now;

    CHECK(imu_poll(0, IMU_OFF) == 0 && imu.state == S_OFF);
    imu_poll(1, IMU_MOTION);
    CHECK(last_reg == 0x60 && imu.state == S_RESET_WAIT);
    CHECK(imu_poll(5, IMU_MOTION) == 21);
    for (now = 21; now < 40; now++) imu_poll(now, IMU_MOTION);
    CHECK(imu.state == S_RUN || imu.state == S_READ);
    CHECK(last_reg == 0x08 && last_value == 0x83);
    for (fake_us = 40000; fake_us < 140000; fake_us += 500) imu_poll(fake_us / 1000, IMU_MOTION);
    CHECK(stored >= 20 && stored <= 24 && last_gyro_z == 16);
    /* gyro only (the gyro mouse): the gyro still where it belongs */
    last_gyro_z = 0;
    for (fake_us = 140000; fake_us < 200000; fake_us += 500) imu_poll(fake_us / 1000, IMU_GYRO);
    CHECK(last_gyro_z == 16);
    imu_poll(1200, IMU_OFF);
    CHECK(last_reg == 0x08 && last_value == 0);
    /* the ICM-42670: big-endian, its gyro scaled from 16.4 to 16 per dps */
    imu_chip = &imu_icm42670;
    last_gyro_z = 0;
    for (fake_us = 1300000; fake_us < 1400000; fake_us += 500) imu_poll(fake_us / 1000, IMU_MOTION);
    CHECK(last_gyro_z == 0x1000 * 999 / 1024);
    imu_poll(1500, IMU_OFF);
    CHECK(last_reg == 0x1f && last_value == 0);
    return TEST_DONE("imu_i2c");
}
#endif
