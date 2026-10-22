#ifndef I2C_H
#define I2C_H

/* board_pad.c's I2C (PB4 SCL, PB6 SDA, 400 kHz), for imu_qmi8658a.c: one
 * register transfer at a time, run from the I2C interrupt. */

void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);   /* starts; EV_IMU when done */
void plat_i2c_write(u8 dev, u8 reg, u8 value);
int plat_i2c_done(void);                    /* 1 done, 0 busy, -1 failed */

#endif
