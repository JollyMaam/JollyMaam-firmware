#include <string.h>
#include "pad.h"

/* Plain HID: a generic gamepad over Classic and over LE (SDL maps their button
 * order without a database entry), and a keyboard and mouse for desktops and
 * games. */

/* ---- generic gamepad ------------------------------------------------------------ */

#define GENERIC_ID   1
#define GENERIC_SIZE 9

static const u8 generic_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, GENERIC_ID,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0xc0,
};

#define GENERIC_KEEPALIVE_MS 100

static u8 generic_last[GENERIC_SIZE];
static u8 generic_sent, generic_wired;
static u32 generic_sent_ms;

static u8 generic_begin(const struct profile_session *s)
{
    generic_sent = 0;
    generic_wired = s->usb;
    return IMU_OFF;
}

/* On change; on USB also every 100 ms unchanged, because a host only polls
 * while something has the device open and the first report may be missed. */
static int generic_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    out[0] = (u8)p->buttons;
    out[1] = (u8)(p->buttons >> 8);
    out[2] = in_hat(p->buttons);
    out[3] = (u8)((p->lx + 32768) >> 8);
    out[4] = (u8)((p->ly + 32768) >> 8);
    out[5] = (u8)((p->rx + 32768) >> 8);
    out[6] = (u8)((p->ry + 32768) >> 8);
    out[7] = (u8)(p->l2 >> 8);
    out[8] = (u8)(p->r2 >> 8);
    if (generic_sent && !memcmp(out, generic_last, GENERIC_SIZE) &&
        (!generic_wired || now - generic_sent_ms < GENERIC_KEEPALIVE_MS))
        return 0;
    memcpy(generic_last, out, GENERIC_SIZE);
    generic_sent = 1;
    generic_sent_ms = now;
    *id = GENERIC_ID;
    return GENERIC_SIZE;
}

static const struct bt_device generic_device = {
    .transport = BT_CLASSIC, .name = board_name, .class_of_device = 0x002508,
    .vendor_id = USB_OWN_VID, .product_id = USB_OWN_JOYSTICK_PID, .version = 0x0100,
    .hid_descriptor = generic_descriptor, .hid_descriptor_len = sizeof(generic_descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

/* Wired, both generic gamepads are this one: V-USB's shared joystick ID
 * (pad.h), ours by the manufacturer string. */
static const struct usb_identity generic_usb = {
    .vendor_id = USB_OWN_VID, .product_id = USB_OWN_JOYSTICK_PID, .bcd_device = 0x0100,
    .manufacturer = USB_MANUFACTURER, .product = board_name,
    .kind = USB_HID, .report_ids = 1, .interval = 1, .in_size = 16, .max_power = 250,
    .audio = USB_AUDIO_SPEAKER, .hid_descriptor = generic_descriptor, .hid_descriptor_len = sizeof(generic_descriptor),
};

const struct profile profile_generic = {
    .name = "generic", .device = &generic_device, .usb = &generic_usb,
    .begin = generic_begin, .report = generic_report,
};

/* ---- generic gamepad over LE ---------------------------------------------------------
 * HID over GATT, laid out for Linux's input mapping so SDL takes it as a
 * gamepad without a database entry (its evdev auto-mapping): Button usages
 * 1-15 become BTN_SOUTH ... BTN_THUMBR, 21-24 BTN_TRIGGER_HAPPY5-8 (SDL's
 * Elite paddles, mapped only under Microsoft's vendor ID), Consumer Record
 * KEY_RECORD (SDL's misc1), Z and Rz the analog triggers.  Usages 6, 9, 10
 * (BTN_Z, BTN_TL2, BTN_TR2) and 16-20 are left out so nothing else claims a
 * role; 22 and 24 stay 0 (SDL wants all four paddles before it maps any). */

#define GBLE_SIZE    15
#define GBLE_INPUT   0x001a
#define GBLE_PNP     0x0010
#define GBLE_VERSION 0x0100

/* GATT database from this .gatt (BTstack's compile_gatt.py), the report map
 * inline. Handles 0x000d/0x000e are left unused after removing the manufacturer
 * characteristic, preserving the existing report and CCC handles:
 *
 * PRIMARY_SERVICE, GAP_SERVICE
 * CHARACTERISTIC, GAP_DEVICE_NAME, READ | DYNAMIC,                       0x0003
 * CHARACTERISTIC, GAP_APPEARANCE, READ, C4 03
 * CHARACTERISTIC, GAP_PERIPHERAL_PREFERRED_CONNECTION_PARAMETERS, READ, 06 00 09 00 00 00 90 01
 * PRIMARY_SERVICE, GATT_SERVICE
 * CHARACTERISTIC, GATT_SERVICE_CHANGED, READ | INDICATE | DYNAMIC,
 * PRIMARY_SERVICE, ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_PNP_ID, READ | DYNAMIC,                  0x0010
 * PRIMARY_SERVICE, ORG_BLUETOOTH_SERVICE_BATTERY_SERVICE
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL, READ, 64
 * PRIMARY_SERVICE, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_HID_INFORMATION, READ, 11 01 00 02
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_REPORT_MAP, READ,
 *   05 01 09 05 a1 01 85 01                    gamepad, report 1
 *   05 09 09 01 09 02 09 03 09 04 09 05 09 07 09 08 09 0b 09 0c 09 0d 09 0e 09 0f
 *   09 15 09 16 09 17 09 18 15 00 25 01 75 01 95 10 81 02         16 buttons
 *   05 01 09 39 15 00 25 07 35 00 46 3b 01 65 14 75 04 95 01 81 42 hat, null state
 *   45 00 65 00 05 0c 0a b2 00 15 00 25 01 75 01 95 01 81 02       Record (M)
 *   75 03 95 01 81 03
 *   05 01 15 00 27 ff ff 00 00 75 10 95 06
 *   09 30 09 31 09 33 09 34 09 32 09 35 81 02                      X Y Rx Ry Z Rz
 *   c0
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_REPORT, READ | NOTIFY | DYNAMIC | ENCRYPTION_KEY_SIZE_16,  0x001a
 * REPORT_REFERENCE, READ, 1, 1
 * CHARACTERISTIC, ORG_BLUETOOTH_CHARACTERISTIC_HID_CONTROL_POINT, WRITE_WITHOUT_RESPONSE | DYNAMIC,
 */
static const u8 gble_att_db[] = {
    0x01, 0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18, 0x0d, 0x00, 0x02, 0x00, 0x02,
    0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a, 0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,
    0x0d, 0x00, 0x02, 0x00, 0x04, 0x00, 0x03, 0x28, 0x02, 0x05, 0x00, 0x01, 0x2a, 0x0a, 0x00, 0x02,
    0x00, 0x05, 0x00, 0x01, 0x2a, 0xc4, 0x03, 0x0d, 0x00, 0x02, 0x00, 0x06, 0x00, 0x03, 0x28, 0x02,
    0x07, 0x00, 0x04, 0x2a, 0x10, 0x00, 0x02, 0x00, 0x07, 0x00, 0x04, 0x2a, 0x06, 0x00, 0x09, 0x00,
    0x00, 0x00, 0x90, 0x01, 0x0a, 0x00, 0x02, 0x00, 0x08, 0x00, 0x00, 0x28, 0x01, 0x18, 0x0d, 0x00,
    0x02, 0x00, 0x09, 0x00, 0x03, 0x28, 0x22, 0x0a, 0x00, 0x05, 0x2a, 0x08, 0x00, 0x02, 0x01, 0x0a,
    0x00, 0x05, 0x2a, 0x0a, 0x00, 0x0e, 0x01, 0x0b, 0x00, 0x02, 0x29, 0x00, 0x00, 0x0a, 0x00, 0x02,
    0x00, 0x0c, 0x00, 0x00, 0x28, 0x0a, 0x18, 0x0d, 0x00, 0x02, 0x00, 0x0f, 0x00, 0x03, 0x28, 0x02,
    0x10, 0x00, 0x50, 0x2a, 0x08, 0x00, 0x02, 0x01, 0x10, 0x00, 0x50, 0x2a, 0x0a, 0x00, 0x02, 0x00,
    0x11, 0x00, 0x00, 0x28, 0x0f, 0x18, 0x0d, 0x00, 0x02, 0x00, 0x12, 0x00, 0x03, 0x28, 0x02, 0x13,
    0x00, 0x19, 0x2a, 0x09, 0x00, 0x02, 0x00, 0x13, 0x00, 0x19, 0x2a, 0x64, 0x0a, 0x00, 0x02, 0x00,
    0x14, 0x00, 0x00, 0x28, 0x12, 0x18, 0x0d, 0x00, 0x02, 0x00, 0x15, 0x00, 0x03, 0x28, 0x02, 0x16,
    0x00, 0x4a, 0x2a, 0x0c, 0x00, 0x02, 0x00, 0x16, 0x00, 0x4a, 0x2a, 0x11, 0x01, 0x00, 0x02, 0x0d,
    0x00, 0x02, 0x00, 0x17, 0x00, 0x03, 0x28, 0x02, 0x18, 0x00, 0x4b, 0x2a, 0x86, 0x00, 0x02, 0x00,
    0x18, 0x00, 0x4b, 0x2a, 0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x09, 0x01,
    0x09, 0x02, 0x09, 0x03, 0x09, 0x04, 0x09, 0x05, 0x09, 0x07, 0x09, 0x08, 0x09, 0x0b, 0x09, 0x0c,
    0x09, 0x0d, 0x09, 0x0e, 0x09, 0x0f, 0x09, 0x15, 0x09, 0x16, 0x09, 0x17, 0x09, 0x18, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x45, 0x00, 0x65,
    0x00, 0x05, 0x0c, 0x0a, 0xb2, 0x00, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75,
    0x10, 0x95, 0x06, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x09, 0x32, 0x09, 0x35, 0x81,
    0x02, 0xc0, 0x0d, 0x00, 0x02, 0x00, 0x19, 0x00, 0x03, 0x28, 0x12, 0x1a, 0x00, 0x4d, 0x2a, 0x08,
    0x00, 0x03, 0xf5, 0x1a, 0x00, 0x4d, 0x2a, 0x0a, 0x00, 0x0f, 0xf1, 0x1b, 0x00, 0x02, 0x29, 0x00,
    0x00, 0x0a, 0x00, 0x02, 0x00, 0x1c, 0x00, 0x08, 0x29, 0x01, 0x01, 0x0d, 0x00, 0x02, 0x00, 0x1d,
    0x00, 0x03, 0x28, 0x04, 0x1e, 0x00, 0x4c, 0x2a, 0x08, 0x00, 0x04, 0x01, 0x1e, 0x00, 0x4c, 0x2a,
    0x00, 0x00,
};

static struct {
    u8 last[GBLE_SIZE];
    u8 sent;
    u8 usb;                      /* wired: the Classic generic gamepad's report */
    u8 er[16], ir[16];
} gb;

/* The board's name, then tail, as a scan response, zeros after: for the
 * session's profile (Generic BLE's, config mode's), one at a time. */
u8 profile_scan_response[31];

u8 profile_scan_name(const char *tail)
{
    u32 n = strlen(board_name), k = strlen(tail);

    if (n > sizeof(profile_scan_response) - 2 - k) n = sizeof(profile_scan_response) - 2 - k;
    memset(profile_scan_response, 0, sizeof(profile_scan_response));
    profile_scan_response[0] = (u8)(n + k + 1);
    profile_scan_response[1] = 0x09;   /* complete local name */
    memcpy(profile_scan_response + 2, board_name, n);
    memcpy(profile_scan_response + 2 + n, tail, k);
    return (u8)(n + k + 2);
}

static struct bt_device gble_device;

static u8 gble_begin(const struct profile_session *s)
{
    static const u8 firmware_key[16] = { 'C', 'Y', 'C', 'L', 'O', 'N', 'E', '2',
                                         '-', 'G', 'E', 'N', 'E', 'R', 'I', 'C' };
    u8 block[16];

    gble_device.scan_response_len = profile_scan_name("");
    memset(&gb, 0, sizeof(gb));
    gb.usb = s->usb;
    generic_sent = 0;
    generic_wired = s->usb;
    /* LE security keys for this slot, as profile_xbox.c makes them. */
    memset(block, 0, sizeof(block));
    memcpy(block, s->address, 6);
    block[6] = s->slot;
    block[15] = 'E';
    util_aes128(firmware_key, block, gb.er);
    block[15] = 'I';
    util_aes128(firmware_key, block, gb.ir);
    return IMU_OFF;
}

static void put_axis(u8 *out, u16 v)
{
    out[0] = (u8)v;
    out[1] = (u8)(v >> 8);
}

/* On change only, like the Classic generic gamepad. */
static int gble_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    u32 b = p->buttons;

    if (gb.usb) return generic_report(p, now, out, id);
    out[0] = (u8)((b & (PAD_A | PAD_B | PAD_X | PAD_Y)) | (b & PAD_M ? 0x04 : 0) |
                  (b & (PAD_L1 | PAD_R1)) >> 1 | (b & PAD_SELECT) >> 3);
    out[1] = (u8)((b & (PAD_START | PAD_HOME | PAD_L3 | PAD_R3)) >> 11 |
                  (b & PAD_R4 ? 0x10 : 0) | (b & PAD_L4 ? 0x40 : 0));
    out[2] = (u8)(in_hat(b) | (b & PAD_SCREENSHOT ? 0x10 : 0));
    put_axis(out + 3, (u16)(p->lx + 32768));
    put_axis(out + 5, (u16)(p->ly + 32768));
    put_axis(out + 7, (u16)(p->rx + 32768));
    put_axis(out + 9, (u16)(p->ry + 32768));
    put_axis(out + 11, p->l2);
    put_axis(out + 13, p->r2);
    if (gb.sent && !memcmp(out, gb.last, GBLE_SIZE)) return 0;
    memcpy(gb.last, out, GBLE_SIZE);
    gb.sent = 1;
    *id = 1;
    return GBLE_SIZE;
}

/* Bluetooth thread: fixed Microsoft PnP ID for SDL paddle mapping. */
static int gble_read(u8 kind, u16 handle, u16 offset, u8 *out, u16 max)
{
    (void)offset;
    if (kind != BT_WRITE_ATT) return -1;
    if (handle == 0x0003) {
        u32 n = strlen(board_name);

        if (n > max) n = max;
        memcpy(out, board_name, n);
        return (int)n;
    }
    if (handle != GBLE_PNP || max < 7) return -1;
    out[0] = 0x02;                                 /* vendor ID from the USB-IF */
    put_axis(out + 1, 0x045e);
    put_axis(out + 3, 0x0bfe);
    put_axis(out + 5, GBLE_VERSION);
    return 7;
}

static u8 gble_advertising(const struct bt_bond *bond, int pairing, int wake, u8 out[31])
{
    /* Flags, appearance gamepad, the HID service. */
    static const u8 adv[] = { 0x02, 0x01, 0x06, 0x03, 0x19, 0xc4, 0x03, 0x03, 0x03, 0x12, 0x18 };

    (void)bond;
    (void)wake;
    memcpy(out, adv, sizeof(adv));
    out[2] = pairing ? 0x06 : 0x04;    /* general discoverable only when pairing */
    return sizeof(adv);
}

static struct bt_device gble_device = {
    .transport = BT_LE,
    .name = board_name,
    .att_db = gble_att_db,
    .input_handle = GBLE_INPUT,
    .advertising = gble_advertising,
    .scan_response = profile_scan_response,
    .sm_er = gb.er, .sm_ir = gb.ir,
    .interval_min = 6, .interval_max = 9,
};

const struct profile profile_generic_ble = {
    .name = "genericble", .device = &gble_device, .usb = &generic_usb,
    .begin = gble_begin, .report = gble_report, .read = gble_read,
};

/* ---- keyboard and mouse ------------------------------------------------------------
 * What each control does is the slot's layout (variant).  The stick and the
 * gyro both move the pointer, each at its own speed; a gyro speed of 0 turns
 * the gyro off. Home is in no layout. Physical shortcuts run before this
 * layout, and an unconsumed M retains its layout function. */

#define KEYBOARD_ID   1
#define MOUSE_ID      2
#define KEYBOARD_SIZE 8
#define MOUSE_SIZE    7

enum {
    KB_A = 0x04, KB_D = 0x07, KB_E = 0x08, KB_F = 0x09, KB_Q = 0x14, KB_R = 0x15, KB_S = 0x16,
    KB_W = 0x1a, KB_ENTER = 0x28, KB_ESCAPE = 0x29, KB_TAB = 0x2b, KB_SPACE = 0x2c,
    KB_PRINT = 0x46, KB_RIGHT = 0x4f, KB_LEFT = 0x50, KB_DOWN = 0x51, KB_UP = 0x52,
    KB_CTRL = 0xe0, KB_SHIFT = 0xe1, KB_ALT = 0xe2, KB_GUI = 0xe3,
};
enum { M_LEFT = 1, M_RIGHT = 2, M_MIDDLE = 4, M_BACK = 8, M_FORWARD = 16 };

enum { STICK_NONE, STICK_POINTER, STICK_SCROLL, STICK_WASD };
#define HOLD_SCROLL   1          /* the pointer stick scrolls */
#define HOLD_GYRO_OFF 2          /* the gyro lets go, to put the pad back */
#define HOLD_SLOW     4          /* the pointer moves at a quarter */

struct binding { u32 control; u8 key, mouse, hold; };
struct layout { u8 left, right; const struct binding *b; u8 n; };

static const struct binding game[] = {
    { PAD_A, KB_SPACE }, { PAD_B, KB_CTRL }, { PAD_X, KB_R }, { PAD_Y, KB_E },
    { PAD_L1, KB_Q }, { PAD_R1, KB_F }, { PAD_L3, KB_SHIFT }, { PAD_R3, 0, M_MIDDLE },
    { PAD_L2, 0, M_RIGHT }, { PAD_R2, 0, M_LEFT }, { PAD_SELECT, KB_TAB }, { PAD_START, KB_ESCAPE },
    { PAD_SCREENSHOT, KB_PRINT }, { PAD_L4, 0, M_BACK }, { PAD_R4, 0, M_FORWARD },
    { PAD_M, 0, 0, HOLD_SCROLL | HOLD_GYRO_OFF },
    { PAD_UP, KB_UP }, { PAD_RIGHT, KB_RIGHT }, { PAD_DOWN, KB_DOWN }, { PAD_LEFT, KB_LEFT },
};

/* L4 is Alt and R4 Ctrl, to hold for shortcuts (L4 then Select: Alt+Tab). */
static const struct binding desktop[] = {
    { PAD_A, 0, M_LEFT }, { PAD_B, 0, M_RIGHT }, { PAD_X, 0, M_MIDDLE }, { PAD_Y, KB_ENTER },
    { PAD_L1, 0, M_BACK }, { PAD_R1, 0, M_FORWARD }, { PAD_L3, KB_GUI }, { PAD_R3, KB_SHIFT },
    { PAD_L2, 0, M_RIGHT }, { PAD_R2, 0, M_LEFT }, { PAD_SELECT, KB_TAB }, { PAD_START, KB_ESCAPE },
    { PAD_SCREENSHOT, KB_PRINT }, { PAD_L4, KB_ALT }, { PAD_R4, KB_CTRL },
    { PAD_M, 0, 0, HOLD_SLOW | HOLD_GYRO_OFF },
    { PAD_UP, KB_UP }, { PAD_RIGHT, KB_RIGHT }, { PAD_DOWN, KB_DOWN }, { PAD_LEFT, KB_LEFT },
};

static const struct layout layouts[2] = {
    { STICK_WASD, STICK_POINTER, game, sizeof(game) / sizeof(game[0]) },
    { STICK_POINTER, STICK_SCROLL, desktop, sizeof(desktop) / sizeof(desktop[0]) },
};

#define MOVE_PRESS      (STICK_MAX / 2)
#define MOVE_RELEASE    (STICK_MAX * 2 / 5)
#define SCROLL_DEADZONE (STICK_MAX * 35 / 100)
#define SCROLL_PER_SEC  15
#define MAX_DT_MS       50

static const u8 mouse_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x75, 0x01, 0x95, 0x05, 0x91, 0x02, 0x75, 0x03, 0x95, 0x01, 0x91, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06,
    0x81, 0x00, 0xc0, 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02, 0x75,
    0x03, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff,
    0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95,
    0x01, 0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};

static struct {
    const struct layout *layout;
    u16 stick_speed, gyro_speed, gyro_accel;
    u8 keyboard[KEYBOARD_SIZE], keyboard_sent;
    u8 buttons;
    u8 held[2];                  /* stick-as-keys: a bit per direction */
    u32 last_ms;
    u8 have_last;
    s64 rest[2], scroll_rest[2];
    s32 pending[2], wheel[2];
    struct imu_pointer pointer;
} m;

static u32 isqrt(u32 v)
{
    u32 r = 0, bit = 1u << 30;

    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static void press(u8 *kb, u8 key)
{
    u32 i;

    if (!key) return;
    if (key >= KB_CTRL) {
        kb[0] |= (u8)(1u << (key - KB_CTRL));
        return;
    }
    for (i = 2; i < KEYBOARD_SIZE; i++) {
        if (kb[i] == key) return;
        if (!kb[i]) {
            kb[i] = key;
            return;
        }
    }
}

/* A stick as four keys, with hysteresis per direction; y is positive down. */
static void stick_keys(u8 *kb, u8 *held, s16 x, s16 y)
{
    static const u8 keys[4] = { KB_W, KB_D, KB_S, KB_A };
    const s32 v[4] = { -(s32)y, x, y, -(s32)x };
    u32 d;

    for (d = 0; d < 4; d++) {
        if (v[d] >= MOVE_PRESS) *held |= (u8)(1u << d);
        else if (v[d] < MOVE_RELEASE) *held &= (u8)~(1u << d);
        if (*held & (1u << d)) press(kb, keys[d]);
    }
}

/* Pointer motion over dt at `speed` counts a second all the way over, with
 * the square of the deflection for fine aim near the centre. */
static void stick_motion(s16 x, s16 y, u32 dt, u32 speed)
{
    const s32 axis[2] = { x, y };
    const s64 per_count = (s64)STICK_MAX * STICK_MAX * 1000;
    u32 reach = isqrt((u32)(axis[0] * axis[0]) + (u32)(axis[1] * axis[1]));
    int i;

    if (reach > STICK_MAX) reach = STICK_MAX;
    for (i = 0; i < 2; i++) {
        s64 total = (s64)axis[i] * (s32)reach * speed * dt + m.rest[i];
        s32 out = (s32)(total / per_count);

        m.rest[i] = total - (s64)out * per_count;
        m.pending[i] += out;
    }
}

/* Wheel notches (up positive) and pan (right positive). */
static void scroll_motion(s16 x, s16 y, u32 dt)
{
    const s32 axis[2] = { -(s32)y, x };
    const s32 travel = STICK_MAX - SCROLL_DEADZONE;
    const s64 per_notch = (s64)travel * travel * 1000;
    int i;

    for (i = 0; i < 2; i++) {
        s32 past = (axis[i] < 0 ? -axis[i] : axis[i]) - SCROLL_DEADZONE;
        s64 rate;
        s32 out;

        if (past <= 0) {
            m.scroll_rest[i] = 0;
            continue;
        }
        if (past > travel) past = travel;
        rate = (s64)past * past * SCROLL_PER_SEC;
        m.scroll_rest[i] += (axis[i] < 0 ? -rate : rate) * dt;
        out = (s32)(m.scroll_rest[i] / per_notch);
        m.scroll_rest[i] -= (s64)out * per_notch;
        m.wheel[i] += out;
    }
}

static void stick(u8 role, u8 index, s16 x, s16 y, u8 hold, u32 dt, u8 *kb)
{
    if (role == STICK_POINTER && (hold & HOLD_SCROLL)) role = STICK_SCROLL;
    if (role == STICK_POINTER) stick_motion(x, y, dt, hold & HOLD_SLOW ? m.stick_speed / 4u : m.stick_speed);
    else if (role == STICK_SCROLL) scroll_motion(x, y, dt);
    else if (role == STICK_WASD) stick_keys(kb, &m.held[index], x, y);
}

static s32 clamp(s32 v, s32 l)
{
    return v > l ? l : v < -l ? -l : v;
}

static u8 mouse_begin(const struct profile_session *s)
{
    memset(&m, 0, sizeof(m));
    m.layout = &layouts[s->settings && s->settings->variant == 1 ? 1 : 0];
    m.stick_speed = s->settings ? s->settings->stick_speed : 1500;
    m.gyro_speed = s->settings ? s->settings->gyro_speed : 0;
    m.gyro_accel = s->settings ? s->settings->gyro_accel : 100;
    return m.gyro_speed ? IMU_GYRO : IMU_OFF;
}

/* Keys go as messages, each change of them must arrive.  Mouse reports
 * carry relative motion; they are only built when the link has room for one
 * (pull), so motion gathers until then and none is replaced. */
static int mouse_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    u32 controls = p->buttons, dt = 0, i;
    u8 kb[KEYBOARD_SIZE], buttons = 0, hold = 0;
    s32 gx, gy, x, y;

    if (m.have_last) dt = now - m.last_ms;
    if (dt > MAX_DT_MS) dt = MAX_DT_MS;
    m.last_ms = now;
    m.have_last = 1;
    memset(kb, 0, sizeof(kb));
    for (i = 0; i < m.layout->n; i++) {
        const struct binding *b = &m.layout->b[i];

        if (!(controls & b->control)) continue;
        press(kb, b->key);
        buttons |= b->mouse;
        hold |= b->hold;
    }
    stick(m.layout->left, 0, p->lx, p->ly, hold, dt, kb);
    stick(m.layout->right, 1, p->rx, p->ry, hold, dt, kb);
    if (m.gyro_speed) {
        /* Always taken, so turns made while the gyro is let go are dropped. */
        imu_pointer(&m.pointer, p, m.gyro_speed, m.gyro_accel, &gx, &gy);
        if (!(hold & HOLD_GYRO_OFF)) {
            m.pending[0] += gx;
            m.pending[1] += gy;
        }
    }
    if ((!m.keyboard_sent || memcmp(kb, m.keyboard, sizeof(kb))) &&
        host_send(KEYBOARD_ID, kb, sizeof(kb))) {
        memcpy(m.keyboard, kb, sizeof(kb));
        m.keyboard_sent = 1;
    }
    if (!m.pending[0] && !m.pending[1] && !m.wheel[0] && !m.wheel[1] && buttons == m.buttons) return 0;
    x = clamp(m.pending[0], 32767);
    y = clamp(m.pending[1], 32767);
    out[0] = buttons;
    util_put16(out + 1, (u16)x);
    util_put16(out + 3, (u16)y);
    out[5] = (u8)(s8)clamp(m.wheel[0], 127);
    out[6] = (u8)(s8)clamp(m.wheel[1], 127);
    m.pending[0] -= x;
    m.pending[1] -= y;
    m.wheel[0] -= (s8)out[5];
    m.wheel[1] -= (s8)out[6];
    m.buttons = buttons;
    *id = MOUSE_ID;
    return MOUSE_SIZE;
}

static const struct bt_device mouse_device = {
    .transport = BT_CLASSIC, .name = board_name, .class_of_device = 0x0025c0,
    .vendor_id = 0x1209, .product_id = 0x0001, .version = 0x0100,  /* pid.codes' test product */
    .hid_descriptor = mouse_descriptor, .hid_descriptor_len = sizeof(mouse_descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

const struct profile profile_mouse = {
    .name = "mouse", .device = &mouse_device,
    .begin = mouse_begin, .report = mouse_report,
};

#ifdef UNITTEST
const char board_name[] = "Test controller";
static u8 kb_sent[8];
int host_send(u16 id, const u8 *data, u16 len) { (void)id; memcpy(kb_sent, data, len); return 1; }
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void util_aes128(const u8 key[16], const u8 in[16], u8 out[16]) { (void)key; memcpy(out, in, 16); }
void imu_pointer(struct imu_pointer *pt, const struct pad *pad, u16 per_degree, u16 accel,
                 s32 *x, s32 *y)
{
    (void)pt; (void)pad; (void)per_degree; (void)accel;
    *x = 5;
    *y = -3;
}

int main(void)
{
    struct set_slot set = { .type = PROFILE_MOUSE, .stick_speed = 1000, .gyro_speed = 32, .gyro_accel = 100 };
    struct profile_session s = { 4, 0, { 0 }, 0, &set };
    struct pad p;
    u8 out[16], id;

    memset(&p, 0, sizeof(p));
    CHECK(generic_begin(&s) == IMU_OFF);
    p.buttons = PAD_A | PAD_START;
    CHECK(generic_report(&p, 0, out, &id) == GENERIC_SIZE && out[0] == 1 && out[1] == 8 && out[3] == 0x80);
    CHECK(generic_report(&p, 8, out, &id) == 0);

    CHECK(mouse_begin(&s) == IMU_GYRO && m.layout == &layouts[0]);
    p.buttons = PAD_A;
    p.ly = -STICK_MAX;              /* left stick up: W */
    p.r2 = TRIGGER_MAX;
    p.buttons |= PAD_R2;
    p.rx = STICK_MAX;
    CHECK(mouse_report(&p, 0, out, &id) == MOUSE_SIZE && id == MOUSE_ID);
    CHECK(kb_sent[2] == KB_SPACE && kb_sent[3] == KB_W && out[0] == M_LEFT);
    CHECK((s16)(out[1] | out[2] << 8) == 5);
    CHECK(mouse_report(&p, 20, out, &id) == MOUSE_SIZE);
    CHECK((s16)(out[1] | out[2] << 8) == 20 + 5);    /* 20 ms of stick at 1000/s, a gyro take */

    set.variant = 1;
    set.gyro_speed = 0;
    CHECK(mouse_begin(&s) == IMU_OFF && m.layout == &layouts[1]);

    /* genericble: Linux button order, paddles at 21/23, Share as Record, C/M as Button 3. */
    memset(&p, 0, sizeof(p));
    CHECK(gble_begin(&s) == IMU_OFF && gb.er[6] == 4 && gb.er[15] == 'E');
    CHECK(gble_device.scan_response_len == strlen(board_name) + 2);
    CHECK(profile_scan_response[0] == strlen(board_name) + 1 && profile_scan_response[1] == 9);
    CHECK(!memcmp(profile_scan_response + 2, board_name, strlen(board_name)));
    CHECK(profile_scan_name(" Config") == strlen(board_name) + 9 &&
          !memcmp(profile_scan_response + 2 + strlen(board_name), " Config", 7));
    CHECK(gble_read(BT_WRITE_ATT, 3, 0, out, sizeof(out)) == strlen(board_name));
    CHECK(!memcmp(out, board_name, strlen(board_name)));
    CHECK(gble_report(&p, 0, out, &id) == GBLE_SIZE && id == 1);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 8);
    CHECK(out[3] == 0 && out[4] == 0x80 && out[5] == 0 && out[6] == 0x80 &&
          out[7] == 0 && out[8] == 0x80 && out[9] == 0 && out[10] == 0x80);
    CHECK(out[11] == 0 && out[12] == 0 && out[13] == 0 && out[14] == 0);
    p.buttons = PAD_A | PAD_Y | PAD_L1 | PAD_SELECT | PAD_START | PAD_R3 | PAD_R4 | PAD_L4 | PAD_SCREENSHOT;
    p.lx = -32768;
    p.ry = STICK_MAX;
    p.l2 = TRIGGER_MAX;
    p.buttons |= PAD_L2;
    CHECK(gble_report(&p, 0, out, &id) == GBLE_SIZE && id == 1);
    CHECK(out[0] == (0x01 | 0x10 | 0x20 | 0x80) && out[1] == (0x01 | 0x08 | 0x10 | 0x40));
    CHECK(out[2] == (8 | 0x10));
    CHECK(out[3] == 0 && out[4] == 0 && out[5] == 0 && out[6] == 0x80 && out[9] == 0xff && out[10] == 0xff);
    CHECK(out[11] == 0xff && out[12] == 0xff && out[13] == 0 && out[14] == 0);
    CHECK(gble_report(&p, 1, out, &id) == 0);
    p.buttons = PAD_M | PAD_LEFT;
    CHECK(gble_report(&p, 2, out, &id) == GBLE_SIZE && out[0] == 0x04 && out[1] == 0 && out[2] == 6);
    p.buttons = PAD_SCREENSHOT | PAD_LEFT;
    CHECK(gble_report(&p, 3, out, &id) == GBLE_SIZE && out[0] == 0 && out[1] == 0 && out[2] == (6 | 0x10));
    CHECK(gble_read(BT_WRITE_ATT, GBLE_PNP, 0, out, 16) == 7 && out[0] == 2 &&
          out[1] == 0x5e && out[2] == 0x04 && out[3] == 0xfe && out[4] == 0x0b &&
          out[5] == 0 && out[6] == 1);
    CHECK(gble_read(BT_WRITE_ATT, GBLE_INPUT, 0, out, 16) < 0);
    /* The handles the code names are the ones the database has; bt_pad.c takes
     * the one after the input report for its notification switch. */
    CHECK(test_att_uuid(gble_att_db, GBLE_INPUT) == 0x2a4d && test_att_uuid(gble_att_db, GBLE_INPUT + 1) == 0x2902);
    CHECK(test_att_uuid(gble_att_db, GBLE_PNP) == 0x2a50);
    return TEST_DONE("profile_hid");
}
#endif
