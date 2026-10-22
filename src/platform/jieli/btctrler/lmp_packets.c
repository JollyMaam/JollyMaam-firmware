/* Classic packet-slot and packet-type-table policy, from SDK lmp.c.
 * The link scheduler and packet formatter remain SDK implementations. */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

void lmp_tx_max_slot(struct lmp_connection *conn)
{
    u8 features = conn->core_s.remote_features[0];
    u8 slots = features & 2 ? 5 : features & 1 ? 3 : 1;
    conn->link->core.max_slot = slots;
    lmp_request(conn, "11", PDU(MAX_SLOT), slots);
}

void lmp_rx_max_slot(struct lmp_connection *conn, u8 *rx)
{
    struct link_connection *link = conn->link;
    u8 current = link->core.max_slot;
    if (current == rx[0]) return;
    if (current < rx[0] && lmp_connection_esco_open())
        lmp_request(conn, "11", PDU(MAX_SLOT), current);
    else link->core.max_slot = rx[0];
}

void lmp_rx_max_slot_req(struct lmp_connection *conn, u8 *rx)
{
    if (conn->link->core.max_slot < rx[0] && lmp_connection_esco_open()) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), MAX_SLOT_REQ, 0x1e);
    } else {
        conn->link->core.max_slot = rx[0];
        lmp_response(conn, "11", PDU(ACCEPTED), MAX_SLOT_REQ);
    }
}

void lmp_rx_accepted_max_slot_req(struct lmp_connection *conn)
{ hci_send_event(0x1b, "21", conn->core_s.handle, conn->link->core.max_slot); }

void lmp_rx_not_accepted_max_slot_req(struct lmp_connection *conn)
{ (void)conn; }

void lmp_tx_packet_type_table_req(struct lmp_connection *conn)
{
    if ((config_btctler_mode & 1) && !g_dut_hdl)
        lmp_request(conn, "111", PDU(ESCAPE_4), PACKET_TYPE_TABLE_REQ, 1);
}

void lmp_rx_packet_type_table_req(struct lmp_connection *conn, u8 *rx)
{
    if ((config_btctler_mode & 1) && !g_dut_hdl && rx[0] == 0)
        lmp_response(conn, "b05", PDU(ESCAPE_4), NOT_ACCEPTED_EXT, ESCAPE_4, PACKET_TYPE_TABLE_REQ, 0x1e);
    else lmp_response(conn, "1111", PDU(ESCAPE_4), ACCEPTED_EXT, ESCAPE_4, PACKET_TYPE_TABLE_REQ);
}

void lmp_connection_ctl_slot(struct lmp_connection *conn, u8 slot)
{
    for (struct lmp_connection *p = lmp_acl_link; p < lmp_acl_link + 1; p++) {
        if (p == conn || !p->core_s.handle) continue;
        p->link->core.max_slot = slot;
        lmp_request(p, "11", PDU(MAX_SLOT), slot);
    }
}

void lmp_send_max_slot(struct lmp_connection *conn, u8 slot)
{ lmp_request(conn, "M11", slot, PDU(MAX_SLOT), slot); }

void lmp_tx_max_slot_req(struct lmp_connection *conn, u8 max_slot)
{ lmp_request(conn, "11", PDU(MAX_SLOT_REQ), max_slot); }

void lmp_tx_auto_rate(struct lmp_connection *conn)
{ lmp_request(conn, "1", PDU(AUTO_RATE)); }

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
struct bredr_dut *g_dut_hdl;
int config_btctler_mode;
static struct link_connection link;
static int esco, requests, responses, events, v[5];
static const char *last_format;
bool lmp_connection_esco_open(void) { return esco; }
static void capture(struct lmp_connection *c, const char *format, va_list ap)
{
    CHECK(c == lmp_acl_link); last_format = format;
    int n = !strcmp(format, "b05") ? 5 : !strcmp(format, "1111") ? 4 :
            !strcmp(format, "111") || !strcmp(format, "M11") ? 3 :
            !strcmp(format, "11") ? 2 : 1;
    for (int i = 0; i < n; i++) v[i] = va_arg(ap, int);
}
int lmp_request(struct lmp_connection *c, const char *format, ...)
{ va_list ap; va_start(ap, format); capture(c, format, ap); va_end(ap); requests++; return 0; }
int lmp_response(struct lmp_connection *c, const char *format, ...)
{ va_list ap; va_start(ap, format); capture(c, format, ap); va_end(ap); responses++; return 0; }
int hci_send_event(int id, const char *format, ...)
{
    CHECK(id == 0x1b && !strcmp(format, "21")); events++;
    va_list ap; va_start(ap, format); v[0] = va_arg(ap, int); v[1] = va_arg(ap, int); va_end(ap); return 0;
}
static void reset(void) { requests = responses = events = 0; }
int main(void)
{
    struct lmp_connection *c = lmp_acl_link;
    c->link = &link; c->core_s.handle = 0x234;
    for (int features = 0; features < 256; features++) {
        c->core_s.remote_features[0] = features; reset(); lmp_tx_max_slot(c);
        int slots = features & 2 ? 5 : features & 1 ? 3 : 1;
        CHECK(requests == 1 && link.core.max_slot == slots && v[0] == PDU(MAX_SLOT) && v[1] == slots);
    }
    const u8 slots[] = {1, 3, 5};
    for (esco = 0; esco <= 1; esco++) {
        for (unsigned a = 0; a < 3; a++) for (unsigned b = 0; b < 3; b++) {
            u8 desired = slots[b]; link.core.max_slot = slots[a]; reset();
            lmp_rx_max_slot(c, &desired);
            int refused = esco && slots[a] < desired;
            CHECK(link.core.max_slot == (refused ? slots[a] : desired));
            CHECK(requests == refused);
            if (refused) CHECK(v[0] == PDU(MAX_SLOT) && v[1] == slots[a]);
            link.core.max_slot = slots[a]; reset(); lmp_rx_max_slot_req(c, &desired);
            CHECK(responses == 1 && v[0] == PDU(refused ? NOT_ACCEPTED : ACCEPTED) && v[1] == MAX_SLOT_REQ);
            CHECK(link.core.max_slot == (refused ? slots[a] : desired));
            if (refused) CHECK(v[2] == 0x1e);
        }
    }
    link.core.max_slot = 5; reset(); lmp_rx_accepted_max_slot_req(c);
    CHECK(events == 1 && v[0] == 0x234 && v[1] == 5);
    reset(); lmp_rx_not_accepted_max_slot_req(c); CHECK(!events && !requests && !responses);
    struct bredr_dut dut;
    for (int mode = 0; mode < 4; mode++) for (int active = 0; active < 2; active++) {
        config_btctler_mode = mode; g_dut_hdl = active ? &dut : 0;
        reset(); lmp_tx_packet_type_table_req(c);
        CHECK(requests == ((mode & 1) && !active ? 1 : 0));
        if (requests) CHECK(v[0] == 254 && v[1] == PACKET_TYPE_TABLE_REQ && v[2] == 1);
        for (int table = 0; table < 256; table++) {
            u8 p = table; reset(); lmp_rx_packet_type_table_req(c, &p);
            int reject = (mode & 1) && !active && table == 0;
            CHECK(responses == 1 && v[1] == (reject ? NOT_ACCEPTED_EXT : ACCEPTED_EXT));
            CHECK(v[2] == ESCAPE_4 && v[3] == PACKET_TYPE_TABLE_REQ);
            if (reject) CHECK(v[4] == 0x1e);
        }
    }
    reset(); lmp_connection_ctl_slot(c, 1); CHECK(!requests);
    c->core_s.handle = 0; lmp_connection_ctl_slot(0, 1); CHECK(!requests);
    c->core_s.handle = 0x234; lmp_connection_ctl_slot(0, 3);
    CHECK(requests == 1 && link.core.max_slot == 3 && v[0] == PDU(MAX_SLOT) && v[1] == 3);
    reset(); lmp_send_max_slot(c, 5);
    CHECK(!strcmp(last_format, "M11") && v[0] == 5 && v[1] == PDU(MAX_SLOT) && v[2] == 5);
    reset(); lmp_tx_max_slot_req(c, 3); CHECK(v[0] == PDU(MAX_SLOT_REQ) && v[1] == 3);
    reset(); lmp_tx_auto_rate(c); CHECK(v[0] == PDU(AUTO_RATE));
    return TEST_DONE("btctrler_lmp_packets");
}
#endif
