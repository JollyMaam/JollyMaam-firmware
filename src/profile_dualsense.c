#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "imu.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "usb.h"

/*
 * DualSense Edge over Bluetooth Classic HID (054c:0df2).
 * ======================================================
 *
 * The one long-established controller whose back paddles and gyro reach
 * applications without configuration: SDL's HIDAPI PS5 driver (SDL2 and 3),
 * Steam Input and Linux's hid-playstation all know it.  Nothing Sony-published
 * here; the layouts are the ones SDL's SDL_hidapi_ps5.c and hid-playstation
 * read, and the report descriptor follows the stock firmware's DualShock 4
 * one (same simple report, extended reports 0x31..0x39 instead of 0x11..0x19).
 *
 * Reports (offsets are into the report body, after the report id)
 * ---------------------------------------------------------------
 * Input 0x01, 9 bytes - "simple" report, what a DualSense sends over
 * Bluetooth until a host reads a feature report or sends it an output report;
 * the one generic HID drivers (DirectInput, Android) use:
 *   [0..3]  left X, left Y, right X, right Y (down is positive)
 *   [4]     hat (low nibble, 0 = up clockwise to 7, 8 = centred), square,
 *           cross, circle, triangle (bits 4..7)
 *   [5]     L1, R1, L2, R2, Create, Options, L3, R3
 *   [6]     PS, touchpad click, then a 6-bit counter
 *   [7..8]  L2, R2 analog
 *
 * Input 0x31, 77 bytes - extended report:
 *   [0]      sequence tag (high nibble)
 *   [1..54]  state, as SDL's PS5StatePacket_t:
 *            +0 sticks (4), +4 L2, +5 R2, +6 counter, +7 buttons (4),
 *            +11 packet sequence (32 bit), +15 gyro X/Y/Z, +21 accel X/Y/Z
 *            (16 bit each), +27 sensor timestamp (32 bit, 1/3 us),
 *            +31 temperature, +32/+36 touch points (bit 7 set = no touch),
 *            +52 battery (status << 4 | level), +53 connection state
 *   [73..76] CRC-32 of 0xa1, 0x31 and bytes [0..72]
 *   Button bytes (+7..+9): [hat | square 0x10, cross 0x20, circle 0x40,
 *   triangle 0x80], [L1 0x01, R1 0x02, L2 0x04, R2 0x08, Create 0x10,
 *   Options 0x20, L3 0x40, R3 0x80], [PS 0x01, touchpad 0x02, mute 0x04,
 *   left Fn 0x10, right Fn 0x20, left paddle 0x40, right paddle 0x80].
 *
 * Output 0x31, 77 bytes (host -> pad): [0] sequence tag, [1] 0x10, [2..]
 * effects as SDL's DS5EffectsState_t: +0 valid flags 0 (0x01 compatible
 * vibration, 0x02 haptics select), +1 valid flags 1 (lights), +2 right
 * (high-frequency) motor, +3 left (low-frequency) motor, +38 valid flags 2
 * (0x04 compatible vibration v2); CRC at the end, not checked here.
 *
 * Feature reports (GET_REPORT), each ending in a CRC-32 of 0xa3, the report
 * id and the body before it (hid-playstation checks it over Bluetooth):
 *   0x05, 40 bytes: IMU calibration
 *   0x09, 19 bytes: pairing info, [0..5] our address least significant first
 *   0x20, 63 bytes: firmware info, [43..44] update version
 *
 * Mapping
 * -------
 * The pad's positional face buttons go to the same positions (A south =
 * cross, B east = circle, X west = square, Y north = triangle), Select to
 * Create, Start to Options, Home to PS, L4/R4 to the left/right paddles, M
 * (the Switch 2 profile's C) to the touchpad click and the capture button to
 * mute - both are buttons SDL and Steam expose (SDL: touchpad, misc1).
 */

#define DS_REPORT_SIMPLE        0x01U
#define DS_REPORT_EXTENDED      0x31U
#define DS_FEATURE_CALIBRATION  0x05U
#define DS_FEATURE_PAIRING      0x09U
#define DS_FEATURE_FIRMWARE     0x20U
#define DS_SIMPLE_SIZE          9U
#define DS_EXTENDED_SIZE        77U
#define DS_SIMPLE_KEEPALIVE_MS  100U
#define DS_TRIGGER_BUTTON_LEVEL 0x20U

/* Start with extended reports instead of waiting for the host to ask.
 * SDL decides at open time: a pad already sending 0x31 is run in full mode
 * (paddles, gyro, touchpad, rumble) straight away; one sending 0x01 only gets
 * switched over once an application uses rumble, sensors or LEDs, and the
 * simple report has no paddles.  A real DualSense behaves this way once any
 * program has touched it.  The cost: generic HID readers (DirectInput) see
 * only the simple report, and get nothing from a pad in extended mode. */
#define DS_START_EXTENDED 1

#define DS_HID_INPUT  1U
#define DS_HID_FEATURE 3U

/* Descriptor: Generic Desktop gamepad, simple report 0x01 with usages a
 * generic driver understands, then vendor-defined extended reports and
 * feature reports.  Item encodings follow the stock DualShock 4 descriptor. */
static const u8 dualsense_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,             /* gamepad */
    0x85, 0x01,                                     /* report 0x01 */
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, /* X Y Z Rz */
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, /* hat */
    0x95, 0x01, 0x81, 0x42,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, /* 14 buttons */
    0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x01,             /* counter */
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, /* Rx Ry: L2 R2 */
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x06, 0x00, 0xff,                               /* vendor page */
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x85, 0x31, 0x09, 0x31, 0x95, 0x4d, 0x81, 0x02, 0x09, 0x3b, 0x91, 0x02,
    0x85, 0x32, 0x09, 0x32, 0x95, 0x8d, 0x81, 0x02, 0x09, 0x3c, 0x91, 0x02,
    0x85, 0x33, 0x09, 0x33, 0x95, 0xcd, 0x81, 0x02, 0x09, 0x3d, 0x91, 0x02,
    0x85, 0x34, 0x09, 0x34, 0x96, 0x0d, 0x01, 0x81, 0x02, 0x09, 0x3e, 0x91, 0x02,
    0x85, 0x35, 0x09, 0x35, 0x96, 0x4d, 0x01, 0x81, 0x02, 0x09, 0x3f, 0x91, 0x02,
    0x85, 0x36, 0x09, 0x36, 0x96, 0x8d, 0x01, 0x81, 0x02, 0x09, 0x40, 0x91, 0x02,
    0x85, 0x37, 0x09, 0x37, 0x96, 0xcd, 0x01, 0x81, 0x02, 0x09, 0x41, 0x91, 0x02,
    0x85, 0x38, 0x09, 0x38, 0x96, 0x0d, 0x02, 0x81, 0x02, 0x09, 0x42, 0x91, 0x02,
    0x85, 0x39, 0x09, 0x39, 0x96, 0x22, 0x02, 0x81, 0x02, 0x09, 0x43, 0x91, 0x02,
    0x06, 0x80, 0xff,                               /* feature reports */
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xb1, 0x02, /* calibration */
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2f, 0xb1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xb1, 0x02, /* pairing info */
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3f, 0xb1, 0x02, /* firmware info */
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0x80, 0x09, 0x28, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0x82, 0x09, 0x2a, 0x95, 0x09, 0xb1, 0x02,
    0x85, 0x83, 0x09, 0x2b, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0xf0, 0x09, 0x47, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0xf1, 0x09, 0x48, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0xf2, 0x09, 0x49, 0x95, 0x0f, 0xb1, 0x02,
    0xc0,
};

/* 15 ms sniff interval, as for the other Classic profiles. */
static const edr_sniff_par_t dualsense_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

/* Peripheral / gamepad class of device, "just works" SSP. */
static const edr_init_cfg_t dualsense_edr_config = {
    .class_type = 0x002508U,
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = sizeof(dualsense_report_descriptor),
    .report_map = dualsense_report_descriptor,
    .sniff_param = &dualsense_sniff_param,
};

static volatile u8 extended_reports;
static u8 report_counter;
static u32 packet_sequence;
static u8 simple_last[DS_SIMPLE_SIZE];
static u8 simple_sent;
static u32 simple_last_send_ms;

/* CRC-32 (IEEE, reflected, as zlib's crc32()), four bits at a time. */
static u32 ds_crc32(u32 crc, const u8 *data, u16 length)
{
    static const u32 table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
    };

    crc = ~crc;
    while (length--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 0x0fU];
        crc = (crc >> 4) ^ table[crc & 0x0fU];
    }
    return ~crc;
}

static void put_u16(u8 *output, u16 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
}

static void put_u32(u8 *output, u32 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
    output[2] = (u8)(value >> 16);
    output[3] = (u8)(value >> 24);
}

/* CRC over the HIDP header byte, the report id and the body before the last
 * four bytes, stored little endian in those four bytes. */
static void ds_seal(u8 header, u8 report_id, u8 *body, u16 size)
{
    u8 prefix[2];
    u32 crc;

    prefix[0] = header;
    prefix[1] = report_id;
    crc = ds_crc32(0U, prefix, sizeof(prefix));
    crc = ds_crc32(crc, body, (u16)(size - 4U));
    put_u32(body + size - 4U, crc);
}

/* The three button bytes shared by both reports; the simple report uses the
 * first two and the low bits of the third. */
static void ds_buttons(const struct cyclone2_input_report *input, u8 out[3])
{
    u16 buttons = input->buttons;

    out[0] = input->hat <= CYCLONE2_HAT_NEUTRAL ? input->hat :
             CYCLONE2_HAT_NEUTRAL;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) out[0] |= 0x10U; /* square */
    /* A and B swapped, as the Xbox profile needed (tested 2026-09-26). */
    if (buttons & CYCLONE2_REPORT_BUTTON_B) out[0] |= 0x20U; /* cross */
    if (buttons & CYCLONE2_REPORT_BUTTON_A) out[0] |= 0x40U; /* circle */
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) out[0] |= 0x80U; /* triangle */

    out[1] = 0U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) out[1] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) out[1] |= 0x02U;
    if (cyclone2_trigger_u8(input->left_trigger) > DS_TRIGGER_BUTTON_LEVEL)
        out[1] |= 0x04U;
    if (cyclone2_trigger_u8(input->right_trigger) > DS_TRIGGER_BUTTON_LEVEL)
        out[1] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) out[1] |= 0x10U;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) out[1] |= 0x20U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) out[1] |= 0x40U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) out[1] |= 0x80U;

    out[2] = 0U;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) out[2] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_M) out[2] |= 0x02U;          /* touchpad */
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) out[2] |= 0x04U; /* mute */
    if (buttons & CYCLONE2_REPORT_BUTTON_L4) out[2] |= 0x40U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R4) out[2] |= 0x80U;
}

static void fill_simple(u8 report[DS_SIMPLE_SIZE],
                        const struct cyclone2_input_report *input)
{
    u8 buttons[3];

    ds_buttons(input, buttons);
    report[0] = cyclone2_stick_u8(input->left_x);
    report[1] = cyclone2_stick_u8(input->left_y);
    report[2] = cyclone2_stick_u8(input->right_x);
    report[3] = cyclone2_stick_u8(input->right_y);
    report[4] = buttons[0];
    report[5] = buttons[1];
    report[6] = (u8)((buttons[2] & 0x03U) | (u8)(report_counter << 2));
    report[7] = cyclone2_trigger_u8(input->left_trigger);
    report[8] = cyclone2_trigger_u8(input->right_trigger);
}

static void fill_extended(u8 report[DS_EXTENDED_SIZE],
                          const struct cyclone2_input_report *input)
{
    struct cyclone2_imu_sample sample;
    u8 *state = report + 1;

    memset(report, 0, DS_EXTENDED_SIZE);
    report[0] = (u8)(report_counter << 4);
    state[0] = cyclone2_stick_u8(input->left_x);
    state[1] = cyclone2_stick_u8(input->left_y);
    state[2] = cyclone2_stick_u8(input->right_x);
    state[3] = cyclone2_stick_u8(input->right_y);
    state[4] = cyclone2_trigger_u8(input->left_trigger);
    state[5] = cyclone2_trigger_u8(input->right_trigger);
    state[6] = report_counter;
    ds_buttons(input, state + 7);
    put_u32(state + 11, packet_sequence);
    if (cyclone2_imu_latest(&sample) == 0) {
        /* QMI8658A at +/-2048 dps (16 counts per dps) and +/-4 g (8192
         * counts per g) - exactly the scale the calibration report below
         * declares.  Axes: this is the Switch profile's mapping carried
         * through SDL's Switch-to-PlayStation conversion, so SDL reports the
         * same motion for both profiles. */
        put_u16(state + 15, (u16)sample.gyro_y);
        put_u16(state + 17, (u16)sample.gyro_z);
        put_u16(state + 19, (u16)sample.gyro_x);
        put_u16(state + 21, (u16)(s16)-sample.accel_y);
        put_u16(state + 23, (u16)sample.accel_z);
        put_u16(state + 25, (u16)(s16)-sample.accel_x);
        /* 1/3 microsecond units; wraps every ~24 minutes like the real one. */
        put_u32(state + 27, sample.timestamp_ms * 3000U);
    }
    state[31] = 0x16U;  /* temperature, as a real pad idles */
    state[32] = 0x80U;  /* touch point 1: not touching */
    state[36] = 0x80U;  /* touch point 2: not touching */
    state[52] = 0x0aU;  /* discharging, 100 % - the Cyclone 2 has no gauge */
    state[53] = 0x00U;  /* no USB, no headset */
    ds_seal(0xa1U, DS_REPORT_EXTENDED, report, DS_EXTENDED_SIZE);
}

/* Host GET_REPORT, on the BTstack run loop.  Reading any of these switches a
 * real DualSense to extended reports; so does this one. */
static int dualsense_get_report(u8 report_type, u8 report_id, u8 *report,
                                u16 size)
{
    if (report_type != DS_HID_FEATURE) return 0;
    memset(report, 0, size);
    switch (report_id) {
    case DS_FEATURE_CALIBRATION:
        if (size < 36U + 4U) return 0;
        /* Gyro: bias 0, +/-8000 counts for +/-500 dps, i.e. 16 counts per
         * dps.  Accel: +/-8192 counts for +/-1 g.  SDL and hid-playstation
         * both derive their scale from these the same way. */
        put_u16(report + 6, 8000U);            /* pitch + */
        put_u16(report + 8, (u16)(s16)-8000);  /* pitch - */
        put_u16(report + 10, 8000U);           /* yaw + */
        put_u16(report + 12, (u16)(s16)-8000); /* yaw - */
        put_u16(report + 14, 8000U);           /* roll + */
        put_u16(report + 16, (u16)(s16)-8000); /* roll - */
        put_u16(report + 18, 500U);            /* speed + */
        put_u16(report + 20, 500U);            /* speed - */
        put_u16(report + 22, 8192U);           /* accel X + */
        put_u16(report + 24, (u16)(s16)-8192); /* accel X - */
        put_u16(report + 26, 8192U);
        put_u16(report + 28, (u16)(s16)-8192);
        put_u16(report + 30, 8192U);
        put_u16(report + 32, (u16)(s16)-8192);
        break;
    case DS_FEATURE_PAIRING: {
        const u8 *address = bt_get_mac_addr();

        if (size < 6U + 4U) return 0;
        if (address) memcpy(report, address, 6U); /* least significant first */
        break;
    }
    case DS_FEATURE_FIRMWARE:
        if (size < 45U + 4U) return 0;
        /* Update version 4.00: newer than the 2.21 from which Linux uses
         * vibration v2 and the 2.24 from which SDL uses improved rumble. */
        put_u16(report + 43, 0x0400U);
        break;
    default:
        return 0;
    }
    if (size >= 4U) ds_seal(0xa3U, report_id, report, size);
    extended_reports = 1U;
    return 1;
}

/* Output report 0x31: rumble.  Lights are not wired to anything yet. */
static void dualsense_output(u8 *packet, u16 size, u16 channel)
{
    const u8 *effects;
    u8 valid0;
    u8 valid1;
    u8 valid2;

    (void)channel;
    if (size < 2U || (packet[0] != 0xa2U && packet[0] != 0x52U)) return;
    if (packet[1] != DS_REPORT_EXTENDED || size < 2U + 2U + 39U) return;
    extended_reports = 1U;
    effects = packet + 4;
    valid0 = effects[0];
    valid1 = effects[1];
    valid2 = effects[38];
    if ((valid0 & 0x01U) || (valid2 & 0x04U)) {
        /* Hosts refresh or stop rumble themselves; no timeout here. */
        cyclone2_rumble_set_latched(effects[3], effects[2]);
    } else if (valid1 == 0U) {
        /* No vibration and no lights: SDL's "rumble off".  A lights-only
         * update (valid1 set) leaves the motors alone. */
        cyclone2_rumble_stop();
    }
}

static void dualsense_session_start(void)
{
    extended_reports = DS_START_EXTENDED;
    report_counter = 0U;
    packet_sequence = 0U;
    simple_sent = 0U;
    cyclone2_imu_enable();
}

static void dualsense_link_up(void)
{
    extended_reports = DS_START_EXTENDED;
    simple_sent = 0U;
}

static void dualsense_session_exit(void)
{
    cyclone2_rumble_stop();
    cyclone2_imu_disable();
}

static const struct cyclone2_classic_profile dualsense_classic = {
    .local_name = "DualSense Edge Wireless Controller",
    .edr = &dualsense_edr_config,
    .vendor_id = 0x054cU,
    .product_id = 0x0df2U,
    .version = 0x0100U,
    .output = dualsense_output,
    .get_report = dualsense_get_report,
    .session_start = dualsense_session_start,
    .link_up = dualsense_link_up,
    .session_exit = dualsense_session_exit,
};

void cyclone2_dualsense_start(u8 slot, u8 pairing)
{
    cyclone2_classic_start(&dualsense_classic, slot, pairing);
}

/* Extended reports go out on every call (the scheduler's report period), as a
 * real pad streams them; simple reports on change plus a keepalive.  Returns
 * as cyclone2_profile_send() documents. */
int cyclone2_dualsense_send(const struct cyclone2_input_report *input)
{
    u8 report[DS_EXTENDED_SIZE];
    u32 now;
    int result;

    if (!edr_hid_is_connected()) {
        simple_sent = 0U;
        return 0;
    }
    /* Never queue behind an unsent report: when the link drains slower than
     * the report period (sniff), a queue of stale full-state reports is
     * pure input lag.  The next poll sends the then-current state. */
    if (edr_hid_is_busy()) return -1;
    if (extended_reports) {
        fill_extended(report, input);
        result = edr_hid_data_send(DS_REPORT_EXTENDED, report,
                                   DS_EXTENDED_SIZE);
    } else {
        now = sys_timer_get_ms();
        fill_simple(report, input);
        /* Byte 6 carries the report counter in its top six bits; only the
         * two button bits in it count as a change. */
        if (simple_sent &&
            !memcmp(report, simple_last, 6U) &&
            (report[6] & 0x03U) == (simple_last[6] & 0x03U) &&
            !memcmp(report + 7, simple_last + 7, DS_SIMPLE_SIZE - 7U) &&
            now - simple_last_send_ms < DS_SIMPLE_KEEPALIVE_MS)
            return 0;
        result = edr_hid_data_send(DS_REPORT_SIMPLE, report, DS_SIMPLE_SIZE);
        if (result == 0) {
            memcpy(simple_last, report, DS_SIMPLE_SIZE);
            simple_sent = 1U;
            simple_last_send_ms = now;
        }
    }
    if (result == 3) return -1; /* HID transmit queue full */
    if (result != 0) return -2;
    report_counter++;
    packet_sequence++;
    return 1;
}
