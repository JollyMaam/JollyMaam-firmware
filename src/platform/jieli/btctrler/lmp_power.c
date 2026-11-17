/* Classic transmit-power negotiation, recovered from SDK lmp.c.
 * RF power adjustment remains in the SDK's bredr_pwr_set implementation. */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

void link_conn_rx_remote_max_power_res(struct link_connection *link, u8 stop_incr)
{
    link->power.normal.max_power = 1;
    if (stop_incr) link->stop_incr_power = 1;
}
void link_conn_rx_remote_min_power_res(struct link_connection *link)
{ link->power.normal.min_power = 1; }
void lmp_rx_max_power(struct lmp_connection *conn)
{ link_conn_rx_remote_max_power_res(conn->link, 1); }
void lmp_rx_min_power(struct lmp_connection *conn)
{ link_conn_rx_remote_min_power_res(conn->link); }
void lmp_set_fix_power(u8 *addr, u8 value)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    if (conn) bredr_pwr_set(conn->link, 0, value);
}
void lmp_rx_incr_power(struct lmp_connection *conn)
{
    u8 result = bredr_pwr_set(conn->link, 1, 1);
    if (result != 2) return;
    if ((config_btctler_mode & 1) && !g_dut_hdl && !link_conn_tx_empty(conn->link)) return;
    lmp_response(conn, "1", PDU(MAX_POWER));
}
void lmp_rx_decr_power(struct lmp_connection *conn)
{
    u8 result = bredr_pwr_set(conn->link, 1, -1);
    if (result == 1) lmp_response(conn, "1", PDU(MIN_POWER));
}
void lmp_tx_power_req(struct lmp_connection *conn, u8 mode)
{
    if (!conn || (conn->core.state & 0x200000)) return;
    if (mode != 1 && mode != 2) return;
    if ((conn->core_s.remote_features[7] & hci_param.features[7]) & 4)
        lmp_request(conn, "P111", PDU(ESCAPE_4), POWER_CONTROL_REQ, mode == 2);
    else lmp_request(conn, "P11", PDU(mode == 1 ? DECR_POWER_REQ : INCR_POWER_REQ), 0);
}

#ifdef UNITTEST
#include <stdarg.h>
#include <string.h>
#include "test.h"
struct hci_parameter hci_param;
struct bredr_dut *g_dut_hdl;
int config_btctler_mode;
static struct lmp_connection conn;
static struct link_connection link;
static u8 address[6];
static int found = 1, power_result, power_calls, power_sel, power_inc, empty, empty_calls;
static int requests, responses, opcode, value, direction;
static const char *format;
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(addr == address); return found ? &conn : 0; }
int bredr_pwr_set(struct link_connection *lc, char sel, char inc)
{ CHECK(lc == &link); power_calls++; power_sel = sel; power_inc = (s8)inc; return power_result; }
int link_conn_tx_empty(struct link_connection *lc) { CHECK(lc == &link); empty_calls++; return empty; }
int lmp_response(struct lmp_connection *c, const char *fmt, ...)
{
    CHECK(c == &conn && !strcmp(fmt, "1"));
    va_list ap; va_start(ap, fmt); opcode = va_arg(ap, int); va_end(ap);
    responses++; return 0;
}
int lmp_request(struct lmp_connection *c, const char *fmt, ...)
{
    CHECK(c == &conn); format = fmt;
    va_list ap; va_start(ap, fmt); opcode = va_arg(ap, int); value = va_arg(ap, int);
    if (!strcmp(fmt, "P111")) direction = va_arg(ap, int);
    else CHECK(!strcmp(fmt, "P11"));
    va_end(ap); requests++; return 0;
}
int main(void)
{
    conn.link = &link;
    for (int stop = 0; stop < 256; stop++) for (int initial = 0; initial < 2; initial++) {
        memset(&link.power, 0, sizeof(link.power));
        link.power.normal.min_power = 1; link.power.normal.pwr_txing = 1; link.power.normal.pwr_tx_cnt = 9;
        link.stop_incr_power = initial;
        link_conn_rx_remote_max_power_res(&link, stop);
        CHECK(link.power.normal.max_power && link.power.normal.min_power && link.power.normal.pwr_txing && link.power.normal.pwr_tx_cnt == 9);
        CHECK(link.stop_incr_power == (stop ? 1 : initial));
        link.power.normal.min_power = 0; link_conn_rx_remote_min_power_res(&link);
        CHECK(link.power.normal.min_power && link.power.normal.max_power && link.power.normal.pwr_txing && link.power.normal.pwr_tx_cnt == 9);
    }
    memset(&link.power, 0, sizeof(link.power)); link.stop_incr_power = 0;
    lmp_rx_max_power(&conn); CHECK(link.power.normal.max_power && link.stop_incr_power && !link.power.normal.min_power);
    lmp_rx_min_power(&conn); CHECK(link.power.normal.min_power);
    for (int val = 0; val < 256; val++) {
        lmp_set_fix_power(address, val); CHECK(power_sel == 0 && (u8)power_inc == val);
    }
    int before = power_calls; found = 0; lmp_set_fix_power(address, 12); CHECK(power_calls == before); found = 1;
    /* A result is interpreted as an SDK byte, regardless of high bits. */
    for (int result = 0; result < 512; result++) for (int mode = 0; mode < 4; mode++)
        for (int dut = 0; dut < 2; dut++) for (empty = 0; empty < 2; empty++) {
            power_result = result; config_btctler_mode = mode;
            g_dut_hdl = dut ? (struct bredr_dut *)&conn : 0;
            responses = empty_calls = 0; lmp_rx_incr_power(&conn);
            CHECK(power_sel == 1 && power_inc == 1);
            int at_max = (u8)result == 2, wait_for_empty = (mode & 1) && !dut;
            CHECK(empty_calls == (at_max && wait_for_empty));
            CHECK(responses == (at_max && (!wait_for_empty || empty)));
            if (responses) CHECK(opcode == PDU(MAX_POWER));
            responses = empty_calls = 0; lmp_rx_decr_power(&conn);
            CHECK(power_sel == 1 && power_inc == -1 && !empty_calls);
            CHECK(responses == ((u8)result == 1)); if (responses) CHECK(opcode == PDU(MIN_POWER));
        }
    requests = 0; lmp_tx_power_req(0, 1); CHECK(!requests);
    for (int mask = 0; mask < 8; mask++) for (int mode = 0; mode < 256; mode++) {
        conn.core.state = 0x10000 | (mask & 1 ? 0x200000 : 0);
        conn.core_s.remote_features[7] = mask & 2 ? 0xa5 : 0xa1;
        hci_param.features[7] = mask & 4 ? 0xf5 : 0xf1;
        requests = 0; lmp_tx_power_req(&conn, mode);
        int enabled = !(mask & 1) && (mode == 1 || mode == 2);
        CHECK(requests == enabled);
        if (!enabled) continue;
        if ((mask & 6) == 6) CHECK(opcode == PDU(ESCAPE_4) && !strcmp(format, "P111") && value == POWER_CONTROL_REQ && direction == (mode == 2));
        else CHECK(opcode == PDU(mode == 1 ? DECR_POWER_REQ : INCR_POWER_REQ) && !strcmp(format, "P11") && !value);
    }
    return TEST_DONE("btctrler_lmp_power");
}
#endif
