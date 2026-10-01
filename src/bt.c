/* The Bluetooth host: a profile's bt_device turned into SDP records (Classic)
 * on upstream BTstack; architecture/bluetooth.md.  BTstack runs on its own
 * thread; the control thread talks to it through the requests, the input
 * slot and the message queue below, and hears back through bt_poll(). */
#include <string.h>
#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_hid_parser.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
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
void hci_jieli_link_policy(void);

#define MAX_REPORT     80
#define MESSAGES       8
#define EVENTS         8
#define IN_FLIGHT      2
/* Single-slot ACL packets only (DM1 DH1 2-DH1 3-DH1), as the stock firmware
 * pages: a report never rides a longer packet that fails more often. */
#define ACL_TYPES      0x001e
#define UNSNIFF_MS     50

enum { RQ_START = 1, RQ_CONNECT = 2, RQ_DISCONNECT = 4, RQ_STOP = 8, RQ_ABORT = 16 };

struct report { u16 len; u8 data[MAX_REPORT]; };

static const struct profile *prof;
static u8 addr[6];                       /* ours, least significant byte first */
static struct bt_bond bond;
static u8 pairing;

static volatile u8 requests;
static volatile u8 idle = 1;
static volatile u8 loop_up;

static struct report input;
static volatile u8 input_pending;
static struct report messages[MESSAGES];
static volatile u8 msg_read, msg_write;

static struct bt_ev events[EVENTS];
static volatile u8 ev_read, ev_write;

/* bluetooth thread only */
static u8 working;
static u8 acl_up;
static u8 linked;
static u8 send_pending;
static u8 qos_pending;
static u16 hid_cid;
static hci_con_handle_t con_handle;
static u32 unsniff_next;
static btstack_data_source_t tx_source;
static btstack_packet_callback_registration_t hci_registration, l2cap_registration;
static u8 sdp_hid[720];
static u8 sdp_id[100];

/* ---- control thread side ------------------------------------------------- */

static void reverse(const u8 in[6], u8 out[6])
{
    u32 i;
    for (i = 0; i < 6; i++) out[i] = in[5 - i];
}

static void request(u8 r)
{
    plat_lock();
    requests |= r;
    plat_unlock();
    plat_bt_wake();
}

static void body(void);

void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *b, int pair)
{
    static u8 thread;

    prof = p;
    memcpy(addr, address, 6);
    memset(&bond, 0, sizeof(bond));
    if (b) bond = *b;
    pairing = (u8)pair;
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

int bt_send_input(u8 id, const u8 *data, u16 len)
{
    if (len + 2 > MAX_REPORT || !hid_cid) return 0;
    plat_lock();
    input.data[0] = 0xa1;
    input.data[1] = id;
    memcpy(input.data + 2, data, len);
    input.len = len + 2;
    input_pending = 1;
    plat_unlock();
    plat_bt_wake();
    return 1;
}

int bt_send_message(u8 id, const u8 *data, u16 len)
{
    u8 next;

    if (len + 2 > MAX_REPORT || !hid_cid) return 0;
    plat_lock();
    next = (u8)((msg_write + 1) % MESSAGES);
    if (next == msg_read) {
        plat_unlock();
        return 0;
    }
    messages[msg_write].data[0] = 0xa1;
    messages[msg_write].data[1] = id;
    memcpy(messages[msg_write].data + 2, data, len);
    messages[msg_write].len = len + 2;
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

/* ---- bluetooth thread ---------------------------------------------------- */

static void emit(u8 kind, u8 reason)
{
    struct bt_ev *e;
    u8 next = (u8)((ev_write + 1) % EVENTS);

    if (next == ev_read) return;
    e = &events[ev_write];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->reason = reason;
    e->lost = reason == ERROR_CODE_CONNECTION_TIMEOUT || reason == ERROR_CODE_LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT;
    e->bond = bond;
    ev_write = next;
    plat_wake(EV_BT);
}

static void emit_write(u8 kind, u16 id, const u8 *data, int len)
{
    struct bt_ev *e;
    u8 next = (u8)((ev_write + 1) % EVENTS);

    if (next == ev_read || len < 0 || len > BT_WRITE_MAX) return;
    e = &events[ev_write];
    e->kind = BT_EV_WRITE;
    e->write.kind = kind;
    e->write.id = id;
    e->write.len = (u16)len;
    memcpy(e->write.data, data, (u32)len);
    ev_write = next;
    plat_wake(EV_BT);
}

/* Link keys: only the slot's own bond, from the copy given to bt_start(). */
static void kdb_nop(void) {}
static void kdb_addr(bd_addr_t a) { (void)a; }

static int has_key(void)
{
    static const u8 zero[16];
    return memcmp(bond.link_key, zero, 16) != 0;
}

static int kdb_get(bd_addr_t a, link_key_t key, link_key_type_t *type)
{
    u8 peer[6];

    reverse(a, peer);
    if (!has_key() || memcmp(peer, bond.peer, 6)) {
        util_log("bt: no key for %s", bd_addr_to_str(a));
        return 0;
    }
    memcpy(key, bond.link_key, 16);
    *type = (link_key_type_t)bond.key_type;
    return 1;
}

static void kdb_put(bd_addr_t a, link_key_t key, link_key_type_t type)
{
    reverse(a, bond.peer);
    memcpy(bond.link_key, key, 16);
    bond.key_type = (u8)type;
    util_log("bt: bonded %s type %u", bd_addr_to_str(a), type);
    emit(BT_EV_BONDED, 0);
}

static void kdb_delete(bd_addr_t a)
{
    u8 peer[6];

    reverse(a, peer);
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
    if (it->context || !has_key()) return 0;
    it->context = (void *)1;
    reverse(bond.peer, a);
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

static int has_peer(void)
{
    static const u8 zero[6];
    return memcmp(bond.peer, zero, 6) != 0;
}

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

    if (hid_cid || !has_peer()) return;
    reverse(bond.peer, peer);
    gap_set_security_level(LEVEL_2);
    hid_device_connect(peer, &cid);
    gap_set_security_level(LEVEL_0);
}

/* A host holding the link in sniff takes no data between anchors: leave sniff
 * before sending or tearing down, at most every 50 ms. */
static void unsniff(void)
{
    hci_connection_t *c;

    if (!acl_up || (s32)(plat_ms() - unsniff_next) < 0) return;
    c = hci_connection_for_handle(con_handle);
    if (!c || !c->connection_mode) return;
    hci_send_cmd(&hci_exit_sniff_mode, con_handle);
    unsniff_next = plat_ms() + UNSNIFF_MS;
}

static int room(void)
{
    hci_connection_t *c = hci_connection_for_handle(con_handle);
    return !c || c->num_packets_sent < IN_FLIGHT;
}

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
        working = 0;
        hci_power_control(HCI_POWER_OFF);
        return;
    }
    if (r & RQ_DISCONNECT) {
        unsniff();
        if (hid_cid) hid_device_disconnect(hid_cid);
        else if (acl_up) gap_disconnect(con_handle);
    }
    if ((r & RQ_CONNECT) && working) {
        if (acl_up && !hid_cid) gap_disconnect(con_handle);
        else page();
    }
    /* The host polls a peripheral every 25 ms by default, which delivered
     * reports in bursts: ask for a 1.25 ms poll interval. */
    if (qos_pending && acl_up && hci_can_send_command_packet_now()) {
        qos_pending = 0;
        hci_send_cmd(&hci_qos_setup, con_handle, 0, 2, 8000, 0, 1250, 0xffffffffu);
    }
    if (input_pending || msg_read != msg_write) unsniff();
    if (!send_pending && hid_cid && (input_pending || msg_read != msg_write) && room()) {
        send_pending = 1;
        hid_device_request_can_send_now_event(hid_cid);
    }
}

static void send_now(void)
{
    struct report r;

    r.len = 0;
    plat_lock();
    if (msg_read != msg_write) {
        r = messages[msg_read];
        msg_read = (u8)((msg_read + 1) % MESSAGES);
    } else if (input_pending) {
        r = input;
        input_pending = 0;
    }
    plat_unlock();
    if (r.len) hid_device_send_interrupt_message(hid_cid, r.data, r.len);
    send_pending = 0;
    if (input_pending || msg_read != msg_write) plat_bt_wake();
}

static void unlinked(u8 reason)
{
    int was = linked;

    linked = 0;
    hid_cid = 0;
    send_pending = 0;
    if (pairing) scan(1);
    if (was) emit(BT_EV_UNLINKED, reason);
    else if (!pairing) emit(BT_EV_FAILED, reason);
}

static void on_event(u8 type, u16 channel, u8 *packet, u16 size)
{
    bd_addr_t a;

    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            working = 1;
            hci_jieli_set_address(addr);
            reverse(addr, a);
            hci_set_bd_addr(a);
            hci_jieli_link_policy();
            if (pairing) {
                memset(a, 0, 6);
                hci_send_cmd(&hci_delete_stored_link_key, a, 1);
            }
            if (pairing || !has_peer()) scan(1);
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
        hci_event_connection_complete_get_bd_addr(packet, a);
        reverse(a, bond.peer);
        emit(BT_EV_PEER, 0);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        util_log("bt: acl down reason 0x%02x", hci_event_disconnection_complete_get_reason(packet));
        acl_up = 0;
        unlinked(hci_event_disconnection_complete_get_reason(packet));
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

static void on_l2cap(u8 type, u16 channel, u8 *packet, u16 size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case L2CAP_EVENT_INCOMING_CONNECTION:
        util_log("bt: l2cap in psm 0x%02x", l2cap_event_incoming_connection_get_psm(packet));
        break;
    case L2CAP_EVENT_CHANNEL_OPENED:
        util_log("bt: l2cap open psm 0x%02x status 0x%02x", l2cap_event_channel_opened_get_psm(packet),
                 l2cap_event_channel_opened_get_status(packet));
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
    const struct bt_device *d = prof->device;
    int size;

    (void)cid;
    if (!prof->read) return 0;
    size = btstack_hid_get_report_size_for_id(id, type, d->hid_descriptor, d->hid_descriptor_len);
    if (size <= 0 || size > MAX_REPORT) return 0;
    if (prof->read(type == HID_REPORT_TYPE_FEATURE ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT,
                   id, out, (u16)size) <= 0)
        return 0;
    *len = size;
    return 1;
}

static void setup(void)
{
    const struct bt_device *d = prof->device;
    hid_sdp_record_t hid;

    working = acl_up = linked = send_pending = qos_pending = 0;
    hid_cid = 0;
    hci_jieli_prepare(d->sniff_slots, d->sniff_attempt);
    hci_jieli_set_address(addr);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    hci_set_link_key_db(&key_db);
    hci_enable_acl_packet_types(ACL_TYPES);
    l2cap_init();
    l2cap_registration.callback = on_l2cap;
    l2cap_add_event_handler(&l2cap_registration);
    sdp_init();
    gap_set_local_name(d->name);
    gap_set_class_of_device(d->class_of_device);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_set_security_level(LEVEL_0);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    memset(&hid, 0, sizeof(hid));
    hid.hid_device_subclass = (u16)d->class_of_device;
    hid.hid_virtual_cable = 1;
    hid.hid_remote_wake = 1;
    hid.hid_reconnect_initiate = 1;
    hid.hid_normally_connectable = true;
    hid.hid_supervision_timeout = 8000;
    hid.hid_descriptor = d->hid_descriptor;
    hid.hid_descriptor_size = d->hid_descriptor_len;
    hid.device_name = d->name;
    memset(sdp_hid, 0, sizeof(sdp_hid));
    hid_create_sdp_record(sdp_hid, sdp_create_service_record_handle(), &hid);
    if (sdp_register_service(sdp_hid)) util_log("bt: SDP HID record not registered");
    memset(sdp_id, 0, sizeof(sdp_id));
    device_id_create_sdp_record(sdp_id, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_USB, d->vendor_id, d->product_id,
                                d->version);
    sdp_register_service(sdp_id);
    hid_device_init(false, d->hid_descriptor_len, d->hid_descriptor);
    hid_device_accept_truncated_hid_reports(true);
    hid_device_register_report_data_callback(on_output);
    hid_device_register_set_report_callback(on_set_report);
    hid_device_register_report_request_callback(on_get_report);
    hid_device_register_packet_handler(on_event);
    hci_registration.callback = on_event;
    hci_add_event_handler(&hci_registration);
    btstack_run_loop_set_data_source_handler(&tx_source, tx_poll);
    btstack_run_loop_enable_data_source_callbacks(&tx_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&tx_source);
}

static void teardown(void)
{
    hid_device_deinit();
    sdp_deinit();
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
        loop_up = 1;
        hci_power_control(HCI_POWER_ON);
        btstack_run_loop_execute();
        loop_up = 0;
        teardown();
        working = acl_up = linked = 0;
        hid_cid = 0;
        idle = 1;
        emit(BT_EV_STOPPED, 0);
    }
}
