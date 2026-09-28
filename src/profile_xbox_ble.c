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
#include "rijndael.h"
#include "rumble.h"
#include "usb.h"
#include "xbox_ble_att_db.h"

/*
 * Xbox Wireless Controller, Series X|S (model 1914, 045e:0b13), over
 * Bluetooth LE: HID over GATT with LE pairing.
 * ===================================================================
 *
 * The Series X|S controller only speaks LE.  Windows binds xinputhid to it
 * (XInput without a dongle), Linux hid-microsoft (rumble) and SDL know it, and
 * unlike the One S it has a Share button under the Xbox button, which the
 * capture button becomes.
 *
 * GATT: src/xbox_ble.gatt (compiled into xbox_ble_att_db.h) - GAP, Device
 * Information with the PnP ID, Battery, and the HID service with the
 * controller's own report map (283 bytes, as xpadneo dumped it), input report
 * 0x01 and output report 0x03.  The HID characteristics need an encrypted
 * link, so a host has to pair first; the pad also asks for it (SM security
 * request) as soon as a host connects.
 *
 * Pairing is BTstack's SM, legacy "just works" with bonding (see
 * btstack_jl_le.h).  The LTK we hand the host is derived from this slot's ER
 * and rebuilt from it when the host comes back, so nothing is written to
 * flash for a bond.  ER/IR come from the slot's Bluetooth address: a slot
 * given a new address (pair N fresh, newaddr) is a new controller to every
 * host, which matches how the hosts key their bonds anyway.
 *
 * Reports (layout in profile_xbox.c): input 0x01, 16 bytes, notified when it
 * changes; output 0x03, 8 bytes of rumble, written by the host.
 */

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

/* Flags, appearance (gamepad), 16-bit service list (HID).  The name does not
 * fit next to them; it is in the scan response.  Flags 0x06 (discoverable)
 * while pairing, 0x04 otherwise: bonded hosts reconnect either way. */
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
        /* The Cyclone 2 has no fuel gauge. */
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
    /* HID Control Point (suspend / exit suspend): nothing to do. */
}

static void xbox_ble_state_changed(int connected)
{
    if (!connected) {
        xbox_ble_secure = 0U;
        cyclone2_rumble_stop();
    }
    xbox_ble_post_event(connected ? BLE_ST_CONNECT : BLE_ST_DISCONN);
}

/* Encrypted: freshly paired, or a bonded host back.  A bonded host expects
 * the input report to still be subscribed (CCC values persist for bonded
 * clients), and Windows does not write it again on reconnect. */
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
    /* 7.5..11.25 ms, what hosts give a real controller. */
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
    uint32_t rk[RKLENGTH(128)];
    int rounds = rijndaelSetupEncrypt(rk, firmware_key, 128);
    u8 block[16];

    memset(block, 0, sizeof(block));
    if (address) memcpy(block, address, 6U);
    block[6] = slot;
    block[15] = 'E';
    rijndaelEncrypt(rk, rounds, block, xbox_ble_er);
    block[15] = 'I';
    rijndaelEncrypt(rk, rounds, block, xbox_ble_ir);
}

void cyclone2_xbox_ble_start(u8 slot, u8 pairing)
{
    /* LE only: no Classic page or inquiry scan. */
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

void cyclone2_xbox_ble_disconnect(void)
{
    btstack_jl_le_disconnect();
}

int cyclone2_xbox_ble_exit(void)
{
    cyclone2_rumble_stop();
    btstack_jl_le_set_scan_response(NULL, 0);
    btstack_jl_le_set_advertising(NULL, 0);
    return 1;
}

int cyclone2_xbox_ble_is_connected(void)
{
    return btstack_jl_le_connected();
}

/* Reconnection is the host's: it connects when it sees the advertisement. */
int cyclone2_xbox_ble_reconnect(void)
{
    return 0;
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

enum cyclone2_profile_event cyclone2_xbox_ble_handle_event(
    const struct sys_event *event)
{
    if ((u32)event->arg != SYS_BT_EVENT_BLE_STATUS) return 0;
    if (event->u.bt.event == BLE_ST_CONNECT)
        return CYCLONE2_PROFILE_EVENT_CONNECTED;
    if (event->u.bt.event == BLE_ST_DISCONN)
        return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    return CYCLONE2_PROFILE_EVENT_NONE;
}
