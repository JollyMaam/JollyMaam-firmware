#ifndef I2C_H
#define I2C_H

/* The board's I2C, for imu_i2c.c: one register transfer at a time,
 * running while the loop goes on (the Cyclone's on its I2C interrupt, the
 * U2CB's on its I2C engine).  The board raises EV_IMU when one is done. */

void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);   /* starts; EV_IMU when done */
void plat_i2c_write(u8 dev, u8 reg, u8 value);
int plat_i2c_done(void);                    /* 1 done, 0 busy, -1 failed */

/* imu_i2c.c's: the board's sensor (the QMI8658A unless the board sets
 * another), its address (8-bit, 0xd4 unless set), and its axes (a board
 * whose sensor sits otherwise than the Cyclone's defines them). */
struct imu_chip {
    u8 reset[2];                 /* register, value: a soft reset */
    u8 reset_ms;                 /* then this long before the set-up */
    const u8 (*setup)[2];        /* register, value: in turn */
    u8 setups;
    u8 sleep[2];                 /* register, value: both sensors off */
    u8 data;                     /* accelerometer x, y, z then gyro x, y, z */
    u8 big_endian;
    u16 gyro_scale;              /* to 16 LSB per dps: * gyro_scale / 1024 */
};
extern const struct imu_chip imu_qmi8658a, imu_icm42670;
extern const struct imu_chip *imu_chip;
extern u8 imu_dev;
extern const s8 imu_axis[3];

#endif
