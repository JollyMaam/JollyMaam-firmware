#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "btstack_jl_hid_ext.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "profile_xbox.h"
#include "config.h"
#include "controller.h"
#include "rumble.h"
#include "usb.h"

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

static const edr_sniff_par_t xbox_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

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

/* Report 0x01's 16 bytes, shared with the Series X|S (BLE) profile.  The
 * Series X|S layout differs only at the end: View is button 11 instead of
 * the AC Back bit, and byte 15 bit 0 is Share (consumer usage Record), which
 * the capture button becomes. */
void cyclone2_xbox_fill_state(u8 state[XBOX_STATE_SIZE],
                              const struct cyclone2_input_report *input,
                              int series)
{
    u16 buttons = input->buttons;

    memset(state, 0, XBOX_STATE_SIZE);
    if (series && (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT)) {
        buttons &= (u16)~CYCLONE2_REPORT_BUTTON_SCREENSHOT;
        state[15] = 0x01U;
    }

    put_u16(state + 0, cyclone2_stick_u16(input->left_x));
    put_u16(state + 2, cyclone2_stick_u16(input->left_y));
    put_u16(state + 4, cyclone2_stick_u16(input->right_x));
    put_u16(state + 6, cyclone2_stick_u16(input->right_y));
    put_u16(state + 8, (u16)(input->left_trigger >> 6));
    put_u16(state + 10, (u16)(input->right_trigger >> 6));
    state[12] = input->hat < CYCLONE2_HAT_NEUTRAL ? (u8)(input->hat + 1U) : 0U;

    state[13] = (u8)(buttons & ~(CYCLONE2_REPORT_BUTTON_A |
                                 CYCLONE2_REPORT_BUTTON_B));
    if (buttons & CYCLONE2_REPORT_BUTTON_A) state[13] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) state[13] |= 0x01U;
    state[14] = (u8)((buttons >> 8) & 0x7fU);
    if (!series && (buttons & CYCLONE2_REPORT_BUTTON_SELECT)) state[15] = 0x01U;
}

static u8 xbox_magnitude(u8 value)
{
    if (value >= 100U) return 255U;
    return (u8)((value * 255U) / 100U);
}

void cyclone2_xbox_rumble(const u8 body[8])
{
    u8 left;
    u8 right;
    u16 duration_ms;

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
        if (config.profile[get_cur_bt_idx() % CONFIG_PROFILES].rumble)
            cyclone2_rumble_set(left, right, duration_ms);
    }
}

static void xbox_output(u8 *packet, u16 size, u16 channel)
{
    (void)channel;
    if (size < 2U || (packet[0] != 0xa2U && packet[0] != 0x52U)) return;
    if (packet[1] != XBOX_REPORT_RUMBLE || size < 2U + 8U) return;
    cyclone2_xbox_rumble(packet + 2);
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
 * link keeps moving), report 0x02 when Guide changes.  The state replaces an
 * unsent one; a Guide change is an event, queued so it is never lost.  Returns as
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
    cyclone2_xbox_fill_state(state, input, 0);
    if (state_sent && !memcmp(state, last_state, sizeof(state)) &&
        now - last_send_ms < XBOX_KEEPALIVE_MS)
        return 0;
    result = edr_hid_input_send(XBOX_REPORT_STATE, state, sizeof(state));
    if (result == 3) return -1;
    if (result != 0) return -2;
    memcpy(last_state, state, sizeof(state));
    state_sent = 1U;
    last_send_ms = now;
    return 1;
}
