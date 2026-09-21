#include <string.h>

#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_event.h>
#include <btstack/btstack_task.h>
#include <btstack/third_party/common/ble_user.h>

#include "app_comm_bt.h"
#include "bt_common.h"
#include "input.h"
#include "le_common.h"
#include "le_gatt_common.h"
#include "profile.h"
#include "rumble.h"
#include "switch2_profile_data.h"

#define SWITCH2_REPORT_SIZE       63U
#define SWITCH2_COMMAND_PREFIX   0x21U
#define SWITCH2_MAX_COMMAND_DATA 0x78U
#define SWITCH2_LTK_SIZE          16U

#define SWITCH2_BTN_B             0x0001U
#define SWITCH2_BTN_A             0x0002U
#define SWITCH2_BTN_Y             0x0004U
#define SWITCH2_BTN_X             0x0008U
#define SWITCH2_BTN_R             0x0010U
#define SWITCH2_BTN_ZR            0x0020U
#define SWITCH2_BTN_PLUS          0x0040U
#define SWITCH2_BTN_RCLICK        0x0080U
#define SWITCH2_BTN_DOWN          0x0100U
#define SWITCH2_BTN_RIGHT         0x0200U
#define SWITCH2_BTN_LEFT          0x0400U
#define SWITCH2_BTN_UP            0x0800U
#define SWITCH2_BTN_L             0x1000U
#define SWITCH2_BTN_ZL            0x2000U
#define SWITCH2_BTN_MINUS         0x4000U
#define SWITCH2_BTN_LCLICK        0x8000U

#define SWITCH2_BTN_HOME          0x00010000UL
#define SWITCH2_BTN_CAPTURE       0x00020000UL
#define SWITCH2_BTN_GR            0x00040000UL
#define SWITCH2_BTN_GL            0x00080000UL
#define SWITCH2_BTN_C             0x00100000UL

/* The Switch 2 emulator reference advertises this exact Nintendo payload. */
static u8 switch2_adv_data[] = {
    0x02, 0x01, 0x06,
    0x1b, 0xff,
    0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 switch2_empty_report_map[] = { 0xc0 };
static const sm_cfg_t switch2_sm_config = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
    .authentication_req_flags = SM_AUTHREQ_BONDING,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

static u16 switch2_con_handle;
static u8 switch2_counter;
static u8 switch2_ready;
static u8 switch2_ltk_valid;
static u8 switch2_ltk[SWITCH2_LTK_SIZE];
static struct cyclone2_input_report switch2_input;

/* Exported by the JL crypto toolbox linked by the firmware SDK. */
extern void aes128_calc_cyphertext(const u8 *key, const u8 *plaintext,
                                   u8 *cyphertext);

static const u8 switch2_ltk_key_b1[SWITCH2_LTK_SIZE] = {
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1,
    0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
};

static const struct conn_update_param_t switch2_connection_params[] = {
    {6, 6, 0, 600},
    {6, 9, 0, 600},
    {12, 12, 0, 600},
};

static uint16_t switch2_att_read_callback(hci_con_handle_t connection_handle,
                                           uint16_t att_handle, uint16_t offset,
                                           uint8_t *buffer, uint16_t buffer_size);
static int switch2_att_write_callback(hci_con_handle_t connection_handle,
                                      uint16_t att_handle,
                                      uint16_t transaction_mode, uint16_t offset,
                                      uint8_t *buffer, uint16_t buffer_size);
static int switch2_event_packet_handler(int event, u8 *packet, u16 size,
                                        u8 *ext_param);

static const gatt_server_cfg_t switch2_server_config = {
    .att_read_cb = switch2_att_read_callback,
    .att_write_cb = switch2_att_write_callback,
    .event_packet_handler = switch2_event_packet_handler,
};

static const gatt_ctrl_t switch2_gatt_control_block = {
    .mtu_size = 517,
    .cbuffer_size = 1024,
    .multi_dev_flag = 0,
    .server_config = (gatt_server_cfg_t *)&switch2_server_config,
    .client_config = NULL,
    .sm_config = (sm_cfg_t *)&switch2_sm_config,
    .hci_cb_packet_handler = NULL,
};

static adv_cfg_t switch2_adv_config = {
    .adv_data = switch2_adv_data,
    .rsp_data = NULL,
    .adv_data_len = sizeof(switch2_adv_data),
    .rsp_data_len = 0,
    .adv_interval = 48,
    .adv_auto_do = 1,
    .adv_type = ADV_IND,
    .adv_channel = ADV_CHANNEL_ALL,
};

static void switch2_post_ble_event(u8 event)
{
    struct sys_event event_data;

    memset(&event_data, 0, sizeof(event_data));
    event_data.type = SYS_BT_EVENT;
    event_data.arg = (void *)SYS_BT_EVENT_BLE_STATUS;
    event_data.u.bt.event = event;
    sys_event_notify(&event_data);
}

static int switch2_event_packet_handler(int event, u8 *packet, u16 size,
                                        u8 *ext_param)
{
    (void)size;
    (void)ext_param;

    switch (event) {
    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        switch2_con_handle = little_endian_read_16(packet, 0);
        ble_gatt_server_connetion_update_request(
            switch2_con_handle, switch2_connection_params,
            sizeof(switch2_connection_params) /
                sizeof(switch2_connection_params[0]));
        switch2_post_ble_event(BLE_ST_CONNECT);
        break;
    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        switch2_con_handle = 0;
        switch2_ready = 0;
        switch2_post_ble_event(BLE_ST_DISCONN);
        break;
    default:
        break;
    }
    return 0;
}

static u16 switch2_copy_read(u8 *buffer, u16 buffer_size, u16 offset,
                             const u8 *value, u16 value_size)
{
    u16 copy_size;

    if (offset >= value_size) return 0;
    if (!buffer) return value_size - offset;
    copy_size = value_size - offset;
    if (copy_size > buffer_size) copy_size = buffer_size;
    memcpy(buffer, value + offset, copy_size);
    return copy_size;
}

static void switch2_pack_stick(u8 output[3], u8 x, u8 y)
{
    u16 packed_x = ((u16)x << 4) | (x >> 4);
    u16 packed_y = ((u16)y << 4) | (y >> 4);

    output[0] = packed_x;
    output[1] = (packed_x >> 8) | (packed_y << 4);
    output[2] = packed_y >> 4;
}

static void switch2_make_report(u8 output[SWITCH2_REPORT_SIZE])
{
    u32 buttons = 0;
    u8 hat = switch2_input.hat;

    memset(output, 0, SWITCH2_REPORT_SIZE);
    output[0] = ++switch2_counter;
    output[1] = 0x20;

    /* Keep the GameSir's printed A/B and X/Y labels in the same Nintendo
     * order used by the existing Switch Classic profile. */
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_A) buttons |= SWITCH2_BTN_B;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_B) buttons |= SWITCH2_BTN_A;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_X) buttons |= SWITCH2_BTN_Y;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_Y) buttons |= SWITCH2_BTN_X;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_R1) buttons |= SWITCH2_BTN_R;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_L1) buttons |= SWITCH2_BTN_L;
    if (switch2_input.left_trigger > 0x20U) buttons |= SWITCH2_BTN_ZL;
    if (switch2_input.right_trigger > 0x20U) buttons |= SWITCH2_BTN_ZR;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_START) buttons |= SWITCH2_BTN_PLUS;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_SELECT) buttons |= SWITCH2_BTN_MINUS;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_R3) buttons |= SWITCH2_BTN_RCLICK;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_L3) buttons |= SWITCH2_BTN_LCLICK;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_HOME) buttons |= SWITCH2_BTN_HOME;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_SCREENSHOT) buttons |= SWITCH2_BTN_CAPTURE;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_R4) buttons |= SWITCH2_BTN_GR;
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_L4) buttons |= SWITCH2_BTN_GL;
    /* The Cyclone 2's M button is the extra physical button available for C. */
    if (switch2_input.buttons & CYCLONE2_REPORT_BUTTON_M) buttons |= SWITCH2_BTN_C;

    switch (hat) {
    case CYCLONE2_HAT_DOWN:       buttons |= SWITCH2_BTN_DOWN; break;
    case CYCLONE2_HAT_DOWN_RIGHT: buttons |= SWITCH2_BTN_DOWN | SWITCH2_BTN_RIGHT; break;
    case CYCLONE2_HAT_RIGHT:      buttons |= SWITCH2_BTN_RIGHT; break;
    case CYCLONE2_HAT_UP_RIGHT:   buttons |= SWITCH2_BTN_UP | SWITCH2_BTN_RIGHT; break;
    case CYCLONE2_HAT_UP:         buttons |= SWITCH2_BTN_UP; break;
    case CYCLONE2_HAT_UP_LEFT:    buttons |= SWITCH2_BTN_UP | SWITCH2_BTN_LEFT; break;
    case CYCLONE2_HAT_LEFT:       buttons |= SWITCH2_BTN_LEFT; break;
    case CYCLONE2_HAT_DOWN_LEFT:  buttons |= SWITCH2_BTN_DOWN | SWITCH2_BTN_LEFT; break;
    default: break;
    }

    output[2] = buttons;
    output[3] = buttons >> 8;
    output[4] = buttons >> 16;
    switch2_pack_stick(output + 5, switch2_input.left_x,
                       (u8)(0xffU - switch2_input.left_y));
    switch2_pack_stick(output + 8, switch2_input.right_x,
                       (u8)(0xffU - switch2_input.right_y));
    output[11] = 0x38;
    output[14] = 0x28;
}

static void switch2_rumble(const u8 *data, u16 size)
{
    u8 left;
    u8 right;

    if (size < 8U) return;
    left = (u8)(((u16)(data[1] & 0xfeU) * 255U) / 0xc8U);
    right = (u8)(((u16)(data[5] & 0xfeU) * 255U) / 0xc8U);
    if (data[1] == 0U && data[3] == 0x40U) left = 0;
    if (data[5] == 0U && data[7] == 0x40U) right = 0;
    cyclone2_rumble_set_latched(left, right);
}

static void switch2_reverse_bytes(const u8 *input, u8 *output, u8 length)
{
    u8 index;

    for (index = 0; index < length; ++index)
        output[index] = input[length - index - 1U];
}

static const u8 switch2_flash_13000[] = {
    0x01, 0x00, 'C','Y','C','L','O','N','E','2','-','S','W','2', 0x00, 0x00,
    0x00, 0x00, 0x7e, 0x05, 0x69, 0x20, 0x01, 0x06, 0x01,
    0x23, 0x23, 0x23, 0xa0, 0xa0, 0xa0, 0xe6, 0xe6, 0xe6,
    0x32, 0x32, 0x32,
};

static const u8 switch2_flash_13040[] = {
    0x3b, 0xe0, 0xd3, 0x41, 0xc6, 0x60, 0x6a, 0xbc,
    0x4d, 0xd7, 0xa2, 0xbb, 0x71, 0x1e, 0xdd, 0x37,
};

static const u8 switch2_flash_13080[] = {
    0x01,0xad,0xd9,0x9a,0x55,0x56,0x65,0xa0,0x00,0x0a,0xa0,0x00,0x0a,0xe2,0x20,0x0e,
    0xe2,0x20,0x0e,0x9a,0xad,0xd9,0x9a,0xad,0xd9,0x0a,0xa5,0x50,0x0a,0xa5,0x50,0x2f,
    0xf6,0x62,0x2f,0xf6,0x62,0x0a,0xff,0xff,
};

static const u8 switch2_flash_130a8[] = {
    0xb3,0x67,0x83,0x2e,0x66,0x5e,0x3a,0x06,0x5f,
};

static const u8 switch2_flash_130c0[] = {
    0x01,0xad,0xd9,0x9a,0x55,0x56,0x65,0xa0,0x00,0x0a,0xa0,0x00,0x0a,0xe2,0x20,0x0e,
    0xe2,0x20,0x0e,0x9a,0xad,0xd9,0x9a,0xad,0xd9,0x0a,0xa5,0x50,0x0a,0xa5,0x50,0x2f,
    0xf6,0x62,0x2f,0xf6,0x62,0x0a,0xff,0xff,
};

static const u8 switch2_flash_130e8[] = {
    0x2c,0x08,0x84,0xd1,0x65,0x63,0x2a,0x26,0x62,
};

static const u8 switch2_flash_13100[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xa6,0xf2,0x62,0xbd,0xa8,0x00,0x08,0x3d,0x2f,0xed,0x20,0x41,
};

static void switch2_copy_flash(u32 address, u8 *output, u16 length,
                               u32 block_address, const u8 *block,
                               u16 block_length)
{
    u32 end = address + length;
    u32 block_end = block_address + block_length;
    u32 start;
    u32 copy_end;

    if (end <= block_address || address >= block_end) return;
    start = address > block_address ? address : block_address;
    copy_end = end < block_end ? end : block_end;
    memcpy(output + start - address, block + start - block_address,
           copy_end - start);
}

static void switch2_read_flash(u32 address, u8 *output, u16 length)
{
    memset(output, 0xff, length);
    switch2_copy_flash(address, output, length, 0x013000U,
                       switch2_flash_13000, sizeof(switch2_flash_13000));
    switch2_copy_flash(address, output, length, 0x013040U,
                       switch2_flash_13040, sizeof(switch2_flash_13040));
    switch2_copy_flash(address, output, length, 0x013080U,
                       switch2_flash_13080, sizeof(switch2_flash_13080));
    switch2_copy_flash(address, output, length, 0x0130a8U,
                       switch2_flash_130a8, sizeof(switch2_flash_130a8));
    switch2_copy_flash(address, output, length, 0x0130c0U,
                       switch2_flash_130c0, sizeof(switch2_flash_130c0));
    switch2_copy_flash(address, output, length, 0x0130e8U,
                       switch2_flash_130e8, sizeof(switch2_flash_130e8));
    switch2_copy_flash(address, output, length, 0x013100U,
                       switch2_flash_13100, sizeof(switch2_flash_13100));
}

static u8 switch2_command_data(u8 command, u8 subcommand, const u8 *request,
                               u16 request_size, u8 *output)
{
    static const u8 firmware_info[12] = {
        0x02,0x01,0x04,0x02,0x0c,0x00,0x00,0x00,0x00,0x02,0x03,0x00,
    };
    static const u8 unknown_1103[29] = {
        0x01,0x20,0x03,0x00,0x00,0x0a,0xe8,0x1c,0x3b,0x79,0x7d,0x8b,0x3a,
        0x0a,0xe8,0x9c,0x42,0x58,0xa0,0x0b,0x42,0x0a,0xe8,0x9c,0x41,0x58,
        0xa0,0x0b,0x41,
    };
    u8 feature_flags[8] = {0};
    const u8 *mac;
    u32 address;
    u8 length;
    u8 index;
    static u8 ltk_a1[SWITCH2_LTK_SIZE];
    static u8 ltk_a2[SWITCH2_LTK_SIZE];
    static u8 ltk_b2[SWITCH2_LTK_SIZE];

    switch (command) {
    case 0x01:
        if (subcommand == 0x0c) {
            output[0] = 0x61; output[1] = 0x12;
            output[2] = 0x50; output[3] = 0x10;
            return 4;
        }
        break;
    case 0x02:
        if (subcommand == 0x04 && request_size >= 15U) {
            length = request[8];
            if (length > SWITCH2_MAX_COMMAND_DATA - 8U) return 0;
            address = (u32)request[12] | ((u32)request[13] << 8) |
                      ((u32)request[14] << 16);
            switch2_read_flash(address, output + 8U, length);
            memset(output, 0, 8U);
            memcpy(output, request + 8U, 8U);
            output[1] = 0;
            return length + 8U;
        }
        break;
    case 0x07:
        if (subcommand == 0x01) {
            output[0] = 0;
            return 1;
        }
        break;
    case 0x0c:
        if (subcommand == 0x01) {
            feature_flags[0] = 0x07;
            feature_flags[1] = 0x07;
            feature_flags[2] = 0x01;
            memcpy(output + 4U, feature_flags, sizeof(feature_flags));
            return 12;
        }
        if (subcommand == 0x04) {
            switch2_ready = 1;
            memset(output, 0, 4U);
            return 4;
        }
        if (subcommand == 0x02 || subcommand == 0x03 || subcommand == 0x05) {
            memset(output, 0, 4U);
            return 4;
        }
        break;
    case 0x10:
        if (subcommand == 0x01) {
            memcpy(output, firmware_info, sizeof(firmware_info));
            return sizeof(firmware_info);
        }
        break;
    case 0x11:
        if (subcommand == 0x01) {
            output[0] = 1;
            output[1] = output[2] = output[3] = 0;
            return 4;
        }
        if (subcommand == 0x03) {
            memcpy(output, unknown_1103, sizeof(unknown_1103));
            return sizeof(unknown_1103);
        }
        break;
    case 0x15:
        if (subcommand == 0x01) {
            mac = bt_get_mac_addr();
            output[0] = 1; output[1] = 4; output[2] = 1;
            /* JL stores Bluetooth addresses least-significant byte first;
             * the Switch 2 command carries the reference's reversed form. */
            for (index = 0; index < 6U; ++index)
                output[3U + index] = mac[5U - index];
            return 9;
        }
        if (subcommand == 0x04 && request_size >= 9U + SWITCH2_LTK_SIZE) {
            memcpy(ltk_a1, request + 9U, SWITCH2_LTK_SIZE);
            for (index = 0; index < SWITCH2_LTK_SIZE; ++index)
                switch2_ltk[index] =
                    ltk_a1[SWITCH2_LTK_SIZE - index - 1U] ^
                    switch2_ltk_key_b1[SWITCH2_LTK_SIZE - index - 1U];
            switch2_ltk_valid = 1;
            output[0] = 1;
            memcpy(output + 1U, switch2_ltk_key_b1, SWITCH2_LTK_SIZE);
            return SWITCH2_LTK_SIZE + 1U;
        }
        if (subcommand == 0x02 && request_size >= 9U + SWITCH2_LTK_SIZE &&
            switch2_ltk_valid) {
            memcpy(ltk_a2, request + 9U, SWITCH2_LTK_SIZE);
            switch2_reverse_bytes(ltk_a2, ltk_a1, SWITCH2_LTK_SIZE);
            aes128_calc_cyphertext(switch2_ltk, ltk_a1, ltk_b2);
            output[0] = 1;
            memcpy(output + 1U, ltk_b2, SWITCH2_LTK_SIZE);
            return SWITCH2_LTK_SIZE + 1U;
        }
        if (subcommand == 0x03 && request_size >= 10U &&
            (request[8] == 0U || request[9] == 0U)) {
            output[0] = 1;
            return 1;
        }
        break;
    case 0x16:
        if (subcommand == 0x01) {
            memset(output, 0, 0x18U);
            return 0x18;
        }
        break;
    case 0x18:
        if (subcommand == 0x01) {
            output[0] = 0; output[1] = 0; output[2] = 0x40; output[3] = 0xf0;
            output[4] = 0; output[5] = 0; output[6] = 0x60; output[7] = 0;
            return 8;
        }
        break;
    default:
        /* 0x09/0x0a confirmations intentionally have an empty response. */
        break;
    }
    return 0;
}

static void switch2_send_command_response(const u8 *request, u16 request_size)
{
    static u8 data[SWITCH2_MAX_COMMAND_DATA];
    static u8 response[SWITCH2_COMMAND_PREFIX + 8U + SWITCH2_MAX_COMMAND_DATA];
    u8 output_length;
    u16 response_length;
    u16 offset = request_size >= SWITCH2_COMMAND_PREFIX + 8U ?
                 SWITCH2_COMMAND_PREFIX : 0U;
    const u8 *command = request + offset;

    if (request_size < offset + 8U) return;
    memset(data, 0, sizeof(data));
    output_length = switch2_command_data(command[0], command[3], command,
                                         request_size - offset, data);
    memset(response, 0, SWITCH2_COMMAND_PREFIX + 8U + output_length);
    memcpy(response + SWITCH2_COMMAND_PREFIX, command, 4U);
    response[SWITCH2_COMMAND_PREFIX + 4U] = 0x10;
    response[SWITCH2_COMMAND_PREFIX + 5U] = 0x78;
    if (command[1] == 0x91U)
        response[SWITCH2_COMMAND_PREFIX + 1U] = 1;
    memcpy(response + SWITCH2_COMMAND_PREFIX + 8U, data, output_length);
    response_length = SWITCH2_COMMAND_PREFIX + 8U + output_length;
    ble_comm_att_send_data(switch2_con_handle, SWITCH2_ATT_RESPONSE_HANDLE,
                           response, response_length, ATT_OP_AUTO_READ_CCC);
}

static uint16_t switch2_att_read_callback(hci_con_handle_t connection_handle,
                                           uint16_t att_handle, uint16_t offset,
                                           uint8_t *buffer, uint16_t buffer_size)
{
    u8 report[SWITCH2_REPORT_SIZE];
    static const u8 device_info[] = {0x04,0x00,0x05,0x00,0x01,0x01,0x00};
    static const u8 device_info2[] = {0x36,0x80,0x74,0xee,0xbb,0x3d,0x8e,0x13};

    (void)connection_handle;
    switch (att_handle) {
    case 0x0003: return switch2_copy_read(buffer, buffer_size, offset,
                                           device_info, sizeof(device_info));
    case 0x0007: return switch2_copy_read(buffer, buffer_size, offset,
                                           device_info2, sizeof(device_info2));
    case SWITCH2_ATT_INPUT_REPORT_HANDLE:
        switch2_make_report(report);
        return switch2_copy_read(buffer, buffer_size, offset, report,
                                  sizeof(report));
    default:
        return 0;
    }
}

static int switch2_att_write_callback(hci_con_handle_t connection_handle,
                                      uint16_t att_handle,
                                      uint16_t transaction_mode, uint16_t offset,
                                      uint8_t *buffer, uint16_t buffer_size)
{
    (void)transaction_mode;
    (void)offset;

    switch (att_handle) {
    case 0x0005:
    case SWITCH2_ATT_INPUT_REPORT_CCC_HANDLE:
    case 0x001bU:
    case SWITCH2_ATT_RESPONSE_CCC_HANDLE:
    case 0x0023U:
    case 0x0027U:
    case 0x002fU:
        if (buffer_size >= 1U)
            ble_gatt_server_characteristic_ccc_set(connection_handle,
                                                   att_handle, buffer[0]);
        return 0;
    case SWITCH2_ATT_VIBRATION_HANDLE:
        switch2_rumble(buffer, buffer_size);
        return 0;
    case SWITCH2_ATT_COMMAND_HANDLE:
        switch2_send_command_response(buffer, buffer_size);
        return 0;
    default:
        return 0;
    }
}

void cyclone2_ble_set_custom_mode(u8 enable);

void cyclone2_switch2_prepare_stack(void)
{
    static const ble_init_cfg_t config = {
        .same_address = 0,
        .appearance = BLE_APPEARANCE_HID_GAMEPAD,
        .report_map = switch2_empty_report_map,
        .report_map_size = sizeof(switch2_empty_report_map),
    };

    cyclone2_ble_set_custom_mode(1);
    btstack_ble_start_before_init(&config, 0);
}

void cyclone2_switch2_ble_before_init(void)
{
    ble_comm_init(&switch2_gatt_control_block);
}

void cyclone2_switch2_ble_init(void)
{
    ble_comm_set_config_name("Pro Controller", 0);
    ble_gatt_server_set_profile(cyclone2_switch2_profile_data,
                                sizeof(cyclone2_switch2_profile_data));
    ble_gatt_server_set_adv_config(&switch2_adv_config);
    ble_module_enable(1);
}

void cyclone2_switch2_ble_exit(void)
{
    ble_module_enable(0);
    ble_comm_exit();
    cyclone2_ble_set_custom_mode(0);
}

void cyclone2_switch2_start(u8 slot, u8 pairing)
{
    (void)slot;
    (void)pairing;
    memset(&switch2_input, 0, sizeof(switch2_input));
    switch2_input.hat = CYCLONE2_HAT_NEUTRAL;
    switch2_input.left_x = switch2_input.left_y = 0x80;
    switch2_input.right_x = switch2_input.right_y = 0x80;
    switch2_counter = 0;
    switch2_ready = 0;
    switch2_ltk_valid = 0;
    memset(switch2_ltk, 0, sizeof(switch2_ltk));
    switch2_con_handle = 0;
}

void cyclone2_switch2_disconnect(void)
{
    if (switch2_con_handle) ble_comm_disconnect(switch2_con_handle);
}

void cyclone2_switch2_exit(void)
{
    cyclone2_rumble_stop();
    btstack_ble_exit(0);
}

int cyclone2_switch2_is_connected(void)
{
    return switch2_con_handle != 0 &&
           ble_gatt_server_get_connect_state(switch2_con_handle) == BLE_ST_CONNECT;
}

int cyclone2_switch2_send(const struct cyclone2_input_report *report)
{
    u8 output[SWITCH2_REPORT_SIZE];
    int result;

    switch2_input = *report;
    if (!cyclone2_switch2_is_connected() ||
        !ble_gatt_server_characteristic_ccc_get(
            switch2_con_handle, SWITCH2_ATT_INPUT_REPORT_CCC_HANDLE)) return 0;
    if (!switch2_ready) return 0;
    switch2_make_report(output);
    result = ble_comm_att_send_data(switch2_con_handle,
                                    SWITCH2_ATT_INPUT_REPORT_HANDLE, output,
                                    sizeof(output), ATT_OP_AUTO_READ_CCC);
    if (result == GATT_OP_RET_SUCESS) return 1;
    if (result == GATT_BUFFER_FULL || result == GATT_CMD_RET_BUSY) return -1;
    if (result == GATT_CMD_USE_CCC_FAIL) return 0;
    return -2;
}

enum cyclone2_profile_event cyclone2_switch2_handle_event(
    const struct sys_event *event)
{
    if ((u32)event->arg != SYS_BT_EVENT_BLE_STATUS) return 0;
    if (event->u.bt.event == BLE_ST_CONNECT)
        return CYCLONE2_PROFILE_EVENT_CONNECTED;
    if (event->u.bt.event == BLE_ST_DISCONN)
        return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    return CYCLONE2_PROFILE_EVENT_NONE;
}
