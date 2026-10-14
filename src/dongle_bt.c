/* The dongle's Classic HID host: one bond, our pad's link (link.h) or a
 * stock pad (the stock-format keepalive, for investigation).  With a bond it
 * only listens: our pad pages it, as it does a console, and the dongle takes
 * the master role so its polling sets the link's pace.  The pad's state and
 * config go to the control thread through newest-wins slots (dongle.h). */
#include <string.h>
#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "classic/btstack_link_key_db.h"
#include "classic/sdp_server.h"
#include "bluetooth_psm.h"
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
void hci_jieli_link_policy(void);

/* Stock VM identity, read per device; target can be overridden over USB. */
static u8 own_addr[6];                    /* controller order, least significant byte first */
static bd_addr_t pad_base;               /* display order */
static u8 page_offsets, page_offset;
static u32 pair_until;                   /* pairing: page the rendezvous address until then */

#define PAIR_MS        30000

static int pairing(void)
{
    return pair_until && (s32)(plat_ms() - pair_until) < 0;
}

/* link.h's rendezvous address, display order. */
static void rendezvous(bd_addr_t a)
{
    reverse_bd_addr(link_pair_address, a);
}

#define LISTEN_MS      15000     /* before paging the pad ourselves (no bond only) */
#define STORE_BOND     1         /* syscfg user item: peer, key, type */
#define PAGE_EVERY_MS  2000      /* after the last page ended */
#define MTU            512
#define OUTPUT_MS      100       /* working stock-format keepalive interval */

static btstack_packet_callback_registration_t hci_reg, l2cap_reg;
static btstack_timer_source_t timer, out_timer;
static u16 control_cid, interrupt_cid;
/* One bond, in flash (or RAM in ephemeral mode). Accept an incoming
 * reconnect, or page the saved peer; without a bond probe pairing addresses. */
static struct bond { u8 peer[6], key[16], type, valid; } bond;
static u8 working, acl_up, output_pending;
static u8 paging;
static hci_con_handle_t handle;
static volatile u8 state = CTL_BOOT;
static btstack_data_source_t commands;
static struct { u8 flags, target[6]; } pending;
enum { SET_TARGET = 1, FORGET_BOND = 2, SEND_OUTPUT = 4 };

static void on_packet(u8 type, u16 channel, u8 *packet, u16 size);

/* ---- the bond, for BTstack ------------------------------------------------- */

static void kdb_nop(void) {}
static void kdb_addr(bd_addr_t a) { (void)a; }

static int kdb_get(bd_addr_t a, link_key_t k, link_key_type_t *t)
{
    if (!bond.valid || memcmp(a, bond.peer, 6)) return 0;
    memcpy(k, bond.key, 16);
    *t = (link_key_type_t)bond.type;
    return 1;
}

static void kdb_put(bd_addr_t a, link_key_t k, link_key_type_t t)
{
    memcpy(bond.peer, a, 6);
    memcpy(bond.key, k, 16);
    bond.type = (u8)t;
    bond.valid = 1;
    pair_until = 0;
    util_log("bt: bond %s: %d", set_is_ephemeral() ? "in RAM" : "saved",
             dongle_store_write(STORE_BOND, &bond, sizeof(bond)));
}

static void kdb_delete(bd_addr_t a)
{
    if (!memcmp(a, bond.peer, 6)) {
        bond.valid = 0;
        dongle_store_write(STORE_BOND, &bond, sizeof(bond));
    }
}

static int kdb_iter_init(btstack_link_key_iterator_t *it)
{
    it->context = (void *)0;
    return 1;
}

static int kdb_iter_next(btstack_link_key_iterator_t *it, bd_addr_t a, link_key_t k,
                         link_key_type_t *t)
{
    if (it->context || !bond.valid) return 0;
    it->context = (void *)1;
    memcpy(a, bond.peer, 6);
    memcpy(k, bond.key, 16);
    *t = (link_key_type_t)bond.type;
    return 1;
}

static void kdb_iter_done(btstack_link_key_iterator_t *it) { (void)it; }

static const btstack_link_key_db_t key_db = {
    .open = kdb_nop, .set_local_bd_addr = kdb_addr, .close = kdb_nop,
    .get_link_key = kdb_get, .put_link_key = kdb_put, .delete_link_key = kdb_delete,
    .iterator_init = kdb_iter_init, .iterator_get_next = kdb_iter_next,
    .iterator_done = kdb_iter_done,
};

/* ---- the link: input and config in, output back ----------------------------- */

static u8 peer_is_link;          /* the peer speaks link.h, not the stock format */
/* Bluetooth thread to control thread, under plat_lock. */
static struct {
    struct pad pad;              /* newest; motion samples gathered until taken */
    u8 have_pad;
    struct link_config config;
    u8 have_config;
    u8 output[LINK_OUTPUT_SIZE];
    struct link_stats stats;
} x;
static u8 last_seq, have_seq;

static void send_output(void)
{
    u8 status;

    if (peer_is_link) {
        u8 r[2 + LINK_OUTPUT_SIZE];

        r[0] = 0xa2;
        r[1] = LINK_OUTPUT_ID;
        plat_lock();
        memcpy(r + 2, x.output, LINK_OUTPUT_SIZE);
        plat_unlock();
        status = l2cap_send(interrupt_cid, r, sizeof(r));
    } else {
        u8 r[DONGLE_OUTPUT_SIZE];
        dongle_output(r);
        status = l2cap_send(interrupt_cid, r, sizeof(r));
    }
    if (status) util_log("bt: output send failed: 0x%02x", status);
}

/* A newer state replaces the one waiting, but its motion samples join the
 * waiting ones (the newest IMU_PER_REPORT): a sample is never sent twice and
 * only dropped when the host falls that far behind. */
static void take_input(const u8 *d, u16 n)
{
    struct pad p;
    u8 seq, flags;
    u32 i;

    if (!link_unpack_input(d, n, &p, &seq, &flags)) return;
    plat_lock();
    if (x.have_pad) {
        struct imu_sample all[IMU_PER_REPORT + LINK_IMU_MAX];
        u32 k = 0;

        for (i = 0; i < x.pad.imu_count; i++) all[k++] = x.pad.imu[i];
        for (i = 0; i < p.imu_count; i++) all[k++] = p.imu[i];
        p.imu_count = (u8)(k > IMU_PER_REPORT ? IMU_PER_REPORT : k);
        memcpy(p.imu, all + (k - p.imu_count), p.imu_count * sizeof(all[0]));
        x.stats.replaced++;
    }
    x.pad = p;
    x.have_pad = 1;
    x.stats.inputs++;
    if (have_seq && (u8)(seq - last_seq) > 1) x.stats.lost += (u8)(seq - last_seq) - 1u;
    plat_unlock();
    have_seq = 1;
    last_seq = seq;
    plat_wake(EV_BT);
}

static void dongle_on_input_report(const u8 *packet, u16 size)
{
    struct link_config c;

    if (size < 2 || packet[0] != 0xa1) return;
    if (packet[1] == LINK_INPUT_ID) {
        peer_is_link = 1;
        take_input(packet + 2, (u16)(size - 2));
    } else if (packet[1] == LINK_CONFIG_ID && link_unpack_config(packet + 2, (u16)(size - 2), &c)) {
        bd_addr_t r;

        if (!peer_is_link) util_log("bt: our pad, type %u", c.type);
        rendezvous(r);
        if (bond.valid && !memcmp(bond.peer, r, 6)) {
            /* Paired at the rendezvous: the slot's own address from now on. */
            reverse_bd_addr(c.address, bond.peer);
            util_log("bt: bond is %s's, saved: %d", bd_addr_to_str(bond.peer),
                     dongle_store_write(STORE_BOND, &bond, sizeof(bond)));
        }
        peer_is_link = 1;
        plat_lock();
        x.config = c;
        x.have_config = 1;
        plat_unlock();
        plat_wake(EV_BT);
    }
}

static void request_output(void)
{
    if (!interrupt_cid || output_pending) return;
    output_pending = 1;
    l2cap_request_can_send_now_event(interrupt_cid);
}

static void on_out_timer(btstack_timer_source_t *t)
{
    if (!interrupt_cid) return;
    request_output();
    btstack_run_loop_set_timer(t, OUTPUT_MS);
    btstack_run_loop_add_timer(t);
}

static void hid_up(void)
{
    state = CTL_CONNECTED;
    util_log("bt: hid up");
    request_output();
    btstack_run_loop_set_timer_handler(&out_timer, on_out_timer);
    btstack_run_loop_set_timer(&out_timer, OUTPUT_MS);
    btstack_run_loop_add_timer(&out_timer);
}

/* ---- connection management ---------------------------------------------- */

/* As on the pad: finish authentication/encryption before starting outgoing
 * HID traffic. Incoming channels retain their registered LEVEL_0 policy. */
static u8 open_hid_channel(bd_addr_t address, u16 psm, u16 *cid)
{
    u8 status;

    gap_set_security_level(LEVEL_2);
    status = l2cap_create_channel(on_packet, address, psm, MTU, cid);
    gap_set_security_level(LEVEL_0);
    return status;
}

static void page_pad(void)
{
    bd_addr_t a;
    u16 cid;
    u8 status;

    if (pairing()) rendezvous(a);
    else if (bond.valid) memcpy(a, bond.peer, 6);
    else {
        if (!page_offsets) return;
        memcpy(a, pad_base, 6);
        a[5] = (u8)(a[5] + page_offset);
        page_offset = (u8)((page_offset + 1) % page_offsets);
    }
    status = open_hid_channel(a, BLUETOOTH_PSM_HID_CONTROL, &cid);
    paging = status == 0;
    if (paging) state = CTL_CONNECTING;
    util_log("bt: paging the pad at %s: status 0x%02x", bd_addr_to_str(a), status);
}

/* Paging is for a target given over USB (padtool target) or a stock pad
 * found by its base address; a bonded pad of ours pages us. */
static void on_timer(btstack_timer_source_t *t)
{
    if (pair_until && !pairing()) {
        pair_until = 0;
        util_log("bt: pairing ended");
        if (!acl_up) state = CTL_IDLE;
    }
    if (!acl_up && !paging && (!bond.valid || pairing())) page_pad();
    btstack_run_loop_set_timer(t, PAGE_EVERY_MS);
    btstack_run_loop_add_timer(t);
}

static void on_packet(u8 type, u16 channel, u8 *packet, u16 size)
{
    bd_addr_t a;
    u16 cid;

    if (type == L2CAP_DATA_PACKET) {
        if (channel == interrupt_cid) {
            dongle_on_input_report(packet, size);
        }
        return;
    }
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING || working) break;
        working = 1;
        /* HCI Reset restored the factory address. */
        hci_jieli_set_address(own_addr);
        reverse_bd_addr(own_addr, a);
        hci_set_bd_addr(a);
        hci_jieli_link_policy();
        state = CTL_IDLE;
        gap_connectable_control(1);
        util_log("bt: listening as %s", bd_addr_to_str(a));
        util_log("bt: bond %s", bond.valid ? bd_addr_to_str(bond.peer) : "none");
        btstack_run_loop_set_timer_handler(&timer, on_timer);
        btstack_run_loop_set_timer(&timer, LISTEN_MS);
        btstack_run_loop_add_timer(&timer);
        break;
    case HCI_EVENT_CONNECTION_REQUEST:
        paging = 1;              /* not while the pad comes to us */
        state = CTL_CONNECTING;
        hci_event_connection_request_get_bd_addr(packet, a);
        util_log("bt: connection request from %s class %06x link %u", bd_addr_to_str(a),
                 (unsigned)hci_event_connection_request_get_class_of_device(packet),
                 hci_event_connection_request_get_link_type(packet));
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        hci_event_connection_complete_get_bd_addr(packet, a);
        util_log("bt: acl %s status 0x%02x", bd_addr_to_str(a),
                 hci_event_connection_complete_get_status(packet));
        paging = 0;
        if (hci_event_connection_complete_get_status(packet)) { state = CTL_IDLE; break; }
        acl_up = 1;
        /* This host accepts one pad: stop page scan while its link is up,
         * so scanning cannot compete with the connection for radio time. */
        gap_connectable_control(0);
        handle = hci_event_connection_complete_get_connection_handle(packet);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        util_log("bt: disconnected, reason 0x%02x",
                 hci_event_disconnection_complete_get_reason(packet));
        acl_up = 0;
        peer_is_link = 0;
        have_seq = 0;
        plat_lock();
        x.have_pad = 0;
        plat_unlock();
        state = CTL_IDLE;
        gap_connectable_control(1);
        paging = 0;
        control_cid = interrupt_cid = 0;
        output_pending = 0;
        btstack_run_loop_remove_timer(&out_timer);
        break;
    case HCI_EVENT_COMMAND_STATUS:
        if (packet[2]) util_log("bt: command %04x status 0x%02x", little_endian_read_16(packet, 4), packet[2]);
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        util_log("bt: authentication complete 0x%02x", packet[2]);
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        util_log("bt: encryption 0x%02x on %u role %s", packet[2], packet[5],
                 gap_get_role(handle) == HCI_ROLE_SLAVE ? "slave" : "master");
        break;
    case HCI_EVENT_MODE_CHANGE:
        util_log("bt: mode %u interval %u slots", packet[5], little_endian_read_16(packet, 6));
        break;
    case HCI_EVENT_ROLE_CHANGE:
        util_log("bt: role %s status 0x%02x", packet[9] ? "slave" : "master", packet[2]);
        break;
    case L2CAP_EVENT_CAN_SEND_NOW:
        if (channel == interrupt_cid && output_pending) {
            output_pending = 0;
            send_output();
        }
        break;
    case L2CAP_EVENT_INCOMING_CONNECTION:
        cid = l2cap_event_incoming_connection_get_local_cid(packet);
        util_log("bt: incoming l2cap psm %x", l2cap_event_incoming_connection_get_psm(packet));
        l2cap_accept_connection(cid);
        break;
    case L2CAP_EVENT_CHANNEL_OPENED:
        cid = l2cap_event_channel_opened_get_local_cid(packet);
        util_log("bt: l2cap psm %x %s status 0x%02x mtu %u", l2cap_event_channel_opened_get_psm(packet),
                 l2cap_event_channel_opened_get_incoming(packet) ? "in" : "out",
                 l2cap_event_channel_opened_get_status(packet),
                 l2cap_event_channel_opened_get_remote_mtu(packet));
        if (l2cap_event_channel_opened_get_status(packet)) break;
        if (l2cap_event_channel_opened_get_psm(packet) == BLUETOOTH_PSM_HID_CONTROL) control_cid = cid;
        else if (l2cap_event_channel_opened_get_psm(packet) == BLUETOOTH_PSM_HID_INTERRUPT) interrupt_cid = cid;
        if (control_cid && interrupt_cid && state != CTL_CONNECTED) hid_up();
        /* Our page opened control: interrupt follows, as a HID host does. */
        if (!l2cap_event_channel_opened_get_incoming(packet) &&
            l2cap_event_channel_opened_get_psm(packet) == BLUETOOTH_PSM_HID_CONTROL) {
            l2cap_event_channel_opened_get_address(packet, a);
            open_hid_channel(a, BLUETOOTH_PSM_HID_INTERRUPT, &cid);
        }
        break;
    case L2CAP_EVENT_CHANNEL_CLOSED:
        cid = l2cap_event_channel_closed_get_local_cid(packet);
        if (cid == control_cid || cid == interrupt_cid) {
            if (cid == control_cid) control_cid = 0;
            if (cid == interrupt_cid) interrupt_cid = 0;
            output_pending = 0;
            btstack_run_loop_remove_timer(&out_timer);
            state = CTL_CONNECTING;
            if (acl_up) gap_disconnect(handle);
        }
        util_log("bt: l2cap closed");
        break;
    default:
        break;
    }
}

/* USB callers only enqueue requests. Bond persistence and BTstack calls
 * run on the Bluetooth thread, including when USB arrives during pairing. */
static void on_commands(btstack_data_source_t *ds, btstack_data_source_callback_type_t type)
{
    u8 flags, target[6];

    (void)ds; (void)type;
    plat_lock();
    flags = pending.flags;
    memcpy(target, pending.target, sizeof(target));
    pending.flags = 0;
    plat_unlock();
    if (flags & SET_TARGET) {
        memcpy(pad_base, target, sizeof(pad_base));
        page_offsets = 1;
        page_offset = 0;
        util_log("bt: target %s", bd_addr_to_str(pad_base));
    }
    if ((flags & SEND_OUTPUT) && peer_is_link) request_output();
    if (flags & FORGET_BOND) {
        bond.valid = 0;
        dongle_store_write(STORE_BOND, &bond, sizeof(bond));
        util_log("bt: bond forgotten%s", set_is_ephemeral() ? " (ephemeral)" : "");
        if (acl_up) gap_disconnect(handle);
        state = CTL_PAIRING;
        pair_until = plat_ms() + PAIR_MS;
        util_log("bt: pairing: paging the rendezvous address for %u s", PAIR_MS / 1000);
        if (!acl_up && !paging) page_pad();
    }
}

static void body(void)
{
    dongle_identity(own_addr, pad_base);
    page_offsets = dongle_address_valid(pad_base) ? 6 : 0;
    if (plat_store_read(STORE_BOND, &bond, sizeof(bond)) != sizeof(bond) ||
        !dongle_address_valid(bond.peer) || bond.valid != 1) memset(&bond, 0, sizeof(bond));
    hci_jieli_prepare(0, 0, 0);
    hci_jieli_tx_power(4, 1);
    hci_jieli_set_address(own_addr);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    hci_set_link_key_db(&key_db);
    l2cap_init();
    sdp_init();
    gap_set_local_name("Cyclone 2 dongle");
    gap_set_class_of_device(0x000104);   /* computer, desktop: a host */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    hci_set_master_slave_policy(0);     /* the pad pages us: we take master */
    gap_set_page_timeout(0x2000);       /* 5.1 s a page, not 24 */
    gap_set_security_level(LEVEL_0);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    l2cap_register_service(on_packet, BLUETOOTH_PSM_HID_CONTROL, MTU, LEVEL_0);
    l2cap_register_service(on_packet, BLUETOOTH_PSM_HID_INTERRUPT, MTU, LEVEL_0);
    hci_reg.callback = on_packet;
    hci_add_event_handler(&hci_reg);
    l2cap_reg.callback = on_packet;
    l2cap_add_event_handler(&l2cap_reg);
    btstack_run_loop_set_data_source_handler(&commands, on_commands);
    btstack_run_loop_enable_data_source_callbacks(&commands, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&commands);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}

void dongle_bt_target(const u8 addr[6])
{
    if (!dongle_address_valid(addr)) return;
    plat_lock();
    memcpy(pending.target, addr, sizeof(pending.target));
    pending.flags |= SET_TARGET;
    plat_unlock();
    plat_bt_wake();
}

void dongle_bt_forget(void)
{
    plat_lock();
    pending.flags |= FORGET_BOND;
    plat_unlock();
    plat_bt_wake();
}

u8 dongle_bt_state(void)
{
    return state;
}

int dongle_bt_linked(void)
{
    return state == CTL_CONNECTED && peer_is_link;
}

int dongle_bt_take_input(struct pad *p)
{
    int got;

    plat_lock();
    got = x.have_pad;
    if (got) *p = x.pad;
    x.have_pad = 0;
    plat_unlock();
    return got;
}

int dongle_bt_take_config(struct link_config *c)
{
    int got;

    plat_lock();
    got = x.have_config;
    if (got) *c = x.config;
    x.have_config = 0;
    plat_unlock();
    return got;
}

/* Changed output goes at once; the timer repeats it every LINK_OUTPUT_MS. */
void dongle_bt_output(const u8 out[LINK_OUTPUT_SIZE])
{
    int changed;

    plat_lock();
    changed = memcmp(x.output, out, LINK_OUTPUT_SIZE) != 0;
    memcpy(x.output, out, LINK_OUTPUT_SIZE);
    if (changed) pending.flags |= SEND_OUTPUT;
    plat_unlock();
    if (changed) plat_bt_wake();
}

void dongle_bt_stats(struct link_stats *out)
{
    plat_lock();
    *out = x.stats;
    plat_unlock();
}

void dongle_bt_start(void)
{
    plat_bt_thread(body);
}
