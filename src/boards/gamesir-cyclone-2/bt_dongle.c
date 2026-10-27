/* The dongle's side of the link (link.h): an LE central for up to HUB_SLOTS
 * bonded pads of ours, a bond and a link per slot.  Slot 0 is the dongle's
 * own pad; the others link only in hub mode (dongle_bt_slots).  Bonded pads
 * not linked are on the filter accept list and one connection attempt waits
 * for whichever advertises first; pairing a slot, it scans for a pad
 * advertising LINK_ADV_PAIRING and gives it a new key.  ATT goes straight
 * over its L2CAP channel to link.h's fixed handles: no discovery, no GATT
 * client.  The pads' state and config go to the control thread through
 * newest-wins slots (dongle.h). */
#include <string.h>
#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "bluetooth.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "dongle.h"
#include "link.h"

const hci_transport_t *hci_jieli_transport(void);
const btstack_run_loop_t *hci_jieli_run_loop(void);
void hci_jieli_set_address(const u8 a[6]);
void hci_jieli_prepare(u16 slots, u16 attempt, int edr3);
void hci_jieli_tx_power(u8 step, u8 pro);

#define SLOTS          HUB_SLOTS
#define STORE_BOND(s)  ((s) ? 2 + (s) : 1)   /* syscfg user items 1, 3, ... (2 is the address) */
#define BOND_LE        2         /* bond.valid: an LE bond (1 was the Classic link's) */
#define PAIR_MS        30000
#define KEY_SETTLE_MS  150       /* the pad's control thread files the key before we encrypt */
#define SETUP_MS       3000      /* connected to linked, at most */
#define ATT_MTU        247       /* asked for: an input report in one notification */
#define SCAN_INTERVAL  0x0030    /* 30 ms, scanning all of it */
#define TIMEOUT_10MS   100       /* supervision timeout, 1 s */

static u8 own_addr[6];                    /* controller order, least significant byte first */
static struct bond { u8 peer[6], key[16], type, valid; } bonds[SLOTS];   /* peer: BTstack's order */
static u32 pair_until;                    /* pairing: scanning until then */
static u8 pair_slot;                      /* the slot that pairing fills */
static u8 working, active = 1;            /* active: slots that link (dongle_bt_slots) */
static u8 pairing_connecting;             /* our gap_connect() to a pairing pad is out */
static u8 listed[SLOTS];                  /* on the filter accept list */
static btstack_packet_callback_registration_t hci_reg;
static btstack_timer_source_t timer;
static btstack_data_source_t poll_source;
static struct { u8 flags, forget, active; } pending;
enum { FORGET_BOND = 1, SEND_OUTPUT = 2, SET_ACTIVE = 4 };

/* After the connection: what is left to do, in order. */
enum { STEP_NONE, STEP_MTU, STEP_MTU_WAIT, STEP_RAND, STEP_RAND_WAIT, STEP_KEY, STEP_KEY_WAIT,
       STEP_SETTLE, STEP_ENCRYPT, STEP_ENCRYPT_WAIT, STEP_SUBSCRIBE, STEP_UP };

/* One per slot, on the Bluetooth thread. */
static struct link {
    hci_con_handle_t handle;
    u8 step;
    u8 pairing_conn;             /* this connection came from pairing: a new key */
    u8 peer_is_link;             /* our pad sent its config on this connection */
    u8 phy_pending, length_pending, output_pending;
    u8 dropping;                 /* set up too slowly: disconnecting */
    u32 up_ms;                   /* connected then */
    u8 last_seq, have_seq;
    u8 updating;                 /* a connection update is out */
    u16 interval;                /* the connection's, 1.25 ms units */
    btstack_timer_source_t out_timer, settle_timer;
} links[SLOTS];
static u8 new_key[16], rand_n;            /* pairing: one slot at a time */

/* Bluetooth thread to control thread, under plat_lock. */
static struct {
    struct pad pad;              /* newest; motion samples gathered until taken */
    u8 have_pad;
    u8 raw[LINK_INPUT_SIZE];     /* the newest input as it came, for hub mode */
    u8 have_raw;
    struct link_config config;
    u8 have_config;
    u8 output[LINK_OUTPUT_SIZE];
    struct link_stats stats;
} x[SLOTS];

static int pairing(void)
{
    return pair_until && (s32)(plat_ms() - pair_until) < 0;
}

static void reverse16(const u8 *in, u8 *out)
{
    int i;
    for (i = 0; i < 16; i++) out[i] = in[15 - i];
}

static struct link *link_of(hci_con_handle_t h)
{
    u32 s;

    for (s = 0; s < SLOTS; s++)
        if (links[s].handle == h && h != HCI_CON_HANDLE_INVALID) return &links[s];
    return NULL;
}

/* The links' interval.  In hub mode LINK_INTERVAL only while every bonded
 * slot is linked and none pairs; else HUB_INTERVAL, which leaves the
 * controller room to scan and connect between the links' events. */
static u16 interval(void)
{
    u32 s;

    if (active <= 1) return LINK_INTERVAL;
    if (pairing()) return HUB_INTERVAL;
    for (s = 0; s < active; s++)
        if (bonds[s].valid && links[s].step != STEP_UP) return HUB_INTERVAL;
    return LINK_INTERVAL;
}

/* Linked slots follow interval(), one connection update at a time each. */
static void pace(void)
{
    u16 want = interval();
    u32 s;

    for (s = 0; s < SLOTS; s++) {
        struct link *l = &links[s];

        if (l->step != STEP_UP || l->updating || l->interval == want || !hci_can_send_command_packet_now()) continue;
        util_log("bt: slot %u: interval %u -> %u", s, l->interval, want);
        l->updating = 1;
        l->interval = want;      /* a refused update waits for the next change */
        gap_update_connection_parameters(l->handle, want, want, 0, TIMEOUT_10MS);
    }
}

static u8 slot_of(const struct link *l)
{
    return (u8)(l - links);
}

/* ---- the link: input and config in, output back ----------------------------- */

/* A newer state replaces the one waiting, but its motion samples join the
 * waiting ones (the newest IMU_PER_REPORT): a sample is never sent twice and
 * only dropped when the host falls that far behind. */
static void take_input(struct link *l, const u8 *d, u16 n)
{
    struct pad p;
    u8 seq, flags, s = slot_of(l);
    u32 i;

    if (!link_unpack_input(d, n, &p, &seq, &flags)) return;
    plat_lock();
    if (x[s].have_pad) {
        struct imu_sample all[IMU_PER_REPORT + LINK_IMU_MAX];
        u32 k = 0;

        for (i = 0; i < x[s].pad.imu_count; i++) all[k++] = x[s].pad.imu[i];
        for (i = 0; i < p.imu_count; i++) all[k++] = p.imu[i];
        p.imu_count = (u8)(k > IMU_PER_REPORT ? IMU_PER_REPORT : k);
        memcpy(p.imu, all + (k - p.imu_count), p.imu_count * sizeof(all[0]));
        x[s].stats.replaced++;
    }
    x[s].pad = p;
    x[s].have_pad = 1;
    memcpy(x[s].raw, d, LINK_INPUT_SIZE);
    x[s].have_raw = 1;
    x[s].stats.inputs++;
    if (l->have_seq && (u8)(seq - l->last_seq) > 1) x[s].stats.lost += (u8)(seq - l->last_seq) - 1u;
    plat_unlock();
    l->have_seq = 1;
    l->last_seq = seq;
    plat_wake(EV_BT);
}

static void take_config(struct link *l, const u8 *d, u16 n)
{
    struct link_config c;
    u8 s = slot_of(l);

    if (!link_unpack_config(d, n, &c)) return;
    if (!l->peer_is_link) util_log("bt: slot %u: our pad, type %u", s, c.type);
    l->peer_is_link = 1;
    plat_lock();
    x[s].config = c;
    x[s].have_config = 1;
    plat_unlock();
    plat_wake(EV_BT);
}

/* ---- ATT, by hand ---------------------------------------------------------------- */

static int att_send(hci_con_handle_t h, u8 *pdu, u16 len)
{
    if (!l2cap_can_send_fixed_channel_packet_now(h, L2CAP_CID_ATTRIBUTE_PROTOCOL)) {
        l2cap_request_can_send_fix_channel_now_event(h, L2CAP_CID_ATTRIBUTE_PROTOCOL);
        return 0;
    }
    return l2cap_send_connectionless(h, L2CAP_CID_ATTRIBUTE_PROTOCOL, pdu, len) == 0;
}

static int att_write(hci_con_handle_t h, u8 opcode, u16 attr, const u8 *data, u16 len)
{
    u8 pdu[3 + 20];

    pdu[0] = opcode;
    little_endian_store_16(pdu, 1, attr);
    memcpy(pdu + 3, data, len);
    return att_send(h, pdu, (u16)(3 + len));
}

static int send_output(struct link *l)
{
    u8 out[LINK_OUTPUT_SIZE];

    plat_lock();
    memcpy(out, x[slot_of(l)].output, LINK_OUTPUT_SIZE);
    plat_unlock();
    return att_write(l->handle, ATT_WRITE_COMMAND, LINK_OUTPUT_HANDLE, out, LINK_OUTPUT_SIZE);
}

/* For a fixed channel the "channel" is the connection handle. */
static void on_att(u8 type, u16 channel, u8 *packet, u16 size)
{
    struct link *l;

    if (type == HCI_EVENT_PACKET) {
        if (hci_event_packet_get_type(packet) == L2CAP_EVENT_CAN_SEND_NOW) plat_bt_wake();
        return;
    }
    if (type != ATT_DATA_PACKET || size < 1 || !(l = link_of(channel))) return;
    switch (packet[0]) {
    case ATT_HANDLE_VALUE_NOTIFICATION:
        if (size < 3 || l->step != STEP_UP) break;      /* nothing before encryption */
        if (little_endian_read_16(packet, 1) == LINK_INPUT_HANDLE) take_input(l, packet + 3, (u16)(size - 3));
        else if (little_endian_read_16(packet, 1) == LINK_CONFIG_HANDLE) take_config(l, packet + 3, (u16)(size - 3));
        break;
    case ATT_EXCHANGE_MTU_RESPONSE:
        if (l->step != STEP_MTU_WAIT) break;
        util_log("bt: slot %u: att mtu %u", slot_of(l), size >= 3 ? little_endian_read_16(packet, 1) : 0);
        l->step = l->pairing_conn ? STEP_RAND : STEP_ENCRYPT;
        break;
    case ATT_WRITE_RESPONSE:
        if (l->step != STEP_KEY_WAIT) break;
        util_log("bt: slot %u: key written", slot_of(l));
        l->step = STEP_SETTLE;
        btstack_run_loop_set_timer(&l->settle_timer, KEY_SETTLE_MS);
        btstack_run_loop_add_timer(&l->settle_timer);
        break;
    case ATT_ERROR_RESPONSE:
        util_log("bt: slot %u: att error 0x%02x for 0x%02x on 0x%04x", slot_of(l), size >= 5 ? packet[4] : 0,
                 size >= 2 ? packet[1] : 0, size >= 4 ? little_endian_read_16(packet, 2) : 0);
        if (l->step == STEP_MTU_WAIT) l->step = l->pairing_conn ? STEP_RAND : STEP_ENCRYPT;
        else if (l->step == STEP_KEY_WAIT) gap_disconnect(l->handle);
        break;
    case ATT_EXCHANGE_MTU_REQUEST: {      /* the pad does not ask, but answer if it does */
        u8 r[3] = { ATT_EXCHANGE_MTU_RESPONSE };

        little_endian_store_16(r, 1, ATT_MTU);
        att_send(l->handle, r, sizeof(r));
        break;
    }
    }
    plat_bt_wake();              /* on_poll: the next step */
}

/* ---- connecting ------------------------------------------------------------------ */

/* The slots to reconnect: bonded, active, not linked, not being paired.
 * The filter accept list holds them and one LE Create Connection waits for
 * any (BTstack restarts it when the list changes).  Pairing pauses it: the
 * pairing pad gets a direct connection. */
static void connect_bonded(void)
{
    u32 s, any = 0;

    if (!working) return;
    for (s = 0; s < SLOTS; s++) {
        u8 want = bonds[s].valid && s < active && links[s].handle == HCI_CON_HANDLE_INVALID && !pairing();

        if (want != listed[s]) {
            if (want) gap_whitelist_add((bd_addr_type_t)bonds[s].type, bonds[s].peer);
            else gap_whitelist_remove((bd_addr_type_t)bonds[s].type, bonds[s].peer);
            listed[s] = want;
            if (want) util_log("bt: slot %u: waiting for %s", s, bd_addr_to_str(bonds[s].peer));
        }
        any |= want;
    }
    if (!any) {
        if (!pairing_connecting) gap_connect_cancel();
        return;
    }
    if (pairing_connecting) return;
    gap_set_connection_parameters(SCAN_INTERVAL, SCAN_INTERVAL, interval(), interval(), 0, TIMEOUT_10MS, 0, 0);
    gap_connect_with_whitelist();          /* already: COMMAND_DISALLOWED, fine */
}

static void unlist(u8 s)
{
    if (!listed[s]) return;
    gap_whitelist_remove((bd_addr_type_t)bonds[s].type, bonds[s].peer);
    listed[s] = 0;
}

static void start_pairing(u8 s)
{
    pair_slot = s;
    pair_until = plat_ms() + PAIR_MS;
    util_log("bt: slot %u: pairing: scanning for a pad for %u s", s, PAIR_MS / 1000);
    connect_bonded();                     /* stops reconnecting while pairing */
    if (links[s].handle != HCI_CON_HANDLE_INVALID) gap_disconnect(links[s].handle);
    gap_set_scan_params(0, SCAN_INTERVAL, SCAN_INTERVAL, 0);
    gap_start_scan();
}

static void on_advertisement(u8 *packet)
{
    bd_addr_t a;
    const u8 *d = gap_event_advertising_report_get_data(packet);
    u8 n = gap_event_advertising_report_get_data_length(packet), status;

    if (!pairing() || pairing_connecting || links[pair_slot].handle != HCI_CON_HANDLE_INVALID) return;
    if (n < LINK_ADV_SIZE || memcmp(d, link_adv, LINK_ADV_FLAGS) || !(d[LINK_ADV_FLAGS] & LINK_ADV_PAIRING)) return;
    gap_event_advertising_report_get_address(packet, a);
    gap_stop_scan();
    gap_set_connection_parameters(SCAN_INTERVAL, SCAN_INTERVAL, interval(), interval(), 0, TIMEOUT_10MS, 0, 0);
    status = gap_connect(a, (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet));
    util_log("bt: slot %u: pairing with %s: 0x%02x", pair_slot, bd_addr_to_str(a), status);
    pairing_connecting = status == 0;
    if (status) gap_start_scan();
}

static void on_timer(btstack_timer_source_t *t)
{
    if (pair_until && !pairing()) {
        pair_until = 0;
        util_log("bt: pairing ended");
        gap_stop_scan();
        if (pairing_connecting) gap_connect_cancel();
        connect_bonded();
    }
    btstack_run_loop_set_timer(t, 1000);
    btstack_run_loop_add_timer(t);
}

static void on_out_timer(btstack_timer_source_t *t)
{
    struct link *l = btstack_run_loop_get_timer_context(t);

    if (l->step != STEP_UP) return;
    l->output_pending = 1;
    btstack_run_loop_set_timer(t, LINK_OUTPUT_MS);
    btstack_run_loop_add_timer(t);
}

static void on_settle(btstack_timer_source_t *t)
{
    struct link *l = btstack_run_loop_get_timer_context(t);

    if (l->step == STEP_SETTLE) l->step = STEP_ENCRYPT;
    plat_bt_wake();
}

static void linked(struct link *l)
{
    l->step = STEP_UP;
    l->output_pending = 1;
    util_log("bt: slot %u: link up", slot_of(l));
    btstack_run_loop_set_timer(&l->out_timer, LINK_OUTPUT_MS);
    btstack_run_loop_add_timer(&l->out_timer);
}

/* What one link has left to do after its connection, and output. */
static void link_poll(struct link *l)
{
    u8 pdu[3], key[16], s = slot_of(l);

    if (l->handle == HCI_CON_HANDLE_INVALID) return;
    if (l->phy_pending && hci_can_send_command_packet_now()) {
        l->phy_pending = 0;
        gap_le_set_phy(l->handle, 0, 2, 2, 0);          /* 2M both ways */
    }
    /* Not up in time: again.  Sometimes our control PDUs stall on a link,
     * the disconnect too (the pad ends it then: bt_pad.c's link_watch). */
    if (l->step != STEP_UP && !l->dropping && plat_ms() - l->up_ms >= SETUP_MS) {
        util_log("bt: slot %u: no link after %u ms, disconnecting", s, SETUP_MS);
        l->dropping = 1;
        gap_disconnect(l->handle);
    }
    if (l->length_pending && hci_can_send_command_packet_now()) {
        l->length_pending = 0;
        hci_send_cmd(&hci_le_set_data_length, l->handle, 251, 2120);
    }
    switch (l->step) {
    case STEP_MTU:
        pdu[0] = ATT_EXCHANGE_MTU_REQUEST;
        little_endian_store_16(pdu, 1, ATT_MTU);
        if (att_send(l->handle, pdu, 3)) l->step = STEP_MTU_WAIT;
        break;
    case STEP_RAND:
        if (!hci_can_send_command_packet_now()) break;
        if (!rand_n) util_log("bt: slot %u: new key", s);
        hci_send_cmd(&hci_le_rand);
        l->step = STEP_RAND_WAIT;
        break;
    case STEP_KEY:
        if (att_write(l->handle, ATT_WRITE_REQUEST, LINK_KEY_HANDLE, new_key, 16)) l->step = STEP_KEY_WAIT;
        break;
    case STEP_ENCRYPT:
        if (!hci_can_send_command_packet_now()) break;
        reverse16(l->pairing_conn ? new_key : bonds[s].key, key);   /* as the pad answers it (bt_pad.c) */
        util_log("bt: slot %u: encrypting", s);
        hci_send_cmd(&hci_le_start_encryption, l->handle, 0, 0, 0, key);
        l->step = STEP_ENCRYPT_WAIT;
        break;
    case STEP_SUBSCRIBE: {
        u8 on[2] = { 1, 0 };

        if (att_write(l->handle, ATT_WRITE_COMMAND, LINK_INPUT_HANDLE + 1, on, 2)) linked(l);
        break;
    }
    case STEP_UP:
        if (l->output_pending && send_output(l)) l->output_pending = 0;
        break;
    }
}

/* Requests from the control thread, then each link's steps, whenever there is room. */
static void on_poll(btstack_data_source_t *ds, btstack_data_source_callback_type_t type)
{
    u8 flags, forget, now_active;
    u32 s;

    (void)ds; (void)type;
    plat_lock();
    flags = pending.flags;
    forget = pending.forget;
    now_active = pending.active;
    pending.flags = 0;
    plat_unlock();
    if (flags & SET_ACTIVE && now_active != active) {
        active = now_active;
        util_log("bt: %u slot%s link", active, active == 1 ? "" : "s");
        for (s = active; s < SLOTS; s++)
            if (links[s].handle != HCI_CON_HANDLE_INVALID) gap_disconnect(links[s].handle);
        connect_bonded();
    }
    if (flags & FORGET_BOND) {
        unlist(forget);
        memset(&bonds[forget], 0, sizeof(bonds[forget]));
        dongle_store_write(STORE_BOND(forget), &bonds[forget], sizeof(bonds[forget]));
        util_log("bt: slot %u: bond forgotten%s", forget, set_is_ephemeral() ? " (ephemeral)" : "");
        if (working) {
            start_pairing(forget);
        } else {                          /* BTSTACK_EVENT_STATE starts it */
            pair_slot = forget;
            pair_until = plat_ms() + PAIR_MS;
        }
    }
    for (s = 0; s < SLOTS; s++) {
        if (flags & SEND_OUTPUT) links[s].output_pending = 1;
        link_poll(&links[s]);
    }
    pace();
}

static void up(u8 *packet)
{
    bd_addr_t a;
    bd_addr_type_t type = (bd_addr_type_t)hci_subevent_le_connection_complete_get_peer_address_type(packet);
    u8 status = hci_subevent_le_connection_complete_get_status(packet), from_pairing = pairing_connecting;
    struct link *l = NULL;
    u32 s;

    pairing_connecting = 0;
    hci_subevent_le_connection_complete_get_peer_address(packet, a);
    if (status) {
        /* 0x02: cancelled, BTstack's own to change the list too */
        if (status != ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER)
            util_log("bt: connection to %s: 0x%02x", bd_addr_to_str(a), status);
        if (from_pairing && pairing()) gap_start_scan();
        connect_bonded();
        return;
    }
    if (from_pairing) {
        l = &links[pair_slot];
    } else {
        for (s = 0; s < SLOTS && !l; s++)
            if (bonds[s].valid && links[s].handle == HCI_CON_HANDLE_INVALID && !memcmp(bonds[s].peer, a, 6))
                l = &links[s];
    }
    if (!l) {
        util_log("bt: %s is no slot's", bd_addr_to_str(a));
        gap_disconnect(hci_subevent_le_connection_complete_get_connection_handle(packet));
        connect_bonded();
        return;
    }
    s = slot_of(l);
    l->handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
    util_log("bt: slot %u: le %s interval %u latency %u timeout %u%s", s, bd_addr_to_str(a),
             hci_subevent_le_connection_complete_get_conn_interval(packet),
             hci_subevent_le_connection_complete_get_conn_latency(packet),
             hci_subevent_le_connection_complete_get_supervision_timeout(packet),
             from_pairing ? " (pairing)" : "");
    l->pairing_conn = from_pairing;
    if (from_pairing) {
        memcpy(bonds[s].peer, a, 6);
        bonds[s].type = (u8)type;
    }
    l->step = STEP_MTU;
    if (from_pairing) rand_n = 0;
    l->phy_pending = l->length_pending = 1;
    l->dropping = 0;
    l->up_ms = plat_ms();
    l->output_pending = 0;
    l->updating = 0;
    l->interval = hci_subevent_le_connection_complete_get_conn_interval(packet);
    l->peer_is_link = 0;
    l->have_seq = 0;
    connect_bonded();                     /* off the list; the others wait again */
    plat_bt_wake();
}

static void down(struct link *l, u8 reason)
{
    u8 s = slot_of(l);

    util_log("bt: slot %u: disconnected, reason 0x%02x", s, reason);
    l->handle = HCI_CON_HANDLE_INVALID;
    l->step = STEP_NONE;
    l->pairing_conn = 0;
    l->peer_is_link = 0;
    plat_lock();
    x[s].have_pad = x[s].have_raw = 0;
    plat_unlock();
    btstack_run_loop_remove_timer(&l->out_timer);
    btstack_run_loop_remove_timer(&l->settle_timer);
    if (pairing() && s == pair_slot) {
        gap_set_scan_params(0, SCAN_INTERVAL, SCAN_INTERVAL, 0);
        gap_start_scan();
    }
    connect_bonded();
}

static void encrypted(struct link *l, u8 status, u8 on)
{
    u8 s = slot_of(l);
    u32 k;

    if (l->step != STEP_ENCRYPT_WAIT) return;
    if (status || !on) {
        util_log("bt: slot %u: encryption failed 0x%02x: %s", s, status, l->pairing_conn ? "pair again" : "not our bond");
        gap_disconnect(l->handle);
        return;
    }
    if (l->pairing_conn) {
        /* The pad has this key now: a slot that held it before has lost it. */
        for (k = 0; k < SLOTS; k++)
            if (k != s && bonds[k].valid && !memcmp(bonds[k].peer, bonds[s].peer, 6)) {
                unlist((u8)k);
                memset(&bonds[k], 0, sizeof(bonds[k]));
                dongle_store_write(STORE_BOND(k), &bonds[k], sizeof(bonds[k]));
                util_log("bt: slot %u: bond moved to slot %u", k, s);
                if (links[k].handle != HCI_CON_HANDLE_INVALID) gap_disconnect(links[k].handle);
            }
        memcpy(bonds[s].key, new_key, 16);
        bonds[s].valid = BOND_LE;
        pair_until = 0;
        l->pairing_conn = 0;
        gap_stop_scan();
        util_log("bt: slot %u: bonded with %s %s: %d", s, bd_addr_to_str(bonds[s].peer),
                 set_is_ephemeral() ? "in RAM" : "saved", dongle_store_write(STORE_BOND(s), &bonds[s], sizeof(bonds[s])));
        connect_bonded();                 /* pairing over: the others reconnect */
    }
    util_log("bt: slot %u: encrypted", s);
    if (s >= active) {
        util_log("bt: slot %u links in hub mode only", s);
        gap_disconnect(l->handle);
        return;
    }
    l->step = STEP_SUBSCRIBE;
    plat_bt_wake();
}

static void on_packet(u8 type, u16 channel, u8 *packet, u16 size)
{
    bd_addr_t a;
    struct link *l;
    u32 i;

    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING || working) break;
        working = 1;
        /* HCI Reset restored the factory address. */
        hci_jieli_set_address(own_addr);
        for (i = 0; i < 6; i++) a[i] = own_addr[5 - i];
        hci_set_bd_addr(a);
        util_log("bt: central as %s", bd_addr_to_str(a));
        for (i = 0; i < SLOTS; i++)
            util_log("bt: slot %u: bond %s", i, bonds[i].valid ? bd_addr_to_str(bonds[i].peer) : "none");
        btstack_run_loop_set_timer_handler(&timer, on_timer);
        btstack_run_loop_set_timer(&timer, 1000);
        btstack_run_loop_add_timer(&timer);
        if (pairing()) start_pairing(pair_slot);
        else connect_bonded();
        break;
    case GAP_EVENT_ADVERTISING_REPORT:
        on_advertisement(packet);
        break;
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            up(packet);
            break;
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            l = link_of(hci_subevent_le_connection_update_complete_get_connection_handle(packet));
            util_log("bt: slot %d: le update status 0x%02x interval %u", l ? slot_of(l) : -1,
                     hci_subevent_le_connection_update_complete_get_status(packet),
                     hci_subevent_le_connection_update_complete_get_conn_interval(packet));
            if (!l) break;
            l->updating = 0;
            if (!hci_subevent_le_connection_update_complete_get_status(packet))
                l->interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            plat_bt_wake();
            break;
        case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
            util_log("bt: le data length tx %u rx %u",
                     hci_subevent_le_data_length_change_get_max_tx_octets(packet),
                     hci_subevent_le_data_length_change_get_max_rx_octets(packet));
            break;
        case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
            l = link_of(hci_subevent_le_phy_update_complete_get_connection_handle(packet));
            util_log("bt: slot %d: le phy status 0x%02x tx %u rx %u", l ? slot_of(l) : -1,
                     hci_subevent_le_phy_update_complete_get_status(packet),
                     hci_subevent_le_phy_update_complete_get_tx_phy(packet), packet[7]);
            break;
        }
        break;
    case HCI_EVENT_COMMAND_COMPLETE:
        if (hci_event_command_complete_get_command_opcode(packet) == HCI_OPCODE_HCI_LE_RAND) {
            const u8 *r = hci_event_command_complete_get_return_parameters(packet);
            u32 us = plat_us();

            l = &links[pair_slot];
            if (l->step != STEP_RAND_WAIT) break;
            /* Two LE Rands, the time mixed in. */
            for (i = 0; i < 8; i++) new_key[8 * rand_n + i] = (u8)(r[1 + i] ^ (us >> (8 * (i & 3))));
            l->step = ++rand_n < 2 ? STEP_RAND : STEP_KEY;
            plat_bt_wake();
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if ((l = link_of(hci_event_disconnection_complete_get_connection_handle(packet))))
            down(l, hci_event_disconnection_complete_get_reason(packet));
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        if ((l = link_of(hci_event_encryption_change_get_connection_handle(packet))))
            encrypted(l, hci_event_encryption_change_get_status(packet),
                      hci_event_encryption_change_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_COMMAND_STATUS:
        if (hci_event_command_status_get_status(packet))
            util_log("bt: command %04x status 0x%02x", hci_event_command_status_get_command_opcode(packet),
                     hci_event_command_status_get_status(packet));
        /* A refused update ends it (pace() leaves it until the interval wanted changes). */
        if (hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_LE_CONNECTION_UPDATE &&
            hci_event_command_status_get_status(packet))
            for (i = 0; i < SLOTS; i++) links[i].updating = 0;
        /* Start Encryption refused: the link waiting for it (one at a time). */
        if (hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_LE_START_ENCRYPTION &&
            hci_event_command_status_get_status(packet))
            for (i = 0; i < SLOTS; i++)
                if (links[i].step == STEP_ENCRYPT_WAIT) {
                    encrypted(&links[i], hci_event_command_status_get_status(packet), 0);
                    break;
                }
        break;
    default:
        break;
    }
}

static void body(void)
{
    u32 s;

    dongle_identity(own_addr);
    for (s = 0; s < SLOTS; s++) {
        struct link *l = &links[s];

        if (plat_store_read(STORE_BOND(s), &bonds[s], sizeof(bonds[s])) != sizeof(bonds[s]) ||
            bonds[s].valid != BOND_LE || !dongle_address_valid(bonds[s].peer))
            memset(&bonds[s], 0, sizeof(bonds[s]));
        l->handle = HCI_CON_HANDLE_INVALID;
        btstack_run_loop_set_timer_handler(&l->out_timer, on_out_timer);
        btstack_run_loop_set_timer_context(&l->out_timer, l);
        btstack_run_loop_set_timer_handler(&l->settle_timer, on_settle);
        btstack_run_loop_set_timer_context(&l->settle_timer, l);
    }
    hci_jieli_prepare(0, 0, 0);
    hci_jieli_tx_power(4, 1);
    hci_jieli_set_address(own_addr);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    l2cap_init();
    l2cap_register_fixed_channel(on_att, L2CAP_CID_ATTRIBUTE_PROTOCOL);
    hci_reg.callback = on_packet;
    hci_add_event_handler(&hci_reg);
    btstack_run_loop_set_data_source_handler(&poll_source, on_poll);
    btstack_run_loop_enable_data_source_callbacks(&poll_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&poll_source);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}

/* USB callers only enqueue requests: bond writes and BTstack calls run on
 * the Bluetooth thread. */
void dongle_bt_forget(u8 slot)
{
    if (slot >= SLOTS) return;
    plat_lock();
    pending.flags |= FORGET_BOND;
    pending.forget = slot;
    plat_unlock();
    plat_bt_wake();
}

void dongle_bt_slots(u8 n)
{
    if (n < 1 || n > SLOTS) n = SLOTS;
    plat_lock();
    pending.flags |= SET_ACTIVE;
    pending.active = n;
    plat_unlock();
    plat_bt_wake();
}

/* For usb.c's status: a link up, else pairing, else waiting for a pad. */
u8 dongle_bt_state(void)
{
    u32 s;

    if (!working) return CTL_BOOT;
    for (s = 0; s < SLOTS; s++)
        if (links[s].step == STEP_UP) return CTL_CONNECTED;
    if (pairing()) return CTL_PAIRING;
    for (s = 0; s < SLOTS; s++)
        if (listed[s] || links[s].handle != HCI_CON_HANDLE_INVALID) return CTL_CONNECTING;
    return CTL_IDLE;
}

int dongle_bt_linked(u8 slot)
{
    return slot < SLOTS && links[slot].step == STEP_UP && links[slot].peer_is_link;
}

int dongle_bt_take_input(u8 slot, struct pad *p)
{
    int got;

    plat_lock();
    got = x[slot].have_pad;
    if (got) *p = x[slot].pad;
    x[slot].have_pad = 0;
    plat_unlock();
    return got;
}

int dongle_bt_take_raw_input(u8 slot, u8 out[LINK_INPUT_SIZE])
{
    int got;

    plat_lock();
    got = x[slot].have_raw;
    if (got) memcpy(out, x[slot].raw, LINK_INPUT_SIZE);
    x[slot].have_raw = x[slot].have_pad = 0;      /* the parsed one goes nowhere then */
    plat_unlock();
    return got;
}

int dongle_bt_take_config(u8 slot, struct link_config *c)
{
    int got;

    plat_lock();
    got = x[slot].have_config;
    if (got) *c = x[slot].config;
    x[slot].have_config = 0;
    plat_unlock();
    return got;
}

/* Changed output goes at once; the timer repeats it every LINK_OUTPUT_MS. */
void dongle_bt_output(u8 slot, const u8 out[LINK_OUTPUT_SIZE])
{
    int changed;

    plat_lock();
    changed = memcmp(x[slot].output, out, LINK_OUTPUT_SIZE) != 0;
    memcpy(x[slot].output, out, LINK_OUTPUT_SIZE);
    if (changed) pending.flags |= SEND_OUTPUT;
    plat_unlock();
    if (changed) plat_bt_wake();
}

void dongle_bt_stats(u8 slot, struct link_stats *out)
{
    plat_lock();
    *out = x[slot].stats;
    plat_unlock();
}

void dongle_bt_start(void)
{
    plat_bt_thread(body);
}
