/* Owned HCI link policy dispatch, recovered from the BR23 SDK LLVM IR.
 * Keep the SDK's routing/return convention; QoS uses our asynchronous adapter.
 * Unsupported legacy commands retain the SDK's consumed/no-event behaviour.
 * Controller-thread entry only. No connection pointer escapes the call. */
#include "btctrler_sdk_abi.h"
#include "btstack_qos_jieli.h"

static unsigned short read16(const unsigned char *p)
{ return (unsigned short)(p[0] | ((unsigned short)p[1] << 8)); }

int hci_lmp_link_policy_cmd_handler(unsigned short opcode, unsigned char *data, int len)
{
    struct lmp_connection *conn;
    int required;
    switch (opcode) {
    case 0x0803: required = 10; break; /* Sniff Mode */
    case 0x0804: case 0x0809: case 0x080c: case 0x080f: required = 2; break;
    case 0x080b: required = 7; break;
    case 0x080d: required = 4; break;
    case 0x0807: required = 20; break;
    case 0x080e: required = 0; break;
    case 0x0805: case 0x0806: case 0x0810: case 0x0811: return 1;
    default: return 0; /* Let the enclosing SDK parser try another group. */
    }
    if (len < required) {
        BTCTRLER_BIN_hci_send_event(0x0f, "112", 0x12, 1, (int)opcode);
        return 1;
    }
    switch (opcode) {
    case 0x0803:
    case 0x0804:
        conn = BTCTRLER_BIN_lmp_conn_for_handle(read16(data));
        /* Preserve the SDK's silent missing-handle behaviour for sniff. */
        if (!conn) break;
        BTCTRLER_BIN_hci_send_event(0x0f, "112", 0, 1, (int)opcode);
        if (opcode == 0x0803)
            BTCTRLER_BIN_lmp_hci_sniff_mode(conn, read16(data + 2), read16(data + 4),
                              read16(data + 6), read16(data + 8));
        else BTCTRLER_BIN_lmp_hci_exit_sniff_mode(conn);
        break;
    case 0x0807: cyclone2_qos_setup(data); break;
    case 0x0809: BTCTRLER_BIN_lmp_hci_role_discovery(read16(data) & 0x0fff); break;
    case 0x080b: BTCTRLER_BIN_lmp_hci_switch_role_command(data, data[6]); break;
    case 0x080c: BTCTRLER_BIN_lmp_hci_read_link_policy_settings(read16(data) & 0x0fff); break;
    case 0x080d:
        BTCTRLER_BIN_lmp_hci_write_link_policy_settings(read16(data) & 0x0fff, read16(data + 2));
        break;
    case 0x080e:
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "12", 0, (int)BTCTRLER_ABI_default_policy_get());
        break;
    case 0x080f:
        BTCTRLER_ABI_default_policy_set((unsigned char)read16(data));
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0);
        break;
    }
    return 1;
}

/* Recovered SDK QoS storage, shared with its retained MAX_SLOT receive path. */
unsigned short btctrler_qos_poll_slots;
#define QOS_REQUESTED 512u

void lmp_vendor_qos_interval_config(unsigned short slots)
{
    if (!slots || (slots >= 6 && slots <= 4096)) btctrler_qos_poll_slots = slots;
}

void lmp_tx_qos_req(struct lmp_connection *conn)
{
    unsigned int state = BTCTRLER_ABI_connection_state(conn);
    if ((state & QOS_REQUESTED) || !btctrler_qos_poll_slots) return;
    BTCTRLER_ABI_connection_state_set(conn, state | QOS_REQUESTED);
    BTCTRLER_BIN_lmp_request(conn, "121", 84, (int)btctrler_qos_poll_slots, 2);
}

/* The pad has no voice link. Preserve the tested eSCO exclusion as owned
 * policy rather than rewriting a binary function body to ret void. */
void lmp_event_esco_link_setup(struct lmp_connection *conn)
{
    (void)conn;
}

unsigned char cyclone2_lmp_qos_send(unsigned short handle, unsigned short slots)
{
    struct lmp_connection *conn = BTCTRLER_BIN_lmp_conn_for_handle(handle);
    if (!conn) return 2;
    if (BTCTRLER_ABI_connection_role(conn) != 1) return 0x0c;
    BTCTRLER_ABI_connection_state_set(conn,
        BTCTRLER_ABI_connection_state(conn) & ~QOS_REQUESTED);
    /* The SDK also tests this global when receiving MAX_SLOT_REQ. Restrict
     * its lifetime to this synchronous enqueue; the controller thread owns it. */
    lmp_vendor_qos_interval_config(slots);
    lmp_tx_qos_req(conn);
    lmp_vendor_qos_interval_config(0);
    return 0;
}

unsigned char cyclone2_lmp_qos_answer(unsigned short handle, unsigned char status)
{
    struct lmp_connection *conn = BTCTRLER_BIN_lmp_conn_for_handle(handle);
    if (!conn) return 2;
    /* SDK responder derives TID from the current receive transaction. */
    if (status) BTCTRLER_BIN_lmp_response(conn, "111", 8, 42, (int)status);
    else BTCTRLER_BIN_lmp_response(conn, "11", 6, 42);
    return 0;
}

void lmp_tx_supervision_timeout(struct lmp_connection *conn)
{
    /* LMP supervision timeout is a central-only procedure. */
    if (BTCTRLER_ABI_connection_role(conn) == 1) {
        cyclone2_bringup_record(31745);
        return;
    }
    cyclone2_bringup_record(31232u | BTCTRLER_ABI_connection_role(conn));
    cyclone2_bringup_record(30976u | BTCTRLER_ABI_connection_step(conn));
    BTCTRLER_BIN_lmp_request(conn, "12", 110, (int)((unsigned int)BTCTRLER_ABI_supervision_ms() * 1000 / 625));
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct lmp_connection { int unused; };
static struct lmp_connection test_conn;
static int test_present = 1, test_role = 1, test_action, test_events, test_status;
static int test_a, test_b, test_c, test_d, test_policy = 3, test_interval;
static unsigned int test_marker, test_state;
static int test_qos_sends;
struct lmp_connection *BTCTRLER_BIN_lmp_conn_for_handle(int handle)
{ test_a = handle; return test_present ? &test_conn : 0; }
void BTCTRLER_BIN_lmp_hci_sniff_mode(struct lmp_connection *c, unsigned short max, unsigned short min,
                       unsigned short attempt, unsigned short timeout)
{ CHECK(c == &test_conn); test_action = 3; test_a = max; test_b = min; test_c = attempt; test_d = timeout; }
void BTCTRLER_BIN_lmp_hci_exit_sniff_mode(struct lmp_connection *c)
{ CHECK(c == &test_conn); test_action = 4; }
void BTCTRLER_BIN_lmp_hci_role_discovery(unsigned short h) { test_action = 9; test_a = h; }
void BTCTRLER_BIN_lmp_hci_switch_role_command(unsigned char *p, unsigned char r)
{ test_action = 11; test_a = p[0]; test_b = r; }
void BTCTRLER_BIN_lmp_hci_read_link_policy_settings(unsigned short h) { test_action = 12; test_a = h; }
void BTCTRLER_BIN_lmp_hci_write_link_policy_settings(unsigned short h, unsigned short p)
{ test_action = 13; test_a = h; test_b = p; }
int BTCTRLER_BIN_hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    CHECK(event == 15 && !strcmp(format, "112"));
    test_status = va_arg(ap, int); CHECK(va_arg(ap, int) == 1);
    test_events++; va_end(ap); return 0;
}
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(va_arg(ap, int) == 0);
    if (opcode == 0x080e) { CHECK(!strcmp(format, "12")); test_b = va_arg(ap, int); }
    else CHECK(opcode == 0x080f && !strcmp(format, "1"));
    test_events++; va_end(ap); return 0;
}
unsigned char BTCTRLER_ABI_default_policy_get(void) { return test_policy; }
void BTCTRLER_ABI_default_policy_set(unsigned char p) { test_policy = p; }
unsigned char BTCTRLER_ABI_connection_role(struct lmp_connection *c)
{ CHECK(c == &test_conn); return test_role; }
unsigned int BTCTRLER_ABI_connection_state(struct lmp_connection *c)
{ CHECK(c == &test_conn); return test_state; }
void BTCTRLER_ABI_connection_state_set(struct lmp_connection *c, unsigned int state)
{ CHECK(c == &test_conn); test_state = state; }
unsigned char BTCTRLER_ABI_connection_step(struct lmp_connection *c)
{ CHECK(c == &test_conn); return 36; }
unsigned short BTCTRLER_ABI_supervision_ms(void) { return 2000; }
void cyclone2_bringup_record(unsigned int code) { test_marker = code; }
int BTCTRLER_BIN_lmp_request(struct lmp_connection *c, const char *format, ...)
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
int BTCTRLER_BIN_lmp_response(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(c == &test_conn);
    test_a = va_arg(ap, int); CHECK(va_arg(ap, int) == 42);
    if (test_a == 8) { CHECK(!strcmp(format, "111")); test_b = va_arg(ap, int); }
    else CHECK(test_a == 6 && !strcmp(format, "11"));
    va_end(ap); return 0;
}
void cyclone2_qos_setup(const unsigned char *p) { test_action = 7; test_a = p[12]; }
int main(void)
{
    unsigned char p[20] = { 0x34, 0x12, 24, 0, 8, 0, 2, 0, 1, 0, 0, 0, 0xa6 };
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
    hci_lmp_link_policy_cmd_handler(0x080f, p, 2); CHECK(test_policy == 0x34);
    hci_lmp_link_policy_cmd_handler(0x0807, p, 20); CHECK(test_action == 7 && test_a == 0xa6);
    test_action = 0; hci_lmp_link_policy_cmd_handler(0x0807, p, 19);
    CHECK(!test_action && test_status == 0x12);
    CHECK(hci_lmp_link_policy_cmd_handler(0x0811, 0, 0) == 1);
    CHECK(hci_lmp_link_policy_cmd_handler(0x0405, 0, 0) == 0);
    test_present = 0; CHECK(cyclone2_lmp_qos_send(256, 6) == 2);
    test_present = 1; test_role = 0; CHECK(cyclone2_lmp_qos_send(256, 6) == 0x0c);
    test_role = 1; CHECK(cyclone2_lmp_qos_send(256, 6) == 0);
    CHECK(test_action == 42 && test_interval == 6 && !btctrler_qos_poll_slots);
    CHECK(test_state & QOS_REQUESTED);
    lmp_vendor_qos_interval_config(8); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 1); /* One-shot until explicitly rearmed. */
    test_state = 0x20; lmp_vendor_qos_interval_config(0); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 1 && test_state == 0x20);
    lmp_vendor_qos_interval_config(4096); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(4097); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(5); CHECK(btctrler_qos_poll_slots == 4096);
    lmp_vendor_qos_interval_config(6); lmp_tx_qos_req(&test_conn);
    CHECK(test_qos_sends == 2 && test_state == (0x20 | QOS_REQUESTED));
    lmp_vendor_qos_interval_config(0);
    lmp_event_esco_link_setup(&test_conn);
    CHECK(cyclone2_lmp_qos_answer(256, 0) == 0 && test_a == 6);
    CHECK(cyclone2_lmp_qos_answer(256, 0x1e) == 0 && test_a == 8 && test_b == 0x1e);
    test_present = 0; CHECK(cyclone2_lmp_qos_answer(256, 0) == 2);
    test_role = 1; test_d = 0; test_marker = 0; lmp_tx_supervision_timeout(&test_conn);
    CHECK(test_marker == 31745 && test_d == 0);
    test_role = 0; test_d = 0; test_marker = 0; lmp_tx_supervision_timeout(&test_conn);
    CHECK(test_d == 3200 && test_marker == (30976u | 36));
    return TEST_DONE("btctrler_link_policy");
}
#endif
