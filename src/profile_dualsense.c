#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "btstack_jl_hid_ext.h"
#include "imu.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "lights.h"
#include "rumble.h"
#include "usb.h"

/* DualSense Edge identity; physical axis orientation and host compatibility
 * still need hardware validation. */

#define DS_REPORT_SIMPLE        0x01U
#define DS_REPORT_EXTENDED      0x31U
/* Output report 0x31: offsets into its common effects block (after the
 * report id and two tag bytes), as Linux hid-playstation lays it out. */
#define DS_EFFECTS_SIZE              47U
#define DS_EFFECT_PLAYER_LIGHTS      43U
#define DS_EFFECT_LIGHTBAR           44U
#define DS_VALID1_LIGHTBAR           0x04U
#define DS_VALID1_PLAYER_LIGHTS      0x10U
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

static const u8 dualsense_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
    0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04,
    0x95, 0x01, 0x81, 0x42,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x06, 0x00, 0xff,
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
    0x06, 0x80, 0xff,
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xb1, 0x02,
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2f, 0xb1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xb1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3f, 0xb1, 0x02,
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

static const edr_sniff_par_t dualsense_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

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

static void ds_buttons(const struct cyclone2_input_report *input, u8 out[3])
{
    u16 buttons = input->buttons;

    out[0] = input->hat <= CYCLONE2_HAT_NEUTRAL ? input->hat :
             CYCLONE2_HAT_NEUTRAL;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) out[0] |= 0x10U;

    if (buttons & CYCLONE2_REPORT_BUTTON_A) out[0] |= 0x20U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) out[0] |= 0x40U;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) out[0] |= 0x80U;

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
    if (buttons & CYCLONE2_REPORT_BUTTON_M) out[2] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) out[2] |= 0x04U;
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

        put_u16(state + 15, (u16)sample.gyro_y);
        put_u16(state + 17, (u16)sample.gyro_z);
        put_u16(state + 19, (u16)sample.gyro_x);
        put_u16(state + 21, (u16)(s16)-sample.accel_y);
        put_u16(state + 23, (u16)sample.accel_z);
        put_u16(state + 25, (u16)(s16)-sample.accel_x);

        put_u32(state + 27, sample.timestamp_ms * 3000U);
    }
    state[31] = 0x16U;
    state[32] = 0x80U;
    state[36] = 0x80U;
    state[52] = 0x0aU;
    state[53] = 0x00U;
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

        put_u16(report + 6, 8000U);
        put_u16(report + 8, (u16)(s16)-8000);
        put_u16(report + 10, 8000U);
        put_u16(report + 12, (u16)(s16)-8000);
        put_u16(report + 14, 8000U);
        put_u16(report + 16, (u16)(s16)-8000);
        put_u16(report + 18, 500U);
        put_u16(report + 20, 500U);
        put_u16(report + 22, 8192U);
        put_u16(report + 24, (u16)(s16)-8192);
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

        put_u16(report + 43, 0x0400U);
        break;
    default:
        return 0;
    }
    if (size >= 4U) ds_seal(0xa3U, report_id, report, size);
    extended_reports = 1U;
    return 1;
}

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

        cyclone2_rumble_set_latched(effects[3], effects[2]);
    } else if (valid1 == 0U) {

        cyclone2_rumble_stop();
    }
    if (size < 4U + DS_EFFECTS_SIZE) return;
    if (valid1 & DS_VALID1_LIGHTBAR)
        cyclone2_lights_host_lightbar(effects[DS_EFFECT_LIGHTBAR],
                                      effects[DS_EFFECT_LIGHTBAR + 1],
                                      effects[DS_EFFECT_LIGHTBAR + 2]);
    if (valid1 & DS_VALID1_PLAYER_LIGHTS) {
        /* Five lights, lit symmetrically: 1 player is the middle one, 5 is
         * all of them.  The count is the player number; shown as that many
         * of our four from the top, as a Switch numbers players. */
        u8 players = effects[DS_EFFECT_PLAYER_LIGHTS] & 0x1fU;
        u8 count = 0U;

        for (; players; players &= (u8)(players - 1U)) count++;
        if (count > CYCLONE2_PLAYER_LIGHTS) count = CYCLONE2_PLAYER_LIGHTS;
        cyclone2_lights_host_players((u8)((1U << count) - 1U), 0U);
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

    .local_name = "DualSense Edge Controller",
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
    if (extended_reports) {
        fill_extended(report, input);
        result = edr_hid_input_send(DS_REPORT_EXTENDED, report,
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
        result = edr_hid_input_send(DS_REPORT_SIMPLE, report, DS_SIMPLE_SIZE);
        if (result == 0) {
            memcpy(simple_last, report, DS_SIMPLE_SIZE);
            simple_sent = 1U;
            simple_last_send_ms = now;
        }
    }
    if (result == 3) return -1;
    if (result != 0) return -2;
    report_counter++;
    packet_sequence++;
    return 1;
}
