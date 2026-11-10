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
 * (plat_bt_poll), bt_poll() hands them on.
 *
 * LE (a profile with an att_db): the core serves GAP and GATT itself and
 * takes the app's services: 0x76, then 0x77 {0x10, uuid128} a service and
 * 0x78 {props, 0x10, uuid128, 01 00} a characteristic, answered 0x29 {its
 * value handle}, a CCC after it.  The profile's BTstack database is read
 * for its 128-bit services and characteristics only (no values, no reads:
 * write and notify are all this side does); its handles map to the core's.
 * Advertising (visibility bit 2): the data, if the profile has its own, written
 * into the core's RAM (the window): length at 0xc4341, 31 bytes after it; the
 * scan response likewise at 0xc4361 (else the name of command 0x04, 14 bytes:
 * board_short_name, then the profile's, "U2C BT Config").  A readable
 * characteristic's value is fixed when it is created (0x78's tail {len,
 * value}): the profile's read() then.  Events: 0x02 connected, 0x05
 * disconnected, 0x08 {handle, data} a write; 0x09 {handle, data} notifies,
 * acknowledged 0x06 {09 00}. */

#define IN_FLIGHT    2           /* input reports the core has not acknowledged */
#define PAGE_TIMEOUT 0x12c0      /* 0.625 ms units: 3 s, as stock's */
#define PAGE_WAIT_MS 4500        /* a page with no link this long: BT_EV_FAILED */
#define STOP_WAIT_MS 1000        /* our disconnect's 0xb6 at most this long */
#define WRITES       4           /* host writes waiting for bt_poll() */
#define SDP_MAX      768
#define RECORD       34          /* a bond table record */
#define LE_NAME      14          /* command 0x04's name */
/* The core's attribute table (0xc4970..) ends before its data at 0xc4b6f:
 * room for 11 of Switch 2's characteristics (up to 0x001e, the last it
 * uses); its 13th crossed into that data and the core stopped answering. */
#define LE_CHARS     11
#define LE_NOTIFY    197         /* a notification's data at most (MTU 512 by 0x39, kept below 200) */
#define LE_READ_MAX  16          /* a readable characteristic's value at most */
#define CORE_ADV     ((volatile u8 *)0xc4341)   /* the core's advertising data: length, 31 bytes */
#define CORE_SCAN    ((volatile u8 *)0xc4361)   /* its scan response */
/* The core's attribute table: entries {u16 handle, 02, u16 type, ...} from
 * 0xc4975 (its own GAP and GATT at 1..0x0b first, then ours); a service
 * {.., len, uuid}, a characteristic {.., 01, props, u16 value handle, len,
 * uuid, len, value}, a CCC {.., 02, value 2}; with 11 of Switch 2's
 * characteristics it ends at 0xc4b76. */
#define CORE_TABLE   0xc4975u
#define CORE_TABLE_END 0xc4c80u      /* the command ring */
#define CORE_GAP_LAST 0x0b
#define LE_MAP       (2 * LE_CHARS + 4)

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
    u8 le;                       /* the session is LE */
    u8 chars;
    struct { u16 ours, core; } map[LE_CHARS];   /* value handles: the profile's, the core's */
    struct { u16 core, ours; } renumber[LE_MAP];   /* services, declarations, CCCs */
    u8 renumbers;
} b;

static u16 handle_29;             /* the last 0x29's: a characteristic's value handle */

static int bonded(void)
{
    return !util_is_zero(b.bond.peer, 6);
}

/* ---- the core's events ------------------------------------------------------ */

static void queue_write(u8 kind, u16 id, const u8 *data, u32 n)
{
    struct bt_write *w;

    if ((u8)(b.w_head - b.w_tail) >= WRITES) {
        util_log("bt: host write dropped");
        return;
    }
    w = &b.writes[b.w_head++ % WRITES];
    w->kind = kind;
    w->id = id;
    w->len = (u16)(n < BT_WRITE_MAX ? n : BT_WRITE_MAX);
    memcpy(w->data, data, w->len);
}

/* An LE write: on a value handle of the profile's, a write to it; on the
 * CCC after one (subscribing), nothing more. */
static void le_write(const u8 *d, u8 n)
{
    u16 h;
    u32 i;

    if (n < 2) return;
    h = (u16)(d[0] | d[1] << 8);
    for (i = 0; i < b.chars; i++)
        if (b.map[i].core == h) {
            queue_write(BT_WRITE_ATT, b.map[i].ours, d + 2, n - 2u);
            return;
        }
}

static u16 core_handle(u16 ours)
{
    u32 i;

    for (i = 0; i < b.chars; i++)
        if (b.map[i].ours == ours) return b.map[i].core;
    return 0;
}

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
        if (p->len >= 2 && (p->data[0] == 0xb0 || p->data[0] == 0x09)) {
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
    case 0x02:                           /* LE connected */
        b.in_flight = 0;
        if (!b.session || !b.le || b.stopping) break;
        b.linked = 1;
        b.ev_linked = 1;
        util_log("bt: LE linked");
        break;
    case 0x05:                           /* LE disconnected */
        b.in_flight = 0;
        if (b.stopping) {
            plat_radio_send(0x02, (const u8[]){ 0 }, 1, 0);
            stopped();
        }
        else if (b.linked && b.le) {
            b.linked = 0;
            b.ev_unlinked = 1;
            b.lost = 0;
        }
        break;
    case 0x08:                           /* LE write */
#ifndef UPDATER                          /* there: hundreds a second, an update's */
        if (p->len >= 2) util_log("bt: write %04x [%u] %02x %02x %02x %02x", p->data[0] | p->data[1] << 8, p->len - 2,
                                  p->data[2], p->data[3], p->data[4], p->data[5]);
#endif
        if (b.le && b.linked) le_write(p->data, p->len);
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
    for (t0 = plat_ms(); plat_ms() - t0 < (ack == 0x29 ? 2000u : 500u);) {
        if (!plat_radio_take(&pkt)) continue;
        if (pkt.id == ack && ack == 0x29) {
            handle_29 = (u16)(pkt.data[0] | pkt.data[1] << 8);
            return 1;
        }
        if (pkt.id == ack && pkt.data[0] == id) return !pkt.data[1];
        handle(&pkt);
    }
    util_log("bt: %02x: no answer", id);
    return 0;
}

/* The profile's database (BTstack's: {u16 size, u16 flags, u16 handle, u16
 * or uuid128 type, value} each, then 0) to the core: its 128-bit primary
 * services and characteristics, in order; the value handles mapped. */
static int gatt(const u8 *db)
{
    const u8 *e = db + 1;
    u8 c[20 + LE_READ_MAX];

    b.chars = b.renumbers = 0;
    for (;;) {
        u16 size = (u16)(e[0] | e[1] << 8), flags = (u16)(e[2] | e[3] << 8);
        u16 type = (u16)(e[6] | e[7] << 8);

        if (!size) return 1;
        if (!(flags & 0x0200) && type == 0x2800 && size == 8 + 16) {
            c[0] = 0x10;
            memcpy(c + 1, e + 8, 16);
            if (!command(0x77, c, 17, 0)) return 0;
            if (b.renumbers < LE_MAP) {
                b.renumber[b.renumbers].core = handle_29;
                b.renumber[b.renumbers++].ours = (u16)(e[4] | e[5] << 8);
            }
        } else if (!(flags & 0x0200) && type == 0x2803 && size == 8 + 3 + 16) {
            if (b.chars >= LE_CHARS) {
                util_log("bt: characteristic %04x dropped: the core's table is full", (unsigned)(e[9] | e[10] << 8));
                e += size;
                continue;
            }
            c[0] = e[8];                 /* properties */
            c[1] = 0x10;
            memcpy(c + 2, e + 11, 16);
            c[18] = 1, c[19] = 0;        /* as stock's: one byte, 0 */
            if (e[8] & 0x02 && b.prof && b.prof->read) {   /* readable: the profile's value */
                int n = b.prof->read(BT_WRITE_ATT, (u16)(e[9] | e[10] << 8), 0, c + 19, LE_READ_MAX);

                if (n > 0) c[18] = (u8)n;
                else c[19] = 0;
            }
            {
                u32 t0 = plat_us();
                int ok = command(0x78, c, (u16)(19 + c[18]), 0);

                util_log("bt: char %04x props %02x len %u -> %04x %d in %u us", (unsigned)(e[9] | e[10] << 8),
                         e[8], c[18], handle_29, ok, (unsigned)(plat_us() - t0));
                if (!ok) return 0;
            }
            b.map[b.chars].ours = (u16)(e[9] | e[10] << 8);
            b.map[b.chars].core = handle_29;
            b.chars++;
            if (b.renumbers + 2 <= LE_MAP) {     /* the declaration; the CCC after a notifying one */
                b.renumber[b.renumbers].core = (u16)(handle_29 - 1);
                b.renumber[b.renumbers++].ours = (u16)(e[4] | e[5] << 8);
                if (e[8] & 0x30) {
                    b.renumber[b.renumbers].core = (u16)(handle_29 + 1);
                    b.renumber[b.renumbers++].ours = (u16)((e[9] | e[10] << 8) + 1);
                }
            }
        }
        e += size;
    }
}

static u16 renumbered(u16 h)
{
    u32 i;

    if (h <= CORE_GAP_LAST) return (u16)(h + b.prof->device->att_gap);   /* the core's GAP and GATT */
    for (i = 0; i < b.chars; i++)
        if (b.map[i].core == h) return b.map[i].ours;
    for (i = 0; i < b.renumbers; i++)
        if (b.renumber[i].core == h) return b.renumber[i].ours;
    return h;
}

/* A host that uses fixed handles (Switch 2): the core's table renumbered
 * in place to the profile's handles, its own GAP and GATT moved after them;
 * the value handles are then the profile's own.  Proven: not yet. */
static int renumber(void)
{
    volatile u8 *t = (volatile u8 *)CORE_TABLE, *end = (volatile u8 *)CORE_TABLE_END;
    u32 n = 0, i;

    if ((t[0] | t[1] << 8) != 1 || (t[3] | t[4] << 8) != 0x2800) {
        util_log("bt: the core's table is not where it was");
        return 0;
    }
    while (t + 6 < end) {
        u16 h = (u16)(t[0] | t[1] << 8), type = (u16)(t[3] | t[4] << 8), nh;

        if (!h || t[2] != 0x02) break;
        nh = renumbered(h);
        util_log("bt: attr %04x -> %04x type %04x at %05x", h, nh, type, (unsigned)(u32)t);
        t[0] = (u8)nh, t[1] = (u8)(nh >> 8);
        if (type == 0x2803) {
            u16 v = (u16)(t[7] | t[8] << 8), nv = renumbered(v);

            t[7] = (u8)nv, t[8] = (u8)(nv >> 8);
            t += 9;
            t += 1 + t[0];               /* uuid */
            t += 1 + t[0];               /* value */
        } else {
            t += 5;
            t += 1 + t[0];               /* a service's uuid, a CCC's value */
        }
        n++;
    }
    for (i = 0; i < b.chars; i++) b.map[i].core = b.map[i].ours;
    util_log("bt: %u attributes renumbered, stopped at %05x: %02x %02x %02x %02x %02x %02x", (unsigned)n, (unsigned)(u32)t,
             t[0], t[1], t[2], t[3], t[4], t[5]);
    return 1;
}

/* The core started again and set up for the profile (none: idle, no SDP),
 * invisible until visibility() says otherwise.  The ADC is the core's too. */
static int core_start(const struct profile *p)
{
    static const u8 on = 1, x42 = 2, x39[2] = { 0x00, 0x02 }   /* MTU 512: the console stops at less (Telink: 185) */, x79[2] = { 0, 4 };
    static const u8 service[17] = { 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x10, 0xff, 0, 0 };
    static const u8 characteristic[20] = { 0x18, 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x13, 0xff, 0, 0, 1, 0 };
    static u8 records[SDP_MAX], table[5 * RECORD], le_name[LE_NAME];
    const struct bt_device *d = p ? p->device : NULL;
    int le = d && d->transport == BT_LE;
    u32 cod = d ? d->class_of_device : 0x002508;
    u8 cod3[3] = { (u8)cod, (u8)(cod >> 8), (u8)(cod >> 16) };
    const char *name = d && d->name ? d->name : board_name;
    u16 n = 0;
    int ok, i;

    memset(le_name, 0, sizeof(le_name));
    strncpy((char *)le_name, board_short_name, sizeof(le_name));
    if (le && d->name) {                 /* "<board> <profile>" */
        size_t k = strlen((char *)le_name);

        if (k + 1 < sizeof(le_name)) {
            le_name[k] = ' ';
            strncpy((char *)le_name + k + 1, d->name, sizeof(le_name) - k - 1);
        }
    }
    if (d && !le) {
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
    ok = ok && command(0x04, le_name, sizeof(le_name), 0);
    ok = ok && command(0x01, b.address, 6, 0);
    ok = ok && command(0x00, b.address, 6, 0);
    ok = ok && command(0x0c, &on, 1, 0);
    ok = ok && command(0x15, cod3, 3, 0);
    ok = ok && command(0x26, table, sizeof(table), 0);
    ok = ok && command(0x76, NULL, 0, 0);
    if (le) ok = ok && gatt(d->att_db) && (!d->att_gap || renumber());
    else {                               /* stock's own service, as stock's start-up has it */
        ok = ok && command(0x77, service, sizeof(service), 0);
        ok = ok && command(0x78, characteristic, sizeof(characteristic), 0);
    }
    ok = ok && command(0x42, &x42, 1, 0);
    ok = ok && command(0x39, x39, 2, 0);
    ok = ok && command(0x79, x79, 2, 0);
    ok = ok && (!n || command(0xa0, records, n, 1));
    ok = ok && command(0x03, name, (u16)strlen(name), 0);
    b.core_up = (u8)ok;
    if (!ok) util_log("bt: the core did not start");
    return ok;
}

static void core_data(volatile u8 *to, const u8 *data, u8 n)
{
    u32 i;

    if (n > 31) n = 31;
    to[0] = n;
    for (i = 0; i < 31; i++) to[1 + i] = i < n ? data[i] : 0;
}

/* The profile's advertising data (and scan response) into the core's RAM. */
static void advertising(int wake)
{
    const struct bt_device *d = b.prof ? b.prof->device : NULL;
    u8 adv[31];

    if (!d || !d->advertising) return;
    core_data(CORE_ADV, adv, d->advertising(&b.bond, b.pairing, wake, adv));
    if (d->scan_response) core_data(CORE_SCAN, d->scan_response, (u8)d->scan_response_len);
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
    b.le = p->device->transport == BT_LE;
    if (b.le ? !p->device->att_db : p->device->transport != BT_CLASSIC) {
        util_log("bt: %s: not on this pad", p->name);
        return;
    }
    if (!core_start(p)) return;
    if (b.le) {                          /* advertising to anyone: no SMP here */
        advertising(0);
        visibility(4);
        util_log("bt: %s advertising, %u characteristics", p->name, b.chars);
        return;
    }
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
    if (b.linked) {                      /* LE too: if 0x11 does nothing there, the timeout */
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
    if (!b.session) return;
    b.bond = *bond;
    if (b.le) advertising(0);
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

/* An HIDP DATA frame on the interrupt channel: {0xa1, id, data}; on LE a
 * notification of the profile's handle id (MTU 23: 20 bytes at most). */
static int send_frame(u16 id, const u8 *data, u16 len)
{
    static u8 f[2 + PROFILE_REPORT_MAX];
    u16 h;

    if (!b.linked || len > PROFILE_REPORT_MAX) return 0;
    if (b.le) {
        h = core_handle(id);
        if (!h || len > LE_NOTIFY) return 0;
        f[0] = (u8)h;
        f[1] = (u8)(h >> 8);
        memcpy(f + 2, data, len);
        if (!plat_radio_send(0x09, f, (u16)(len + 2), 0)) return 0;
        b.in_flight++;
        b.stats.sent++;
        return 1;
    }
    f[0] = 0xa1;
    f[1] = (u8)id;
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
    return send_frame(b.le ? b.prof->device->input_handle : id, data, len);
}

int bt_send_message(u16 id, const u8 *data, u16 len)
{
    return send_frame(id, data, len);
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
