/* Owned HCI link policy dispatch (lmp_hci_cmd_parser.c) and the LMP QoS,
 * supervision and eSCO procedures (lmp.c), recovered from the BR23 SDK.
 * Unsupported legacy commands keep the SDK's consumed/no-event behaviour.
 * Controller-thread entry only. No connection pointer escapes the call. */
#include "btctrler/glue.h"

static u16 read16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

int hci_lmp_link_policy_cmd_handler(u16 opcode, u8 *data, u32 len)
{
    struct lmp_connection *conn;
    u32 required;
    switch (opcode) {
    case 0x0803: required = 10; break; /* Sniff Mode */
    case 0x0804: case 0x0809: case 0x080c: case 0x080f: required = 2; break;
    case 0x080b: required = 7; break;
    case 0x080d: required = 4; break;
    case 0x0807: required = 20; break;
    case 0x080e: required = 0; break;
    case 0x0805: case 0x0806: case 0x0810: case 0x0811: return 1;
    default: return 0; /* Let the enclosing parser try another group. */
    }
    if (len < required) {
        hci_send_event(0x0f, "112", 0x12, 1, opcode);
        return 1;
    }
    switch (opcode) {
    case 0x0803:
    case 0x0804:
        conn = lmp_conn_for_handle(read16(data));
        /* The SDK's sniff commands are silent for a missing handle. */
        if (!conn) break;
        hci_send_event(0x0f, "112", 0, 1, opcode);
        if (opcode == 0x0803)
            lmp_hci_sniff_mode(conn, read16(data + 2), read16(data + 4),
                               read16(data + 6), read16(data + 8));
        else lmp_hci_exit_sniff_mode(conn);
        break;
    case 0x0807: btctrlermod_qos_setup(data); break;
    case 0x0809: lmp_hci_role_discovery(read16(data) & 0x0fff); break;
    case 0x080b: lmp_hci_switch_role_command(data, data[6]); break;
    case 0x080c: lmp_hci_read_link_policy_settings(read16(data) & 0x0fff); break;
    case 0x080d: lmp_hci_write_link_policy_settings(read16(data) & 0x0fff, read16(data + 2)); break;
    case 0x080e:
        /* Stored in a byte, returned with the formatter's two-byte field. */
        hci_send_event_cmd_complete(opcode, "12", 0, gbredr_local_dev.default_link_policy_settings);
        break;
    case 0x080f:
        gbredr_local_dev.default_link_policy_settings = (u8)read16(data);
        hci_send_event_cmd_complete(opcode, "1", 0);
        break;
    }
    return 1;
}

/* lmp.c's set_qos_poll_interval; its MAX_SLOT_REQ receive path reads it too. */
u16 btctrler_qos_poll_slots;
#define QOS_REQUESTED 512u

void lmp_vendor_qos_interval_config(u16 slots)
{
    if (!slots || (slots >= 6 && slots <= 4096)) btctrler_qos_poll_slots = slots;
}

void lmp_tx_qos_req(struct lmp_connection *conn)
{
    if ((conn->core.state & QOS_REQUESTED) || !btctrler_qos_poll_slots) return;
    conn->core.state |= QOS_REQUESTED;
    /* LMP_quality_of_service_req (42): poll interval, NBC 2. */
    lmp_request(conn, "121", 42 << 1, btctrler_qos_poll_slots, 2);
}

/* The pad has no voice link: never set one up. */
void lmp_event_esco_link_setup(struct lmp_connection *conn)
{
    (void)conn;
}

u8 btctrlermod_lmp_qos_send(u16 handle, u16 slots)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) return 2;
    if (conn->core.role != 1) return 0x0c;
    conn->core.state &= ~QOS_REQUESTED;
    /* MAX_SLOT_REQ reception would send a request whenever the interval is
     * set: keep it set only for this synchronous call. */
    lmp_vendor_qos_interval_config(slots);
    lmp_tx_qos_req(conn);
    lmp_vendor_qos_interval_config(0);
    return 0;
}

u8 btctrlermod_lmp_qos_answer(u16 handle, u8 status)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (!conn) return 2;
    /* lmp_response takes the transaction ID of the PDU being answered. */
    if (status) lmp_response(conn, "111", 4 << 1, 42, status);   /* not_accepted */
    else lmp_response(conn, "11", 3 << 1, 42);                   /* accepted */
    return 0;
}

void lmp_tx_supervision_timeout(struct lmp_connection *conn)
{
    /* LMP_supervision_timeout (55) is the central's to send. */
    if (conn->core.role == 1) return;
    lmp_request(conn, "12", 55 << 1, gbredr_local_dev.super_timeout_value * 1000u / 625);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev = { .default_link_policy_settings = 3, .super_timeout_value = 2000 };
static struct lmp_connection test_conn = { .core.role = 1 };
static int test_present = 1, test_action, test_events, test_status;
static int test_a, test_b, test_c, test_d, test_interval;
static int test_qos_sends;
struct lmp_connection *lmp_conn_for_handle(int handle)
{ test_a = handle; return test_present ? &test_conn : 0; }
void lmp_hci_sniff_mode(struct lmp_connection *c, u16 max, u16 min, u16 attempt, u16 timeout)
{ CHECK(c == &test_conn); test_action = 3; test_a = max; test_b = min; test_c = attempt; test_d = timeout; }
void lmp_hci_exit_sniff_mode(struct lmp_connection *c)
{ CHECK(c == &test_conn); test_action = 4; }
void lmp_hci_role_discovery(u16 h) { test_action = 9; test_a = h; }
void lmp_hci_switch_role_command(u8 *p, u8 r) { test_action = 11; test_a = p[0]; test_b = r; }
void lmp_hci_read_link_policy_settings(u16 h) { test_action = 12; test_a = h; }
void lmp_hci_write_link_policy_settings(u16 h, u16 p) { test_action = 13; test_a = h; test_b = p; }
int hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    CHECK(event == 15 && !strcmp(format, "112"));
    test_status = va_arg(ap, int); CHECK(va_arg(ap, int) == 1);
    test_events++; va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(va_arg(ap, int) == 0);
    if (opcode == 0x080e) { CHECK(!strcmp(format, "12")); test_b = va_arg(ap, int); }
    else CHECK(opcode == 0x080f && !strcmp(format, "1"));
    test_events++; va_end(ap); return 0;
}
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(c == &test_conn);
    if (!strcmp(format, "12")) {
        CHECK(va_arg(ap, int) == 110); test_d = va_arg(ap, int);
    } else {
        CHECK(!strcmp(format, "121") && va_arg(ap, int) == 84);
        test_interval = va_arg(ap, int); CHECK(va_arg(ap, int) == 2);
        test_action = 42; test_qos_sends++;
    }
    va_end(ap); return 0;
}
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(c == &test_conn);
    test_a = va_arg(ap, int); CHECK(va_arg(ap, int) == 42);
    if (test_a == 8) { CHECK(!strcmp(format, "111")); test_b = va_arg(ap, int); }
    else CHECK(test_a == 6 && !strcmp(format, "11"));
    va_end(ap); return 0;
}
void btctrlermod_qos_setup(const u8 *p) { test_action = 7; test_a = p[12]; }
int main(void)
{
    u8 p[20] = { 0x34, 0x12, 24, 0, 8, 0, 2, 0, 1, 0, 0, 0, 0xa6 };
    CHECK(hci_lmp_link_policy_cmd_handler(0x0803, p, 10) == 1);
    CHECK(test_action == 3 && test_a == 24 && test_b == 8 && test_c == 2 && test_d == 1);
    CHECK(test_events == 1 && test_status == 0);
    test_present = 0; test_action = 0;
    hci_lmp_link_policy_cmd_handler(0x0804, p, 2);
    CHECK(test_action == 0 && test_events == 1);
    test_present = 1; hci_lmp_link_policy_cmd_handler(0x0804, p, 2); CHECK(test_action == 4);
    hci_lmp_link_policy_cmd_handler(0x0809, p, 2); CHECK(test_action == 9 && test_a == 0x234);
    hci_lmp_link_policy_cmd_handler(0x080b, p, 7); CHECK(test_action == 11 && test_a == 0x34 && test_b == 2);
    hci_lmp_link_policy_cmd_handler(0x080c, p, 2); CHECK(test_action == 12 && test_a == 0x234);
    hci_lmp_link_policy_cmd_handler(0x080d, p, 4); CHECK(test_action == 13 && test_a == 0x234 && test_b == 24);
    hci_lmp_link_policy_cmd_handler(0x080e, 0, 0); CHECK(test_b == 3);
    hci_lmp_link_policy_cmd_handler(0x080f, p, 2); CHECK(gbredr_local_dev.default_link_policy_settings == 0x34);
    hci_lmp_link_policy_cmd_handler(0x0807, p, 20); CHECK(test_action == 7 && test_a == 0xa6);
    test_action = 0; hci_lmp_link_policy_cmd_handler(0x0807, p, 19);
    CHECK(!test_action && test_status == 0x12);
    CHECK(hci_lmp_link_policy_cmd_handler(0x0811, 0, 0) == 1);
    CHECK(hci_lmp_link_policy_cmd_handler(0x0405, 0, 0) == 0);
    test_present = 0; CHECK(btctrlermod_lmp_qos_send(256, 6) == 2);
    test_present = 1; test_conn.core.role = 0; CHECK(btctrlermod_lmp_qos_send(256, 6) == 0x0c);
    test_conn.core.role = 1; CHECK(btctrlermod_lmp_qos_send(256, 6) == 0);
    CHECK(test_action == 42 && test_interval == 6 && !btctrler_qos_poll_slots);
    CHECK(test_conn.core.state & QOS_REQUESTED);
    lmp_vendor_qos_interval_config(8); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 1); /* One-shot until explicitly rearmed. */
    test_conn.core.state = 0x20; lmp_vendor_qos_interval_config(0); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 1 && test_conn.core.state == 0x20);
    lmp_vendor_qos_interval_config(4096); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(4097); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(5); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(6); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 2 && test_conn.core.state == (0x20 | QOS_REQUESTED));
    lmp_vendor_qos_interval_config(0);
    lmp_event_esco_link_setup(&test_conn);
    CHECK(btctrlermod_lmp_qos_answer(256, 0) == 0 && test_a == 6);
    CHECK(btctrlermod_lmp_qos_answer(256, 0x1e) == 0 && test_a == 8 && test_b == 0x1e);
    test_present = 0; CHECK(btctrlermod_lmp_qos_answer(256, 0) == 2);
    test_conn.core.role = 1; test_d = 0; lmp_tx_supervision_timeout(&test_conn);
    CHECK(test_d == 0);
    test_conn.core.role = 0; test_d = 0; lmp_tx_supervision_timeout(&test_conn);
    CHECK(test_d == 3200);
    return TEST_DONE("btctrler_link_policy");
}
#endif
