#ifndef LINK_H
#define LINK_H

#include "pad.h"

/* The pad to dongle link (transport dongle).  Classic HID: the pad is the HID
 * device (profile_link.c), the dongle its host and master (dongle_bt.c).  The
 * pad sends its state, the dongle turns it into the slot's type on USB with
 * the same profile code the pad uses on its own USB port.  Little-endian,
 * byte by byte (no struct layout).
 *
 * Input (0x20, interrupt, newest wins), LINK_INPUT_SIZE bytes:
 *   0 sequence, 1 flags (bits 0-1 motion samples, 2 low battery, 3 on USB
 *   power), 2 hat, 3 -, 4 buttons (PAD_*), 6 lx ly rx ry (s16), 14 l2 r2,
 *   18 the newest sample's time (plat_us), 22 µs between the two samples,
 *   24 samples oldest first: accel[3] gyro[3] (s16)
 * Config (0x22, a message, once a session and when it changes), LINK_CONFIG_SIZE:
 *   0 version, 1 type (PROFILE_*), 2 mouse layout, 3 -, 4 stick_speed,
 *   6 gyro_speed, 8 gyro_accel, 10 the slot's address
 * Output (0x21, dongle to pad, on change and every LINK_OUTPUT_MS), LINK_OUTPUT_SIZE:
 *   0 rumble left, right, 2 flags (0 players, 1 lightbar, 2 the USB host
 *   took the personality), 3 players, 4 players flashing, 5 lightbar rgb,
 *   8 motion wanted (IMU_*) */

#define LINK_INPUT_ID     0x20
#define LINK_OUTPUT_ID    0x21
#define LINK_CONFIG_ID    0x22
#define LINK_VERSION      1
#define LINK_INPUT_SIZE   48
#define LINK_CONFIG_SIZE  16
#define LINK_OUTPUT_SIZE  10
#define LINK_IMU_MAX      2
#define LINK_OUTPUT_MS    100
#define LINK_NAME         "Cyclone 2 Link"   /* what the dongle pairs with */

enum { LINK_LOW_BATTERY = 4, LINK_USB_POWER = 8 };
enum { LINK_HAS_PLAYERS = 1, LINK_HAS_LIGHTBAR = 2, LINK_USB_READY = 4 };

struct link_config {
    u8 type, variant;
    u16 stick_speed, gyro_speed, gyro_accel;
    u8 address[6];
};

void link_pack_input(const struct pad *p, u8 seq, u8 flags, u8 out[LINK_INPUT_SIZE]);
int link_unpack_input(const u8 *in, u16 len, struct pad *p, u8 *seq, u8 *flags);
void link_pack_config(const struct link_config *c, u8 out[LINK_CONFIG_SIZE]);
int link_unpack_config(const u8 *in, u16 len, struct link_config *c);
void link_pack_output(const struct profile_host *h, u8 imu, u8 usb_ready, u8 out[LINK_OUTPUT_SIZE]);
int link_unpack_output(const u8 *in, u16 len, struct profile_host *h, u8 *usb_ready);

#endif
