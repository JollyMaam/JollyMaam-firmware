#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_task.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "imu.h"
#include "led.h"
#include "nintendo_switch_pro_hid.h"
#include "nintendo_switch_pro_spi.h"
#include "profile.h"
#include "rumble.h"
#include "usb.h"
#include "user_cfg_id.h"

/*
 * Nintendo Switch Pro Controller emulation over Bluetooth Classic HID.
 * =====================================================================
 *
 * This file implements the device half of Nintendo's HID protocol.  Nothing
 * here is Nintendo-published documentation; the layout below is the one the
 * stock Cyclone 2 firmware speaks and the one Linux's hid-nintendo, SDL's
 * hidapi switch driver and the working emulators in this workspace all
 * expect.  Keep the byte offsets in this comment in sync with the code.
 *
 * Where the protocol comes from
 * -----------------------------
 * - USB HID 1.11 section 6.2.2 defines the report descriptor item encoding
 *   used by data/nintendo_switch_pro_hid.bin (6.2.2.5 Input/Output/Feature items,
 *   6.2.2.6 Collection/End Collection, 6.2.2.7 Global items such as Usage
 *   Page/Report ID/Report Size/Report Count, 6.2.2.8 Local items such as
 *   Usage).
 * - The Bluetooth HID Profile 1.1.1 defines the transport: section 3.1.1
 *   the one-octet HIDP message header (HIDP-Hdr), 3.1.2 the message types,
 *   3.2.1/3.2.2 control and interrupt channel transfers, 5.3.4 the SDP
 *   attributes (parser version 0x0111 lives there).
 * - Everything Nintendo-specific (report IDs, subcommand numbers, reply
 *   contents, virtual SPI flash) is community knowledge, cross-checked
 *   against the emulators under /work (github.com-601-NS1-Wakeup-Demo,
 *   HIDMaestro) and against HCI traffic captured from the stock firmware.
 *
 * Transport framing
 * -----------------
 * A Bluetooth HID host opens two L2CAP channels: HID control (PSM 0x0011)
 * and HID interrupt (PSM 0x0013).  Every packet on either channel begins
 * with a HIDP header byte:
 *
 *   0xa1  DATA | INPUT        controller -> host input report
 *   0xa2  DATA | OUTPUT       host -> controller interrupt output report
 *   0x52  SET_REPORT | OUTPUT host -> controller control-channel report
 *
 * followed by the report ID that the report descriptor declares.
 * edr_hid_data_send() prepends 0xa1 and the report ID, so the byte arrays
 * built in this file contain only the report body.  Received output reports
 * still carry both bytes; switch_output() documents the exact offsets.
 *
 * Input reports (controller -> host)
 * ----------------------------------
 *   0x21  subcommand reply: 12-byte state + ACK + subcommand id + payload
 *   0x30  full state:      12-byte state + 3 x 12-byte IMU samples (48 B)
 *   0x31  extended (MCU/NFC/IR) report; declared but never sent here
 *   0x32  as above
 *   0x33  as above
 *   0x3f  simple state:    11 bytes, buttons + hat + four 16-bit sticks
 *
 * The 12-byte state block shared by 0x21 and 0x30 is:
 *
 *   [0]     report timer, incremented by a few counts per report; the host
 *           uses it to notice dropped reports
 *   [1]     battery and connection: high nibble battery (8 = full), low
 *           nibble connection type (1 = wired), so 0x81 means "full, wired"
 *   [2..4]  24-bit button status, little endian
 *   [5..7]  left stick, three 12-bit values packed as shown in pack_stick()
 *   [8..10] right stick
 *   [11]    vibration status; real controllers rotate 0xa0/0xb0/0x90 and
 *           hosts such as SDL watch this byte for link health
 *
 * The 11-byte simple report 0x3f is:
 *
 *   [0]      B, A, Y, X, L, R, ZL, ZR (bit 0 first)
 *   [1]      minus, plus, right stick, left stick, home, capture, then two
 *            unused bits
 *   [2]      hat switch: 0 = up, clockwise to 7 = up-left, 8 = centred
 *   [3..4]   left stick X, 16-bit little endian
 *   [5..6]   left stick Y
 *   [7..8]   right stick X
 *   [9..10]  right stick Y
 *
 * Face buttons follow Nintendo's labels, not the physical silkscreen: on a
 * Switch the east face button is "A" and the south button is "B", the
 * opposite of the Xbox-style labels printed on the pad.  The Cyclone 2's
 * physical A/B are wired so that this translation is required; see
 * fill_controller_state() and fill_simple_state().
 *
 * Output reports (host -> controller)
 * -----------------------------------
 *   0x01  rumble plus subcommand (12+ bytes)
 *   0x10  rumble only, 9 bytes
 *   0x11  rumble only, 48 bytes
 *   0x12  rumble only, 48 bytes
 *
 * The body of report 0x01 starts with a packet counter, then the rumble
 * data, then the subcommand.  Inside the packet handed to switch_output(),
 * which still has the 0xa2/0x52 header and the report ID:
 *
 *   packet[0]      0xa2 or 0x52
 *   packet[1]      0x01 (report id)
 *   packet[2]      packet counter
 *   packet[3..10]  rumble: two 4-byte fields (high/low band amplitude and
 *                  frequency), one per motor
 *   packet[11]     subcommand id
 *   packet[12..]   subcommand arguments
 *
 * Subcommand replies
 * ------------------
 * Every subcommand is answered with input report 0x21 whose payload is the
 * usual 12-byte state followed by:
 *
 *   [12]  ACK: 0x80 | (meaningful low bits).  The low bits identify replies
 *         that carry data: 0x81 pairing information, 0x82 device
 *         information, 0x83 trigger elapsed times, 0x90 SPI read,
 *         0xa0 NFC/IR MCU configuration.  A plain 0x80 acknowledges a
 *         command with no payload.
 *   [13]  the subcommand id being acknowledged, echoed
 *   [14..] reply payload, see reply_subcommand()
 *
 * Virtual SPI flash
 * -----------------
 * Subcommand 0x10 reads the controller's SPI flash, which holds factory
 * calibration and cosmetic data.  Reads may ask for any address and length
 * and may cross region boundaries, so read_virtual_spi() resolves every byte
 * independently.  Addresses with no entry read as 0xff, the value of erased
 * flash; that is important for the "user calibration" magic values at
 * 0x8010/0x801b, which must stay unset so hosts use the factory data.
 * Region contents and their meaning are in the table further down.
 *
 * Local Bluetooth address byte order
 * ----------------------------------
 * bt_get_mac_addr() returns the address the way the stack stores it:
 * least-significant byte first, i.e. the reverse of the "aa:bb:cc:dd:ee:ff"
 * form that bluetoothctl prints.  Nintendo uses both orders in different
 * replies, so the two call sites below go through fill_address_big_endian()
 * and fill_address_little_endian() instead of copying bytes around by hand.
 */

/* Input report IDs. */
#define SWITCH_REPORT_SUBCOMMAND_REPLY  0x21U
#define SWITCH_REPORT_FULL_STATE        0x30U
#define SWITCH_REPORT_SIMPLE_STATE      0x3fU

/* Output report IDs. */
#define SWITCH_OUTPUT_SUBCOMMAND        0x01U
#define SWITCH_OUTPUT_RUMBLE            0x10U

/* Subcommand ids, answered by reply_subcommand(). */
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

/* Button bits for the 24-bit button field of reports 0x21 and 0x30.  The
 * names on the left are Nintendo's: on a Switch pad the east face button is
 * "A" and the south button is "B".  Bits 14, 15 and 24..31 do not exist. */
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

/* Report 0x3f uses a different, compact button layout: one byte for the
 * four face buttons plus L/R/ZL/ZR, and a second byte for everything else
 * (see fill_simple_state()). */
#define NINTENDO_SIMPLE_B               0x01U
#define NINTENDO_SIMPLE_A               0x02U
#define NINTENDO_SIMPLE_Y               0x04U
#define NINTENDO_SIMPLE_X               0x08U
#define NINTENDO_SIMPLE_L               0x10U
#define NINTENDO_SIMPLE_R               0x20U
#define NINTENDO_SIMPLE_ZL              0x40U
#define NINTENDO_SIMPLE_ZR              0x80U

/* The HID report descriptor lives in data/nintendo_switch_pro_hid.bin so it
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

/* Latest input report accepted by the controller, in the host-independent
 * form defined by input.h.  Reports sent from here translate it into the
 * Nintendo wire format. */
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

/* Free-running report counter, byte 0 of every state block. */
static u8 timer_counter;

/* Subcommand 0x48 / 0x40 state: the console enables these before it expects
 * rumble or motion data to appear in report 0x30. */
static u8 rumble_enabled;
static u8 imu_enabled;

/* Subcommand 0x06 with argument 0 tells us to stop streaming while the
 * console goes to sleep.  Only a profile restart clears it. */
static u8 host_stopped;

/* Number of transmit-result lines still to log after a connection opens. */
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

/* 12-bit stick values: the report packs each axis into 12 bits by repeating
 * the top nibble in the bottom nibble, so 0x80 becomes 0x808 and the host
 * recovers an 8-bit value by taking the high byte. */
static u16 axis12(u8 value)
{
    return ((u16)value << 4) | (value >> 4);
}

/* Pack two 12-bit values into the three bytes Nintendo uses per stick:
 *   output[0] = first[11:4]
 *   output[1] = first[3:0] (high nibble) | second[11:8] (low nibble)
 *   output[2] = second[7:0] */
static void pack_stick(u8 output[3], u8 x, u8 y)
{
    u16 packed_x = axis12(x);
    u16 packed_y = axis12(y);

    output[0] = packed_x;
    output[1] = (packed_x >> 8) | (packed_y << 4);
    output[2] = packed_y >> 4;
}

/* Build the 12-byte state block shared by reports 0x21 and 0x30.  Offsets
 * are in the file header comment; SWITCH_STATE_* names them.
 *
 * The face-button swap below is not a bug: Nintendo's "A" is the east button
 * and "B" is the south button, while the Cyclone 2's physical A and B sit
 * where an Xbox pad puts B and A.  Translating here means the labels on the
 * pad keep their usual meaning on a Switch.
 *
 * Sticks are inverted on Y because the sensor reports Y increasing upwards
 * while Nintendo's convention has Y increasing downwards. */
static void fill_controller_state(u8 state[SWITCH_CONTROLLER_STATE_SIZE])
{
    u16 buttons = latest_input.buttons;
    u32 button_status = 0;

    memset(state, 0, SWITCH_CONTROLLER_STATE_SIZE);
    /* The timer advances by 3 per report, matching a real Pro Controller at
     * 60 Hz, and wraps naturally at 8 bits. */
    timer_counter += 3U;
    state[SWITCH_STATE_TIMER_OFFSET] = timer_counter;
    /* 0x81 = battery level 8 (full) with connection type 1 (wired).  The
     * Cyclone 2 does not measure its battery, so report a full pack. */
    state[SWITCH_STATE_BATTERY_OFFSET] = 0x81U;

    if (buttons & CYCLONE2_REPORT_BUTTON_X) button_status |= JC_BTN_Y;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) button_status |= JC_BTN_X;
    if (buttons & CYCLONE2_REPORT_BUTTON_A) button_status |= JC_BTN_B;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) button_status |= JC_BTN_A;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) button_status |= JC_BTN_L;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) button_status |= JC_BTN_R;
    if (latest_input.left_trigger > 0x20U) button_status |= JC_BTN_ZL;
    if (latest_input.right_trigger > 0x20U) button_status |= JC_BTN_ZR;
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
        /* QMI8658A is configured for +/-4 g (8192 counts/g); Nintendo's
         * expected +/-8 g report format uses 4096 counts/g. */
        write_s16_le(output + index * 12U + 0U, sample.accel_x / 2);
        write_s16_le(output + index * 12U + 2U, sample.accel_y / 2);
        write_s16_le(output + index * 12U + 4U, sample.accel_z / 2);
        write_s16_le(output + index * 12U + 6U, -sample.gyro_x); /* roll */
        write_s16_le(output + index * 12U + 8U, -sample.gyro_y); /* pitch */
        write_s16_le(output + index * 12U + 10U, sample.gyro_z); /* yaw */
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
    if (latest_input.left_trigger > 0x20U) report[0] |= NINTENDO_SIMPLE_ZL;
    if (latest_input.right_trigger > 0x20U) report[0] |= NINTENDO_SIMPLE_ZR;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) report[1] |= JC_BTN_MINUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) report[1] |= JC_BTN_PLUS >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) report[1] |= JC_BTN_RSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) report[1] |= JC_BTN_LSTICK >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) report[1] |= JC_BTN_HOME >> 8;
    if (buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) report[1] |= JC_BTN_CAP >> 8;
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
 * The bytes live in data/nintendo_switch_pro_spi.bin, which is indexed exactly
 * like the controller's flash: the file starts at address 0x6000 and every
 * address in the table below is (offset in that file) + 0x6000.  Keeping the
 * data binary means the flash contents can be hex-dumped and compared against
 * a real controller without reading C.
 *
 * A read may be of any length and may cross region boundaries, so
 * read_virtual_spi() resolves each byte on its own.  A byte with no region
 * reads 0xff, the value of erased flash.
 */

/* First address covered by nintendo_switch_pro_spi_flash[]. */
#define SWITCH_SPI_WINDOW_BASE 0x6000U

struct switch_spi_region {
    u32 address;
    u16 length;
    const u8 *data;
};

/* Points at an address inside nintendo_switch_pro_spi_flash[]; the blob is
 * indexed like the flash, so the offset is simply address - window base. */
#define SWITCH_SPI_BYTE(address) \
    (nintendo_switch_pro_spi_flash + ((address) - SWITCH_SPI_WINDOW_BASE))

static const struct switch_spi_region switch_spi_regions[] = {
    /* 0x6000: mostly erased; the controller type 0x03 0x02 sits at 0x6012 */
    {0x6000U, 32U, SWITCH_SPI_BYTE(0x6000U)},
    /* 0x6020: accelerometer and gyroscope factory calibration */
    {0x6020U, 24U, SWITCH_SPI_BYTE(0x6020U)},
    /* 0x603d: stick factory calibration, left then right */
    {0x603dU, 18U, SWITCH_SPI_BYTE(0x603dU)},
    /* 0x6050: body, button and grip colours */
    {0x6050U, 12U, SWITCH_SPI_BYTE(0x6050U)},
    /* 0x6080: 6-axis parameters */
    {0x6080U, 6U, SWITCH_SPI_BYTE(0x6080U)},
    /* 0x6086: left stick parameters */
    {0x6086U, 18U, SWITCH_SPI_BYTE(0x6086U)},
    /* 0x6098: right stick parameters */
    {0x6098U, 18U, SWITCH_SPI_BYTE(0x6098U)},
};

/* The blob covers 0x6000..0x60a9 because the last region ends at 0x60a9; a
 * different size means the blob and the table above no longer agree. */
_Static_assert(sizeof(nintendo_switch_pro_spi_flash) == 0xaaU,
               "Switch SPI window size changed");

/* One byte of virtual flash; addresses outside the table are erased. */
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
        data[2] = 0x03U; /* Pro Controller */
        data[3] = 0x02U;
        fill_address_big_endian(data + 4U, mac);
        data[10] = 0x00U;
        data[11] = 0x02U;
        break;
    }
    case SWITCH_SUBCOMMAND_REPORT_MODE:
        /* The console switches between the 11-byte simple report and the
         * 48-byte full report here; ignore anything else it might ask for. */
        if (request_size != 0U &&
            (request[0] == SWITCH_REPORT_SIMPLE_STATE ||
             request[0] == SWITCH_REPORT_FULL_STATE)) {
            input_report_mode = request[0];
        }
        break;
    case SWITCH_SUBCOMMAND_TRIGGER_TIME: {
        /* Elapsed time for seven trigger buttons; we do not measure it, so
         * report a constant 100 ms per entry like the stock firmware. */
        u8 index;

        ack[0] = 0x83U;
        for (index = 0U; index < 14U; index += 2U)
            write_s16_le(data + index, 100);
        break;
    }
    case SWITCH_SUBCOMMAND_HCI_STATE:
        /* Argument 0 means "stop sending input reports"; the console sends
         * it on its way to sleep.  Nothing clears this until the profile is
         * restarted, which matches how the link is torn down here. */
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
        /* Low nibble = which player LEDs are lit.  The Cyclone 2 has one RGB
         * chain, so show any lit player LED as dim green on all of them. */
        if (request_size != 0U) {
            cyclone2_led_set_all(0U, request[0] & 0x0fU ? 8U : 0U, 0U);
        }
        break;
    case SWITCH_SUBCOMMAND_HOME_LIGHT:
        /* HOME_LIGHT is a separate Switch command.  Do not let it change
         * the IMU state: the old fall-through made an ordinary home-LED
         * update with value zero stop motion sampling. */
        break;
    case SWITCH_SUBCOMMAND_ENABLE_IMU:
        /* Motion reporting is normally off; the console turns it on once it
         * has read the calibration above. */
        imu_enabled = request_size != 0U && request[0] != 0U;
        if (imu_enabled) cyclone2_imu_enable();
        else cyclone2_imu_disable();
        break;
    case SWITCH_SUBCOMMAND_IMU_CONFIG:
        /* Sensitivity selection.  We always report the factory default, so
         * accept the command and keep the default scaling. */
        break;
    case SWITCH_SUBCOMMAND_ENABLE_RUMBLE:
        /* Arguments may carry a vibration code; only the on/off state is
         * tracked here because the ERM motors cannot reproduce Nintendo's
         * two-frequency HD rumble waveforms. */
        rumble_enabled = request_size != 0U && request[0] != 0U;
        if (!rumble_enabled) cyclone2_rumble_stop();
        break;
    default:
        /* Unknown subcommands still get a bare acknowledgement so the host
         * does not retry until it gives up on the controller. */
        ack[0] = 0x80U;
        break;
    }

    /* The reply is sent immediately, without waiting for the next scheduled
     * input report: the host is blocking on it.  Return value is ignored by
     * the current transport, so a full transmit queue would drop the reply;
     * see the report scheduler work in the architecture plan. */
    edr_hid_data_send(SWITCH_REPORT_SUBCOMMAND_REPLY, reply, sizeof(reply));
}

/* Handle an output report from the host.
 *
 * `packet` still starts with the HIDP header byte (0xa2 for the interrupt
 * channel, 0x52 for control-channel SET_REPORT) followed by the report id;
 * the offsets below are the ones documented in the file header. */
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
        /* Report 0x10: packet[2] is the packet counter, packet[3..10] the
         * two four-byte rumble fields. */
        handle_rumble(packet + 3U);
        return;
    }
    if (report_id == SWITCH_OUTPUT_SUBCOMMAND && size >= 12U) {
        /* Report 0x01: same rumble block, then the subcommand at packet[11]
         * with its arguments from packet[12] on. */
        cyclone2_usb_debug("Switch subcommand=%02x arg0=%02x size=%u",
                           packet[11], size > 12U ? packet[12] : 0U, size);
        handle_rumble(packet + 3U);
        reply_subcommand(packet[11], packet + 12, size - 12U);
    }
}

static void switch_hid_initialize(void)
{
    memset(&latest_input, 0, sizeof(latest_input));
    input_report_mode = SWITCH_REPORT_SIMPLE_STATE;
    latest_input.hat = CYCLONE2_HAT_NEUTRAL;
    latest_input.left_x = latest_input.left_y = 0x80U;
    latest_input.right_x = latest_input.right_y = 0x80U;
    rumble_enabled = 0U;
    imu_enabled = 0U;
    host_stopped = 0U;
    tx_debug_remaining = 4U;
    simple_last_send_ms = sys_timer_get_ms() - SWITCH_SIMPLE_KEEPALIVE_MS;
    cyclone2_imu_disable();
    user_hid_init(switch_output);
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
        result = edr_hid_data_send(SWITCH_REPORT_FULL_STATE, report,
                                   sizeof(report));
    } else {
        now = sys_timer_get_ms();
        if (!input_changed &&
            now - simple_last_send_ms < SWITCH_SIMPLE_KEEPALIVE_MS) {
            return 0;
        }
        fill_simple_state(report);
        result = edr_hid_data_send(SWITCH_REPORT_SIMPLE_STATE, report,
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
    if (result == 3) return -1; /* SDK HID circular buffer is full. */
    return -2;
}

#define SWITCH_HID_CONNECT_DELAY_MS 100U

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
    .sniff_param = NULL,
};

static u8 switch_slot;
static u8 switch_pairing;
static u8 bond_delete_pending;
static u8 peer_debug_remaining;
static u8 bond_to_delete[6];
static u8 reconnect_address[6];

/* Log the far end of a connection event.  Addresses arrive least significant
 * byte first, like our own, so print them back to front. */
static void log_peer_address(const char *what, const u8 *address)
{
    cyclone2_usb_debug("%s peer %02x:%02x:%02x:%02x:%02x:%02x", what,
                       address[5], address[4], address[3], address[2],
                       address[1], address[0]);
}

extern void bt_set_local_name(char *name, u8 len);
extern void delete_link_key(bd_addr_t *address, u8 index);

/* Drop the stored link key for a peer.
 *
 * Bonding records live in the Bluetooth controller's own memory (a chip dump
 * shows one at 0x10760: peer address followed by the key), not in flash, so
 * neither reflashing nor erasing VM can clear them.  delete_link_key() is the
 * SDK's way in: the slot index selects a pairing entry and the record is
 * matched by peer address.  Try every index, because the index the SDK tracks
 * belongs to the current connection and a connection that fails
 * authentication never establishes one. */
void cyclone2_switch_forget_peer(const u8 address[6])
{
    u8 index;

    for (index = 0U; index < 4U; index++) {
        delete_link_key((bd_addr_t *)address, index);
    }
}

/* Reconnect to the console whose address is stored in syscfg for this slot.
 * Paging has to wait until the stack has finished initialising, which is why
 * this runs from a timer rather than directly. */
static void reconnect_saved(void *private_data)
{
    (void)private_data;
    if (edr_hid_is_connected()) return;
    bt_wait_phone_connect_control(0);
    user_send_cmd_prepare(USER_CTRL_START_CONNEC_VIA_ADDR,
                          sizeof(reconnect_address), reconnect_address);
}

/* A console's HID host opens the interrupt channel only after the link is
 * encrypted, and Nintendo's pairing flow expects the controller to ask for
 * the HID channels here.  Without this the ACL link comes up but no input
 * report would ever have a channel to travel on. */
static void connect_hid_after_encryption(void *private_data)
{
    int result;

    (void)private_data;
    if (edr_hid_is_connected()) return;
    result = user_send_cmd_prepare(USER_CTRL_HID_CONN, 0, NULL);
    cyclone2_usb_debug("request HID channels after encryption result=%d",
                       result);
}

/* Bring up the Switch profile for one slot.
 *
 * `pairing` is set when the user asked to forget the old bond and pair again:
 * the stored remote address for the slot is cleared and the controller waits
 * to be paired (page and inquiry scan enabled once the stack is up).  Bonded
 * starts instead page the stored address, or fall back to the stack's own
 * "connect last device" memory if the slot has no stored address yet. */
void cyclone2_switch_start(u8 slot, u8 pairing)
{
    u8 remote[6];

    switch_slot = slot;
    switch_pairing = pairing;
    bond_delete_pending = 0U;
    peer_debug_remaining = 8U;
    if (pairing) {
        if (syscfg_read(CFG_EDR_ADDRESS_BEGIN + slot, remote,
                        sizeof(remote)) == sizeof(remote)) {
            memcpy(bond_to_delete, remote, sizeof(remote));
            bond_delete_pending = 1U;
        }
        memset(remote, 0, sizeof(remote));
        syscfg_write(CFG_EDR_ADDRESS_BEGIN + slot, remote, sizeof(remote));
    }

    bt_set_local_name("Pro Controller", sizeof("Pro Controller") - 1U);
    btstack_edr_start_before_init(&switch_edr_config, 0);
    switch_hid_initialize();
}

void cyclone2_switch_disconnect(void)
{
    user_hid_disconnect();
}

void cyclone2_switch_exit(void)
{
    cyclone2_imu_disable();
    btstack_edr_exit(0);
}

int cyclone2_switch_is_connected(void)
{
    return edr_hid_is_connected() != 0;
}

enum cyclone2_profile_event cyclone2_switch_handle_event(
    const struct sys_event *event)
{
    static const u8 zero_address[6];
    u8 remote[6];

    /* The connection sequence an active session goes through is:
     *   INIT_OK            stack ready; start paging or become pairable
     *   FIRST/SECOND_CONNECTED  ACL is up; remember the console's address
     *   ENCRY_COMPLETE     link is trustworthy; open the HID channels
     *   disconnect events  tear the session down (see the controller)
     * Only the interrupt channel counts as "connected" for reports, which
     * cyclone2_switch_is_connected() reports. */
    if ((u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
        bt_comm_edr_status_event_handler((struct bt_event *)&event->u.bt);
        if (event->u.bt.event == BT_STATUS_INIT_OK) {
            if (switch_pairing) {
                if (bond_delete_pending) {
                    delete_link_key(bond_to_delete,
                                    get_remote_dev_info_index());
                    bond_delete_pending = 0U;
                }
                bt_wait_phone_connect_control(1);
            } else if (syscfg_read(CFG_EDR_ADDRESS_BEGIN + switch_slot,
                                   remote, sizeof(remote)) == sizeof(remote) &&
                       memcmp(remote, zero_address, sizeof(remote))) {
                memcpy(reconnect_address, remote, sizeof(remote));
                sys_timeout_add(NULL, reconnect_saved, 100U);
            } else if (!bt_connect_phone_back_start()) {
                bt_wait_phone_connect_control(1);
            }
        } else if (event->u.bt.event == BT_STATUS_FIRST_CONNECTED ||
                   event->u.bt.event == BT_STATUS_SECOND_CONNECTED) {
            tx_debug_remaining = 4U;
            log_peer_address("connected", event->u.bt.args);
            syscfg_write(CFG_EDR_ADDRESS_BEGIN + switch_slot,
                         event->u.bt.args, 6U);
        } else if (event->u.bt.event == BT_STATUS_ENCRY_COMPLETE) {
            log_peer_address("encrypted", event->u.bt.args);
            sys_timeout_add(NULL, connect_hid_after_encryption,
                            SWITCH_HID_CONNECT_DELAY_MS);
        } else if (event->u.bt.event == BT_STATUS_FIRST_DISCONNECT ||
                   event->u.bt.event == BT_STATUS_SECOND_DISCONNECT) {
            log_peer_address("disconnected", event->u.bt.args);
            return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
        }
    } else if ((u32)event->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        bt_comm_edr_hci_event_handler((struct bt_event *)&event->u.bt);
        if (event->u.bt.event == HCI_EVENT_CONNECTION_COMPLETE ||
            event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE ||
            event->u.bt.event == BTSTACK_EVENT_HCI_CONNECTIONS_DELETE) {
            if (peer_debug_remaining != 0U) {
                peer_debug_remaining--;
                log_peer_address("hci", event->u.bt.args);
            }
        }
        if (event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE)
            return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    }
    return CYCLONE2_PROFILE_EVENT_NONE;
}
