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
    u32 timestamp_ms;
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
};

void cyclone2_imu_task(void *private_data);
void cyclone2_imu_enable(void);
void cyclone2_imu_disable(void);
int cyclone2_imu_latest(struct cyclone2_imu_sample *sample);
u8 cyclone2_imu_recent(struct cyclone2_imu_sample *samples, u8 maximum);
void cyclone2_imu_get_status(struct cyclone2_imu_status *status);

#endif
