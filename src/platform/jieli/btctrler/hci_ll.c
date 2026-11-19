/* The SDK's BLE HCI command entry (hci_ll.c): ll_hci_cmd_handler receives the
 * LE controller commands the HCI task posts as an array of words (opcode
 * first, then one word per parameter), starts or stops advertising, scanning
 * and initiating, hands link-layer commands to the link tasks
 * (ll_push_control_data) and answers with HCI events.  Any opcode it does not
 * handle goes to le_controller_handler_for_ble5.  ble_update_target registers
 * ll_hci_destory with the SDK's update-target table (section .update_target).
 *
 * SDK quirks kept: the disconnect command (0x0406) is handled here only when
 * a role is enabled and otherwise falls through to the OCF switch (OCF 6 is
 * LE Set Advertising Parameters); the LE Set Scan Enable path passes a scan
 * parameter whose "enable" byte is never set (only filter_duplicates is);
 * parameter structures are uninitialised locals, as the SDK has them; several
 * commands ignore the role/feature gate (create connection, cancel, encrypt
 * RAND, connection update, P-256, DH key).  The SDK's
 * BD19 also retains the SDK's exported per-command ll_hci_* wrappers even
 * though the current BR23 app path does not call them. */
#include <string.h>
#include "btctrler/glue.h"

void log_print(int level, const char *file, const char *format, ...);
extern void ll_read_local_p256_public_key_ir(void) __asm__("ll_read_local_p256_public_key");
extern void ll_generate_dh_key_ir(const u8 *data, const u8 *size) __asm__("ll_generate_dh_key");
#ifdef UNITTEST
extern u8 log_tag_const_i_HCI_LL;
extern u64 config_btctler_le_features;
#else
extern const u8 log_tag_const_i_HCI_LL;
extern const u64 config_btctler_le_features;
#endif

#define HCI_LL_CODE __attribute__((section(".ble_hci_code")))
#define LL_LOG(...) do { if (log_tag_const_i_HCI_LL) log_print(2, (const char *)0, __VA_ARGS__); } while (0)
#define ARG(n) ((const u8 *)(uintptr_t)(u32)cmd[n])

HCI_LL_CODE void ll_hci_destory(void)
{
    ll_ext_destroy();
    ll_destroy();
}

const struct update_target ble_update_target __attribute__((section(".update_target"), used)) = {
    "ble", (update_handler_t)ll_hci_destory
};

HCI_LL_CODE void ll_event_handler(int *msg)
{
    (void)msg;
}

HCI_LL_CODE void ll_hci_cmd_handler(int *cmd)
{
    struct disconnect_paramter disconnect;
    u8 adv_param[15], adv_data[32], scan_resp_data[32];
    struct scan_enable_parameter scan_enable;
    u8 key[16], plaintext_data[16], enc[16], rand[8];
    struct long_term_key_request_reply_parameter ltk_reply;
    struct long_term_key_request_negative_reply_parameter ltk_negative;
    struct connection_update_parameter conn_update;
    struct test_key_num_parameter key_num;
    u16 hold_par[2];
    struct set_data_length_parameter data_length;
    struct set_phy_param phy_param;
    u16 opcode = (u16)cmd[0];
    u32 opcode32 = (u32)cmd[0] & 65535;

    if (opcode32 == 1030) {
        if ((config_btctler_le_roles & 3) != 0) {
            LL_LOG("[Info]: [HCI_LL]HCI_DISCONNECT 0x%x, 0x%x\r\n", cmd[1], cmd[2]);
            disconnect.connection_handle = (u16)cmd[1];
            disconnect.reason = (u8)cmd[2];
            int result = ll_push_control_data(opcode, (const u8 *)&disconnect);
            hci_send_event(15, "112", result, 1, 1030);
            return;
        }
    }
    switch (cmd[0] & 1023) {
    case 6:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISING_PARAMETERS\r\n");
        memcpy(adv_param, cmd + 1, 15);
        ll_adv_set_params(adv_param);
        return;
    case 8:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISING_DATA\r\n");
        memcpy(adv_data, cmd + 1, 32);
        ll_adv_set_adv_data(adv_data);
        return;
    case 9:
        if ((config_btctler_le_roles & 4) == 0) return;
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_SCAN_RESPONSE_DATA\r\n");
        memcpy(scan_resp_data, cmd + 1, 32);
        ll_adv_set_scan_rsp_data(scan_resp_data);
        return;
    case 10:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_ADVERTISE_ENABLE %s\r\n", cmd[1] != 0 ? "Enable" : "Disable");
        if (cmd[1] == 0) ll_adv_close();
        else ll_adv_open();
        return;
    case 12:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_SET_SCAN_ENABLE %s\r\n", cmd[1] != 0 ? "Enable" : "Disable");
        if (cmd[1] == 0) {
            ll_scan_close();
        } else {
            scan_enable.filter_duplicates = (u8)cmd[2];
            ll_scan_open(&scan_enable.enable);
        }
        return;
    case 13:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CREATE_CONNECTION\r\n");
        ll_init_open(ARG(1));
        return;
    case 14:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CREATE_CONNECTION_CANCEL\r\n");
        destroy_le_ext_init();
        ll_init_close();
        return;
    case 23:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_ENCRYPT\r\n");
        if ((config_btctler_le_features & 1) == 0) return;
        memcpy(key, cmd + 1, 16);
        memcpy(plaintext_data, cmd + 5, 16);
        aes128_start(key, plaintext_data, enc);
        hci_send_event_cmd_complete(8215, "1c16", 0, enc);
        return;
    case 24:
        LL_LOG("[Info]: [HCI_LL]HCI_LE_RAND\r\n");
        pseudo_random_genrate(rand, 8);
        hci_send_event_cmd_complete(opcode32, "1c08", 0, rand);
        return;
    case 25: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_START_ENCRYPT\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        int result = ll_push_control_data(opcode, ARG(1));
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 26: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_LONG_TERM_KEY_REQUEST_REPLY\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        ltk_reply.connection_handle = (u16)cmd[1];
        memcpy(ltk_reply.long_term_key, cmd + 2, 16);
        int result = ll_push_control_data(opcode, (const u8 *)&ltk_reply);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&ltk_reply, 0) & 4095);
        return;
    }
    case 27: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_LONG_TERM_KEY_REQUEST_NAGATIVE_REPLY\r\n");
        if ((config_btctler_le_roles & 3) == 0) return;
        if ((config_btctler_le_features & 1) == 0) return;
        ltk_negative.connection_handle = (u16)cmd[1];
        int result = ll_push_control_data(opcode, (const u8 *)&ltk_negative);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&ltk_negative, 0) & 4095);
        return;
    }
    case 19: {
        LL_LOG("[Info]: [HCI_LL]HCI_LE_CONNECTION_UPDATE\r\n");
        conn_update.connection_handle = (u16)cmd[1];
        conn_update.Conn_Interval_Min = (u16)cmd[2];
        conn_update.Conn_Interval_Max = (u16)cmd[3];
        conn_update.Conn_Latency = (u16)cmd[4];
        conn_update.Supervision_Timeout = (u16)cmd[5];
        conn_update.Minimum_CE_Length = (u16)cmd[6];
        conn_update.Maximum_CE_Length = (u16)cmd[7];
        int result = ll_push_control_data(opcode, (const u8 *)&conn_update);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 37:
        hci_send_event(15, "112", 0, 1, opcode32);
        ll_read_local_p256_public_key_ir();
        return;
    case 38:
        hci_send_event(15, "112", 0, 1, opcode32);
        ll_generate_dh_key_ir(ARG(1), ARG(2));
        return;
    case 131: {
        if ((config_btctler_le_roles & 3) == 0) return;
        key_num.connection_handle = (u16)cmd[1];
        key_num.key_num = (u8)cmd[2];
        int result = ll_push_control_data(opcode, (const u8 *)&key_num);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 132: {
        if ((config_btctler_le_roles & 3) == 0) return;
        hold_par[0] = (u16)cmd[1];
        hold_par[1] = (u16)cmd[2];
        int result = ll_push_control_data(opcode, (const u8 *)hold_par);
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 34: {
        if ((config_btctler_le_features & 32) == 0) return;
        data_length.connection_handle = (u16)cmd[1];
        data_length.txoctets = (u16)cmd[2];
        data_length.txtime = (u16)cmd[3];
        int result = ll_push_control_data(opcode, (const u8 *)&data_length);
        hci_send_event_cmd_complete(opcode32, "12", result, btctler_little_endian_read_16((const u8 *)&data_length, 0) & 4095);
        return;
    }
    case 20: {
        int result = ll_push_control_data(opcode, ARG(1));
        hci_send_event(15, "112", result, 1, opcode32);
        return;
    }
    case 50:
        if ((config_btctler_le_features & 2304) == 0) return;
        phy_param.Connection_Handle = (u16)cmd[1];
        phy_param.ALL_PHYs = (u8)cmd[2];
        phy_param.TX_PHYs = (u8)cmd[3];
        phy_param.RX_PHYs = (u8)cmd[4];
        phy_param.PHY_Options = (u16)cmd[5];
        le_controller_handler_for_ble5(opcode, (const u8 *)&phy_param, 7);
        return;
    default:
        le_controller_handler_for_ble5(opcode, ARG(1), (u32)cmd[2]);
        return;
    }
}

#ifdef CONFIG_CPU_BD19
/* These private objects are part of hci_ll.c.o's BD19 data layout. */
static hci_ll_param_t config_hci_ll_param
    __attribute__((section(".ble_hci_data"), aligned(1)));
static u32 hci_cmd_tmp_buff[32]
    __attribute__((section(".ble_hci_bss"), aligned(4)));
static const struct connection_parameter create_param_default
    __attribute__((section(".ble_hci_const"), aligned(1))) = {
        .le_scan_interval = 24,
        .le_scan_window = 8,
        .initiator_filter_policy = 0,
        .peer_address_type = 0,
        .peer_address = { 0 },
        .own_address_type = 0,
        .conn_interval_min = 6,
        .conn_interval_max = 32,
        .conn_latency = 0,
        .supervision_timeout = 384,
        .minimum_ce_length = 1,
        .maximum_ce_length = 1,
    };
char g_le_name[18] __attribute__((section(".ble_hci_bss"), aligned(1)));

extern int config_btctler_le_afh_en;
extern int CONFIG_WIFI_DETECT_ENABLE;
#ifdef UNITTEST
extern u8 log_tag_const_d_HCI_LL, log_tag_const_e_HCI_LL;
#else
extern const u8 log_tag_const_d_HCI_LL, log_tag_const_e_HCI_LL;
#endif
extern int ll_scan_set_params(const u8 *data);
extern int le_controller_get_mac(void *addr);
extern int le_controller_get_random_mac(void *addr);
extern struct le_link *ll_link_for_handle(int handle);
extern void hw_dma_free(void *dma);

typedef struct __attribute__((packed, may_alias)) { u32 value; } hci_ll_u32_packed;
typedef struct __attribute__((packed, may_alias)) { u64 value:48; } hci_ll_u48_packed;
typedef struct __attribute__((packed, may_alias)) { u64 value; } hci_ll_u64_packed;
typedef u64 hci_ll_u64_align1 __attribute__((aligned(1), may_alias));
typedef u64 hci_ll_u64_align4 __attribute__((aligned(4), may_alias));

static __attribute__((always_inline)) inline u32 hci_ll_load_u32(const void *ptr)
{
    const hci_ll_u32_packed *unaligned = ptr;
    return unaligned->value;
}

static __attribute__((always_inline)) inline u64 hci_ll_load_u48(const void *ptr)
{
    const hci_ll_u48_packed *unaligned = ptr;
    return unaligned->value;
}
typedef u32 hci_ll_u32_align2 __attribute__((aligned(2), may_alias));

static __attribute__((always_inline)) inline void hci_ll_store_u32_unaligned(void *ptr, u32 value)
{
    hci_ll_u32_packed *unaligned = ptr;
    unaligned->value = value;
}

static __attribute__((always_inline)) inline void hci_ll_store_u64_unaligned(void *ptr, u64 value)
{
    hci_ll_u64_packed *unaligned = ptr;
    unaligned->value = value;
}

HCI_LL_CODE void hci_ll_get_device_address(u8 *addr_type, u8 *addr)
{
    u8 address_type = config_hci_ll_param.Own_Address_Type;
    if ((address_type & 3) == 0) le_controller_get_mac(addr);
    else le_controller_get_random_mac(addr);
    *addr_type = config_hci_ll_param.Own_Address_Type & 3;
}

HCI_LL_CODE void hci_ll_get_link_local_address(u16 handle, u8 *addr_type, u8 *addr)
{
    if (handle == 0) {
        hci_ll_get_device_address(addr_type, addr);
        return;
    }
    struct le_link *link = ll_link_for_handle(handle);
    if (!link) {
        hci_ll_get_device_address(addr_type, addr);
        return;
    }
    *addr_type = link->local.addr_type;
    __builtin_memcpy(addr, link->local.addr, 6);
}

HCI_LL_CODE void ll_hci_adv_set_params(u16 interval_min, u16 interval_max,
                                       u8 adv_type, u8 direct_address_type,
                                       u8 *direct_address, u8 channel_map,
                                       u8 filter_policy)
{
    (void)filter_policy;
    u32 params0 = ((u32)interval_max << 16) | interval_min;
    u32 params1 = ((u32)direct_address[0] << 24) |
                  ((u32)direct_address_type << 16) |
                  ((u32)(config_hci_ll_param.Own_Address_Type & 3) << 8) |
                  adv_type;
    u32 address0 = hci_ll_load_u32(direct_address + 1);
    u32 address1 = (u32)direct_address[5] | ((u32)channel_map << 8) |
                   ((u32)(config_hci_ll_param.Adv_Filter_Policy & 3) << 16);
    btctrler_hci_cmd_to_task(4194310, 6, 8198, params0, params1, address0, address1);
}

HCI_LL_CODE void ll_hci_adv_set_data(u8 length, u8 *data)
{
    u32 params[8];
    __builtin_memset(params, 0, sizeof(params));
    params[0] = length;
    __builtin_memcpy((u8 *)params + 1, data, length);
    btctrler_hci_cmd_to_task(4194310, 10, 8200,
        params[0], params[1], params[2], params[3], params[4], params[5], params[6], params[7]);
}

HCI_LL_CODE void ll_hci_adv_scan_response_set_data(u8 length, u8 *data)
{
    u8 scan_response[31];
    __builtin_memcpy(scan_response, data, length);
    u32 first3 = (u32)scan_response[0] | ((u32)scan_response[1] << 8) |
                 ((u32)scan_response[2] << 16);
    u32 packed_length = (first3 << 8) | length;
    btctrler_hci_cmd_to_task(4194310, 10, 8201, packed_length,
        hci_ll_load_u32(scan_response + 3), hci_ll_load_u32(scan_response + 7),
        hci_ll_load_u32(scan_response + 11), hci_ll_load_u32(scan_response + 15),
        hci_ll_load_u32(scan_response + 19), hci_ll_load_u32(scan_response + 23),
        hci_ll_load_u32(scan_response + 27));
}

HCI_LL_CODE int ll_hci_adv_enable_sdk(u8 enable) __asm__("ll_hci_adv_enable");
HCI_LL_CODE int ll_hci_adv_enable_sdk(u8 enable)
{
    btctrler_hci_cmd_to_task(4194310, 2, 8202, (int)enable);
    return 0;
}

HCI_LL_CODE int ll_hci_scan_enable_sdk(u8 enable, u8 filter_duplicates) __asm__("ll_hci_scan_enable");
HCI_LL_CODE int ll_hci_scan_enable_sdk(u8 enable, u8 filter_duplicates)
{
    btctrler_hci_cmd_to_task(4194310, 3, 8204, enable, filter_duplicates);
    return 0;
}

HCI_LL_CODE int ll_hci_create_conn(u8 *conn_param, u8 *addr_param)
{
    u8 peer[7];
    __builtin_memcpy(peer, (const u8 *)&create_param_default + 5, sizeof(peer));
    u8 own_policy = *(u8 *)&config_hci_ll_param;
    u64 conn_words = (hci_ll_load_u48(conn_param) << 16) | 6;
    __builtin_memcpy(peer, addr_param, sizeof(peer));
    u8 *buffer = (u8 *)hci_cmd_tmp_buff;
    *(u16 *)(void *)(buffer + 0) = 24;
    *(u16 *)(void *)(buffer + 2) = 8;
    buffer[4] = own_policy >> 6;
    __builtin_memcpy(buffer + 5, peer, sizeof(peer));
    buffer[12] = own_policy & 3;
    hci_ll_store_u64_unaligned(buffer + 13, conn_words);
    hci_ll_store_u32_unaligned(buffer + 21, 65537);
    btctrler_hci_cmd_to_task(4194310, 2, 8205, hci_cmd_tmp_buff);
    return 0;
}

HCI_LL_CODE int ll_hci_create_conn_ext(void *param)
{
    __builtin_memcpy(hci_cmd_tmp_buff, param, 25);
    btctrler_hci_cmd_to_task(4194310, 2, 8205, hci_cmd_tmp_buff);
    return 0;
}

HCI_LL_CODE int ll_hci_create_conn_cancel(void)
{
    btctrler_hci_cmd_to_task(4194310, 1, 8206);
    return 0;
}

HCI_LL_CODE int ll_hci_disconnect(u16 handle, u8 reason)
{
    btctrler_hci_cmd_to_task(4194310, 3, 1030, handle, reason);
    return 0;
}

HCI_LL_CODE int ll_hci_connection_update(u16 handle, u16 interval_min,
                                         u16 interval_max, u16 latency,
                                         u16 timeout, u16 minimum_ce,
                                         u16 maximum_ce)
{
    btctrler_hci_cmd_to_task(4194310, 8, 8211, handle, interval_min,
        interval_max, latency, timeout, minimum_ce, maximum_ce);
    return 0;
}

HCI_LL_CODE int ll_hci_encryption(u8 *key, u8 *plaintext)
{
    btctrler_hci_cmd_to_task(4194310, 9, 8215,
        hci_ll_load_u32(key), hci_ll_load_u32(key + 4),
        hci_ll_load_u32(key + 8), hci_ll_load_u32(key + 12),
        hci_ll_load_u32(plaintext), hci_ll_load_u32(plaintext + 4),
        hci_ll_load_u32(plaintext + 8), hci_ll_load_u32(plaintext + 12));
    return 0;
}

HCI_LL_CODE int ll_hci_get_le_rand(void)
{
    btctrler_hci_cmd_to_task(4194310, 1, 8216);
    return 0;
}

HCI_LL_CODE u16 ll_hci_get_acl_data_len(void) { return le_param.acl_data_packet_length; }
HCI_LL_CODE u16 ll_hci_get_acl_total_num(void) { return le_param.acl_packets_total_num; }

HCI_LL_CODE u64 ll_hci_get_remote_features(void)
{
    u64 value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (u64)le_param.remote_features[i] << (8 * i);
    return value;
}

HCI_LL_CODE void ll_hci_init(void) { ll_init(); }
HCI_LL_CODE void ll_hci_reset(void) { ll_reset(); }

HCI_LL_CODE int ll_hci_long_term_key_request_reply(u16 handle, u8 *ltk)
{
    btctrler_hci_cmd_to_task(4194310, 6, 8218, handle,
        hci_ll_load_u32(ltk), hci_ll_load_u32(ltk + 4),
        hci_ll_load_u32(ltk + 8), hci_ll_load_u32(ltk + 12));
    return 0;
}

HCI_LL_CODE int ll_hci_long_term_key_request_nagative_reply(u16 handle)
{
    btctrler_hci_cmd_to_task(4194310, 2, 8219, handle);
    return 0;
}

HCI_LL_CODE hci_ll_param_t *ll_hci_param_config_get(void) { return &config_hci_ll_param; }

HCI_LL_CODE void ll_hci_periodic_adv_creat_sync(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 3, 8260, data, size);
}

HCI_LL_CODE void ll_hci_periodic_adv_create_sync_cancel(void)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 1, 8261);
}

HCI_LL_CODE void ll_hci_periodic_adv_terminate_sync(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 3, 8262, data, size);
}

HCI_LL_CODE void ll_hci_private_free_dma_rx(u8 *rx_head)
{
    hw_dma_free(rx_head - 16);
}

HCI_LL_CODE void ll_hci_read_channel_map(u16 handle)
{
    btctrler_hci_cmd_to_task(4194310, 2, 8213, handle);
}

HCI_LL_CODE int ll_hci_read_local_p256_pb_key(void)
{
    btctrler_hci_cmd_to_task(4194310, 1, 8229);
    return 0;
}

HCI_LL_CODE int ll_hci_generate_dhkey(const u8 *data, u32 size)
{
    btctrler_hci_cmd_to_task(4194310, 3, 8230, data, size);
    return 0;
}

HCI_LL_CODE void ll_hci_scan_set_params(u8 scan_type, u16 scan_interval, u16 scan_window)
{
    struct scan_parameter param;
    param.LE_Scan_Type = scan_type;
    param.LE_Scan_Interval = scan_interval;
    param.LE_Scan_Window = scan_window;
    param.Own_Address_Type = config_hci_ll_param.Own_Address_Type;
    param.Scanning_Filter_Policy = config_hci_ll_param.Scan_Filter_Policy;
    ll_scan_set_params((const u8 *)&param);
}

HCI_LL_CODE void ll_hci_set_data_length(u16 handle, u16 tx_octets, u16 tx_time)
{
    if (config_btctler_le_features & 32)
        btctrler_hci_cmd_to_task(4194310, 4, 8226, handle, tx_octets, tx_time);
}

HCI_LL_CODE void ll_hci_set_event_mask(const u8 *mask)
{
    *(hci_ll_u64_align4 *)(void *)&le_param = *(const hci_ll_u64_align1 *)(const void *)mask;
}

HCI_LL_CODE void ll_hci_set_ext_adv_data(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8247, data, size);
}

HCI_LL_CODE void ll_hci_set_ext_adv_enable(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8249, data, size);
}

HCI_LL_CODE void ll_hci_set_ext_adv_params(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8246, data, size);
}

HCI_LL_CODE void ll_hci_set_ext_scan_enable(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8258, data, size);
}

HCI_LL_CODE void ll_hci_set_ext_scan_params(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8257, data, size);
}

HCI_LL_CODE void ll_hci_ext_create_conn(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 1) && (config_btctler_le_features & 4096))
        btctrler_hci_cmd_to_task(4194310, 3, 8259, data, size);
}

HCI_LL_CODE void ll_hci_set_periodic_adv_data(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 3, 8255, data, size);
}

HCI_LL_CODE void ll_hci_set_periodic_adv_enable(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 3, 8256, data, size);
}

HCI_LL_CODE void ll_hci_set_periodic_adv_params(u8 *data, u32 size)
{
    if ((config_btctler_le_roles & 2) && (config_btctler_le_features & 12288) == 12288)
        btctrler_hci_cmd_to_task(4194310, 3, 8254, data, size);
}

HCI_LL_CODE void ll_hci_set_phy(u16 handle, u8 all_phys, u8 tx_phy, u8 rx_phy, u16 options)
{
    if (config_btctler_le_features & 2304)
        btctrler_hci_cmd_to_task(4194310, 6, 8242, handle, all_phys, tx_phy, rx_phy, options);
}

HCI_LL_CODE void ll_hci_set_random_address(u8 *address)
{
    __builtin_memcpy(le_param.random_address, address, 6);
}

HCI_LL_CODE int ll_hci_start_encryption(u16 handle, u32 rand_low, u32 rand_high,
                                        u16 ediv, u8 *ltk)
{
    struct start_encryption_parameter *param = (struct start_encryption_parameter *)hci_cmd_tmp_buff;
    param->connection_handle = handle;
    *(hci_ll_u32_align2 *)(void *)&param->random_number[0] = rand_low;
    *(hci_ll_u32_align2 *)(void *)&param->random_number[4] = rand_high;
    *(u16 *)(void *)param->encrypted_diversifier = ediv;
    __builtin_memcpy(param->long_term_key, ltk, 16);
    btctrler_hci_cmd_to_task(4194310, 2, 8217, param);
    return 0;
}

HCI_LL_CODE int ll_hci_vendor_send_key_num(u16 handle, u8 num)
{
    btctrler_hci_cmd_to_task(4194310, 3, 63619, handle, num);
    return 0;
}

HCI_LL_CODE int ll_vendor_latency_hold_cnt(u16 handle, u16 hold_cnt)
{
    btctrler_hci_cmd_to_task(4194310, 3, 63620, handle, hold_cnt);
    return 0;
}

HCI_LL_CODE void ll_hci_set_host_channel_classification(u8 *channel_map)
{
    if ((config_btctler_le_roles & 1) == 0) return;
    __builtin_memcpy(hci_cmd_tmp_buff, channel_map, 5);
    if (config_btctler_le_afh_en) {
        if (log_tag_const_e_HCI_LL)
            log_print(4, NULL, "<Error>: [HCI_LL]confilct afh, need disable afh!!!\n\r\n");
        return;
    }
    if (CONFIG_WIFI_DETECT_ENABLE) {
        if (log_tag_const_e_HCI_LL)
            log_print(4, NULL, "<Error>: [HCI_LL]confilct wifi_dectect, need disable wifi_dectect!!!\n\r\n");
        return;
    }
    btctrler_hci_cmd_to_task(4194310, 2, 8212, hci_cmd_tmp_buff);
}

HCI_LL_CODE void ll_hci_set_name(const char *name)
{
    u32 length = ((u32)strlen(name) + 1) & 65535;
    __builtin_memcpy(g_le_name, name, length);
    if (log_tag_const_d_HCI_LL)
        log_print(1, NULL, "[Debug]: [HCI_LL]local_name %d / %s\n\r\n", (int)length, name);
}
#endif

#ifdef UNITTEST
#include <stdarg.h>
#include <stdio.h>
#include "test.h"

int config_btctler_le_roles;
u64 config_btctler_le_features;
u8 log_tag_const_i_HCI_LL;
#ifdef CONFIG_CPU_BD19
u8 log_tag_const_d_HCI_LL, log_tag_const_e_HCI_LL;
struct le_parameter le_param;
static u32 test_task_opcode;
static u32 test_task_argc;
static u32 test_task_args[10];
static u8 test_task_payload[32];
static u8 test_task_payload_size;
static int mutate_device_address_type;
static u8 device_address_type_after_callback;
int config_btctler_le_afh_en, CONFIG_WIFI_DETECT_ENABLE;
static u8 test_scan_params[7];
int btctrler_hci_cmd_to_task(int task, int argc, ...)
{
    va_list ap;
    (void)task;
    test_task_argc = (u32)argc;
    test_task_opcode = 0;
    test_task_payload_size = 0;
    memset(test_task_args, 0, sizeof(test_task_args));
    va_start(ap, argc);
    for (int i = 0; i < argc; ++i) {
        uintptr_t value;
        int pointer_arg = i == argc - 1 &&
            (test_task_opcode == 8205 || test_task_opcode == 8212 || test_task_opcode == 8217);
        if (pointer_arg) value = (uintptr_t)va_arg(ap, const void *);
        else value = va_arg(ap, unsigned int);
        if (i == 0) test_task_opcode = (u32)value;
        else if (i <= 9) test_task_args[i - 1] = (u32)value;
        if (i == argc - 1 && (test_task_opcode == 8205 || test_task_opcode == 8212 ||
                              test_task_opcode == 8217)) {
            test_task_payload_size = test_task_opcode == 8212 ? 5 :
                                     test_task_opcode == 8217 ? 28 : 25;
            memcpy(test_task_payload, (const void *)value, test_task_payload_size);
        }
    }
    va_end(ap);
    return 0;
}
int ll_scan_set_params(const u8 *data) { memcpy(test_scan_params, data, 7); return 0; }
int le_controller_get_mac(void *addr)
{
    memset(addr, 0x11, 6);
    if (mutate_device_address_type) {
        config_hci_ll_param.Own_Address_Type = device_address_type_after_callback;
        mutate_device_address_type = 0;
    }
    return 0;
}
int le_controller_get_random_mac(void *addr)
{
    memset(addr, 0x22, 6);
    if (mutate_device_address_type) {
        config_hci_ll_param.Own_Address_Type = device_address_type_after_callback;
        mutate_device_address_type = 0;
    }
    return 0;
}
struct le_link *ll_link_for_handle(int handle) { (void)handle; return NULL; }
void hw_dma_free(void *dma) { (void)dma; }
#endif

static char trace[2048];
static void T(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    size_t n = strlen(trace);
    vsnprintf(trace + n, sizeof(trace) - n, format, ap);
    va_end(ap);
}
#define EXPECT(s) do { if (strcmp(trace, (s))) { fprintf(stderr, "trace:    %s\nexpected: %s\n", trace, (s)); CHECK(0); } trace[0] = 0; } while (0)

static u8 push_result = 7;
static u8 pushed[32];
void log_print(int level, const char *file, const char *format, ...) { (void)file; T("log%d(%s);", level, format); }
void ll_ext_destroy(void) { T("ext_destroy;"); }
void ll_destroy(void) { T("destroy;"); }
u8 ll_push_control_data(u16 opcode, const u8 *param)
{
    static const struct { u16 opcode; int size; } sizes[] = {
        { 0x0406, 3 }, { 0x201a, 18 }, { 0x201b, 2 }, { 0x2013, 14 }, { 0x2022, 6 }, { 0xfc83, 3 }, { 0xfc84, 4 },
    };
    T("push(%x);", opcode);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (sizes[i].opcode == opcode) memcpy(pushed, param, sizes[i].size);
    return push_result;
}
int hci_send_event(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int a = va_arg(ap, int), b = va_arg(ap, int), c = va_arg(ap, int);
    T("event(%d,%s,%d,%d,%d);", opcode, format, a, b, c);
    va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    int a = va_arg(ap, int);
    if (!strcmp(format, "12")) T("complete(%x,%s,%d,%d);", opcode, format, a, va_arg(ap, int));
    else { const u8 *p = va_arg(ap, const u8 *); T("complete(%x,%s,%d,%02x%02x);", opcode, format, a, p[0], p[1]); }
    va_end(ap); return 0;
}
int ll_adv_set_params(const u8 *d) { T("adv_params(%d,%d);", d[0], d[14]); return 0; }
int ll_adv_set_adv_data(const u8 *d) { T("adv_data(%d,%d);", d[0], d[31]); return 0; }
int ll_adv_set_scan_rsp_data(const u8 *d) { T("scan_rsp(%d,%d);", d[0], d[31]); return 0; }
int ll_adv_open(void) { T("adv_open;"); return 0; }
int ll_adv_close(void) { T("adv_close;"); return 0; }
static u8 scan_filter;
int ll_scan_open(const u8 *d) { scan_filter = d[1]; T("scan_open;"); return 0; }
int ll_scan_close(void) { T("scan_close;"); return 0; }
int ll_init_open(const u8 *d) { T("init_open(%lx);", (unsigned long)(uintptr_t)d); return 0; }
int ll_init_close(void) { T("init_close;"); return 0; }
void destroy_le_ext_init(void) { T("destroy_ext_init;"); }
void aes128_start(u8 *key, u8 *plain, u8 *out) { T("aes(%d,%d);", key[0], plain[0]); memset(out, 0xa5, 16); out[0] = key[15]; out[1] = plain[15]; }
void pseudo_random_genrate(uint8_t *dest, unsigned int size) { T("rand(%u);", size); memset(dest, 0x5a, size); }
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return b[pos + 1] << 8 | b[pos]; }
int le_controller_handler_for_ble5(u16 opcode, const u8 *data, u32 size)
{
    if (size == 7) T("ble5(%x,7);phy(%02x%02x%02x%02x%02x%02x%02x);", opcode, data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
    else T("ble5(%x,%lx,%u);", opcode, (unsigned long)(uintptr_t)data, size);
    return 0;
}
void p256_stub(void) __asm__("ll_read_local_p256_public_key");
void p256_stub(void) { T("p256;"); }
void dhkey_stub(const u8 *a, const u8 *b) __asm__("ll_generate_dh_key");
void dhkey_stub(const u8 *a, const u8 *b) { T("dhkey(%lx,%lx);", (unsigned long)(uintptr_t)a, (unsigned long)(uintptr_t)b); }

static void run(int *cmd) { trace[0] = 0; ll_hci_cmd_handler(cmd); }

int main(void)
{
    /* The update target and the shutdown hook. */
    CHECK(!strcmp(ble_update_target.name, "ble"));
    CHECK(ble_update_target.driver_close == (update_handler_t)ll_hci_destory);
    trace[0] = 0; ll_hci_destory(); EXPECT("ext_destroy;destroy;");
    int msg[2] = { 1, 2 }; ll_event_handler(msg); EXPECT("");

    config_btctler_le_roles = 7; config_btctler_le_features = ~0ull;
    int cmd[10];

#ifdef CONFIG_CPU_BD19
    /* Wrapper vectors are derived from the raw BD19 command stores and loads. */
    hci_ll_param_t *params = ll_hci_param_config_get();
    params->Own_Address_Type = 2;
    params->Adv_Filter_Policy = 1;
    u8 address_type, address[6];
    params->Own_Address_Type = 0;
    device_address_type_after_callback = 3; mutate_device_address_type = 1;
    hci_ll_get_device_address(&address_type, address);
    CHECK(address_type == 3 && address[0] == 0x11);
    params->Own_Address_Type = 1;
    device_address_type_after_callback = 2; mutate_device_address_type = 1;
    hci_ll_get_device_address(&address_type, address);
    CHECK(address_type == 2 && address[0] == 0x22);
    params->Own_Address_Type = 2;
    u8 direct[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    ll_hci_adv_set_params(0x1234, 0x5678, 3, 1, direct, 7, 9);
    CHECK(test_task_opcode == 8198 && test_task_argc == 6);
    CHECK(test_task_args[0] == 0x56781234 && test_task_args[1] == 0x10010203);
    CHECK(test_task_args[2] == 0x50403020 && test_task_args[3] == 0x00010760);
    CHECK(ll_hci_adv_enable_sdk(0xff) == 0 && test_task_opcode == 8202 && test_task_args[0] == 255);
    CHECK(ll_hci_scan_enable_sdk(1, 2) == 0 && test_task_opcode == 8204 &&
          test_task_args[0] == 1 && test_task_args[1] == 2);
    u8 adv_data[31];
    for (unsigned i = 0; i < sizeof(adv_data); ++i) adv_data[i] = (u8)(i + 1);
    ll_hci_adv_set_data(sizeof(adv_data), adv_data);
    static const u32 adv_words[8] = {
        0x0302011f, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c
    };
    CHECK(test_task_opcode == 8200 && test_task_argc == 10);
    CHECK(memcmp(test_task_args, adv_words, sizeof(adv_words)) == 0);

    u8 conn[6] = { 1, 2, 3, 4, 5, 6 }, peer[7] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6 };
    params->Own_Address_Type = 0;
    CHECK(ll_hci_create_conn(conn, peer) == 0 && test_task_opcode == 8205 && test_task_payload_size == 25);
    static const u8 conn_expected[25] = {
        24, 0, 8, 0, 0, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0,
        6, 0, 1, 2, 3, 4, 5, 6, 1, 0, 1, 0
    };
    CHECK(memcmp(test_task_payload, conn_expected, sizeof(conn_expected)) == 0);

    u8 ltk[16]; for (unsigned i = 0; i < sizeof(ltk); ++i) ltk[i] = (u8)(0x80 + i);
    CHECK(ll_hci_start_encryption(0x1234, 0x11223344, 0x55667788, 0x99aa, ltk) == 0);
    static const u8 encrypt_expected[28] = {
        0x34, 0x12, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
        0xaa, 0x99, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f
    };
    CHECK(test_task_opcode == 8217 && test_task_payload_size == sizeof(encrypt_expected));
    CHECK(memcmp(test_task_payload, encrypt_expected, sizeof(encrypt_expected)) == 0);

    ll_hci_scan_set_params(1, 0x1234, 0x5678);
    static const u8 scan_expected[7] = { 1, 0x34, 0x12, 0x78, 0x56, 0, 0 };
    CHECK(memcmp(test_scan_params, scan_expected, sizeof(scan_expected)) == 0);
    u8 event_mask_store[9] = { 0, 1, 3, 5, 7, 9, 11, 13, 15 };
    ll_hci_set_event_mask(event_mask_store + 1);
    CHECK(memcmp(&le_param, event_mask_store + 1, 8) == 0);
    u8 *le_bytes = (u8 *)&le_param;
    for (unsigned i = 0; i < 9; ++i) le_bytes[i] = (u8)(0x40 + i);
    ll_hci_set_event_mask(le_bytes + 1);
    static const u8 overlap_expected[8] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48 };
    CHECK(memcmp(le_bytes, overlap_expected, sizeof(overlap_expected)) == 0);
#endif

    /* Disconnect (0x0406): a link-layer push and the event with the constant opcode.  Without a role bit
     * (roles & 3 == 0) the command falls through to the OCF switch: OCF 6 is advertising parameters. */
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x0406; cmd[1] = 0x1234; cmd[2] = 0x13;
    run(cmd); EXPECT("push(406);event(15,112,7,1,1030);");
    CHECK(pushed[0] == 0x34 && pushed[1] == 0x12 && pushed[2] == 0x13);
    config_btctler_le_roles = 4; run(cmd); EXPECT("adv_params(52,0);");
    config_btctler_le_roles = 0; run(cmd); EXPECT("");
    config_btctler_le_roles = 7;
    log_tag_const_i_HCI_LL = 1; run(cmd);
    EXPECT("log2([Info]: [HCI_LL]HCI_DISCONNECT 0x%x, 0x%x\r\n);push(406);event(15,112,7,1,1030);");
    log_tag_const_i_HCI_LL = 0;

    /* Advertising parameters / data / scan response: 15 and 32 bytes copied from cmd + 1; role bit 4. */
    int words[20];
    memset(words, 0, sizeof(words));
    memcpy(words + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62);
    words[0] = 0x2006; run(words); EXPECT("adv_params(65,79);");
    words[0] = 0x2008; run(words); EXPECT("adv_data(65,102);");
    words[0] = 0x2009; run(words); EXPECT("scan_rsp(65,102);");
    config_btctler_le_roles = 3;
    words[0] = 0x2006; run(words); EXPECT("");
    words[0] = 0x2008; run(words); EXPECT("");
    words[0] = 0x2009; run(words); EXPECT("");
    config_btctler_le_roles = 0;
    /* Advertise / scan enable are not gated. */
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x200a; cmd[1] = 1; run(cmd); EXPECT("adv_open;");
    cmd[1] = 0; run(cmd); EXPECT("adv_close;");
    cmd[0] = 0x200c; cmd[1] = 1; cmd[2] = 0x1ff; run(cmd); EXPECT("scan_open;"); CHECK(scan_filter == 0xff);
    cmd[1] = 0; run(cmd); EXPECT("scan_close;");
    log_tag_const_i_HCI_LL = 1;
    cmd[0] = 0x200a; cmd[1] = 5; run(cmd); EXPECT("log2([Info]: [HCI_LL]HCI_LE_SET_ADVERTISE_ENABLE %s\r\n);adv_open;");
    log_tag_const_i_HCI_LL = 0;
    /* Create connection (a pointer in cmd[1]) and cancel. */
    cmd[0] = 0x200d; cmd[1] = 0x1000; run(cmd); EXPECT("init_open(1000);");
    cmd[0] = 0x200e; run(cmd); EXPECT("destroy_ext_init;init_close;");

    /* LE Encrypt: feature bit 0; key = cmd + 1 (16 bytes), plaintext = cmd + 5; reply opcode constant 0x2017. */
    config_btctler_le_features = 1;
    memset(words, 0, sizeof(words));
    for (int i = 0; i < 32; i++) ((u8 *)(words + 1))[i] = i + 1;
    words[0] = 0x2017; run(words); EXPECT("aes(1,17);complete(2017,1c16,0,1020);");
    config_btctler_le_features = 0; run(words); EXPECT("");
    /* LE Rand: eight random bytes, reply opcode from the command. */
    words[0] = 0x2018; run(words); EXPECT("rand(8);complete(2018,1c08,0,5a5a);");

    /* Commands that need a role and the encryption feature. */
    config_btctler_le_roles = 1; config_btctler_le_features = 1;
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 0x2019; cmd[1] = 0x7;
    run(cmd); EXPECT("push(2019);event(15,112,7,1,8217);");
    config_btctler_le_features = 0; run(cmd); EXPECT("");
    config_btctler_le_features = 1; config_btctler_le_roles = 0; run(cmd); EXPECT("");
    config_btctler_le_roles = 1;
    cmd[0] = 0x201a; cmd[1] = 0x0abc; for (int i = 0; i < 4; i++) cmd[2 + i] = 0x11223344 + i;
    run(cmd); EXPECT("push(201a);complete(201a,12,7,2748);");
    CHECK(pushed[0] == 0xbc && pushed[1] == 0x0a && pushed[2] == 0x44 && pushed[3] == 0x33 && pushed[6] == 0x45);
    config_btctler_le_features = 0; run(cmd); EXPECT(""); config_btctler_le_features = 1;
    cmd[0] = 0x201b; cmd[1] = 0xf123; run(cmd); EXPECT("push(201b);complete(201b,12,7,291);");
    config_btctler_le_roles = 0; run(cmd); EXPECT(""); config_btctler_le_roles = 1;
    /* Vendor commands need a role only. */
    config_btctler_le_features = 0;
    cmd[0] = 0xfc83; cmd[1] = 0x33; cmd[2] = 0x44; run(cmd); EXPECT("push(fc83);event(15,112,7,1,64643);");
    CHECK(pushed[0] == 0x33 && pushed[2] == 0x44);
    cmd[0] = 0xfc84; cmd[1] = 0x1111; cmd[2] = 0x2222; run(cmd); EXPECT("push(fc84);event(15,112,7,1,64644);");
    CHECK(pushed[0] == 0x11 && pushed[1] == 0x11 && pushed[2] == 0x22 && pushed[3] == 0x22);
    config_btctler_le_roles = 0; run(cmd); EXPECT("");

    /* Connection update: seven fields, not gated. */
    for (int i = 0; i < 10; i++) cmd[i] = 0x100 * (i + 1) + i;
    cmd[0] = 0x2013; run(cmd); EXPECT("push(2013);event(15,112,7,1,8211);");
    CHECK(pushed[0] == 1 && pushed[1] == 2 && pushed[2] == 2 && pushed[3] == 3 && pushed[12] == 7);
    /* OCF 20: the push argument is the word cmd[1] itself. */
    cmd[0] = 0x2014; cmd[1] = 0x2000; run(cmd); EXPECT("push(2014);event(15,112,7,1,8212);");
    /* P-256 key and DH key: the event first, then the call. */
    cmd[0] = 0x2025; run(cmd); EXPECT("event(15,112,0,1,8229);p256;");
    cmd[0] = 0x2026; cmd[1] = 0x300; cmd[2] = 0x400; run(cmd); EXPECT("event(15,112,0,1,8230);dhkey(300,400);");
    /* Data length: feature bit 5. */
    config_btctler_le_features = 32;
    cmd[0] = 0x2022; cmd[1] = 0x0eef; cmd[2] = 0xfb; cmd[3] = 0x848; run(cmd);
    EXPECT("push(2022);complete(2022,12,7,3823);");
    CHECK(pushed[2] == 0xfb && pushed[4] == 0x48 && pushed[5] == 0x08);
    config_btctler_le_features = 31; run(cmd); EXPECT("");
    /* Set PHY: feature bit 8 or 11; the seven parameter bytes go to the LE 5 handler. */
    cmd[0] = 0x2032; cmd[1] = 0x0042; cmd[2] = 1; cmd[3] = 2; cmd[4] = 3; cmd[5] = 0x0405;
    config_btctler_le_features = 0x100; run(cmd); EXPECT("ble5(2032,7);phy(42000102030504);");
    config_btctler_le_features = 0x800; run(cmd); EXPECT("ble5(2032,7);phy(42000102030504);");
    config_btctler_le_features = 0x6ff; run(cmd); EXPECT("");
    /* Any other opcode goes to the LE 5 handler with cmd[1] as a pointer and cmd[2] as the size. */
    cmd[0] = 0x2040; cmd[1] = 0x500; cmd[2] = 9; run(cmd); EXPECT("ble5(2040,500,9);");
    cmd[0] = 0x3fc1; run(cmd); EXPECT("ble5(3fc1,500,9);");
    return TEST_DONE("btctrler_hci_ll");
}
#endif
