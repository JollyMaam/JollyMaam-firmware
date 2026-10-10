#ifndef DONGLE_LINK_H
#define DONGLE_LINK_H

#include "pad.h"

#define DONGLE_REPORT_ID       0x20
#define DONGLE_REPORT_VERSION  1
#define DONGLE_CMD_ID          0x21

/* A 16-bit stick coordinate pair. 8 bytes. */
struct dongle_sticks {
    s16 lx, ly;
    s16 rx, ry;
};

/* Uplink report payload from pad to dongle (following 0xa1, report_id).
 * Total size: 58 bytes (with IMU) or 46 bytes (without IMU). Naturally aligned. */
struct dongle_uplink_pkt {
    u8 seq;                /* Monotonic sequence 0..255 */
    u8 flags;              /* bit 0: low batt, bit 1: charging, bit 2: imu present */
    u16 buttons;           /* PAD_* button bitmask */
    u8 hat;                /* HAT_* */
    u8 history_count;      /* number of valid history samples in history[] (0..3) */
    u16 l2, r2;            /* triggers (0..65535) */
    u32 timestamp_us;      /* pad sample timestamp (plat_ms * 1000 + tick_us) */
    struct dongle_sticks current;    /* newest stick state */
    struct dongle_sticks history[3]; /* preceding stick samples (oldest to newest) */
    s16 accel[3];          /* accelerometer */
    s16 gyro[3];           /* gyroscope */
};

/* Downlink command from dongle to pad (following 0xa2, DONGLE_CMD_ID). 8 bytes. */
struct dongle_downlink_pkt {
    u8 seq;                /* Monotonic sequence 0..255 */
    u8 flags;              /* Command flags */
    u8 motor_left;         /* Left motor PWM 0..255 */
    u8 motor_right;        /* Right motor PWM 0..255 */
    u8 led_home_r;         /* Home button LED R */
    u8 led_home_g;         /* Home button LED G */
    u8 led_home_b;         /* Home button LED B */
    u8 player_leds;        /* Player LED pattern */
};

#define DONGLE_UPLINK_BASE_SIZE 46  /* size without IMU */
#define DONGLE_UPLINK_FULL_SIZE 58  /* size with IMU */

#endif
