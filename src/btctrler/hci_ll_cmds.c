/* BLE HCI command handling and controller address accessors from hci_ll_cmds.c.
 * Keep the SDK's OCF-only dispatch, feature-gate silence, and command status
 * choices, including the legacy result values returned after event emission. */
#include "btctrler/glue.h"
#include <stdarg.h>
#include <string.h>

#ifdef UNITTEST
u64 config_btctler_le_features;
#else
extern const u64 config_btctler_le_features;
#endif
void printf_buf(const u8 *buf, int size);
void hci_add_cmd_handler(struct hci_cmd_registration *handler);
void log_print(int level, const char *file, const char *format, ...);
extern void ll_read_local_p256_public_key_ir(void) __asm__("ll_read_local_p256_public_key");
extern void ll_generate_dh_key_ir(const u8 *data, u32 size) __asm__("ll_generate_dh_key");

#define HCI_LL_LOG(...) log_print(2, (const char *)0, __VA_ARGS__)
#define BLE_HCI_CODE __attribute__((section(".ble_hci_code")))

_Static_assert(offsetof(struct le_parameter, event_mask) == 0, "le event mask offset");
_Static_assert(offsetof(struct le_parameter, public_address) == 8, "le public address offset");
_Static_assert(offsetof(struct le_parameter, random_address) == 14, "le random address offset");
_Static_assert(offsetof(struct le_parameter, features) == 34, "le features offset");
_Static_assert(offsetof(struct le_parameter, support_states) == 56, "le supported states offset");
_Static_assert(offsetof(struct le_parameter, acl_packets_total_num) == 64, "le acl count offset");
_Static_assert(offsetof(struct le_parameter, acl_data_packet_length) == 66, "le acl length offset");
_Static_assert(offsetof(struct le_parameter, transmit_power_level) == 72, "le tx power offset");
_Static_assert(offsetof(struct le_parameter, white_list_size) == 151, "le white-list size offset");
#ifdef UNITTEST
_Static_assert(offsetof(struct le_parameter, resolution_enable) == 168, "host le resolution-enable offset");
_Static_assert(offsetof(struct le_parameter, resolving_list_size) == 169, "host le resolving-list size offset");
#else
_Static_assert(offsetof(struct le_parameter, resolution_enable) == 160, "le resolution-enable offset");
_Static_assert(offsetof(struct le_parameter, resolving_list_size) == 161, "le resolving-list size offset");
#endif

struct le_parameter le_param __attribute__((section(".ble_hci_data"))) = {
    .public_address = { 1, 2, 3, 4, 5, 8 },
    .version = { 6, 15, 0, 9, 97, 0, 0, 0 },
    .versnr = 8,
    .compld = 120,
    .subversnr = 1906,
    .support_states = { 0xff, 0xff, 0xff, 0x1f, 0, 0, 0, 0 },
    .transmit_power_level = 1,
    .white_list_size = 25,
    .resolving_list_size = 1,
};

static BLE_HCI_CODE int le_controller_handler(u16 opcode, const u8 *data, u32 size);
static BLE_HCI_CODE int vendor_handler(u16 opcode, const u8 *data, u32 size);

static struct hci_cmd_registration le_controller_registration
    __attribute__((section(".ble_hci_data"))) = {
        .le_controller = le_controller_handler,
        .vendor = vendor_handler,
    };

BLE_HCI_CODE int ll_push_acl_data(const u8 *packet, int len)
{
    (void)len;
    int handle = btctler_little_endian_read_16(packet, 0) & 0x0fff;
    u16 payload_size = btctler_little_endian_read_16(packet, 2);
    ll_send_acl_packet(handle, packet, payload_size);
    return 0;
}

BLE_HCI_CODE int hci_le_controller_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    return le_controller_handler(opcode, data, len);
}

static BLE_HCI_CODE int le_controller_handler(u16 opcode, const u8 *data, u32 size)
{
    u8 channel_map[5];
    u8 enc[16];
    u8 rand[8];
    u8 peer_rpa[6];
    u8 local_rpa[6];
    u16 ocf = opcode & 0x03ff;
    int status;

    switch (ocf) {
    case 1: /* HCI_LE_SET_EVENT_MASK */
        memcpy(le_param.event_mask, data, 8);
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_EVENT_MASK\r\n");
        printf_buf(le_param.event_mask, 8);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 2: /* HCI_LE_READ_BUFFER_SIZE */
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_BUFFER_SIZE\r\n");
        (void)hci_send_event_cmd_complete(opcode, "121", 0,
            le_param.acl_data_packet_length, le_param.acl_packets_total_num);
        return 1;
    case 3: /* HCI_LE_READ_LOCAL_SUPPORTED_FEATURES */
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_LOCAL_SUPPORT_FEATURES : 0x%x\r\n", le_param.features[0]);
        (void)hci_send_event_cmd_complete(opcode, "1c08", 0, le_param.features);
        return 1;
    case 5: /* HCI_LE_SET_RANDOM_ADDRESS */
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_RANDOM_ADDRESS\r\n");
        memcpy(le_param.random_address, data, 6);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 6:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_ADVERTISING_PARAMETERS\r\n");
        status = ll_adv_set_params(data);
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 7:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_ADVERTISING_CHANNEL_TX_POWER\r\n");
        (void)hci_send_event_cmd_complete(opcode, "11", 0, le_param.transmit_power_level);
        return 1;
    case 8:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_ADVERTISING_DATA\r\n");
        status = ll_adv_set_adv_data(data);
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 9:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_SCAN_RESPONSE_DATA\r\n");
        status = ll_adv_set_scan_rsp_data(data);
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 10:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_ADVERTISE_ENABLE %s\r\n", *data ? "Enable" : "Disable");
        status = *data ? ll_adv_open() : ll_adv_close();
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 11:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_SCAN_PARAMETERS\r\n");
        status = ll_scan_set_params(data);
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 12:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_SCAN_ENABLE %s\r\n", *data ? "Enable" : "Disable");
        status = *data ? ll_scan_open(data) : ll_scan_close();
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        return 1;
    case 13:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_CREATE_CONNECTION\r\n");
        status = ll_init_open(data);
        (void)hci_send_event(15, "112", status & 0xff, 1, opcode);
        return 1;
    case 14:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_CREATE_CONNECTION_CANCEL\r\n");
        destroy_le_ext_init();
        status = ll_init_close();
        (void)hci_send_event_cmd_complete(opcode, "1", status & 0xff);
        le_connection_complete_event(0, 2);
        return 1;
    case 15:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_WHITE_LIST_SIZE\r\n");
        (void)hci_send_event_cmd_complete(opcode, "11", 0, le_param.white_list_size);
        return 1;
    case 16:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_CLEAR_WHITE_LIST\r\n");
        ll_white_list_init();
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 17:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_ADD_DEVICE_TO_WHITE_LIST\r\n");
        ll_white_list_add(data);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 18:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_REMOVE_DEVICE_FROM_WHITE_LIST\r\n");
        status = ll_white_list_remove(data);
        (void)hci_send_event_cmd_complete(opcode, "1", status);
        return 1;
    case 19:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_CONNECTION_UPDATE\r\n");
        status = (config_btctler_le_roles & 1) ? ll_push_control_data(opcode, data) : 12;
        (void)hci_send_event(15, "112", status, 1, opcode);
        return 1;
    case 20:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_HOST_CHANNEL_CLASSIFICATION\r\n");
        status = (config_btctler_le_roles & 1) ? ll_push_control_data(opcode, data) : 12;
        (void)hci_send_event_cmd_complete(opcode, "1", status);
        return 1;
    case 21:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_CHANNEL_MAP\r\n");
        if (!(config_btctler_le_roles & 3)) return 1;
        status = ll_read_channel_map(channel_map, data);
        (void)hci_send_event_cmd_complete(opcode, "1Hc05", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff, channel_map);
        return 1;
    case 22:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_REMOTE_USED_FEATURES\r\n");
        status = (config_btctler_le_roles & 3) ? ll_push_control_data(opcode, data) : 12;
        (void)hci_send_event(15, "112", status, 1, opcode);
        return 1;
    case 23:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_ENCRYPT\r\n");
        if (!(config_btctler_le_features & 1)) return 1;
        aes128_start((u8 *)data, (u8 *)(data + 16), enc);
        (void)hci_send_event_cmd_complete(opcode, "1c16", 0, enc);
        return 1;
    case 24:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_RAND\r\n");
        pseudo_random_genrate(rand, 8);
        (void)hci_send_event_cmd_complete(opcode, "1c08", 0, rand);
        return 1;
    case 25:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_START_ENCRYPT\r\n");
        status = ((config_btctler_le_roles & 3) && (config_btctler_le_features & 1))
            ? ll_push_control_data(opcode, data) : 12;
        (void)hci_send_event(15, "112", status, 1, opcode);
        return 1;
    case 26:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_LONG_TERM_KEY_REQUEST_REPLY\r\n");
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 1)) return 1;
        printf_buf(data, 18);
        status = ll_push_control_data(opcode, data);
        (void)hci_send_event_cmd_complete(opcode, "12", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff);
        return 1;
    case 27:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_LONG_TERM_KEY_REQUEST_NAGATIVE_REPLY\r\n");
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 1)) return 1;
        status = ll_push_control_data(opcode, data);
        (void)hci_send_event_cmd_complete(opcode, "12", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff);
        return 1;
    case 28:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_SUPPORTED_STATES\r\n");
        (void)hci_send_event_cmd_complete(opcode, "1c08", 0, le_param.support_states);
        return 1;
    case 32:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_REMOTE_CONNECTION_PARAMETER_REQUEST_REPLY\r\n");
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 2)) return 1;
        status = ll_push_control_data(opcode, data);
        (void)hci_send_event_cmd_complete(opcode, "12", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff);
        return 1;
    case 33:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_REMOTE_CONNECTION_PARAMETER_REQUEST_NEGATIVE_REPLY\r\n");
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 2)) return 1;
        status = ll_push_control_data(opcode, data);
        (void)hci_send_event_cmd_complete(opcode, "12", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff);
        return 1;
    case 34:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_DATA_LENGTH\r\n");
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 32)) return 1;
        status = ll_push_control_data(opcode, data);
        (void)hci_send_event_cmd_complete(opcode, "12", status,
            btctler_little_endian_read_16(data, 0) & 0x0fff);
        return 1;
    case 35:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_SUGGESTED_DEFAULT_DATA_LENGTH\r\n");
        if (!(config_btctler_le_features & 32)) return 1;
        struct ll_data_pdu_length *suggested = ll_read_suggested_default_data_length();
        (void)hci_send_event_cmd_complete(opcode, "122", 0,
            suggested->suggestedMaxTxOctets, suggested->suggestedMaxTxTime);
        return 1;
    case 36:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_WRITE_SUGGESTED_DEFAULT_DATA_LENGTH\r\n");
        if (!(config_btctler_le_features & 32)) return 1;
        ll_write_suggested_default_data_length(data);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 39:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_ADD_DEVICE_TO_RESOLVING_LIST\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_resolving_list_add(data);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 40:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_REMOVE_DEVICE_FROM_RESOLVING_LIST\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_resolving_list_remove(data);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 41:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_CLEAR_RESOLVING_LIST\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_resolving_list_init();
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 42:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_RESOLVING_LIST_SIZE\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        (void)hci_send_event_cmd_complete(opcode, "11", 0, le_param.resolving_list_size);
        return 1;
    case 43:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_PEER_RESOLVABLE_ADDRESS\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_read_peer_resolvable_address(*data, data + 1, peer_rpa);
        (void)hci_send_event_cmd_complete(opcode, "1A", 0, peer_rpa);
        return 1;
    case 44:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_LOCAL_RESOLVABLE_ADDRESS\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_read_local_resolvable_address(*data, data + 1, local_rpa);
        (void)hci_send_event_cmd_complete(opcode, "1A", 0, local_rpa);
        return 1;
    case 45:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_ADDRESS_RESOLUTION_ENABLE\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        le_param.resolution_enable = *data;
        ll_resolution_enable();
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 46:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_SET_RESOLVABLE_PRIVATE_ADDRESS_TIMEOUT\r\n");
        if (!(config_btctler_le_features & 64)) return 1;
        ll_set_resolvable_private_address_timeout(data);
        (void)hci_send_event_cmd_complete(opcode, "1", 0);
        return 1;
    case 47:
        HCI_LL_LOG("[Info]: [HCI LL]HCI_LE_READ_MAXIMUM_DATA_LENGTH\r\n");
        if (!(config_btctler_le_features & 32)) return 1;
        struct ll_data_pdu_length *maximum = ll_read_maximum_data_length();
        (void)hci_send_event_cmd_complete(opcode, "12222", 0,
            maximum->supportedMaxTxOctets, maximum->supportedMaxTxTime,
            maximum->supportedMaxRxOctets, maximum->supportedMaxRxTime);
        return 1;
    case 37:
        (void)hci_send_event(15, "112", 0, 1, opcode);
        /* The SDK body calls this symbol through its no-argument declaration,
         * while sdk.h exposes the ll_keys.c signature. Preserve the IR ABI. */
        ll_read_local_p256_public_key_ir();
        return 1;
    case 38:
        (void)hci_send_event(15, "112", 0, 1, opcode);
        ll_generate_dh_key_ir(data, size);
        return 1;
    default:
        status = le_controller_handler_for_ble5(opcode, data, size);
        if (status != 0) return status;
        switch (ocf) {
        case 29:
            if (config_btctler_mode & 4) le_receive_test_handle((u8 *)data, size);
            (void)hci_send_event_cmd_complete(opcode, "1", 0);
            return 1;
        case 30:
            if (config_btctler_mode & 4) le_transmitter_test_handle((struct dut_tx_param *)data, size);
            (void)hci_send_event_cmd_complete(opcode, "1", 0);
            return 1;
        case 31:
            if (config_btctler_mode & 4) {
                u16 result = le_test_end_handle();
                (void)hci_send_event_cmd_complete(opcode, "12", 0, result);
            }
            return 1;
        case 51:
            if (config_btctler_mode & 4) le_enhanced_receiver_test_handle((u8 *)data, size);
            (void)hci_send_event_cmd_complete(opcode, "1", 0);
            return 1;
        case 52:
            if (config_btctler_mode & 4) le_enhanced_transmitter_test_handle((u8 *)data, size);
            (void)hci_send_event_cmd_complete(opcode, "1", 0);
            return 1;
        default:
            log_print(4, (const char *)0, "<Error>: [HCI LL]le_hci_command_handler cmd not finish\r\n");
            return 0;
        }
    }
}

BLE_HCI_CODE int le_controller_init(void)
{
    HCI_LL_LOG("[Info]: [HCI LL]LE init - Controller\r\n");
    ll_init();
    hci_add_cmd_handler(&le_controller_registration);
    return 0;
}

BLE_HCI_CODE int le_controller_set_mac(void *addr)
{
    memcpy(le_param.public_address, addr, 6);
    return 0;
}

BLE_HCI_CODE int le_controller_get_mac(void *addr)
{
    memcpy(addr, le_param.public_address, 6);
    return 0;
}

BLE_HCI_CODE int le_controller_set_random_mac(void *addr)
{
    memcpy(le_param.random_address, addr, 6);
    return 0;
}

BLE_HCI_CODE int le_controller_get_random_mac(void *addr)
{
    memcpy(addr, le_param.random_address, 6);
    return 0;
}

static BLE_HCI_CODE int vendor_handler(u16 opcode, const u8 *data, u32 size)
{
    u16 debug_index;
    if ((opcode & 0x03ff) != 128) return 0;
    HCI_LL_LOG("[Info]: [HCI LL]HCI_SET_BLE_RF_DEBUG\r\n");
    memcpy(&debug_index, data, size);
    ll_set_rf_debug(debug_index);
    return 1;
}

#ifdef UNITTEST
#include "test.h"

int config_btctler_le_roles = 0;
int config_btctler_mode = 0;

static int test_event_count;
static int test_event_opcode;
static int test_event_args[5];
static int test_event_argc;
static int test_acl_handle;
static int test_acl_size;
static const char *test_event_format;
static struct hci_cmd_registration *test_registration;
static u16 test_debug_index;
static u32 test_log_value;
static int test_log_has_value;
static int test_log_level;
static char test_log_format[100];
static int test_control_count;
static int test_aes_count;
static u8 test_aes_output[16];
static u8 *test_aes_key;
static u8 *test_aes_plaintext;
static int test_ble5_return;
static int test_order[8];
static int test_order_count;
u64 config_btctler_le_features;

static void capture_event_args(const char *format, va_list args)
{
    test_event_argc = 0;
    if (strcmp(format, "121") == 0 || strcmp(format, "112") == 0) {
        for (int i = 0; i < 3; i++) test_event_args[test_event_argc++] = va_arg(args, int);
    } else if (strcmp(format, "12222") == 0) {
        for (int i = 0; i < 5; i++) test_event_args[test_event_argc++] = va_arg(args, int);
    } else if (strcmp(format, "12") == 0 || strcmp(format, "11") == 0) {
        for (int i = 0; i < 2; i++) test_event_args[test_event_argc++] = va_arg(args, int);
    } else if (strcmp(format, "1Hc05") == 0) {
        test_event_args[test_event_argc++] = va_arg(args, int);
        test_event_args[test_event_argc++] = va_arg(args, int);
        (void)va_arg(args, void *);
    } else if (strcmp(format, "1") == 0) {
        test_event_args[test_event_argc++] = va_arg(args, int);
    } else if (strcmp(format, "1c08") == 0 || strcmp(format, "1c16") == 0 ||
               strcmp(format, "1A") == 0) {
        test_event_args[test_event_argc++] = va_arg(args, int);
        void *payload = va_arg(args, void *);
        if (strcmp(format, "1c16") == 0) memcpy(test_aes_output, payload, sizeof(test_aes_output));
    } else if (strcmp(format, "122") == 0) {
        test_event_args[test_event_argc++] = va_arg(args, int);
        test_event_args[test_event_argc++] = va_arg(args, int);
        test_event_args[test_event_argc++] = va_arg(args, int);
    }
}

int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    test_event_count++;
    test_event_opcode = opcode;
    test_event_format = format;
    va_list args;
    va_start(args, format);
    capture_event_args(format, args);
    va_end(args);
    if (strcmp(format, "1") == 0 && test_order_count < 8) test_order[test_order_count++] = 3;
    return 0;
}

int hci_send_event(int opcode, const char *format, ...)
{
    test_event_count++;
    test_event_opcode = opcode;
    test_event_format = format;
    va_list args;
    va_start(args, format);
    capture_event_args(format, args);
    va_end(args);
    return 0;
}

void hci_add_cmd_handler(struct hci_cmd_registration *handler)
{
    test_registration = handler;
}

void ll_init(void)
{
    test_order[test_order_count++] = 1;
}

int ll_init_close(void)
{
    test_order[test_order_count++] = 2;
    return 0x105;
}

void destroy_le_ext_init(void)
{
    test_order[test_order_count++] = 0;
}

void le_connection_complete_event(struct le_link *link, u8 status)
{
    (void)link;
    (void)status;
    test_order[test_order_count++] = 4;
}

void ll_set_rf_debug(u16 debug_index)
{
    test_debug_index = debug_index;
}

u16 btctler_little_endian_read_16(const u8 *buffer, int pos)
{
    return buffer[pos] | (u16)buffer[pos + 1] << 8;
}

int le_controller_handler_for_ble5(u16 opcode, const u8 *data, u32 size)
{
    (void)opcode;
    (void)data;
    (void)size;
    return test_ble5_return;
}

void ll_send_acl_packet(int handle, const u8 *packet, int size)
{
    (void)packet;
    test_event_opcode = handle;
    test_acl_handle = handle;
    test_acl_size = size;
}

u8 ll_push_control_data(u16 opcode, const u8 *data)
{
    (void)opcode;
    (void)data;
    test_control_count++;
    return 0x5a;
}

void aes128_start(u8 *key, u8 *plaintext, u8 *encrypt)
{
    test_aes_key = key;
    test_aes_plaintext = plaintext;
    memset(encrypt, 0x3c, 16);
    test_aes_count++;
}

int ll_adv_set_params(const u8 *data)
{
    (void)data;
    return 0x1234;
}

void printf_buf(const u8 *buf, int size)
{
    (void)buf;
    (void)size;
}

void log_print(int level, const char *file, const char *format, ...)
{
    (void)file;
    test_log_level = level;
    strncpy(test_log_format, format, sizeof(test_log_format) - 1);
    test_log_format[sizeof(test_log_format) - 1] = '\0';
    if (strstr(format, "0x%x") != 0) {
        va_list args;
        va_start(args, format);
        test_log_value = (u32)va_arg(args, int);
        va_end(args);
        test_log_has_value = 1;
    }
}

int main(void)
{
    const u8 addr[6] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba };
    const u8 event_mask[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const u8 random[6] = { 1, 3, 5, 7, 9, 11 };
    u8 encrypt_data[32];
    for (unsigned i = 0; i < sizeof(encrypt_data); i++) encrypt_data[i] = (u8)i;
    u8 out[6] = { 0 };
    CHECK(le_controller_set_mac((void *)addr) == 0);
    CHECK(le_controller_get_mac(out) == 0 && memcmp(addr, out, 6) == 0);
    memset(out, 0, sizeof(out));
    CHECK(le_controller_set_random_mac((void *)addr) == 0);
    CHECK(le_controller_get_random_mac(out) == 0 && memcmp(addr, out, 6) == 0);

    test_event_count = 0;
    CHECK(hci_le_controller_cmd_handler(0x2001, event_mask, sizeof(event_mask)) == 1);
    CHECK(memcmp(le_param.event_mask, event_mask, sizeof(event_mask)) == 0);
    CHECK(test_event_count == 1 && test_event_opcode == 0x2001 && strcmp(test_event_format, "1") == 0);
    CHECK(test_event_argc == 1 && test_event_args[0] == 0);
    CHECK(strcmp(test_log_format, "[Info]: [HCI LL]HCI_LE_SET_EVENT_MASK\r\n") == 0);

    le_param.features[0] = 0x5a;
    test_log_has_value = 0;
    CHECK(hci_le_controller_cmd_handler(0x2003, event_mask, sizeof(event_mask)) == 1);
    CHECK(test_log_has_value && test_log_value == 0x5a);
    CHECK(strcmp(test_log_format, "[Info]: [HCI LL]HCI_LE_READ_LOCAL_SUPPORT_FEATURES : 0x%x\r\n") == 0);

    le_param.acl_data_packet_length = 0x1234;
    le_param.acl_packets_total_num = 0x56;
    CHECK(hci_le_controller_cmd_handler(0x2002, event_mask, sizeof(event_mask)) == 1);
    CHECK(strcmp(test_event_format, "121") == 0 && test_event_argc == 3);
    CHECK(test_event_args[0] == 0 && test_event_args[1] == 0x1234 && test_event_args[2] == 0x56);

    CHECK(hci_le_controller_cmd_handler(0x2006, event_mask, sizeof(event_mask)) == 1);
    CHECK(test_event_args[0] == 0x34); /* The SDK truncates helper status to one byte. */

    test_event_count = 0;
    CHECK(hci_le_controller_cmd_handler(0x2005, random, sizeof(random)) == 1);
    CHECK(test_event_count == 1 && test_event_opcode == 0x2005);
    CHECK(memcmp(le_param.random_address, random, 6) == 0);
    CHECK(hci_le_controller_cmd_handler(0x2017, random, sizeof(random)) == 1);
    CHECK(test_event_count == 1); /* encryption disabled: the SDK emits no event */

    test_aes_count = 0;
    config_btctler_le_features = 1;
    CHECK(hci_le_controller_cmd_handler(0x2017, encrypt_data, sizeof(encrypt_data)) == 1);
    CHECK(test_event_count == 2 && test_aes_count == 1);
    CHECK(test_aes_key == encrypt_data && test_aes_plaintext == encrypt_data + 16);
    for (unsigned i = 0; i < sizeof(test_aes_output); i++) CHECK(test_aes_output[i] == 0x3c);
    config_btctler_le_features = 0;

    test_control_count = 0;
    CHECK(hci_le_controller_cmd_handler(0x2013, random, sizeof(random)) == 1);
    CHECK(test_control_count == 0 && test_event_args[0] == 12);
    config_btctler_le_roles = 1;
    CHECK(hci_le_controller_cmd_handler(0x2013, random, sizeof(random)) == 1);
    CHECK(test_control_count == 1 && test_event_args[0] == 0x5a);
    config_btctler_le_roles = 0;

    test_event_count = 0;
    test_control_count = 0;
    config_btctler_le_roles = 1;
    CHECK(hci_le_controller_cmd_handler(0x2022, random, sizeof(random)) == 1);
    CHECK(test_event_count == 0 && test_control_count == 0); /* Data-length feature is disabled. */
    config_btctler_le_features = 32;
    CHECK(hci_le_controller_cmd_handler(0x2022, random, sizeof(random)) == 1);
    CHECK(test_event_count == 1 && test_control_count == 1);
    CHECK(test_event_args[0] == 0x5a && test_event_args[1] == 0x0301);
    config_btctler_le_features = 0;
    config_btctler_le_roles = 0;

    test_event_count = 0;
    test_order_count = 0;
    CHECK(hci_le_controller_cmd_handler(0x200e, random, sizeof(random)) == 1);
    CHECK(test_order_count == 4);
    CHECK(test_order[0] == 0 && test_order[1] == 2 && test_order[2] == 3 && test_order[3] == 4);
    CHECK(test_event_args[0] == 5); /* Cancel status is truncated after the event is sent. */

    test_event_count = 0;
    CHECK(hci_le_controller_cmd_handler(0x201f, random, sizeof(random)) == 1);
    CHECK(test_event_count == 0); /* SDK test-end path is silent when DUT mode is off. */
    test_ble5_return = 7;
    CHECK(hci_le_controller_cmd_handler(0x2400, random, sizeof(random)) == 7);
    CHECK(test_event_count == 0); /* Nonzero BLE 5 result passes through. */
    test_ble5_return = 0;
    CHECK(hci_le_controller_cmd_handler(0x2400, random, sizeof(random)) == 0);
    CHECK(test_log_level == 4 && strcmp(test_log_format, "<Error>: [HCI LL]le_hci_command_handler cmd not finish\r\n") == 0);

    u8 acl[4] = { 0xbc, 0xfa, 0x34, 0x12 };
    CHECK(ll_push_acl_data(acl, 0) == 0);
    CHECK(test_acl_handle == 0x0abc && test_acl_size == 0x1234);

    test_order_count = 0;
    CHECK(le_controller_init() == 0 && test_registration == &le_controller_registration);
    CHECK(test_order_count == 1 && test_order[0] == 1);
    CHECK(strcmp(test_log_format, "[Info]: [HCI LL]LE init - Controller\r\n") == 0);
    CHECK(test_registration->le_controller != 0 && test_registration->vendor != 0);
    u8 debug_index[2] = { 0x34, 0x12 };
    test_debug_index = 0;
    CHECK(test_registration->vendor(0x0080, debug_index, sizeof(debug_index)) == 1);
    CHECK(test_debug_index == 0x1234);
    CHECK(test_registration->vendor(0x0081, debug_index, sizeof(debug_index)) == 0);
    return TEST_DONE("btctrler_hci_ll_cmds");
}
#endif
