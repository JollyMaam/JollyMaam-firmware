/* The Bluetooth host on upstream BTstack.  A
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
void hci_jieli_prepare(u16 slots, u16 attempt, int edr3);
void hci_jieli_tx_power(u8 step, u8 pro);
void hci_jieli_loop_stats(u32 *runs, u32 *us);
void hci_jieli_sniff_slots(u16 slots);
void hci_jieli_link_policy(void);
u32 hci_jieli_anchor(u32 *at);
int hci_jieli_anchor_at(u32 n, u32 *at);
void hci_jieli_anchor_period(u32 us);
void hci_jieli_fast_open(u16 handle, void (*done)(u32 n));
void hci_jieli_fast_close(void);
u32 hci_jieli_fast_in_flight(void);
int hci_jieli_fast_send(const u8 *pdu, u16 len);

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
/* Classic: the host's sniff is kept up to this interval (16 slots, 10 ms)
 * and left above it, or when a report waits longer than SNIFF_STALL_US. */
#define SNIFF_KEEP_SLOTS 16
#define SNIFF_STALL_US   50000
/* Just in time (bt_input_due_us): the adaptive lead starts at LEAD_START_US,
 * grows by LEAD_UP_US for each report that missed its anchor by one, and
 * shrinks by LEAD_DOWN_US after LEAD_HITS on time; at least LEAD_MIN_US, and
 * at least LEAD_ROOM_US short of the anchor period. */
#define LEAD_START_US    2500
#define LEAD_MIN_US      1000
#define LEAD_UP_US       250
#define LEAD_DOWN_US     100
#define LEAD_HITS        50
#define LEAD_ROOM_US     1000

enum { RQ_START = 1, RQ_CONNECT = 2, RQ_STOP = 4, RQ_ABORT = 8, RQ_DROP = 16 };

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
static volatile u8 want;                 /* the controller has room and the slot is empty */
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
static u16 sniff_slots;                  /* the host's sniff interval; 0: active */
static u8 sniff_tries;                   /* our own sniff requests this link */
static u32 sniff_next;                   /* not before: 2 s of active mode first */
static btstack_data_source_t tx_source;
static btstack_packet_callback_registration_t hci_registration, sm_registration;
static u8 sdp_hid[720];
static u8 sdp_id[100];
static u8 sm_on, wake_adv, data_length_pending, phy_pending, interval_asked;
static u16 ccc_handle[CCCS];
static u8 ccc_value[CCCS];
static btstack_packet_callback_registration_t l2cap_registration;
/* Reports in the controller, oldest first: when each went in and when it
 * entered the slot, to time Number Of Completed Packets against. */
static struct { u32 sent_us, slot_us, anchor; } flight[IN_FLIGHT_MAX];
static u8 flight_n;
static u32 last_ack_us;
static u32 want_us;                      /* when bt_input_wanted() last became true */
static volatile u16 le_mtu;              /* the link's ATT MTU */
static u32 lead_us = LEAD_START_US;      /* adaptive just-in-time lead */
static u32 due_us;                       /* the lead before the anchor wanted; 0: none */
static u8 lead_hits;

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
static void le_flush(void);
static void update_want(void);

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
    input_len = 0;
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
void bt_drop(void) { request(RQ_DROP); }

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
    want = 0;
    plat_unlock();
    if (le) le_flush();
    else plat_bt_wake();
    return 1;
}

void bt_stats(struct bt_stats *out)
{
    u32 t;

    plat_lock();
    *out = stats;
    plat_unlock();
    hci_jieli_loop_stats(&out->loop_runs, &out->loop_us);
    out->anchors = hci_jieli_anchor(&t);
}

void bt_timing(struct bt_timing *out, int reset)
{
    plat_lock();
    if (reset) {
        memset(&timing.ack_wait, 0, 4 * sizeof(timing.ack_wait));
        last_ack_us = 0;
    }
    *out = timing;
    plat_unlock();
}

int bt_input_pending(void)
{
    return input_pending;
}

/* Pull: a report handed over now goes to the controller at once. */
int bt_input_wanted(void)
{
    return want;
}

u32 bt_input_wanted_us(void)
{
    u32 t;

    plat_lock();
    t = want ? want_us | 1 : 0;          /* never 0 while wanted */
    plat_unlock();
    return t;
}

/* The central's anchors come every period_us: the connection interval or
 * the sniff interval.  0: no fixed grid.  In Classic active mode the QoS poll
 * interval is only the longest gap: a Linux host (Intel) with data waiting
 * polled every ~1.3 ms, and a report built at once met the next poll. */
static u32 period_us(void)
{
    if (timing.link.transport == 2) return timing.link.interval * 1250u;
    if (timing.link.transport == 1 && timing.link.mode == 2) return timing.link.interval * 625u;
    return 0;
}

/* The lead in use: the slot's jit_us when fixed (1: none), else the
 * adaptive one; never closer than LEAD_ROOM_US to the period. */
static u32 lead(u32 period)
{
    u32 l = link.jit_us == 1 ? 0 : link.jit_us ? link.jit_us : lead_us;

    if (period < LEAD_MIN_US + LEAD_ROOM_US) return 0;
    return l + LEAD_ROOM_US > period ? period - LEAD_ROOM_US : l;
}

/* Just in time: a report wanted now goes out at the next anchor (the one
 * before it is already in the controller), so build it the lead before that
 * anchor instead of at once; a report built at once waited most of an
 * interval in the controller.  At once when the next anchor is closer than
 * the lead, or when the anchors are not known. */
u32 bt_input_due_us(void)
{
    u32 t = 0, p, l, at, next, now;

    plat_lock();
    if (want) {
        t = want_us;
        p = period_us();
        l = lead(p);
        if (l && hci_jieli_anchor(&at)) {
            now = plat_us();
            if (now - at < 8 * p) {
                next = at + p;
                while ((s32)(next - now) <= 0) next += p;
                if ((s32)(next - l - t) > 0) t = next - l;
                due_us = t == next - l ? t | 1 : 0;
            }
        }
        t |= 1;                          /* never 0 while wanted */
    }
    plat_unlock();
    return t;
}

/* A report went out at the anchor it was meant for, or one later: move the
 * adaptive lead.  Later than that is the host or the radio, not the lead. */
static void lead_learn(u32 anchors_late)
{
    u32 p = period_us();

    if (anchors_late == 1) {
        lead_hits = 0;
        if (lead_us + LEAD_UP_US + LEAD_ROOM_US <= p) lead_us += LEAD_UP_US;
    } else if (!anchors_late && ++lead_hits >= LEAD_HITS) {
        lead_hits = 0;
        if (lead_us >= LEAD_MIN_US + LEAD_DOWN_US) lead_us -= LEAD_DOWN_US;
    }
    timing.link.lead_us = lead(p);
    timing.link.period_us = p;
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
    if (le && input_pending) le_flush();      /* room came while it waited */
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

/* After a Classic pairing a host that only paired (a desktop's pairing
 * dialog, bluetoothctl pair) reads SDP and drops the link about 5 s later
 * without opening HID, and the pad showed up only after another connect.
 * A Pro Controller opens HID itself; so do we, if the host has not after
 * this long.  A host that opens HID at once (a Switch) is left alone. */
#define BONDED_OPEN_MS 1500
static btstack_timer_source_t bonded_open;
static void page(void);

static void bonded_open_timeout(btstack_timer_source_t *t)
{
    (void)t;
    if (!acl_up || hid_cid || le) return;
    util_log("bt: host paired without opening HID: opening it");
    page();
}

static void kdb_put(bd_addr_t a, link_key_t key, link_key_type_t type)
{
    reverse(a, bond.peer, 6);
    memcpy(bond.link_key, key, 16);
    bond.key_type = (u8)type;
    util_log("bt: bonded %s type %u", bd_addr_to_str(a), type);
    emit(BT_EV_PAIRED, 0);
    if (pairing && !le) {
        btstack_run_loop_remove_timer(&bonded_open);
        btstack_run_loop_set_timer_handler(&bonded_open, bonded_open_timeout);
        btstack_run_loop_set_timer(&bonded_open, BONDED_OPEN_MS);
        btstack_run_loop_add_timer(&bonded_open);
    }
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
/* Whether to leave the host's sniff mode to send.  Measured 2026-10-05: a
 * Switch 2 holding a Pro Controller link in 5 ms sniff polled it every
 * ~15-30 ms once we left (build to ack 16.9 ms against 9.5 ms in sniff); a
 * Linux host (Intel) in 1.25-5 ms sniff delivered every report on time and
 * in 15 ms sniff 23.7 ms late (8 ms, in clumps, once left).  One 12 s stall
 * was seen in 2.5 ms sniff: a report that waits too long leaves sniff too.
 * We never ask for sniff ourselves by default: asked for 2-8 slots, the Linux
 * host grants 6-12.  (These measurements predate the sniff receive-window
 * fix in hci_jieli_open; the 12 s wedges were that bug.) */
static int have_data(void);

static int sniff_too_slow(void)
{
    if (le || !sniff_slots) return 0;
    if (sniff_slots > SNIFF_KEEP_SLOTS && have_data()) return 1;
    return flight_n && plat_us() - flight[0].sent_us > SNIFF_STALL_US;
}

/* Ask the host for sniff (slot settings sniff_max, sniff_min), after 2 s
 * of active mode - a host that sniffs by itself (a Switch, within ~50 ms
 * of the link) is never asked - and three tries a link.  A Linux host
 * answers the pad's request with its own interval (asked 8 -> 12). */
static void request_sniff(void)
{
    u16 max = link.sniff_max, min = link.sniff_min ? link.sniff_min : link.sniff_max;

    if (le || !hid_cid || !max || sniff_slots || sniff_tries >= 3 ||
        (s32)(plat_ms() - sniff_next) < 0 || !hci_can_send_command_packet_now())
        return;
    sniff_next = plat_ms() + 2000;
    sniff_tries++;
    hci_jieli_sniff_slots(max);
    util_log("bt: asking for sniff %u-%u slots", min, max);
    hci_send_cmd(&hci_sniff_mode, con_handle, max, min, 1, 0);
}

static void unsniff(void)
{
    hci_connection_t *c;

    if (le || !acl_up || (s32)(plat_ms() - unsniff_next) < 0) return;
    c = hci_connection_for_handle(con_handle);
    if (!c || !c->connection_mode) return;
    if (flight_n && plat_us() - flight[0].sent_us > SNIFF_STALL_US) {
        static u32 logged;               /* once a second, not every try */

        if (!logged || plat_ms() - logged >= 1000) {
            logged = plat_ms() | 1;
            util_log("bt: report waiting %u ms in sniff %u slots: leaving it",
                     (plat_us() - flight[0].sent_us) / 1000, sniff_slots);
        }
    }
    hci_send_cmd(&hci_exit_sniff_mode, con_handle);
    unsniff_next = plat_ms() + UNSNIFF_MS;
}

/* ---- LE: advertising and the GATT server --------------------------------------- */

static void advertise(void)
{
    static u8 data[31];          /* BTstack keeps the pointer, not a copy */
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
    /* Always: the controller keeps the last session's otherwise. */
    gap_scan_response_set_data(dev->scan_response_len, (u8 *)dev->scan_response);
    gap_advertisements_enable(1);
    util_log("bt: advertising%s", wake_adv ? " to wake" : pairing ? " to pair" : "");
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
        if (i >= 0 && size && (buf[0] & 1) && handle == dev->input_handle + 1) {
            /* The first report can precede the host's subscription.  Replay
             * the cached state: an on-change profile may otherwise stay
             * silent until a control moves. */
            plat_lock();
            if (input_len) {
                if (!input_pending) slot_us = plat_us();
                input_pending = 1;
                want = 0;
            }
            plat_unlock();
            plat_wake(EV_BT);    /* the control thread sends it (le_flush) */
            update_want();
        }
        util_log("bt: host %s notifications on 0x%04x", size && buf[0] ? "enables" : "disables", handle - 1);
        /* Never encrypted: ask for its interval now, past the link's first
         * procedures (data length, PHY).  Asked at connection, the host's
         * update came ~40 ms in and the pad reset, twice (a clash with
         * those procedures is suspected, not shown).  Its own interval:
         * the link settings are slot south's. */
        if (dev->unencrypted && dev->interval_min && size && (buf[0] & 1) && !interval_asked) {
            interval_asked = 1;  /* once: each padtool run subscribes again */
            gap_request_connection_parameter_update(con_handle, dev->interval_min, dev->interval_max, 0, 400);
        }
        return 0;
    }
    emit_write(BT_WRITE_ATT, handle, buf, size);
    return 0;
}

/* A Secure Connections key is the pairing's own: the security manager keeps
 * it only in RAM, so the bond takes it and the host's identity along. */
static void bond_from_sm(void)
{
    int i = sm_le_device_index(con_handle), type, size, auth, authz, sc;
    u16 ediv;
    u8 rand[8];
    bd_addr_t a;
    sm_key_t irk, ltk;

    if (i < 0) return;
    le_device_db_encryption_get(i, &ediv, rand, ltk, &size, &auth, &authz, &sc);
    if (!sc) return;
    le_device_db_info(i, &type, a, irk);
    memcpy(bond.ltk, ltk, 16);
    memcpy(bond.le_irk, irk, 16);
    reverse(a, bond.le_id, 6);
    bond.le_id_type = (u8)type;
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
        if (sm_event_pairing_complete_get_status(packet)) break;
        bond_from_sm();
        emit(BT_EV_PAIRED, 0);
        break;
    }
}

/* ---- sending -------------------------------------------------------------------- */

/* LE: from any thread (the fast path's count); Classic: the bluetooth thread. */
static int room(void)
{
    hci_connection_t *c;

    if (le) return hci_jieli_fast_in_flight() < link.in_flight;
    c = hci_connection_for_handle(con_handle);
    return !c || c->num_packets_sent < link.in_flight;
}

/* For this thread to send: LE input goes from the control thread. */
static int have_data(void)
{
    return (!le && input_pending) || msg_read != msg_write;
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
    } else if (!le && input_pending && room()) {
        m.handle = dev->input_handle;
        m.len = input_len;
        memcpy(m.data, input, input_len);
        input_pending = 0;
        slot = slot_us;
        stats.sent++;
        if (want_us) {
            util_stat_add(&timing.want_send, plat_us() - want_us);
            want_us = 0;
        }
        util_trace(TR_SENT, 0);
    }
    plat_unlock();
    if (m.len) {
        /* Timed from before the hand-off: handing a packet over wakes the
         * controller task, which can hold this thread for milliseconds. */
        u32 sent = plat_us(), at, anchor = hci_jieli_anchor(&at);

        if (!le) hid_device_send_interrupt_message(hid_cid, m.data, m.len);
        else if (m.handle != dev->input_handle || notifying(m.handle))
            att_server_notify(con_handle, m.handle, m.data, m.len);
        util_trace(TR_HANDED, 0);
        /* Messages ride along unmarked (slot 0): only reports are timed.
         * LE: flight[] is the fast path's alone. */
        if (!le && flight_n < IN_FLIGHT_MAX) {
            flight[flight_n].sent_us = sent;
            flight[flight_n].slot_us = slot;
            flight[flight_n].anchor = anchor;
            flight_n++;
        }
    }
    send_pending = 0;
    if (have_data()) plat_bt_wake();
    /* Room left after this one: ask for the next report now, so the
     * controller holds one for the next connection event. */
    update_want();
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
    if ((r & RQ_DROP) && acl_up) gap_disconnect(con_handle);
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
    /* Ask once after HID is ready. QoS failure does not stop input. */
    if (qos_pending && acl_up && hci_can_send_command_packet_now()) {
        qos_pending = 0;
        if (link.poll_us) {
            util_log("bt: qos request %u us (best effort)", link.poll_us);
            hci_send_cmd(&hci_qos_setup, con_handle, 0, 1, 0, 0, (u32)link.poll_us, 0xffffffffu);
        }
    }
    if (sniff_too_slow()) unsniff();
    request_sniff();
    if (!send_pending && linked && have_data() && (msg_read != msg_write || room())) {
        /* Straight in when the stack has a buffer: the can-send-now event
         * round trip cost 0.3-0.9 ms between a report and the controller. */
        if (le ? att_server_can_send_packet_now(con_handle) : hci_can_send_acl_packet_now(con_handle)) {
            send_now();
            return;
        }
        send_pending = 1;
        if (le) att_server_request_can_send_now_event(con_handle);
        else hid_device_request_can_send_now_event(hid_cid);
    }
    update_want();
}

/* Ask the control thread for a report when one would go straight in:
 * linked, nothing waiting in the slot, room in the controller. */
static void update_want(void)
{
    u8 r = linked && (le ? notifying(dev->input_handle) : !send_pending) && room(), wake = 0;

    plat_lock();
    if (r && !input_pending && !want) {
        want = wake = 1;
        want_us = plat_us();
        util_trace(TR_WANT, 0);
    } else if (!r) {
        want = 0;
    }
    plat_unlock();
    if (wake) plat_wake(EV_BT);
}

/* ---- link events ------------------------------------------------------------------ */

/* Our page gave way to the host's own connection: the controller ends a page
 * with this (non-standard) status when the paged device connects to us first
 * (create_link_connection).  BlueZ retries a known HID device every ~30 s, so
 * a pad that pages it at the same moment met this on about one connect in
 * five, and treating it as a failure dropped the host's link (0x16).  Wait
 * for that link instead; if it does not come, it is an ordinary failure. */
#define PAGE_GAVE_WAY 0xfe
#define GAVE_WAY_MS   3000
static btstack_timer_source_t gave_way;
static void unlinked(u8 reason);

static void gave_way_timeout(btstack_timer_source_t *t)
{
    (void)t;
    if (acl_up) return;
    util_log("bt: the host's connection did not come");
    unlinked(PAGE_GAVE_WAY);
}

/* Our dongle encrypts as it connects (bt_dongle.c).  Sometimes the link
 * then takes none of its control PDUs (the encryption, its PHY and data
 * length requests, its disconnect; not established why), while ours still
 * go through: we end such a link ourselves and connect again. */
#define LINK_ENCRYPT_MS 2000
static btstack_timer_source_t link_watch;
static u8 le_encrypted, link_redo;

static void link_watch_timeout(btstack_timer_source_t *t)
{
    (void)t;
    if (!acl_up || !le || le_encrypted) return;
    util_log("bt: not encrypted after %u ms: connecting again", LINK_ENCRYPT_MS);
    link_redo = 1;
    gap_disconnect(con_handle);
}

static void unlinked(u8 reason)
{
    int was = linked;

    btstack_run_loop_remove_timer(&gave_way);
    btstack_run_loop_remove_timer(&bonded_open);
    btstack_run_loop_remove_timer(&link_watch);
    if (link_redo) reason = ERROR_CODE_LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT;   /* lost: again */
    link_redo = 0;

    linked = 0;
    if (le) hci_jieli_fast_close();
    hid_cid = 0;
    send_pending = 0;
    acl_up = 0;
    qos_pending = 0;
    flight_n = 0;
    sniff_slots = 0;
    sniff_tries = 0;
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
    lead_us = LEAD_START_US;
    lead_hits = 0;
    hci_jieli_anchor_period(0);
    plat_lock();
    flight_n = 0;
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
    hci_jieli_anchor_period(period_us());
}

/* A report the controller says went (flight[n], under plat_lock): its
 * timing, and the adaptive lead. */
static void flight_done(u8 n, u32 now, u32 anchor)
{
    u32 air;

    util_stat_add(&timing.ack_wait, now - flight[n].sent_us);
    /* It went at the first anchor after the hand-off, and the central
     * acknowledged it at the next one; acknowledged later, it went at
     * the anchor before the acknowledging one (or was sent again). */
    if (flight[n].slot_us && anchor > flight[n].anchor) {
        u32 late = anchor - flight[n].anchor > 2 ? anchor - flight[n].anchor - 2 : 0;
        u32 went = late ? anchor - 1 : flight[n].anchor + 1;

        /* Without a fixed grid (active mode) a poll may come at any
         * time: no anchor is the one a report was meant for. */
        if (period_us()) {
            if (late) stats.late++;
            lead_learn(late);
        }
        if (hci_jieli_anchor_at(went, &air) && (s32)(air - flight[n].slot_us) > 0)
            util_stat_add(&timing.slot_air, air - flight[n].slot_us);
    }
}

/* Classic: Number Of Completed Packets, the controller says reports went.
 * BTstack's own packets (L2CAP) are acked here too and are not in flight[];
 * whatever the connection has outstanding beyond ours is theirs. */
static void acked(void)
{
    hci_connection_t *c = hci_connection_for_handle(con_handle);
    u32 now = plat_us(), at, anchor = hci_jieli_anchor(&at);
    int left = c ? c->num_packets_sent : 0;
    u8 i, n = 0;

    plat_lock();
    if (last_ack_us) util_stat_add(&timing.ack_gap, now - last_ack_us);
    util_trace(TR_ACK, (u16)(flight_n - left));
    last_ack_us = now;
    while (flight_n - n > left && n < flight_n) flight_done(n++, now, anchor);
    plat_unlock();
    for (i = n; i < flight_n; i++) flight[i - n] = flight[i];
    flight_n = (u8)(flight_n - n);
    /* Room appears here.  tx_poll ran before this event in the same pass
     * (BTstack polls the data source added last first): ask now. */
    update_want();
    if (have_data()) plat_bt_wake();
}

/* LE: n of our reports went (the fast path's, in the controller's context).
 * Room for the next: ask the control thread for it, or wake it to send the
 * one waiting. */
static void fast_done(u32 n)
{
    u32 now = plat_us(), at, anchor = hci_jieli_anchor(&at);
    u8 i, k = 0;

    plat_lock();
    stats.acked += n;
    if (last_ack_us) util_stat_add(&timing.ack_gap, now - last_ack_us);
    util_trace(TR_ACK, (u16)n);
    last_ack_us = now;
    while (k < n && k < flight_n) flight_done(k++, now, anchor);
    for (i = k; i < flight_n; i++) flight[i - k] = flight[i];
    flight_n = (u8)(flight_n - k);
    plat_unlock();
    update_want();
    if (input_pending) plat_wake(EV_BT);
}

/* LE, on the control thread only (one sender keeps the newest last): the
 * waiting report as a notification, straight to the controller. */
static void le_flush(void)
{
    u8 pdu[4 + 3 + MAX_INPUT];
    u16 n;
    u32 at;

    if (!linked || !input_pending || !notifying(dev->input_handle) || !room()) return;
    plat_lock();
    n = input_len;
    if (n + 3 > le_mtu) {        /* as att_server_notify: too long for the link */
        input_pending = 0;
        plat_unlock();
        return;
    }
    little_endian_store_16(pdu, 0, (u16)(n + 3));
    little_endian_store_16(pdu, 2, L2CAP_CID_ATTRIBUTE_PROTOCOL);
    pdu[4] = ATT_HANDLE_VALUE_NOTIFICATION;
    little_endian_store_16(pdu, 5, dev->input_handle);
    memcpy(pdu + 7, input, n);
    input_pending = 0;
    stats.sent++;
    if (want_us) {
        util_stat_add(&timing.want_send, plat_us() - want_us);
        want_us = 0;
    }
    if (flight_n < IN_FLIGHT_MAX) {
        flight[flight_n].sent_us = plat_us();
        flight[flight_n].slot_us = slot_us;
        flight[flight_n].anchor = hci_jieli_anchor(&at);
        /* Handed over after the anchor it was built for: count from that
         * one, so the lead learns it was late. */
        if (due_us && (s32)(at - due_us) > 0 && flight[flight_n].anchor)
            flight[flight_n].anchor--;
        flight_n++;
    }
    due_us = 0;
    plat_unlock();
    util_trace(TR_SENT, 0);
    hci_jieli_fast_send(pdu, (u16)(n + 7));
    util_trace(TR_HANDED, 0);
    update_want();
}

/* The slot's interval, else the device's. */
static void request_interval(void)
{
    if (link.le_interval_min)
        gap_request_connection_parameter_update(con_handle, link.le_interval_min,
            link.le_interval_max ? link.le_interval_max : link.le_interval_min, 0, 400);
    else if (dev->interval_min)
        gap_request_connection_parameter_update(con_handle, dev->interval_min,
                                                dev->interval_max, 0, 400);
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
    le_mtu = ATT_DEFAULT_MTU;
    hci_jieli_fast_open(con_handle, fast_done);
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
    le_encrypted = 0;
    interval_asked = 0;
    if (dev == profile_link.device) {
        btstack_run_loop_set_timer_handler(&link_watch, link_watch_timeout);
        btstack_run_loop_set_timer(&link_watch, LINK_ENCRYPT_MS);
        btstack_run_loop_add_timer(&link_watch);
    }
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
                if (acl_up && !le) acked();      /* LE: BTstack's own (fast_done has ours) */
            }
        break;
    }
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            util_log("bt: working");
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
        btstack_run_loop_remove_timer(&gave_way);
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
        le_encrypted = 1;
        if (sm_on) {
            int i = ccc((u16)(dev->input_handle + 1), 1);
            if (i >= 0) ccc_value[i] = 1;
        }
        request_interval();
        break;
    case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE:
        util_log("bt: le parameter request %s",
                 l2cap_event_connection_parameter_update_response_get_result(packet) ? "refused" : "accepted");
        break;
    case HCI_EVENT_QOS_SETUP_COMPLETE:
        if (size < 23 || packet[1] != 21 || le || !acl_up ||
            little_endian_read_16(packet, 3) != con_handle) break;
        util_log("bt: qos status 0x%02x accepted poll %u us", packet[2], little_endian_read_32(packet, 15));
        if (packet[2]) break;
        plat_lock();
        timing.link.poll_us = little_endian_read_32(packet, 15);
        plat_unlock();
        break;
    case HCI_EVENT_QOS_VIOLATION:
        if (size < 4 || le || !acl_up || little_endian_read_16(packet, 2) != con_handle) break;
        util_log("bt: qos no longer met");
        plat_lock();
        timing.link.poll_us = 0;
        plat_unlock();
        break;
    case HCI_EVENT_COMMAND_STATUS:
        if (hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_QOS_SETUP &&
            hci_event_command_status_get_status(packet))
            util_log("bt: qos request refused 0x%02x", hci_event_command_status_get_status(packet));
        if ((hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_SNIFF_MODE ||
             hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_EXIT_SNIFF_MODE) &&
            hci_event_command_status_get_status(packet))
            util_log("bt: %s refused 0x%02x",
                     hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_SNIFF_MODE
                         ? "sniff" : "exit sniff", hci_event_command_status_get_status(packet));
        break;
    case HCI_EVENT_MODE_CHANGE:
        util_log("bt: mode %u interval %u", hci_event_mode_change_get_mode(packet),
                 hci_event_mode_change_get_interval(packet));
        plat_lock();
        timing.link.mode = hci_event_mode_change_get_mode(packet);
        timing.link.interval = hci_event_mode_change_get_mode(packet) ? hci_event_mode_change_get_interval(packet) : 0;
        plat_unlock();
        hci_jieli_anchor_period(period_us());
        sniff_slots = hci_event_mode_change_get_mode(packet) == 2 ? hci_event_mode_change_get_interval(packet) : 0;
        if (!sniff_slots) sniff_next = plat_ms() + 2000;
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        send_now();
        break;
    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED:
            util_log("bt: hid open status 0x%02x", hid_subevent_connection_opened_get_status(packet));
            if (hid_subevent_connection_opened_get_status(packet) == PAGE_GAVE_WAY && !acl_up) {
                hid_cid = 0;
                util_log("bt: page gave way to the host's connection");
                btstack_run_loop_set_timer_handler(&gave_way, gave_way_timeout);
                btstack_run_loop_set_timer(&gave_way, GAVE_WAY_MS);
                btstack_run_loop_add_timer(&gave_way);
                break;
            }
            if (hid_subevent_connection_opened_get_status(packet)) {
                hid_cid = 0;
                if (!acl_up) unlinked(hid_subevent_connection_opened_get_status(packet));
                break;
            }
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            sniff_next = plat_ms() + 2000;
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
        case HID_SUBEVENT_SUSPEND:
            util_log("bt: host HID suspend");
            break;
        case HID_SUBEVENT_EXIT_SUSPEND:
            util_log("bt: host HID exit suspend");
            break;
        case HID_SUBEVENT_VIRTUAL_CABLE_UNPLUG:
            util_log("bt: host HID virtual cable unplug");
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
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == ATT_EVENT_CAN_SEND_NOW) on_event(type, channel, packet, size);
    if (hci_event_packet_get_type(packet) == ATT_EVENT_MTU_EXCHANGE_COMPLETE)
        le_mtu = att_event_mtu_exchange_complete_get_MTU(packet);
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

#ifdef BT_SMP_LOG
#include "hci_dump.h"

/* -DBT_SMP_LOG: LE pairing in the log: the SMP PDUs, the controller's
 * encryption and P-256 commands and events, and BTstack's errors. */
static void smp_log_packet(u8 type, u8 in, u8 *p, u16 len)
{
    char hex[3 * 18 + 1];
    u16 op, i, n;

    if (type == HCI_ACL_DATA_PACKET && len >= 9 && little_endian_read_16(p, 6) == L2CAP_CID_SECURITY_MANAGER_PROTOCOL) {
        n = (u16)(len - 8 < 18 ? len - 8 : 18);
        for (i = 0; i < n; i++) snprintf(hex + 3 * i, 4, " %02x", p[8 + i]);
        hex[3 * n] = 0;
        util_log("smp %s%s", in ? "<" : ">", hex);
        return;
    }
    if (type == HCI_COMMAND_DATA_PACKET && len >= 2) {
        op = little_endian_read_16(p, 0);
        if (op == 0x2017 || op == 0x2018 || op == 0x201a || op == 0x201b || op == 0x2019 ||
            op == 0x2025 || op == 0x2026)
            util_log("hci cmd %04x", op);
        return;
    }
    if (type != HCI_EVENT_PACKET || len < 2) return;
    switch (p[0]) {
    case HCI_EVENT_ENCRYPTION_CHANGE:
    case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
        util_log("hci ev %02x status %02x", p[0], p[2]);
        break;
    case HCI_EVENT_COMMAND_COMPLETE:
        op = little_endian_read_16(p, 3);
        if (len >= 6 && (op == 0x2017 || op == 0x2018 || op == 0x201a || op == 0x201b))
            util_log("hci done %04x status %02x", op, p[5]);
        break;
    case HCI_EVENT_COMMAND_STATUS:
        op = little_endian_read_16(p, 4);
        if (len >= 6 && (op == 0x2017 || op == 0x2018 || op == 0x201a || op == 0x201b || op == 0x2019 ||
                         op == 0x2025 || op == 0x2026))
            util_log("hci status %04x %02x", op, p[2]);
        break;
    case HCI_EVENT_LE_META:
        if (p[2] == HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST) util_log("hci ev ltk request");
        if (p[2] == 0x08 || p[2] == 0x09) util_log("hci ev p256 %02x status %02x", p[2], p[3]);
        break;
    }
}

static void smp_log_message(int level, const char *format, va_list ap)
{
    char text[96];

    (void)level;
    vsnprintf(text, sizeof(text), format, ap);
    util_log("btstack: %s", text);
}

static const hci_dump_t smp_log = { NULL, smp_log_packet, smp_log_message };
#endif

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
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
        sm_registration.callback = on_sm;
        sm_add_event_handler(&sm_registration);
        if (!zero(bond.ltk, 16) && !zero(bond.le_id, 6)) {
            u8 rand[8] = { 0 };
            bd_addr_t a;
            int i;

            memcpy(key, bond.le_irk, 16);
            reverse(bond.le_id, a, 6);
            i = le_device_db_add(bond.le_id_type, a, key);
            memcpy(key, bond.ltk, 16);
            if (i >= 0) le_device_db_encryption_set(i, 0, rand, key, 16, 0, 0, 1);
        }
#ifdef BT_SMP_LOG
        hci_dump_init(&smp_log);
#endif
    }
    att_server_init(dev->att_db, att_read, att_write);
    att_server_register_packet_handler(on_att);
}

static void setup(void)
{
    working = acl_up = linked = send_pending = qos_pending = stopping = 0;
    hid_cid = 0;
    hci_jieli_prepare(dev->sniff_slots, dev->sniff_attempt, !link.edr3_off);
    hci_jieli_tx_power(link.tx_power < 5 ? link.tx_power : 4, link.tx_power >= 5);
    util_log("bt: tx power %u", link.tx_power + 1);
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
#ifdef BT_SMP_LOG
            hci_dump_init(NULL);
#endif
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
