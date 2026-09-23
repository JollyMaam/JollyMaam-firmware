/*
 * Upstream BTstack host for the Jieli BR23 controller (btctrler.a).
 *
 * This file is the BTstack side of the port: it owns HCI, L2CAP, SDP and the
 * HID device profile and drives them from the port's run loop.  It never
 * includes Jieli SDK headers; the controller, the OS services, the clock and
 * the persistence are reached through port/jieli-br23 and btstack_jl.h.
 */

#include "btstack_config.h"

#include <string.h>

#include "bluetooth.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "classic/btstack_link_key_db.h"
#include "classic/hid_device.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "l2cap.h"

#include "hci_transport_jl_br23.h"

#include "btstack_jl.h"

#define BTSTACK_JL_MAX_QUEUED_REPORTS 24
#define BTSTACK_JL_MAX_REPORT_SIZE    64
/* HID SDP record: service attributes plus the 170 byte Switch report
 * descriptor, which upstream stores inline in the record. */
#define BTSTACK_JL_SDP_RECORD_SIZE    360
#define BTSTACK_JL_HID_DESCRIPTOR_MAX 240

typedef struct {
    uint16_t size;
    uint8_t  data[BTSTACK_JL_MAX_REPORT_SIZE];
} btstack_jl_report_t;

static btstack_jl_config_t jl_config;
static btstack_jl_event_handler_t jl_event_handler;

static void jl_address_to_jieli(const uint8_t in[6], uint8_t out[6]);
static void jl_trace(uint32_t code);
static void jl_apply_scan_state(void);
static void jl_apply_local_address(void);

static uint8_t jl_address[6];
static uint8_t jl_address_valid;
/* The controller and the Jieli SDK keep device addresses in wire order (least
 * significant byte first).  BTstack's bd_addr_t is the human readable order and
 * reverses at the HCI boundary, so every address that crosses btstack_jl.h is
 * converted here. */
static uint8_t jl_address_jieli[6];
static uint8_t jl_peer_address[6];
static uint8_t jl_peer_valid;
static uint8_t jl_connectable;
static uint8_t jl_discoverable;
static uint8_t jl_working;
static uint8_t jl_shutdown_requested;
static uint8_t jl_send_pending;

/* Requests queued by the application task and executed on the run-loop task. */
enum {
    JL_REQUEST_SCAN       = 1 << 0,
    JL_REQUEST_CONNECT    = 1 << 1,
    JL_REQUEST_DISCONNECT = 1 << 2,
    JL_REQUEST_ADDRESS    = 1 << 3,
    JL_REQUEST_SHUTDOWN   = 1 << 4,
};

static volatile uint8_t jl_request;
/* Bring-up diagnostics for the report path. */
static uint32_t jl_tx_sent;
static uint32_t jl_tx_stall_since;
static uint8_t  jl_tx_stall_reported;
static volatile uint8_t jl_start_requested;
static volatile uint8_t jl_idle = 1;
/* 1 while a session is set up and the run loop is installed.  The application
 * can ask for a shutdown at any time; this says whether there is a live session
 * to stop (see btstack_jl_core_request_shutdown). */
static volatile uint8_t jl_run_loop_up;
static uint8_t jl_request_address[6];

static uint16_t jl_hid_cid;
static uint16_t jl_con_handle;
static uint8_t  jl_con_handle_valid;
static btstack_timer_source_t jl_credit_timer;

/* Bring-up diagnostics: how many ACL buffers BTstack believes it may use. */
static void jl_credit_timer_handler(btstack_timer_source_t *ts)
{
    if (jl_con_handle_valid) {
        jl_trace(0x600u | hci_number_free_acl_slots_for_handle(jl_con_handle));
    }
    btstack_run_loop_set_timer(ts, 300);
    btstack_run_loop_add_timer(ts);
}

static btstack_jl_report_t jl_reports[BTSTACK_JL_MAX_QUEUED_REPORTS];
static volatile uint16_t jl_report_read;
static volatile uint16_t jl_report_write;
static volatile uint16_t jl_report_count;

static btstack_packet_callback_registration_t jl_hci_event_registration;
static btstack_packet_callback_registration_t jl_l2cap_event_registration;
static btstack_data_source_t jl_tx_data_source;
static uint8_t jl_sdp_service_buffer[BTSTACK_JL_SDP_RECORD_SIZE];
static uint8_t jl_hid_descriptor[BTSTACK_JL_HID_DESCRIPTOR_MAX];
static uint16_t jl_hid_descriptor_size;

static void jl_notify_ex(uint8_t type, uint8_t code, uint8_t status,
                         const uint8_t *address)
{
    btstack_jl_event_t event;

    event.type = type;
    event.code = code;
    event.status = status;
    if (address != 0) {
        /* The application expects the Jieli/wire order. */
        jl_address_to_jieli(address, event.address);
    } else {
        memset(event.address, 0, sizeof(event.address));
    }

    if (jl_event_handler != 0) {
        jl_event_handler(&event);
    }
}

static void jl_notify(uint8_t type, uint8_t status, const uint8_t *address)
{
    jl_notify_ex(type, 0, status, address);
}

/* Application-level events land in the bring-up log as 0x500 | event so a
 * session can be read back from the console after the fact. */
static void jl_notify_logged(uint8_t type, uint8_t status, const uint8_t *address)
{
    if (jl_config.trace != 0) {
        jl_config.trace(0x500u | type);
    }
    jl_notify_ex(type, 0, status, address);
}

static void jl_enter_critical(void)
{
    if (jl_config.critical_enter != 0) {
        jl_config.critical_enter();
    }
}

static void jl_exit_critical(void)
{
    if (jl_config.critical_exit != 0) {
        jl_config.critical_exit();
    }
}

static void jl_trace(uint32_t code)
{
    if (jl_config.trace != 0) {
        jl_config.trace(code);
    }
}

/* btstack order (MSB first) <-> Jieli/wire order (LSB first). */
static void jl_address_to_jieli(const uint8_t in[6], uint8_t out[6])
{
    uint8_t i;
    for (i = 0; i < 6; i++) {
        out[i] = in[5 - i];
    }
}

/* Overrides the L2CAP instrumentation hook (see l2cap.c). */
void btstack_l2cap_trace(uint32_t code)
{
    jl_trace(0x2000u + code);
}

/* Overrides the port's weak bring-up trace hook. */
void jieli_btctrler_trace(uint32_t code)
{
    jl_trace(0x100u + code);
}

/* --------------------------------------------------------------- link keys */

static void jl_link_key_db_open(void)
{
}

static void jl_link_key_db_close(void)
{
}

/* The controller can change its own address (the application picks a
 * different one per slot), so remember which controller the stored keys
 * belong to. */
static uint8_t jl_link_key_db_local_addr[6];
static uint8_t jl_link_key_db_local_addr_valid;

static void jl_link_key_db_set_local_bd_addr(bd_addr_t bd_addr)
{
    memcpy(jl_link_key_db_local_addr, bd_addr, sizeof(jl_link_key_db_local_addr));
    jl_link_key_db_local_addr_valid = 1;
}

static int jl_link_key_db_get(bd_addr_t bd_addr, link_key_t link_key, link_key_type_t *type)
{
    uint8_t key_type = 0;
    uint8_t jieli_addr[6];

    jl_address_to_jieli(bd_addr, jieli_addr);
    if ((jl_config.link_key_get == 0) ||
        (jl_config.link_key_get(jieli_addr, link_key, &key_type) == 0)) {
        /* Bring-up log: 0x2602 = no key for this peer, 0x2601 = found. */
        jl_trace(0x2602u);
        return 0;
    }
    jl_trace(0x2601u);
    *type = (link_key_type_t) key_type;
    return 1;
}

static void jl_link_key_db_put(bd_addr_t bd_addr, link_key_t link_key, link_key_type_t type)
{
    uint8_t jieli_addr[6];

    if (jl_config.link_key_put != 0) {
        jl_address_to_jieli(bd_addr, jieli_addr);
        jl_config.link_key_put(jieli_addr, link_key, (uint8_t) type);
        jl_trace(0x2701u);
    }
}

static void jl_link_key_db_delete(bd_addr_t bd_addr)
{
    uint8_t jieli_addr[6];

    if (jl_config.link_key_delete != 0) {
        jl_address_to_jieli(bd_addr, jieli_addr);
        jl_config.link_key_delete(jieli_addr);
    }
}

static uint8_t jl_iterator_index;

static int jl_link_key_db_iterator_init(btstack_link_key_iterator_t *it)
{
    (void) it;
    jl_iterator_index = 0;
    return jl_config.link_key_iterator != 0;
}

static int jl_link_key_db_iterator_get_next(btstack_link_key_iterator_t *it,
                                            bd_addr_t bd_addr, link_key_t link_key,
                                            link_key_type_t *type)
{
    uint8_t key_type = 0;
    uint8_t index = jl_iterator_index;
    uint8_t jieli_addr[6];

    (void) it;

    if (jl_config.link_key_iterator == 0) {
        return 0;
    }
    if (jl_config.link_key_iterator(index, jieli_addr, link_key, &key_type) == 0) {
        return 0;
    }
    jl_address_to_jieli(jieli_addr, bd_addr);
    jl_iterator_index = (uint8_t)(index + 1);
    *type = (link_key_type_t) key_type;
    return 1;
}

static void jl_link_key_db_iterator_done(btstack_link_key_iterator_t *it)
{
    (void) it;
}

static const btstack_link_key_db_t jl_link_key_db = {
    .open = &jl_link_key_db_open,
    .set_local_bd_addr = &jl_link_key_db_set_local_bd_addr,
    .close = &jl_link_key_db_close,
    .get_link_key = &jl_link_key_db_get,
    .put_link_key = &jl_link_key_db_put,
    .delete_link_key = &jl_link_key_db_delete,
    .iterator_init = &jl_link_key_db_iterator_init,
    .iterator_get_next = &jl_link_key_db_iterator_get_next,
    .iterator_done = &jl_link_key_db_iterator_done,
};

/* --------------------------------------------------------------- TX queue */

/* The application queues a report from its own task, the run loop sends it.
 * os critical sections on the SDK side guard the indices. */
static int jl_report_enqueue(const uint8_t *message, uint16_t size)
{
    btstack_jl_report_t *slot;
    int result = 0;

    if (size > BTSTACK_JL_MAX_REPORT_SIZE) {
        return 2;
    }

    jl_enter_critical();
    if ((jl_hid_cid == 0) || !jl_working) {
        result = 1;
    } else if (jl_report_count >= BTSTACK_JL_MAX_QUEUED_REPORTS) {
        result = 3;
    } else {
        slot = &jl_reports[jl_report_write];
        slot->size = size;
        memcpy(slot->data, message, size);
        jl_report_write = (uint16_t)((jl_report_write + 1) % BTSTACK_JL_MAX_QUEUED_REPORTS);
        jl_report_count++;
    }
    jl_exit_critical();
    return result;
}

static void jl_report_dequeue(btstack_jl_report_t *out)
{
    jl_enter_critical();
    if (jl_report_count != 0) {
        *out = jl_reports[jl_report_read];
        jl_report_read = (uint16_t)((jl_report_read + 1) % BTSTACK_JL_MAX_QUEUED_REPORTS);
        jl_report_count--;
    } else {
        out->size = 0;
    }
    jl_exit_critical();
}

static void jl_tx_poll(btstack_data_source_t *data_source,
                       btstack_data_source_callback_type_t callback_type)
{
    uint8_t address[6];
    uint8_t request;

    (void) data_source;
    (void) callback_type;

    /* Requests from the application task are executed here, in the run-loop
     * task: BTstack is not thread safe, so the application must never call
     * into it directly. */
    jl_enter_critical();
    request = jl_request;
    jl_request = 0;
    memcpy(address, jl_request_address, sizeof(address));
    jl_exit_critical();

    if ((request & JL_REQUEST_SCAN) != 0) {
        jl_apply_scan_state();
    }
    if ((request & JL_REQUEST_ADDRESS) != 0) {
        /* address holds the wire (Jieli) order the application asked for. */
        memcpy(jl_address_jieli, address, sizeof(jl_address_jieli));
        jl_address_to_jieli(jl_address_jieli, jl_address);
        jl_address_valid = 1;
        jl_apply_local_address();
    }
    if ((request & JL_REQUEST_DISCONNECT) != 0) {
        if (jl_hid_cid != 0) {
            hid_device_disconnect(jl_hid_cid);
            jl_hid_cid = 0;
            jl_send_pending = 0;
        } else if (jl_peer_valid) {
            hci_connection_t *connection =
                hci_connection_for_bd_addr_and_type(jl_peer_address, BD_ADDR_TYPE_ACL);
            if (connection != 0) {
                gap_disconnect(connection->con_handle);
            }
        }
    }
    if ((request & JL_REQUEST_CONNECT) != 0) {
        bd_addr_t peer;
        uint16_t hid_cid = 0;

        if (jl_hid_cid == 0) {
            jl_address_to_jieli(address, peer);
            memcpy(jl_peer_address, peer, sizeof(jl_peer_address));
            jl_peer_valid = 1;
            hid_device_connect(peer, &hid_cid);
        }
    }

    if ((request & JL_REQUEST_SHUTDOWN) != 0) {
        btstack_jl_core_power_off();
        return;
    }

    if (!jl_send_pending && (jl_report_count != 0) && (jl_hid_cid != 0)) {
        jl_send_pending = 1;
        /* A report that cannot be handed over within a second means the link
         * is not draining: record it once with the free-slot count. */
        if (jl_tx_stall_since == 0) {
            jl_tx_stall_since = jl_config.time_ms();
        } else if ((jl_tx_stall_reported == 0) &&
                   (jl_config.time_ms() - jl_tx_stall_since > 1000)) {
            jl_tx_stall_reported = 1;
            jl_trace(0x2400u | hci_number_free_acl_slots_for_handle(jl_con_handle));
        }
        hid_device_request_can_send_now_event(jl_hid_cid);
    }
}

/* --------------------------------------------------------- local state */

static void jl_apply_scan_state(void)
{
    if (!jl_working) {
        return;
    }
    gap_connectable_control(jl_connectable);
    gap_discoverable_control(jl_discoverable);
}

static void jl_apply_local_address(void)
{
    /* HCI Reset restores the controller's factory address, so reapply ours
     * before the host reads it back.  The controller wants the wire order,
     * BTstack wants the human readable one. */
    jieli_btctrler_set_bd_addr(jl_address_jieli);
    hci_set_bd_addr(jl_address);
}

static void jl_peer_from_handle(uint16_t handle, uint8_t out[6])
{
    hci_connection_t *connection = hci_connection_for_handle(handle);

    if (connection != 0) {
        memcpy(out, connection->address, 6);
    } else {
        memcpy(out, jl_peer_address, 6);
    }
}

/* ------------------------------------------------------- packet handler */

static void jl_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    bd_addr_t event_addr;
    uint8_t status;

    (void) channel;
    (void) size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)) {
                case HCI_STATE_WORKING:
                    jl_working = 1;
                    if (jl_address_valid) {
                        jl_apply_local_address();
                    }
                    jl_apply_scan_state();
                    jl_notify_logged(BTSTACK_JL_EVENT_READY, 0, 0);
                    break;
                case HCI_STATE_OFF:
                    jl_working = 0;
                    /* The halt that ends a radio session has finished: the
                     * transport is closed and the controller is powered down.
                     * Only now may the run loop stop so the stack can be torn
                     * down without losing the power-off command (see
                     * btstack_jl_core_request_shutdown()). */
                    if (jl_shutdown_requested) {
                        jl_shutdown_requested = 0;
                        btstack_run_loop_trigger_exit();
                    }
                    break;
                default:
                    break;
            }
            break;

        case HCI_EVENT_CONNECTION_COMPLETE:
            status = hci_event_connection_complete_get_status(packet);
            jl_con_handle = hci_event_connection_complete_get_connection_handle(packet);
            jl_con_handle_valid = 1;
            hci_event_connection_complete_get_bd_addr(packet, event_addr);
            memcpy(jl_peer_address, event_addr, sizeof(jl_peer_address));
            jl_peer_valid = 1;
            jl_notify_ex(BTSTACK_JL_EVENT_HCI_STATUS, HCI_EVENT_CONNECTION_COMPLETE,
                         status, event_addr);
            if (status == ERROR_CODE_SUCCESS) {
                /* The HID services require an encrypted link, and BTstack only
                 * answers an incoming L2CAP connection request once it knows
                 * the peer's supported features.  Without this query the
                 * connection response for the HID channels stays pending and
                 * the console gives up without ever seeing the channels. */
                hci_remote_features_query(jl_con_handle);
                jl_notify_logged(BTSTACK_JL_EVENT_CONNECTED, 0, event_addr);
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            status = hci_event_disconnection_complete_get_reason(packet);
            /* Always record the reason: the console detaching the link was the
             * hardest failure to see. */
            jl_trace(0x2a00u | status);
            jl_peer_from_handle(
                hci_event_disconnection_complete_get_connection_handle(packet),
                event_addr);
            jl_hid_cid = 0;
            jl_send_pending = 0;
            jl_notify_ex(BTSTACK_JL_EVENT_HCI_STATUS, HCI_EVENT_DISCONNECTION_COMPLETE,
                         status, event_addr);
            jl_notify_logged(BTSTACK_JL_EVENT_DISCONNECTED, status, event_addr);
            break;

        case HCI_EVENT_LINK_KEY_REQUEST:
            /* The console is asking us for the key of a peer we should know. */
            jl_trace(0x2801u);
            break;

        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            jl_trace(0x2901u);
            break;

        case HCI_EVENT_ENCRYPTION_CHANGE:
            if (hci_event_encryption_change_get_encryption_enabled(packet)) {
                jl_peer_from_handle(
                    hci_event_encryption_change_get_connection_handle(packet),
                    event_addr);
                jl_notify_logged(BTSTACK_JL_EVENT_ENCRYPTED, 0, event_addr);
            } else {
                jl_notify_ex(BTSTACK_JL_EVENT_HCI_STATUS, HCI_EVENT_ENCRYPTION_CHANGE,
                             0, jl_peer_address);
            }
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_CONNECTION_OPENED:
                    status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        jl_hid_cid = 0;
                        jl_notify_logged(BTSTACK_JL_EVENT_HID_CLOSED, status, jl_peer_address);
                        break;
                    }
                    jl_hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    /* 0x2b00|1: the HID interrupt channel is open, which is
                     * what the profile counts as "connected". */
                    jl_trace(0x2b00u | (jl_hid_cid != 0 ? 1u : 0u));
                    hid_subevent_connection_opened_get_bd_addr(packet, event_addr);
                    memcpy(jl_peer_address, event_addr, sizeof(jl_peer_address));
                    jl_peer_valid = 1;
                    jl_notify_logged(BTSTACK_JL_EVENT_HID_OPENED, 0, event_addr);
                    break;

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    jl_hid_cid = 0;
                    jl_send_pending = 0;
                    jl_notify_logged(BTSTACK_JL_EVENT_HID_CLOSED, 0, jl_peer_address);
                    break;

                case HID_SUBEVENT_CAN_SEND_NOW: {
                    btstack_jl_report_t report;
                    jl_report_dequeue(&report);
                    if (report.size != 0) {
                        hid_device_send_interrupt_message(jl_hid_cid, report.data,
                                                          report.size);
                        jl_tx_sent++;
                        if ((jl_tx_sent & 0x0fu) == 0u) {
                            jl_trace(0x2300u | ((jl_tx_sent >> 4) & 0xffu));
                        }
                    }
                    jl_send_pending = 0;
                    jl_tx_stall_since = 0;
                    jl_tx_stall_reported = 0;
                    break;
                }

                default:
                    break;
            }
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------- HID callbacks */

/* Output reports from the host (rumble + subcommands on the interrupt channel,
 * SET_REPORT on the control channel) have to reach the application, otherwise
 * the console's HID handshake never completes. */
static void jl_hid_report_data(uint16_t cid, hid_report_type_t report_type,
                               uint16_t report_id, int report_size,
                               uint8_t *report)
{
    uint8_t packet[BTSTACK_JL_MAX_REPORT_SIZE];

    (void) cid;
    if (jl_config.hid_output == 0) {
        return;
    }
    if ((unsigned) report_size + 2u > sizeof(packet)) {
        return;
    }
    packet[0] = (uint8_t)(0xa0u | (uint8_t) report_type);
    packet[1] = (uint8_t) report_id;
    if (report_size > 0) {
        memcpy(&packet[2], report, (size_t) report_size);
    }
    /* Bring-up log: report id, and for a rumble+subcommand report the
     * subcommand id and its first argument (the console asks the controller to
     * stop/start sending input reports this way). */
    jl_trace(0x2000u | report_id);
    if ((report_id == 0x01U) && (report_size >= 10)) {
        jl_trace(0x2100u | packet[11]);
        jl_trace(0x2200u | packet[12]);
    }
    jl_config.hid_output(packet, (uint16_t)(report_size + 2));
}

static void jl_hid_set_report(uint16_t cid, hid_report_type_t report_type,
                              int report_size, uint8_t *report)
{
    uint8_t packet[BTSTACK_JL_MAX_REPORT_SIZE];

    (void) cid;
    if (jl_config.hid_output == 0) {
        return;
    }
    /* report already carries the report id (if the descriptor declares one). */
    if ((unsigned) report_size + 1u > sizeof(packet)) {
        return;
    }
    packet[0] = (uint8_t)(0x50u | (uint8_t) report_type);
    memcpy(&packet[1], report, (size_t) report_size);
    jl_config.hid_output(packet, (uint16_t)(report_size + 1));
}

/* GET_REPORT: the only request that matters here is the report descriptor
 * (feature report 0), which hosts ask for when they did not read it from SDP. */
static int jl_hid_report_request(uint16_t cid, hid_report_type_t report_type,
                                 uint16_t report_id, int *out_report_size,
                                 uint8_t *out_report)
{
    (void) cid;

    if ((report_type == HID_REPORT_TYPE_FEATURE) && (report_id == 0) &&
        (jl_hid_descriptor_size != 0)) {
        memcpy(out_report, jl_hid_descriptor, jl_hid_descriptor_size);
        *out_report_size = jl_hid_descriptor_size;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ setup/exit */

/* Bring-up diagnostics: record what L2CAP does, with the PSM / status in the
 * low bits, as 0x900 | value entries in the bring-up log. */
static void jl_l2cap_event_handler(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size)
{
    uint8_t event;

    (void) channel;
    (void) size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    event = hci_event_packet_get_type(packet);
    switch (event) {
        case L2CAP_EVENT_INCOMING_CONNECTION:
            jl_trace(0x900u | l2cap_event_incoming_connection_get_psm(packet));
            break;
        case L2CAP_EVENT_CHANNEL_OPENED:
            jl_trace(0x910u | l2cap_event_channel_opened_get_status(packet));
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            jl_trace(0x920u);
            break;
        default:
            jl_trace(0x930u | event);
            break;
    }
}

int btstack_jl_core_init(const btstack_jl_config_t *config,
                         btstack_jl_event_handler_t handler)
{
    if (config == 0) {
        return -1;
    }
    jl_config = *config;
    jl_event_handler = handler;
    return 0;
}

/* Runs on the "btstack" task: the application task that calls btstack_init()
 * has a ~640 byte stack, far too small for SDP/L2CAP/HID initialization. */
static int btstack_jl_core_setup(void)
{
    hid_sdp_record_t hid_params;
    hci_transport_config_jl_br23_t transport_config;

    if (jl_event_handler == 0) {
        return -1;
    }

    if (jl_config.bd_addr != 0) {
        memcpy(jl_address_jieli, jl_config.bd_addr, sizeof(jl_address_jieli));
        jl_address_to_jieli(jl_address_jieli, jl_address);
        jl_address_valid = 1;
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_jl_br23_get_instance());

    jl_trace(11u);
    memset(&transport_config, 0, sizeof(transport_config));
    if (jl_address_valid) {
        transport_config.bd_addr = jl_address_jieli;
    }
    hci_init(hci_transport_jl_br23_instance(), &transport_config);
    hci_set_link_key_db(&jl_link_key_db);

    l2cap_init();
    jl_l2cap_event_registration.callback = &jl_l2cap_event_handler;
    l2cap_add_event_handler(&jl_l2cap_event_registration);
    jl_trace(12u);
    sdp_init();
    jl_trace(13u);

    if (jl_config.local_name != 0) {
        gap_set_local_name(jl_config.local_name);
    }
    gap_set_class_of_device(jl_config.class_of_device);
    /* A console becomes master and may pull the link into sniff mode. */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    /* No L2CAP-level security requirement: the console drives authentication
     * and encryption itself before it opens the HID channels.  BTstack's
     * default LEVEL_2 would otherwise run its Security Mode 4 rule, which
     * disconnects the link when the controller does not report the encryption
     * key size, and the HID channels never come up.  Set before the HID
     * services are registered so their required security level is LEVEL_0. */
    gap_set_security_level(LEVEL_0);

    /* SSP: no input, no output, general bonding.  btstack answers the IO
     * capability request itself from these settings. */
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(jl_config.io_capability);
    gap_ssp_set_authentication_requirement(jl_config.authentication_requirement);
    gap_ssp_set_auto_accept(1);

    memset(&hid_params, 0, sizeof(hid_params));
    hid_params.hid_device_subclass = (uint16_t) jl_config.class_of_device;
    hid_params.hid_country_code = 0;
    hid_params.hid_virtual_cable = 0;
    hid_params.hid_remote_wake = 1;
    hid_params.hid_reconnect_initiate = 1;
    hid_params.hid_normally_connectable = true;
    hid_params.hid_boot_device = false;
    hid_params.hid_ssr_host_max_latency = 0;
    hid_params.hid_ssr_host_min_timeout = 0;
    hid_params.hid_supervision_timeout = jl_config.supervision_timeout_ms;
    hid_params.hid_descriptor = jl_config.report_descriptor;
    hid_params.hid_descriptor_size = jl_config.report_descriptor_size;
    hid_params.device_name = jl_config.local_name;

    memset(jl_sdp_service_buffer, 0, sizeof(jl_sdp_service_buffer));
    hid_create_sdp_record(jl_sdp_service_buffer, sdp_create_service_record_handle(),
                          &hid_params);
    jl_trace(14u);
    sdp_register_service(jl_sdp_service_buffer);
    jl_trace(15u);

    jl_hid_descriptor_size = jl_config.report_descriptor_size;
    if (jl_hid_descriptor_size > sizeof(jl_hid_descriptor)) {
        jl_hid_descriptor_size = sizeof(jl_hid_descriptor);
    }
    memcpy(jl_hid_descriptor, jl_config.report_descriptor, jl_hid_descriptor_size);
    hid_device_init(false, jl_config.report_descriptor_size,
                    jl_config.report_descriptor);
    hid_device_register_report_data_callback(&jl_hid_report_data);
    hid_device_register_set_report_callback(&jl_hid_set_report);
    hid_device_register_report_request_callback(&jl_hid_report_request);
    jl_trace(16u);
    hid_device_register_packet_handler(&jl_packet_handler);

    jl_hci_event_registration.callback = &jl_packet_handler;
    hci_add_event_handler(&jl_hci_event_registration);

    /* Poll the report queue from the run loop; the application queues from
     * its own task and never touches BTstack structures directly. */
    btstack_run_loop_set_timer_handler(&jl_credit_timer, &jl_credit_timer_handler);
    btstack_run_loop_set_timer(&jl_credit_timer, 500);
    btstack_run_loop_add_timer(&jl_credit_timer);

    btstack_run_loop_set_data_source_handler(&jl_tx_data_source, &jl_tx_poll);
    btstack_run_loop_enable_data_source_callbacks(&jl_tx_data_source,
                                                  DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&jl_tx_data_source);

    jl_report_read = 0;
    jl_report_write = 0;
    jl_report_count = 0;
    jl_hid_cid = 0;
    jl_send_pending = 0;
    jl_working = 0;
    jl_shutdown_requested = 0;

    jl_trace(17u);
    return 0;
}

void btstack_jl_core_request_shutdown(void)
{
    /* Nothing to stop when the session is already parked.  The run loop has
     * been deinitialised by then, so waking it here would follow a null
     * pointer and reset the chip -- exactly the crash a profile switch used to
     * hit when it re-exited an already-stopped profile. */
    if (!jl_run_loop_up) {
        return;
    }
    jl_enter_critical();
    jl_request |= JL_REQUEST_SHUTDOWN;
    jl_exit_critical();
    /* Wake the run loop so the request is handled on the BTstack task.  The
     * exit is requested from there once the controller reports HCI_STATE_OFF.
     * Triggering the exit here instead would race the halt and could stop the
     * run loop before the transport (and the controller task) is closed. */
    btstack_run_loop_poll_data_sources_from_irq();
}

uint8_t btstack_jl_core_idle(void);

void btstack_jl_core_power_off(void)
{
    jl_shutdown_requested = 1;
    jl_working = 0;
    jl_hid_cid = 0;
    jl_send_pending = 0;
    jl_report_count = 0;
    hci_power_control(HCI_POWER_OFF);
}

void btstack_jl_core_set_scan(uint8_t connectable, uint8_t discoverable)
{
    jl_enter_critical();
    jl_connectable = connectable ? 1 : 0;
    jl_discoverable = discoverable ? 1 : 0;
    jl_request |= JL_REQUEST_SCAN;
    jl_exit_critical();
}

void btstack_jl_core_set_address(const uint8_t address[6])
{
    if (address == 0) {
        return;
    }
    /* Keep the controller in sync right away (it is not BTstack state), and
     * let the run loop update the host's copy. */
    jieli_btctrler_set_bd_addr(address);
    jl_enter_critical();
    memcpy(jl_request_address, address, sizeof(jl_request_address));
    jl_request |= JL_REQUEST_ADDRESS;
    jl_exit_critical();
}

const uint8_t *btstack_jl_core_get_address(void)
{
    return jl_address_jieli;
}

int btstack_jl_core_hid_connect(const uint8_t address[6])
{
    if (address == 0) {
        return 1;
    }
    jl_enter_critical();
    memcpy(jl_request_address, address, sizeof(jl_request_address));
    jl_request |= JL_REQUEST_CONNECT;
    jl_exit_critical();
    return 0;
}

int btstack_jl_core_disconnect(void)
{
    jl_enter_critical();
    jl_request |= JL_REQUEST_DISCONNECT;
    jl_exit_critical();
    return 0;
}

int btstack_jl_core_hid_connected(void)
{
    return jl_hid_cid != 0;
}

int btstack_jl_core_send_report(const uint8_t *message, uint16_t size)
{
    return jl_report_enqueue(message, size);
}

void btstack_jl_core_delete_all_link_keys(void)
{
    bd_addr_t all_zero;

    /* The HCI stack is torn down between radio sessions; a forget request that
     * arrives while it is parked has nothing to talk to. */
    if (!jl_working) {
        return;
    }
    memset(all_zero, 0, sizeof(all_zero));
    hci_send_cmd(&hci_delete_stored_link_key, all_zero, 1);
}

void btstack_jl_core_delete_link_key(const uint8_t address[6])
{
    bd_addr_t peer;

    if (address == 0 || !jl_working) {
        return;
    }
    jl_address_to_jieli(address, peer);
    hci_send_cmd(&hci_delete_stored_link_key, peer, 1);
}

void btstack_jl_core_request_start(void)
{
    jl_start_requested = 1;
}

/* Last-resort teardown: end the session even if the controller never reported
 * HCI_STATE_OFF.  The controller may then still be powered up, but the host
 * session ends instead of leaving the next start queued behind a live one. */
void btstack_jl_core_force_shutdown(void)
{
    if (!jl_run_loop_up) {
        return;
    }
    /* Kill the link before ending the run loop.  The halt that the run loop is
     * sitting in waits for the peer to confirm the disconnect, which a dead
     * peer never does; closing the controller here means the next session
     * opens a fresh one rather than inheriting a half-shut transport. */
    hci_transport_jl_br23_drop_link();
    btstack_run_loop_trigger_exit();
}

/* Release the BTstack singletons when a radio session ends.
 *
 * Upstream BTstack expects every protocol to be deinitialised before the next
 * init ("the deinit functions of all protocols/profiles have to be called
 * before a new init").  Skipping this is not just a leak: btstack_run_loop_init()
 * asserts that no run loop is installed yet and l2cap_init() would relink the
 * fixed channels it already linked, so the second session of a profile switch
 * used to trip the assert and park the BTstack task for good. */
static void btstack_jl_core_teardown(void)
{
    hid_device_deinit();
    sdp_deinit();
    l2cap_deinit();
    hci_deinit();
    btstack_run_loop_deinit();
}

void btstack_jl_core_run(void *argument)
{
    (void) argument;

    /* The task stays alive for the whole boot: a profile switch or a
     * disconnect powers the controller down and parks here instead of
     * deleting the task, which would block on a task that no longer services
     * its queue. */
    for (;;) {
        if (!jl_start_requested) {
            jieli_btctrler_yield(2);
            continue;
        }
        jl_start_requested = 0;
        jl_idle = 0;
        jl_request = 0;
        jl_shutdown_requested = 0;

        if (btstack_jl_core_setup() != 0) {
            jl_trace(1u);
            jl_idle = 1;
            continue;
        }
        jl_run_loop_up = 1;
        jl_trace(18u);
        hci_power_control(HCI_POWER_ON);
        jl_trace(30u);
        btstack_run_loop_execute();
        jl_trace(31u);
        jl_run_loop_up = 0;
        /* The session is over (shutdown reached HCI_STATE_OFF): release
         * BTstack so the next session can initialise cleanly.  This runs in
         * place, without a chip reset. */
        btstack_jl_core_teardown();
        jl_working = 0;
        jl_idle = 1;
    }
}

uint8_t btstack_jl_core_idle(void)
{
    return jl_idle;
}
