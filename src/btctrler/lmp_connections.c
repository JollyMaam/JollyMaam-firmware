/* Classic connection slots, role dispatch and startup/reset, from SDK lmp.c. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && BTCFG_NAME_REQ && !BTCFG_TWS,
               "one Classic slot, standard HCI host, remote names, no TWS");
#ifndef UNITTEST
_Static_assert(sizeof(struct lmp_connection) == 280 &&
               offsetof(struct lmp_connection, timer) == 16 &&
               offsetof(struct lmp_connection, core.enc_state) == 174 &&
               offsetof(struct lmp_connection, core.state) == 176 &&
               offsetof(struct lmp_connection, link) == 220,
               "SDK connection allocation and timeout offsets");
#endif

void lmp_init(void) { memset(lmp_acl_link, 0, sizeof(lmp_acl_link)); }

struct lmp_connection *lmp_create_conn(void)
{
    struct lmp_connection *conn = lmp_acl_link;
    if (conn->core_s.handle) return 0;
    memset(conn, 0, sizeof(*conn));
    conn->core_s.handle = 0x30;
    conn->rx_acl_u.next = conn->rx_acl_u.prev = &conn->rx_acl_u;
    conn->change_rx_bulk_timer = 0xff;
    return conn;
}

struct lmp_connection *lmp_conn_for_handle(int handle)
{
    struct lmp_connection *conn = lmp_acl_link;
    return (config_btctler_modules & 1) && conn->core_s.handle && conn->core_s.handle == handle ? conn : 0;
}

struct lmp_connection *lmp_conn_for_link(void *link)
{ return lmp_acl_link[0].link == link ? lmp_acl_link : 0; }

struct lmp_connection *lmp_get_other_conn(struct lmp_connection *conn)
{ return conn != lmp_acl_link && lmp_acl_link[0].core_s.handle ? lmp_acl_link : 0; }

u8 *lmp_conn_remote_address(struct lmp_connection *conn) { return conn->core_s.remote_addr; }

struct lmp_connection *lmp_conn_for_active_state(void)
{
    local_irq_disable();
    struct lmp_connection *conn = lmp_acl_link;
    if (!conn->core_s.handle || !conn->link || (!conn->esco_conn && !conn->sbc_open)) conn = 0;
    local_irq_enable();
    return conn;
}

u8 lmp_get_conn_num(void)
{
    local_irq_disable();
    u8 count = lmp_acl_link[0].core_s.handle && lmp_acl_link[0].link;
    local_irq_enable();
    return count;
}

void lmp_role_machine_handler_register(struct lmp_connection *conn, void *handle)
{ conn->role_machine_handler = handle; }

void lmp_role_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    if (!conn || !conn->link || (conn->core.state & 8) || !conn->role_machine_handler) return;
    int ret;
    do {
        ret = conn->role_machine_handler(conn, rx, hci_cmd);
        /* Return 1 runs the next stage with the same received packet; return
         * 2 consumes it. The handler may also change during that call. */
        if (ret == 2) rx = 0;
    } while (ret);
}

void lmp_tx_detch(struct lmp_connection *conn, u8 err_code)
{
    if (!conn->link || (conn->core.state & 8)) return;
    conn->core.step = LMP_TX_FINISH;
    conn->core.stage = LMP_STAGE_DISCONN;
    conn->core.state |= 8;
    conn->err_code = err_code;
    lmp_request(conn, "11", PDU(DETACH), err_code == 0x16 ? 0x13 : err_code);
    bredr_link_event(conn->link, 1, 5);
    os_time_dly(20);
}

void lmp_tx_disconnect(struct lmp_connection *conn, int *rx)
{ lmp_tx_detch(conn, (u8)rx[2]); }

void lmp_set_supervision_timeout(struct lmp_connection *conn, int timeout)
{ (void)conn; (void)timeout; } /* SDK stub */

/* The SDK callback was file-static and addressed connection fields by offset. */
void lmp_connection_timeout(void *priv)
{
    struct lmp_connection *conn = priv;
    sys_timer_del(conn->timer);
    conn->timer = 0;
    if (!(conn->core.state & 2) || (!(conn->core.enc_state & 4) && hci_param.need_enc)) {
        if (!conn->link || g_dut_hdl) return;
        lmp_tx_detch(conn, 8);
        lmp_detach_check();
    }
}

int lmp_connection_start(struct link_connection *lc, u8 role)
{
    struct lmp_connection *conn = lmp_create_conn();
    if (!conn) { btctrler_assert_failed("conn != NULL: lmp_connection_start", 0); return -12; }
    /* The SDK throttled other connections here; this build has one slot. */
    conn->link = lc;
    conn->core.role = role;
    conn->core.link_policy = hci_param.default_link_policy;
    conn->core.state = 0;
    conn->core_s.remote_sr = lc->core.remote_sr;
    conn->core_s.remote_class_dev = lc->core.remote_class_dev;
    memcpy(conn->core_s.remote_addr, lc->core.remote_addr, 6);
    if (role == 0) {
        if (page_parm.remote_name_req) {
            page_parm.remote_name_req = 0;
            conn->core.state = 1u << 21;
            conn->role_machine_handler = lmp_name_req_machine;
            lmp_name_req_init(conn);
        } else {
            conn->role_machine_handler = lmp_master_machine;
            lmp_master_init(conn);
        }
    } else {
        conn->role_machine_handler = lmp_slave_machine;
        conn->core.stage = LMP_STAGE_CONN;
        conn->core.step = LMP_WAIT_HOST_CONNECTION_REQ;
    }
    conn->timer = sys_timer_add(conn, lmp_connection_timeout, 5000);
    conn->core.state |= 1;
    lmp_scan_enable(hci_param.scan_enable);
    return 0;
}

void lmp_close_all(void)
{
    link_page_try_start_disable(-14);
    link_inquiry_scan_try_start_disable();
    link_page_scan_try_start_disable();
}

void lmp_reset(void)
{
    if (lmp_get_conn_num()) btctrler_assert_failed("lmp_get_conn_num() == 0", 0);
    lmp_close_all();
    link_task_close_all();
    bredr_link_close();
    link_bulk_init();
    bredr_link_init();
    lmp_init();
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
struct hci_parameter hci_param;
struct page_parm_t page_parm;
struct bredr_dut *g_dut_hdl;
int config_btctler_modules = 3;
static struct link_connection link;
static int irq_depth, irq_pairs, asserts, detach_checks, request_op, request_reason;
static int calls, callback_mode, cmd[6], timer_id;
static struct bredr_rx_bulk packet;
static struct bredr_rx_bulk *seen_rx[4];
static void (*timer_func)(void *);
static char trace[128];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
void local_irq_disable(void) { CHECK(!irq_depth); irq_depth++; }
void local_irq_enable(void) { CHECK(irq_depth == 1); irq_depth--; irq_pairs++; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == lmp_acl_link && !strcmp(format, "11"));
    va_list ap; va_start(ap, format); request_op = va_arg(ap, int); request_reason = va_arg(ap, int);
    va_end(ap); note("request"); return 0;
}
int bredr_link_event(void *lc, int argc, ...)
{
    va_list ap; va_start(ap, argc); int event = va_arg(ap, int); va_end(ap);
    CHECK(lc == &link && argc == 1 && event == 5); note("event"); return 0;
}
void os_time_dly(int ticks) { CHECK(ticks == 20); note("wait"); }
void lmp_detach_check(void) { detach_checks++; note("detach"); }
void sys_timer_del(u16 id) { timer_id = id; note("del"); }
u16 sys_timer_add(void *priv, void (*func)(void *), u32 msec)
{
    CHECK(priv == lmp_acl_link && msec == 5000 && !(lmp_acl_link[0].core.state & 1));
    timer_func = func; note("timer"); return 321;
}
void lmp_scan_enable(u8 mode) { CHECK(mode == hci_param.scan_enable); note("scan"); }
static void check_started(struct lmp_connection *c)
{
    CHECK(c == lmp_acl_link && c->link == &link && c->core.link_policy == 5);
    CHECK(c->core_s.remote_sr == 2 && c->core_s.remote_class_dev == 0x123456);
    CHECK(!memcmp(c->core_s.remote_addr, link.core.remote_addr, 6));
}
void lmp_master_init(struct lmp_connection *c)
{ check_started(c); CHECK(c->core.state == 0 && c->role_machine_handler == lmp_master_machine); note("master"); }
void lmp_name_req_init(struct lmp_connection *c)
{ check_started(c); CHECK(c->core.state == (1u << 21) && !page_parm.remote_name_req); note("name"); }
int lmp_master_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ (void)c; (void)rx; (void)msg; return 0; }
int lmp_slave_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ (void)c; (void)rx; (void)msg; return 0; }
int lmp_name_req_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ (void)c; (void)rx; (void)msg; return 0; }
void link_page_try_start_disable(int err) { CHECK(err == -14); note("page"); }
void link_inquiry_scan_try_start_disable(void) { note("inquiry"); }
void link_page_scan_try_start_disable(void) { note("pagescan"); }
void link_task_close_all(void) { note("tasks"); }
void bredr_link_close(void) { note("close"); }
int link_bulk_init(void) { note("bulk"); return 0; }
void bredr_link_init(void) { note("init"); }
static int second(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ CHECK(c == lmp_acl_link && msg == cmd); seen_rx[calls++] = rx; return 0; }
static int first(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{
    CHECK(c == lmp_acl_link && msg == cmd); seen_rx[calls++] = rx;
    if (callback_mode) { c->role_machine_handler = second; return 2; }
    const int ret[] = {1, 2, 1, 0}; return ret[calls - 1];
}
static void clear(void) { lmp_init(); trace[0] = 0; }
int main(void)
{
    struct lmp_connection *c = lmp_acl_link;
    memset(c, 0xa5, sizeof(*c)); lmp_init();
    struct lmp_connection zero = {0}; CHECK(!memcmp(c, &zero, sizeof(*c)));
    CHECK(lmp_conn_for_link(0) == c); /* preserve SDK null-link lookup */
    CHECK(lmp_conn_for_handle(0) == 0 && lmp_get_conn_num() == 0);
    CHECK(lmp_create_conn() == c && c->core_s.handle == 0x30 && c->change_rx_bulk_timer == 0xff);
    CHECK(c->rx_acl_u.next == &c->rx_acl_u && c->rx_acl_u.prev == &c->rx_acl_u);
    CHECK(!lmp_create_conn());
    for (int modules = 0; modules < 4; modules++) {
        config_btctler_modules = modules;
        for (int h = -1; h < 0x10001; h++)
            CHECK(lmp_conn_for_handle(h) == ((modules & 1) && h == 0x30 ? c : 0));
    }
    config_btctler_modules = 3;
    CHECK(lmp_get_other_conn(c) == 0 && lmp_get_other_conn(0) == c);
    CHECK(lmp_conn_remote_address(c) == c->core_s.remote_addr);
    c->link = &link; CHECK(lmp_conn_for_link(&link) == c && lmp_get_conn_num() == 1);
    for (int mask = 0; mask < 16; mask++) {
        c->core_s.handle = mask & 1 ? 0x30 : 0;
        c->link = mask & 2 ? &link : 0;
        c->sbc_open = !!(mask & 4); c->esco_conn = mask & 8 ? (struct lmp_esco *)&link : 0;
        CHECK(lmp_conn_for_active_state() == ((mask & 3) == 3 && (mask & 12) ? c : 0));
        CHECK(!irq_depth);
    }
    clear(); c->link = &link; c->role_machine_handler = first;
    lmp_role_machine(c, &packet, cmd);
    CHECK(calls == 4 && seen_rx[0] == &packet && seen_rx[1] == &packet && !seen_rx[2] && !seen_rx[3]);
    calls = 0; callback_mode = 1; lmp_role_machine(c, &packet, cmd);
    CHECK(calls == 2 && seen_rx[0] == &packet && !seen_rx[1]);
    calls = 0; c->role_machine_handler = first;
    lmp_role_machine(0, &packet, cmd); c->link = 0; lmp_role_machine(c, &packet, cmd);
    c->link = &link; c->core.state = 8; lmp_role_machine(c, &packet, cmd);
    c->core.state = 0; c->role_machine_handler = 0; lmp_role_machine(c, &packet, cmd); CHECK(!calls);
    lmp_role_machine_handler_register(c, first); CHECK(c->role_machine_handler == first);
    clear(); c->link = &link; c->core.state = 0x100;
    lmp_tx_detch(c, 0x16);
    CHECK(request_op == PDU(DETACH) && request_reason == 0x13 && c->err_code == 0x16);
    CHECK(c->core.state == 0x108 && c->core.stage == LMP_STAGE_DISCONN && c->core.step == LMP_TX_FINISH);
    CHECK(!strcmp(trace, "request event wait "));
    trace[0] = 0; lmp_tx_detch(c, 8); CHECK(!trace[0] && c->err_code == 0x16);
    c->core.state = 0; cmd[2] = 0x108; lmp_tx_disconnect(c, cmd); CHECK(c->err_code == 8);
    clear(); lmp_tx_detch(c, 8); CHECK(!trace[0]);
    link.core.remote_sr = 2; link.core.remote_class_dev = 0x123456; memset(link.core.remote_addr, 0x5a, 6);
    hci_param.default_link_policy = 5; hci_param.scan_enable = 3;
    for (int mode = 0; mode < 3; mode++) {
        clear(); page_parm.remote_name_req = mode == 1;
        CHECK(lmp_connection_start(&link, mode == 2 ? 1 : 0) == 0);
        check_started(c); CHECK(c->timer == 321 && c->core.state == (mode == 1 ? (1u << 21) | 1 : 1));
        CHECK(!strcmp(trace, mode == 0 ? "master timer scan " : mode == 1 ? "name timer scan " : "timer scan "));
        if (mode == 2) CHECK(c->core.step == LMP_WAIT_HOST_CONNECTION_REQ && c->role_machine_handler == lmp_slave_machine);
        trace[0] = 0; int old = asserts;
        CHECK(lmp_connection_start(&link, 0) == -12 && asserts == old + 1 && !trace[0]);
    }
    for (int mask = 0; mask < 32; mask++) {
        clear(); c->timer = 321; c->core.state = mask & 1 ? 2 : 0;
        c->core.enc_state = mask & 2 ? 4 : 0; hci_param.need_enc = !!(mask & 4);
        c->link = mask & 8 ? &link : 0; struct bredr_dut dut; g_dut_hdl = mask & 16 ? &dut : 0;
        detach_checks = 0; timer_func(c);
        int detach = (!(mask & 1) || (!(mask & 2) && (mask & 4))) && (mask & 8) && !(mask & 16);
        CHECK(!c->timer && timer_id == 321 && detach_checks == !!detach);
        CHECK(!strcmp(trace, detach ? "del request event wait detach " : "del "));
    }
    g_dut_hdl = 0; clear(); lmp_reset();
    CHECK(!strcmp(trace, "page inquiry pagescan tasks close bulk init "));
    CHECK(!memcmp(c, &zero, sizeof(*c)) && !irq_depth && irq_pairs);
    return TEST_DONE("btctrler_lmp_connections");
}
#endif
