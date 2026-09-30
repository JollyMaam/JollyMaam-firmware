#ifndef CYCLONE2_GYRO_H
#define CYCLONE2_GYRO_H

#include "generic/typedef.h"

/* The gyro as a pointing device: rotation since the last take, with the
 * sensor's zero-rate offset removed.  The offset is learned whenever the pad
 * is held still (see gyro.c); until the first time, no motion comes out.
 * The profile owns the IMU (cyclone2_imu_enable); this only reads it. */

struct cyclone2_gyro_status {
    u8 calibrated;
    s16 bias[3];
    u32 rests;          /* windows taken as the pad at rest */
};

/* Forget accumulated motion; call as a session starts.  The learned offset
 * is kept: it belongs to the sensor, not the session. */
void cyclone2_gyro_reset(void);

/* Rotation since the last take (or reset), in counts at `per_degree` counts
 * per degree: yaw to the right and pitch downwards are positive, as a mouse's
 * x and y.  Call every report; new samples are taken in as they arrive. */
void cyclone2_gyro_take(u16 per_degree, s32 *x, s32 *y);

void cyclone2_gyro_get_status(struct cyclone2_gyro_status *status);

#endif
