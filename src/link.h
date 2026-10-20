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
 *   power), 2 buttons (PAD_*, u32), 6 lx ly rx ry (s16), 14 l2 r2,
 *   18 the newest sample's time (plat_us), 22 µs between the two samples,
 *   24 samples oldest first: accel[3] gyro[3] (s16)
 * Config (0x22, a message, first and then every second), LINK_CONFIG_SIZE:
 *   0 version, 1 type (PROFILE_*), 2 mouse layout, 3 flags (bit 0: gyro bias
 *   known), 4 stick_speed, 6 gyro_speed, 8 gyro_accel, 10 the slot's address,
 *   16 the gyro's zero-rate offset (s16 x3, 1/16 LSB)
 * Output (0x21, dongle to pad, on change and every LINK_OUTPUT_MS), LINK_OUTPUT_SIZE:
 *   0 rumble left, right, 2 flags (0 players, 1 lightbar, 2 the USB host
 *   took the personality), 3 players, 4 players flashing, 5 lightbar rgb,
 *   8 motion wanted (IMU_*) */

#define LINK_INPUT_ID     0x20
#define LINK_OUTPUT_ID    0x21
#define LINK_CONFIG_ID    0x22
#define LINK_VERSION      3
#define LINK_INPUT_SIZE   48
#define LINK_CONFIG_SIZE  22
#define LINK_OUTPUT_SIZE  10
#define LINK_IMU_MAX      2
#define LINK_OUTPUT_MS    100
#define LINK_NAME         "Cyclone 2 Link"
/* Pairing: neither side can search (the controller does not inquire), so a
 * pairing pad answers at this address and a pairing dongle pages it.  The
 * dongle files the bond under the slot's own address from the config, which
 * the pad uses from then on; the link key does not depend on the address.
 * C2:32:4C:49:4E:4B, least significant byte first. */
static const u8 link_pair_address[6] = { 0x4b, 0x4e, 0x49, 0x4c, 0x32, 0xc2 };

enum { LINK_LOW_BATTERY = 4, LINK_USB_POWER = 8 };
enum { LINK_HAS_PLAYERS = 1, LINK_HAS_LIGHTBAR = 2, LINK_USB_READY = 4 };

struct link_config {
    u8 type, variant;
    u16 stick_speed, gyro_speed, gyro_accel;
    u8 address[6];
    s16 gyro_bias[3];            /* struct pad's, clamped */
    u8 gyro_bias_known;
};

void link_pack_input(const struct pad *p, u8 seq, u8 flags, u8 out[LINK_INPUT_SIZE]);
int link_unpack_input(const u8 *in, u16 len, struct pad *p, u8 *seq, u8 *flags);
void link_pack_config(const struct link_config *c, u8 out[LINK_CONFIG_SIZE]);
int link_unpack_config(const u8 *in, u16 len, struct link_config *c);
void link_pack_output(const struct profile_host *h, u8 imu, u8 usb_ready, u8 out[LINK_OUTPUT_SIZE]);
int link_unpack_output(const u8 *in, u16 len, struct profile_host *h, u8 *usb_ready);

#endif
