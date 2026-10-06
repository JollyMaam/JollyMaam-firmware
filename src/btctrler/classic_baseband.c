/* Owned Classic controller/baseband commands (lmp_hci_cmd_parser.c).
 * Radio procedures stay the SDK's.  Commands the SDK only acknowledges are
 * listed explicitly; replies the SDK built wrongly are corrected (noted). */
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

#define DEV gbredr_local_dev

static u16 read16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 read24(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16; }
static void complete(u16 opcode, int error) { hci_send_event_cmd_complete(opcode, "1", error); }

#ifdef UNITTEST
static int allocation_fails;
#endif
static void *allocate(u32 size)
{
#ifdef UNITTEST
    if (allocation_fails) return 0;
#endif
    return malloc(size);
}

/* Write Extended Inquiry Response: FEC byte, then 240 bytes of fields. */
static int write_eir(const u8 *data)
{
    u32 length = 0;
    const u8 *fields = data + 1;
    while (length < 240 && fields[length]) {
        u32 n = fields[length] + 1u;
        if (n > 240 - length) return 0x12;
        length += n;
    }
    /* The SDK keeps the buffer (and frees it) once allocated. */
    if (!DEV.eir.data) {
        DEV.eir.data = allocate(240);
        if (!DEV.eir.data) return 7;    /* Memory Capacity Exceeded */
    }
    memset(DEV.eir.data, 0, 240);
    memcpy(DEV.eir.data, fields, length);
    DEV.eir.len = (u8)length;
    DEV.eir.type = length < 17 ? 3 : 10;    /* DM1 or DM3 */
    return 0;
}

static void read_name(u16 opcode)
{
    /* Heap, not stack: the controller task's stack is small.  The event
     * formatter copies the name before it returns. */
    u8 *out = allocate(248);
    if (!out) { complete(opcode, 7); return; }
    u32 length = 0;
    while (length < 32 && DEV.local_name[length]) length++;
    memset(out, 0, 248); memcpy(out, DEV.local_name, length);
    hci_send_event_cmd_complete(opcode, "1C248", 0, out);
    free(out);
}

int hci_lmp_controller_baseband_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    u32 required;
    switch (opcode) {
    /* Consumed without an event or side effect. */
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
    if (opcode == 0x0c11 || opcode == 0x0c3a) {    /* counted lists */
        u32 bytes = opcode == 0x0c11 ? 22 : 3;
        if (len < 1 + bytes * data[0]) { complete(opcode, 0x12); return 1; }
    }
    switch (opcode) {
    case 0x0c1a: /* Write Scan Enable: the event goes before the scan starts. */
        complete(opcode, 0); lmp_scan_enable(data[0]); return 1;
    case 0x0c24: DEV.dev_class = (int)read24(data); break;
    case 0x0c23: hci_send_event_cmd_complete(opcode, "13", 0, DEV.dev_class); return 1;
    case 0x0c25: /* The SDK replies one byte of the two. */
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.voice_setting); return 1;
    case 0x0c26: DEV.voice_setting = read16(data); break;
    case 0x0c33: {
        struct host_buffer_size_info *b = &hci_param.host_buf_info;
        b->host_acl_data_packer_length = read16(data);
        b->host_sco_data_packer_length = data[2];
        b->host_total_num_acl_data_packets = read16(data + 3);
        b->host_total_num_sco_data_packets = read16(data + 5);
        break;
    }
    /* Simple Pairing Mode: host support bit, extended features page 1. */
    case 0x0c55: hci_send_event_cmd_complete(opcode, "11", 0, hci_param.ext_features[0] & 1); return 1;
    case 0x0c56: hci_param.ext_features[0] = (u8)((hci_param.ext_features[0] & ~1u) | (data[0] != 0)); break;
    case 0x0c18: DEV.page_timeout_value = read16(data); break;
    case 0x0c52: complete(opcode, write_eir(data)); return 1;
    case 0x0c27: case 0x0c28: case 0x0c37:
        /* Flush and supervision timeouts: the SDK stores nothing and
         * answers with the handle only. */
        hci_send_event_cmd_complete(opcode, "1H", 0, read16(data)); return 1;
    case 0x0c2d: hci_send_event_cmd_complete(opcode, "1H1", 0, read16(data), 8); return 1;
    case 0x0c1f: hci_send_event_cmd_complete(opcode, "11", 0, DEV.authentication_enable); return 1;
    case 0x0c20: DEV.authentication_enable = data[0] & 1; break;
    case 0x0c09: hci_send_event_cmd_complete(opcode, "11", 0, DEV.pin_code_type); return 1;
    case 0x0c0a: DEV.pin_code_type = data[0] & 1; break;
    case 0x0c0d: hci_send_event_cmd_complete(opcode, "122", 0, 0, 0); return 1;
    case 0x0c12: hci_send_event_cmd_complete(opcode, "12", 0, 0); return 1;
    case 0x0c13: lmp_hci_write_local_name((const char *)data); break;
    case 0x0c14: read_name(opcode); return 1;
    case 0x0c16: DEV.connection_accept_timeout_value = read16(data); break;
    case 0x0c15: /* Corrected: the SDK replied with parameters it was not given. */
        hci_send_event_cmd_complete(opcode, "12", 0, DEV.connection_accept_timeout_value); return 1;
    case 0x0c1c: /* Write Page Scan Activity */
        DEV.page_scan_interval = read16(data);
        DEV.page_scan_window = read16(data + 2); break;
    case 0x0c1b: hci_send_event_cmd_complete(opcode, "122", 0, DEV.page_scan_interval, DEV.page_scan_window); return 1;
    case 0x0c1e: /* Write Inquiry Scan Activity */
        DEV.inquiry_scan_interval = read16(data);
        DEV.inquiry_scan_window = read16(data + 2); break;
    case 0x0c1d: hci_send_event_cmd_complete(opcode, "122", 0, DEV.inquiry_scan_interval, DEV.inquiry_scan_window); return 1;
    case 0x0c38: hci_send_event_cmd_complete(opcode, "11", 0, DEV.supported_iac); return 1;
    case 0x0c39: /* Corrected: the SDK sent the LAP's address, not its value. */
        hci_send_event_cmd_complete(opcode, "113", 0, DEV.num_current_iac, read24(DEV.iac_lap)); return 1;
    case 0x0c43: DEV.inquiry_scan_type = data[0] & 1; break;
    case 0x0c45: DEV.inquiry_mode = data[0] & 3; break;
    case 0x0c46: /* Corrected: the SDK's format dropped the value. */
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.page_scan_type); return 1;
    case 0x0c47: DEV.page_scan_type = data[0] & 1; break;
    case 0x0c58: hci_send_event_cmd_complete(opcode, "11", 0, 0); return 1;
    case 0x0c36: /* Corrected: the SDK passed 32000 as a double. */
        hci_send_event_cmd_complete(opcode, "1H2", 0, read16(data), 32000); return 1;
    case 0x0c53: /* Refresh Encryption Key; the SDK ignores the result. */
        lmp_hci_refresh_encryption_key(read16(data) & 0x0fff);
        hci_send_event(0x0f, "112", 0, 1, opcode); return 1;
    /* Acknowledged only: event filter, stored keys, IAC list, AFH host
     * classification, LE host support. */
    case 0x0c05: case 0x0c11: case 0x0c3a: case 0x0c3f: case 0x0c6d: break;
    }
    complete(opcode, 0); return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev;
struct hci_parameter hci_param;
static u8 *const extended = hci_param.ext_features;
static int sequence, event_at, helper_at, error, seen_opcode, event_kind, values[4];
static u16 refreshed;
static unsigned char scan, response[248], written_name[248];
static const char *format_seen;
void lmp_scan_enable(unsigned char enable) { scan = enable; helper_at = ++sequence; }
void lmp_hci_write_local_name(const char *n)
{ memcpy(written_name, n, 248); helper_at = ++sequence; }
int lmp_hci_refresh_encryption_key(unsigned short h)
{ refreshed = h; helper_at = ++sequence; return 99; }
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); error = va_arg(ap, int);
    event_at = ++sequence; seen_opcode = opcode; event_kind = 0x0e; format_seen = format;
    memset(values, 0, sizeof(values));
    if (!strcmp(format, "1C248")) memcpy(response, va_arg(ap, unsigned char *), 248);
    else for (unsigned int i = 1; format[i]; i++) values[i-1] = va_arg(ap, int);
    va_end(ap); return 0;
}
int hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(event == 0x0f && !strcmp(format, "112"));
    error = va_arg(ap, int); CHECK(va_arg(ap, int) == 1); seen_opcode = va_arg(ap, int);
    event_kind = event; event_at = ++sequence; va_end(ap); return 0;
}
static void run(u16 opcode, u8 *data, u32 len)
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
        for (int n = 0; n < commands[i].length; n++) {
            run(commands[i].opcode, 0, n); CHECK(error == 0x12 && event_at && !helper_at);
        }
        run(commands[i].opcode, data, commands[i].length); CHECK(!error && event_at);
    }
    const unsigned short silent[] = {0x0c2e,0x0c2f,0x0c31};
    for (unsigned int i = 0; i < 3; i++) {
        run(silent[i], 0, 0); CHECK(!event_at && !helper_at);
    }
    CHECK(!hci_lmp_controller_baseband_cmd_handler(0x0c17, 0, 0));
    data[0] = 0xff; run(0x0c1a, data, 1); CHECK(scan == 255 && event_at < helper_at);
    run(0x0c53, data, 2); CHECK(refreshed == 255 && helper_at < event_at && event_kind == 0x0f);
    data[1] = 0xf2; run(0x0c53, data, 2); CHECK(refreshed == 0x02ff);
    /* Each flag write sets its own bit field and no other. */
    const u16 flag_opcodes[] = {0x0c20,0x0c0a,0x0c43,0x0c45,0x0c47};
    const u8 masks[] = {1,2,4,48,8}, shifts[] = {0,1,2,4,3};
    for (unsigned int i = 0; i < 5; i++) for (int v = 0; v < 256; v++) {
        memset(&DEV, 0xa5, 1); data[0] = (u8)v; run(flag_opcodes[i], data, 1);
        CHECK(*(u8 *)&DEV == (u8)((0xa5 & ~masks[i]) | ((v << shifts[i]) & masks[i])));
    }
    extended[0] = 0xa4; data[0] = 2; run(0x0c56, data, 1); CHECK(extended[0] == 0xa5);
    run(0x0c55, 0, 0); CHECK(values[0] == 1);
    data[0] = 0; run(0x0c56, data, 1); CHECK(extended[0] == 0xa4);
    data[0] = 0x34; data[1] = 0x12; data[2] = 0x56; data[3] = 0x78; data[4] = 0x9a; data[5] = 0xbc; data[6] = 0xde;
    run(0x0c33, data, 7); struct host_buffer_size_info *b = &hci_param.host_buf_info;
    CHECK(b->host_acl_data_packer_length == 0x1234 && b->host_sco_data_packer_length == 0x56);
    CHECK(b->host_total_num_acl_data_packets == 0x9a78 && b->host_total_num_sco_data_packets == 0xdebc);
    run(0x0c24, data, 3); run(0x0c23, 0, 0); CHECK(values[0] == 0x561234 && !strcmp(format_seen, "13"));
    run(0x0c16, data, 2); run(0x0c15, 0, 0); CHECK(values[0] == 0x1234 && DEV.connection_accept_timeout_value == 0x1234);
    run(0x0c18, data, 2); CHECK(DEV.page_timeout_value == 0x1234);
    run(0x0c26, data, 2); CHECK(DEV.voice_setting == 0x1234);
    run(0x0c25, 0, 0); CHECK(values[0] == 0x1234 && !strcmp(format_seen, "11"));
    run(0x0c1c, data, 4); run(0x0c1b, 0, 0); CHECK(values[0] == 0x1234 && values[1] == 0x7856);
    run(0x0c1e, data, 4); run(0x0c1d, 0, 0); CHECK(values[0] == 0x1234 && values[1] == 0x7856);
    run(0x0c36, data, 2); CHECK(values[0] == 0x1234 && values[1] == 32000 && !strcmp(format_seen, "1H2"));
    DEV.page_scan_type = 1; run(0x0c46, 0, 0); CHECK(values[0] == 1 && !strcmp(format_seen, "11"));
    DEV.iac_lap[0] = 0x33; DEV.iac_lap[1] = 0x8b; DEV.iac_lap[2] = 0x9e; DEV.num_current_iac = 1;
    run(0x0c39, 0, 0); CHECK(values[0] == 1 && values[1] == 0x9e8b33);
    memset(DEV.local_name, 'n', 32); run(0x0c14, 0, 0);
    CHECK(!memcmp(DEV.local_name, response, 32)); for (int i = 32; i < 248; i++) CHECK(response[i] == 0);
    run(0x0c13, data, 248); CHECK(!memcmp(data, written_name, 248) && helper_at < event_at);
    data[0] = 2; run(0x0c11, data, 44); CHECK(error == 0x12);
    run(0x0c11, data, 45); CHECK(!error);
    run(0x0c3a, data, 6); CHECK(error == 0x12);
    run(0x0c3a, data, 7); CHECK(!error);
    /* EIR is a bounded TLV sequence; errors preserve the prior SDK state. */
    memset(data, 0, sizeof(data)); data[1] = 3; data[2] = 9; data[3] = 'a'; data[4] = 'b';
    run(0x0c52, data, 241); CHECK(DEV.eir.len == 4 && DEV.eir.type == 3 && !memcmp(DEV.eir.data, data + 1, 4));
    for (int i = 4; i < 240; i++) CHECK(DEV.eir.data[i] == 0);
    u8 *original = DEV.eir.data;
    data[1] = 239; run(0x0c52, data, 241); CHECK(!error && DEV.eir.len == 240 && DEV.eir.type == 10 && DEV.eir.data == original);
    data[1] = 240; run(0x0c52, data, 241); CHECK(error == 0x12 && DEV.eir.len == 240);
    memset(data, 0, sizeof(data)); data[1] = 238; data[240] = 1;
    run(0x0c52, data, 241); CHECK(error == 0x12 && DEV.eir.len == 240);
    memset(data, 0, sizeof(data)); run(0x0c52, data, 241); CHECK(!error && DEV.eir.len == 0 && DEV.eir.type == 3);
    free(DEV.eir.data); DEV.eir.data = 0; allocation_fails = 1;
    DEV.eir.len = 41; run(0x0c52, data, 241); CHECK(error == 7 && !DEV.eir.data && DEV.eir.len == 41);
    run(0x0c14, 0, 0); CHECK(error == 7); allocation_fails = 0;
    return TEST_DONE("btctrler_classic_baseband");
}
#endif
