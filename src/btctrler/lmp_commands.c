/* HCI task messages and per-connection dispatch, recovered from SDK lmp.c.
 * Queue allocation and thread scheduling stay in the SDK. */
#include <stdint.h>
#include <string.h>
#include "btctrler/glue.h"

_Static_assert(!BTCFG_TWS && !BTCFG_FAST_CONNECT && !BTCFG_BREDR_INQUIRY &&
               !BTCFG_MASTER_ESCO && BTCFG_NAME_REQ, "pad command routing configuration");
extern BTCTRLER_CONFIG int config_bredr_afh_user;
#ifndef UNITTEST
_Static_assert(sizeof(void *) == sizeof(int), "SDK task messages store pointers in one word");
#endif

void lmp_hci_cmd_handler(int *cmd)
{
    switch (cmd[0]) {
    case 0x0c03: lmp_disconnect_all_standard_connect(); break;
    case 0x0c1a: lmp_scan_enable((u8)cmd[1]); break;
    case 0x0401: case 0x0402: break; /* Inquiry disabled. */
    case 0x0cfe: { /* Internal connection cancel. */
        u8 addr[6]; memcpy(addr, cmd + 1, sizeof(addr));
        struct lmp_connection *conn = lmp_conn_for_address(addr);
        if (conn) lmp_tx_detch(conn, 0x13);
        break;
    }
    case 0x0cfd: link_page_try_start_disable(0xfd); break;
    case 0x0405:
        link_page_try_start_enable(0, (u8 *)(cmd + 1), (u8)cmd[5], cmd[4]);
        if (cmd[6] == 0xaa) hci_param.emitter_or_receiver = 1;
        break;
    case 0x0419: {
        u8 addr[6]; memcpy(addr, cmd + 1, sizeof(addr));
        lmp_hci_remote_name_request(addr, (u8)cmd[3], (u16)cmd[4]);
        break;
    }
    case 0x0c3f:
        if (config_bredr_afh_user) lmp_master_tx_set_afh_user((u8 *)(cmd + 1));
        else lmp_tx_channel_classification((u8 *)(cmd + 1));
        break;
    }
}

void lmp_hci_cmd_handler_to_conn(u8 *cmd, int *msg)
{
    u16 handle = (cmd[0] | (cmd[1] << 8)) & 0x0fff;
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) { btctrler_assert_failed("conn: lmp_hci_cmd_handler_to_conn", 0); return; }
    lmp_role_machine(conn, 0, msg);
}

void lmp_hci_cmd_to_conn_handler(int *msg)
{
    struct lmp_connection *conn = (struct lmp_connection *)(uintptr_t)(u32)msg[0];
    if (!conn) return;
    switch (msg[1]) {
    case 0x080b:
        if (conn->core.role != msg[2]) conn->core.role_switch = 1;
        break;
    case 0x0c35:
        if (conn->rx_acl_u.next != &conn->rx_acl_u) lmp_send_acl_u_packet_to_host(conn);
        break;
    case 0x0406: lmp_tx_disconnect(conn, msg); break;
    case 0x0cfc: lmp_send_test_key(conn, (u8)msg[2]); break;
    case 0x0cfb: lmp_send_user_info(conn, (u32)msg[2]); break;
    case 0x0803:
        /* The SDK optimized the ignored min_interval into undef. Pass the
         * original message value; the owned sniff handler still ignores it. */
        lmp_hci_sniff_mode(conn, (u16)msg[2], (u16)msg[3], (u16)msg[4], (u16)msg[5]);
        break;
    case 0x0804: lmp_hci_exit_sniff_mode(conn); break;
    case 0x0428: break; /* Voice disabled. */
    }
    lmp_role_machine(conn, 0, msg + 1);
    lmp_detach_check();
}

#ifdef UNITTEST
#include "test.h"
int config_bredr_afh_user;
struct hci_parameter hci_param;
static struct lmp_connection conn;
static int found = 1, asserts, role_calls, detach_calls, lookup_handle;
static int *role_msg;
static int args[5];
static u8 address[10];
static char trace[128];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
void lmp_disconnect_all_standard_connect(void) { note("reset"); }
void lmp_scan_enable(u8 mode) { args[0] = mode; note("scan"); }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ memcpy(address, addr, 6); return found ? &conn : 0; }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ lookup_handle = handle; return found ? &conn : 0; }
void lmp_tx_detch(struct lmp_connection *c, u8 reason)
{ CHECK(c == &conn); args[0] = reason; note("detch"); }
void link_page_try_start_disable(int err) { args[0] = err; note("cancel"); }
void link_page_try_start_enable(u8 name, u8 *addr, u8 sr, int offset)
{ args[0] = name; args[1] = sr; args[2] = offset; memcpy(address, addr, 6); note("page"); }
void lmp_hci_remote_name_request(u8 *addr, u8 sr, u16 offset)
{ args[0] = sr; args[1] = offset; memcpy(address, addr, 6); note("name"); }
void lmp_master_tx_set_afh_user(u8 *map) { memcpy(address, map, 10); note("afh"); }
void lmp_tx_channel_classification(u8 *map) { memcpy(address, map, 10); note("classification"); }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void lmp_role_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ CHECK(c == &conn && !rx); role_msg = msg; role_calls++; note("role"); }
void lmp_detach_check(void) { detach_calls++; note("detach"); }
void lmp_send_acl_u_packet_to_host(struct lmp_connection *c) { CHECK(c == &conn); note("acl"); }
void lmp_tx_disconnect(struct lmp_connection *c, int *msg)
{ CHECK(c == &conn); args[0] = msg[2]; note("disconnect"); }
void lmp_send_test_key(struct lmp_connection *c, u8 key)
{ CHECK(c == &conn); args[0] = key; note("key"); }
void lmp_send_user_info(struct lmp_connection *c, u32 info)
{ CHECK(c == &conn); args[0] = info; note("info"); }
void lmp_hci_sniff_mode(struct lmp_connection *c, u16 max, u16 min, u16 attempt, u16 timeout)
{ CHECK(c == &conn); args[0] = max; args[1] = min; args[2] = attempt; args[3] = timeout; note("sniff"); }
void lmp_hci_exit_sniff_mode(struct lmp_connection *c) { CHECK(c == &conn); note("unsniff"); }
static void reset(void) { trace[0] = 0; role_calls = detach_calls = 0; memset(args, 0, sizeof(args)); }
int main(void)
{
    int cmd[7] = {0};
    const u8 addr[] = {1,2,3,4,5,6}; memcpy(cmd + 1, addr, 6);
    reset(); cmd[0] = 0x0c03; lmp_hci_cmd_handler(cmd); CHECK(!strcmp(trace, "reset "));
    reset(); cmd[0] = 0x0c1a; cmd[1] = 0x103; lmp_hci_cmd_handler(cmd); CHECK(args[0] == 3);
    memcpy(cmd + 1, addr, 6);
    reset(); cmd[0] = 0x0cfe; lmp_hci_cmd_handler(cmd);
    CHECK(!strcmp(trace, "detch ") && args[0] == 0x13 && !memcmp(address, addr, 6));
    reset(); found = 0; lmp_hci_cmd_handler(cmd); CHECK(!trace[0]); found = 1;
    reset(); cmd[0] = 0x0cfd; lmp_hci_cmd_handler(cmd); CHECK(args[0] == 0xfd);
    cmd[0] = 0x0405; cmd[4] = 0x12345; cmd[5] = 0x102; cmd[6] = 0xaa;
    hci_param.default_link_policy = 5; hci_param.scan_enable = 3;
    reset(); lmp_hci_cmd_handler(cmd);
    CHECK(args[0] == 0 && args[1] == 2 && args[2] == 0x12345 && !memcmp(address, addr, 6));
    CHECK(hci_param.emitter_or_receiver && hci_param.default_link_policy == 5 && hci_param.scan_enable == 3);
    hci_param.emitter_or_receiver = 0; cmd[6] = 1; lmp_hci_cmd_handler(cmd); CHECK(!hci_param.emitter_or_receiver);
    reset(); cmd[0] = 0x0419; cmd[3] = 0x103; lmp_hci_cmd_handler(cmd);
    CHECK(args[0] == 3 && args[1] == 0x2345 && !memcmp(address, addr, 6));
    const u8 map[] = {1,2,3,4,5,6,7,8,9,10}; memcpy(cmd + 1, map, 10); cmd[0] = 0x0c3f;
    for (config_bredr_afh_user = 0; config_bredr_afh_user < 2; config_bredr_afh_user++) {
        reset(); lmp_hci_cmd_handler(cmd);
        CHECK(!strcmp(trace, config_bredr_afh_user ? "afh " : "classification ") && !memcmp(address, map, 10));
    }
    reset(); cmd[0] = 0x0401; lmp_hci_cmd_handler(cmd); cmd[0] = 0x0402; lmp_hci_cmd_handler(cmd);
    cmd[0] = 0xffff; lmp_hci_cmd_handler(cmd); CHECK(!trace[0]);
    u8 handle[] = {0x34,0xf2}; reset(); lmp_hci_cmd_handler_to_conn(handle, cmd);
    CHECK(lookup_handle == 0x234 && role_msg == cmd && role_calls == 1);
    reset(); found = 0; lmp_hci_cmd_handler_to_conn(handle, cmd); CHECK(asserts == 1 && !role_calls); found = 1;
    /* Non-PIE tests keep static storage in the SDK's 32-bit address range. */
    CHECK((uintptr_t)&conn <= UINT32_MAX);
    if ((uintptr_t)&conn > UINT32_MAX) return 1;
    int msg[7] = {(int)(uintptr_t)&conn, 0, 0x12345, 0x10234, 0x10345, 0x10456};
    const int opcodes[] = {0x080b,0x0c35,0x0406,0x0cfc,0x0cfb,0x0803,0x0804,0x0428,0xffff};
    const char *traces[] = {"role detach ","acl role detach ","disconnect role detach ",
        "key role detach ","info role detach ","sniff role detach ","unsniff role detach ","role detach ","role detach "};
    static struct list_head packet;
    conn.rx_acl_u.next = &packet;
    for (unsigned i = 0; i < sizeof(opcodes) / sizeof(*opcodes); i++) {
        reset(); msg[1] = opcodes[i]; lmp_hci_cmd_to_conn_handler(msg);
        CHECK(!strcmp(trace, traces[i]) && role_msg == msg + 1 && role_calls == 1 && detach_calls == 1);
        switch (msg[1]) {
        case 0x080b: CHECK(conn.core.role_switch); break;
        case 0x0cfc: CHECK(args[0] == 0x45); break;
        case 0x0cfb: case 0x0406: CHECK(args[0] == 0x12345); break;
        case 0x0803: CHECK(args[0] == 0x2345 && args[1] == 0x234 && args[2] == 0x345 && args[3] == 0x456); break;
        }
    }
    reset(); conn.core.role = 1; conn.core.role_switch = 0; msg[1] = 0x080b; msg[2] = 1;
    lmp_hci_cmd_to_conn_handler(msg); CHECK(!conn.core.role_switch);
    reset(); conn.rx_acl_u.next = &conn.rx_acl_u; msg[1] = 0x0c35; lmp_hci_cmd_to_conn_handler(msg);
    CHECK(!strcmp(trace, "role detach "));
    reset(); msg[0] = 0; lmp_hci_cmd_to_conn_handler(msg); CHECK(!trace[0]);
    return TEST_DONE("btctrler_lmp_commands");
}
#endif
