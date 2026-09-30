#ifndef CYCLONE2_IMU_H
#define CYCLONE2_IMU_H

#include "generic/typedef.h"

struct cyclone2_imu_sample {
    s16 accel_x;
    s16 accel_y;
    s16 accel_z;
    s16 gyro_x;
    s16 gyro_y;
    s16 gyro_z;
    /* When it was read (cyclone2_time_us), and a count of samples read. */
    u32 timestamp_us;
    u32 sequence;
};

struct cyclone2_imu_status {
    u32 samples;
    u32 errors;
    u32 last_sample_ms;
    u8 device_id;
    u8 initialized;
    u8 last_error;
    u8 enabled;
    u32 enable_requests;
    u32 disable_requests;
    /* Accelerometer and gyro switched off (CTRL7 = 0) while not wanted. */
    u8 asleep;
    /* How long the last 12-byte sample read took on the bus. */
    u32 read_us;
};

void cyclone2_imu_task(void *private_data);
/* From the control poll, every millisecond: paces the sampling. */
void cyclone2_imu_tick(void);
/* Accelerometer and gyro, every 4 ms. */
void cyclone2_imu_enable(void);
/* The gyro alone, every 2 ms (the samples' accel reads zero): pointing. */
void cyclone2_imu_enable_gyro(void);
void cyclone2_imu_disable(void);
int cyclone2_imu_latest(struct cyclone2_imu_sample *sample);
/* The samples after `sequence` still in the history, oldest first, at most
 * `count`; returns how many.  The history holds the last 8 (16 ms). */
int cyclone2_imu_since(u32 sequence, struct cyclone2_imu_sample *samples,
                       int count);
void cyclone2_imu_get_status(struct cyclone2_imu_status *status);

#endif
