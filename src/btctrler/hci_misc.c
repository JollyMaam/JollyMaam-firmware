/* Remaining common HCI control and Classic information/status boundaries.
 * Normal replies and consumed-without-event SDK stubs are preserved. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG int config_btctler_le_roles;

static u16 handle(const u8 *data)
{
    return (data[0] | (data[1] << 8)) & 0x0fff;
}

int hci_link_control_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    u32 required;
    switch (opcode) {
    case 0x0406: required = 3; break; /* Disconnect */
    case 0x041d: required = 2; break; /* Read Remote Version Information */
    default: return 0;
    }
    /* Fix: the SDK read the handle/reason before checking the buffer. */
    if (!data || len < required) {
        hci_send_event(0x0f, "112", 0x12, 1, opcode);
        return 1;
    }
    u16 h = handle(data);
    u8 status = opcode == 0x0406 ? 2 : 0;
    if ((config_btctler_modules & 2) && (config_btctler_le_roles & 3) && ll_link_for_handle(h))
        status = ll_push_control_data(opcode, data);
    if (config_btctler_modules & 1) {
        if (opcode == 0x0406) {
            if (lmp_conn_for_handle(h) || lmp_esco_conn_for_handle(h))
                status = (u8)lmp_hci_disconnect(h, data[2]) ? 2 : 0;
        } else if (lmp_conn_for_handle(h)) {
            status = (u8)lmp_hci_read_remote_version_information(h);
        }
    }
    hci_send_event(0x0f, "112", status, 1, opcode);
    return 1;
}

int hci_link_policy_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

int hci_status_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

int hci_testing_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    switch (opcode) {
    /* The SDK consumes these without performing a test or raising an event. */
    case 0x1801: case 0x1802: case 0x1803:
    case 0x1807: case 0x1808: case 0x1809: return 1;
    case 0x1804: /* Write Simple Pairing Debug Mode: acknowledge only. */
        hci_send_event_cmd_complete(opcode, "1", data && len >= 1 ? 0 : 0x12);
        return 1;
    default: return 0;
    }
}

int hci_lmp_informational_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    (void)data; (void)len;
    if (opcode != 0x1005) return 0;
    hci_send_event_cmd_complete(opcode, "12122", 0, 640, 64, 10, 8);
    return 1;
}

int hci_lmp_status_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    switch (opcode) {
    case 0x1403: case 0x1405: case 0x1406: case 0x1408: break;
    default: return 0;
    }
    if (!data || len < 2) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
    switch (opcode) {
    /* Preserve the SDK's placeholder measurements. */
    case 0x1403: hci_send_event_cmd_complete(opcode, "1H1", 0, handle(data), 100); break;
    case 0x1405: hci_send_event_cmd_complete(opcode, "1H1", 0, handle(data), 0); break;
    /* Read AFH Channel Map and Read Encryption Key Size stay silent.
     * The transport already answers the latter for the host. */
    default: break;
    }
    return 1;
}

int hci_lmp_testing_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
int config_btctler_modules, config_btctler_le_roles;
static struct lmp_connection classic;
static struct le_link le;
static int le_present, classic_present, esco_present, le_result, classic_result;
static int le_calls, classic_calls, lookup_calls, last_handle, last_reason;
static int events, event_id, event_opcode, values[5];
static const char *event_format;
struct le_link *ll_link_for_handle(int h)
{ lookup_calls++; last_handle = h; return le_present ? &le : 0; }
struct lmp_connection *lmp_conn_for_handle(int h)
{ lookup_calls++; last_handle = h; return classic_present ? &classic : 0; }
struct lmp_connection *lmp_esco_conn_for_handle(int h)
{ lookup_calls++; last_handle = h; return esco_present ? &classic : 0; }
u8 ll_push_control_data(u16 op, const u8 *p)
{ CHECK(op == 0x0406 || op == 0x041d); CHECK(p != 0); le_calls++; return le_result; }
int lmp_hci_disconnect(u16 h, u8 reason)
{ classic_calls++; last_handle = h; last_reason = reason; return classic_result; }
int lmp_hci_read_remote_version_information(u16 h)
{ classic_calls++; last_handle = h; return classic_result; }
int hci_send_event(int id, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); events++; event_id = id; event_format = fmt;
    for (int i = 0; i < 3; i++) values[i] = va_arg(ap, int);
    va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int op, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); events++; event_opcode = op; event_format = fmt;
    int n = !strcmp(fmt, "12122") ? 5 : !strcmp(fmt, "1H1") ? 3 : 1;
    for (int i = 0; i < n; i++) values[i] = va_arg(ap, int);
    va_end(ap); return 0;
}
static void reset(void)
{
    events = le_calls = classic_calls = lookup_calls = 0;
    le_present = classic_present = esco_present = le_result = classic_result = 0;
    config_btctler_modules = 3; config_btctler_le_roles = 6;
}
int main(void)
{
    const u8 p[] = {0x34, 0xf2, 0x13};
    const u16 control[] = {0x0406, 0x041d};
    for (unsigned k = 0; k < 2; k++) {
        int required = k == 0 ? 3 : 2;
        for (int n = 0; n < required; n++) {
            reset();
            CHECK(hci_link_control_cmd_handler(control[k], p, n) == 1);
            CHECK(events == 1 && event_id == 15 && values[0] == 0x12);
            CHECK(values[1] == 1 && values[2] == control[k] && !lookup_calls);
        }
        reset(); CHECK(hci_link_control_cmd_handler(control[k], 0, required) == 1);
        CHECK(values[0] == 0x12 && !lookup_calls);
        reset(); hci_link_control_cmd_handler(control[k], p, required);
        CHECK(values[0] == (k == 0 ? 2 : 0)); /* preserve unknown-handle SDK behavior */
        for (int modules = 0; modules < 4; modules++) {
            for (int roles = 0; roles < 8; roles++) {
                reset(); config_btctler_modules = modules; config_btctler_le_roles = roles;
                le_present = classic_present = 1; le_result = 9;
                hci_link_control_cmd_handler(control[k], p, required);
                CHECK(le_calls == ((modules & 2) && (roles & 3) ? 1 : 0));
                CHECK(classic_calls == !!(modules & 1));
                CHECK(values[0] == (modules & 1 ? 0 : le_calls ? 9 : k == 0 ? 2 : 0));
                CHECK(last_handle == 0x234 || !lookup_calls);
            }
        }
        reset(); classic_present = 1; classic_result = 0x105;
        hci_link_control_cmd_handler(control[k], p, required);
        CHECK(values[0] == (k == 0 ? 2 : 5));
    }
    reset(); esco_present = 1;
    hci_link_control_cmd_handler(0x0406, p, 3);
    CHECK(classic_calls == 1 && last_reason == 0x13);
    reset(); hci_link_control_cmd_handler(0x041d, p, 2);
    CHECK(!classic_calls);
    CHECK(hci_link_control_cmd_handler(0xffff, 0, 0) == 0);

    const u16 status[] = {0x1403, 0x1405, 0x1406, 0x1408};
    for (unsigned k = 0; k < 4; k++) {
        for (int n = 0; n < 2; n++) {
            reset(); CHECK(hci_lmp_status_parameters_cmd_handler(status[k], p, n) == 1);
            CHECK(events == 1 && values[0] == 0x12 && event_opcode == status[k]);
        }
        reset(); hci_lmp_status_parameters_cmd_handler(status[k], p, 2);
        CHECK(events == (k < 2 ? 1 : 0));
        if (k < 2) CHECK(values[0] == 0 && values[1] == 0x234 && values[2] == (k ? 0 : 100));
    }
    reset(); CHECK(hci_lmp_informational_parameters_cmd_handler(0x1005, 0, 0) == 1);
    CHECK(events == 1 && !strcmp(event_format, "12122"));
    CHECK(values[0] == 0 && values[1] == 640 && values[2] == 64 && values[3] == 10 && values[4] == 8);
    reset(); CHECK(hci_testing_cmd_handler(0x1804, 0, 0) == 1);
    CHECK(values[0] == 0x12);
    hci_testing_cmd_handler(0x1804, p, 1); CHECK(values[0] == 0);
    const u16 silent[] = {0x1801,0x1802,0x1803,0x1807,0x1808,0x1809};
    for (unsigned k = 0; k < sizeof(silent) / sizeof(*silent); k++) {
        reset(); CHECK(hci_testing_cmd_handler(silent[k], 0, 0) == 1); CHECK(events == 0);
    }
    reset();
    CHECK(hci_link_policy_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(hci_status_parameters_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(hci_lmp_testing_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(hci_lmp_status_parameters_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(hci_lmp_informational_parameters_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(hci_testing_cmd_handler(0xffff, 0, 0) == 0);
    CHECK(events == 0);
    return TEST_DONE("btctrler_hci_misc");
}
#endif
