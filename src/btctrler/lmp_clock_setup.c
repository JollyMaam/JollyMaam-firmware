/* Classic clock information and connection completion, from SDK lmp.c. */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(!BTCFG_FAST_CONNECT, "connection requests omit SDK fast-connect gates");

void lmp_rx_timing_accuracy_req(struct lmp_connection *conn)
{ lmp_response(conn, "111", PDU(TIMING_ACCURACY_RES), gbredr_local_dev.drift, gbredr_local_dev.jitter); }
void lmp_rx_timing_accuracy_res(struct lmp_connection *conn, u8 *rx)
{ (void)conn; (void)rx; } /* SDK stub. */
void lmp_tx_timing_accuracy_req(struct lmp_connection *conn)
{
    if (conn->core.state & 0x100) return;
    conn->core.state |= 0x100;
    lmp_request(conn, "111", PDU(TIMING_ACCURACY_REQ), gbredr_local_dev.drift, gbredr_local_dev.jitter);
}
void lmp_tx_clkoffset_req(struct lmp_connection *conn)
{ lmp_request(conn, "1", PDU(CLKOFFSET_REQ)); }
void lmp_rx_clkoffset_req(struct lmp_connection *conn)
{ lmp_response(conn, "12", PDU(CLKOFFSET_RES), ((u32)bredr_link_clk_offset(conn->link) >> 1) & 0x7fff); }
void lmp_rx_clkoffset_res(struct lmp_connection *conn, u8 *rx)
{ hci_send_event(0x1c, "122", 0, conn->core_s.handle, rx[0] | (rx[1] << 8)); }
void lmp_rx_supervision_timeout(struct lmp_connection *conn, u8 *rx)
{ hci_send_event(0x38, "22", conn->core_s.handle, rx[0] | (rx[1] << 8)); }
void lmp_rx_quality_of_service_req(struct lmp_connection *conn, u8 *rx)
{ (void)rx; lmp_response(conn, "11", PDU(ACCEPTED), QUALITY_OF_SERVICE); }
void lmp_rx_detch(struct lmp_connection *conn, u8 *rx)
{
    if (rx[0] == 0x08) rx[0] = 0x13;
    else if (rx[0] == 0x22) rx[0] = 0x08;
    conn->err_code = rx[0];
    conn->core.state |= 8;
    os_time_dly(20);
}
void lmp_rx_host_connection_req(struct lmp_connection *conn)
{
    conn->core.state |= 1;
    if (!(conn->core.state & 0x400000)) hci_send_event(0xf9, "A1", conn->core_s.remote_addr, 0);
    if (conn->core.role != 1) btctrler_assert_failed("host connection request role", conn->core.role);
}
void lmp_rx_setup_complete(struct lmp_connection *conn)
{
    lmp_setup_complete(conn);
    lmp_send_event_connection_complete(conn, 0, conn->core_s.handle, 1);
    if (conn->core.role == 1 || (conn->core.state & 0x2000)) {
        conn->core.state = (conn->core.state & ~0x43u) | 0x42;
        hci_send_event(0x20, "A1", conn->core_s.remote_addr, conn->core_s.remote_sr);
    }
}
void lmp_rx_accepted_host_connection_req(struct lmp_connection *conn)
{
    if (conn->core.state & 8) {
        conn->err_code = 0x16;
        lmp_request(conn, "11", PDU(DETACH), 0x16);
        return;
    }
    if (!(conn->core.state & 0x400000)) hci_send_event(0xf9, "A1", conn->core_s.remote_addr, 0);
    lmp_tx_setup_complete(conn);
    conn->core.state = (conn->core.state & ~3u) | 2;
}
void lmp_rx_not_accepted_host_connection_req(struct lmp_connection *conn, u8 *rx)
{ lmp_tx_detch(conn, rx[1]); }

#ifdef UNITTEST
#include <stdarg.h>
#include <string.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev;
static struct lmp_connection conn;
static struct link_connection link;
static int requests, responses, opcode, values[3], clock_offset, event, asserts, detaches, delay;
static const char *format;
static char trace[64];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
static void capture(const char *fmt, va_list ap)
{
    format = fmt; opcode = va_arg(ap, int);
    values[0] = values[1] = 0;
    if (strcmp(fmt, "1")) values[0] = va_arg(ap, int);
    if (!strcmp(fmt, "111")) values[1] = va_arg(ap, int);
}
int lmp_request(struct lmp_connection *c, const char *fmt, ...)
{
    CHECK(c == &conn); va_list ap; va_start(ap, fmt); capture(fmt, ap); va_end(ap);
    requests++; note("request"); return 0;
}
int lmp_response(struct lmp_connection *c, const char *fmt, ...)
{
    CHECK(c == &conn); va_list ap; va_start(ap, fmt); capture(fmt, ap); va_end(ap);
    responses++; return 0;
}
int bredr_link_clk_offset(struct link_connection *lc) { CHECK(lc == &link); return clock_offset; }
int hci_send_event(int code, const char *fmt, ...)
{
    event = code; format = fmt; va_list ap; va_start(ap, fmt);
    if (!strcmp(fmt, "A1")) {
        CHECK(va_arg(ap, u8 *) == conn.core_s.remote_addr); values[0] = va_arg(ap, int);
        note(code == 0xf9 ? "request_event" : "repeat_event");
    } else {
        if (code == 0x1c) CHECK(va_arg(ap, int) == 0);
        CHECK(va_arg(ap, int) == 0x123); values[0] = va_arg(ap, int);
    }
    va_end(ap); return 0;
}
void os_time_dly(int ticks) { CHECK(ticks == 20 && (conn.core.state & 8)); delay++; }
void lmp_tx_detch(struct lmp_connection *c, u8 reason) { CHECK(c == &conn); values[0] = reason; detaches++; }
void btctrler_assert_failed(const char *what, int value) { CHECK(what && value == 0); asserts++; note("assert"); }
void lmp_setup_complete(struct lmp_connection *c)
{ CHECK(c == &conn); note("setup"); c->core.state |= 0x100000; }
void lmp_send_event_connection_complete(struct lmp_connection *c, u8 err, u16 handle, u8 type)
{ CHECK(c == &conn && !err && handle == 0x123 && type == 1); note("complete"); }
void lmp_tx_setup_complete(struct lmp_connection *c)
{ CHECK(c == &conn); c->core.state |= 0x800; note("tx_setup"); }
static void reset(void) { trace[0] = 0; requests = responses = event = asserts = 0; }
int main(void)
{
    conn.link = &link; conn.core_s.handle = 0x123; conn.core_s.remote_sr = 0x81;
    gbredr_local_dev.drift = 0xab; gbredr_local_dev.jitter = 0xcd;
    lmp_rx_timing_accuracy_req(&conn);
    CHECK(responses == 1 && !strcmp(format, "111") && opcode == PDU(TIMING_ACCURACY_RES) && values[0] == 0xab && values[1] == 0xcd);
    lmp_rx_timing_accuracy_res(0, 0); CHECK(responses == 1);
    conn.core.state = 0x123000; lmp_tx_timing_accuracy_req(&conn);
    CHECK(requests == 1 && opcode == PDU(TIMING_ACCURACY_REQ) && conn.core.state == 0x123100 && values[0] == 0xab && values[1] == 0xcd);
    lmp_tx_timing_accuracy_req(&conn); CHECK(requests == 1);
    lmp_tx_clkoffset_req(&conn); CHECK(requests == 2 && opcode == PDU(CLKOFFSET_REQ) && !strcmp(format, "1"));
    for (clock_offset = 0; clock_offset < 65536; clock_offset++) {
        lmp_rx_clkoffset_req(&conn);
        CHECK(opcode == PDU(CLKOFFSET_RES) && !strcmp(format, "12") && values[0] == (clock_offset >> 1));
        u8 rx[2] = {clock_offset, clock_offset >> 8};
        lmp_rx_clkoffset_res(&conn, rx); CHECK(event == 0x1c && !strcmp(format, "122") && values[0] == clock_offset);
        lmp_rx_supervision_timeout(&conn, rx); CHECK(event == 0x38 && !strcmp(format, "22") && values[0] == clock_offset);
    }
    clock_offset = -1; lmp_rx_clkoffset_req(&conn); CHECK(values[0] == 0x7fff);
    lmp_rx_quality_of_service_req(&conn, 0);
    CHECK(opcode == PDU(ACCEPTED) && !strcmp(format, "11") && values[0] == QUALITY_OF_SERVICE);
    for (int reason = 0; reason < 256; reason++) {
        u8 rx[2] = {reason, reason}; conn.core.state = 0x100;
        lmp_rx_detch(&conn, rx); int mapped = reason == 8 ? 0x13 : reason == 0x22 ? 8 : reason;
        CHECK(rx[0] == mapped && conn.err_code == mapped && conn.core.state == 0x108);
        lmp_rx_not_accepted_host_connection_req(&conn, rx); CHECK(values[0] == reason);
    }
    CHECK(detaches == 256 && delay == 256);
    for (int mask = 0; mask < 4; mask++) {
        reset(); u32 state = 0x800 | (mask & 1 ? 0x400000 : 0);
        conn.core.state = state; conn.core.role = mask >> 1;
        lmp_rx_host_connection_req(&conn);
        CHECK(conn.core.state == (state | 1) && event == (mask & 1 ? 0 : 0xf9));
        CHECK(asserts == !(mask >> 1));
    }
    for (int state = 0; state < 256; state++) for (int mask = 0; mask < 4; mask++) {
        reset(); conn.core.role = mask & 1; conn.core.state = state | (mask & 2 ? 0x2000 : 0);
        u32 after = conn.core.state | 0x100000;
        lmp_rx_setup_complete(&conn);
        CHECK(!strcmp(trace, mask ? "setup complete repeat_event " : "setup complete "));
        CHECK(conn.core.state == (mask ? (after & ~0x43u) | 0x42 : after));
        CHECK(event == (mask ? 0x20 : 0)); if (mask) CHECK(values[0] == 0x81);
    }
    for (int state = 0; state < 256; state++) for (int silent = 0; silent < 2; silent++) {
        reset(); u32 before = state | (silent ? 0x400000 : 0); conn.core.state = before;
        lmp_rx_accepted_host_connection_req(&conn);
        if (state & 8) CHECK(!strcmp(trace, "request ") && requests == 1 && opcode == PDU(DETACH) && values[0] == 0x16 && conn.err_code == 0x16 && conn.core.state == before);
        else {
            CHECK(!strcmp(trace, silent ? "tx_setup " : "request_event tx_setup "));
            CHECK(conn.core.state == ((before | 0x800) & ~3u) + 2);
        }
    }
    return TEST_DONE("btctrler_lmp_clock_setup");
}
#endif
