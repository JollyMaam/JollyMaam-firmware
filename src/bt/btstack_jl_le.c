/*
 * LE peripheral on the BTstack port.  See btstack_jl_le.h.
 *
 * One connection.  CCC descriptors are ordinary dynamic attributes in the
 * profile's database; this file keeps their state (write 0x0001 enables
 * notifications) so the profile does not have to.
 *
 * SMP only when the profile asks for it (config->sm_er): the Switch 2 does not
 * use it and disconnects when it is attempted, so for it sm_init() is never
 * called - sm.c's HCI handler is then not registered (hci_init() clears the
 * handler list every session) - and the LE LTK request is answered here from
 * the profile.  HID over GATT hosts (the Xbox BLE profile) pair and bond
 * through BTstack's SM.
 */

#include <string.h>

#include "btstack_config.h"
#include "bluetooth.h"
#include "bluetooth_gatt.h"
#include "btstack_crypto.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h"
#include "ble/att_db.h"
#include "ble/sm.h"
#include "l2cap.h"

#include "btstack_jl_le.h"
/* Application hooks; declared here because the SDK headers that declare them
 * redefine bool and cannot be mixed with BTstack's. */
extern void cyclone2_bringup_record(uint32_t code);
extern void cyclone2_usb_debug(const char *format, ...);

#define LE_MAX_CCC        16U
#define LE_REPORT_MAX     64U
#define LE_PENDING_MAX   200U

extern void btstack_jl_core_wake(void);
extern void jl_enter_critical_public(void);
extern void jl_exit_critical_public(void);
/* Report statistics shared with the Classic path (btstack_jl_core.c). */
extern uint32_t btstack_jl_time_us(void);
extern void btstack_jl_air_reset(void);
extern uint32_t btstack_jl_air_note_post(uint8_t replacing, uint32_t now_us,
                                         uint32_t *posted_us);
extern void btstack_jl_air_note_take(uint32_t seq, uint32_t posted_us,
                                     uint32_t now_us);
extern void btstack_jl_air_note_handoff(uint16_t handle);
extern void btstack_jl_air_note_le(uint16_t interval, uint16_t latency,
                                   uint8_t tx_phy, uint8_t rx_phy);
extern int btstack_jl_link_has_room(uint16_t handle);
extern void btstack_jl_air_note_le_length(uint16_t tx_octets);
extern void btstack_jl_air_poll_link(uint16_t handle);

static const struct btstack_jl_le_config *le_config;
static btstack_packet_callback_registration_t le_hci_registration;
static volatile hci_con_handle_t le_con_handle = HCI_CON_HANDLE_INVALID;
static uint8_t le_working;

/* Advertising requested by the application (any task). */
static uint8_t le_adv_data[31];
static volatile uint8_t le_adv_len;
static volatile uint8_t le_adv_dirty;
static uint8_t le_scan_response[31];
static volatile uint8_t le_scan_response_len;

static uint8_t le_sm_active;
static btstack_packet_callback_registration_t le_sm_registration;

/* Latest input report (any task writes, run loop sends). */
static uint8_t le_report[LE_REPORT_MAX];
static volatile uint16_t le_report_len;
static volatile uint16_t le_report_handle;
static volatile uint8_t le_report_dirty;
static uint32_t le_report_seq;
static uint32_t le_report_posted_us;

/* One pending notification from the run loop (command responses). */
static uint8_t le_pending[LE_PENDING_MAX];
static uint16_t le_pending_len;
static uint16_t le_pending_handle;

static uint16_t le_ccc_handle[LE_MAX_CCC];
static uint8_t le_ccc_value[LE_MAX_CCC];

static volatile uint8_t le_disconnect_requested;
/* Ask for 251-byte PDUs once the command channel is free (btstack_jl_le_poll). */
static uint8_t le_data_length_pending;
/* The app asked to disconnect: it is shutting the radio down, so do not
 * advertise again - the host would reconnect for the ~100 ms until it does. */
static volatile uint8_t le_stopping;
static uint8_t le_initialized;
static uint8_t le_peer[6];
static uint8_t le_peer_valid;

static int ccc_index(uint16_t ccc_handle, int create)
{
    unsigned i;

    for (i = 0; i < LE_MAX_CCC; i++) {
        if (le_ccc_handle[i] == ccc_handle) return (int)i;
    }
    if (!create) return -1;
    for (i = 0; i < LE_MAX_CCC; i++) {
        if (le_ccc_handle[i] == 0) {
            le_ccc_handle[i] = ccc_handle;
            le_ccc_value[i] = 0;
            return (int)i;
        }
    }
    return -1;
}

int btstack_jl_le_notifications_enabled(uint16_t value_handle)
{
    int i = ccc_index((uint16_t)(value_handle + 1U), 0);
    return i >= 0 && (le_ccc_value[i] & 1U) != 0;
}

void btstack_jl_le_set_ccc(uint16_t value_handle, uint16_t value)
{
    int i = ccc_index((uint16_t)(value_handle + 1U), 1);
    if (i >= 0) le_ccc_value[i] = (uint8_t)value;
}

static int is_ccc(uint16_t handle)
{

    return att_uuid_for_handle(handle) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION;
}

static uint16_t le_att_read(hci_con_handle_t con_handle, uint16_t handle,
                            uint16_t offset, uint8_t *buffer,
                            uint16_t buffer_size)
{
    (void)con_handle;
    {
        int i = ccc_index(handle, 0);
        if (i >= 0) {
            uint8_t value[2] = {le_ccc_value[i], 0};
            if (buffer == NULL) return 2;
            if (offset >= 2) return 0;
            memcpy(buffer, value + offset,
                   buffer_size < 2 - offset ? buffer_size : 2 - offset);
            return (uint16_t)(2 - offset);
        }
    }
    if (le_config && le_config->read) {
        return le_config->read(handle, offset, buffer, buffer_size);
    }
    return 0;
}

static int le_att_write(hci_con_handle_t con_handle, uint16_t handle,
                        uint16_t transaction_mode, uint16_t offset,
                        uint8_t *buffer, uint16_t buffer_size)
{
    (void)con_handle;
    (void)offset;
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE) return 0;
    if (is_ccc(handle)) {
        int i = ccc_index(handle, 1);
        if (i >= 0 && buffer_size >= 1) le_ccc_value[i] = buffer[0];
        return 0;
    }
    if (le_config && le_config->write) {
        le_config->write(handle, buffer, buffer_size);
    }
    return 0;
}

static void le_try_send(void)
{
    if (le_con_handle == HCI_CON_HANDLE_INVALID) return;
    if (!att_server_can_send_packet_now(le_con_handle)) {
        att_server_request_can_send_now_event(le_con_handle);
        return;
    }
    if (le_pending_len != 0) {
        att_server_notify(le_con_handle, le_pending_handle, le_pending,
                          le_pending_len);
        le_pending_len = 0;
        if (le_report_dirty) att_server_request_can_send_now_event(le_con_handle);
        return;
    }
    /* The controller holds four LE buffers, and a notification waiting in
     * one cannot be replaced: keep the report here until the link has room
     * (the Number Of Completed Packets event wakes the loop to retry). */
    if (le_report_dirty && btstack_jl_link_has_room(le_con_handle)) {
        uint8_t copy[LE_REPORT_MAX];
        uint16_t length;
        uint16_t handle;
        uint32_t now_us = btstack_jl_time_us();
        int enabled;

        jl_enter_critical_public();
        length = le_report_len;
        handle = le_report_handle;
        memcpy(copy, le_report, length);
        le_report_dirty = 0;
        enabled = btstack_jl_le_notifications_enabled(handle);
        if (enabled) {
            btstack_jl_air_note_take(le_report_seq, le_report_posted_us, now_us);
        }
        jl_exit_critical_public();
        if (enabled) {
            btstack_jl_air_note_handoff(le_con_handle);
            att_server_notify(le_con_handle, handle, copy, length);
        }
    }
}

void btstack_jl_le_notify(uint16_t handle, const uint8_t *data,
                          uint16_t length)
{
    if (le_con_handle == HCI_CON_HANDLE_INVALID) return;
    if (length > LE_PENDING_MAX) length = LE_PENDING_MAX;
    if (le_pending_len == 0 && att_server_can_send_packet_now(le_con_handle)) {
        att_server_notify(le_con_handle, handle, data, length);
        return;
    }
    memcpy(le_pending, data, length);
    le_pending_len = length;
    le_pending_handle = handle;
    att_server_request_can_send_now_event(le_con_handle);
}

void btstack_jl_le_set_report(uint16_t handle, const uint8_t *data,
                              uint16_t length)
{
    uint32_t now_us = btstack_jl_time_us();
    uint8_t replacing;

    if (length > LE_REPORT_MAX) length = LE_REPORT_MAX;
    jl_enter_critical_public();
    replacing = le_report_dirty;
    le_report_seq = btstack_jl_air_note_post(replacing, now_us,
                                             &le_report_posted_us);
    memcpy(le_report, data, length);
    le_report_len = length;
    le_report_handle = handle;
    le_report_dirty = 1;
    jl_exit_critical_public();
    /* A report already waiting has a wake or a completion on its way. */
    if (!replacing) btstack_jl_core_wake();
}

static void le_apply_advertising(void)
{
    bd_addr_t none;

    if (!le_working) return;
    le_adv_dirty = 0;
    if (le_adv_len == 0 || le_stopping ||
        le_con_handle != HCI_CON_HANDLE_INVALID) {
        gap_advertisements_enable(0);
        return;
    }
    memset(none, 0, sizeof(none));

    gap_advertisements_set_params(0x0020, 0x0020, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(le_adv_len, le_adv_data);
    if (le_scan_response_len != 0)
        gap_scan_response_set_data(le_scan_response_len, le_scan_response);
    gap_advertisements_enable(1);
}

void btstack_jl_le_set_scan_response(const uint8_t *data, uint8_t length)
{
    if (length > sizeof(le_scan_response)) length = sizeof(le_scan_response);
    jl_enter_critical_public();
    if (data && length) memcpy(le_scan_response, data, length);
    le_scan_response_len = data ? length : 0;
    le_adv_dirty = 1;
    jl_exit_critical_public();
    btstack_jl_core_wake();
}

void btstack_jl_le_set_advertising(const uint8_t *data, uint8_t length)
{
    if (length > sizeof(le_adv_data)) length = sizeof(le_adv_data);
    jl_enter_critical_public();
    if (data && length) memcpy(le_adv_data, data, length);
    le_adv_len = data ? length : 0;
    le_adv_dirty = 1;
    jl_exit_critical_public();
    btstack_jl_core_wake();
}

static void le_hci_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            if (hci_subevent_le_connection_complete_get_status(packet) != 0) break;
            le_con_handle =
                hci_subevent_le_connection_complete_get_connection_handle(packet);
            {
                bd_addr_t peer;
                unsigned i;

                hci_subevent_le_connection_complete_get_peer_address(packet, peer);
                for (i = 0; i < 6; i++) le_peer[i] = peer[5 - i];
                le_peer_valid = 1;
            }
            memset(le_ccc_value, 0, sizeof(le_ccc_value));
            le_pending_len = 0;
            le_report_dirty = 0;
            btstack_jl_air_reset();
            btstack_jl_air_note_le(
                hci_subevent_le_connection_complete_get_conn_interval(packet),
                hci_subevent_le_connection_complete_get_conn_latency(packet),
                0, 0);
            /* Neither side asks for longer PDUs on its own: at the default
             * 27 bytes a Switch 2 report (70 bytes with headers) went out in
             * three connection events.  Sent directly: BTstack's connection
             * task for it (gap_le_set_data_length) never ran here. */
            le_data_length_pending = 1;
            if (!le_sm_active) {
                /* att_server holds every ATT request while
                 * gap_reconnect_security_setup_active() says an identity
                 * (IRK) lookup is pending - and without sm_init() nobody ever
                 * runs that lookup, so MTU exchange, discovery and write
                 * requests went unanswered and the console waited forever.
                 * There is no IRK to resolve here: mark it done, and say
                 * there is no bond record (sm.c would have set -1). */
                hci_connection_t *connection = hci_connection_for_handle(le_con_handle);
                if (connection) {
                    connection->sm_connection.sm_irk_lookup_state = IRK_LOOKUP_FAILED;
                    connection->sm_connection.sm_le_db_index = -1;
                }
            } else {

                sm_request_pairing(le_con_handle);
            }
            cyclone2_usb_debug("LE connected handle=%04x", le_con_handle);
            cyclone2_bringup_record(0x4c01u);
            if (le_config && le_config->state) le_config->state(1);
            break;
        case HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST: {
            if (le_sm_active) break;
            hci_con_handle_t handle =
                hci_subevent_le_long_term_key_request_get_connection_handle(packet);
            uint8_t ltk[16];

            if (le_config && le_config->get_ltk && le_config->get_ltk(ltk)) {
                /* HCI takes the key least significant byte first. */
                uint8_t reversed[16];
                unsigned i;

                for (i = 0; i < 16; i++) reversed[i] = ltk[15 - i];
                hci_send_cmd(&hci_le_long_term_key_request_reply, handle, reversed);
                cyclone2_usb_debug("LE LTK request answered");
                cyclone2_bringup_record(0x4c03u);
            } else {
                hci_send_cmd(&hci_le_long_term_key_negative_reply, handle);
                cyclone2_usb_debug("LE LTK request refused (no key)");
                cyclone2_bringup_record(0x4c04u);
            }
            break;
        }
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            if (hci_subevent_le_connection_update_complete_get_status(packet) != 0) break;
            btstack_jl_air_note_le(
                hci_subevent_le_connection_update_complete_get_conn_interval(packet),
                hci_subevent_le_connection_update_complete_get_conn_latency(packet),
                0, 0);
            cyclone2_usb_debug("LE connection interval %u x 1.25 ms, latency %u",
                hci_subevent_le_connection_update_complete_get_conn_interval(packet),
                hci_subevent_le_connection_update_complete_get_conn_latency(packet));
            break;
        case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
            btstack_jl_air_note_le_length(
                hci_subevent_le_data_length_change_get_max_tx_octets(packet));
            cyclone2_usb_debug("LE data length tx %u rx %u",
                hci_subevent_le_data_length_change_get_max_tx_octets(packet),
                hci_subevent_le_data_length_change_get_max_rx_octets(packet));
            break;
        case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
            if (hci_subevent_le_phy_update_complete_get_status(packet) != 0) break;
            btstack_jl_air_note_le(0, 0,
                hci_subevent_le_phy_update_complete_get_tx_phy(packet),
                packet[7]);   /* RX PHY: no getter in this BTstack */
            break;
        default:
            break;
        }
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        if (hci_event_encryption_change_get_connection_handle(packet) == le_con_handle) {
            cyclone2_usb_debug("LE encryption status=%u enabled=%u",
                               hci_event_encryption_change_get_status(packet),
                               hci_event_encryption_change_get_encryption_enabled(packet));
            cyclone2_bringup_record(0x4c05u);
            if (hci_event_encryption_change_get_status(packet) == 0 &&
                hci_event_encryption_change_get_encryption_enabled(packet)) {
                if (le_config && le_config->encrypted) le_config->encrypted();
                if (le_config && le_config->conn_interval_min != 0) {

                    gap_request_connection_parameter_update(
                        le_con_handle, le_config->conn_interval_min,
                        le_config->conn_interval_max, 0, 400);
                }
            }
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == le_con_handle) {
            cyclone2_usb_debug("LE disconnected reason=%02x",
                               hci_event_disconnection_complete_get_reason(packet));
            cyclone2_bringup_record(0x4c02u);
            le_con_handle = HCI_CON_HANDLE_INVALID;
            le_pending_len = 0;
            if (le_config && le_config->state) le_config->state(0);
            le_apply_advertising();
        }
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        le_try_send();
        break;
    default:
        break;
    }
}

static void le_sm_handler(uint8_t packet_type, uint16_t channel,
                          uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_JUST_WORKS_REQUEST:

        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;
    case SM_EVENT_PAIRING_COMPLETE:
        cyclone2_usb_debug("LE pairing complete status=%02x reason=%02x",
                           sm_event_pairing_complete_get_status(packet),
                           sm_event_pairing_complete_get_reason(packet));
        cyclone2_bringup_record(0x4c06u);
        break;
    case SM_EVENT_REENCRYPTION_COMPLETE:
        cyclone2_usb_debug("LE bond reused status=%02x",
                           sm_event_reencryption_complete_get_status(packet));
        cyclone2_bringup_record(0x4c07u);
        break;
    default:
        break;
    }
}

static void le_att_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type == HCI_EVENT_PACKET &&
        hci_event_packet_get_type(packet) == ATT_EVENT_CAN_SEND_NOW) {
        le_try_send();
    }
}

void btstack_jl_le_configure(const struct btstack_jl_le_config *config)
{
    le_config = config;
}

int btstack_jl_le_peer_address(uint8_t address[6])
{
    if (!le_peer_valid) return 0;
    memcpy(address, le_peer, 6);
    return 1;
}

int btstack_jl_le_connected(void)
{
    return le_con_handle != HCI_CON_HANDLE_INVALID;
}

void btstack_jl_le_disconnect(void)
{
    le_stopping = 1;
    le_adv_dirty = 1;
    le_disconnect_requested = 1;
    btstack_jl_core_wake();
}

void btstack_jl_le_setup(void)
{
    le_con_handle = HCI_CON_HANDLE_INVALID;
    le_working = 0;
    le_pending_len = 0;
    le_report_dirty = 0;
    le_disconnect_requested = 0;
    le_stopping = 0;
    memset(le_ccc_handle, 0, sizeof(le_ccc_handle));
    if (le_config == NULL || le_config->att_db == NULL) return;
    le_initialized = 1;
    le_device_db_init();
    le_sm_active = le_config->sm_er != NULL;
    if (le_sm_active) {
        sm_key_t key;

        sm_init();
        memcpy(key, le_config->sm_er, 16);
        sm_set_er(key);
        memcpy(key, le_config->sm_ir, 16);
        sm_set_ir(key);
        sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
        le_sm_registration.callback = &le_sm_handler;
        sm_add_event_handler(&le_sm_registration);
    }
    att_server_init(le_config->att_db, &le_att_read, &le_att_write);
    le_hci_registration.callback = &le_hci_handler;
    hci_add_event_handler(&le_hci_registration);
    att_server_register_packet_handler(&le_att_handler);
}

void btstack_jl_le_working(void)
{
    if (!le_initialized) return;
    le_working = 1;
    le_apply_advertising();
}

void btstack_jl_le_poll(void)
{
    if (!le_initialized) return;
    if (le_adv_dirty) le_apply_advertising();
    if (le_disconnect_requested) {
        le_disconnect_requested = 0;
        if (le_con_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_con_handle);
    }
    if (le_data_length_pending && le_con_handle != HCI_CON_HANDLE_INVALID &&
        hci_can_send_command_packet_now()) {
        le_data_length_pending = 0;
        hci_send_cmd(&hci_le_set_data_length, le_con_handle, 251, 2120);
    }
    if (le_con_handle != HCI_CON_HANDLE_INVALID) {
        btstack_jl_air_poll_link(le_con_handle);
    }
    if (le_report_dirty && le_con_handle != HCI_CON_HANDLE_INVALID) {
        le_try_send();
    }
}

void btstack_jl_le_teardown(void)
{
    if (!le_initialized) return;
    le_initialized = 0;
    hci_remove_event_handler(&le_hci_registration);
    att_server_deinit();
    if (le_sm_active) {
        sm_remove_event_handler(&le_sm_registration);
        sm_deinit();
        /* sm_init() starts the crypto engine only once per boot, and the next
         * hci_init() drops its HCI handler: without this, every session after
         * the first waits forever on LE Encrypt / LE Rand, so a reconnect
         * never answers the host's LTK request and the link stays unencrypted. */
        btstack_crypto_deinit();
        le_sm_active = 0;
    }
    le_con_handle = HCI_CON_HANDLE_INVALID;
    le_working = 0;
}
