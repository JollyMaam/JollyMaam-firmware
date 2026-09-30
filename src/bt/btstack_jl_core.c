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
#include "classic/device_id_server.h"
#include "btstack_hid_parser.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "l2cap.h"

#include "hci_transport_jl_br23.h"

#include "btstack_jl.h"
#include "btstack_jl_le.h"

#define BTSTACK_JL_MAX_QUEUED_REPORTS 24

#define BTSTACK_JL_MAX_REPORT_SIZE    80

/* ACL packets the controller may hold for the link at once.  It offers ten
 * buffers; a report sitting in one waits behind the others and cannot be
 * replaced by a newer one, so keep the queue here, in the input slot. */
#ifndef BTSTACK_JL_MAX_IN_FLIGHT
#define BTSTACK_JL_MAX_IN_FLIGHT      2
#endif

/* Classic ACL packet types for the connections we page, positive logic as
 * hci_enable_acl_packet_types() takes them: single-slot only (DM1, DH1,
 * 2-DH1, 3-DH1).  The stock firmware pages with exactly this set (HCI packet
 * type 0x3318).  A report then never rides a three- or five-slot packet,
 * which is longer on air, fails more often under interference and costs more
 * when it has to be sent again.  The controller answers Change Connection
 * Packet Type with Unknown HCI Command, so a host's own page keeps its set. */
#define BTSTACK_JL_ACL_PACKET_TYPES   0x001eu

#define BTSTACK_JL_RSSI_PERIOD_MS     1000u
#define BTSTACK_JL_STALL_US           50000u

#define BTSTACK_JL_SDP_RECORD_SIZE    720

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

extern int le_controller_set_mac(const uint8_t *address);

static uint8_t jl_address[6];
static uint8_t jl_address_valid;

static uint8_t jl_address_jieli[6];
static uint8_t jl_peer_address[6];
static uint8_t jl_peer_valid;
static uint8_t jl_connectable;
static uint8_t jl_discoverable;
static uint8_t jl_working;
static uint8_t jl_shutdown_requested;
/* A CAN_SEND_NOW request is outstanding; the application task reads it. */
static volatile uint8_t jl_send_pending;
/* The link is in sniff (HCI Mode Change); the application task reads it. */
static volatile uint8_t jl_link_sniff;

enum {
    JL_REQUEST_SCAN       = 1 << 0,
    JL_REQUEST_CONNECT    = 1 << 1,
    JL_REQUEST_DISCONNECT = 1 << 2,
    JL_REQUEST_ADDRESS    = 1 << 3,
    JL_REQUEST_SHUTDOWN   = 1 << 4,
    JL_REQUEST_UNSNIFF    = 1 << 5,
    JL_REQUEST_LE_ADV     = 1 << 6,
};

static volatile uint8_t jl_request;

static uint16_t jl_pairings;

static uint32_t jl_unsniff_next_ms;
/* Ask for a short poll interval once the HID channel is up (see jl_tx_poll). */
static volatile uint8_t jl_qos_pending;
static uint32_t jl_rssi_next_ms;

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

static volatile uint8_t jl_acl_up;
static btstack_timer_source_t jl_credit_timer;

static void jl_credit_timer_handler(btstack_timer_source_t *ts)
{
    if (jl_con_handle_valid) {

        if (hci_connection_for_handle(jl_con_handle) == 0) {
            jl_con_handle_valid = 0;
        } else if (hci_number_free_acl_slots_for_handle(jl_con_handle) == 0) {
            jl_trace(0x600u);
        }
    }
    btstack_run_loop_set_timer(ts, 300);
    btstack_run_loop_add_timer(ts);
}

static btstack_jl_report_t jl_reports[BTSTACK_JL_MAX_QUEUED_REPORTS];
static volatile uint16_t jl_report_read;
static volatile uint16_t jl_report_write;
static volatile uint16_t jl_report_count;

/* The newest input report, apart from the queue: a full-state report that
 * waits behind another is only input lag, so a new one replaces it. */
static btstack_jl_report_t jl_input;
static volatile uint8_t jl_input_pending;
static uint32_t jl_input_posted_us;
static uint32_t jl_input_seq;               /* of the report in jl_input */

/* Report statistics and sequence numbers, shared by the Classic input slot
 * and the LE one (btstack_jl_le.c). */
static btstack_jl_input_stats_t jl_input_stats;
static volatile uint32_t jl_input_posted_seq;
static volatile uint32_t jl_input_taken_seq;
static uint64_t jl_input_wait_total_us;
static uint64_t jl_input_hold_total_us;

/* Packets handed to the controller on one connection, waiting for their
 * Number Of Completed Packets (run loop only).  A packet is complete once the
 * connection's completion count reaches `target`. */
#define JL_HOLD_RING 8
static struct {
    uint16_t handle;
    uint32_t completed;
    uint32_t target[JL_HOLD_RING];
    uint32_t sent_us[JL_HOLD_RING];
    uint8_t  head;
    uint8_t  count;
} jl_hold;

static btstack_packet_callback_registration_t jl_hci_event_registration;
static btstack_packet_callback_registration_t jl_l2cap_event_registration;
static btstack_data_source_t jl_tx_data_source;
static uint8_t jl_sdp_service_buffer[BTSTACK_JL_SDP_RECORD_SIZE];
/* LE advertising requested by the application: the payload is copied here and
 * applied on the run loop.  Length 0 means advertising off. */
static uint8_t jl_le_adv_data[31];
static uint8_t jl_le_adv_len;

static uint8_t jl_sdp_device_id_buffer[100];

static void jl_notify_ex(uint8_t type, uint8_t code, uint8_t status,
                         const uint8_t *address)
{
    btstack_jl_event_t event;

    event.type = type;
    event.code = code;
    event.status = status;
    if (address != 0) {

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

/* Wake the run loop to handle a request - only once it exists: before
 * btstack_run_loop_init() the poll dereferences a null pointer and the chip
 * hangs until the watchdog resets it (measured: `pair 3` with the LE layer
 * setting its advertisement from the profile's start()).  A request made
 * earlier is picked up by the first poll anyway. */
void btstack_jl_core_wake(void)
{
    if (jl_run_loop_up) btstack_run_loop_poll_data_sources_from_irq();
}

void jl_enter_critical_public(void)
{
    jl_enter_critical();
}

void jl_exit_critical_public(void)
{
    jl_exit_critical();
}

static void jl_trace(uint32_t code)
{
    if (jl_config.trace != 0) {
        jl_config.trace(code);
    }
}

static void jl_address_to_jieli(const uint8_t in[6], uint8_t out[6])
{
    uint8_t i;
    for (i = 0; i < 6; i++) {
        out[i] = in[5 - i];
    }
}

void btstack_l2cap_trace(uint32_t code)
{
    jl_trace(0x2000u + code);
}

/* Overrides the port's weak bring-up trace hook. */
void jieli_btctrler_trace(uint32_t code)
{
    jl_trace(0x100u + code);
}

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

/* The application queues a report from its own task, the run loop sends it.
 * os critical sections on the SDK side guard the indices. */
static int jl_report_enqueue(const uint8_t *message, uint16_t size)
{
    btstack_jl_report_t *slot;
    static uint8_t reported_failure;
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

    if (result != 0) {
        if (!reported_failure) {
            reported_failure = 1;
            jl_trace(0x2500u | (uint32_t)result);
            jl_trace(0x2510u | (uint32_t)(jl_report_count & 0xffu));
        }
    } else {
        reported_failure = 0;
    }
    return result;
}

static uint32_t jl_time_us(void)
{
    return jl_config.time_us != 0 ? jl_config.time_us() : 0u;
}

uint32_t btstack_jl_time_us(void)
{
    return jl_time_us();
}

void btstack_jl_air_reset(void)
{
    jl_enter_critical();
    memset(&jl_input_stats, 0, sizeof(jl_input_stats));
    jl_input_wait_total_us = 0;
    jl_input_hold_total_us = 0;
    jl_exit_critical();
    memset(&jl_hold, 0, sizeof(jl_hold));
}

/* An input report was stored in a slot, replacing an unsent one or not.
 * Caller holds the critical section; returns the report's sequence number. */
uint32_t btstack_jl_air_note_post(uint8_t replacing, uint32_t now_us,
                                  uint32_t *posted_us)
{
    if (replacing) {
        jl_input_stats.replaced++;
    } else {
        *posted_us = now_us;
    }
    return ++jl_input_posted_seq;
}

/* The report with this sequence number, first posted at posted_us, left its
 * slot.  Caller holds the critical section. */
void btstack_jl_air_note_take(uint32_t seq, uint32_t posted_us, uint32_t now_us)
{
    uint32_t wait_us = now_us - posted_us;

    if ((int32_t) wait_us < 0) {
        wait_us = 0;    /* the clocks of two tasks, a tick apart */
    }
    jl_input_taken_seq = seq;
    jl_input_stats.sent++;
    jl_input_wait_total_us += wait_us;
    if (wait_us > jl_input_stats.wait_max_us) {
        jl_input_stats.wait_max_us = wait_us;
    }
}

/* (run loop) Just before a packet goes to the controller on this handle. */
void btstack_jl_air_note_handoff(uint16_t handle)
{
    hci_connection_t *connection = hci_connection_for_handle(handle);
    uint8_t index;

    if (connection == 0) return;
    if (handle != jl_hold.handle) {
        memset(&jl_hold, 0, sizeof(jl_hold));
        jl_hold.handle = handle;
    }
    if (jl_hold.count == JL_HOLD_RING) return;
    index = (uint8_t)((jl_hold.head + jl_hold.count) % JL_HOLD_RING);
    jl_hold.target[index] = jl_hold.completed + connection->num_packets_sent + 1u;
    jl_hold.sent_us[index] = jl_time_us();
    jl_hold.count++;
}

static void jl_hold_completed(const uint8_t *packet, uint16_t size)
{
    uint32_t now_us = jl_time_us();
    uint32_t hold_us;
    unsigned handles = size > 2 ? packet[2] : 0u;
    unsigned i;

    for (i = 0; i < handles && 3u + 4u * i + 4u <= size; i++) {
        uint16_t handle = little_endian_read_16(packet, 3 + 4 * i) & 0x0fffu;

        if (handle != jl_hold.handle) continue;
        jl_hold.completed += little_endian_read_16(packet, 5 + 4 * i);
        while (jl_hold.count != 0 &&
               (int32_t)(jl_hold.completed - jl_hold.target[jl_hold.head]) >= 0) {
            hold_us = now_us - jl_hold.sent_us[jl_hold.head];
            if ((int32_t) hold_us < 0) hold_us = 0;
            jl_enter_critical();
            jl_input_stats.hold_count++;
            if (hold_us > BTSTACK_JL_STALL_US) jl_input_stats.stalls++;
            jl_input_hold_total_us += hold_us;
            if (hold_us > jl_input_stats.hold_max_us) {
                jl_input_stats.hold_max_us = hold_us;
            }
            jl_exit_critical();
            jl_hold.head = (uint8_t)((jl_hold.head + 1u) % JL_HOLD_RING);
            jl_hold.count--;
        }
    }
}

/* (run loop) LE data length, for the statistics. */
void btstack_jl_air_note_le_length(uint16_t tx_octets)
{
    jl_input_stats.le_tx_octets = tx_octets;
}

/* (run loop) LE connection parameters and PHY, for the statistics. */
void btstack_jl_air_note_le(uint16_t interval, uint16_t latency,
                            uint8_t tx_phy, uint8_t rx_phy)
{
    if (interval != 0) {
        jl_input_stats.le_interval = interval;
        jl_input_stats.le_latency = latency;
    }
    if (tx_phy != 0) {
        jl_input_stats.le_tx_phy = tx_phy;
        jl_input_stats.le_rx_phy = rx_phy;
    }
}

/* (run loop) Read the link's RSSI once a second; the answer arrives as a
 * Command Complete (jl_packet_handler). */
void btstack_jl_air_poll_link(uint16_t handle)
{
    uint32_t now = jl_config.time_ms();

    if ((int32_t)(now - jl_rssi_next_ms) < 0) return;
    if (!hci_can_send_command_packet_now()) return;
    jl_rssi_next_ms = now + BTSTACK_JL_RSSI_PERIOD_MS;
    hci_send_cmd(&hci_read_rssi, handle);
}

static void jl_rssi_complete(const uint8_t *packet, uint16_t size)
{
    int8_t rssi;
    uint8_t magnitude;

    /* Command Complete: status at 5, handle at 6, RSSI at 8. */
    if (size < 9 || packet[5] != 0) return;
    rssi = (int8_t) packet[8];
    magnitude = rssi < 0 ? (uint8_t)(-rssi) : 0u;
    if (magnitude == 0) return;
    jl_input_stats.rssi_last = magnitude;
    if (magnitude > jl_input_stats.rssi_worst) {
        jl_input_stats.rssi_worst = magnitude;
    }
}

/* (run loop) Fewer than BTSTACK_JL_MAX_IN_FLIGHT packets are waiting in the
 * controller for this connection.  Its Number Of Completed Packets event
 * wakes the loop to look again. */
int btstack_jl_link_has_room(uint16_t handle)
{
    hci_connection_t *connection = hci_connection_for_handle(handle);

    return connection == 0 ||
           connection->num_packets_sent < BTSTACK_JL_MAX_IN_FLIGHT;
}

/* Returns 0 when stored and the run loop must be woken, -1 when stored and a
 * wake is already on its way (see btstack_jl_core_set_input_report). */
static int jl_input_set(const uint8_t *message, uint16_t size)
{
    int result = 0;
    uint32_t now_us;

    if (size > BTSTACK_JL_MAX_REPORT_SIZE) {
        return 2;
    }
    /* Read outside the critical section: with interrupts held off the clock
     * can miss a tick. */
    now_us = jl_time_us();
    jl_enter_critical();
    if ((jl_hid_cid == 0) || !jl_working) {
        result = 1;
    } else {
        if (jl_input_pending || jl_send_pending) result = -1;
        /* In sniff nothing drains until the loop takes the link out. */
        if (jl_link_sniff) result = 0;
        jl_input_seq = btstack_jl_air_note_post(jl_input_pending, now_us,
                                                &jl_input_posted_us);
        jl_input.size = size;
        memcpy(jl_input.data, message, size);
        jl_input_pending = 1;
    }
    jl_exit_critical();
    return result;
}

/* Queued reports first, in order; then the input report. */
static void jl_report_take(btstack_jl_report_t *out)
{
    uint32_t now_us = jl_time_us();

    jl_enter_critical();
    if (jl_report_count != 0) {
        *out = jl_reports[jl_report_read];
        jl_report_read = (uint16_t)((jl_report_read + 1) % BTSTACK_JL_MAX_QUEUED_REPORTS);
        jl_report_count--;
        jl_input_stats.messages++;
    } else if (jl_input_pending) {
        *out = jl_input;
        jl_input_pending = 0;
        /* Measured from the first post the host has not seen, so a
         * replaced report counts as waiting from then on. */
        btstack_jl_air_note_take(jl_input_seq, jl_input_posted_us, now_us);
    } else {
        out->size = 0;
    }
    jl_exit_critical();
}

/* Exit sniff on the current link if it is in sniff, at most every 50 ms.
 *
 * A controller held in sniff by the console takes no ACL data between anchors,
 * and answers a disconnect on its own schedule.  Pushing reports into that link
 * filled the controller's buffers and the host's queue and ended in a chip
 * reset, so the host leaves sniff before it sends anything or tears anything
 * down - which is what the vendor host's bt_comm_edr_sniff_clean() did before
 * every HID send.  Must run on the run-loop task (it sends an HCI command). */
static void jl_exit_sniff_if_needed(void)
{
    hci_connection_t *connection;

    if (!jl_con_handle_valid) {
        return;
    }
    if (jl_unsniff_next_ms != 0 &&
        (int32_t)(jl_config.time_ms() - jl_unsniff_next_ms) < 0) {
        return;
    }
    connection = hci_connection_for_handle(jl_con_handle);
    if (connection == 0 || connection->connection_mode == 0) {
        return;
    }
    hci_send_cmd(&hci_exit_sniff_mode, jl_con_handle);
    jl_unsniff_next_ms = jl_config.time_ms() + 50;
    jl_trace(0x2c00u | connection->connection_mode);
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

    btstack_jl_le_poll();
    if ((request & JL_REQUEST_SCAN) != 0) {
        jl_apply_scan_state();
    }
    if ((request & JL_REQUEST_ADDRESS) != 0) {

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

            gap_set_security_level(LEVEL_2);
            hid_device_connect(peer, &hid_cid);
            gap_set_security_level(LEVEL_0);
        }
    }

    if ((request & JL_REQUEST_LE_ADV) != 0) {
        if (jl_le_adv_len != 0) {
            bd_addr_t none;

            memset(none, 0, sizeof(none));

            gap_advertisements_set_params(0x0030, 0x0030, 0, 0, none, 0x07, 0);
            gap_advertisements_set_data(jl_le_adv_len, jl_le_adv_data);
            gap_advertisements_enable(1);
        } else {
            gap_advertisements_enable(0);
        }
    }
    if ((request & JL_REQUEST_UNSNIFF) != 0) {
        jl_exit_sniff_if_needed();
    }
    /* The host is usually the master, and a slave may only transmit when the
     * master polls it: every 40 slots (25 ms) by default, which delivered
     * reports in bursts 25-40 ms apart.  Guaranteed service with a 1.25 ms
     * latency asks the master for a poll interval to match.  (This
     * controller reports the result with event code 0x01, not 0x0d; it
     * accepts with the values asked for.) */
    if (jl_qos_pending && jl_con_handle_valid &&
        hci_can_send_command_packet_now()) {
        jl_qos_pending = 0;
        hci_send_cmd(&hci_qos_setup, jl_con_handle, 0, 2, 8000, 0, 1250,
                     0xffffffffu);
    }
    if (jl_hid_cid != 0 && jl_con_handle_valid) {
        btstack_jl_air_poll_link(jl_con_handle);
    }

    if ((request & JL_REQUEST_SHUTDOWN) != 0) {

        jl_exit_sniff_if_needed();
        btstack_jl_core_power_off();
        return;
    }

    if (jl_input_pending || (jl_report_count != 0)) {
        /* Rate-limited, and only when the link really is in sniff. */
        jl_exit_sniff_if_needed();
    }
    if (!jl_send_pending && ((jl_report_count != 0) || jl_input_pending) &&
        (jl_hid_cid != 0) &&
        (!jl_con_handle_valid || btstack_jl_link_has_room(jl_con_handle))) {
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
    /* The LE side keeps its own copy of the public address, set once when the
     * controller starts: without this `pair N fresh` changed the Classic
     * address and the pad still advertised over LE from the old one. */
    le_controller_set_mac(jl_address_jieli);
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
                    btstack_jl_le_working();
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
                jl_acl_up = 1;

                hci_remote_features_query(jl_con_handle);
                jl_notify_logged(BTSTACK_JL_EVENT_CONNECTED, 0, event_addr);
            }
            break;

        case HCI_EVENT_COMMAND_COMPLETE:
            if (hci_event_command_complete_get_command_opcode(packet) ==
                HCI_OPCODE_HCI_READ_RSSI) {
                jl_rssi_complete(packet, size);
            }
            break;

        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
            jl_hold_completed(packet, size);
            break;

        case HCI_EVENT_MODE_CHANGE:
            jl_link_sniff = hci_event_mode_change_get_mode(packet) == 2;
            if (jl_link_sniff) jl_input_stats.sniff_entries++;
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            jl_link_sniff = 0;
            status = hci_event_disconnection_complete_get_reason(packet);

            jl_trace(0x2a00u | status);
            jl_peer_from_handle(
                hci_event_disconnection_complete_get_connection_handle(packet),
                event_addr);
            jl_hid_cid = 0;
            jl_send_pending = 0;
            jl_acl_up = 0;
            jl_notify_ex(BTSTACK_JL_EVENT_HCI_STATUS, HCI_EVENT_DISCONNECTION_COMPLETE,
                         status, event_addr);
            jl_notify_logged(BTSTACK_JL_EVENT_DISCONNECTED, status, event_addr);
            break;

        case HCI_EVENT_LINK_KEY_REQUEST:

            jl_trace(0x2801u);
            break;

        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            /* A pairing completed and the console handed us its key (0x2901).
             * Count them per boot: a reset that always lands on the same count
             * means something is leaking for each pairing. */
            jl_pairings++;
            jl_trace(0x2901u);
            jl_trace(0x7a00u | (uint32_t)(jl_pairings & 0xffu));
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

                    jl_trace(0x2b00u | (jl_hid_cid != 0 ? 1u : 0u));
                    hid_subevent_connection_opened_get_bd_addr(packet, event_addr);
                    memcpy(jl_peer_address, event_addr, sizeof(jl_peer_address));
                    jl_peer_valid = 1;
                    jl_qos_pending = 1;
                    jl_input_pending = 0;
                    btstack_jl_air_reset();
                    jl_notify_logged(BTSTACK_JL_EVENT_HID_OPENED, 0, event_addr);
                    break;

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    jl_hid_cid = 0;
                    jl_send_pending = 0;
                    jl_notify_logged(BTSTACK_JL_EVENT_HID_CLOSED, 0, jl_peer_address);
                    break;

                case HID_SUBEVENT_CAN_SEND_NOW: {
                    btstack_jl_report_t report;
                    jl_report_take(&report);
                    if (report.size != 0) {
                        btstack_jl_air_note_handoff(jl_con_handle);
                        hid_device_send_interrupt_message(jl_hid_cid, report.data,
                                                          report.size);
                        jl_tx_sent++;
                        if ((jl_tx_sent & 0x0fu) == 0u) {
                            jl_trace(0x2300u | ((jl_tx_sent >> 8) & 0xffu));
                        }
                    }
                    jl_send_pending = 0;
                    jl_tx_stall_since = 0;
                    jl_tx_stall_reported = 0;
                    /* A report posted while this one waited did not wake the
                     * loop (jl_input_set); ask for the next slot now. */
                    if ((jl_report_count != 0) || jl_input_pending) {
                        btstack_jl_core_wake();
                    }
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

    /* Skip the rumble-only report: the console sends it ~20 times a second,
     * alternating with the subcommand reports, so recording it fills the
     * bring-up ring long before a session ends.  0x01 (rumble + subcommand) is
     * kept - the subcommand traces follow it. */
    if (report_id != 0x10U) {
        jl_trace(0x2000u | report_id);
    }
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

    if ((unsigned) report_size + 1u > sizeof(packet)) {
        return;
    }
    packet[0] = (uint8_t)(0x50u | (uint8_t) report_type);
    memcpy(&packet[1], report, (size_t) report_size);
    jl_config.hid_output(packet, (uint16_t)(report_size + 1));
}

static int jl_hid_report_request(uint16_t cid, hid_report_type_t report_type,
                                 uint16_t report_id, int *out_report_size,
                                 uint8_t *out_report)
{
    int size;

    (void) cid;
    if (jl_config.hid_get_report == 0) {
        return 0;
    }
    size = btstack_hid_get_report_size_for_id(report_id, report_type,
                                              jl_config.report_descriptor,
                                              jl_config.report_descriptor_size);
    if ((size <= 0) || (size > BTSTACK_JL_MAX_REPORT_SIZE)) {
        return 0;
    }
    if (!jl_config.hid_get_report((uint8_t) report_type, (uint8_t) report_id,
                                  out_report, (uint16_t) size)) {
        return 0;
    }
    *out_report_size = size;
    return 1;
}

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

    hci_enable_acl_packet_types(BTSTACK_JL_ACL_PACKET_TYPES);
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

    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(jl_config.io_capability);
    gap_ssp_set_authentication_requirement(jl_config.authentication_requirement);
    gap_ssp_set_auto_accept(1);

    memset(&hid_params, 0, sizeof(hid_params));
    hid_params.hid_device_subclass = (uint16_t) jl_config.class_of_device;
    hid_params.hid_country_code = 0;

    hid_params.hid_virtual_cable = 1;
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
    {
        /* A failure here leaves the pad with an empty SDP database: hosts
         * that look for the HID service (BlueZ) then never set it up. */
        uint8_t status = sdp_register_service(jl_sdp_service_buffer);
        if (status != 0) {
            cyclone2_usb_debug("BTSTACK: SDP HID record not registered (0x%02x)",
                               status);
        }
    }
    jl_trace(15u);
    memset(jl_sdp_device_id_buffer, 0, sizeof(jl_sdp_device_id_buffer));
    device_id_create_sdp_record(jl_sdp_device_id_buffer,
                                sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_USB,
                                jl_config.device_id_vendor,
                                jl_config.device_id_product,
                                jl_config.device_id_version);
    if (sdp_register_service(jl_sdp_device_id_buffer) != 0) {
        cyclone2_usb_debug("BTSTACK: SDP Device ID record not registered");
    }

    hid_device_init(false, jl_config.report_descriptor_size,
                    jl_config.report_descriptor);

    hid_device_accept_truncated_hid_reports(true);
    hid_device_register_report_data_callback(&jl_hid_report_data);
    hid_device_register_set_report_callback(&jl_hid_set_report);
    hid_device_register_report_request_callback(&jl_hid_report_request);
    jl_trace(16u);
    hid_device_register_packet_handler(&jl_packet_handler);

    jl_hci_event_registration.callback = &jl_packet_handler;
    hci_add_event_handler(&jl_hci_event_registration);

    btstack_jl_le_setup();

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
    jl_input_pending = 0;
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
    jl_acl_up = 0;
    jl_hid_cid = 0;
    jl_send_pending = 0;
    jl_report_count = 0;
    jl_input_pending = 0;
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

void btstack_jl_core_le_advertise(const uint8_t *data, uint8_t length)
{
    if (length > sizeof(jl_le_adv_data)) length = sizeof(jl_le_adv_data);
    jl_enter_critical();
    if (data != 0 && length != 0) memcpy(jl_le_adv_data, data, length);
    jl_le_adv_len = (data != 0) ? length : 0;
    jl_request |= JL_REQUEST_LE_ADV;
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

uint16_t btstack_jl_core_reports_queued(void)
{
    return (uint16_t)(jl_report_count + jl_input_pending);
}

/* Wakes the run loop: it would otherwise find the report only on its next
 * pass, up to a system tick (10 ms) later. */
int btstack_jl_core_send_report(const uint8_t *message, uint16_t size)
{
    int result = jl_report_enqueue(message, size);

    if (result == 0) btstack_jl_core_wake();
    return result;
}

/* Wakes the run loop only for a report into an empty slot with no send
 * outstanding: otherwise the pending CAN_SEND_NOW takes it, and waking the
 * loop for every report costs the application task its control polls. */
int btstack_jl_core_set_input_report(const uint8_t *message, uint16_t size)
{
    int result = jl_input_set(message, size);

    if (result == 0) btstack_jl_core_wake();
    return result < 0 ? 0 : result;
}

uint32_t btstack_jl_core_input_posted(void)
{
    return jl_input_posted_seq;
}

uint32_t btstack_jl_core_input_taken(void)
{
    return jl_input_taken_seq;
}

void btstack_jl_core_input_stats(btstack_jl_input_stats_t *out)
{
    jl_enter_critical();
    *out = jl_input_stats;
    out->wait_avg_us = jl_input_stats.sent != 0 ?
        (uint32_t)(jl_input_wait_total_us / jl_input_stats.sent) : 0u;
    out->hold_avg_us = jl_input_stats.hold_count != 0 ?
        (uint32_t)(jl_input_hold_total_us / jl_input_stats.hold_count) : 0u;
    jl_exit_critical();
}

void btstack_jl_core_delete_all_link_keys(void)
{
    bd_addr_t all_zero;

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

void btstack_jl_core_request_unsniff(void)
{
    jl_enter_critical();
    jl_request |= JL_REQUEST_UNSNIFF;
    jl_exit_critical();
    btstack_run_loop_poll_data_sources_from_irq();
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
    btstack_jl_le_teardown();
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

uint8_t btstack_jl_core_link_up(void)
{
    return jl_acl_up;
}
