#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "btstack_jl_hid_ext.h"
#include "controller.h"
#include "gyro.h"
#include "imu.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "profile_playstation.h"
#include "lights.h"
#include "rumble.h"

/* DualShock 4 (second revision, CUH-ZCT2) over Bluetooth, as Linux
 * hid-playstation and SDL read one.  Report layouts follow hid-playstation;
 * none of it has been compared with a real pad or tried on a PS4. */

#define DS4_REPORT_SIMPLE          0x01U
#define DS4_REPORT_EXTENDED        0x11U
#define DS4_FEATURE_CALIBRATION    0x02U
#define DS4_FEATURE_CALIBRATION_BT 0x05U
#define DS4_FEATURE_FIRMWARE       0xa3U
#define DS4_SIMPLE_SIZE            9U
#define DS4_EXTENDED_SIZE          77U
#define DS4_SIMPLE_KEEPALIVE_MS    100U

/* Output report 0x11: after the report id come a hardware-control byte and
 * an audio byte, then the effects block these offsets index. */
#define DS4_EFFECTS_SIZE           10U
#define DS4_EFFECT_FLAGS           0U
#define DS4_EFFECT_MOTOR_WEAK      3U
#define DS4_EFFECT_MOTOR_STRONG    4U
#define DS4_EFFECT_LIGHTBAR        5U
#define DS4_FLAG_MOTOR             0x01U
#define DS4_FLAG_LIGHTBAR          0x02U

/* The touchpad, driven by M and the gyro (see profile_playstation.h). */
#define DS4_TOUCH_WIDTH            1920U
#define DS4_TOUCH_HEIGHT           942U

/* Battery byte: no cable, full.  The pad's own charge is not reported. */
#define DS4_STATUS_BATTERY_FULL    0x0aU

/* Start with 0x11 reports, as the DualSense profile starts with 0x31 and for
 * the same reason: SDL runs a pad already sending them in full mode.  A real
 * DualShock 4 sends 0x01 until a host reads calibration or sends effects. */
#define DS4_START_EXTENDED 1

#define DS4_HID_FEATURE 3U

static const u8 dualshock4_report_descriptor[] = {
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
    0x06, 0x04, 0xff,
    0x85, 0x02, 0x09, 0x24, 0x95, 0x24, 0xb1, 0x02,
    0x85, 0xa3, 0x09, 0x25, 0x95, 0x30, 0xb1, 0x02,
    0x85, 0x05, 0x09, 0x26, 0x95, 0x28, 0xb1, 0x02,
    0x85, 0x06, 0x09, 0x27, 0x95, 0x34, 0xb1, 0x02,
    0x85, 0x07, 0x09, 0x28, 0x95, 0x30, 0xb1, 0x02,
    0x85, 0x08, 0x09, 0x29, 0x95, 0x2f, 0xb1, 0x02,
    0x85, 0x09, 0x09, 0x2a, 0x95, 0x13, 0xb1, 0x02,
    0x06, 0x03, 0xff,
    0x85, 0x03, 0x09, 0x21, 0x95, 0x26, 0xb1, 0x02,
    0x85, 0x04, 0x09, 0x22, 0x95, 0x2e, 0xb1, 0x02,
    0x85, 0xf0, 0x09, 0x47, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0xf1, 0x09, 0x48, 0x95, 0x3f, 0xb1, 0x02,
    0x85, 0xf2, 0x09, 0x49, 0x95, 0x0f, 0xb1, 0x02,
    0x06, 0x00, 0xff,
    0x85, 0x11, 0x09, 0x20, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x95, 0x4d, 0x81, 0x02, 0x09, 0x21, 0x91, 0x02,
    0x85, 0x12, 0x09, 0x22, 0x95, 0x8d, 0x81, 0x02, 0x09, 0x23, 0x91, 0x02,
    0x85, 0x13, 0x09, 0x24, 0x95, 0xcd, 0x81, 0x02, 0x09, 0x25, 0x91, 0x02,
    0x85, 0x14, 0x09, 0x26, 0x96, 0x0d, 0x01, 0x81, 0x02, 0x09, 0x27, 0x91, 0x02,
    0x85, 0x15, 0x09, 0x28, 0x96, 0x4d, 0x01, 0x81, 0x02, 0x09, 0x29, 0x91, 0x02,
    0x85, 0x16, 0x09, 0x2a, 0x96, 0x8d, 0x01, 0x81, 0x02, 0x09, 0x2b, 0x91, 0x02,
    0x85, 0x17, 0x09, 0x2c, 0x96, 0xcd, 0x01, 0x81, 0x02, 0x09, 0x2d, 0x91, 0x02,
    0x85, 0x18, 0x09, 0x2e, 0x96, 0x0d, 0x02, 0x81, 0x02, 0x09, 0x2f, 0x91, 0x02,
    0x85, 0x19, 0x09, 0x30, 0x96, 0x22, 0x02, 0x81, 0x02, 0x09, 0x31, 0x91, 0x02,
    0xc0,
};

static const edr_sniff_par_t dualshock4_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

static const edr_init_cfg_t dualshock4_edr_config = {
    .class_type = 0x002508U,
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = sizeof(dualshock4_report_descriptor),
    .report_map = dualshock4_report_descriptor,
    .sniff_param = &dualshock4_sniff_param,
};

static volatile u8 extended_reports;
static u8 report_counter;
static u8 simple_last[DS4_SIMPLE_SIZE];
static u8 simple_sent;
static u32 simple_last_send_ms;
static struct playstation_touch touch;

/* Sticks, the button bytes (the last with a six-bit report counter above PS
 * and the touchpad click) and the triggers: the simple report whole, and the
 * start of the extended one. */
static void fill_common(u8 state[DS4_SIMPLE_SIZE],
                        const struct cyclone2_input_report *input)
{
    state[0] = cyclone2_stick_u8(input->left_x);
    state[1] = cyclone2_stick_u8(input->left_y);
    state[2] = cyclone2_stick_u8(input->right_x);
    state[3] = cyclone2_stick_u8(input->right_y);
    playstation_buttons(input, state + 4);
    state[6] = (u8)((state[6] & 0x03U) | (u8)(report_counter << 2));
    state[7] = cyclone2_trigger_u8(input->left_trigger);
    state[8] = cyclone2_trigger_u8(input->right_trigger);
}

/* Two bytes of Bluetooth header (0xc0: HID data follows), the 32-byte state
 * USB also sends, then touch frames and the CRC. */
static void fill_extended(u8 report[DS4_EXTENDED_SIZE],
                          const struct cyclone2_input_report *input)
{
    u8 *state = report + 2;
    u8 *touch_frame = report + 35;
    u32 timestamp_us;

    memset(report, 0, DS4_EXTENDED_SIZE);
    report[0] = 0xc0U;
    fill_common(state, input);
    /* The sensor timestamp counts 16/3 microseconds, in 16 bits. */
    if (playstation_motion(state + 12, &timestamp_us) == 0)
        playstation_put_u16(state + 9,
                            (u16)(((u64)timestamp_us * 3U) >> 4));
    state[29] = DS4_STATUS_BATTERY_FULL;
    report[34] = 1U;
    touch_frame[0] = report_counter;
    playstation_touch_fill(&touch, touch_frame + 1);
    touch_frame[5] = 0x80U;
    playstation_seal(0xa1U, DS4_REPORT_EXTENDED, report, DS4_EXTENDED_SIZE);
}

/* Gyro and accelerometer calibration.  The Bluetooth layout gives the three
 * gyro "plus" points before the three "minus" ones; USB interleaves them.
 * 8000 counts at 500 degrees a second and 8192 a g are what the IMU gives,
 * as for the DualSense. */
static void fill_calibration(u8 *report)
{
    playstation_put_u16(report + 6, 8000U);
    playstation_put_u16(report + 8, 8000U);
    playstation_put_u16(report + 10, 8000U);
    playstation_put_u16(report + 12, (u16)(s16)-8000);
    playstation_put_u16(report + 14, (u16)(s16)-8000);
    playstation_put_u16(report + 16, (u16)(s16)-8000);
    playstation_put_u16(report + 18, 500U);
    playstation_put_u16(report + 20, 500U);
    playstation_put_u16(report + 22, 8192U);
    playstation_put_u16(report + 24, (u16)(s16)-8192);
    playstation_put_u16(report + 26, 8192U);
    playstation_put_u16(report + 28, (u16)(s16)-8192);
    playstation_put_u16(report + 30, 8192U);
    playstation_put_u16(report + 32, (u16)(s16)-8192);
}

/* Host GET_REPORT, on the BTstack run loop.  Reading calibration switches a
 * real DualShock 4 to 0x11 reports; so does any of these here. */
static int dualshock4_get_report(u8 report_type, u8 report_id, u8 *report,
                                 u16 size)
{
    if (report_type != DS4_HID_FEATURE) return 0;
    memset(report, 0, size);
    switch (report_id) {
    case DS4_FEATURE_CALIBRATION:
        if (size < 34U) return 0;
        fill_calibration(report);
        break;
    case DS4_FEATURE_CALIBRATION_BT:
        if (size < 34U + 4U) return 0;
        fill_calibration(report);
        playstation_seal(0xa3U, report_id, report, size);
        break;
    case DS4_FEATURE_FIRMWARE:
        /* Build date and time, then hardware and firmware versions, which
         * hosts only log.  No CRC on this one, over Bluetooth too. */
        if (size < 42U) return 0;
        memcpy(report, "Sep 30 2026", 11U);
        memcpy(report + 16, "00:00:00", 8U);
        playstation_put_u16(report + 34, 0x0100U);
        playstation_put_u16(report + 40, 0x0100U);
        break;
    default:
        return 0;
    }
    extended_reports = 1U;
    return 1;
}

/* Output report 0x11, on the interrupt channel or as SET_REPORT.  Flag bits
 * say which of rumble and lightbar it sets; the rest are left as they were.
 * The low six bits of the hardware-control byte ask for a report interval;
 * profile.#.report_hz decides that here instead. */
static void dualshock4_output(u8 *packet, u16 size, u16 channel)
{
    const u8 *effects;

    (void)channel;
    if (size < 2U || (packet[0] != 0xa2U && packet[0] != 0x52U)) return;
    if (packet[1] != DS4_REPORT_EXTENDED || size < 4U + DS4_EFFECTS_SIZE)
        return;
    extended_reports = 1U;
    effects = packet + 4;
    if (effects[DS4_EFFECT_FLAGS] & DS4_FLAG_MOTOR)
        cyclone2_rumble_set_latched(effects[DS4_EFFECT_MOTOR_STRONG],
                                    effects[DS4_EFFECT_MOTOR_WEAK]);
    if (effects[DS4_EFFECT_FLAGS] & DS4_FLAG_LIGHTBAR)
        cyclone2_lights_host_lightbar(effects[DS4_EFFECT_LIGHTBAR],
                                      effects[DS4_EFFECT_LIGHTBAR + 1],
                                      effects[DS4_EFFECT_LIGHTBAR + 2]);
}

static void dualshock4_session_start(void)
{
    extended_reports = DS4_START_EXTENDED;
    report_counter = 0U;
    simple_sent = 0U;
    playstation_touch_reset(&touch, DS4_TOUCH_WIDTH, DS4_TOUCH_HEIGHT);
    cyclone2_gyro_reset();
    cyclone2_imu_enable();
}

static void dualshock4_link_up(void)
{
    extended_reports = DS4_START_EXTENDED;
    simple_sent = 0U;
}

static void dualshock4_session_exit(void)
{
    cyclone2_rumble_stop();
    cyclone2_imu_disable();
}

static const struct cyclone2_classic_profile dualshock4_classic = {
    .local_name = "Wireless Controller",
    .edr = &dualshock4_edr_config,
    .vendor_id = 0x054cU,
    .product_id = 0x09ccU,
    .version = 0x0100U,
    .output = dualshock4_output,
    .get_report = dualshock4_get_report,
    .session_start = dualshock4_session_start,
    .link_up = dualshock4_link_up,
    .session_exit = dualshock4_session_exit,
};

void cyclone2_dualshock4_start(u8 slot, u8 pairing)
{
    cyclone2_classic_start(&dualshock4_classic, slot, pairing);
}

/* As cyclone2_dualsense_send(): extended reports on every call, simple ones
 * on change plus a keepalive.  Returns as cyclone2_profile_send() documents. */
int cyclone2_dualshock4_send(const struct cyclone2_input_report *report_in)
{
    struct cyclone2_input_report shaped = *report_in;
    u8 report[DS4_EXTENDED_SIZE];
    u32 now;
    int result;

    if (!edr_hid_is_connected()) {
        simple_sent = 0U;
        return 0;
    }
    now = sys_timer_get_ms();
    playstation_touch_update(&touch, &shaped, now);
    if (extended_reports) {
        fill_extended(report, &shaped);
        result = edr_hid_input_send(DS4_REPORT_EXTENDED, report,
                                    DS4_EXTENDED_SIZE);
    } else {
        fill_common(report, &shaped);
        /* Byte 6 carries the report counter in its top six bits; only the
         * two button bits in it count as a change. */
        if (simple_sent &&
            !memcmp(report, simple_last, 6U) &&
            (report[6] & 0x03U) == (simple_last[6] & 0x03U) &&
            !memcmp(report + 7, simple_last + 7, DS4_SIMPLE_SIZE - 7U) &&
            now - simple_last_send_ms < DS4_SIMPLE_KEEPALIVE_MS)
            return 0;
        result = edr_hid_input_send(DS4_REPORT_SIMPLE, report,
                                    DS4_SIMPLE_SIZE);
        if (result == 0) {
            memcpy(simple_last, report, DS4_SIMPLE_SIZE);
            simple_sent = 1U;
            simple_last_send_ms = now;
        }
    }
    if (result == 3) return -1;
    if (result != 0) return -2;
    report_counter++;
    return 1;
}
