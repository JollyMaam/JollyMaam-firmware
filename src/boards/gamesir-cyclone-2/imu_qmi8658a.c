#include <string.h>
#include "pad.h"
#include "i2c.h"

/* The Cyclone 2's motion sensor, a QMI8658A on I2C, without ever waiting: a
 * state machine the loop advances, with each transfer running on the I2C
 * interrupt.  Samples go to motion.c (imu_store); its units, 16 LSB per
 * degree per second, are this sensor's at +-2048 dps. */

#define DEV         0xd4
#define REG_RESET   0x60
#define REG_CTRL7   0x08
#define REG_ACCEL   0x35
#define REG_GYRO    0x3b
#define RESET_MS    20
#define RETRY_MS    1000
#define PERIOD_US   4460         /* 224.2 Hz, the rate set below */

/* Recovered from the stock firmware (docs/gamesir-cyclone2/peripherals.md), with both rates
 * at 224.2 Hz instead of 448.4: CTRL2 0x15, CTRL3 0x75. */
static const u8 setup[][2] = {
    { 0x02, 0x60 }, { 0x03, 0x15 }, { 0x04, 0x75 }, { 0x06, 0x55 },
    { 0x0b, 0x01 }, { 0x0a, 0x12 }, { 0x0a, 0x00 }, { 0x08, 0x83 },
};

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

static void store(void)
{
    const u8 *d = imu.mode == IMU_GYRO ? imu.data - 6 : imu.data;
    s16 accel[3], gyro[3];
    u32 i;

    for (i = 0; i < 3; i++) {
        accel[i] = imu.mode == IMU_GYRO ? 0 : (s16)(d[2 * i] | d[2 * i + 1] << 8);
        gyro[i] = (s16)(d[6 + 2 * i] | d[7 + 2 * i] << 8);
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
    CHECK(last_reg == REG_RESET && imu.state == S_RESET_WAIT);
    CHECK(imu_poll(5, IMU_MOTION) == 21);
    for (now = 21; now < 40; now++) imu_poll(now, IMU_MOTION);
    CHECK(imu.state == S_RUN || imu.state == S_READ);
    CHECK(last_reg == 0x08 && last_value == 0x83);
    for (fake_us = 40000; fake_us < 140000; fake_us += 500) imu_poll(fake_us / 1000, IMU_MOTION);
    CHECK(stored >= 20 && stored <= 24 && last_gyro_z == 16);
    imu_poll(1200, IMU_OFF);
    CHECK(last_reg == REG_CTRL7 && last_value == 0);
    return TEST_DONE("imu_qmi8658a");
}
#endif
