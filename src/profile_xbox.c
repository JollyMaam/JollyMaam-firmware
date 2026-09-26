#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "usb.h"

/*
 * Xbox Wireless Controller over Bluetooth Classic HID (045e:02fd, the Xbox
 * One S controller with firmware 4.8).
 * =========================================================================
 *
 * Windows binds its xinputhid driver to this id, so games that only speak
 * XInput see an Xbox controller without a dongle; Linux, macOS, Android and
 * SDL all know it too.  The stock Cyclone 2 firmware's "bt xinput mode" is
 * this same identity (its image carries the descriptor below and the Device ID
 * 045e:02fd version 0x0100).  XInput has no paddles or gyro; those are
 * reported as extra HID buttons where hosts that read raw HID can see them.
 *
 * Input report 0x01, 16 bytes (the report descriptor below, which is the one
 * the stock firmware sends; offsets as SDL's SDL_hidapi_xboxone.c reads them,
 * minus the report id):
 *
 *   [0..1]   left stick X, 0..65535, centre 0x8000
 *   [2..3]   left stick Y, down is positive
 *   [4..5]   right stick X
 *   [6..7]   right stick Y
 *   [8..9]   left trigger, 10 bits
 *   [10..11] right trigger, 10 bits
 *   [12]     hat: 1 = up, clockwise to 8 = up-left, 0 = centred
 *   [13..14] buttons 1..15: A, B, -, X, Y, -, LB, RB, -, -, View, Menu,
 *            Guide, LS, RS.  Linux maps these usages to BTN_SOUTH, EAST, C,
 *            NORTH, WEST, Z, TL, TR, TL2, TR2, SELECT, START, MODE, THUMBL,
 *            THUMBR - the order of enum cyclone2_button, so the pad's button
 *            word goes in as it is and the extra controls (capture, M, L4,
 *            R4) land on the unused buttons 3, 6, 9 and 10.
 *   [15]     bit 0: View again, as consumer usage AC Back - where firmware
 *            4.8 puts it and where xinputhid looks for it
 *
 * Input report 0x02, 1 byte: bit 0 Guide (consumer AC Home).  Firmware 4.8
 * sends Guide only here on Windows; SDL takes it from here once it has seen
 * one, and from button 13 before that.
 *
 * Output report 0x03, 8 bytes (rumble):
 *   [0]      enable: 0x01 right (weak) motor, 0x02 left (strong), 0x04 right
 *            trigger, 0x08 left trigger
 *   [1..4]   magnitude 0..100: left trigger, right trigger, left, right
 *   [5]      duration, 10 ms units
 *   [6]      start delay, 10 ms units
 *   [7]      loop count (SDL sends 0xeb with duration 0xff: "keep going")
 */

#define XBOX_REPORT_STATE    0x01U
#define XBOX_REPORT_GUIDE    0x02U
#define XBOX_REPORT_RUMBLE   0x03U
#define XBOX_STATE_SIZE      16U
#define XBOX_KEEPALIVE_MS    100U

static const u8 xbox_report_descriptor[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,0x85,0x01,0x09,0x01,0xa1,0x00,0x09,0x30,0x09,0x31,
    0x15,0x00,0x27,0xff,0xff,0x00,0x00,0x95,0x02,0x75,0x10,0x81,0x02,0xc0,0x09,0x01,
    0xa1,0x00,0x09,0x32,0x09,0x35,0x15,0x00,0x27,0xff,0xff,0x00,0x00,0x95,0x02,0x75,
    0x10,0x81,0x02,0xc0,0x05,0x02,0x09,0xc5,0x15,0x00,0x26,0xff,0x03,0x95,0x01,0x75,
    0x0a,0x81,0x02,0x15,0x00,0x25,0x00,0x75,0x06,0x95,0x01,0x81,0x03,0x05,0x02,0x09,
    0xc4,0x15,0x00,0x26,0xff,0x03,0x95,0x01,0x75,0x0a,0x81,0x02,0x15,0x00,0x25,0x00,
    0x75,0x06,0x95,0x01,0x81,0x03,0x05,0x01,0x09,0x39,0x15,0x01,0x25,0x08,0x35,0x00,
    0x46,0x3b,0x01,0x66,0x14,0x00,0x75,0x04,0x95,0x01,0x81,0x42,0x75,0x04,0x95,0x01,
    0x15,0x00,0x25,0x00,0x35,0x00,0x45,0x00,0x65,0x00,0x81,0x03,0x05,0x09,0x19,0x01,
    0x29,0x0f,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0f,0x81,0x02,0x15,0x00,0x25,0x00,
    0x75,0x01,0x95,0x01,0x81,0x03,0x05,0x0c,0x0a,0x24,0x02,0x15,0x00,0x25,0x01,0x95,
    0x01,0x75,0x01,0x81,0x02,0x15,0x00,0x25,0x00,0x75,0x07,0x95,0x01,0x81,0x03,0x05,
    0x0c,0x09,0x01,0x85,0x02,0xa1,0x01,0x05,0x0c,0x0a,0x23,0x02,0x15,0x00,0x25,0x01,
    0x95,0x01,0x75,0x01,0x81,0x02,0x15,0x00,0x25,0x00,0x75,0x07,0x95,0x01,0x81,0x03,
    0xc0,0x05,0x0f,0x09,0x21,0x85,0x03,0xa1,0x02,0x09,0x97,0x15,0x00,0x25,0x01,0x75,
    0x04,0x95,0x01,0x91,0x02,0x15,0x00,0x25,0x00,0x75,0x04,0x95,0x01,0x91,0x03,0x09,
    0x70,0x15,0x00,0x25,0x64,0x75,0x08,0x95,0x04,0x91,0x02,0x09,0x50,0x66,0x01,0x10,
    0x55,0x0e,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x01,0x91,0x02,0x09,0xa7,0x15,
    0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x01,0x91,0x02,0x65,0x00,0x55,0x00,0x09,0x7c,
    0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x01,0x91,0x02,0xc0,0x05,0x06,0x09,0x20,
    0x85,0x04,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x01,0x81,0x02,0xc0,
};

/* 15 ms sniff interval, as for the Switch profile: the host asks for sniff
 * once the HID channels are up, and a gamepad wants a short one. */
static const edr_sniff_par_t xbox_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

/* Peripheral / gamepad class of device, "just works" SSP. */
static const edr_init_cfg_t xbox_edr_config = {
    .class_type = 0x002508U,
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = sizeof(xbox_report_descriptor),
    .report_map = xbox_report_descriptor,
    .sniff_param = &xbox_sniff_param,
};

static u8 last_state[XBOX_STATE_SIZE];
static u8 last_guide;
static u8 state_sent;
static u32 last_send_ms;

static void put_u16(u8 *output, u16 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
}

static void fill_state(u8 state[XBOX_STATE_SIZE],
                       const struct cyclone2_input_report *input)
{
    u16 buttons = input->buttons;

    memset(state, 0, XBOX_STATE_SIZE);
    /* x * 257 maps 0x00..0xff onto 0x0000..0xffff with 0x80 near 0x8000. */
    put_u16(state + 0, (u16)(input->left_x * 257U));
    put_u16(state + 2, (u16)(input->left_y * 257U));
    put_u16(state + 4, (u16)(input->right_x * 257U));
    put_u16(state + 6, (u16)(input->right_y * 257U));
    put_u16(state + 8, (u16)((input->left_trigger * 1023U) / 255U));
    put_u16(state + 10, (u16)((input->right_trigger * 1023U) / 255U));
    state[12] = input->hat < CYCLONE2_HAT_NEUTRAL ? (u8)(input->hat + 1U) : 0U;
    state[13] = (u8)buttons;
    state[14] = (u8)((buttons >> 8) & 0x7fU);
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) state[15] = 0x01U;
}

/* Magnitude 0..100 to the motor's 0..255. */
static u8 xbox_magnitude(u8 value)
{
    if (value >= 100U) return 255U;
    return (u8)((value * 255U) / 100U);
}

static void xbox_output(u8 *packet, u16 size, u16 channel)
{
    const u8 *body;
    u8 left;
    u8 right;
    u16 duration_ms;

    (void)channel;
    if (size < 2U || (packet[0] != 0xa2U && packet[0] != 0x52U)) return;
    if (packet[1] != XBOX_REPORT_RUMBLE || size < 2U + 8U) return;
    body = packet + 2;
    left = (body[0] & 0x02U) ? xbox_magnitude(body[3]) : 0U;
    right = (body[0] & 0x01U) ? xbox_magnitude(body[4]) : 0U;
    if (!left && !right) {
        cyclone2_rumble_stop();
        return;
    }
    /* A looping effect runs until the host changes it; SDL always loops and
     * refreshes.  A one-shot runs for its duration. */
    duration_ms = (u16)body[5] * 10U;
    if (body[7] != 0U || duration_ms == 0U) {
        cyclone2_rumble_set_latched(left, right);
    } else {
        if (duration_ms > CYCLONE2_RUMBLE_MAX_DURATION_MS)
            duration_ms = CYCLONE2_RUMBLE_MAX_DURATION_MS;
        cyclone2_rumble_set(left, right, duration_ms);
    }
}

static void xbox_session_start(void)
{
    memset(last_state, 0, sizeof(last_state));
    last_guide = 0U;
    state_sent = 0U;
}

static const struct cyclone2_classic_profile xbox_classic = {
    .local_name = "Xbox Wireless Controller",
    .edr = &xbox_edr_config,
    .vendor_id = 0x045eU,
    .product_id = 0x02fdU,
    .version = 0x0100U,
    .output = xbox_output,
    .session_start = xbox_session_start,
    .link_up = xbox_session_start,
    .session_exit = cyclone2_rumble_stop,
};

void cyclone2_xbox_start(u8 slot, u8 pairing)
{
    cyclone2_classic_start(&xbox_classic, slot, pairing);
}

/* Report 0x01 when the state changes (and every XBOX_KEEPALIVE_MS so an idle
 * link keeps moving), report 0x02 when Guide changes.  Returns as
 * cyclone2_profile_send() documents: 1 sent, 0 skipped, -1 queue full,
 * -2 other failure. */
int cyclone2_xbox_send(const struct cyclone2_input_report *input)
{
    u8 state[XBOX_STATE_SIZE];
    u8 guide = (input->buttons & CYCLONE2_REPORT_BUTTON_HOME) ? 1U : 0U;
    u32 now = sys_timer_get_ms();
    int result;

    if (!edr_hid_is_connected()) {
        state_sent = 0U;
        return 0;
    }
    if (guide != last_guide) {
        result = edr_hid_data_send(XBOX_REPORT_GUIDE, &guide, 1U);
        if (result == 3) return -1;
        if (result != 0) return -2;
        last_guide = guide;
    }
    fill_state(state, input);
    if (state_sent && !memcmp(state, last_state, sizeof(state)) &&
        now - last_send_ms < XBOX_KEEPALIVE_MS)
        return 0;
    result = edr_hid_data_send(XBOX_REPORT_STATE, state, sizeof(state));
    if (result == 3) return -1; /* HID transmit queue full */
    if (result != 0) return -2;
    memcpy(last_state, state, sizeof(state));
    state_sent = 1U;
    last_send_ms = now;
    return 1;
}
