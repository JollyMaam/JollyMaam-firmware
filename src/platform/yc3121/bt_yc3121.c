#include <string.h>
#include "pad.h"
#include "board.h"
#include "radio_yc3121.h"
#include "sdp_yc3121.h"

/* Bluetooth on the YC3121: Classic HID through the radio core, which runs
 * Yichip's HID device (L2CAP, SDP, pairing and HIDP are its own).  The
 * commands and events are in the analysis notes; each was proven against
 * BlueZ:
 *   start-up as stock v1.01's, then 0xa0 our SDP records and 0x03 the name;
 *   0x26 the bond table (5 records of 34 bytes), built here from the slot's
 *   bond, so the core knows only the slot's host;
 *   0x02 visibility (bit 0 discoverable, bit 1 connectable);
 *   0xb1 page the host {address, u16 page timeout, link key};
 *   0x11 disconnect; 0xb0 an input report {0xa1, id, data};
 *   events: 0xb9 baseband up, 0xb0 HID up, 0xb6 down, 0xb5 a pairing's
 *   {our address, the host's, its link key}, 0xb2 a host's HIDP frame,
 *   0x06 {command, status} an acknowledgement (an input's too).
 * A session restarts the core with the profile's identity (its address,
 * name, class, SDP): after a disconnect, as a restart drops a link
 * silently.  One thread: the platform's wait loop takes the core's events
 * (plat_bt_poll), bt_poll() hands them on. */

#define IN_FLIGHT    2           /* input reports the core has not acknowledged */
#define PAGE_TIMEOUT 0x12c0      /* 0.625 ms units: 3 s, as stock's */
#define PAGE_WAIT_MS 4500        /* a page with no link this long: BT_EV_FAILED */
#define STOP_WAIT_MS 1000        /* our disconnect's 0xb6 at most this long */
#define WRITES       4           /* host writes waiting for bt_poll() */
#define SDP_MAX      768
#define RECORD       34          /* a bond table record */

static struct radio_packet pkt;

static struct {
    u8 core_up;                  /* the core configured for the session (or idle) */
    u8 session, pairing;         /* bt_start() ran, no bt_stop() since */
    u8 linked;                   /* HID up */
    u8 in_flight;
    u8 paging;
    u32 page_ms;
    u8 stopping;                 /* our disconnect, waiting for 0xb6 */
    u32 stop_ms;
    u8 ev_linked, ev_unlinked, ev_failed, ev_paired, ev_stopped, lost;
    struct bt_bond bond;         /* the slot's, or the pairing's */
    const struct profile *prof;
    u8 address[6];
    u32 wanted_us;               /* when an input was last wanted */
    struct bt_write writes[WRITES];
    u8 w_head, w_tail;
    struct bt_stats stats;
} b;

static int bonded(void)
{
    return !util_is_zero(b.bond.peer, 6);
}

/* ---- the core's events ------------------------------------------------------ */

static void host_frame(const u8 *f, u8 n)
{
    struct bt_write *w;
    u8 type = f[0] >> 4, param = f[0] & 3;

    if (n < 2) return;
    if (type == 0x4) {                   /* GET_REPORT: not answered yet */
        util_log("bt: GET_REPORT %02x %02x not answered", f[0], f[1]);
        return;
    }
    if (type != 0xa && type != 0x5) {   /* DATA and SET_REPORT only */
        util_log("bt: HIDP %02x ignored", f[0]);
        return;
    }
    if ((u8)(b.w_head - b.w_tail) >= WRITES) {
        util_log("bt: host write dropped");
        return;
    }
    w = &b.writes[b.w_head++ % WRITES];
    w->kind = param == 3 ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT;
    w->id = f[1];
    w->len = (u16)(n - 2 < BT_WRITE_MAX ? n - 2 : BT_WRITE_MAX);
    memcpy(w->data, f + 2, w->len);
}

static void stopped(void)
{
    b.stopping = 0;
    b.ev_stopped = 1;
}

static void handle(const struct radio_packet *p)
{
    switch (p->id) {
    case 0x06:                           /* an acknowledgement */
        if (p->len >= 2 && p->data[0] == 0xb0) {
            if (b.in_flight) b.in_flight--;
            b.stats.acked++;
            if (p->data[1]) b.stats.late++;   /* refused: counted as late */
        }
        break;
    case 0xb9:                           /* baseband up */
        break;
    case 0xb0:                           /* HID up */
        b.paging = 0;
        b.in_flight = 0;
        if (!b.session || b.stopping) break;
        b.linked = 1;
        b.ev_linked = 1;
        util_log("bt: linked");
        break;
    case 0xb6:                           /* down (it comes once a channel) */
        if (b.stopping) {
            plat_radio_send(0x02, (const u8[]){ 0 }, 1, 0);
            stopped();
        } else if (b.linked) {
            b.linked = 0;
            b.ev_unlinked = 1;
            b.lost = 1;                  /* lost or ended: the core does not say */
        } else if (b.paging) {
            b.paging = 0;
            b.ev_failed = 1;
        }
        b.in_flight = 0;
        break;
    case 0xb5:                           /* paired: our address, the host's, the key */
        if (p->len < 28 || !b.session) break;
        memset(&b.bond, 0, sizeof(b.bond));
        memcpy(b.bond.peer, p->data + 6, 6);
        memcpy(b.bond.link_key, p->data + 12, 16);
        b.ev_paired = 1;
        util_log("bt: paired");
        break;
    case 0xb2:
        host_frame(p->data, p->len);
        break;
    case 0x0d:                           /* the core's bond table: ours is the slot's */
    case 0x14:
    case 0xb7:
    case 0x09:
        break;
    default:
        util_log("bt: event %02x [%u]", p->id, p->len);
    }
}

/* ---- commands ---------------------------------------------------------------- */

/* A command, then its acknowledgement (event 0x06 {id, status}; 0x29 for
 * 0x77 and 0x78), other events handled meanwhile; set-up only. */
static int command(u8 id, const void *data, u16 len, int wide)
{
    u8 ack = id == 0x77 || id == 0x78 ? 0x29 : 0x06;
    u32 t0;

    if (!plat_radio_send(id, data, len, wide)) {
        util_log("bt: %02x: no room", id);
        return 0;
    }
    for (t0 = plat_ms(); plat_ms() - t0 < 500;) {
        if (!plat_radio_take(&pkt)) continue;
        if (pkt.id == ack && (ack == 0x29 || pkt.data[0] == id)) return ack == 0x29 || !pkt.data[1];
        handle(&pkt);
    }
    util_log("bt: %02x: no answer", id);
    return 0;
}

/* The core started again and set up for the profile (none: idle, no SDP),
 * invisible until visibility() says otherwise.  The ADC is the core's too. */
static int core_start(const struct profile *p)
{
    static const u8 cmd04[14] = { '8', '0', 'K', 'A' };   /* as stock; meaning not established */
    static const u8 on = 1, x42 = 2, x39[2] = { 200, 0 }, x79[2] = { 0, 4 };
    static const u8 service[17] = { 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x10, 0xff, 0, 0 };
    static const u8 characteristic[20] = { 0x18, 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x13, 0xff, 0, 0, 1, 0 };
    static u8 records[SDP_MAX], table[5 * RECORD];
    const struct bt_device *d = p ? p->device : NULL;
    u32 cod = d ? d->class_of_device : 0x002508;
    u8 cod3[3] = { (u8)cod, (u8)(cod >> 8), (u8)(cod >> 16) };
    const char *name = d && d->name ? d->name : board_name;
    u16 n = 0;
    int ok, i;

    if (d) {
        struct yc_sdp_hid hid = {
            .name = name, .descriptor = d->hid_descriptor, .descriptor_len = d->hid_descriptor_len,
            .subclass = (u8)(cod & 0xfc), .vendor = d->vendor_id, .product = d->product_id,
            .version = d->version,
        };
        n = yc_sdp_records(records, sizeof(records), &hid);
        if (!n) util_log("bt: %s: SDP records do not fit", name);
    }
    /* The bond table: the slot's host first (most recent), the rest empty
     * with their order bytes, as the core keeps them; byte 1 as in every
     * bond the core made (meaning not established). */
    memset(table, 0, sizeof(table));
    for (i = 0; i < 5; i++) table[i * RECORD] = (u8)i;
    if (bonded()) {
        table[1] = 0x33;
        memcpy(table + 2, b.bond.peer, 6);
        memcpy(table + 18, b.bond.link_key, 16);
    }
    b.core_up = 0;
    ok = plat_radio_start() == 0;
    if (ok) plat_adc_init();
    while (ok && plat_radio_take(&pkt)) handle(&pkt);
    ok = ok && command(0x04, cmd04, sizeof(cmd04), 0);
    ok = ok && command(0x01, b.address, 6, 0);
    ok = ok && command(0x00, b.address, 6, 0);
    ok = ok && command(0x0c, &on, 1, 0);
    ok = ok && command(0x15, cod3, 3, 0);
    ok = ok && command(0x26, table, sizeof(table), 0);
    ok = ok && command(0x76, NULL, 0, 0);
    ok = ok && command(0x77, service, sizeof(service), 0);
    ok = ok && command(0x78, characteristic, sizeof(characteristic), 0);
    ok = ok && command(0x42, &x42, 1, 0);
    ok = ok && command(0x39, x39, 2, 0);
    ok = ok && command(0x79, x79, 2, 0);
    ok = ok && (!n || command(0xa0, records, n, 1));
    ok = ok && command(0x03, name, (u16)strlen(name), 0);
    b.core_up = (u8)ok;
    if (!ok) util_log("bt: the core did not start");
    return ok;
}

static void visibility(u8 flags)
{
    command(0x02, &flags, 1, 0);
}

static void page(void)
{
    u8 c[24];

    memcpy(c, b.bond.peer, 6);
    c[6] = (u8)PAGE_TIMEOUT;
    c[7] = PAGE_TIMEOUT >> 8;
    memcpy(c + 8, b.bond.link_key, 16);
    if (!plat_radio_send(0xb1, c, sizeof(c), 0)) return;
    b.paging = 1;
    b.page_ms = plat_ms();
    util_log("bt: paging the host");
}

/* ---- bt_*() ----------------------------------------------------------------- */

void plat_bt_init(void)
{
    memcpy(b.address, plat_unique_id(), 6);
    core_start(NULL);
    visibility(0);
}

void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *bond,
              int pairing, const struct bt_link *link)
{
    (void)link;
    memset(&b.bond, 0, sizeof(b.bond));
    if (bond) b.bond = *bond;
    memcpy(b.address, address, 6);
    b.prof = p;
    b.session = 1;
    b.pairing = (u8)pairing;
    b.linked = b.in_flight = b.paging = b.stopping = 0;
    b.ev_linked = b.ev_unlinked = b.ev_failed = b.ev_paired = b.ev_stopped = 0;
    b.w_head = b.w_tail = 0;
    if (p->device->transport != BT_CLASSIC) {
        util_log("bt: %s is not Classic: not on this pad", p->name);
        return;
    }
    if (!core_start(p)) return;
    visibility(pairing ? 3 : 2);
    util_log("bt: %s %s", p->name, pairing ? "pairing" : bonded() ? "bonded" : "no bond");
    if (!pairing && bonded()) page();
}

void bt_connect(void)
{
    if (b.session && b.core_up && !b.linked && !b.paging && !b.stopping && bonded()) page();
}

void bt_drop(void)
{
    if (b.linked) plat_radio_send(0x11, NULL, 0, 0);
}

void bt_stop(void)
{
    if (!b.session) {
        b.ev_stopped = 1;
        return;
    }
    b.session = 0;
    b.paging = 0;
    if (b.linked) {
        b.linked = 0;
        b.stopping = 1;
        b.stop_ms = plat_ms();
        plat_radio_send(0x11, NULL, 0, 0);
        return;
    }
    plat_radio_send(0x02, (const u8[]){ 0 }, 1, 0);
    stopped();
}

void bt_abort(void)
{
    bt_stop();
}

void bt_update_bond(const struct bt_bond *bond)
{
    if (b.session) b.bond = *bond;
}

int bt_radio_quiet(void)
{
    return !b.session && !b.stopping;
}

int bt_input_wanted(void)
{
    int wanted = b.session && b.linked && b.in_flight < IN_FLIGHT;

    if (!wanted) b.wanted_us = 0;
    else if (!b.wanted_us) b.wanted_us = plat_us() | 1;
    return wanted;
}

int bt_input_pending(void)
{
    return b.in_flight != 0;
}

u32 bt_input_wanted_us(void)
{
    return bt_input_wanted() ? b.wanted_us : 0;
}

u32 bt_input_due_us(void)
{
    return bt_input_wanted() ? plat_us() : 0;
}

/* An HIDP DATA frame on the interrupt channel: {0xa1, id, data}. */
static int send_frame(u8 id, const u8 *data, u16 len)
{
    static u8 f[2 + PROFILE_REPORT_MAX];

    if (!b.linked || len > PROFILE_REPORT_MAX) return 0;
    f[0] = 0xa1;
    f[1] = id;
    memcpy(f + 2, data, len);
    if (!plat_radio_send(0xb0, f, (u16)(len + 2), 1)) return 0;
    b.in_flight++;
    b.stats.sent++;
    return 1;
}

int bt_send_input(u8 id, const u8 *data, u16 len)
{
    if (!bt_input_wanted()) return 0;
    b.wanted_us = 0;
    return send_frame(id, data, len);
}

int bt_send_message(u16 id, const u8 *data, u16 len)
{
    return send_frame((u8)id, data, len);
}

int bt_poll(struct bt_ev *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (b.ev_linked) {
        b.ev_linked = 0;
        ev->kind = BT_EV_LINKED;
    } else if (b.ev_paired) {
        b.ev_paired = 0;
        ev->kind = BT_EV_PAIRED;
        ev->bond = b.bond;
    } else if (b.w_tail != b.w_head) {
        ev->kind = BT_EV_WRITE;
        ev->write = b.writes[b.w_tail++ % WRITES];
    } else if (b.ev_unlinked) {
        b.ev_unlinked = 0;
        ev->kind = BT_EV_UNLINKED;
        ev->lost = b.lost;
    } else if (b.ev_failed) {
        b.ev_failed = 0;
        ev->kind = BT_EV_FAILED;
    } else if (b.ev_stopped) {
        b.ev_stopped = 0;
        ev->kind = BT_EV_STOPPED;
    } else {
        return 0;
    }
    return 1;
}

/* From the wait loop: the core's events, the timeouts. */
u32 plat_bt_poll(void)
{
    u32 now;

    while (plat_radio_take(&pkt)) handle(&pkt);
    now = plat_ms();
    if (b.paging && now - b.page_ms > PAGE_WAIT_MS) {
        b.paging = 0;
        b.ev_failed = 1;
    }
    if (b.stopping && now - b.stop_ms > STOP_WAIT_MS) {
        plat_radio_send(0x02, (const u8[]){ 0 }, 1, 0);
        stopped();
    }
    return b.ev_linked | b.ev_paired | b.ev_unlinked | b.ev_failed | b.ev_stopped |
           (b.w_tail != b.w_head) || bt_input_wanted() ? EV_BT : 0;
}

void bt_stats(struct bt_stats *out)
{
    *out = b.stats;
}

void bt_timing(struct bt_timing *out, int reset)
{
    (void)reset;
    memset(out, 0, sizeof(*out));
    out->link.transport = b.linked ? 1 : 0;
}

/* Waking a Switch: the core has its own command for it (stock's 0x70,
 * "WakeUp_Switch"), not used yet; nothing to wake here. */
static int wake_start(const struct profile_session *s)
{
    (void)s;
    return -1;
}

static int wake_step(u32 now, u32 *next)
{
    (void)now;
    (void)next;
    return 1;
}

static void wake_stop(void) {}

const struct profile_wake profile_switch_wake = { wake_start, wake_step, wake_stop };
