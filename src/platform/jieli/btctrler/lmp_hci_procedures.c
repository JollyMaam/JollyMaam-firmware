/* Public Classic HCI procedures, recovered from SDK lmp_hci.c. */
#include "btctrler/glue.h"

#ifdef CONFIG_CPU_BD19
#define LMP_HCI_CODE __attribute__((section(".classic_lmp_code")))
#else
#define LMP_HCI_CODE
#endif

LMP_HCI_CODE int lmp_hci_reset(void)
{ btctrler_hci_cmd_to_task(0x400002, 1, 0x0c03); return 0; }
LMP_HCI_CODE int lmp_hci_write_scan_enable(u8 enable)
{ btctrler_hci_cmd_to_task(0x400002, 2, 0x0c1a, enable); return 0; }
LMP_HCI_CODE int lmp_hci_set_connection_encryption(u16 handle, int enable)
{ return lmp_hci_cmd_to_conn_for_handle(handle, 2, 0x0413, enable); }
LMP_HCI_CODE void lmp_hci_sniff_mode_command(u16 handle, u16 max_interval, u16 min_interval,
                                             u16 attempt, u16 timeout)
{ lmp_hci_cmd_to_conn_for_handle(handle, 5, 0x0803, max_interval, min_interval, attempt, timeout); }
LMP_HCI_CODE void lmp_hci_exit_sniff_mode_command(u16 handle)
{ lmp_hci_cmd_to_conn_for_handle(handle, 1, 0x0804); }
LMP_HCI_CODE void lmp_hci_authentication_requested(u16 handle)
{ lmp_hci_cmd_to_conn_for_handle(handle, 1, 0x0411); }
/* The SDK's public setter is a no-op; the HCI parser has its own path. */
LMP_HCI_CODE void lmp_hci_write_link_supervision_timeout(u16 handle, int tm)
{ (void)handle; (void)tm; }

LMP_HCI_CODE int lmp_hci_disconnect(u16 handle, u8 reason)
{
    if (!(config_btctler_modules & 1)) return 0;
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) return lmp_hci_disconnect_sync_connection(handle, reason);
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    return lmp_hci_cmd_to_conn_for_handle(handle, 2, 0x0406, reason);
}
LMP_HCI_CODE int lmp_hci_test_key_cmd(u8 cmd, u16 handle)
{
    return lmp_conn_for_handle(handle) ?
        lmp_hci_cmd_to_conn_for_handle(handle, 2, 0x0cfc, cmd) : 0;
}
LMP_HCI_CODE int lmp_hci_send_user_info_cmd(u32 info, u16 handle)
{
    return lmp_conn_for_handle(handle) ?
        lmp_hci_cmd_to_conn_for_handle(handle, 2, 0x0cfb, info) : 0;
}

LMP_HCI_CODE void lmp_hci_read_remote_supported_features(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) { btctrler_assert_failed("remote features connection", handle); return; }
    hci_send_event(0x0b, "1Hc08", 0, handle, conn->core_s.remote_features);
}
LMP_HCI_CODE void lmp_hci_read_remote_extended_features(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) { btctrler_assert_failed("remote extended features connection", handle); return; }
    hci_send_event(0x23, "1H11c08", 0, handle, 1, 1, conn->core_s.remote_ext_features);
}
LMP_HCI_CODE void lmp_hci_read_clock_offset(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) return;
    if (!conn->core.role) lmp_tx_clkoffset_req(conn);
    else hci_send_event(0x1c, "122", 0, handle, bredr_link_clk_offset(conn->link));
}
LMP_HCI_CODE int lmp_hci_read_remote_version_information(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
#ifdef CONFIG_CPU_BD19
    lmp_tx_version_req(conn);
#else
    if (!conn) return 0x02; /* Fix: the SDK passed null to the LMP sender. */
    lmp_tx_version_req(conn);
#endif
    return 0;
}
LMP_HCI_CODE int lmp_hci_refresh_encryption_key(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
#ifndef CONFIG_CPU_BD19
    if (!conn) return 0x02;
#endif
    lmp_tx_pause_encryption_req(conn);
    return 0;
}
LMP_HCI_CODE void lmp_hci_role_discovery(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    hci_send_event_cmd_complete(0x0809, "1H1", conn ? 0 : 0x0c, handle, conn ? conn->core.role : 0);
}
LMP_HCI_CODE void lmp_hci_read_link_policy_settings(u16 handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    hci_send_event_cmd_complete(0x080c, "1H1", conn ? 0 : 0x0c, handle, conn ? conn->core.link_policy : 0);
}
LMP_HCI_CODE void lmp_hci_write_link_policy_settings(u16 handle, u16 policy)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (conn) conn->core.link_policy = policy;
    hci_send_event_cmd_complete(0x080d, "1H", conn ? 0 : 0x0c, handle);
}
LMP_HCI_CODE u16 lmp_hci_read_default_link_policy_settings(void)
{ return gbredr_local_dev.default_link_policy_settings; }
LMP_HCI_CODE void lmp_hci_write_default_link_policy_settings(u16 settings)
{ gbredr_local_dev.default_link_policy_settings = settings; }

#ifdef UNITTEST
#include <stdarg.h>
#include <string.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev;
int config_btctler_modules = 3;
static struct lmp_connection conn;
static struct link_connection link;
static int found = 1, lookups, queued, task, args[8], count, result = 7;
static int deleted, sync_calls, clk_requests, version_requests, pause_requests, asserts;
static int event, complete, values[5];
static const char *format;
static const void *features;
struct lmp_connection *lmp_conn_for_handle(int handle)
{ CHECK(handle == 0x123); lookups++; return found ? &conn : 0; }
int lmp_hci_cmd_to_conn_for_handle(u16 handle, int argc, ...)
{
    CHECK(handle == 0x123 && argc > 0 && argc <= 8);
    va_list ap; va_start(ap, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(ap, int);
    va_end(ap); count = argc; queued++; return result;
}
int btctrler_hci_cmd_to_task(int cmd, int argc, ...)
{
    CHECK(cmd == 0x400002 && argc > 0 && argc <= 8);
    va_list ap; va_start(ap, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(ap, int);
    va_end(ap); count = argc; task++; return -1;
}
void sys_timer_del(u16 id) { CHECK(id == 23 && conn.enc_timer == 23); deleted++; }
int lmp_hci_disconnect_sync_connection(int handle, u8 reason)
{ CHECK(handle == 0x123 && reason == 0x16); sync_calls++; return 9; }
void lmp_tx_clkoffset_req(struct lmp_connection *c) { CHECK(c == &conn); clk_requests++; }
int bredr_link_clk_offset(struct link_connection *lc) { CHECK(lc == &link); return 0xface; }
void lmp_tx_version_req(struct lmp_connection *c)
{ CHECK(!c || c == &conn); version_requests++; }
void lmp_tx_pause_encryption_req(struct lmp_connection *c)
{ CHECK(!c || c == &conn); pause_requests++; }
void btctrler_assert_failed(const char *what, int value)
{ CHECK(what && value == 0x123); asserts++; }
int hci_send_event(int code, const char *fmt, ...)
{
    event = code; format = fmt;
    va_list ap; va_start(ap, fmt);
    values[0] = va_arg(ap, int); values[1] = va_arg(ap, int);
    if (code == 0x0b) features = va_arg(ap, u8 *);
    else if (code == 0x23) {
        values[2] = va_arg(ap, int); values[3] = va_arg(ap, int); features = va_arg(ap, u8 *);
    } else { CHECK(code == 0x1c); values[2] = va_arg(ap, int); }
    va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int code, const char *fmt, ...)
{
    complete = code; format = fmt;
    va_list ap; va_start(ap, fmt);
    values[0] = va_arg(ap, int); values[1] = va_arg(ap, int);
    if (strcmp(fmt, "1H")) values[2] = va_arg(ap, int);
    va_end(ap); return 0;
}
int main(void)
{
    conn.link = &link;
    CHECK(lmp_hci_reset() == 0 && task == 1 && count == 1 && args[0] == 0x0c03);
    for (int i = 0; i < 256; i++) {
        CHECK(lmp_hci_write_scan_enable(i) == 0 && count == 2 && args[0] == 0x0c1a && args[1] == i);
    }
    CHECK(lmp_hci_set_connection_encryption(0x123, 0x12345) == result);
    CHECK(count == 2 && args[0] == 0x0413 && args[1] == 0x12345);
    lmp_hci_sniff_mode_command(0x123, 0xffff, 0x8000, 0x1234, 0xabcd);
    CHECK(count == 5 && args[0] == 0x0803 && args[1] == 0xffff && args[2] == 0x8000 &&
          args[3] == 0x1234 && args[4] == 0xabcd);
    lmp_hci_exit_sniff_mode_command(0x123); CHECK(count == 1 && args[0] == 0x0804);
    lmp_hci_authentication_requested(0x123); CHECK(count == 1 && args[0] == 0x0411);
    int before = queued;
    lmp_hci_write_link_supervision_timeout(0x123, 100); CHECK(queued == before && !lookups);
    for (int modules = 0; modules < 4; modules++) {
        config_btctler_modules = modules; before = queued; int reads = lookups;
        CHECK(lmp_hci_disconnect(0x123, 0x16) == (modules & 1 ? result : 0));
        CHECK(queued == before + !!(modules & 1) && lookups == reads + !!(modules & 1));
        if (modules & 1) CHECK(count == 2 && args[0] == 0x0406 && args[1] == 0x16);
    }
    conn.enc_timer = 23;
    CHECK(lmp_hci_disconnect(0x123, 0x16) == result && deleted == 1 && !conn.enc_timer);
    found = 0; CHECK(lmp_hci_disconnect(0x123, 0x16) == 9 && sync_calls == 1);
    before = queued;
    CHECK(lmp_hci_test_key_cmd(0xaa, 0x123) == 0 && lmp_hci_send_user_info_cmd(0xabcdef, 0x123) == 0);
    CHECK(queued == before); found = 1;
    CHECK(lmp_hci_test_key_cmd(0xaa, 0x123) == result && count == 2 && args[0] == 0x0cfc && args[1] == 0xaa);
    CHECK(lmp_hci_send_user_info_cmd(0xabcdef, 0x123) == result && count == 2 && args[0] == 0x0cfb && args[1] == 0xabcdef);
    for (found = 0; found < 2; found++) {
        int old_asserts = asserts; event = 0;
        lmp_hci_read_remote_supported_features(0x123);
        if (found) CHECK(event == 0x0b && !strcmp(format, "1Hc08") && features == conn.core_s.remote_features && !values[0] && values[1] == 0x123);
        else CHECK(!event && asserts == old_asserts + 1);
        event = 0; old_asserts = asserts; lmp_hci_read_remote_extended_features(0x123);
        if (found) CHECK(event == 0x23 && !strcmp(format, "1H11c08") && features == conn.core_s.remote_ext_features && !values[0] && values[1] == 0x123 && values[2] == 1 && values[3] == 1);
        else CHECK(!event && asserts == old_asserts + 1);
#ifdef CONFIG_CPU_BD19
        CHECK(!lmp_hci_read_remote_version_information(0x123));
        CHECK(!lmp_hci_refresh_encryption_key(0x123));
#else
        CHECK(lmp_hci_read_remote_version_information(0x123) == (found ? 0 : 2));
        CHECK(lmp_hci_refresh_encryption_key(0x123) == (found ? 0 : 2));
#endif
        for (int role = 0; role < 2; role++) {
            conn.core.role = role; event = 0; int old_clk = clk_requests;
            lmp_hci_read_clock_offset(0x123);
            CHECK(clk_requests == old_clk + (found && !role));
            if (found && role) CHECK(event == 0x1c && !strcmp(format, "122") && !values[0] && values[1] == 0x123 && values[2] == 0xface);
            else CHECK(!event);
            lmp_hci_role_discovery(0x123);
            CHECK(complete == 0x0809 && !strcmp(format, "1H1") && values[0] == (found ? 0 : 0x0c) && values[1] == 0x123 && values[2] == (found ? role : 0));
        }
        conn.core.link_policy = 0xa5; lmp_hci_read_link_policy_settings(0x123);
        CHECK(complete == 0x080c && !strcmp(format, "1H1") && values[0] == (found ? 0 : 0x0c) && values[1] == 0x123 && values[2] == (found ? 0xa5 : 0));
        lmp_hci_write_link_policy_settings(0x123, 0xabcd);
        CHECK(complete == 0x080d && !strcmp(format, "1H") && values[0] == (found ? 0 : 0x0c) && values[1] == 0x123 && conn.core.link_policy == (found ? 0xcd : 0xa5));
    }
#ifdef CONFIG_CPU_BD19
    CHECK(version_requests == 2 && pause_requests == 2);
#else
    CHECK(version_requests == 1 && pause_requests == 1);
#endif
    for (int i = 0; i < 65536; i++) {
        lmp_hci_write_default_link_policy_settings(i);
        CHECK(lmp_hci_read_default_link_policy_settings() == (u8)i);
    }
    return TEST_DONE("btctrler_lmp_hci_procedures");
}
#endif
