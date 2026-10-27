#ifndef LINK_H
#define LINK_H

#include "pad.h"

/* The pad to dongle link (transport dongle).  LE: the pad is the peripheral
 * with a GATT server of fixed handles (profile_link.c), the dongle the
 * central (bt_dongle.c), which talks ATT on them without discovery.  The pad
 * sends its state, the dongle turns it into the slot's type on USB with the
 * same profile code the pad uses on its own USB port.  Little-endian, byte by
 * byte (no struct layout).
 *
 * Input (notifications on LINK_INPUT_HANDLE, newest wins), LINK_INPUT_SIZE bytes:
 *   0 sequence, 1 flags (bits 0-1 motion samples, 2 low battery, 3 on USB
 *   power), 2 buttons (PAD_*, u32), 6 lx ly rx ry (s16), 14 l2 r2,
 *   18 the newest sample's time (plat_us), 22 µs between the two samples,
 *   24 samples oldest first: accel[3] gyro[3] (s16)
 * Config (notifications on LINK_CONFIG_HANDLE, first and then every second),
 * LINK_CONFIG_SIZE:
 *   0 version, 1 type (PROFILE_*), 2 mouse layout, 3 flags (bit 0: gyro bias
 *   known), 4 stick_speed, 6 gyro_speed, 8 gyro_accel, 10 the slot's address,
 *   16 the gyro's zero-rate offset (s16 x3, 1/16 LSB)
 * Output (write without response to LINK_OUTPUT_HANDLE, on change and every
 * LINK_OUTPUT_MS), LINK_OUTPUT_SIZE:
 *   0 rumble left, right, 2 flags (0 players, 1 lightbar, 2 the USB host
 *   took the personality), 3 players, 4 players flashing, 5 lightbar rgb,
 *   8 motion wanted (IMU_*)
 *
 * Pairing: a pairing pad advertises LINK_ADV with LINK_ADV_PAIRING set; a
 * pairing dongle connects to the first it hears and writes a new 16-byte
 * key to LINK_KEY_HANDLE (write request).  Both keep it as the bond; from
 * then on the dongle connects to the pad's address and encrypts with it as
 * the LTK (EDIV and Rand 0), as the pad's bt_pad.c answers any LE profile
 * without SMP.  The dongle subscribes to the input after encryption. */

#define LINK_INPUT_HANDLE   0x0003   /* its CCC: + 1 */
#define LINK_CONFIG_HANDLE  0x0006
#define LINK_OUTPUT_HANDLE  0x0009
#define LINK_KEY_HANDLE     0x000b
#define LINK_VERSION      1
#define LINK_INPUT_SIZE   48
#define LINK_CONFIG_SIZE  22
#define LINK_OUTPUT_SIZE  10
#define LINK_KEY_SIZE     16
#define LINK_IMU_MAX      2
#define LINK_OUTPUT_MS    100
#define LINK_NAME         "Cyclone 2 Link"
/* Advertising: flags, then manufacturer data (company 0xffff, reserved for
 * tests): "C2LK", LINK_VERSION, LINK_ADV_* flags. */
#define LINK_ADV_SIZE     13
#define LINK_ADV_FLAGS    12         /* the flags byte's offset */
enum { LINK_ADV_PAIRING = 1 };
static const u8 link_adv[LINK_ADV_FLAGS] = {
    0x02, 0x01, 0x06, 0x09, 0xff, 0xff, 0xff, 'C', '2', 'L', 'K', LINK_VERSION,
};
/* Connection interval the dongle asks for, 1.25 ms units. */
#ifndef LINK_INTERVAL
#define LINK_INTERVAL     3
#endif

/* Hub mode (tools/padhub.c turns it on, until the next boot): for a Linux PC, the dongle's
 * USB is one vendor HID device that carries the link's packets as they are,
 * tagged with the dongle slot they belong to; tools/padhub.c makes each
 * slot a controller of its type with uhid.  Report HUB_REPORT_IN, 63 bytes:
 *   0 slot, 1 HUB_* kind, 2 the payload: HUB_INPUT a LINK_INPUT_SIZE input
 *   (newest wins), HUB_CONFIG a LINK_CONFIG_SIZE config (when it arrives
 *   and every second), HUB_DOWN nothing (the slot's pad is gone),
 *   HUB_STATS (every second, a linked slot) five 32-bit counts, least
 *   significant byte first, since the dongle started: inputs from the pad,
 *   inputs a newer one replaced before the dongle's loop took them, inputs
 *   the pad sent that never came (its sequence), inputs a newer one replaced
 *   before USB took them, inputs sent
 * Report HUB_REPORT_OUT (interrupt OUT), 63 bytes: 0 slot, 1 HUB_OUTPUT,
 *   2 a LINK_OUTPUT_SIZE output; its LINK_USB_READY: the controller is up
 *   on the PC.  Repeat it (LINK_OUTPUT_MS); the dongle passes it on.
 * Feature report USB_TOOL_REPORT "tool" ends hub mode: the tool device
 * until slot 0's link next goes down; "pair" and a slot byte: forget that
 * slot's bond and pair it (padtool pair, as the tool device is not there).
 * In hub mode every slot links; outside it only slot 0, the dongle's own pad. */
#define HUB_VENDOR_ID   0x1209   /* pid.codes' test VID/PID: a private device */
#define HUB_PRODUCT_ID  0x0001
#define HUB_REPORT_IN   1
#define HUB_REPORT_OUT  2
#define HUB_REPORT_SIZE 63
#define HUB_SLOTS       2        /* the controller's central links */
/* Hub mode: the links' interval while the dongle scans or connects (a slot
 * bonded but not linked, or pairing); LINK_INTERVAL again once every bonded
 * slot is linked.  The controller scans and connects between a central
 * link's events only from 6 (7.5 ms); at 5 and below it never does. */
#ifndef HUB_INTERVAL
#define HUB_INTERVAL    6
#endif
enum { HUB_INPUT = 1, HUB_CONFIG = 2, HUB_DOWN = 3, HUB_OUTPUT = 4, HUB_STATS = 5 };

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
