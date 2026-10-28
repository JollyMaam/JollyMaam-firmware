/* Owned common controller/baseband and local-information commands
 * (device_manager.c): event masks, reset, version, features, address. */
#include <string.h>
#include "btctrler/glue.h"

#ifdef CONFIG_CPU_BD19
#define DEV_CODE __attribute__((section(".hci_controller_code")))
extern BTCTRLER_CONFIG int config_btctler_le_roles;
extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_LMP;
void log_print(int, const char *, const char *, ...);
void printf_buf(const void *, int);
typedef u64 device_mask_load __attribute__((aligned(1), may_alias));
typedef u64 device_mask_store __attribute__((aligned(2), may_alias));
#else
#define DEV_CODE
#endif

/* The controller's identity and defaults (the SDK's device_manager.c):
 * Core 5.4 (13), manufacturer Zhuhai Jieli (0x05d6). */
__attribute__((section(".hci_controller_data"))) struct hci_parameter hci_param = {
    .hci_version = 13, .hci_revision = 0x3120,
    .lmp_pal_version = 13, .lmp_pal_subversion = 0x22bb,
    .manufacturer_name = 0x05d6,
    .local_supported_commands = {
        0xff, 0xff, 0xff, 0x03, 0x80, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0x1f, 0xf2, 0x0f, 0xe8, 0xfe,
        0x3f, 0xf7, 0x8d, 0xff, 0x1c, 0x00, 0x00, 0x00, 0x7d, 0xf7, 0xff, 0xff, 0x7f,
    },
    .addr = { 0x57, 0x74, 0x78, 0xa2, 0x5a, 0xab },
    .features = { 0xbf, 0xfe, 0x8d, 0xfa, 0xc8, 0x2d, 0x79, 0x83 },
    .default_link_policy = 5,       /* role switch and sniff allowed */
    .need_enc = 1,
};

/* Setters for the SDK's application layer.  LMP features page 0: byte 3
 * bits 1-2 EDR ACL 2/3 Mb/s, bytes 4 and 5 bit 4 AFH classification as
 * slave and as master, byte 7 bit 2 enhanced power control. */
DEV_CODE void set_bt_full_name_event(u8 en) { hci_param.full_name_event = en & 1; }
DEV_CODE void set_bt_need_enc(u8 enc) { hci_param.need_enc = enc & 1; }
DEV_CODE void set_bt_version(u8 version) { hci_param.hci_version = hci_param.lmp_pal_version = version; }
DEV_CODE void set_bt_afh_classs_enc(u8 afh_class)
{
    if (afh_class) { hci_param.features[4] |= 0x10; hci_param.features[5] |= 0x10; }
    else { hci_param.features[4] &= ~0x10; hci_param.features[5] &= ~0x10; }
}
static void feature(int byte, u8 bit, u8 en)
{
    if (en) hci_param.features[byte] |= bit;
    else hci_param.features[byte] &= ~bit;
}
DEV_CODE void set_bt_enhanced_power_control(u8 en) { feature(7, 0x04, en); }
DEV_CODE void set_bt_data_rate_acl_3mbs_mode(u8 en) { feature(3, 0x04, en); }
DEV_CODE void set_bt_data_rate_acl_2mbs_mode(u8 en) { feature(3, 0x02, en); }

/* The SDK printed the remote's manufacturer and version here (LE). */
DEV_CODE void hci_read_remote_version_information_show(const char *str, u8 version, u16 name, u16 subversion)
{
#ifdef CONFIG_CPU_BD19
    if (!log_tag_const_i_HCI_LMP) return;
    log_print(2, NULL, "[Info]: [HCI_LMP]HCI_EVENT_READ_REMOTE_VERSION_INFORMATION_COMPLETE \r\n---Module : %s\r\n", str);
    switch (name) {
    case 10: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Cambridge Silicon Radio\r\n"); break;
    case 13: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Texas Instruments Inc.\r\n"); break;
    case 15: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Broadcom Corporation\r\n"); break;
    case 29: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Qualcomm\r\n"); break;
    case 70: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : MediaTek, Inc.\r\n"); break;
    case 76: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Apple,Inc.\r\n"); break;
    case 89: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Nordic Semiconductor ASA\r\n"); break;
    case 90: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : EM Microelectronic-Marin SA\r\n"); break;
    case 93: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Realtek Semiconductor Corporation\r\n"); break;
    case 529: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : Telink Semiconductor Co. Ltd\r\n"); break;
    default: log_print(2, NULL, "[Info]: [HCI_LMP]-Manufacture Name  : %04x\r\n", name); break;
    }
    switch (version) {
    case 3: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 2.0 + EDR\r\n"); break;
    case 4: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 2.1 + EDR\r\n"); break;
    case 5: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 3.0 + HS\r\n"); break;
    case 6: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 4.0\r\n"); break;
    case 7: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 4.1\r\n"); break;
    case 8: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 4.2\r\n"); break;
    case 9: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 5.0\r\n"); break;
    case 10: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 5.1\r\n"); break;
    case 11: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : Bluetooth Core Specification 5.2\r\n"); break;
    default: log_print(2, NULL, "[Info]: [HCI_LMP]-Version           : %02x\r\n", version); break;
    }
    log_print(2, NULL, "[Info]: [HCI_LMP]-Subversion        : %04x\r\n", subversion);
#else
    (void)str; (void)version; (void)name; (void)subversion;
#endif
}

DEV_CODE int hci_controller_baseband_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    (void)len;
#ifndef CONFIG_CPU_BD19
    u32 required;
    switch (opcode) {
    case 0x0c01: case 0x0c63: required = 8; break; /* Event Mask / Page 2 */
    case 0x0c03: required = 0; break; /* Reset */
    default: return 0;
    }
    if (len < required || (required && !data)) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
#endif
    switch (opcode) {
    case 0x0c01:
#ifdef CONFIG_CPU_BD19
        *(device_mask_store *)(void *)hci_param.event_mask = *(const device_mask_load *)(const void *)data;
        if (log_tag_const_i_HCI_LMP) {
            log_print(2, NULL, "[Info]: [HCI_LMP]HCI_SET_EVENT_MASK\r\n");
            printf_buf(hci_param.event_mask, 8);
        }
#else
        memcpy(hci_param.event_mask, data, 8);
#endif
        break;
    case 0x0c63:
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) {
            log_print(2, NULL, "[Info]: [HCI_LMP]HCI_SET_EVENT_MASK_PAGE_2\r\n");
            printf_buf(data, 8);
        }
#endif
        /* The SDK acknowledges Page 2 without storing or applying it. */
        break;
    case 0x0c03: {
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]---------HCI_RESET---------\r\n");
        int modules = config_btctler_modules;
#else
        int modules = config_btctler_modules;
#endif
        if (modules & 1) lmp_reset();
#ifdef CONFIG_CPU_BD19
        if (modules & 2) {
#else
        if (config_btctler_modules & 2) {
#endif
            hci_param.features[4] |= 0x40;  /* LE supported (controller) */
            hci_param.features[6] |= 0x02;  /* simultaneous LE and BR/EDR */
            if (config_btctler_mode & 1) ll_reset();
            else if (config_btctler_mode & 4) le_dut_hci_reset_handle();
        }
        break;
    }
#ifdef CONFIG_CPU_BD19
    default: return 0;
#endif
    }
    hci_send_event_cmd_complete(opcode, "1", 0);
    return 1;
}

DEV_CODE int hci_informational_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    (void)len;
#ifndef CONFIG_CPU_BD19
    u32 required;
    switch (opcode) {
    case 0x1001: case 0x1002: case 0x1003: case 0x1009: required = 0; break;
    case 0x1004: required = 1; break;
    default: return 0;
    }
    if (len < required || (required && !data)) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
#endif
    switch (opcode) {
    case 0x1001: /* Read Local Version Information */
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_LOCAL_VERSION_INFORMATION\r\n");
#endif
        hci_send_event_cmd_complete(opcode, "112122", 0, hci_param.hci_version,
            hci_param.hci_revision, hci_param.lmp_pal_version,
            hci_param.manufacturer_name, hci_param.lmp_pal_subversion);
        break;
    case 0x1002: /* Read Local Supported Commands */
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_LOCAL_SUPPORTED_COMMAND\r\n");
#endif
        hci_send_event_cmd_complete(opcode, "1c64", 0, hci_param.local_supported_commands);
        break;
    case 0x1003: /* Read Local Supported Features */
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_LOCAL_SUPPORT_FEATURES\r\n");
#endif
        hci_send_event_cmd_complete(opcode, "1c08", 0, hci_param.features);
        break;
    case 0x1004: { /* Read Local Extended Features */
#ifdef CONFIG_CPU_BD19
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_LOCAL_EXTENDED_FEATURES %d\r\n", data[0]);
        u32 page = data[0];
        const u8 *features = (const u8 *)&hci_param + offsetof(struct hci_parameter, ext_features)
                           + ((int)page - 1) * 8;
        hci_send_event_cmd_complete(opcode, "111c08", 0, page, 2, features);
#else
        /* Page 0 is the standard features, 1-2 ext_features.  The SDK
         * indexed past ext_features for larger pages; refuse those. */
        static const u8 empty[8];
        u32 page = data[0];
        const u8 *features = page == 0 ? hci_param.features :
            page <= 2 ? hci_param.ext_features + (page - 1) * 8 : 0;
        hci_send_event_cmd_complete(opcode, "111c08", features ? 0 : 0x12,
            page, 2, features ? features : empty);
#endif
        break;
    }
    case 0x1009: { /* Read BD_ADDR: Classic's in a dual-mode build. */
#ifdef CONFIG_CPU_BD19
        u8 logging = log_tag_const_i_HCI_LMP;
        if (logging) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_BD_ADDR\r\n");
#endif
        if (config_btctler_modules & 1) {
#ifdef CONFIG_CPU_BD19
            if (logging) { char *addr = btctler_bd_addr_to_str(hci_param.addr); log_print(2, NULL, "[Info]: [HCI_LMP]CLASSIC_MAC %s\r\n", addr); }
#endif
            hci_send_event_cmd_complete(opcode, "1A", 0, hci_param.addr);
        } else if (config_btctler_modules & 2) {
#ifdef CONFIG_CPU_BD19
            if (logging) { char *addr = btctler_bd_addr_to_str(le_param.public_address); log_print(2, NULL, "[Info]: [HCI_LMP]LE_MAC %s\r\n", addr); }
#endif
            hci_send_event_cmd_complete(opcode, "1A", 0, le_param.public_address);
        }
        break;
    }
#ifdef CONFIG_CPU_BD19
    default: return 0;
#endif
    }
    return 1;
}

#ifdef CONFIG_CPU_BD19
DEV_CODE int hci_link_control_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    (void)len;
    switch (opcode) {
    case 0x0406:
    case 0x041d: {
        if (opcode == 0x041d && log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_REMOTE_VERSION_INFORMATION\r\n");
        u16 handle = btctler_little_endian_read_16(data, 0) & 0x0fff;
        if (opcode == 0x0406 && log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_DISCONNECT 0x%x, 0x%x\r\n", handle, data[2]);
        int modules = config_btctler_modules;
        u8 status = opcode == 0x0406 ? 2 : 0;
        if ((modules & 2) && (config_btctler_le_roles & 3)
            && ll_link_for_handle(handle)) {
            status = (u8)ll_push_control_data(opcode, data);
        }
        if (modules & 1) {
            struct lmp_connection *conn = lmp_conn_for_handle(handle);
            if (!conn && opcode == 0x0406) conn = lmp_esco_conn_for_handle(handle);
            if (conn) {
                int result = opcode == 0x0406
                    ? lmp_hci_disconnect(handle, data[2])
                    : lmp_hci_read_remote_version_information(handle);
                status = opcode == 0x0406 ? ((u8)result != 0 ? 2 : 0) : (u8)result;
            }
        }
        hci_send_event(0x0f, "112", status, 1, opcode);
        return 1;
    }
    default:
        return 0;
    }
}

DEV_CODE int hci_link_policy_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

DEV_CODE int hci_status_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)data; (void)len; return 0; }

DEV_CODE int hci_testing_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    (void)len;
    switch (opcode) {
    case 0x1801:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_READ_LOOPBACK_MODE\r\n");
        return 1;
    case 0x1802:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_WRITE_LOOPBACK_MODE\r\n");
        return 1;
    case 0x1803:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_ENABLE_DEVICE_UNDER_TEST_MODE\r\n");
        return 1;
    case 0x1804:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_WRITE_SIMPLE_PAIRING_DEBUG_MODE : %d\r\n", data[0]);
        hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 0x1807:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_ENABLE_AMP_RECEIVER_REPORTS\r\n");
        return 1;
    case 0x1808:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_AMP_TEST_END\r\n");
        return 1;
    case 0x1809:
        if (log_tag_const_i_HCI_LMP) log_print(2, NULL, "[Info]: [HCI_LMP]HCI_AMP_TEST\r\n");
        return 1;
    default: return 0;
    }
}
#endif

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct le_parameter le_param;
int config_btctler_modules = 3, config_btctler_mode = 1, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
static u8 *const mask = hci_param.event_mask, *const features = hci_param.features,
    *const extended = hci_param.ext_features, *const commands = hci_param.local_supported_commands,
    *const classic = hci_param.addr, *const le = le_param.public_address;
static int sequence, classic_at, le_at, dut_at, event_at, error, seen_opcode;
static int values[5], page_seen, max_seen;
static unsigned char payload[64];
static const char *format_seen;
#ifdef CONFIG_CPU_BD19
int config_btctler_le_roles;
u8 log_tag_const_i_HCI_LMP;
static const u8 *feature_pointer, *dump_pointer;
static int dump_length, logs, module_after_reset = -1;
static char log_text[8][192];
static u8 *mutate_page;
static int le_present, classic_present, esco_present, le_result, classic_result;
static int le_calls, classic_calls, esco_lookups, status_seen;
static struct le_link test_le;
static struct lmp_connection test_conn;
void log_print(int level, const char *tag, const char *format, ...)
{
    CHECK(level == 2 && !tag && logs < 8);
    va_list ap; va_start(ap, format);
    vsnprintf(log_text[logs++], sizeof(log_text[0]), format, ap); va_end(ap);
    if (mutate_page) *mutate_page = 2;
}
void printf_buf(const void *ptr, int length)
{ dump_pointer = ptr; dump_length = length; }
char *btctler_bd_addr_to_str(const u8 *addr)
{ CHECK(addr == classic || addr == le); return "address"; }
u16 btctler_little_endian_read_16(const u8 *p, int pos)
{ return p[pos] | (u16)p[pos + 1] << 8; }
struct le_link *ll_link_for_handle(int h)
{ CHECK(h == 0x234); return le_present ? &test_le : NULL; }
struct lmp_connection *lmp_conn_for_handle(int h)
{ CHECK(h == 0x234); return classic_present ? &test_conn : NULL; }
struct lmp_connection *lmp_esco_conn_for_handle(int h)
{ CHECK(h == 0x234); ++esco_lookups; return esco_present ? &test_conn : NULL; }
u8 ll_push_control_data(u16 op, const u8 *p)
{ CHECK((op == 0x0406 || op == 0x041d) && p[2] == 0x13); ++le_calls; return le_result; }
int lmp_hci_disconnect(u16 h, u8 reason)
{ CHECK(h == 0x234 && reason == 0x13); ++classic_calls; return classic_result; }
int lmp_hci_read_remote_version_information(u16 h)
{ CHECK(h == 0x234); ++classic_calls; return classic_result; }
int hci_send_event(int event, const char *format, ...)
{
    CHECK(event == 15 && !strcmp(format, "112"));
    va_list ap; va_start(ap, format); status_seen = va_arg(ap, int);
    CHECK(va_arg(ap, int) == 1); seen_opcode = va_arg(ap, int); va_end(ap);
    event_at = ++sequence; return 0;
}
#endif
void lmp_reset(void)
{
    classic_at = ++sequence;
#ifdef CONFIG_CPU_BD19
    if (module_after_reset >= 0) config_btctler_modules = module_after_reset;
#endif
}
void ll_reset(void)
{ le_at = ++sequence; CHECK(features[4] & 0x40); CHECK(features[6] & 2); }
void le_dut_hci_reset_handle(void) { dut_at = ++sequence; }
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); event_at = ++sequence; seen_opcode = opcode;
    format_seen = format; error = va_arg(ap, int);
    if (!strcmp(format, "112122")) for (int i = 0; i < 5; i++) values[i] = va_arg(ap, int);
    else if (!strcmp(format, "1A")) memcpy(payload, va_arg(ap, unsigned char *), 6);
    else if (!strcmp(format, "1c08")) memcpy(payload, va_arg(ap, unsigned char *), 8);
    else if (!strcmp(format, "1c64")) memcpy(payload, va_arg(ap, unsigned char *), 64);
    else if (!strcmp(format, "111c08")) {
        page_seen = va_arg(ap, int); max_seen = va_arg(ap, int);
        const u8 *p = va_arg(ap, const unsigned char *);
#ifdef CONFIG_CPU_BD19
        feature_pointer = p;
        if (page_seen <= 2) memcpy(payload, p, 8);
#else
        memcpy(payload, p, 8);
#endif
    } else CHECK(!strcmp(format, "1"));
    va_end(ap); return 0;
}
static void clear(void)
{ sequence = classic_at = le_at = dut_at = event_at = error = 0; }
int main(void)
{
    unsigned char data[8] = {1,2,3,4,5,6,7,8};
#ifndef CONFIG_CPU_BD19
    for (int n = 0; n < 8; n++) {
        clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c01, 0, n) == 1);
        CHECK(error == 0x12 && event_at && !classic_at && !le_at);
        clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c63, 0, n) == 1);
        CHECK(error == 0x12 && event_at);
    }
#endif
    clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c01, data, 8) == 1);
    CHECK(!memcmp(mask, data, 8) && !error);
    memset(data, 0xa5, 8); clear(); hci_controller_baseband_cmd_handler(0x0c63, data, 8);
    CHECK(mask[0] == 1 && mask[7] == 8 && !error); /* Page 2 remains ignored. */
    memset(features, 0xa5, 8); clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(classic_at == 1 && le_at == 2 && event_at == 3 && !dut_at);
    CHECK(features[4] == 0xe5 && features[6] == 0xa7 && features[5] == 0xa5);
    config_btctler_mode = 5; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(le_at && !dut_at); /* Normal mode wins over DUT. */
    config_btctler_mode = 4; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(dut_at == 2 && event_at == 3 && !le_at);
    config_btctler_modules = 1; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(classic_at == 1 && event_at == 2 && !dut_at && !le_at);
    config_btctler_modules = 2; config_btctler_mode = 0;
    clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(!classic_at && !le_at && !dut_at && event_at == 1);
    clear(); CHECK(!hci_controller_baseband_cmd_handler(0x0c02, 0, 0) && !event_at);
    config_btctler_modules = 3;
    hci_informational_parameters_cmd_handler(0x1001, 0, 0);
    CHECK(!strcmp(format_seen, "112122") && values[0] == 13 && values[1] == 12576);
    CHECK(values[2] == 13 && values[3] == 1494 && values[4] == 8891);
    for (int i = 0; i < 64; i++) commands[i] = (unsigned char)i;
    hci_informational_parameters_cmd_handler(0x1002, 0, 0); CHECK(!memcmp(payload, commands, 64));
    hci_informational_parameters_cmd_handler(0x1003, 0, 0); CHECK(!memcmp(payload, features, 8));
    for (int i = 0; i < 16; i++) extended[i] = (unsigned char)(i + 10);
    for (int i = 0; i < 256; i++) {
        unsigned char page = (unsigned char)i; clear();
        CHECK(hci_informational_parameters_cmd_handler(0x1004, &page, 1) == 1);
        CHECK(page_seen == i && max_seen == 2 && seen_opcode == 0x1004);
#ifdef CONFIG_CPU_BD19
        CHECK(!error && (uintptr_t)feature_pointer == (uintptr_t)extended + (i - 1) * 8);
        if (i <= 2) CHECK(!memcmp(payload, i ? extended + (i - 1) * 8 : features, 8));
#else
        CHECK(error == (i > 2 ? 0x12 : 0));
        const unsigned char zero[8] = {0};
        CHECK(!memcmp(payload, i == 0 ? features : i <= 2 ? extended + (i - 1) * 8 : zero, 8));
#endif
    }
#ifndef CONFIG_CPU_BD19
    clear(); hci_informational_parameters_cmd_handler(0x1004, 0, 0); CHECK(error == 0x12);
#endif
    classic[0] = 1; le[0] = 2;
    hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(payload[0] == 1);
    config_btctler_modules = 2; hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(payload[0] == 2);
    config_btctler_modules = 0; clear(); hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(!event_at);
    CHECK(!hci_informational_parameters_cmd_handler(0x1005, 0, 0));
#ifdef CONFIG_CPU_BD19
    /* Lengths are ignored by the SDK; the storage remains valid here. */
    clear(); hci_controller_baseband_cmd_handler(0x0c01, data, 0);
    CHECK(!error && !memcmp(mask, data, 8));
    u8 page = 1;
    clear(); hci_informational_parameters_cmd_handler(0x1004, &page, 0);
    CHECK(!error && page_seen == 1);
    config_btctler_modules = 3; module_after_reset = 1;
    clear(); hci_controller_baseband_cmd_handler(0x0c03, NULL, 0);
    CHECK(classic_at && le_at == 0); /* mode 0: no reset, but LE features are still set */
    module_after_reset = -1;
    config_btctler_modules = 3; config_btctler_mode = 1; module_after_reset = 1;
    clear(); hci_controller_baseband_cmd_handler(0x0c03, NULL, 0);
    CHECK(classic_at == 1 && le_at == 2); module_after_reset = -1;

    const u8 params[] = {0x34, 0xf2, 0x13};
    const u16 opcodes[] = {0x0406, 0x041d};
    for (unsigned k = 0; k < 2; ++k)
        for (int modules = 0; modules < 4; ++modules)
            for (int roles = 0; roles < 4; ++roles) {
                config_btctler_modules = modules; config_btctler_le_roles = roles;
                le_present = classic_present = 1; esco_present = 0;
                le_result = 9; classic_result = 0x105;
                le_calls = classic_calls = esco_lookups = 0;
                CHECK(hci_link_control_cmd_handler(opcodes[k], params, 0) == 1);
                CHECK(le_calls == !!((modules & 2) && roles));
                CHECK(classic_calls == !!(modules & 1));
                CHECK(status_seen == (modules & 1 ? (k ? 5 : 2) : le_calls ? 9 : k ? 0 : 2));
            }
    config_btctler_modules = 1; classic_present = 0; esco_present = 1;
    hci_link_control_cmd_handler(0x0406, params, 0); CHECK(esco_lookups == 1);
    esco_lookups = 0; classic_calls = 0;
    hci_link_control_cmd_handler(0x041d, params, 0);
    CHECK(!esco_lookups && !classic_calls && !status_seen);
    classic_present = 1; classic_result = 256;
    hci_link_control_cmd_handler(0x0406, params, 0); CHECK(!status_seen);
    CHECK(!hci_link_control_cmd_handler(0xffff, NULL, 0));
    CHECK(!hci_link_policy_cmd_handler(0xffff, NULL, 0));
    CHECK(!hci_status_parameters_cmd_handler(0xffff, NULL, 0));
    for (u16 op = 0x1800; op <= 0x180a; ++op) {
        clear(); int handled = hci_testing_cmd_handler(op, params, 0);
        CHECK(handled == (op >= 0x1801 && op <= 0x1809 && op != 0x1805 && op != 0x1806));
        CHECK(!!event_at == (op == 0x1804));
    }
    log_tag_const_i_HCI_LMP = 1;
    logs = 0; hci_controller_baseband_cmd_handler(0x0c01, data, 0);
    CHECK(logs == 1 && dump_pointer == mask && dump_length == 8);
    logs = 0; hci_controller_baseband_cmd_handler(0x0c63, data, 0);
    CHECK(logs == 1 && dump_pointer == data && dump_length == 8);
    logs = 0; page = 1; mutate_page = &page;
    hci_informational_parameters_cmd_handler(0x1004, &page, 0);
    CHECK(logs == 1 && page_seen == 2 && strstr(log_text[0], "FEATURES 1")); mutate_page = NULL;
    for (int modules = 1; modules <= 2; ++modules) {
        config_btctler_modules = modules; logs = 0;
        hci_informational_parameters_cmd_handler(0x1009, NULL, 0);
        CHECK(logs == 2 && strstr(log_text[1], "MAC address"));
    }
    const u16 manufacturers[] = {10,13,15,29,70,76,89,90,93,529,0xffff};
    for (unsigned i = 0; i < sizeof(manufacturers) / sizeof(*manufacturers); ++i)
        for (unsigned v = 0; v < 14; ++v) {
            logs = 0; hci_read_remote_version_information_show("test", v, manufacturers[i], 0xabcd);
            CHECK(logs == 4 && strstr(log_text[0], "test") && strstr(log_text[3], "abcd"));
        }
#endif
    return TEST_DONE("btctrler_device_manager");
}
#endif
