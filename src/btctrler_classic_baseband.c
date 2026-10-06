/* Recovered Classic controller/baseband dispatch. Radio procedures and
 * shared storage remain SDK-owned. SDK no-op/silent commands are explicit;
 * bounded input and response construction replace unsafe SDK buffer reads. */
#include <stdlib.h>
#include <string.h>
#include "btctrler_sdk_abi.h"

static unsigned short read16(const unsigned char *p)
{ return (unsigned short)(p[0] | ((unsigned short)p[1] << 8)); }
static unsigned int read24(const unsigned char *p)
{ return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16); }
static void complete(unsigned short opcode, int error)
{ BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", error); }
static void flag(unsigned char mask, unsigned char value)
{
    unsigned char *p = BTCTRLER_ABI_classic_flags();
    *p = (unsigned char)((*p & ~mask) | (value & mask));
}
#ifdef UNITTEST
static int allocation_fails;
#endif
static void *allocate(unsigned int size)
{
#ifdef UNITTEST
    if (allocation_fails) return 0;
#endif
    return malloc(size);
}
static int write_eir(const unsigned char *data)
{
    unsigned int length = 0;
    const unsigned char *fields = data + 1;
    while (length < 240 && fields[length]) {
        unsigned int n = fields[length] + 1u;
        if (n > 240 - length) return 0x12;
        length += n;
    }
    unsigned char **storage = BTCTRLER_ABI_classic_eir_buffer();
    if (!*storage) {
        *storage = allocate(240);
        if (!*storage) return 7; /* Memory Capacity Exceeded; no partial state. */
    }
    memset(*storage, 0, 240);
    memcpy(*storage, fields, length);
    *BTCTRLER_ABI_classic_eir_length() = (unsigned char)length;
    *BTCTRLER_ABI_classic_eir_packet_type() = length < 17 ? 3 : 10;
    return 0;
}
static void read_name(unsigned short opcode)
{
    unsigned char *out = allocate(248);
    if (!out) { complete(opcode, 7); return; }
    const unsigned char *name = BTCTRLER_ABI_classic_name();
    unsigned int length = 0;
    while (length < 32 && name[length]) length++;
    memset(out, 0, 248); memcpy(out, name, length);
    BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1C248", 0, out);
    free(out); /* SDK formatter copies synchronously, as in the original. */
}

int hci_lmp_controller_baseband_cmd_handler(unsigned short opcode, unsigned char *data, int len)
{
    int required;
    switch (opcode) {
    /* SDK consumes these without an event or side effect. */
    case 0x0c2e: case 0x0c2f: case 0x0c31: return 1;
    case 0x0c05: case 0x0c0a: case 0x0c11: case 0x0c1a: case 0x0c20:
    case 0x0c3a: case 0x0c43: case 0x0c45: case 0x0c47: case 0x0c56: required = 1; break;
    case 0x0c16: case 0x0c18: case 0x0c26: case 0x0c27:
    case 0x0c36: case 0x0c53: case 0x0c6d: required = 2; break;
    case 0x0c24: case 0x0c2d: required = 3; break;
    case 0x0c1c: case 0x0c1e: case 0x0c28: case 0x0c37: required = 4; break;
    case 0x0c0d: case 0x0c12: case 0x0c33: required = 7; break;
    case 0x0c3f: required = 10; break;
    case 0x0c52: required = 241; break;
    case 0x0c13: required = 248; break;
    case 0x0c09: case 0x0c14: case 0x0c15: case 0x0c1b: case 0x0c1d:
    case 0x0c1f: case 0x0c23: case 0x0c25: case 0x0c38: case 0x0c39:
    case 0x0c46: case 0x0c55: case 0x0c58: required = 0; break;
    default: return 0;
    }
    if (len < required || (required && !data)) { complete(opcode, 0x12); return 1; }
    if (opcode == 0x0c11 || opcode == 0x0c3a) {
        int bytes = opcode == 0x0c11 ? 22 : 3;
        if (len < 1 + bytes * data[0]) { complete(opcode, 0x12); return 1; }
    }
    switch (opcode) {
    case 0x0c1a: /* Write Scan Enable: acknowledge precedes SDK scan changes. */
        complete(opcode, 0); BTCTRLER_BIN_lmp_scan_enable(data[0]); return 1;
    case 0x0c24: *BTCTRLER_ABI_classic_class() = read24(data); break;
    case 0x0c23: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "13", 0, (int)*BTCTRLER_ABI_classic_class()); return 1;
    case 0x0c25:
        /* Preserve the SDK's byte-width voice reply, despite i16 storage. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, (int)*BTCTRLER_ABI_classic_voice()); return 1;
    case 0x0c26: *BTCTRLER_ABI_classic_voice() = read16(data); break;
    case 0x0c33:
        BTCTRLER_ABI_host_buffer_sizes_set(read16(data), data[2], read16(data + 3), read16(data + 5)); break;
    case 0x0c55: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, BTCTRLER_ABI_local_extended_features()[0] & 1); return 1;
    case 0x0c56: {
        unsigned char *p = BTCTRLER_ABI_local_extended_features();
        *p = (unsigned char)((*p & ~1u) | (data[0] ? 1u : 0u)); break;
    }
    case 0x0c18: *BTCTRLER_ABI_classic_page_timeout() = read16(data); break;
    case 0x0c52: complete(opcode, write_eir(data)); return 1;
    case 0x0c27: case 0x0c28: case 0x0c37:
        /* Original SDK stubs acknowledge only the handle and store nothing. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1H", 0, (int)read16(data)); return 1;
    case 0x0c2d: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1H1", 0, (int)read16(data), 8); return 1;
    case 0x0c1f: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, *BTCTRLER_ABI_classic_flags() & 1); return 1;
    case 0x0c20: flag(1, data[0]); break;
    case 0x0c09: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, (*BTCTRLER_ABI_classic_flags() >> 1) & 1); return 1;
    case 0x0c0a: flag(2, (unsigned char)(data[0] << 1)); break;
    case 0x0c0d: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "122", 0, 0, 0); return 1;
    case 0x0c12: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "12", 0, 0); return 1;
    case 0x0c13: BTCTRLER_BIN_lmp_hci_write_local_name(data); break;
    case 0x0c14: read_name(opcode); return 1;
    case 0x0c16: *BTCTRLER_ABI_classic_accept_timeout() = read16(data); break;
    case 0x0c15:
        /* SDK read handler read/stored nonexistent parameters. Read storage. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "12", 0, (int)*BTCTRLER_ABI_classic_accept_timeout()); return 1;
    case 0x0c1c:
        *BTCTRLER_ABI_classic_inquiry_interval() = read16(data);
        *BTCTRLER_ABI_classic_inquiry_window() = read16(data + 2); break;
    case 0x0c1b: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "122", 0, (int)*BTCTRLER_ABI_classic_inquiry_interval(), (int)*BTCTRLER_ABI_classic_inquiry_window()); return 1;
    case 0x0c1e:
        *BTCTRLER_ABI_classic_page_interval() = read16(data);
        *BTCTRLER_ABI_classic_page_window() = read16(data + 2); break;
    case 0x0c1d: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "122", 0, (int)*BTCTRLER_ABI_classic_page_interval(), (int)*BTCTRLER_ABI_classic_page_window()); return 1;
    case 0x0c38: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, (int)*BTCTRLER_ABI_classic_num_iac()); return 1;
    case 0x0c39:
        /* The SDK has one three-byte LAP. Pack its value, not its pointer. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "113", 0, (int)*BTCTRLER_ABI_classic_iac_count(), (int)read24(BTCTRLER_ABI_classic_iac())); return 1;
    case 0x0c43: flag(4, (unsigned char)(data[0] << 2)); break;
    case 0x0c45: flag(48, (unsigned char)(data[0] << 4)); break;
    case 0x0c46:
        /* SDK passed this value to a status-only format, dropping the bit. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, (*BTCTRLER_ABI_classic_flags() >> 3) & 1); return 1;
    case 0x0c47: flag(8, (unsigned char)(data[0] << 3)); break;
    case 0x0c58: BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "11", 0, 0); return 1;
    case 0x0c36:
        /* SDK hard-coded 32000 but passed a double to its integer formatter. */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1H2", 0, (int)read16(data), 32000); return 1;
    case 0x0c53:
        BTCTRLER_BIN_lmp_hci_refresh_encryption_key(read16(data) & 0x0fff);
        BTCTRLER_BIN_hci_send_event(0x0f, "112", 0, 1, (int)opcode); return 1;
    /* Acknowledge-only SDK commands: event filter, stored keys, IAC list,
     * AFH classification and LE host support. No capability is added. */
    case 0x0c05: case 0x0c11: case 0x0c3a: case 0x0c3f: case 0x0c6d: break;
    }
    complete(opcode, 0); return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
static unsigned char flags, name[32], iac[3], num_iac, iac_count, eir_length, eir_type;
static unsigned char extended[16], *eir_buffer;
static unsigned short page_timeout, accept_timeout, voice, inquiry_interval, inquiry_window, page_interval, page_window;
static unsigned int device_class;
#define ACCESS(name, type, value) type *BTCTRLER_ABI_classic_##name(void) { return &value; }
ACCESS(flags, unsigned char, flags)
ACCESS(page_timeout, unsigned short, page_timeout)
ACCESS(accept_timeout, unsigned short, accept_timeout)
ACCESS(voice, unsigned short, voice)
ACCESS(inquiry_interval, unsigned short, inquiry_interval)
ACCESS(inquiry_window, unsigned short, inquiry_window)
ACCESS(page_interval, unsigned short, page_interval)
ACCESS(page_window, unsigned short, page_window)
ACCESS(class, unsigned int, device_class)
ACCESS(num_iac, unsigned char, num_iac)
ACCESS(iac_count, unsigned char, iac_count)
ACCESS(eir_length, unsigned char, eir_length)
ACCESS(eir_packet_type, unsigned char, eir_type)
ACCESS(eir_buffer, unsigned char *, eir_buffer)
unsigned char *BTCTRLER_ABI_classic_name(void) { return name; }
unsigned char *BTCTRLER_ABI_classic_iac(void) { return iac; }
unsigned char *BTCTRLER_ABI_local_extended_features(void) { return extended; }
static int sequence, event_at, helper_at, error, seen_opcode, event_kind, values[4];
static unsigned short buffers[4], refreshed;
static unsigned char scan, response[248], written_name[248];
static const char *format_seen;
void BTCTRLER_ABI_host_buffer_sizes_set(unsigned short a, unsigned short b, unsigned short c, unsigned short d)
{ buffers[0] = a; buffers[1] = b; buffers[2] = c; buffers[3] = d; }
void BTCTRLER_BIN_lmp_scan_enable(unsigned char enable) { scan = enable; helper_at = ++sequence; }
void BTCTRLER_BIN_lmp_hci_write_local_name(unsigned char *n)
{ memcpy(written_name, n, 248); helper_at = ++sequence; }
int BTCTRLER_BIN_lmp_hci_refresh_encryption_key(unsigned short h)
{ refreshed = h; helper_at = ++sequence; return 99; }
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); error = va_arg(ap, int);
    event_at = ++sequence; seen_opcode = opcode; event_kind = 0x0e; format_seen = format;
    memset(values, 0, sizeof(values));
    if (!strcmp(format, "1C248")) memcpy(response, va_arg(ap, unsigned char *), 248);
    else for (unsigned int i = 1; format[i]; i++) values[i-1] = va_arg(ap, int);
    va_end(ap); return 0;
}
int BTCTRLER_BIN_hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(event == 0x0f && !strcmp(format, "112"));
    error = va_arg(ap, int); CHECK(va_arg(ap, int) == 1); seen_opcode = va_arg(ap, int);
    event_kind = event; event_at = ++sequence; va_end(ap); return 0;
}
static void run(unsigned short opcode, unsigned char *data, int len)
{
    sequence = event_at = helper_at = error = 0;
    CHECK(hci_lmp_controller_baseband_cmd_handler(opcode, data, len) == 1);
}
int main(void)
{
    const struct { unsigned short opcode; int length; } commands[] = {
        {0x0c05,1},{0x0c09,0},{0x0c0a,1},{0x0c0d,7},{0x0c11,1},{0x0c12,7},
        {0x0c13,248},{0x0c14,0},{0x0c15,0},{0x0c16,2},{0x0c18,2},{0x0c1a,1},
        {0x0c1b,0},{0x0c1c,4},{0x0c1d,0},{0x0c1e,4},{0x0c1f,0},{0x0c20,1},
        {0x0c23,0},{0x0c24,3},{0x0c25,0},{0x0c26,2},{0x0c27,2},{0x0c28,4},
        {0x0c2d,3},{0x0c33,7},{0x0c36,2},{0x0c37,4},{0x0c38,0},{0x0c39,0},
        {0x0c3a,1},{0x0c3f,10},{0x0c43,1},{0x0c45,1},{0x0c46,0},{0x0c47,1},
        {0x0c52,241},{0x0c53,2},{0x0c55,0},{0x0c56,1},{0x0c58,0},{0x0c6d,2}
    };
    unsigned char data[248] = {0};
    for (unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        for (int n = -1; n < commands[i].length; n++) {
            run(commands[i].opcode, 0, n); CHECK(error == 0x12 && event_at && !helper_at);
        }
        run(commands[i].opcode, data, commands[i].length); CHECK(!error && event_at);
    }
    const unsigned short silent[] = {0x0c2e,0x0c2f,0x0c31};
    for (unsigned int i = 0; i < 3; i++) {
        run(silent[i], 0, -1); CHECK(!event_at && !helper_at);
    }
    CHECK(!hci_lmp_controller_baseband_cmd_handler(0x0c17, 0, 0));
    data[0] = 0xff; run(0x0c1a, data, 1); CHECK(scan == 255 && event_at < helper_at);
    run(0x0c53, data, 2); CHECK(refreshed == 255 && helper_at < event_at && event_kind == 0x0f);
    data[1] = 0xf2; run(0x0c53, data, 2); CHECK(refreshed == 0x02ff);
    const unsigned short flag_opcodes[] = {0x0c20,0x0c0a,0x0c43,0x0c45,0x0c47};
    const unsigned char masks[] = {1,2,4,48,8}, shifts[] = {0,1,2,4,3};
    for (unsigned int i = 0; i < 5; i++) for (int v = 0; v < 256; v++) {
        flags = 0xa5; data[0] = (unsigned char)v; run(flag_opcodes[i], data, 1);
        CHECK(flags == (unsigned char)((0xa5 & ~masks[i]) | ((v << shifts[i]) & masks[i])));
    }
    extended[0] = 0xa4; data[0] = 2; run(0x0c56, data, 1); CHECK(extended[0] == 0xa5);
    run(0x0c55, 0, 0); CHECK(values[0] == 1);
    data[0] = 0; run(0x0c56, data, 1); CHECK(extended[0] == 0xa4);
    data[0] = 0x34; data[1] = 0x12; data[2] = 0x56; data[3] = 0x78; data[4] = 0x9a; data[5] = 0xbc; data[6] = 0xde;
    run(0x0c33, data, 7); CHECK(buffers[0] == 0x1234 && buffers[1] == 0x56 && buffers[2] == 0x9a78 && buffers[3] == 0xdebc);
    run(0x0c24, data, 3); run(0x0c23, 0, 0); CHECK(values[0] == 0x561234 && !strcmp(format_seen, "13"));
    run(0x0c16, data, 2); run(0x0c15, 0, 0); CHECK(values[0] == 0x1234 && accept_timeout == 0x1234);
    run(0x0c18, data, 2); CHECK(page_timeout == 0x1234);
    run(0x0c26, data, 2); CHECK(voice == 0x1234);
    run(0x0c25, 0, 0); CHECK(values[0] == 0x1234 && !strcmp(format_seen, "11"));
    run(0x0c1c, data, 4); run(0x0c1b, 0, 0); CHECK(values[0] == 0x1234 && values[1] == 0x7856);
    run(0x0c1e, data, 4); run(0x0c1d, 0, 0); CHECK(values[0] == 0x1234 && values[1] == 0x7856);
    run(0x0c36, data, 2); CHECK(values[0] == 0x1234 && values[1] == 32000 && !strcmp(format_seen, "1H2"));
    flags = 8; run(0x0c46, 0, 0); CHECK(values[0] == 1 && !strcmp(format_seen, "11"));
    iac[0] = 0x33; iac[1] = 0x8b; iac[2] = 0x9e; iac_count = 1;
    run(0x0c39, 0, 0); CHECK(values[0] == 1 && values[1] == 0x9e8b33);
    memset(name, 'n', 32); run(0x0c14, 0, 0);
    CHECK(!memcmp(name, response, 32)); for (int i = 32; i < 248; i++) CHECK(response[i] == 0);
    run(0x0c13, data, 248); CHECK(!memcmp(data, written_name, 248) && helper_at < event_at);
    data[0] = 2; run(0x0c11, data, 44); CHECK(error == 0x12);
    run(0x0c11, data, 45); CHECK(!error);
    run(0x0c3a, data, 6); CHECK(error == 0x12);
    run(0x0c3a, data, 7); CHECK(!error);
    /* EIR is a bounded TLV sequence; errors preserve the prior SDK state. */
    memset(data, 0, sizeof(data)); data[1] = 3; data[2] = 9; data[3] = 'a'; data[4] = 'b';
    run(0x0c52, data, 241); CHECK(eir_length == 4 && eir_type == 3 && !memcmp(eir_buffer, data + 1, 4));
    for (int i = 4; i < 240; i++) CHECK(eir_buffer[i] == 0);
    unsigned char *original = eir_buffer;
    data[1] = 239; run(0x0c52, data, 241); CHECK(!error && eir_length == 240 && eir_type == 10 && eir_buffer == original);
    data[1] = 240; run(0x0c52, data, 241); CHECK(error == 0x12 && eir_length == 240);
    memset(data, 0, sizeof(data)); data[1] = 238; data[240] = 1;
    run(0x0c52, data, 241); CHECK(error == 0x12 && eir_length == 240);
    memset(data, 0, sizeof(data)); run(0x0c52, data, 241); CHECK(!error && eir_length == 0 && eir_type == 3);
    free(eir_buffer); eir_buffer = 0; allocation_fails = 1;
    eir_length = 41; run(0x0c52, data, 241); CHECK(error == 7 && !eir_buffer && eir_length == 41);
    run(0x0c14, 0, 0); CHECK(error == 7); allocation_fails = 0;
    return TEST_DONE("btctrler_classic_baseband");
}
#endif
