/* Public Classic HCI pairing replies, recovered from SDK lmp_hci.c. */
#include <string.h>
#include "btctrler/glue.h"

#ifdef CONFIG_CPU_BD19
#define LMP_HCI_CODE __attribute__((section(".classic_lmp_code")))
typedef u32 unaligned_u32 __attribute__((aligned(1), may_alias));
#else
#define LMP_HCI_CODE
#endif

static u32 read32(const u8 *data)
{ return data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 | (u32)data[3] << 24; }

LMP_HCI_CODE void lmp_hci_io_capability_request_reply(u8 *addr, u8 io_cap, u8 oob_data, u8 auth_req)
{ lmp_hci_cmd_to_conn_for_addr(addr, 4, 0x042b, io_cap, oob_data, auth_req); }
LMP_HCI_CODE void lmp_hci_user_confirmation_request_reply(u8 *addr)
{ lmp_hci_cmd_to_conn_for_addr(addr, 1, 0x042c); }
LMP_HCI_CODE void lmp_hci_user_confirmation_request_negative_reply(u8 *addr)
{ lmp_hci_cmd_to_conn_for_addr(addr, 1, 0x042d); }
LMP_HCI_CODE void lmp_hci_pin_code_request_negative_reply(u8 *addr)
{ lmp_hci_cmd_to_conn_for_addr(addr, 1, 0x040e); }
LMP_HCI_CODE void lmp_hci_link_key_request_negative_reply(u8 *addr)
{ lmp_hci_cmd_to_conn_for_addr(addr, 1, 0x040c); }
LMP_HCI_CODE void lmp_hci_accept_connection_request(u8 *addr, u8 role)
{ lmp_hci_cmd_to_conn_for_addr(addr, 2, 0x0409, role); }
LMP_HCI_CODE void lmp_hci_reject_connection_request(u8 *addr, u8 reason)
{ lmp_hci_cmd_to_conn_for_addr(addr, 2, 0x040a, reason); }
LMP_HCI_CODE void lmp_hci_send_keypress_notification(u8 *addr, u8 key)
{ lmp_hci_cmd_to_conn_for_addr(addr, 2, 0x0c60, key); }
LMP_HCI_CODE void lmp_hci_user_keypress_request_reply(u8 *addr, u32 key)
{
#ifdef CONFIG_CPU_BD19
    lmp_hci_cmd_to_conn_for_addr(addr, 5, 0x042e, key);
#else
    /* Fix: the SDK declared five words but supplied only these two. */
    lmp_hci_cmd_to_conn_for_addr(addr, 2, 0x042e, key);
#endif
}
LMP_HCI_CODE void lmp_hci_user_keypress_request_negative_reply(u8 *addr, u8 key)
{
#ifdef CONFIG_CPU_BD19
    (void)key; /* HCI User Passkey Request Negative Reply has no key word. */
    lmp_hci_cmd_to_conn_for_addr(addr, 0x042f);
#else
    (void)key; /* HCI User Passkey Request Negative Reply has no key word. */
    /* Fix: the SDK passed the opcode as argc and omitted the opcode word. */
    lmp_hci_cmd_to_conn_for_addr(addr, 1, 0x042f);
#endif
}
LMP_HCI_CODE void lmp_hci_link_key_request_reply(u8 *addr, u8 *link_key)
{
#ifdef CONFIG_CPU_BD19
    const unaligned_u32 *key = (const unaligned_u32 *)link_key;
    lmp_hci_cmd_to_conn_for_addr(addr, 5, 0x040b, key[0], key[1], key[2], key[3]);
#else
    lmp_hci_cmd_to_conn_for_addr(addr, 5, 0x040b, read32(link_key), read32(link_key + 4),
                               read32(link_key + 8), read32(link_key + 12));
#endif
}
LMP_HCI_CODE void lmp_hci_pin_code_request_reply(u8 *addr, u8 len, u8 *pin_code)
{
#ifdef CONFIG_CPU_BD19
    const unaligned_u32 *pin = (const unaligned_u32 *)pin_code;
    lmp_hci_cmd_to_conn_for_addr(addr, 6, 0x040d, len, pin[0], pin[1], pin[2], pin[3]);
#else
    /* Fix: a short PIN must not require a readable 16-byte input buffer. */
    if (len > 16 || (!pin_code && len)) {
        btctrler_assert_failed("PIN length or buffer", len);
        return;
    }
    u8 pin[16] = {0};
    if (len) memcpy(pin, pin_code, len);
    lmp_hci_cmd_to_conn_for_addr(addr, 6, 0x040d, len, read32(pin), read32(pin + 4),
                               read32(pin + 8), read32(pin + 12));
#endif
}
LMP_HCI_CODE void lmp_hci_switch_role_command(u8 *addr, u8 role)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    if (!conn) { btctrler_assert_failed("role switch connection", role); return; }
    hci_send_event(0x0f, "112", 0, 1, 0x080b);
    if (conn->core.role != role) conn->core.role_switch = 1;
    else hci_send_event(0x12, "1A1", 0, conn->core_s.remote_addr, role);
}

#ifdef UNITTEST
#include <stdlib.h>
#include <stdarg.h>
#include "test.h"
static struct lmp_connection conn;
static u8 address[6];
static int calls, argc_seen, found = 1, asserts, events, codes[2], role_seen;
static u32 words[6];
int lmp_hci_cmd_to_conn_for_addr(u8 *addr, int argc, ...)
{
    CHECK(addr == address);
    va_list ap; va_start(ap, argc);
    if (argc == 0x042f) {
        va_end(ap); calls++; argc_seen = argc; return -1;
    }
    CHECK(argc > 0 && argc <= 6);
    words[0] = va_arg(ap, u32);
    if (words[0] == 0x042e && argc == 5) words[1] = va_arg(ap, u32);
    else for (int i = 1; i < argc; i++) words[i] = va_arg(ap, u32);
    va_end(ap); calls++; argc_seen = argc; return -1;
}
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(addr == address); return found ? &conn : 0; }
void btctrler_assert_failed(const char *what, int value)
{ CHECK(what && value >= 0 && value <= 255); asserts++; }
int hci_send_event(int code, const char *format, ...)
{
    CHECK(events < 2); codes[events++] = code;
    va_list ap; va_start(ap, format); CHECK(va_arg(ap, int) == 0);
    if (code == 0x0f) CHECK(!strcmp(format, "112") && va_arg(ap, int) == 1 && va_arg(ap, int) == 0x080b);
    else {
        CHECK(code == 0x12 && !strcmp(format, "1A1") && va_arg(ap, u8 *) == conn.core_s.remote_addr);
        role_seen = va_arg(ap, int);
    }
    va_end(ap); return 0;
}
static void expect(int argc, u32 opcode)
{ CHECK(argc_seen == argc && words[0] == opcode); }
int main(void)
{
    lmp_hci_io_capability_request_reply(address, 0xff, 0x80, 0x42);
    expect(4, 0x042b); CHECK(words[1] == 0xff && words[2] == 0x80 && words[3] == 0x42);
    lmp_hci_user_confirmation_request_reply(address); expect(1, 0x042c);
    lmp_hci_user_confirmation_request_negative_reply(address); expect(1, 0x042d);
    lmp_hci_pin_code_request_negative_reply(address); expect(1, 0x040e);
    lmp_hci_link_key_request_negative_reply(address); expect(1, 0x040c);
    for (int value = 0; value < 256; value++) {
        lmp_hci_accept_connection_request(address, value); expect(2, 0x0409); CHECK(words[1] == (u32)value);
        lmp_hci_reject_connection_request(address, value); expect(2, 0x040a); CHECK(words[1] == (u32)value);
        lmp_hci_send_keypress_notification(address, value); expect(2, 0x0c60); CHECK(words[1] == (u32)value);
        lmp_hci_user_keypress_request_negative_reply(address, value);
#ifdef CONFIG_CPU_BD19
        CHECK(argc_seen == 0x042f);
#else
        expect(1, 0x042f);
#endif
    }
    lmp_hci_user_keypress_request_reply(address, 0xfedcba98);
#ifdef CONFIG_CPU_BD19
    expect(5, 0x042e);
#else
    expect(2, 0x042e);
#endif
    CHECK(words[1] == 0xfedcba98);
    u8 storage[17];
    for (int i = 0; i < 17; i++) storage[i] = i * 13;
    lmp_hci_link_key_request_reply(address, storage + 1); expect(5, 0x040b);
    for (int i = 0; i < 4; i++) CHECK(words[i + 1] == read32(storage + 1 + i * 4));
#ifdef CONFIG_CPU_BD19
    lmp_hci_pin_code_request_reply(address, 16, storage + 1); expect(6, 0x040d);
    CHECK(words[1] == 16);
    for (int i = 0; i < 4; i++) CHECK(words[i + 2] == read32(storage + 1 + i * 4));
#else
    /* ASan observes an input allocation containing exactly len bytes. */
    for (unsigned len = 0; len <= 16; len++) {
        u8 *pin = len ? malloc(len) : 0;
        for (unsigned i = 0; i < len; i++) pin[i] = 0x81 + i;
        lmp_hci_pin_code_request_reply(address, len, pin); expect(6, 0x040d); CHECK(words[1] == len);
        for (unsigned i = 0; i < 16; i++) CHECK(((words[2 + i / 4] >> (8 * (i % 4))) & 255) == (i < len ? 0x81 + i : 0));
        free(pin);
    }
    int before = calls;
    for (int len = 17; len < 256; len++) lmp_hci_pin_code_request_reply(address, len, storage);
    lmp_hci_pin_code_request_reply(address, 1, 0);
    CHECK(calls == before && asserts == 240);
#endif
    for (int current = 0; current < 2; current++) for (int requested = 0; requested < 2; requested++) {
        conn.core.role = current; conn.core.role_switch = 0; events = 0;
        lmp_hci_switch_role_command(address, requested);
        CHECK(codes[0] == 0x0f && events == (current == requested ? 2 : 1));
        CHECK(conn.core.role_switch == (current != requested));
        if (current == requested) CHECK(codes[1] == 0x12 && role_seen == requested);
    }
    found = 0; events = 0; lmp_hci_switch_role_command(address, 1);
#ifdef CONFIG_CPU_BD19
    CHECK(!events && asserts == 1);
#else
    CHECK(!events && asserts == 241);
#endif
    return TEST_DONE("btctrler_lmp_hci_pairing");
}
#endif
