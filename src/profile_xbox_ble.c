#include <string.h>

#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_event.h>
#include <btstack/third_party/common/ble_user.h>

#include "app_comm_bt.h"
#include "bt_common.h"
#include "btstack_jl_le.h"
#include "input.h"
#include "le_common.h"
#include "profile.h"
#include "profile_xbox.h"
#include "aes128.h"
#include "rumble.h"
#include "usb.h"
#include "profile_xbox_ble_att_db.h"

#define XBOX_BLE_STATE_SIZE 16U
#define XBOX_BLE_INPUT_HANDLE \
    ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_REPORT_01_VALUE_HANDLE
#define XBOX_BLE_OUTPUT_HANDLE \
    ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_REPORT_02_VALUE_HANDLE
#define XBOX_BLE_BATTERY_HANDLE \
    ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL_01_VALUE_HANDLE

static u8 xbox_ble_er[16];
static u8 xbox_ble_ir[16];
static u8 xbox_ble_state[XBOX_BLE_STATE_SIZE];
static u8 xbox_ble_sent[XBOX_BLE_STATE_SIZE];
static volatile u8 xbox_ble_secure;
static u8 xbox_ble_have_sent;

static u8 xbox_ble_adv[] = {
    0x02, 0x01, 0x06,
    0x03, 0x19, 0xc4, 0x03,
    0x03, 0x03, 0x12, 0x18,
};

static const u8 xbox_ble_scan_response[] = {
    0x19, 0x09, 'X', 'b', 'o', 'x', ' ', 'W', 'i', 'r', 'e', 'l', 'e', 's',
    's', ' ', 'C', 'o', 'n', 't', 'r', 'o', 'l', 'l', 'e', 'r',
};

static void xbox_ble_post_event(u8 event)
{
    struct sys_event event_data;

    memset(&event_data, 0, sizeof(event_data));
    event_data.type = SYS_BT_EVENT;
    event_data.arg = (void *)SYS_BT_EVENT_BLE_STATUS;
    event_data.u.bt.event = event;
    sys_event_notify(&event_data);
}

static uint16_t xbox_ble_read(uint16_t handle, uint16_t offset, uint8_t *buffer,
                              uint16_t size)
{
    static const u8 battery_full = 100U;
    static const u8 no_rumble[8];
    const u8 *value;
    uint16_t length;

    switch (handle) {
    case XBOX_BLE_INPUT_HANDLE:
        value = xbox_ble_state;
        length = XBOX_BLE_STATE_SIZE;
        break;
    case XBOX_BLE_OUTPUT_HANDLE:
        value = no_rumble;
        length = sizeof(no_rumble);
        break;
    case XBOX_BLE_BATTERY_HANDLE:

        value = &battery_full;
        length = 1U;
        break;
    default:
        return 0;
    }
    if (buffer == NULL) return length;
    if (offset >= length) return 0;
    if (size > length - offset) size = length - offset;
    memcpy(buffer, value + offset, size);
    return size;
}

static void xbox_ble_write(uint16_t handle, const uint8_t *data, uint16_t size)
{
    if (handle == XBOX_BLE_OUTPUT_HANDLE && size >= 8U) cyclone2_xbox_rumble(data);

}

static void xbox_ble_state_changed(int connected)
{
    if (!connected) {
        xbox_ble_secure = 0U;
        cyclone2_rumble_stop();
    }
    xbox_ble_post_event(connected ? BLE_ST_CONNECT : BLE_ST_DISCONN);
}

static void xbox_ble_encrypted(void)
{
    btstack_jl_le_set_ccc(XBOX_BLE_INPUT_HANDLE, 0x0001U);
    xbox_ble_have_sent = 0U;
    xbox_ble_secure = 1U;
}

static const struct btstack_jl_le_config xbox_ble_le_config = {
    .att_db = profile_data,
    .read = xbox_ble_read,
    .write = xbox_ble_write,
    .state = xbox_ble_state_changed,
    .sm_er = xbox_ble_er,
    .sm_ir = xbox_ble_ir,
    .encrypted = xbox_ble_encrypted,

    .conn_interval_min = 6U,
    .conn_interval_max = 9U,
};

void cyclone2_xbox_ble_prepare_stack(void)
{
    btstack_jl_le_configure(&xbox_ble_le_config);
}

/* This slot's SM root keys: AES-128 under a fixed firmware key of the slot's
 * Bluetooth address and a key label.  Not secret against someone who has this
 * firmware and the address, which is as much as a gamepad's bond needs. */
static void xbox_ble_derive_keys(u8 slot)
{
    static const u8 firmware_key[16] = {
        'C', 'Y', 'C', 'L', 'O', 'N', 'E', '2',
        '-', 'X', 'B', 'O', 'X', 'B', 'L', 'E',
    };
    const u8 *address = bt_get_mac_addr();
    u8 block[16];

    memset(block, 0, sizeof(block));
    if (address) memcpy(block, address, 6U);
    block[6] = slot;
    block[15] = 'E';
    aes128_encrypt(firmware_key, block, xbox_ble_er);
    block[15] = 'I';
    aes128_encrypt(firmware_key, block, xbox_ble_ir);
}

void cyclone2_xbox_ble_start(u8 slot, u8 pairing)
{

    bt_wait_phone_connect_control(0);
    xbox_ble_secure = 0U;
    xbox_ble_have_sent = 0U;
    memset(xbox_ble_state, 0, sizeof(xbox_ble_state));
    xbox_ble_derive_keys(slot);
    xbox_ble_adv[2] = pairing ? 0x06U : 0x04U;
    btstack_jl_le_set_scan_response(xbox_ble_scan_response,
                                    sizeof(xbox_ble_scan_response));
    btstack_jl_le_set_advertising(xbox_ble_adv, sizeof(xbox_ble_adv));
    cyclone2_usb_debug("xbox-ble: %s advertisement",
                       pairing ? "pairing" : "reconnect");
}

/* Notify report 0x01 when the state changes.  The LE layer keeps only the
 * newest report and sends it when the link can take it, so nothing queues
 * up behind a slow connection. */
int cyclone2_xbox_ble_send(const struct cyclone2_input_report *report)
{
    cyclone2_xbox_fill_state(xbox_ble_state, report, 1);
    if (!btstack_jl_le_connected() || !xbox_ble_secure) return 0;
    if (xbox_ble_have_sent &&
        !memcmp(xbox_ble_state, xbox_ble_sent, sizeof(xbox_ble_sent)))
        return 0;
    memcpy(xbox_ble_sent, xbox_ble_state, sizeof(xbox_ble_sent));
    xbox_ble_have_sent = 1U;
    btstack_jl_le_set_report(XBOX_BLE_INPUT_HANDLE, xbox_ble_state,
                             sizeof(xbox_ble_state));
    return 1;
}
