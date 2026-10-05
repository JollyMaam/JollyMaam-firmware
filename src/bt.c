/* The Bluetooth host on upstream BTstack; architecture/bluetooth.md.  A
 * profile's bt_device becomes SDP records and a HID device (Classic) or a
 * GATT server and advertising (LE).  BTstack runs on its own thread; the
 * control thread talks to it through the requests, the input slot and the
 * message queue below, and hears back through bt_poll(). */
#include <string.h>
#include "btstack_config.h"
#include "bluetooth_gatt.h"
#include "btstack_crypto.h"
#include "btstack_event.h"
#include "btstack_hid_parser.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "ble/att_db.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h"
#include "ble/sm.h"
#include "classic/btstack_link_key_db.h"
#include "classic/device_id_server.h"
#include "classic/hid_device.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "pad.h"

const hci_transport_t *hci_jieli_transport(void);
const btstack_run_loop_t *hci_jieli_run_loop(void);
void hci_jieli_set_address(const u8 a[6]);
void hci_jieli_drop_link(void);
void hci_jieli_prepare(u16 slots, u16 attempt);
void hci_jieli_tx_power(u8 step, u8 pro);
void hci_jieli_link_policy(void);

#define MAX_INPUT      80
#define MAX_MESSAGE    160
#define MESSAGES       6
#define EVENTS         8
#define IN_FLIGHT      2         /* when the slot's in_flight is 0 */
#define IN_FLIGHT_MAX  8
#define CCCS           8
/* Single-slot ACL packets only (DM1 DH1 2-DH1 3-DH1), as the stock firmware
 * pages: a report never rides a longer packet that fails more often. */
#define ACL_TYPES      0x001e
#define UNSNIFF_MS     50

enum { RQ_START = 1, RQ_CONNECT = 2, RQ_DISCONNECT = 4, RQ_STOP = 8, RQ_ABORT = 16 };

struct message { u16 handle; u16 len; u8 data[MAX_MESSAGE]; };

static const struct profile *prof;
static const struct bt_device *dev;
static u8 addr[6];                       /* ours, least significant byte first */
static struct bt_bond bond;
static u8 pairing;
static u8 le;

static volatile u8 requests;
static volatile u8 idle = 1;

static u8 input[MAX_INPUT];
static u16 input_len;
static volatile u8 input_pending;
static struct bt_stats stats;
static struct bt_timing timing;          /* under plat_lock: read from the control thread */
static struct bt_link link;
static u32 slot_us;                      /* when the slot's report came in */
static struct message messages[MESSAGES];
static volatile u8 msg_read, msg_write;

static struct bt_ev events[EVENTS];
static volatile u8 ev_read, ev_write;

/* bluetooth thread only */
static u8 working, acl_up, linked, send_pending, qos_pending, stopping;
static u16 hid_cid;
static hci_con_handle_t con_handle;
static u32 unsniff_next;
static btstack_data_source_t tx_source;
static btstack_packet_callback_registration_t hci_registration, sm_registration;
static u8 sdp_hid[720];
static u8 sdp_id[100];
static u8 sm_on, wake_adv, data_length_pending, phy_pending;
static u16 ccc_handle[CCCS];
static u8 ccc_value[CCCS];
static btstack_packet_callback_registration_t l2cap_registration;
/* Reports in the controller, oldest first: when each went in and when it
 * entered the slot, to time Number Of Completed Packets against. */
static struct { u32 sent_us, slot_us; } flight[IN_FLIGHT_MAX];
static u8 flight_n;
static u32 last_ack_us;

/* ---- control thread side ------------------------------------------------- */

static void reverse(const u8 *in, u8 *out, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = in[n - 1 - i];
}

static void request(u8 r)
{
    plat_lock();
    requests |= r;
    plat_unlock();
    plat_bt_wake();
}

static void body(void);

void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *b, int pair,
              const struct bt_link *l)
{
    static u8 thread;

    prof = p;
    dev = p->device;
    le = dev->transport == BT_LE;
    memcpy(addr, address, 6);
    memset(&bond, 0, sizeof(bond));
    if (b) bond = *b;
    pairing = (u8)pair;
    link = *l;
    if (!link.in_flight) link.in_flight = IN_FLIGHT;
    if (link.in_flight > IN_FLIGHT_MAX) link.in_flight = IN_FLIGHT_MAX;
    msg_read = msg_write = 0;
    input_pending = 0;
    idle = 0;
    if (!thread) {
        thread = 1;
        plat_bt_thread(body);
    }
    request(RQ_START);
}

void bt_stop(void) { request(RQ_STOP); }
void bt_abort(void) { request(RQ_ABORT); }
void bt_connect(void) { request(RQ_CONNECT); }
void bt_disconnect(void) { request(RQ_DISCONNECT); }

/* A bond learned by the profile (Switch 2 pairs in its own protocol): the
 * host asks for the key right after. */
void bt_update_bond(const struct bt_bond *b)
{
    plat_lock();
    bond = *b;
    plat_unlock();
}

int bt_send_input(u8 id, const u8 *data, u16 len)
{
    if (!linked || len + 2 > MAX_INPUT) return 0;
    plat_lock();
    if (le) {
        memcpy(input, data, len);
        input_len = len;
    } else {
        input[0] = 0xa1;
        input[1] = id;
        memcpy(input + 2, data, len);
        input_len = len + 2;
    }
    if (input_pending) stats.replaced++;
    else slot_us = plat_us();
    input_pending = 1;
    plat_unlock();
    plat_bt_wake();
    return 1;
}

void bt_stats(struct bt_stats *out)
{
    plat_lock();
    *out = stats;
    plat_unlock();
}

void bt_timing(struct bt_timing *out, int reset)
{
    plat_lock();
    if (reset) {
        memset(&timing.ack_wait, 0, 3 * sizeof(timing.ack_wait));
        last_ack_us = 0;
    }
    *out = timing;
    plat_unlock();
}

int bt_input_pending(void)
{
    return input_pending;
}

/* Classic: id is the report ID; LE: the handle to notify. */
int bt_send_message(u16 id, const u8 *data, u16 len)
{
    struct message *m;
    u8 next;

    if (!linked || len + 2 > MAX_MESSAGE) return 0;
    plat_lock();
    next = (u8)((msg_write + 1) % MESSAGES);
    if (next == msg_read) {
        plat_unlock();
        return 0;
    }
    m = &messages[msg_write];
    m->handle = id;
    if (le) {
        memcpy(m->data, data, len);
        m->len = len;
    } else {
        m->data[0] = 0xa1;
        m->data[1] = id;
        memcpy(m->data + 2, data, len);
        m->len = len + 2;
    }
    msg_write = next;
    plat_unlock();
    plat_bt_wake();
    return 1;
}

int bt_poll(struct bt_ev *ev)
{
    if (ev_read == ev_write) return 0;
    *ev = events[ev_read];
    ev_read = (u8)((ev_read + 1) % EVENTS);
    return 1;
}

/* No pairing exchange or link set-up under way: flash writes are safe. */
int bt_radio_quiet(void)
{
    return idle || (working && (!acl_up || linked));
}

/* ---- events up ------------------------------------------------------------- */

static void emit(u8 kind, u8 reason)
{
    struct bt_ev *e;
    u8 next = (u8)((ev_write + 1) % EVENTS);

    if (next == ev_read) return;
    e = &events[ev_write];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->reason = reason;
    e->lost = reason == ERROR_CODE_CONNECTION_TIMEOUT ||
              reason == ERROR_CODE_LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT;
    e->bond = bond;
    ev_write = next;
    plat_wake(EV_BT);
}

static void emit_write(u8 kind, u16 id, const u8 *data, int len)
{
    struct bt_ev *e;
    u8 next = (u8)((ev_write + 1) % EVENTS);

    if (next == ev_read || len < 0) return;
    if (len > BT_WRITE_MAX) len = BT_WRITE_MAX;
    e = &events[ev_write];
    e->kind = BT_EV_WRITE;
    e->write.kind = kind;
    e->write.id = id;
    e->write.len = (u16)len;
    memcpy(e->write.data, data, (u32)len);
    ev_write = next;
    plat_wake(EV_BT);
}

static int zero(const u8 *p, int n)
{
    while (n--)
        if (*p++) return 0;
    return 1;
}

/* ---- Classic: link keys from the slot's bond --------------------------------- */

static void kdb_nop(void) {}
static void kdb_addr(bd_addr_t a) { (void)a; }

static int kdb_get(bd_addr_t a, link_key_t key, link_key_type_t *type)
{
    u8 peer[6];

    reverse(a, peer, 6);
    if (zero(bond.link_key, 16) || memcmp(peer, bond.peer, 6)) {
        util_log("bt: no key for %s", bd_addr_to_str(a));
        return 0;
    }
    memcpy(key, bond.link_key, 16);
    *type = (link_key_type_t)bond.key_type;
    return 1;
}

static void kdb_put(bd_addr_t a, link_key_t key, link_key_type_t type)
{
    reverse(a, bond.peer, 6);
    memcpy(bond.link_key, key, 16);
    bond.key_type = (u8)type;
    util_log("bt: bonded %s type %u", bd_addr_to_str(a), type);
    emit(BT_EV_BONDED, 0);
}

static void kdb_delete(bd_addr_t a)
{
    u8 peer[6];

    reverse(a, peer, 6);
    if (memcmp(peer, bond.peer, 6)) return;
    memset(bond.link_key, 0, 16);
    bond.key_type = 0;
    emit(BT_EV_BONDED, 0);
}

static int kdb_iter_init(btstack_link_key_iterator_t *it)
{
    it->context = (void *)0;
    return 1;
}

static int kdb_iter_next(btstack_link_key_iterator_t *it, bd_addr_t a, link_key_t key,
                         link_key_type_t *type)
{
    if (it->context || zero(bond.link_key, 16)) return 0;
    it->context = (void *)1;
    reverse(bond.peer, a, 6);
    memcpy(key, bond.link_key, 16);
    *type = (link_key_type_t)bond.key_type;
    return 1;
}

static void kdb_iter_done(btstack_link_key_iterator_t *it) { (void)it; }

static const btstack_link_key_db_t key_db = {
    .open = kdb_nop, .set_local_bd_addr = kdb_addr, .close = kdb_nop,
    .get_link_key = kdb_get, .put_link_key = kdb_put, .delete_link_key = kdb_delete,
    .iterator_init = kdb_iter_init, .iterator_get_next = kdb_iter_next,
    .iterator_done = kdb_iter_done,
};

/* ---- Classic: connecting ------------------------------------------------------ */

static void scan(int on)
{
    gap_connectable_control((u8)on);
    gap_discoverable_control((u8)on);
}

/* Outgoing HID channels only after authentication (LEVEL_2); incoming ones
 * stay at LEVEL_0, the host drives security itself.  Opening before
 * authentication caused reason-5 failures. */
static void page(void)
{
    bd_addr_t peer;
    u16 cid;

    if (hid_cid || zero(bond.peer, 6)) return;
    reverse(bond.peer, peer, 6);
    gap_set_security_level(LEVEL_2);
    hid_device_connect(peer, &cid);
    gap_set_security_level(LEVEL_0);
}

/* A host holding the link in sniff takes no data between anchors: leave sniff
 * before sending or tearing down, at most every 50 ms. */
static void unsniff(void)
{
    hci_connection_t *c;

    if (le || !acl_up || (s32)(plat_ms() - unsniff_next) < 0) return;
    c = hci_connection_for_handle(con_handle);
    if (!c || !c->connection_mode) return;
    hci_send_cmd(&hci_exit_sniff_mode, con_handle);
    unsniff_next = plat_ms() + UNSNIFF_MS;
}

/* ---- LE: advertising and the GATT server --------------------------------------- */

static void advertise(void)
{
    u8 data[31];
    u8 n;
    bd_addr_t none;

    if (!working || !le) return;
    if (stopping || acl_up || !dev->advertising) {
        gap_advertisements_enable(0);
        return;
    }
    n = dev->advertising(&bond, pairing, wake_adv, data);
    memset(none, 0, sizeof(none));
    gap_advertisements_set_params(0x0020, 0x0020, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(n, data);
    if (dev->scan_response_len)
        gap_scan_response_set_data(dev->scan_response_len, (u8 *)dev->scan_response);
    gap_advertisements_enable(1);
}

static int ccc(u16 handle, int create)
{
    int i;

    for (i = 0; i < CCCS; i++)
        if (ccc_handle[i] == handle) return i;
    if (!create) return -1;
    for (i = 0; i < CCCS; i++) {
        if (!ccc_handle[i]) {
            ccc_handle[i] = handle;
            ccc_value[i] = 0;
            return i;
        }
    }
    return -1;
}

static int notifying(u16 value_handle)
{
    int i = ccc((u16)(value_handle + 1), 0);
    return i >= 0 && (ccc_value[i] & 1);
}

static u16 copy_out(const u8 *v, u16 n, u16 offset, u8 *buf, u16 size)
{
    if (!buf) return n;
    if (offset >= n) return 0;
    if (size > n - offset) size = n - offset;
    memcpy(buf, v + offset, size);
    return size;
}

static u16 att_read(hci_con_handle_t h, u16 handle, u16 offset, u8 *buf, u16 size)
{
    u8 tmp[MAX_INPUT];
    int i = ccc(handle, 0), n;

    (void)h;
    if (i >= 0) {
        u8 v[2] = { ccc_value[i], 0 };
        return copy_out(v, 2, offset, buf, size);
    }
    if (handle == dev->input_handle) return copy_out(input, input_len, offset, buf, size);
    if (!prof->read) return 0;
    n = prof->read(BT_WRITE_ATT, handle, 0, tmp, sizeof(tmp));
    return n > 0 ? copy_out(tmp, (u16)n, offset, buf, size) : 0;
}

static int att_write(hci_con_handle_t h, u16 handle, u16 mode, u16 offset, u8 *buf, u16 size)
{
    (void)h;
    (void)offset;
    if (mode != ATT_TRANSACTION_MODE_NONE) return 0;
    if (att_uuid_for_handle(handle) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION) {
        int i = ccc(handle, 1);
        if (i >= 0 && size) ccc_value[i] = buf[0];
        util_log("bt: host %s notifications on 0x%04x", size && buf[0] ? "enables" : "disables", handle - 1);
        return 0;
    }
    emit_write(BT_WRITE_ATT, handle, buf, size);
    return 0;
}

static void on_sm(u8 type, u16 channel, u8 *packet, u16 size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_JUST_WORKS_REQUEST:
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;
    case SM_EVENT_PAIRING_COMPLETE:
        util_log("bt: le pairing status 0x%02x", sm_event_pairing_complete_get_status(packet));
        break;
    }
}

/* ---- sending -------------------------------------------------------------------- */

static int room(void)
{
    hci_connection_t *c = hci_connection_for_handle(con_handle);
    return !c || c->num_packets_sent < link.in_flight;
}

static int have_data(void)
{
    return input_pending || msg_read != msg_write;
}

static void send_now(void)
{
    struct message m;
    u32 slot = 0;

    m.len = 0;
    plat_lock();
    if (msg_read != msg_write) {
        m = messages[msg_read];
        msg_read = (u8)((msg_read + 1) % MESSAGES);
    } else if (input_pending && room()) {
        m.handle = dev->input_handle;
        m.len = input_len;
        memcpy(m.data, input, input_len);
        input_pending = 0;
        slot = slot_us;
        stats.sent++;
    }
    plat_unlock();
    if (m.len) {
        if (!le) hid_device_send_interrupt_message(hid_cid, m.data, m.len);
        else if (m.handle != dev->input_handle || notifying(m.handle))
            att_server_notify(con_handle, m.handle, m.data, m.len);
        /* Messages ride along unmarked (slot 0): only reports are timed. */
        if (flight_n < IN_FLIGHT_MAX) {
            flight[flight_n].sent_us = plat_us();
            flight[flight_n].slot_us = slot;
            flight_n++;
        }
    }
    send_pending = 0;
    if (have_data()) plat_bt_wake();
}

/* ---- the run loop's own poll: requests and sends ------------------------------ */

static void tx_poll(btstack_data_source_t *ds, btstack_data_source_callback_type_t t)
{
    u8 r;

    (void)ds;
    (void)t;
    plat_lock();
    r = requests & ~RQ_START;
    requests &= RQ_START;
    plat_unlock();
    if (r & RQ_ABORT) {
        hci_jieli_drop_link();
        btstack_run_loop_trigger_exit();
        return;
    }
    if (r & RQ_STOP) {
        unsniff();
        stopping = 1;
        working = 0;
        hci_power_control(HCI_POWER_OFF);
        return;
    }
    if (r & RQ_DISCONNECT) {
        unsniff();
        if (hid_cid) hid_device_disconnect(hid_cid);
        else if (acl_up) gap_disconnect(con_handle);
    }
    if ((r & RQ_CONNECT) && working && !le) {
        if (acl_up && !hid_cid) gap_disconnect(con_handle);
        else page();
    }
    if (le && phy_pending && acl_up && hci_can_send_command_packet_now()) {
        phy_pending = 0;
        gap_le_set_phy(con_handle, 0, 2, 2, 0);     /* 2M both ways */
    }
    if (le && data_length_pending && acl_up && hci_can_send_command_packet_now()) {
        /* At the default 27 bytes a Switch 2 report took three connection
         * events: ask for 251. */
        data_length_pending = 0;
        hci_send_cmd(&hci_le_set_data_length, con_handle, 251, 2120);
    }
    /* A Classic host polls a peripheral every 25 ms by default, which
     * delivered reports in bursts: ask for a 1.25 ms poll interval. */
    if (qos_pending && acl_up && hci_can_send_command_packet_now()) {
        qos_pending = 0;
        if (link.poll_us)
            hci_send_cmd(&hci_qos_setup, con_handle, 0, 2, 8000, 0, (u32)link.poll_us, 0xffffffffu);
    }
    if (have_data()) unsniff();
    if (!send_pending && linked && have_data() && (msg_read != msg_write || room())) {
        send_pending = 1;
        if (le) att_server_request_can_send_now_event(con_handle);
        else hid_device_request_can_send_now_event(hid_cid);
    }
}

/* ---- link events ------------------------------------------------------------------ */

static void unlinked(u8 reason)
{
    int was = linked;

    linked = 0;
    hid_cid = 0;
    send_pending = 0;
    acl_up = 0;
    flight_n = 0;
    plat_lock();
    memset(&timing.link, 0, sizeof(timing.link));
    plat_unlock();
    if (pairing && !le) scan(1);
    wake_adv = 0;
    advertise();
    if (was) emit(BT_EV_UNLINKED, reason);
    else if (!pairing && !le) emit(BT_EV_FAILED, reason);
}

/* ---- what the link turned out to be: bt_timing().link and the log ------------- */

static void link_up(u8 transport)
{
    flight_n = 0;
    plat_lock();
    memset(&timing.link, 0, sizeof(timing.link));
    timing.link.transport = transport;
    timing.link.phy_tx = timing.link.phy_rx = transport == 2 ? 1 : 0;
    timing.link.tx_octets = timing.link.rx_octets = transport == 2 ? 27 : 0;
    timing.link.acl_buffers = (u16)hci_number_free_acl_slots_for_handle(con_handle);
    plat_unlock();
}

static void le_params(u16 interval, u16 latency, u16 timeout)
{
    plat_lock();
    timing.link.interval = interval;
    timing.link.latency = latency;
    timing.link.timeout = timeout;
    plat_unlock();
}

/* Number Of Completed Packets: the controller says reports went.  BTstack's
 * own packets (ATT responses, L2CAP) are acked here too and are not in
 * flight[]; whatever the connection has outstanding beyond ours is theirs. */
static void acked(void)
{
    hci_connection_t *c = hci_connection_for_handle(con_handle);
    u32 now = plat_us();
    int left = c ? c->num_packets_sent : 0;
    u8 i, n = 0;

    plat_lock();
    if (last_ack_us) util_stat_add(&timing.ack_gap, now - last_ack_us);
    last_ack_us = now;
    while (flight_n - n > left && n < flight_n) {
        util_stat_add(&timing.ack_wait, now - flight[n].sent_us);
        if (flight[n].slot_us) util_stat_add(&timing.slot_ack, now - flight[n].slot_us);
        n++;
    }
    plat_unlock();
    for (i = n; i < flight_n; i++) flight[i - n] = flight[i];
    flight_n = (u8)(flight_n - n);
}

static void le_connected(u8 *packet)
{
    bd_addr_t a;
    hci_connection_t *c;

    if (hci_subevent_le_connection_complete_get_status(packet)) return;
    con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
    acl_up = linked = 1;
    hci_subevent_le_connection_complete_get_peer_address(packet, a);
    util_log("bt: le %s interval %u latency %u timeout %u", bd_addr_to_str(a),
             hci_subevent_le_connection_complete_get_conn_interval(packet),
             hci_subevent_le_connection_complete_get_conn_latency(packet),
             hci_subevent_le_connection_complete_get_supervision_timeout(packet));
    link_up(2);
    le_params(hci_subevent_le_connection_complete_get_conn_interval(packet),
              hci_subevent_le_connection_complete_get_conn_latency(packet),
              hci_subevent_le_connection_complete_get_supervision_timeout(packet));
    reverse(a, bond.le_peer, 6);
    memset(ccc_value, 0, sizeof(ccc_value));
    data_length_pending = 1;
    phy_pending = link.le_phy;
    c = hci_connection_for_handle(con_handle);
    if (!sm_on && c) {
        /* Without SM nobody runs the identity lookup att_server waits for:
         * mark it done, or every ATT request is held forever. */
        c->sm_connection.sm_irk_lookup_state = IRK_LOOKUP_FAILED;
        c->sm_connection.sm_le_db_index = -1;
    }
    if (sm_on) sm_request_pairing(con_handle);
    emit(BT_EV_PEER, 0);
    emit(BT_EV_LINKED, 0);
}

static void on_event(u8 type, u16 channel, u8 *packet, u16 size)
{
    bd_addr_t a;
    u8 key[16];

    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS: {
        u8 i;

        for (i = 0; i < packet[2]; i++)
            if (little_endian_read_16(packet, 3 + 4 * i) == con_handle) {
                stats.acked += little_endian_read_16(packet, 5 + 4 * i);
                if (acl_up) acked();
            }
        break;
    }
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            working = 1;
            hci_jieli_set_address(addr);
            reverse(addr, a, 6);
            hci_set_bd_addr(a);
            if (le) {
                wake_adv = !pairing && !zero(bond.le_peer, 6);
                advertise();
                break;
            }
            hci_jieli_link_policy();
            if (pairing) {
                memset(a, 0, 6);
                hci_send_cmd(&hci_delete_stored_link_key, a, 1);
            }
            if (pairing || zero(bond.peer, 6)) scan(1);
            else page();
        } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && !working) {
            btstack_run_loop_trigger_exit();
        }
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        hci_event_connection_complete_get_bd_addr(packet, a);
        util_log("bt: acl %s status 0x%02x", bd_addr_to_str(a), hci_event_connection_complete_get_status(packet));
        if (hci_event_connection_complete_get_status(packet)) break;
        con_handle = hci_event_connection_complete_get_connection_handle(packet);
        acl_up = 1;
        link_up(1);
        reverse(a, bond.peer, 6);
        emit(BT_EV_PEER, 0);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        util_log("bt: down reason 0x%02x", hci_event_disconnection_complete_get_reason(packet));
        unlinked(hci_event_disconnection_complete_get_reason(packet));
        break;
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            le_connected(packet);
            break;
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            util_log("bt: le update status 0x%02x interval %u latency %u timeout %u",
                     hci_subevent_le_connection_update_complete_get_status(packet),
                     hci_subevent_le_connection_update_complete_get_conn_interval(packet),
                     hci_subevent_le_connection_update_complete_get_conn_latency(packet),
                     hci_subevent_le_connection_update_complete_get_supervision_timeout(packet));
            if (!hci_subevent_le_connection_update_complete_get_status(packet))
                le_params(hci_subevent_le_connection_update_complete_get_conn_interval(packet),
                          hci_subevent_le_connection_update_complete_get_conn_latency(packet),
                          hci_subevent_le_connection_update_complete_get_supervision_timeout(packet));
            break;
        case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
            util_log("bt: le data length tx %u/%u us rx %u/%u us",
                     hci_subevent_le_data_length_change_get_max_tx_octets(packet),
                     hci_subevent_le_data_length_change_get_max_tx_time(packet),
                     hci_subevent_le_data_length_change_get_max_rx_octets(packet),
                     hci_subevent_le_data_length_change_get_max_rx_time(packet));
            plat_lock();
            timing.link.tx_octets = hci_subevent_le_data_length_change_get_max_tx_octets(packet);
            timing.link.tx_time = hci_subevent_le_data_length_change_get_max_tx_time(packet);
            timing.link.rx_octets = hci_subevent_le_data_length_change_get_max_rx_octets(packet);
            timing.link.rx_time = hci_subevent_le_data_length_change_get_max_rx_time(packet);
            plat_unlock();
            break;
        case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
            util_log("bt: le phy status 0x%02x tx %u rx %u",
                     hci_subevent_le_phy_update_complete_get_status(packet),
                     hci_subevent_le_phy_update_complete_get_tx_phy(packet),
                     packet[7]);
            if (hci_subevent_le_phy_update_complete_get_status(packet)) break;
            plat_lock();
            timing.link.phy_tx = hci_subevent_le_phy_update_complete_get_tx_phy(packet);
            timing.link.phy_rx = packet[7];
            plat_unlock();
            break;
        case HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST:
            if (sm_on) break;
            if (zero(bond.ltk, 16)) {
                hci_send_cmd(&hci_le_long_term_key_negative_reply, con_handle);
                break;
            }
            reverse(bond.ltk, key, 16);     /* HCI takes it least significant byte first */
            hci_send_cmd(&hci_le_long_term_key_request_reply, con_handle, key);
            break;
        }
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        if (!le || !hci_event_encryption_change_get_encryption_enabled(packet)) break;
        if (sm_on) {
            int i = ccc((u16)(dev->input_handle + 1), 1);
            if (i >= 0) ccc_value[i] = 1;
        }
        if (link.le_interval_min)
            gap_request_connection_parameter_update(con_handle, link.le_interval_min,
                link.le_interval_max ? link.le_interval_max : link.le_interval_min, 0, 400);
        else if (dev->interval_min)
            gap_request_connection_parameter_update(con_handle, dev->interval_min,
                                                    dev->interval_max, 0, 400);
        break;
    case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE:
        util_log("bt: le parameter request %s",
                 l2cap_event_connection_parameter_update_response_get_result(packet) ? "refused" : "accepted");
        break;
    case HCI_EVENT_QOS_SETUP_COMPLETE:
        /* status, handle, flags, service type, token rate, peak bandwidth,
         * latency, delay variation */
        util_log("bt: qos status 0x%02x latency %u us", packet[2], little_endian_read_32(packet, 15));
        if (packet[2]) break;
        plat_lock();
        timing.link.poll_us = little_endian_read_32(packet, 15);
        plat_unlock();
        break;
    case HCI_EVENT_MODE_CHANGE:
        util_log("bt: mode %u interval %u", hci_event_mode_change_get_mode(packet),
                 hci_event_mode_change_get_interval(packet));
        plat_lock();
        timing.link.mode = hci_event_mode_change_get_mode(packet);
        timing.link.interval = hci_event_mode_change_get_mode(packet) ? hci_event_mode_change_get_interval(packet) : 0;
        plat_unlock();
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        send_now();
        break;
    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED:
            util_log("bt: hid open status 0x%02x", hid_subevent_connection_opened_get_status(packet));
            if (hid_subevent_connection_opened_get_status(packet)) {
                hid_cid = 0;
                if (!acl_up) unlinked(hid_subevent_connection_opened_get_status(packet));
                break;
            }
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            linked = 1;
            qos_pending = 1;
            input_pending = 0;
            emit(BT_EV_LINKED, 0);
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            util_log("bt: hid closed");
            hid_cid = 0;
            send_pending = 0;
            /* The host closed the HID channels: it means it.  Drop the link
             * too, so the disconnect reports why. */
            if (acl_up) gap_disconnect(con_handle);
            break;
        case HID_SUBEVENT_CAN_SEND_NOW:
            send_now();
            break;
        }
        break;
    }
}

static void on_output(u16 cid, hid_report_type_t type, u16 id, int len, u8 *data)
{
    (void)cid;
    (void)type;
    emit_write(BT_WRITE_OUTPUT, id, data, len);
}

static void on_set_report(u16 cid, hid_report_type_t type, int len, u8 *data)
{
    (void)cid;
    if (len < 1) return;
    emit_write(type == HID_REPORT_TYPE_FEATURE ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT,
               data[0], data + 1, len - 1);
}

/* GET_REPORT is answered at once, on this thread: the profile's read() may
 * only return constant data. */
static int on_get_report(u16 cid, hid_report_type_t type, u16 id, int *len, u8 *out)
{
    int size;

    (void)cid;
    if (!prof->read) return 0;
    size = btstack_hid_get_report_size_for_id(id, type, dev->hid_descriptor, dev->hid_descriptor_len);
    if (size <= 0 || size > MAX_INPUT) return 0;
    if (prof->read(type == HID_REPORT_TYPE_FEATURE ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT,
                   id, 0, out, (u16)size) <= 0)
        return 0;
    *len = size;
    return 1;
}

/* att_server also hands its handler LE connection and disconnection events
 * (deprecated forwarding), which on_event already has from HCI. */
static void on_att(u8 type, u16 channel, u8 *packet, u16 size)
{
    if (type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == ATT_EVENT_CAN_SEND_NOW)
        on_event(type, channel, packet, size);
}

/* ---- sessions ----------------------------------------------------------------------- */

static void setup_classic(void)
{
    hid_sdp_record_t hid;

    sdp_init();
    gap_set_local_name(dev->name);
    gap_set_class_of_device(dev->class_of_device);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_set_security_level(LEVEL_0);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    memset(&hid, 0, sizeof(hid));
    hid.hid_device_subclass = (u16)dev->class_of_device;
    hid.hid_virtual_cable = 1;
    hid.hid_remote_wake = 1;
    hid.hid_reconnect_initiate = 1;
    hid.hid_normally_connectable = true;
    hid.hid_supervision_timeout = 8000;
    hid.hid_descriptor = dev->hid_descriptor;
    hid.hid_descriptor_size = dev->hid_descriptor_len;
    hid.device_name = dev->name;
    memset(sdp_hid, 0, sizeof(sdp_hid));
    hid_create_sdp_record(sdp_hid, sdp_create_service_record_handle(), &hid);
    if (sdp_register_service(sdp_hid)) util_log("bt: SDP HID record not registered");
    memset(sdp_id, 0, sizeof(sdp_id));
    device_id_create_sdp_record(sdp_id, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_USB, dev->vendor_id, dev->product_id,
                                dev->version);
    sdp_register_service(sdp_id);
    hid_device_init(false, dev->hid_descriptor_len, dev->hid_descriptor);
    hid_device_accept_truncated_hid_reports(true);
    hid_device_register_report_data_callback(on_output);
    hid_device_register_set_report_callback(on_set_report);
    hid_device_register_report_request_callback(on_get_report);
    hid_device_register_packet_handler(on_event);
}

static void setup_le(void)
{
    sm_key_t key;

    memset(ccc_handle, 0, sizeof(ccc_handle));
    le_device_db_init();
    /* SMP only for profiles that pair through it: the Switch 2 pairs in its
     * own protocol and drops the link if SMP is attempted. */
    sm_on = dev->sm_er != NULL;
    if (sm_on) {
        sm_init();
        memcpy(key, dev->sm_er, 16);
        sm_set_er(key);
        memcpy(key, dev->sm_ir, 16);
        sm_set_ir(key);
        sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
        sm_registration.callback = on_sm;
        sm_add_event_handler(&sm_registration);
    }
    att_server_init(dev->att_db, att_read, att_write);
    att_server_register_packet_handler(on_att);
}

static void setup(void)
{
    working = acl_up = linked = send_pending = qos_pending = stopping = 0;
    hid_cid = 0;
    hci_jieli_prepare(dev->sniff_slots, dev->sniff_attempt);
    hci_jieli_tx_power(link.tx_step, link.tx_table);
    util_log("bt: tx power step %u %s table", link.tx_step, link.tx_table ? "pro" : "normal");
    hci_jieli_set_address(addr);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    hci_set_link_key_db(&key_db);
    hci_enable_acl_packet_types(ACL_TYPES);
    l2cap_init();
    if (le) setup_le();
    else setup_classic();
    hci_registration.callback = on_event;
    hci_add_event_handler(&hci_registration);
    l2cap_registration.callback = on_event;
    l2cap_add_event_handler(&l2cap_registration);
    btstack_run_loop_set_data_source_handler(&tx_source, tx_poll);
    btstack_run_loop_enable_data_source_callbacks(&tx_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&tx_source);
}

static void teardown(void)
{
    if (le) {
        att_server_deinit();
        if (sm_on) {
            sm_remove_event_handler(&sm_registration);
            sm_deinit();
            /* sm_init() starts the crypto engine once per boot; without this
             * every later session waits forever on LE Encrypt. */
            btstack_crypto_deinit();
            sm_on = 0;
        }
    } else {
        hid_device_deinit();
        sdp_deinit();
    }
    l2cap_deinit();
    hci_deinit();
    btstack_run_loop_deinit();
}

/* The thread stays for the whole boot and parks between sessions. */
static void body(void)
{
    for (;;) {
        if (!(requests & RQ_START)) {
            plat_bt_wait(100);
            continue;
        }
        plat_lock();
        requests = 0;
        plat_unlock();
        setup();
        hci_power_control(HCI_POWER_ON);
        btstack_run_loop_execute();
        teardown();
        working = acl_up = linked = 0;
        hid_cid = 0;
        idle = 1;
        emit(BT_EV_STOPPED, 0);
    }
}
