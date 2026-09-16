#include "system/includes.h"
#include "btstack/avctp_user.h"
#include "edr_hid_user.h"

#include "control.h"
#include "cyclone2/switch_hid.h"

#if CONFIG_APP_CYCLONE2

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

#define JC_BTN_Y         BIT(0)
#define JC_BTN_X         BIT(1)
#define JC_BTN_A         BIT(2)
#define JC_BTN_B         BIT(3)
#define JC_BTN_SR_R      BIT(4)
#define JC_BTN_SL_R      BIT(5)
#define JC_BTN_R         BIT(6)
#define JC_BTN_ZR        BIT(7)
#define JC_BTN_MINUS     BIT(8)
#define JC_BTN_PLUS      BIT(9)
#define JC_BTN_RSTICK    BIT(10)
#define JC_BTN_LSTICK    BIT(11)
#define JC_BTN_HOME      BIT(12)
#define JC_BTN_CAP       BIT(13) /* capture button */
#define JC_BTN_DOWN      BIT(16)
#define JC_BTN_UP        BIT(17)
#define JC_BTN_RIGHT     BIT(18)
#define JC_BTN_LEFT      BIT(19)
#define JC_BTN_SR_L      BIT(20)
#define JC_BTN_SL_L      BIT(21)
#define JC_BTN_L         BIT(22)
#define JC_BTN_ZL        BIT(23)

/* Report 0x3f uses a different, compact button layout. */
#define NINTENDO_SIMPLE_B               0x01U
#define NINTENDO_SIMPLE_A               0x02U
#define NINTENDO_SIMPLE_Y               0x04U
#define NINTENDO_SIMPLE_X               0x08U
#define NINTENDO_SIMPLE_L               0x10U
#define NINTENDO_SIMPLE_R               0x20U
#define NINTENDO_SIMPLE_ZL              0x40U
#define NINTENDO_SIMPLE_ZR              0x80U

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

#define SWITCH_CONTROLLER_STATE_SIZE       12U
#define SWITCH_STATE_TIMER_OFFSET           0U
#define SWITCH_STATE_BATTERY_OFFSET         1U
#define SWITCH_STATE_BUTTONS_OFFSET         2U
#define SWITCH_STATE_LEFT_STICK_OFFSET      5U
#define SWITCH_STATE_RIGHT_STICK_OFFSET     8U
#define SWITCH_STATE_VIBRATION_OFFSET      11U

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

static void fill_controller_state(u8 state[SWITCH_CONTROLLER_STATE_SIZE])
{
    u16 buttons = latest_input.buttons;
    u32 button_status = 0;

    memset(state, 0, SWITCH_CONTROLLER_STATE_SIZE);
    timer_counter += 3U;
    state[SWITCH_STATE_TIMER_OFFSET] = timer_counter;
    state[SWITCH_STATE_BATTERY_OFFSET] = 0x81U;

    if (buttons & CYCLONE2_REPORT_BUTTON_X) button_status |= JC_BTN_Y;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) button_status |= JC_BTN_X;
    if (buttons & CYCLONE2_REPORT_BUTTON_A) button_status |= JC_BTN_B;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) button_status |= JC_BTN_A;
    /* Live testing exposed a permutation between the non-face physical
     * controls and the semantic fields produced by cyclone2_input_read().
     * Keep the correction here until the underlying board routing is
     * independently re-measured.  The comments name the physical control. */
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) button_status |= JC_BTN_L;       /* L */
    if (latest_input.left_trigger > 0x20U)   button_status |= JC_BTN_R;       /* R */
    if (latest_input.right_trigger > 0x20U)  button_status |= JC_BTN_ZL;      /* L2 */
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) button_status |= JC_BTN_ZR;  /* R2 */
    if (buttons & CYCLONE2_REPORT_BUTTON_START) button_status |= JC_BTN_MINUS;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) button_status |= JC_BTN_PLUS;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) button_status |= JC_BTN_LSTICK;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) button_status |= JC_BTN_RSTICK;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) button_status |= JC_BTN_HOME;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) button_status |= JC_BTN_CAP;

    switch (latest_input.hat) {
    case CYCLONE2_HAT_UP:         button_status |= JC_BTN_UP; break;
    case CYCLONE2_HAT_UP_RIGHT:   button_status |= JC_BTN_UP | JC_BTN_RIGHT; break;
    case CYCLONE2_HAT_RIGHT:      button_status |= JC_BTN_RIGHT; break;
    case CYCLONE2_HAT_DOWN_RIGHT: button_status |= JC_BTN_DOWN | JC_BTN_RIGHT; break;
    case CYCLONE2_HAT_DOWN:       button_status |= JC_BTN_DOWN; break;
    case CYCLONE2_HAT_DOWN_LEFT:  button_status |= JC_BTN_DOWN | JC_BTN_LEFT; break;
    case CYCLONE2_HAT_LEFT:       button_status |= JC_BTN_LEFT; break;
    case CYCLONE2_HAT_UP_LEFT:    button_status |= JC_BTN_UP | JC_BTN_LEFT; break;
    default: break;
    }
    /* This is the inverse of hid_field_extract(button_status, 0, 24). */
    state[SWITCH_STATE_BUTTONS_OFFSET + 0U] = button_status;
    state[SWITCH_STATE_BUTTONS_OFFSET + 1U] = button_status >> 8;
    state[SWITCH_STATE_BUTTONS_OFFSET + 2U] = button_status >> 16;

    pack_stick(state + SWITCH_STATE_LEFT_STICK_OFFSET, latest_input.left_x,
               (u8)(0xffU - latest_input.left_y));
    pack_stick(state + SWITCH_STATE_RIGHT_STICK_OFFSET, latest_input.right_x,
               (u8)(0xffU - latest_input.right_y));
    state[SWITCH_STATE_VIBRATION_OFFSET] = 0;
}

static void fill_simple_state(u8 report[SWITCH_SIMPLE_PAYLOAD_SIZE])
{
    u16 buttons = latest_input.buttons;
    u16 axis;

    memset(report, 0, SWITCH_SIMPLE_PAYLOAD_SIZE);
    if (buttons & CYCLONE2_REPORT_BUTTON_A) report[0] |= NINTENDO_SIMPLE_A;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) report[0] |= NINTENDO_SIMPLE_B;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) report[0] |= NINTENDO_SIMPLE_Y;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) report[0] |= NINTENDO_SIMPLE_X;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) report[0] |= NINTENDO_SIMPLE_L;
    if (latest_input.left_trigger > 0x20U) report[0] |= NINTENDO_SIMPLE_R;
    if (latest_input.right_trigger > 0x20U) report[0] |= NINTENDO_SIMPLE_ZL;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) report[0] |= NINTENDO_SIMPLE_ZR;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) report[1] |= JC_BTN_MINUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) report[1] |= JC_BTN_PLUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) report[1] |= JC_BTN_LSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) report[1] |= JC_BTN_RSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) report[1] |= JC_BTN_HOME >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) report[1] |= JC_BTN_CAP >> 8;
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
    u8 *state = reply;
    u8 *ack = reply + SWITCH_CONTROLLER_STATE_SIZE;
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
        fill_controller_state(report);
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
