#include "system/includes.h"
#include "btstack/avctp_user.h"
#include "edr_hid_user.h"

#include "control.h"
#include "cyclone2/switch_hid.h"

#if CONFIG_APP_CYCLONE2 && CYCLONE2_BT_SWITCH

#define SWITCH_REPORT_SUBCOMMAND_REPLY  0x21U
#define SWITCH_REPORT_FULL_STATE        0x30U
#define SWITCH_REPORT_SIMPLE_STATE      0x3fU
#define SWITCH_OUTPUT_SUBCOMMAND        0x01U
#define SWITCH_OUTPUT_RUMBLE            0x10U

#define SWITCH_SUBCOMMAND_DEVICE_INFO   0x02U
#define SWITCH_SUBCOMMAND_REPORT_MODE   0x03U
#define SWITCH_SUBCOMMAND_SPI_READ      0x10U
#define SWITCH_SUBCOMMAND_PLAYER_LIGHTS 0x30U
#define SWITCH_SUBCOMMAND_HOME_LIGHT    0x38U
#define SWITCH_SUBCOMMAND_ENABLE_IMU    0x40U
#define SWITCH_SUBCOMMAND_IMU_CONFIG    0x41U
#define SWITCH_SUBCOMMAND_ENABLE_RUMBLE 0x48U

#define SWITCH_HID_DATA_OUTPUT          0xa2U
#define SWITCH_SET_REPORT_OUTPUT        0x52U
#define SWITCH_FULL_PAYLOAD_SIZE        48U
#define SWITCH_SIMPLE_PAYLOAD_SIZE      11U

/* This is the descriptor used by the Cyclone 2 stock firmware for its
 * Switch-compatible Bluetooth HID endpoint (stock app address 0x01e622db). */
const u8 cyclone2_switch_report_map[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,0x06,0x01,0xff,0x85,0x21,0x09,0x21,0x75,0x08,0x95,
    0x30,0x81,0x02,0x85,0x30,0x09,0x30,0x75,0x08,0x95,0x30,0x81,0x02,0x85,0x31,0x09,
    0x31,0x75,0x08,0x96,0x69,0x01,0x81,0x02,0x85,0x32,0x09,0x32,0x75,0x08,0x96,0x69,
    0x01,0x81,0x02,0x85,0x33,0x09,0x33,0x75,0x08,0x96,0x69,0x01,0x81,0x02,0x85,0x3f,
    0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,
    0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,0x75,0x04,0x95,0x01,0x81,0x42,0x05,0x09,
    0x75,0x04,0x95,0x01,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x33,0x09,0x34,
    0x16,0x00,0x00,0x27,0xff,0xff,0x00,0x00,0x75,0x10,0x95,0x04,0x81,0x02,0x06,0x01,
    0xff,0x85,0x01,0x09,0x01,0x75,0x08,0x95,0x30,0x91,0x02,0x85,0x10,0x09,0x10,0x75,
    0x08,0x95,0x30,0x91,0x02,0x85,0x11,0x09,0x11,0x75,0x08,0x95,0x30,0x91,0x02,0x85,
    0x12,0x09,0x12,0x75,0x08,0x95,0x30,0x91,0x02,0xc0,
};

const u16 cyclone2_switch_report_map_size = sizeof(cyclone2_switch_report_map);

struct switch_controller_state {
    u8 timer;
    u8 battery_connection;
    u8 buttons[3];
    u8 left_stick[3];
    u8 right_stick[3];
    u8 vibration;
} __attribute__((packed));

static struct cyclone2_input_report latest_input;
static u8 input_report_mode = SWITCH_REPORT_SIMPLE_STATE;
static u8 timer_counter;

static u16 axis12(u8 value)
{
    return ((u16)value << 4) | (value >> 4);
}

static void pack_stick(u8 output[3], u8 x, u8 y)
{
    u16 packed_x = axis12(x);
    u16 packed_y = axis12(y);

    output[0] = packed_x;
    output[1] = (packed_x >> 8) | (packed_y << 4);
    output[2] = packed_y >> 4;
}

static void fill_controller_state(struct switch_controller_state *state)
{
    u16 buttons = latest_input.buttons;

    memset(state, 0, sizeof(*state));
    timer_counter += 3U;
    state->timer = timer_counter;
    state->battery_connection = 0x81U;

    if (buttons & CYCLONE2_REPORT_BUTTON_X) state->buttons[0] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) state->buttons[0] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_A) state->buttons[0] |= 0x04U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) state->buttons[0] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) state->buttons[0] |= 0x40U;
    if (latest_input.right_trigger > 0x20U) state->buttons[0] |= 0x80U;

    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) state->buttons[1] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) state->buttons[1] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) state->buttons[1] |= 0x04U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) state->buttons[1] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) state->buttons[1] |= 0x10U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) state->buttons[1] |= 0x20U;

    switch (latest_input.hat) {
    case CYCLONE2_HAT_UP:         state->buttons[2] |= 0x02U; break;
    case CYCLONE2_HAT_UP_RIGHT:   state->buttons[2] |= 0x06U; break;
    case CYCLONE2_HAT_RIGHT:      state->buttons[2] |= 0x04U; break;
    case CYCLONE2_HAT_DOWN_RIGHT: state->buttons[2] |= 0x05U; break;
    case CYCLONE2_HAT_DOWN:       state->buttons[2] |= 0x01U; break;
    case CYCLONE2_HAT_DOWN_LEFT:  state->buttons[2] |= 0x09U; break;
    case CYCLONE2_HAT_LEFT:       state->buttons[2] |= 0x08U; break;
    case CYCLONE2_HAT_UP_LEFT:    state->buttons[2] |= 0x0aU; break;
    default: break;
    }
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) state->buttons[2] |= 0x40U;
    if (latest_input.left_trigger > 0x20U) state->buttons[2] |= 0x80U;

    pack_stick(state->left_stick, latest_input.left_x,
               (u8)(0xffU - latest_input.left_y));
    pack_stick(state->right_stick, latest_input.right_x,
               (u8)(0xffU - latest_input.right_y));
}

static void fill_simple_state(u8 report[SWITCH_SIMPLE_PAYLOAD_SIZE])
{
    u16 buttons = latest_input.buttons;
    u16 axis;

    memset(report, 0, SWITCH_SIMPLE_PAYLOAD_SIZE);
    if (buttons & CYCLONE2_REPORT_BUTTON_A) report[0] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) report[0] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) report[0] |= 0x04U;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) report[0] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) report[0] |= 0x10U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) report[0] |= 0x20U;
    if (latest_input.left_trigger > 0x20U) report[0] |= 0x40U;
    if (latest_input.right_trigger > 0x20U) report[0] |= 0x80U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) report[1] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) report[1] |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) report[1] |= 0x04U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) report[1] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) report[1] |= 0x10U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) report[1] |= 0x20U;
    report[2] = latest_input.hat;

    axis = (u16)latest_input.left_x * 257U;
    report[3] = axis; report[4] = axis >> 8;
    axis = (u16)latest_input.left_y * 257U;
    report[5] = axis; report[6] = axis >> 8;
    axis = (u16)latest_input.right_x * 257U;
    report[7] = axis; report[8] = axis >> 8;
    axis = (u16)latest_input.right_y * 257U;
    report[9] = axis; report[10] = axis >> 8;
}

static void pack_pair(u8 output[3], u16 first, u16 second)
{
    output[0] = first;
    output[1] = (first >> 8) | (second << 4);
    output[2] = second >> 4;
}

static void fill_factory_stick_calibration(u8 output[18])
{
    pack_pair(output + 0, 0x600U, 0x600U);
    pack_pair(output + 3, 0x800U, 0x800U);
    pack_pair(output + 6, 0x600U, 0x600U);
    pack_pair(output + 9, 0x800U, 0x800U);
    pack_pair(output + 12, 0x600U, 0x600U);
    pack_pair(output + 15, 0x600U, 0x600U);
}

static void read_virtual_spi(u32 address, u8 length, u8 *output)
{
    u8 factory_calibration[18];
    u32 offset;
    u32 copy_length;

    /* Unimplemented regions read as erased flash. In particular, this keeps
     * the user-calibration magic at 0x8010/0x801b unset. */
    memset(output, 0xff, length);

    if (address >= 0x603dU && address < 0x604fU) {
        fill_factory_stick_calibration(factory_calibration);
        offset = address - 0x603dU;
        copy_length = 18U - offset;
        if (copy_length > length) {
            copy_length = length;
        }
        memcpy(output, factory_calibration + offset, copy_length);
    }
}

static void reply_subcommand(u8 subcommand, const u8 *request, u16 request_size)
{
    u8 reply[SWITCH_FULL_PAYLOAD_SIZE];
    struct switch_controller_state *state = (void *)reply;
    u8 *ack = reply + sizeof(*state);
    u8 *data = ack + 2;

    memset(reply, 0, sizeof(reply));
    fill_controller_state(state);
    ack[0] = subcommand == SWITCH_SUBCOMMAND_SPI_READ ? 0x90U : 0x80U;
    ack[1] = subcommand;

    switch (subcommand) {
    case SWITCH_SUBCOMMAND_DEVICE_INFO: {
        const u8 *mac = bt_get_mac_addr();
        ack[0] = 0x82U;
        data[0] = 0x03U;
        data[1] = 0x48U;
        data[2] = 0x03U; /* Pro Controller */
        data[3] = 0x02U;
        memcpy(data + 4, mac, 6);
        data[10] = 0x01U;
        data[11] = 0x01U;
        break;
    }
    case SWITCH_SUBCOMMAND_REPORT_MODE:
        if (request_size != 0U &&
            (request[0] == SWITCH_REPORT_SIMPLE_STATE ||
             request[0] == SWITCH_REPORT_FULL_STATE)) {
            input_report_mode = request[0];
        }
        break;
    case SWITCH_SUBCOMMAND_SPI_READ:
        if (request_size >= 5U) {
            u32 address = (u32)request[0] | ((u32)request[1] << 8) |
                          ((u32)request[2] << 16) | ((u32)request[3] << 24);
            u8 length = request[4];
            memcpy(data, request, 5);
            memset(data + 5, 0xff, 29);
            if (length > 29U) {
                length = 29U;
                data[4] = length;
            }
            read_virtual_spi(address, length, data + 5);
        }
        break;
    case SWITCH_SUBCOMMAND_PLAYER_LIGHTS:
        if (request_size != 0U) {
            cyclone2_led_set_all(0U, request[0] & 0x0fU ? 8U : 0U, 0U);
        }
        break;
    case SWITCH_SUBCOMMAND_HOME_LIGHT:
    case SWITCH_SUBCOMMAND_ENABLE_IMU:
    case SWITCH_SUBCOMMAND_IMU_CONFIG:
    case SWITCH_SUBCOMMAND_ENABLE_RUMBLE:
        break;
    default:
        ack[0] = 0x80U;
        break;
    }

    edr_hid_data_send(SWITCH_REPORT_SUBCOMMAND_REPLY, reply, sizeof(reply));
}

static void switch_output(u8 *packet, u16 size, u16 channel)
{
    u8 report_id;
    (void)channel;

    if (size < 2U ||
        (packet[0] != SWITCH_HID_DATA_OUTPUT &&
         packet[0] != SWITCH_SET_REPORT_OUTPUT)) {
        return;
    }
    report_id = packet[1];
    if (report_id == SWITCH_OUTPUT_RUMBLE) {
        return;
    }
    if (report_id == SWITCH_OUTPUT_SUBCOMMAND && size >= 12U) {
        reply_subcommand(packet[11], packet + 12, size - 12U);
    }
}

void cyclone2_switch_hid_initialize(void)
{
    memset(&latest_input, 0, sizeof(latest_input));
    latest_input.hat = CYCLONE2_HAT_NEUTRAL;
    latest_input.left_x = latest_input.left_y = 0x80U;
    latest_input.right_x = latest_input.right_y = 0x80U;
    user_hid_init(switch_output);
}

void cyclone2_switch_hid_send(const struct cyclone2_input_report *input)
{
    u8 report[SWITCH_FULL_PAYLOAD_SIZE];

    latest_input = *input;
    if (!edr_hid_is_connected()) {
        return;
    }
    if (input_report_mode == SWITCH_REPORT_FULL_STATE) {
        memset(report, 0, sizeof(report));
        fill_controller_state((void *)report);
        edr_hid_data_send(SWITCH_REPORT_FULL_STATE, report, sizeof(report));
    } else {
        fill_simple_state(report);
        edr_hid_data_send(SWITCH_REPORT_SIMPLE_STATE, report,
                          SWITCH_SIMPLE_PAYLOAD_SIZE);
    }
}

#else

const u8 cyclone2_switch_report_map[] = { 0 };
const u16 cyclone2_switch_report_map_size = 0;
void cyclone2_switch_hid_initialize(void) {}
void cyclone2_switch_hid_send(const struct cyclone2_input_report *input)
{
    (void)input;
}

#endif
