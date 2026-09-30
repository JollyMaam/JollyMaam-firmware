#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_task.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "btstack_jl_hid_ext.h"
#include "imu.h"
#include "lights.h"
#include "main.h"
#include "profile_switch_hid.h"
#include "profile_switch_spi.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "config.h"
#include "usb.h"
#include "user_cfg_id.h"

/* Switch rumble carriers are approximated by ERM amplitude; no frequency control. */

#define SWITCH_REPORT_SUBCOMMAND_REPLY  0x21U
#define SWITCH_REPORT_FULL_STATE        0x30U
#define SWITCH_REPORT_SIMPLE_STATE      0x3fU

#define SWITCH_OUTPUT_SUBCOMMAND        0x01U
#define SWITCH_OUTPUT_RUMBLE            0x10U

#define SWITCH_SUBCOMMAND_PAIRING      0x01U
#define SWITCH_SUBCOMMAND_DEVICE_INFO   0x02U
#define SWITCH_SUBCOMMAND_REPORT_MODE   0x03U
#define SWITCH_SUBCOMMAND_TRIGGER_TIME  0x04U
#define SWITCH_SUBCOMMAND_HCI_STATE     0x06U
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
#define SWITCH_SIMPLE_KEEPALIVE_MS      100U

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
#define JC_BTN_CAP       BIT(13)
#define JC_BTN_DOWN      BIT(16)
#define JC_BTN_UP        BIT(17)
#define JC_BTN_RIGHT     BIT(18)
#define JC_BTN_LEFT      BIT(19)
#define JC_BTN_SR_L      BIT(20)
#define JC_BTN_SL_L      BIT(21)
#define JC_BTN_L         BIT(22)
#define JC_BTN_ZL        BIT(23)

#define NINTENDO_SIMPLE_B               0x01U
#define NINTENDO_SIMPLE_A               0x02U
#define NINTENDO_SIMPLE_Y               0x04U
#define NINTENDO_SIMPLE_X               0x08U
#define NINTENDO_SIMPLE_L               0x10U
#define NINTENDO_SIMPLE_R               0x20U
#define NINTENDO_SIMPLE_ZL              0x40U
#define NINTENDO_SIMPLE_ZR              0x80U

/* The HID report descriptor lives in src/profile_switch_hid.bin so it
 * can be decoded with ordinary HID tools, and is compiled into
 * nintendo_switch_pro_hid_report_descriptor[] by tools/switch_pro_hid_gen.c.
 * That descriptor is the one the Cyclone 2 stock firmware publishes (stock app
 * address 0x01e622db) and is byte-for-byte what real Pro Controllers use: the
 * five input reports (0x21, 0x30, 0x31..0x33, 0x3f) and the four output
 * reports (0x01, 0x10, 0x11, 0x12) described in the file header comment.
 *
 * A wrong size means the blob was replaced with something truncated; hosts
 * negotiate against this, so fail the build rather than connect with a
 * different descriptor. */
_Static_assert(sizeof(nintendo_switch_pro_hid_report_descriptor) ==
               170U, "Switch HID report descriptor size changed");

#define SWITCH_CONTROLLER_STATE_SIZE       12U
#define SWITCH_STATE_TIMER_OFFSET           0U
#define SWITCH_STATE_BATTERY_OFFSET         1U
#define SWITCH_STATE_BUTTONS_OFFSET         2U
#define SWITCH_STATE_LEFT_STICK_OFFSET      5U
#define SWITCH_STATE_RIGHT_STICK_OFFSET     8U
#define SWITCH_STATE_VIBRATION_OFFSET      11U

static struct cyclone2_input_report latest_input;
/* Handshake state.  There is deliberately no separate state machine here:
 * the console drives the session by sending subcommands and this file simply
 * records what it asked for.
 *
 * Reports start in full (0x30) mode.  A working Pro Controller emulator for
 * the real console streams 0x30 at 8 ms from the moment the HID channels open
 * and never sends 0x3f itself; a console that opens the HID channels and then
 * sees only sparse 0x3f reports drops the link after about 260 ms without
 * ever sending a subcommand.  Subcommand 0x03 can still move this back to the
 * simple report at any time. */
static u8 input_report_mode = SWITCH_REPORT_FULL_STATE;

static u8 timer_counter;

static u8 rumble_enabled;
static u8 imu_enabled;

static u8 host_stopped;

static u8 tx_debug_remaining;

/* Timestamp of the last simple-mode report, used to keep the link alive
 * when nothing changes (see SWITCH_SIMPLE_KEEPALIVE_MS). */
static u32 simple_last_send_ms;

/* Decode one 4-byte rumble field into a single 0..255 motor strength.
 *
 * Nintendo encodes two sine bands per motor.  The four bytes are, per
 * dekuNukem's rumble data table (the same encoding SDL implements):
 *
 *   data[0]  high-band frequency, low 8 bits
 *   data[1]  high-band amplitude (bits 7..1) plus frequency bit 8 (bit 0)
 *   data[2]  low-band frequency plus amplitude bit 8 (bit 7)
 *   data[3]  low-band amplitude, low 8 bits
 *
 * Neutral is 00 01 40 40.  These ERM motors cannot reproduce the two carrier
 * frequencies, so this takes the high-band amplitude (0x00..0xc8) as the
 * strength and ignores the spectra.  The low-band byte is only used to
 * recognise the neutral value. */
static u8 decode_rumble_strength(const u8 data[4])
{
    u8 amplitude_code = data[1] & 0xfeU;

    if (amplitude_code == 0U && data[3] == 0x40U) return 0U;
    return (u8)(((u16)amplitude_code * 255U) / 0xc8U);
}

static void handle_rumble(const u8 data[8])
{
    if (!rumble_enabled) return;
    cyclone2_rumble_set_latched(decode_rumble_strength(data),
                                decode_rumble_strength(data + 4U));
}

#define SWITCH_STICK_CENTER   0x800U
#define SWITCH_STICK_RANGE    0x600U

#define SWITCH_TRIGGER_BUTTON_LEVEL  (CYCLONE2_TRIGGER_MAX / 8U)

static u16 axis12(s16 value)
{
    return cyclone2_stick_scaled(value, SWITCH_STICK_CENTER,
                                 SWITCH_STICK_RANGE);
}

static void pack_stick(u8 output[3], s16 x, s16 y)
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
    /* 0x8e = battery level 8 (full), not charging, connection status whose
     * USB bit is clear - i.e. a Bluetooth-linked Pro Controller.  Every
     * working emulator (the ESP32 NS1 demo, jl-esp32, Switch2Connect) sends
     * this; sending 0x81 instead announces a *wired* controller, which is
     * what this firmware did and what the console's bond handling appears to
     * object to on a later reconnect.  The Cyclone 2 does not measure its
     * battery, so report a full pack. */
    state[SWITCH_STATE_BATTERY_OFFSET] = 0x8eU;

    if (buttons & CYCLONE2_REPORT_BUTTON_X) button_status |= JC_BTN_Y;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) button_status |= JC_BTN_X;
    if (buttons & CYCLONE2_REPORT_BUTTON_A) button_status |= JC_BTN_B;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) button_status |= JC_BTN_A;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) button_status |= JC_BTN_L;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) button_status |= JC_BTN_R;
    if (latest_input.left_trigger > SWITCH_TRIGGER_BUTTON_LEVEL)
        button_status |= JC_BTN_ZL;
    if (latest_input.right_trigger > SWITCH_TRIGGER_BUTTON_LEVEL)
        button_status |= JC_BTN_ZR;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) button_status |= JC_BTN_MINUS;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) button_status |= JC_BTN_PLUS;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) button_status |= JC_BTN_LSTICK;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) button_status |= JC_BTN_RSTICK;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) button_status |= JC_BTN_HOME;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) button_status |= JC_BTN_CAP;

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

    state[SWITCH_STATE_BUTTONS_OFFSET + 0U] = button_status;
    state[SWITCH_STATE_BUTTONS_OFFSET + 1U] = button_status >> 8;
    state[SWITCH_STATE_BUTTONS_OFFSET + 2U] = button_status >> 16;

    pack_stick(state + SWITCH_STATE_LEFT_STICK_OFFSET, latest_input.left_x,
               (s16)-latest_input.left_y);
    pack_stick(state + SWITCH_STATE_RIGHT_STICK_OFFSET, latest_input.right_x,
               (s16)-latest_input.right_y);
    state[SWITCH_STATE_VIBRATION_OFFSET] = 0;
}

static void write_s16_le(u8 *output, s16 value)
{
    output[0] = (u8)value;
    output[1] = (u8)((u16)value >> 8);
}

static void fill_imu_samples(u8 output[36])
{
    struct cyclone2_imu_sample sample;
    u8 index;

    memset(output, 0, 36U);
    if (!imu_enabled) return;
    if (cyclone2_imu_latest(&sample) != 0) return;
    for (index = 0U; index < 3U; index++) {
        /* The proven acquisition rate is 100 Hz, below Nintendo's three
         * samples per packet. Repeat the latest sample instead of replaying
         * overlapping history out of chronological order. */

        write_s16_le(output + index * 12U + 0U, sample.accel_x / 2);
        write_s16_le(output + index * 12U + 2U, sample.accel_y / 2);
        write_s16_le(output + index * 12U + 4U, sample.accel_z / 2);
        write_s16_le(output + index * 12U + 6U, -sample.gyro_x);
        write_s16_le(output + index * 12U + 8U, -sample.gyro_y);
        write_s16_le(output + index * 12U + 10U, sample.gyro_z);
    }
}

/* Build the 11-byte simple report 0x3f, whose layout is in the file header.
 * It carries the same information as the state block but with two button
 * bytes and 16-bit axes instead of 12-bit packed ones.  The host scales each
 * 8-bit axis value to 16 bits by multiplying by 257 so that 0x00 and 0xff
 * map exactly onto the 16-bit end points. */
static void fill_simple_state(u8 report[SWITCH_SIMPLE_PAYLOAD_SIZE])
{
    u16 buttons = latest_input.buttons;
    u16 axis;

    memset(report, 0, SWITCH_SIMPLE_PAYLOAD_SIZE);
    if (buttons & CYCLONE2_REPORT_BUTTON_A) report[0] |= NINTENDO_SIMPLE_B;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) report[0] |= NINTENDO_SIMPLE_A;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) report[0] |= NINTENDO_SIMPLE_Y;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) report[0] |= NINTENDO_SIMPLE_X;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) report[0] |= NINTENDO_SIMPLE_L;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) report[0] |= NINTENDO_SIMPLE_R;
    if (latest_input.left_trigger > SWITCH_TRIGGER_BUTTON_LEVEL)
        report[0] |= NINTENDO_SIMPLE_ZL;
    if (latest_input.right_trigger > SWITCH_TRIGGER_BUTTON_LEVEL)
        report[0] |= NINTENDO_SIMPLE_ZR;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) report[1] |= JC_BTN_MINUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) report[1] |= JC_BTN_PLUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) report[1] |= JC_BTN_RSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) report[1] |= JC_BTN_LSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) report[1] |= JC_BTN_HOME >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) report[1] |= JC_BTN_CAP >> 8;
    report[2] = latest_input.hat;

    axis = cyclone2_stick_u16(latest_input.left_x);
    report[3] = axis; report[4] = axis >> 8;
    axis = cyclone2_stick_u16(latest_input.left_y);
    report[5] = axis; report[6] = axis >> 8;
    axis = cyclone2_stick_u16(latest_input.right_x);
    report[7] = axis; report[8] = axis >> 8;
    axis = cyclone2_stick_u16(latest_input.right_y);
    report[9] = axis; report[10] = axis >> 8;
}

/*
 * Virtual SPI flash
 * =================
 *
 * Subcommand 0x10 lets the host read the controller's SPI flash.  Nintendo's
 * map, from dekuNukem's spi_flash_notes and cross-checked against the working
 * emulators:
 *
 *   0x6000  device info / serial number                   32 bytes
 *   0x6020  6-axis (accelerometer + gyroscope) calibration 24 bytes
 *   0x603d  stick calibration, left then right             18 bytes
 *   0x6050  body / button / grip colours                   12 bytes
 *   0x6080  6-axis parameters                               6 bytes
 *   0x6086  left stick parameters                          18 bytes
 *   0x6098  right stick parameters                         18 bytes
 *   0x8010  user stick calibration: must read as erased so that the factory
 *           values above win (its magic values are checked by host drivers)
 *
 * The bytes live in src/profile_switch_spi.bin, which is indexed exactly
 * like the controller's flash: the file starts at address 0x6000 and every
 * address in the table below is (offset in that file) + 0x6000.  Keeping the
 * data binary means the flash contents can be hex-dumped and compared against
 * a real controller without reading C.
 *
 * A read may be of any length and may cross region boundaries, so
 * read_virtual_spi() resolves each byte on its own.  A byte with no region
 * reads 0xff, the value of erased flash.
 */

#define SWITCH_SPI_WINDOW_BASE 0x6000U

struct switch_spi_region {
    u32 address;
    u16 length;
    const u8 *data;
};

#define SWITCH_SPI_BYTE(address) \
    (nintendo_switch_pro_spi_flash + ((address) - SWITCH_SPI_WINDOW_BASE))

static const struct switch_spi_region switch_spi_regions[] = {

    {0x6000U, 32U, SWITCH_SPI_BYTE(0x6000U)},

    {0x6020U, 24U, SWITCH_SPI_BYTE(0x6020U)},

    {0x603dU, 18U, SWITCH_SPI_BYTE(0x603dU)},

    {0x6050U, 12U, SWITCH_SPI_BYTE(0x6050U)},

    {0x6080U, 6U, SWITCH_SPI_BYTE(0x6080U)},

    {0x6086U, 18U, SWITCH_SPI_BYTE(0x6086U)},

    {0x6098U, 18U, SWITCH_SPI_BYTE(0x6098U)},
};

_Static_assert(sizeof(nintendo_switch_pro_spi_flash) == 0xaaU,
               "Switch SPI window size changed");

static u8 switch_spi_read_byte(u32 address)
{
    unsigned region;

    for (region = 0U;
         region < sizeof(switch_spi_regions) / sizeof(switch_spi_regions[0]);
         region++) {
        const struct switch_spi_region *entry = &switch_spi_regions[region];

        if (address >= entry->address &&
            address - entry->address < entry->length) {
            return entry->data[address - entry->address];
        }
    }
    return 0xffU;
}

static void read_virtual_spi(u32 address, u8 length, u8 *output)
{
    u8 index;

    for (index = 0U; index < length; index++) {
        output[index] = switch_spi_read_byte(address + index);
    }
}
/* bt_get_mac_addr() stores the local address least-significant byte first,
 * which is the reverse of the "aa:bb:cc:dd:ee:ff" form tools print.  The two
 * Nintendo replies use the two different orders, so each call site goes
 * through a named helper instead of reversing bytes inline. */

/* Little endian: what the pairing reply carries. */
static void fill_address_little_endian(u8 output[6], const u8 address[6])
{
    memcpy(output, address, 6U);
}

/* Big endian: the address as printed, what the device-information reply
 * carries (SDL's switch driver reads these six bytes as a big-endian MAC). */
static void fill_address_big_endian(u8 output[6], const u8 address[6])
{
    u8 index;

    for (index = 0U; index < 6U; index++) {
        output[index] = address[5U - index];
    }
}

/* Answer one subcommand with input report 0x21.
 *
 * The report body is the normal 12-byte state block followed by
 *
 *   reply[12]     ACK byte
 *   reply[13]     the subcommand id, echoed back
 *   reply[14..]   payload, interpreted per subcommand
 *
 * The ACK byte stays 0x80 for commands that only need acknowledging, and
 * gains a low bit (0x81, 0x82, 0x83, 0x90) when the reply carries data.  The
 * host treats a missing or malformed reply as a protocol failure and will
 * eventually drop the controller, so every subcommand that arrives must be
 * answered. */
static void reply_subcommand(u8 subcommand, const u8 *request, u16 request_size)
{
    /* 24-byte controller identification string from the stock firmware:
     * 0x002508 is the Bluetooth class of device (peripheral, gamepad) and
     * the tail is a constant a real controller sends. */
    static const u8 controller_string[24] = {
        0x00, 0x25, 0x08, 'P', 'r', 'o', ' ', 'C',
        'o', 'n', 't', 'r', 'o', 'l', 'l', 'e',
        'r', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68,
    };
    u8 reply[SWITCH_FULL_PAYLOAD_SIZE];
    u8 *state = reply;
    u8 *ack = reply + SWITCH_CONTROLLER_STATE_SIZE;
    u8 *data = ack + 2;

    cyclone2_bringup_record(0x8000u | subcommand);
    memset(reply, 0, sizeof(reply));
    fill_controller_state(state);
    ack[0] = subcommand == SWITCH_SUBCOMMAND_SPI_READ ? 0x90U : 0x80U;
    ack[1] = subcommand;

    switch (subcommand) {
    case SWITCH_SUBCOMMAND_PAIRING:
        /* Payload: the phase the host asked about, and for phase 1 our
         * address (little endian) followed by the controller string. */
        if (request_size != 0U) {
            const u8 *mac = bt_get_mac_addr();

            ack[0] = 0x81U;
            data[0] = request[0];
            if (request[0] == 1U) {
                /* dekuNukem's bluetooth_hid_subcommands_notes.md: the manual
                 * pairing reply carries the controller's MAC little-endian,
                 * i.e. as bt_get_mac_addr() stores it. */
                fill_address_little_endian(data + 1U, mac);
                memcpy(data + 7U, controller_string,
                       sizeof(controller_string));
            } else if (request[0] == 2U) {
                memset(data + 1U, 0, 16U);
            }
        }
        break;
    case SWITCH_SUBCOMMAND_DEVICE_INFO: {
        /* Payload: firmware version 03.48, controller type 03.02 (Pro
         * Controller), our address big endian, then two status bytes. */
        const u8 *mac = bt_get_mac_addr();
        ack[0] = 0x82U;
        data[0] = 0x03U;
        data[1] = 0x48U;
        data[2] = 0x03U;
        data[3] = 0x02U;
        /* Bytes 4..9 are the MAC in big-endian order (dekuNukem's
         * bluetooth_hid_subcommands_notes.md). */
        fill_address_big_endian(data + 4U, mac);

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
    case SWITCH_SUBCOMMAND_TRIGGER_TIME: {

        u8 index;

        ack[0] = 0x83U;
        for (index = 0U; index < 14U; index += 2U)
            write_s16_le(data + index, 100);
        break;
    }
    case SWITCH_SUBCOMMAND_HCI_STATE:

        if (request_size != 0U && request[0] == 0U)
            host_stopped = 1U;
        break;
    case SWITCH_SUBCOMMAND_SPI_READ:
        /* Argument: 32-bit little-endian address followed by a length byte.
         * Reply: the same five bytes echoed back, then the data.  Host
         * drivers compare the echo, so a shortened read must fix up the
         * echoed length as well. */
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

        /* Low nibble: lights on; high nibble: lights flashing. */
        if (request_size != 0U)
            cyclone2_lights_host_players(request[0] & 0x0fU, request[0] >> 4);
        break;
    case SWITCH_SUBCOMMAND_HOME_LIGHT:
        /* HOME_LIGHT is a separate Switch command.  Do not let it change
         * the IMU state: the old fall-through made an ordinary home-LED
         * update with value zero stop motion sampling. */
        break;
    case SWITCH_SUBCOMMAND_ENABLE_IMU:

        imu_enabled = request_size != 0U && request[0] != 0U;
        if (imu_enabled) cyclone2_imu_enable();
        else cyclone2_imu_disable();
        break;
    case SWITCH_SUBCOMMAND_IMU_CONFIG:

        break;
    case SWITCH_SUBCOMMAND_ENABLE_RUMBLE:
        /* Arguments may carry a vibration code; only the on/off state is
         * tracked here because the ERM motors cannot reproduce Nintendo's
         * two-frequency HD rumble waveforms. */
        rumble_enabled = request_size != 0U && request[0] != 0U;
        if (!rumble_enabled) cyclone2_rumble_stop();
        break;
    default:

        ack[0] = 0x80U;
        break;
    }

    edr_hid_data_send(SWITCH_REPORT_SUBCOMMAND_REPLY, reply, sizeof(reply));
    cyclone2_bringup_record(0x8100u | subcommand);
}

static void switch_output(u8 *packet, u16 size, u16 channel)
{
    u8 report_id;

    cyclone2_usb_debug(
        "Switch RX channel=%u size=%u bytes=%02x %02x %02x %02x",
        channel, size,
        size > 0U ? packet[0] : 0U,
        size > 1U ? packet[1] : 0U,
        size > 2U ? packet[2] : 0U,
        size > 3U ? packet[3] : 0U);

    if (size < 2U ||
        (packet[0] != SWITCH_HID_DATA_OUTPUT &&
         packet[0] != SWITCH_SET_REPORT_OUTPUT)) {
        return;
    }
    report_id = packet[1];
    if (report_id == SWITCH_OUTPUT_RUMBLE && size >= 11U) {

        handle_rumble(packet + 3U);
        return;
    }
    if (report_id == SWITCH_OUTPUT_SUBCOMMAND && size >= 12U) {

        cyclone2_usb_debug("Switch subcommand=%02x arg0=%02x size=%u",
                           packet[11], size > 12U ? packet[12] : 0U, size);
        handle_rumble(packet + 3U);
        reply_subcommand(packet[11], packet + 12, size - 12U);
    }
}

static void switch_hid_initialize(void)
{
    memset(&latest_input, 0, sizeof(latest_input));
    input_report_mode = SWITCH_REPORT_FULL_STATE;
    latest_input.hat = CYCLONE2_HAT_NEUTRAL;
    rumble_enabled = 0U;
    imu_enabled = 0U;
    host_stopped = 0U;
    tx_debug_remaining = 4U;
    simple_last_send_ms = sys_timer_get_ms() - SWITCH_SIMPLE_KEEPALIVE_MS;
    cyclone2_imu_disable();
}

static void switch_link_up(void)
{
    tx_debug_remaining = 4U;
}

/* Queue one input report for the host.
 *
 * Both supported modes are polled from the controller's report scheduler;
 * this function only decides which report to build.  In full mode every call
 * produces a 0x30 report (the console expects a steady stream).  In simple
 * mode a report is only produced when the state changed, with a slow
 * keepalive so an idle link is not considered dead.
 *
 * Returns 1 when the transport accepted the report, 0 when it was skipped or
 * the host is not listening, -1 when the SDK transmit queue was full, and -2
 * for any other failure. */
int cyclone2_switch_send(const struct cyclone2_input_report *input)
{
    u8 report[SWITCH_FULL_PAYLOAD_SIZE];
    u8 input_changed = memcmp(&latest_input, input, sizeof(*input)) != 0;
    u32 now;
    int result;

    latest_input = *input;
    if (!edr_hid_is_connected() || host_stopped) {
        return 0;
    }
    if (input_report_mode == SWITCH_REPORT_FULL_STATE) {
        memset(report, 0, sizeof(report));
        fill_controller_state(report);
        fill_imu_samples(report + SWITCH_CONTROLLER_STATE_SIZE);
        result = edr_hid_input_send(SWITCH_REPORT_FULL_STATE, report,
                                    sizeof(report));
    } else {
        now = sys_timer_get_ms();
        if (!input_changed &&
            now - simple_last_send_ms < SWITCH_SIMPLE_KEEPALIVE_MS) {
            return 0;
        }
        fill_simple_state(report);
        result = edr_hid_input_send(SWITCH_REPORT_SIMPLE_STATE, report,
                                    SWITCH_SIMPLE_PAYLOAD_SIZE);
        if (result == 0) simple_last_send_ms = now;
    }
    if (tx_debug_remaining != 0U) {
        cyclone2_usb_debug("Switch TX report=%02x size=%u result=%d",
                           input_report_mode,
                           input_report_mode == SWITCH_REPORT_FULL_STATE ?
                               SWITCH_FULL_PAYLOAD_SIZE :
                               SWITCH_SIMPLE_PAYLOAD_SIZE,
                           result);
        tx_debug_remaining--;
    }
    if (result == 0) return 1;
    if (result == 3) return -1;
    return -2;
}

static const edr_sniff_par_t switch_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

/* Bluetooth identity of the Switch endpoint.
 *
 * class_type 0x002508 is the standard "peripheral, gamepad" class of device,
 * which is what a console filters on while it looks for controllers.
 * io_capabilities 3 (no input, no output) with authentication_req 4 (general
 * bonding, no man-in-the-middle protection) gives the "just works" pairing a
 * console expects.  Page and supervision timeouts match a real controller. */
static const edr_init_cfg_t switch_edr_config = {
    .class_type = 0x002508U,
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = sizeof(nintendo_switch_pro_hid_report_descriptor),
    .report_map = nintendo_switch_pro_hid_report_descriptor,
    .sniff_param = &switch_sniff_param,
};

static const struct cyclone2_classic_profile switch_classic = {
    .local_name = "Pro Controller",
    .edr = &switch_edr_config,
    .vendor_id = 0x057eU,
    .product_id = 0x2009U,
    .version = 0x0001U,
    .output = switch_output,
    .session_start = switch_hid_initialize,
    .link_up = switch_link_up,
    .session_exit = cyclone2_imu_disable,
};

void cyclone2_switch_start(u8 slot, u8 pairing)
{
    cyclone2_classic_start(&switch_classic, slot, pairing);
}
