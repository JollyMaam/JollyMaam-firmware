/* Radio-to-LMP events and interrupt dispatch, recovered from SDK lmp.c. */
#include <stdint.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_TWS && !BTCFG_BREDR_INQUIRY &&
               !BTCFG_MASTER_ESCO && !BTCFG_ESCO_FORWARD, "standard HCI, no TWS or voice links");
#ifndef UNITTEST
_Static_assert(sizeof(void *) == sizeof(int), "SDK event pointer occupies one message word");
#endif

/* The SDK IRQ ABI supplies no packet length. */
int lmp_cmd_handler_in_irq(void *lc, u8 *data, u8 llid)
{
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    if (!conn || llid != 3) return 0;
    switch (data[0] & ~1) {
    case PDU(SET_AFH): {
        u32 instant = (u32)data[1] | ((u32)data[2] << 8) |
                      ((u32)data[3] << 16) | ((u32)data[4] << 24);
        bredr_link_set_afh(lc, instant, data[5], data + 6);
        return 1;
    }
    case PDU(UNSNIFF_REQ):
        conn->tran_id = data[0] & 1;
        lmp_rx_unsniff_req(conn);
        return 1;
    case PDU(ACCEPTED):
        if (data[1] != UNSNIFF_REQ) break;
        conn->tran_id = data[0] & 1;
        lmp_rx_accepted_unsniff_req(conn);
        return 1;
    }
    return 0;
}

void lmp_event_handler(int *msg)
{
    void *link = (void *)(uintptr_t)(u32)msg[0];
    int event = msg[1];
    if (event == LMP_EVENT_CONNECTION_START) { lmp_connection_start(link, (u8)msg[2]); return; }
    if (event == LMP_EVENT_INQUIRY_RESULT) return;
    struct lmp_connection *conn = lmp_conn_for_link(link);
    if (!conn) return;
    switch (event) {
    case LMP_EVENT_DETACH:
        conn->err_code = (u8)msg[2];
        conn->core.state |= 8;
        lmp_request(conn, "11", PDU(DETACH), (u8)msg[2]);
        break;
    case LMP_EVENT_SUPER_TIMEOUT:
        conn->err_code = 8;
        conn->core.state |= 8;
        break;
    case LMP_EVENT_ROLE_SWITCH_MACHINE: lmp_role_switch_machine(conn, 0); break;
    case LMP_EVENT_SET_MAX_SLOT: lmp_send_max_slot(conn, (u8)msg[2]); break;
    case LMP_EVENT_SLOT_CTRL: lmp_connection_ctl_slot(conn, (u8)msg[2]); break;
    case LMP_EVENT_ESCO_LINK_SETUP:
    case LMP_EVENT_AFTER_TWS_ESCO_LINK_SETUP: break; /* Voice/TWS disabled. */
    case LMP_EVENT_ACL_TX_POST: hci_send_event(0x13, "1H2", 1, conn->core_s.handle, 1); break;
    case LMP_EVENT_RX: lmp_rx_handler(conn); break;
    case LMP_EVENT_TX_POWER: lmp_tx_power_req(conn, (u8)msg[2]); break;
    case LMP_EVENT_TX_SET_AFH: lmp_tx_set_afh(conn, 0); break;
    }
    lmp_detach_check();
}

void bredr_close_all_scan(void)
{
    link_inquiry_scan_disable();
    link_page_disable(0xfe);
    link_page_scan_disable();
}

bool check_lmp_detch_over(void)
{ return !lmp_acl_link[0].core_s.handle || !lmp_acl_link[0].lmp_detch_start; }

void lmp_disconnect_all_standard_connect(void) { lmp_tx_detch(lmp_acl_link, 0x13); }

u8 lmp_standard_connect_check(void)
{ return lmp_acl_link[0].link ? 0x80 : get_bredr_link_state() & 1; }

void lmp_timeout_irq_handler(void) {} /* SDK stubs */
void lmp_authentication_requested(struct lmp_connection *conn) { (void)conn; }

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
static struct link_connection link;
static int found = 1, lookups, v[4], radio_state;
static u32 instant_seen;
static u8 map_seen[10];
static char trace[128];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
struct lmp_connection *lmp_conn_for_link(void *lc)
{ CHECK(lc == &link); lookups++; return found ? lmp_acl_link : 0; }
void bredr_link_set_afh(struct link_connection *lc, u32 instant, u8 enable, u8 *map)
{ CHECK(lc == &link); instant_seen = instant; v[0] = enable; memcpy(map_seen, map, 10); note("afh_irq"); }
void lmp_rx_unsniff_req(struct lmp_connection *c) { CHECK(c == lmp_acl_link); note("unsniff"); }
void lmp_rx_accepted_unsniff_req(struct lmp_connection *c) { CHECK(c == lmp_acl_link); note("accepted"); }
int lmp_connection_start(struct link_connection *lc, u8 role)
{ CHECK(lc == &link); v[0] = role; note("start"); return 0; }
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == lmp_acl_link && !strcmp(format, "11"));
    va_list ap; va_start(ap, format); v[0] = va_arg(ap, int); v[1] = va_arg(ap, int); va_end(ap);
    note("request"); return 0;
}
void lmp_role_switch_machine(struct lmp_connection *c, int result)
{ CHECK(c == lmp_acl_link && result == 0); note("role"); }
void lmp_send_max_slot(struct lmp_connection *c, u8 slot)
{ CHECK(c == lmp_acl_link); v[0] = slot; note("slot"); }
void lmp_connection_ctl_slot(struct lmp_connection *c, u8 slot)
{ CHECK(c == lmp_acl_link); v[0] = slot; note("ctl"); }
int hci_send_event(int event, const char *format, ...)
{
    CHECK(event == 0x13 && !strcmp(format, "1H2"));
    va_list ap; va_start(ap, format); for (int i = 0; i < 3; i++) v[i] = va_arg(ap, int); va_end(ap);
    note("complete"); return 0;
}
void lmp_rx_handler(struct lmp_connection *c) { CHECK(c == lmp_acl_link); note("rx"); }
void lmp_tx_power_req(struct lmp_connection *c, u8 mode)
{ CHECK(c == lmp_acl_link); v[0] = mode; note("power"); }
void lmp_tx_set_afh(struct lmp_connection *c, u8 mode)
{ CHECK(c == lmp_acl_link && mode == 0); note("afh"); }
void lmp_detach_check(void) { note("detach"); }
void link_inquiry_scan_disable(void) { note("inquiry"); }
void link_page_disable(int err) { CHECK(err == 0xfe); note("page"); }
void link_page_scan_disable(void) { note("scan"); }
void lmp_tx_detch(struct lmp_connection *c, u8 error)
{ CHECK(c == lmp_acl_link && error == 0x13); note("detch"); }
u8 get_bredr_link_state(void) { return radio_state; }
static void reset(void) { trace[0] = 0; lookups = 0; }
int main(void)
{
    u8 pdu[] = {PDU(SET_AFH) | 1, 0x78, 0x56, 0x34, 0x12, 1, 1,2,3,4,5,6,7,8,9,10};
    reset(); CHECK(lmp_cmd_handler_in_irq(&link, pdu, 3) == 1);
    CHECK(instant_seen == 0x12345678 && v[0] == 1 && !memcmp(map_seen, pdu + 6, 10));
    reset(); CHECK(!lmp_cmd_handler_in_irq(&link, pdu, 2) && !trace[0]);
    found = 0; CHECK(!lmp_cmd_handler_in_irq(&link, pdu, 3)); found = 1;
    for (int tid = 0; tid < 2; tid++) {
        reset(); pdu[0] = PDU(UNSNIFF_REQ) | tid;
        CHECK(lmp_cmd_handler_in_irq(&link, pdu, 3) == 1 && lmp_acl_link[0].tran_id == tid);
        CHECK(!strcmp(trace, "unsniff "));
        reset(); pdu[0] = PDU(ACCEPTED) | tid; pdu[1] = UNSNIFF_REQ;
        CHECK(lmp_cmd_handler_in_irq(&link, pdu, 3) == 1 && lmp_acl_link[0].tran_id == tid);
        CHECK(!strcmp(trace, "accepted "));
    }
    reset(); pdu[1] = SNIFF_REQ; CHECK(!lmp_cmd_handler_in_irq(&link, pdu, 3) && !trace[0]);
    pdu[0] = PDU(FEATURES_REQ); CHECK(!lmp_cmd_handler_in_irq(&link, pdu, 3));
    CHECK((uintptr_t)&link <= UINT32_MAX); if ((uintptr_t)&link > UINT32_MAX) return 1;
    int msg[] = {(int)(uintptr_t)&link, 0, 0x12345};
    const char *expected[] = {"detach ","start ","","detach ","role detach ","rx detach ",
        "slot detach ","ctl detach ","detach ","detach ","detach ","complete detach ","power detach ",
        "detach ","request detach ","afh detach "};
    lmp_acl_link[0].core_s.handle = 0x234;
    for (int event = 0; event <= 15; event++) {
        reset(); lmp_acl_link[0].core.state = 0x100; msg[1] = event; lmp_event_handler(msg);
        CHECK(!strcmp(trace, expected[event]));
        CHECK(lookups == (event == 1 || event == 2 ? 0 : 1));
        if (event == 1 || event == 6 || event == 7 || event == 12) CHECK(v[0] == 0x45);
        if (event == 3 || event == 14) CHECK(lmp_acl_link[0].core.state == 0x108 &&
                                           lmp_acl_link[0].err_code == (event == 3 ? 8 : 0x45));
        if (event == 11) CHECK(v[0] == 1 && v[1] == 0x234 && v[2] == 1);
        if (event == 14) CHECK(v[0] == PDU(DETACH) && v[1] == 0x45);
    }
    reset(); found = 0; msg[1] = 5; lmp_event_handler(msg); CHECK(!trace[0]); found = 1;
    reset(); bredr_close_all_scan(); CHECK(!strcmp(trace, "inquiry page scan "));
    for (int mask = 0; mask < 4; mask++) {
        lmp_acl_link[0].core_s.handle = mask & 1 ? 0x30 : 0;
        lmp_acl_link[0].lmp_detch_start = !!(mask & 2);
        CHECK(check_lmp_detch_over() == (mask != 3));
    }
    reset(); lmp_disconnect_all_standard_connect(); CHECK(!strcmp(trace, "detch "));
    lmp_acl_link[0].link = 0;
    for (radio_state = 0; radio_state < 256; radio_state++) CHECK(lmp_standard_connect_check() == (radio_state & 1));
    lmp_acl_link[0].link = &link; CHECK(lmp_standard_connect_check() == 0x80);
    reset(); lmp_timeout_irq_handler(); lmp_authentication_requested(lmp_acl_link); CHECK(!trace[0]);
    return TEST_DONE("btctrler_lmp_link_events");
}
#endif
