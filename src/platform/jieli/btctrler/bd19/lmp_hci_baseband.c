/* BD19 lmp_hci_cmd_parser.c.o: original controller/baseband dispatch.
 * BR23's recovery-era bounds/reply corrections remain in classic_baseband.c. */
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_LMP;
void log_print(int, const char *, const char *, ...);
void printf_buf(u8 *, int);
#define LMP_CODE __attribute__((section(".classic_lmp_code")))
#define DEV gbredr_local_dev
#define INFO(...) log_print(2, NULL, __VA_ARGS__)
#define LOG(...) do { if (log_tag_const_i_HCI_LMP) INFO(__VA_ARGS__); } while (0)
typedef u32 unaligned_word __attribute__((aligned(1), may_alias));
#ifdef UNITTEST
static void *baseband_malloc(size_t);
static void baseband_free(void *);
#define malloc baseband_malloc
#define free baseband_free
#endif

LMP_CODE int hci_lmp_controller_baseband_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    switch (opcode) {
    case 0x0c1a:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_SCAN_ENABLE cmd = 0x%x\r\n", data[0]);
        hci_send_event_cmd_complete(opcode, "1", 0);
        lmp_scan_enable(data[0]);
        break;
    case 0x0c24:
        if (log_tag_const_i_HCI_LMP)
            INFO("[Info]: [HCI_LMP]HCI_WRITE_CLASS_OF_DEVICE = 0x%x\r\n", btctler_little_endian_read_24(data, 0));
        DEV.dev_class = btctler_little_endian_read_24(data, 0);
        hci_send_event_cmd_complete(opcode, "1", 0);
        break;
    case 0x0c25:
        LOG("[Info]: [HCI_LMP]HCI_READ_VOICE_SETTING\r\n");
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.voice_setting);
        break;
    case 0x0c26: {
        u16 setting = btctler_little_endian_read_16(data, 0);
        DEV.voice_setting = setting;
        u8 logging = log_tag_const_i_HCI_LMP;
        if (logging) {
            INFO("[Info]: [HCI_LMP]HCI_WRITE_VOICE_SETTING = 0x%x\r\n", setting);
            switch ((DEV.voice_setting >> 8) & 3) {
            case 0: INFO("[Info]: [HCI_LMP]Input Coding: Linear\r\n"); break;
            case 1: INFO("[Info]: [HCI_LMP]Input Coding: \xce\xbc-law Input Coding\r\n"); break;
            case 2: INFO("[Info]: [HCI_LMP]Input Coding: A-law Input Coding\r\n"); break;
            case 3: INFO("[Info]: [HCI_LMP]Reserved for future use\r\n"); break;
            }
            switch ((DEV.voice_setting >> 6) & 3) {
            case 0: INFO("[Info]: [HCI_LMP]Input Data Format: 1\xe2\x80\x99s complement\r\n"); break;
            case 1: INFO("[Info]: [HCI_LMP]Input Data Format: 2\xe2\x80\x99s complement\r\n"); break;
            case 2: INFO("[Info]: [HCI_LMP]Input Data Format: Sign-Magnitude\r\n"); break;
            case 3: INFO("[Info]: [HCI_LMP]Input Data Format: Unsigned\r\n"); break;
            }
            if (DEV.voice_setting & 32)
                INFO("[Info]: [HCI_LMP]Input Sample Size: 16-bit (only for linear PCM)\r\n");
            else
                INFO("[Info]: [HCI_LMP]Input Sample Size: 8-bit (only for linear PCM)\r\n");
            switch (DEV.voice_setting & 3) {
            case 0: INFO("[Info]: [HCI_LMP]Air Coding Format: CVSD\r\n"); break;
            case 1: INFO("[Info]: [HCI_LMP]Air Coding Format: \xce\xbc-law\r\n"); break;
            case 2: INFO("[Info]: [HCI_LMP]Air Coding Format: A-law\r\n"); break;
            case 3: INFO("[Info]: [HCI_LMP]Air Coding Format: Transparent Data\r\n"); break;
            }
        }
        hci_send_event_cmd_complete(opcode, "1", 0);
        break;
    }
    case 0x0c2e:
        LOG("[Info]: [HCI_LMP]HCI_READ_SYNCHRONOUS_FLOW_CONTROL_ENABLE\r\n"); break;
    case 0x0c2f:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_SYNCHRONOUS_FLOW_CONTROL_ENABLE status : %d\r\n", data[0]); break;
    case 0x0c31:
        LOG("[Info]: [HCI_LMP]HCI_SET_CONTROLLER_TO_HOST_FLOW_CONTROL status : %d\r\n", data[0]); break;
    case 0x0c33:
        if (log_tag_const_i_HCI_LMP) {
            u16 acl = btctler_little_endian_read_16(data, 0);
            u8 sco = data[2];
            u16 acl_count = btctler_little_endian_read_16(data, 3);
            u16 sco_count = btctler_little_endian_read_16(data, 5);
            INFO("[Info]: [HCI_LMP]HCI_HOST_BUFFER_SIZE : %x / %x / %x / %x\r\n", acl, sco, acl_count, sco_count);
        }
        hci_param.host_buf_info.host_acl_data_packer_length = btctler_little_endian_read_16(data, 0);
        hci_param.host_buf_info.host_sco_data_packer_length = data[2];
        hci_param.host_buf_info.host_total_num_acl_data_packets = btctler_little_endian_read_16(data, 3);
        hci_param.host_buf_info.host_total_num_sco_data_packets = btctler_little_endian_read_16(data, 5);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c55: {
        int value = hci_param.ext_features[0] & 1;
        LOG("[Info]: [HCI_LMP]HCI_READ_SIMPLE_PAIRING_MODE state = %d\r\n", value);
        hci_send_event_cmd_complete(opcode, "11", 0, value); break;
    }
    case 0x0c56:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_SIMPLE_PAIRING_MODE state = %d\r\n", data[0]);
        hci_param.ext_features[0] = (hci_param.ext_features[0] & ~1u) | (data[0] != 0);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c6d:
        LOG("[Info]: [HCI_LMP]WRITE_LE_HOST_SUPPORT\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c18:
        if (log_tag_const_i_HCI_LMP)
            INFO("[Info]: [HCI_LMP]HCI_WRITE_PAGE_TIMEOUT Timeout = %d\r\n", btctler_little_endian_read_16(data, 0));
        DEV.page_timeout_value = btctler_little_endian_read_16(data, 0);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c52: {
        u8 logging = log_tag_const_i_HCI_LMP;
        if (logging) { INFO("[Info]: [HCI_LMP]HCI_WRITE_EXTENDED_INQUIRY_RESPONSE\r\n"); printf_buf((u8 *)data, len); }
        const u8 *fields = data + 1;
        u8 pos = 0, total = 0;
        while (fields[pos]) {
            u32 step = (u32)fields[pos] + 1;
            total = total + step;
            pos = pos + step;
        }
        if (logging) INFO("[Info]: [HCI_LMP]eir_data_analysis, total len : %d\r\n", total);
        if (total > 240) btctrler_assert_failed("ret <= MAX_EIR_LEN", total);
        DEV.eir.len = total;
        DEV.eir.type = total < 17 ? 3 : 10;
        if (!DEV.eir.data) {
            if (logging) INFO("[Info]: [HCI_LMP]malloc eir data space\r\n");
            DEV.eir.data = malloc(240);
            if (!DEV.eir.data) btctrler_assert_failed("gbredr_local_dev.eir.data", 0);
        }
        *(unaligned_word *)DEV.eir.data = 0;
        __builtin_memcpy(DEV.eir.data, fields, DEV.eir.len);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    }
    case 0x0c27:
        LOG("[Info]: [HCI_LMP]HCI_READ_AUTOMATIC_FLUSH_TIMEOUT_debug\r\n");
        hci_send_event_cmd_complete(opcode, "1H", 0, btctler_little_endian_read_16(data, 0), 0); break;
    case 0x0c28:
        if (log_tag_const_i_HCI_LMP)
            INFO("[Info]: [HCI_LMP]HCI_WRITE_AUTOMATIC_FLUSH_TIMEOUT : Fluash Timeout = 0x%x\r\n", btctler_little_endian_read_16(data, 2));
        hci_send_event_cmd_complete(opcode, "1H", 0, btctler_little_endian_read_16(data, 0)); break;
    case 0x0c2d:
        LOG("[Info]: [HCI_LMP]HCI_READ_TRANSMIT_POWER_LEVEL_debug\r\n");
        hci_send_event_cmd_complete(opcode, "1H1", 0, btctler_little_endian_read_16(data, 0), 8); break;
    case 0x0c1f:
        LOG("[Info]: [HCI_LMP]HCI_READ_AUTHENTICATION_ENABLE state = %d\r\n", DEV.authentication_enable);
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.authentication_enable); break;
    case 0x0c20:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_AUTHENTICATION_ENABLE status : %d\r\n", data[0]);
        DEV.authentication_enable = data[0] & 1;
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c09:
        LOG("[Info]: [HCI_LMP]HCI_READ_PIN_TYPE state = %d\r\n", DEV.pin_code_type);
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.pin_code_type); break;
    case 0x0c0a:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_PIN_TYPE status : %d\r\n", data[0]);
        DEV.pin_code_type = data[0] & 1;
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c05:
        LOG("[Info]: [HCI_LMP]HCI_SET_EVENT_FILTER : %d - %d - %d\r\n", data[0], data[1], data[2]);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c0d:
        LOG("[Info]: [HCI_LMP]HCI_READ_STORED_LINK_KEY\r\n");
        hci_send_event_cmd_complete(opcode, "122", 0, 0, 0); break;
    case 0x0c11:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_STORED_LINK_KEY\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c12:
        LOG("[Info]: [HCI_LMP]HCI_DELETE_STORED_LINK_KEY\r\n");
        hci_send_event_cmd_complete(opcode, "12", 0, 0); break;
    case 0x0c13:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_LOCAL_NAME : %s\r\n", data);
        lmp_hci_write_local_name((const char *)data);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c14: {
        LOG("[Info]: [HCI_LMP]HCI_READ_LOCAL_NAME : %s\r\n", DEV.local_name);
        u8 *name = malloc(248);
        if (!name) btctrler_assert_failed("name", 0);
        __builtin_memset(name, 0, 248);
        __builtin_memcpy(name, DEV.local_name, strlen(DEV.local_name));
        hci_send_event_cmd_complete(opcode, "1C248", 0, name);
        free(name); break;
    }
    case 0x0c16:
        if (log_tag_const_i_HCI_LMP)
            INFO("[Info]: [HCI_LMP]HCI_WRITE_CONNECTION_ACCEPT_TIMEOUT : %d\r\n", btctler_little_endian_read_16(data, 0));
        DEV.connection_accept_timeout_value = btctler_little_endian_read_16(data, 0);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c15: {
        LOG("[Info]: [HCI_LMP]HCI_READ_CONNECTION_ACCEPT_TIMEOUT : %d\r\n", data[0]);
        u16 timeout = btctler_little_endian_read_16(data, 0);
        DEV.connection_accept_timeout_value = timeout;
        hci_send_event_cmd_complete(opcode, "12", 0, timeout); break;
    }
    case 0x0c1c:
        if (log_tag_const_i_HCI_LMP) {
            u16 interval = btctler_little_endian_read_16(data, 0);
            u16 window = btctler_little_endian_read_16(data, 2);
            INFO("[Info]: [HCI_LMP]HCI_WRITE_PAGE_SCAN_ACTIVITY : %x / %x\r\n", interval, window);
        }
        DEV.page_scan_interval = btctler_little_endian_read_16(data, 0);
        DEV.page_scan_window = btctler_little_endian_read_16(data, 2);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c1b:
        LOG("[Info]: [HCI_LMP]HCI_READ_PAGE_SCAN_ACTIVITY\r\n");
        hci_send_event_cmd_complete(opcode, "122", 0, DEV.page_scan_interval, DEV.page_scan_window); break;
    case 0x0c1e:
        if (log_tag_const_i_HCI_LMP) {
            u16 interval = btctler_little_endian_read_16(data, 0);
            u16 window = btctler_little_endian_read_16(data, 2);
            INFO("[Info]: [HCI_LMP]HCI_WRITE_INQUIRY_SCAN_ACTIVITY : %x / %x\r\n", interval, window);
        }
        DEV.inquiry_scan_interval = btctler_little_endian_read_16(data, 0);
        DEV.inquiry_scan_window = btctler_little_endian_read_16(data, 2);
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c1d:
        LOG("[Info]: [HCI_LMP]HCI_READ_INQUIRY_SCAN_ACTIVITY\r\n");
        hci_send_event_cmd_complete(opcode, "122", 0, DEV.inquiry_scan_interval, DEV.inquiry_scan_window); break;
    case 0x0c23:
        LOG("[Info]: [HCI_LMP]HCI_READ_CLASS_OF_DEVICE:\r\n");
        hci_send_event_cmd_complete(opcode, "13", 0, DEV.dev_class); break;
    case 0x0c38:
        LOG("[Info]: [HCI_LMP]HCI_READ_NUMBER_OF_SUPPORTED_IAC\r\n");
        hci_send_event_cmd_complete(opcode, "11", 0, DEV.supported_iac); break;
    case 0x0c39:
        LOG("[Info]: [HCI_LMP]HCI_READ_CURRENT_IAC_LAP\r\n");
        hci_send_event_cmd_complete(opcode, "113", 0, DEV.num_current_iac, DEV.iac_lap); break;
    case 0x0c3a:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_CURRENT_IAC_LAP\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c3f:
        LOG("[Info]: [HCI_LMP]HCI_SET_AFH_HOST_CHANNEL_CLASSIFICATION\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c43:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_INQUIRY_SCAN_TYPE : %x\r\n", data[0]);
        DEV.inquiry_scan_type = data[0] & 1;
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c45:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_INQUIRY_MODE : %x\r\n", data[0]);
        DEV.inquiry_mode = data[0] & 3;
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c46:
        LOG("[Info]: [HCI_LMP]HCI_READ_PAGE_SCAN_TYPE\r\n");
        hci_send_event_cmd_complete(opcode, "1", 0, DEV.page_scan_type); break;
    case 0x0c47:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_PAGE_SCAN_TYPE : %x\r\n", data[0]);
        DEV.page_scan_type = data[0] & 1;
        hci_send_event_cmd_complete(opcode, "1", 0); break;
    case 0x0c58:
        LOG("[Info]: [HCI_LMP]HCI_READ_INQUIRY_RESPONSE_TRANSMIT_POWER_LEVEL\r\n");
        hci_send_event_cmd_complete(opcode, "11", 0, 0); break;
    case 0x0c36:
        LOG("[Info]: [HCI_LMP]HCI_READ_LINK_SUPERVISION_TIMEOUT(debugging): ret 20s\r\n");
        hci_send_event_cmd_complete(opcode, "1H2", 0, btctler_little_endian_read_16(data, 0), 32000.0); break;
    case 0x0c37:
        LOG("[Info]: [HCI_LMP]HCI_WRITE_LINK_SUPERVISION_TIMEOUT(debugging): ret 20s\r\n");
        hci_send_event_cmd_complete(opcode, "1H", 0, btctler_little_endian_read_16(data, 0)); break;
    case 0x0c53:
        LOG("[Info]: [HCI_LMP]HCI_REFRESH_ENCRYPTION_KEY\r\n");
        lmp_hci_refresh_encryption_key(btctler_little_endian_read_16(data, 0) & 0x0fff);
        hci_send_event(0x0f, "112", 0, 1, opcode); break;
    default: return 0;
    }
    return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev;
struct hci_parameter hci_param;
u8 log_tag_const_i_HCI_LMP;
static u8 heap[300], name_result[248];
static u32 alloc_bytes;
static int allocations, frees, assertions, events, event_type, event_opcode, event_values[5];
static int read_offsets[16], reads, scans, scan_value, refreshed, name_writes;
static int log_count, dump_count, dump_size, mutate_voice, mutate_complete, mutate_alloc_len;
static int mutate_log_packet, mutate_pairing, fail_allocation, repair_eir;
static u8 *active_packet;
static const char *logs[16], *event_format;
static const void *event_pointer;
static double supervision;
static void *baseband_malloc(size_t size)
{
    alloc_bytes = size; allocations++;
    if (mutate_alloc_len) DEV.eir.len = 1;
    return fail_allocation ? NULL : heap;
}
static void baseband_free(void *p) { CHECK(p == heap); frees++; }
void btctrler_assert_failed(const char *why, int value)
{ (void)value; assertions++; if (repair_eir && !strcmp(why, "gbredr_local_dev.eir.data")) DEV.eir.data = heap; }
void log_print(int level, const char *tag, const char *text, ...)
{
    CHECK(level == 2 && tag == NULL);
    if (log_count < 16) logs[log_count] = text;
    log_count++;
    if (mutate_log_packet) active_packet[0] = 0x7b;
    if (mutate_pairing) hci_param.ext_features[0] ^= 1;
    if (mutate_voice) {
        if (log_count == 1) { DEV.voice_setting = 0x100; log_tag_const_i_HCI_LMP = 0; }
        if (log_count == 2) DEV.voice_setting = 0xc0;
        if (log_count == 3) DEV.voice_setting = 0x22;
        if (log_count == 4) DEV.voice_setting = 3;
    }
}
void printf_buf(u8 *data, int size) { CHECK(data == active_packet); dump_count++; dump_size = size; }
u16 btctler_little_endian_read_16(const u8 *data, int off)
{
    if (reads < 16) read_offsets[reads] = off;
    reads++;
    return data[off] | (u16)data[off + 1] << 8;
}
u32 btctler_little_endian_read_24(const u8 *data, int off)
{ reads++; return data[off] | (u32)data[off + 1] << 8 | (u32)data[off + 2] << 16; }
void lmp_scan_enable(u8 value) { CHECK(events == 1); scans++; scan_value = value; }
void lmp_hci_write_local_name(const char *name) { CHECK(name == (char *)active_packet); name_writes++; }
int lmp_hci_refresh_encryption_key(u16 handle) { CHECK(events == 0); refreshed = handle; return 19; }
int hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    events++; event_type = event; event_format = format;
    event_values[0] = va_arg(ap, int); event_values[1] = va_arg(ap, int); event_values[2] = va_arg(ap, int);
    va_end(ap); return -3;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    events++; event_type = 14; event_opcode = opcode; event_format = format;
    event_values[0] = va_arg(ap, int);
    if (opcode == 0x0c39) {
        event_values[1] = va_arg(ap, int); event_pointer = va_arg(ap, const void *);
    } else if (opcode == 0x0c36) {
        event_values[1] = va_arg(ap, int); supervision = va_arg(ap, double);
    } else if (opcode == 0x0c14) {
        event_pointer = va_arg(ap, const void *); __builtin_memcpy(name_result, event_pointer, 248);
    } else {
        unsigned int n = (unsigned int)strlen(format);
        if (opcode == 0x0c46) n = 2; /* Deliberately extra SDK argument. */
        if (opcode == 0x0c27) n = 3;
        for (unsigned int i = 1; i < n; i++) event_values[i] = va_arg(ap, int);
    }
    if (mutate_complete) active_packet[0] = 0xe1;
    va_end(ap); return -5;
}
static void reset(u8 *packet, int logging)
{
    active_packet = packet; log_tag_const_i_HCI_LMP = logging;
    events = log_count = reads = assertions = allocations = frees = scans = name_writes = dump_count = 0;
    mutate_voice = mutate_complete = mutate_alloc_len = mutate_log_packet = mutate_pairing = fail_allocation = repair_eir = 0;
    event_pointer = NULL; event_format = NULL; refreshed = -1;
    __builtin_memset(event_values, 0, sizeof(event_values));
}
static void run(u16 opcode, u8 *packet)
{
    CHECK(hci_lmp_controller_baseband_cmd_handler(opcode, packet, 0) == 1);
}
int main(void)
{
    u8 packet[260] = {0};
    static const u16 supported[] = {
        0x0c05,0x0c09,0x0c0a,0x0c0d,0x0c11,0x0c12,0x0c13,0x0c14,
        0x0c15,0x0c16,0x0c18,0x0c1a,0x0c1b,0x0c1c,0x0c1d,0x0c1e,
        0x0c1f,0x0c20,0x0c23,0x0c24,0x0c25,0x0c26,0x0c27,0x0c28,
        0x0c2d,0x0c2e,0x0c2f,0x0c31,0x0c33,0x0c36,0x0c37,0x0c38,
        0x0c39,0x0c3a,0x0c3f,0x0c43,0x0c45,0x0c46,0x0c47,0x0c52,
        0x0c53,0x0c55,0x0c56,0x0c58,0x0c6d
    };
    for (int logging = 0; logging < 2; logging++) {
        for (unsigned int i = 0; i < sizeof(supported) / sizeof(*supported); i++) {
            reset(packet, logging); DEV.eir.data = heap;
            run(supported[i], packet);
            int silent = supported[i] == 0x0c2e || supported[i] == 0x0c2f || supported[i] == 0x0c31;
            CHECK(events == !silent);
            CHECK(log_count == (logging ? (supported[i] == 0x0c26 ? 5 : supported[i] == 0x0c52 ? 2 : 1) : 0));
        }
    }
    reset(packet, 0); CHECK(hci_lmp_controller_baseband_cmd_handler(0x0c17, NULL, 0) == 0 && !events);
    packet[0] = 0x34; packet[1] = 0xf2; packet[2] = 0x56; packet[3] = 0x78;
    packet[4] = 0x9a; packet[5] = 0xbc; packet[6] = 0xde;
    reset(packet, 1); run(0x0c33, packet);
    CHECK(reads == 6 && !memcmp(read_offsets, (int[]){0,3,5,0,3,5}, sizeof(int) * 6));
    CHECK(hci_param.host_buf_info.host_acl_data_packer_length == 0xf234);
    CHECK(hci_param.host_buf_info.host_sco_data_packer_length == 0x56);
    CHECK(hci_param.host_buf_info.host_total_num_acl_data_packets == 0x9a78);
    CHECK(hci_param.host_buf_info.host_total_num_sco_data_packets == 0xdebc);
    reset(packet, 1); mutate_log_packet = 1; run(0x0c18, packet);
    CHECK(reads == 2 && DEV.page_timeout_value == 0xf27b);
    reset(packet, 0); mutate_complete = 1; run(0x0c1a, packet);
    CHECK(scans == 1 && scan_value == 0xe1);
    packet[0] = 0x34;
    reset(packet, 0); DEV.connection_accept_timeout_value = 3; run(0x0c15, packet);
    CHECK(DEV.connection_accept_timeout_value == 0xf234 && event_values[1] == 0xf234);
    reset(packet, 0); run(0x0c36, packet); CHECK(supervision == 32000.0 && event_values[1] == 0xf234);
    reset(packet, 0); DEV.num_current_iac = 3; run(0x0c39, packet);
    CHECK(event_pointer == DEV.iac_lap && event_values[1] == 3 && !strcmp(event_format, "113"));
    reset(packet, 0); DEV.page_scan_type = 1; run(0x0c46, packet);
    CHECK(!strcmp(event_format, "1") && event_values[1] == 1);
    reset(packet, 0); run(0x0c53, packet);
    CHECK(refreshed == 0x234 && event_type == 15 && event_values[2] == 0x0c53);
    reset(packet, 1); mutate_voice = 1; run(0x0c26, packet);
    CHECK(log_count == 5 && log_tag_const_i_HCI_LMP == 0);
    CHECK(strstr(logs[1], "\xce\xbc-law Input") && strstr(logs[2], "Unsigned"));
    CHECK(strstr(logs[3], "16-bit") && strstr(logs[4], "Transparent"));
    reset(packet, 1); hci_param.ext_features[0] = 0xa5; mutate_pairing = 1; run(0x0c55, packet);
    CHECK(event_values[1] == 1 && hci_param.ext_features[0] == 0xa4);
    /* Original bitfield masks, including bytes outside the selected bits. */
    static const u16 ops[] = {0x0c20,0x0c0a,0x0c43,0x0c45,0x0c47};
    static const u8 masks[] = {1,2,4,48,8}, shifts[] = {0,1,2,4,3};
    for (unsigned int i = 0; i < 5; i++) for (unsigned int v = 0; v < 256; v++) {
        reset(packet, 0); packet[0] = v; *(u8 *)&DEV = 0xa5; run(ops[i], packet);
        CHECK(*(u8 *)&DEV == (u8)((0xa5 & ~masks[i]) | ((v << shifts[i]) & masks[i])));
    }
    __builtin_memset(packet, 0, sizeof(packet));
    packet[1] = 2; packet[2] = 0x91; packet[3] = 0x82;
    __builtin_memset(heap, 0xa5, sizeof(heap));
    reset(packet, 1); DEV.eir.data = NULL; run(0x0c52, packet);
    CHECK(allocations == 1 && alloc_bytes == 240 && !frees && !assertions);
    CHECK(DEV.eir.len == 3 && DEV.eir.type == 3 && !memcmp(heap, packet + 1, 3));
    CHECK(heap[3] == 0 && heap[4] == 0xa5 && dump_count == 1 && dump_size == 0 && log_count == 3);
    /* Parser offsets and returned length both wrap at eight bits. */
    __builtin_memset(packet, 0, sizeof(packet)); packet[1] = 250; packet[252] = 6;
    reset(packet, 0); run(0x0c52, packet); CHECK(DEV.eir.len == 2 && DEV.eir.type == 3);
    /* Oversized backing safely observes the SDK's returning length assertion. */
    __builtin_memset(packet, 0, sizeof(packet)); packet[1] = 240;
    reset(packet, 0); run(0x0c52, packet);
    CHECK(assertions == 1 && DEV.eir.len == 241 && DEV.eir.type == 10 && !memcmp(heap, packet + 1, 241));
    __builtin_memset(packet, 0, sizeof(packet)); packet[1] = 16;
    reset(packet, 0); DEV.eir.data = NULL; mutate_alloc_len = 1; run(0x0c52, packet);
    CHECK(DEV.eir.type == 10 && DEV.eir.len == 1 && heap[0] == 16 && heap[1] == 0);
    reset(packet, 0); DEV.eir.data = NULL; fail_allocation = repair_eir = 1;
    run(0x0c52, packet);
    CHECK(assertions == 1 && DEV.eir.data == heap && events == 1);
    /* The hook supplies backing only to observe SDK continuation safely. */
    __builtin_memset(DEV.local_name, 0, sizeof(DEV.local_name));
    __builtin_memcpy(DEV.local_name, "name", 5);
    reset(packet, 0); run(0x0c14, packet);
    CHECK(alloc_bytes == 248 && allocations == 1 && frees == 1 && !memcmp(name_result, "name", 5));
    for (unsigned int i = 5; i < sizeof(name_result); i++) CHECK(name_result[i] == 0);
    return TEST_DONE("bd19_lmp_hci_baseband");
}
#endif
