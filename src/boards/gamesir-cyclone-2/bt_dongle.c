/* The dongle's side of the link (link.h): an LE central for one bonded pad
 * of ours.  With a bond it connects to the pad's address whenever the pad
 * advertises (the initiator waits as long as it takes); pairing, it scans
 * for a pad advertising LINK_ADV_PAIRING and gives it a new key.  ATT goes
 * straight over its L2CAP channel to link.h's fixed handles: no discovery,
 * no GATT client.  The pad's state and config go to the control thread
 * through newest-wins slots (dongle.h). */
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

#define STORE_BOND     1         /* syscfg user item */
#define BOND_LE        2         /* bond.valid: an LE bond (1 was the Classic link's) */
#define PAIR_MS        30000
#define KEY_SETTLE_MS  150       /* the pad's control thread files the key before we encrypt */
#define ATT_MTU        247       /* asked for: an input report in one notification */
#define SCAN_INTERVAL  0x0030    /* 30 ms, scanning all of it */
#define TIMEOUT_10MS   100       /* supervision timeout, 1 s */

static u8 own_addr[6];                    /* controller order, least significant byte first */
static struct bond { u8 peer[6], key[16], type, valid; } bond;   /* peer: BTstack's order */
static u32 pair_until;                    /* pairing: scanning until then */
static volatile u8 state = CTL_BOOT;
static u8 working, connecting;   /* connecting: our gap_connect() waits for the pad */
static hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t hci_reg;
static btstack_timer_source_t timer, out_timer, settle_timer;
static btstack_data_source_t poll_source;
static struct { u8 flags; } pending;
enum { FORGET_BOND = 1, SEND_OUTPUT = 2 };

/* After the connection: what is left to do, in order. */
enum { STEP_NONE, STEP_MTU, STEP_MTU_WAIT, STEP_RAND, STEP_RAND_WAIT, STEP_KEY, STEP_KEY_WAIT,
       STEP_SETTLE, STEP_ENCRYPT, STEP_ENCRYPT_WAIT, STEP_SUBSCRIBE, STEP_UP };
static u8 step;
static u8 pairing_conn;                   /* this connection came from pairing: a new key */
static u8 new_key[16], rand_n;
static u8 phy_pending, length_pending, output_pending;

static int pairing(void)
{
    return pair_until && (s32)(plat_ms() - pair_until) < 0;
}

static void reverse16(const u8 *in, u8 *out)
{
    int i;
    for (i = 0; i < 16; i++) out[i] = in[15 - i];
}

/* ---- the link: input and config in, output back ----------------------------- */

static u8 peer_is_link;          /* our pad sent its config on this connection */
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

static void take_config(const u8 *d, u16 n)
{
    struct link_config c;

    if (!link_unpack_config(d, n, &c)) return;
    if (!peer_is_link) util_log("bt: our pad, type %u", c.type);
    peer_is_link = 1;
    plat_lock();
    x.config = c;
    x.have_config = 1;
    plat_unlock();
    plat_wake(EV_BT);
}

/* ---- ATT, by hand ---------------------------------------------------------------- */

static int att_send(u8 *pdu, u16 len)
{
    if (!l2cap_can_send_fixed_channel_packet_now(handle, L2CAP_CID_ATTRIBUTE_PROTOCOL)) {
        l2cap_request_can_send_fix_channel_now_event(handle, L2CAP_CID_ATTRIBUTE_PROTOCOL);
        return 0;
    }
    return l2cap_send_connectionless(handle, L2CAP_CID_ATTRIBUTE_PROTOCOL, pdu, len) == 0;
}

static int att_write(u8 opcode, u16 h, const u8 *data, u16 len)
{
    u8 pdu[3 + 20];

    pdu[0] = opcode;
    little_endian_store_16(pdu, 1, h);
    memcpy(pdu + 3, data, len);
    return att_send(pdu, (u16)(3 + len));
}

static int send_output(void)
{
    u8 out[LINK_OUTPUT_SIZE];

    plat_lock();
    memcpy(out, x.output, LINK_OUTPUT_SIZE);
    plat_unlock();
    return att_write(ATT_WRITE_COMMAND, LINK_OUTPUT_HANDLE, out, LINK_OUTPUT_SIZE);
}

static void on_att(u8 type, u16 channel, u8 *packet, u16 size)
{
    (void)channel;
    if (type == HCI_EVENT_PACKET) {
        if (hci_event_packet_get_type(packet) == L2CAP_EVENT_CAN_SEND_NOW) plat_bt_wake();
        return;
    }
    if (type != ATT_DATA_PACKET || size < 1) return;
    switch (packet[0]) {
    case ATT_HANDLE_VALUE_NOTIFICATION:
        if (size < 3 || step != STEP_UP) break;      /* nothing before encryption */
        if (little_endian_read_16(packet, 1) == LINK_INPUT_HANDLE) take_input(packet + 3, (u16)(size - 3));
        else if (little_endian_read_16(packet, 1) == LINK_CONFIG_HANDLE) take_config(packet + 3, (u16)(size - 3));
        break;
    case ATT_EXCHANGE_MTU_RESPONSE:
        if (step != STEP_MTU_WAIT) break;
        util_log("bt: att mtu %u", size >= 3 ? little_endian_read_16(packet, 1) : 0);
        step = pairing_conn ? STEP_RAND : STEP_ENCRYPT;
        break;
    case ATT_WRITE_RESPONSE:
        if (step != STEP_KEY_WAIT) break;
        util_log("bt: key written");
        step = STEP_SETTLE;
        btstack_run_loop_set_timer(&settle_timer, KEY_SETTLE_MS);
        btstack_run_loop_add_timer(&settle_timer);
        break;
    case ATT_ERROR_RESPONSE:
        util_log("bt: att error 0x%02x for 0x%02x on 0x%04x", size >= 5 ? packet[4] : 0,
                 size >= 2 ? packet[1] : 0, size >= 4 ? little_endian_read_16(packet, 2) : 0);
        if (step == STEP_MTU_WAIT) step = pairing_conn ? STEP_RAND : STEP_ENCRYPT;
        else if (step == STEP_KEY_WAIT) gap_disconnect(handle);
        break;
    case ATT_EXCHANGE_MTU_REQUEST: {      /* the pad does not ask, but answer if it does */
        u8 r[3] = { ATT_EXCHANGE_MTU_RESPONSE };

        little_endian_store_16(r, 1, ATT_MTU);
        att_send(r, sizeof(r));
        break;
    }
    }
    plat_bt_wake();              /* on_poll: the next step */
}

/* ---- connecting ------------------------------------------------------------------ */

static void connect_bonded(void)
{
    u8 status;

    if (!working || !bond.valid || handle != HCI_CON_HANDLE_INVALID) return;
    gap_set_connection_parameters(SCAN_INTERVAL, SCAN_INTERVAL, LINK_INTERVAL, LINK_INTERVAL, 0,
                                  TIMEOUT_10MS, 0, 0);
    status = gap_connect(bond.peer, (bd_addr_type_t)bond.type);
    if (status && status != ERROR_CODE_COMMAND_DISALLOWED)
        util_log("bt: connect %s: 0x%02x", bd_addr_to_str(bond.peer), status);
    else if (!status) {
        connecting = 1;
        state = CTL_CONNECTING;
        util_log("bt: waiting for %s", bd_addr_to_str(bond.peer));
    }
}

static void start_pairing(void)
{
    state = CTL_PAIRING;
    pair_until = plat_ms() + PAIR_MS;
    util_log("bt: pairing: scanning for a pad for %u s", PAIR_MS / 1000);
    if (handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(handle);          /* scanning starts once it is down */
        return;
    }
    gap_set_scan_params(0, SCAN_INTERVAL, SCAN_INTERVAL, 0);
    if (connecting) gap_connect_cancel();   /* scanning starts as it ends (up()) */
    else gap_start_scan();
}

static void on_advertisement(u8 *packet)
{
    bd_addr_t a;
    const u8 *d = gap_event_advertising_report_get_data(packet);
    u8 n = gap_event_advertising_report_get_data_length(packet), status;

    if (!pairing() || handle != HCI_CON_HANDLE_INVALID) return;
    if (n < LINK_ADV_SIZE || memcmp(d, link_adv, LINK_ADV_FLAGS) || !(d[LINK_ADV_FLAGS] & LINK_ADV_PAIRING)) return;
    gap_event_advertising_report_get_address(packet, a);
    gap_stop_scan();
    gap_set_connection_parameters(SCAN_INTERVAL, SCAN_INTERVAL, LINK_INTERVAL, LINK_INTERVAL, 0,
                                  TIMEOUT_10MS, 0, 0);
    status = gap_connect(a, (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet));
    util_log("bt: pairing with %s: 0x%02x", bd_addr_to_str(a), status);
    pairing_conn = connecting = status == 0;
    if (status) gap_start_scan();
}

static void on_timer(btstack_timer_source_t *t)
{
    if (pair_until && !pairing()) {
        pair_until = 0;
        util_log("bt: pairing ended");
        if (handle == HCI_CON_HANDLE_INVALID) {
            gap_stop_scan();
            state = CTL_IDLE;
            connect_bonded();
        }
    }
    btstack_run_loop_set_timer(t, 1000);
    btstack_run_loop_add_timer(t);
}

static void on_out_timer(btstack_timer_source_t *t)
{
    if (step != STEP_UP) return;
    output_pending = 1;
    btstack_run_loop_set_timer(t, LINK_OUTPUT_MS);
    btstack_run_loop_add_timer(t);
}

static void on_settle(btstack_timer_source_t *t)
{
    (void)t;
    if (step == STEP_SETTLE) step = STEP_ENCRYPT;
    plat_bt_wake();
}

static void linked(void)
{
    step = STEP_UP;
    state = CTL_CONNECTED;
    output_pending = 1;
    util_log("bt: link up");
    btstack_run_loop_set_timer_handler(&out_timer, on_out_timer);
    btstack_run_loop_set_timer(&out_timer, LINK_OUTPUT_MS);
    btstack_run_loop_add_timer(&out_timer);
}

/* The steps after the connection, and output, whenever there is room. */
static void on_poll(btstack_data_source_t *ds, btstack_data_source_callback_type_t type)
{
    u8 flags, pdu[3], key[16];

    (void)ds; (void)type;
    plat_lock();
    flags = pending.flags;
    pending.flags = 0;
    plat_unlock();
    if (flags & FORGET_BOND) {
        bond.valid = 0;
        dongle_store_write(STORE_BOND, &bond, sizeof(bond));
        util_log("bt: bond forgotten%s", set_is_ephemeral() ? " (ephemeral)" : "");
        if (working) start_pairing();
    }
    if (flags & SEND_OUTPUT) output_pending = 1;
    if (handle == HCI_CON_HANDLE_INVALID) return;
    if (phy_pending && hci_can_send_command_packet_now()) {
        phy_pending = 0;
        gap_le_set_phy(handle, 0, 2, 2, 0);          /* 2M both ways */
    }
    if (length_pending && hci_can_send_command_packet_now()) {
        length_pending = 0;
        hci_send_cmd(&hci_le_set_data_length, handle, 251, 2120);
    }
    switch (step) {
    case STEP_MTU:
        pdu[0] = ATT_EXCHANGE_MTU_REQUEST;
        little_endian_store_16(pdu, 1, ATT_MTU);
        if (att_send(pdu, 3)) step = STEP_MTU_WAIT;
        break;
    case STEP_RAND:
        if (!hci_can_send_command_packet_now()) break;
        if (!rand_n) util_log("bt: new key");
        hci_send_cmd(&hci_le_rand);
        step = STEP_RAND_WAIT;
        break;
    case STEP_KEY:
        if (att_write(ATT_WRITE_REQUEST, LINK_KEY_HANDLE, new_key, 16)) step = STEP_KEY_WAIT;
        break;
    case STEP_ENCRYPT:
        if (!hci_can_send_command_packet_now()) break;
        reverse16(pairing_conn ? new_key : bond.key, key);   /* as the pad answers it (bt_pad.c) */
        util_log("bt: encrypting");
        hci_send_cmd(&hci_le_start_encryption, handle, 0, 0, 0, key);
        step = STEP_ENCRYPT_WAIT;
        break;
    case STEP_SUBSCRIBE: {
        u8 on[2] = { 1, 0 };

        if (att_write(ATT_WRITE_COMMAND, LINK_INPUT_HANDLE + 1, on, 2)) linked();
        break;
    }
    case STEP_UP:
        if (output_pending && send_output()) output_pending = 0;
        break;
    }
}

static void up(u8 *packet)
{
    bd_addr_t a;

    connecting = 0;
    hci_subevent_le_connection_complete_get_peer_address(packet, a);
    if (hci_subevent_le_connection_complete_get_status(packet)) {
        util_log("bt: connection to %s: 0x%02x", bd_addr_to_str(a),
                 hci_subevent_le_connection_complete_get_status(packet));
        pairing_conn = 0;
        if (pairing()) gap_start_scan();
        else connect_bonded();
        return;
    }
    handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
    util_log("bt: le %s interval %u latency %u timeout %u%s", bd_addr_to_str(a),
             hci_subevent_le_connection_complete_get_conn_interval(packet),
             hci_subevent_le_connection_complete_get_conn_latency(packet),
             hci_subevent_le_connection_complete_get_supervision_timeout(packet),
             pairing_conn ? " (pairing)" : "");
    if (pairing_conn) {
        memcpy(bond.peer, a, 6);
        bond.type = hci_subevent_le_connection_complete_get_peer_address_type(packet);
    }
    state = CTL_CONNECTING;
    step = STEP_MTU;
    rand_n = 0;
    phy_pending = length_pending = 1;
    output_pending = 0;
    peer_is_link = 0;
    have_seq = 0;
    plat_bt_wake();
}

static void down(u8 reason)
{
    util_log("bt: disconnected, reason 0x%02x", reason);
    handle = HCI_CON_HANDLE_INVALID;
    step = STEP_NONE;
    pairing_conn = 0;
    peer_is_link = 0;
    plat_lock();
    x.have_pad = 0;
    plat_unlock();
    btstack_run_loop_remove_timer(&out_timer);
    btstack_run_loop_remove_timer(&settle_timer);
    state = CTL_IDLE;
    if (pairing()) {
        state = CTL_PAIRING;
        gap_set_scan_params(0, SCAN_INTERVAL, SCAN_INTERVAL, 0);
        gap_start_scan();
    } else {
        connect_bonded();
    }
}

static void encrypted(u8 status, u8 on)
{
    if (step != STEP_ENCRYPT_WAIT) return;
    if (status || !on) {
        util_log("bt: encryption failed 0x%02x: %s", status, pairing_conn ? "pair again" : "not our bond");
        gap_disconnect(handle);
        return;
    }
    if (pairing_conn) {
        memcpy(bond.key, new_key, 16);
        bond.valid = BOND_LE;
        pair_until = 0;
        pairing_conn = 0;
        util_log("bt: bonded with %s %s: %d", bd_addr_to_str(bond.peer),
                 set_is_ephemeral() ? "in RAM" : "saved", dongle_store_write(STORE_BOND, &bond, sizeof(bond)));
    }
    util_log("bt: encrypted");
    step = STEP_SUBSCRIBE;
    plat_bt_wake();
}

static void on_packet(u8 type, u16 channel, u8 *packet, u16 size)
{
    bd_addr_t a;
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
        state = CTL_IDLE;
        util_log("bt: central as %s", bd_addr_to_str(a));
        util_log("bt: bond %s", bond.valid ? bd_addr_to_str(bond.peer) : "none");
        btstack_run_loop_set_timer_handler(&timer, on_timer);
        btstack_run_loop_set_timer(&timer, 1000);
        btstack_run_loop_add_timer(&timer);
        if (pair_until) start_pairing();
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
            util_log("bt: le update status 0x%02x interval %u",
                     hci_subevent_le_connection_update_complete_get_status(packet),
                     hci_subevent_le_connection_update_complete_get_conn_interval(packet));
            break;
        case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
            util_log("bt: le data length tx %u rx %u",
                     hci_subevent_le_data_length_change_get_max_tx_octets(packet),
                     hci_subevent_le_data_length_change_get_max_rx_octets(packet));
            break;
        case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
            util_log("bt: le phy status 0x%02x tx %u rx %u",
                     hci_subevent_le_phy_update_complete_get_status(packet),
                     hci_subevent_le_phy_update_complete_get_tx_phy(packet), packet[7]);
            break;
        }
        break;
    case HCI_EVENT_COMMAND_COMPLETE:
        if (hci_event_command_complete_get_command_opcode(packet) == HCI_OPCODE_HCI_LE_RAND &&
            step == STEP_RAND_WAIT) {
            const u8 *r = hci_event_command_complete_get_return_parameters(packet);
            u32 us = plat_us();

            /* Two LE Rands, the time mixed in. */
            for (i = 0; i < 8; i++) new_key[8 * rand_n + i] = (u8)(r[1 + i] ^ (us >> (8 * (i & 3))));
            step = ++rand_n < 2 ? STEP_RAND : STEP_KEY;
            plat_bt_wake();
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == handle)
            down(hci_event_disconnection_complete_get_reason(packet));
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        encrypted(hci_event_encryption_change_get_status(packet),
                  hci_event_encryption_change_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_COMMAND_STATUS:
        if (hci_event_command_status_get_status(packet))
            util_log("bt: command %04x status 0x%02x", hci_event_command_status_get_command_opcode(packet),
                     hci_event_command_status_get_status(packet));
        if (hci_event_command_status_get_command_opcode(packet) == HCI_OPCODE_HCI_LE_START_ENCRYPTION &&
            hci_event_command_status_get_status(packet))
            encrypted(hci_event_command_status_get_status(packet), 0);
        break;
    default:
        break;
    }
}

static void body(void)
{
    dongle_identity(own_addr);
    if (plat_store_read(STORE_BOND, &bond, sizeof(bond)) != sizeof(bond) || bond.valid != BOND_LE ||
        !dongle_address_valid(bond.peer))
        memset(&bond, 0, sizeof(bond));
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
    btstack_run_loop_set_timer_handler(&settle_timer, on_settle);
    btstack_run_loop_set_data_source_handler(&poll_source, on_poll);
    btstack_run_loop_enable_data_source_callbacks(&poll_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&poll_source);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}

/* USB callers only enqueue requests: bond writes and BTstack calls run on
 * the Bluetooth thread. */
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
