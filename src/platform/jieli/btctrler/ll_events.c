/* BLE HCI event formatting and notification. Keeps the SDK's status mapping,
 * feature gates, packet lengths and legacy address handling. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_E, log_tag_const_d_LL_E, log_tag_const_e_LL_E;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const u8 *data, int len);
extern int printf(const char *format, ...);
extern void dut_hci_emit_le_meta_event(u8 subevent_code, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))

static void cpu_reset(void) { p33_soft_reset(); }

static void ll_events_assert(const char *function, int line, const char *why)
{
    if (config_asser) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_events.c", line);
        printf(why, function);
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
}

void le_connection_update_complete_event(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status) LL_CODE;
void le_connection_update_complete_event(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status)
{
    int event_status;
    switch (status) {
    case LL_CONTROL_SUCCESS: event_status = 0; break;
    case LL_CONTROL_UNKNOW_RSP: event_status = 0x1a; break;
    case LL_CONTROL_REJECT:
        if (!rx) ll_events_assert("le_connection_update_complete_event", 49, "ASSERT-FAILD: rx REJECT why no reason %s");
        event_status = rx->data[1];
        break;
    case LL_CONTROL_EXT_REJECT:
        if (!rx) ll_events_assert("le_connection_update_complete_event", 53, "ASSERT-FAILD: rx EXT REJECT why no reason %s");
        event_status = rx->data[2];
        break;
    default:
        ll_events_assert("le_connection_update_complete_event", 57, "ASSERT-FAILD: 0 %s");
        event_status = status;
        break;
    }
    __hci_emit_le_meta_event(3, "1H222", event_status, link->handle,
                             link->ll_data.interval, link->ll_data.latency, link->ll_data.timeout);
    if (log_tag_const_i_LL_E) {
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE\r\n");
        log_print(2, NULL, "[Info]: [LL_E]-status :    %04d\r\n", event_status);
        log_print(2, NULL, "[Info]: [LL_E]-interval :  %04d\r\n", link->ll_data.interval);
        log_print(2, NULL, "[Info]: [LL_E]-latency :   %04d\r\n", link->ll_data.latency);
        log_print(2, NULL, "[Info]: [LL_E]-timeout :   %04d\r\n", link->ll_data.timeout);
    }
}

void le_read_remote_used_features_complete_event(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status) LL_CODE;
void le_read_remote_used_features_complete_event(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status)
{
    int event_status;
    switch (status) {
    case LL_CONTROL_SUCCESS: event_status = 0; break;
    case LL_CONTROL_UNKNOW_RSP: event_status = 0x1a; break;
    default:
        ll_events_assert("le_read_remote_used_features_complete_event", 89, "ASSERT-FAILD: 0 %s");
        event_status = status;
        break;
    }
    if (!rx)
        __hci_emit_le_meta_event(4, "1Hc08", event_status, link->handle, 0);
    else
        __hci_emit_le_meta_event(4, "1Hc08", event_status, link->handle, &rx->data[1]);
    if (log_tag_const_i_LL_E) {
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_READ_REMOTE_USED_FEATURES_COMPLETE\r\n");
        log_print(2, NULL, "[Info]: [LL_E]-features : \r\n");
        printf_buf(&rx->data[1], 8);
    }
}

bool le_long_term_key_request_event(struct le_link *link, struct ble_rx *rx) LL_CODE;
bool le_long_term_key_request_event(struct le_link *link, struct ble_rx *rx)
{
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST\r\n");
    return __hci_emit_le_meta_event(5, "Hc08c02", link->handle, &rx->data[1], &rx->data[9]);
}

bool le_remote_connection_parameter_request_event(struct le_link *link, struct ble_rx *rx) LL_CODE;
bool le_remote_connection_parameter_request_event(struct le_link *link, struct ble_rx *rx)
{
    u8 *data = &rx->data[1];
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST\r\n");
    return __hci_emit_le_meta_event(6, "1H2222", 0, link->handle,
        btctler_little_endian_read_16(data, 0), btctler_little_endian_read_16(data, 2),
        btctler_little_endian_read_16(data, 4), btctler_little_endian_read_16(data, 6));
}

void le_enhanced_connection_complete_event(struct le_link *link, u8 status) LL_CODE;
void le_enhanced_connection_complete_event(struct le_link *link, u8 status)
{
    u8 local_rpa[6], peer_rpa[6];
    ll_read_local_resolvable_address(link->peer.addr_type, link->peer.addr, local_rpa);
    ll_read_peer_resolvable_address(link->peer.addr_type, link->peer.addr, peer_rpa);
    __hci_emit_le_meta_event(10, "1H11AAA2221", status, link->handle, link->role,
        link->peer.addr_type, link->peer.addr, local_rpa, peer_rpa,
        link->ll_data.interval, link->ll_data.latency, link->ll_data.timeout,
        link->master_clock_accuracy);
    if (log_tag_const_i_LL_E) {
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_EVENT\r\n");
        char *addr = btctler_bd_addr_to_str(link->peer.addr);
        log_print(2, NULL, "[Info]: [LL_E]-Peer MAC: %d / %s\r\n", link->peer.addr_type, addr);
    }
}

void le_connection_complete_event(struct le_link *link, u8 status) LL_CODE;
void le_connection_complete_event(struct le_link *link, u8 status)
{
    static const u8 zero_addr[6] = {0};
    if (link) {
        if (log_tag_const_d_LL_E) {
            char *addr = btctler_bd_addr_to_str(link->peer.addr);
            log_print(1, NULL, "[Debug]: [LL_E]-Peer MAC: %d / %s\r\n", link->peer.addr_type, addr);
        }
        __hci_emit_le_meta_event(1, "1H11A2221", status, link->handle, link->role,
            link->peer.addr_type, link->peer.addr, link->ll_data.interval,
            link->ll_data.latency, link->ll_data.timeout, link->master_clock_accuracy);
        if (log_tag_const_d_LL_E)
            log_print(1, NULL, "[Debug]: [LL_E]HCI_SUBEVENT_LE_CONNECTION_COMPLETE %x\r\n", status);
    } else {
        __hci_emit_le_meta_event(1, "1H11A2221", status, 0, 0, 0, zero_addr, 0, 0, 0, 0);
        if (log_tag_const_d_LL_E)
            log_print(1, NULL, "[Debug]: [LL_E]HCI_SUBEVENT_LE_CONNECTION_COMPLETE %x\r\n", status);
    }
}

void le_connection_complete_event_emit(struct le_link *link) LL_CODE;
void le_connection_complete_event_emit(struct le_link *link)
{
    if (config_btctler_le_features & 64) {
        if (le_param.resolution_enable)
            le_enhanced_connection_complete_event(link, 0);
    } else {
        le_connection_complete_event(link, 0);
    }
}

void le_advertising_report_event(struct ble_rx *rx, u8 event_type, u8 rssi) LL_CODE;
void le_advertising_report_event(struct ble_rx *rx, u8 event_type, u8 rssi)
{
    if (le_scan_filter_duplicates(rx)) return;
    __hci_emit_le_meta_event(2, "111AlB1", 1, event_type, rx->txadd, rx->data,
                             rx->len - 6, &rx->data[6], rssi);
}

void le_direct_advertising_report_event(struct ble_rx *rx, u8 rssi) LL_CODE;
void le_direct_advertising_report_event(struct ble_rx *rx, u8 rssi)
{
    if (le_scan_filter_duplicates(rx)) {
        if (log_tag_const_i_LL_E)
            log_print(2, NULL, "[Info]: [LL_E]filter_duplicates\r\n");
        return;
    }
    __hci_emit_le_meta_event(11, "111A1A1", 1, 1, rx->txadd, rx->data,
                             rx->txadd, rx->data, rssi);
    if (log_tag_const_i_LL_E) {
        char *addr = btctler_bd_addr_to_str(rx->data);
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_DIRECT_ADVERTISING_REPORT_EVENT :MAC %d / %s\r\n", rx->txadd, addr);
        log_print(2, NULL, "[Info]: [LL_E]-Adv data\r\n");
        printf_buf(&rx->data[6], rx->len - 6);
        log_print(2, NULL, "[Info]: [LL_E]-RSSI : %d dBm\r\n", rssi);
    }
}

void le_data_length_change_event(struct le_link *link) LL_CODE;
void le_data_length_change_event(struct le_link *link)
{
    __hci_emit_le_meta_event(7, "H2222", link->handle,
        link->pdu_len.connEffectiveMaxTxOctets, link->pdu_len.connEffectiveMaxTxTime,
        link->pdu_len.connEffectiveMaxRxOctets, link->pdu_len.connEffectiveMaxRxTime);
    if (log_tag_const_i_LL_E) {
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE\r\n");
        log_print(2, NULL, "[Info]: [LL_E]-connEffectiveMaxTxOctets  : %x\r\n", link->pdu_len.connEffectiveMaxTxOctets);
        log_print(2, NULL, "[Info]: [LL_E]-connEffectiveMaxTxTime    : %x\r\n", link->pdu_len.connEffectiveMaxTxTime);
        log_print(2, NULL, "[Info]: [LL_E]-connEffectiveMaxRxOctets  : %x\r\n", link->pdu_len.connEffectiveMaxRxOctets);
        log_print(2, NULL, "[Info]: [LL_E]-connEffectiveMaxRxTime    : %x\r\n", link->pdu_len.connEffectiveMaxRxTime);
    }
}

void le_disconnect_complete_event(struct le_link *link, LL_CONTROL_CASE status) LL_CODE;
void le_disconnect_complete_event(struct le_link *link, LL_CONTROL_CASE status)
{
    int reason;
    if (status == LL_CONTROL_SUCCESS) reason = 22;
    else if (status == LL_CONTROL_TIMEOUT) reason = 34;
    else {
        ll_events_assert("le_disconnect_complete_event", 298, "ASSERT-FAILD: 0 %s");
        reason = status;
    }
    ll_disconnect_process(link, (u8)reason);
}

void hci_event_encryption_change(struct le_link *link, struct ble_rx *rx, u8 status) LL_CODE;
void hci_event_encryption_change(struct le_link *link, struct ble_rx *rx, u8 status)
{
    if (!hci_param.secure_conn_host_support) {
        u8 event_status = status;
        switch (status) {
        case 0: event_status = 0; break;
        case 3: event_status = 0x1a; break;
        case 4: event_status = 0x22; break;
        case 1:
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            event_status = rx->data[1];
            break;
        case 2:
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            event_status = rx->data[2];
            break;
        default:
            ll_events_assert("hci_event_encryption_change", 339, "ASSERT-FAILD: 0 %s");
            break;
        }
        hci_send_event(8, "1H1", event_status, link->handle, link->encrypt_enable != 0);
        if (log_tag_const_i_LL_E)
            log_print(2, NULL, "[Info]: [LL_E]HCI_EVENT_ENCRYPTION_CHANGE ENCRYPTION : %s\r\n", link->encrypt_enable ? "ON" : "OFF");
        if (event_status == 6 && log_tag_const_e_LL_E)
            log_print(4, NULL, "<Error>: [LL_E]PIN Or Key Missing\r\n");
    }
}

void hci_event_encryption_refresh_complete(struct le_link *link, struct ble_rx *rx, u8 status) LL_CODE;
void hci_event_encryption_refresh_complete(struct le_link *link, struct ble_rx *rx, u8 status)
{
    int event_status;
    switch (status) {
    case 0: event_status = 0; break;
    case 3: event_status = 0x1a; break;
    case 4: event_status = 0x22; break;
    case 1:
        if (!rx) ll_events_assert("hci_event_encryption_refresh_complete", 367, "ASSERT-FAILD: rx REJECT why no reason %s");
        event_status = rx->data[1];
        break;
    case 2:
        if (!rx) ll_events_assert("hci_event_encryption_refresh_complete", 371, "ASSERT-FAILD: rx EXT REJECT why no reason %s");
        event_status = rx->data[2];
        break;
    default:
        ll_events_assert("hci_event_encryption_refresh_complete", 376, "ASSERT-FAILD: 0 %s");
        event_status = status;
        break;
    }
    hci_send_event(48, "1H", event_status, link->handle);
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE\r\n");
}

void hci_event_disconnection_complete(u8 handle, u8 reason) LL_CODE;
void hci_event_disconnection_complete(u8 handle, u8 reason)
{
    hci_send_event(5, "1H1", 0, handle, reason);
    if (log_tag_const_i_LL_E) {
        log_print(2, NULL, "[Info]: [LL_E]HCI_EVENT_DISCONNECTION_COMPLETE 0x%x:\r\n", reason);
        switch (reason) {
        case 62: log_print(2, NULL, "[Info]: [LL_E] - CONNECTION_FAILED_TO_BE_ESTABLISHED\r\n"); break;
        case 8: log_print(2, NULL, "[Info]: [LL_E] - CONNECTION_TIMEOUT\r\n"); break;
        case 34: log_print(2, NULL, "[Info]: [LL_E] - LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT\r\n"); break;
        case 22: log_print(2, NULL, "[Info]: [LL_E] - CONNECTION_TERMINATED_BY_LOCAL_HOST\r\n"); break;
        }
    }
}

void hci_event_number_of_completed_packets(struct le_link *link, u8 num_packets) LL_CODE;
void hci_event_number_of_completed_packets(struct le_link *link, u8 num_packets)
{ hci_send_event(19, "1H2", 1, link->handle, num_packets); }

void hci_read_remote_version_information_complete(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status) LL_CODE;
void hci_read_remote_version_information_complete(struct le_link *link, struct ble_rx *rx, LL_CONTROL_CASE status)
{
    int event_status;
    if (status == LL_CONTROL_SUCCESS) event_status = 0;
    else if (status == LL_CONTROL_TIMEOUT) event_status = 34;
    else {
        ll_events_assert("hci_read_remote_version_information_complete", 430, "ASSERT-FAILD: 0 %s");
        event_status = status;
    }
    u8 *data = &rx->data[1];
    u16 manufacturer = btctler_little_endian_read_16(data, 1);
    u16 subversion = btctler_little_endian_read_16(data, 3);
    hci_send_event(12, "1H122", event_status, link->handle, data[0], manufacturer, subversion);
    u8 version = data[0];
    manufacturer = btctler_little_endian_read_16(data, 1);
    subversion = btctler_little_endian_read_16(data, 3);
    hci_read_remote_version_information_show("BLE", version, manufacturer, subversion);
}

void le_read_local_p256_public_key_complete_event(u8 status, u8 *key) LL_CODE;
void le_read_local_p256_public_key_complete_event(u8 status, u8 *key)
{ __hci_emit_le_meta_event(8, "1LB", status, 64, key); }

void le_generate_DHKey_complete_event(u8 status, u8 *dhkey) LL_CODE;
void le_generate_DHKey_complete_event(u8 status, u8 *dhkey)
{ __hci_emit_le_meta_event(9, "1LB", status, 32, dhkey); }

void le_phy_update_complete_event(struct le_link *link, u8 status, bool event_toggle) LL_CODE;
void le_phy_update_complete_event(struct le_link *link, u8 status, bool event_toggle)
{
    if (!(config_btctler_le_features & 2304)) return;
    u8 tx_phy, rx_phy;
    refresh_phy_param(link, &tx_phy, &rx_phy);
    if (!event_toggle) return;
    __hci_emit_le_meta_event(12, "1211", status, link->handle, tx_phy + 1, rx_phy + 1);
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE TX_PHY=%d RX_PHY=%d\r\n", tx_phy + 1, rx_phy + 1);
}

void le_extended_advertising_report_event(struct __ext_adv_report_event *report, u8 *Data) LL_CODE;
void le_extended_advertising_report_event(struct __ext_adv_report_event *report, u8 *Data)
{ hci_send_event(62, "LBLB", 26, report, report->Data_Length, Data); }

void le_periodic_advertising_sync_established_event(struct le_ext_scan_link *link, struct ble_ext_rx *rx, u8 status) LL_CODE;
void le_periodic_advertising_sync_established_event(struct le_ext_scan_link *link, struct ble_ext_rx *rx, u8 status)
{
    u8 address_type = rx ? rx->txadd : 0;
    u8 adv_phy = rx ? rx->secondary_phy : 0;
    u16 sync_handle = link->period_sync ? link->period_sync->sync_handle : 0;
    hci_send_event(62, "11211A121", 14, status, sync_handle,
        link->ext_adv_report.advertising_adi >> 12, address_type,
        link->ext_adv_report.address, adv_phy,
        link->ext_adv_report.periodic_adv_interval, 0);
}

void le_periodic_advertising_sync_lost_event(u16 sync_handle) LL_CODE;
void le_periodic_advertising_sync_lost_event(u16 sync_handle)
{ hci_send_event(62, "12", 16, sync_handle); }

void le_periodic_advertising_report_event(struct __period_adv_report_event *report, u8 *Data) LL_CODE;
void le_periodic_advertising_report_event(struct __period_adv_report_event *report, u8 *Data)
{ hci_send_event(62, "LBLB", 8, report, report->Data_Length, Data); }

void le_connless_iq_report_event(struct le_ext_scan_link *link, struct le_cte_iq_sample_report *report) LL_CODE;
void le_connless_iq_report_event(struct le_ext_scan_link *link, struct le_cte_iq_sample_report *report)
{
    u16 sync_handle = link && link->period_sync ? link->period_sync->sync_handle : 0;
    if (config_btctler_mode & 6) {
        u8 samples = report->sample_cnt;
        dut_hci_emit_le_meta_event(21, "212111121LB", 4095, report->data_channel_index,
            report->rssi, report->rssi_ant_id, report->cte_type, report->slot_duartations,
            report->packet_status, report->conn_event_counter, samples, samples << 1, report->i_sample);
    } else {
        if (log_tag_const_i_LL_E)
            log_print(2, NULL, "[Info]: [LL_E]evt-conless_iq\n\r\n");
        u8 samples = report->sample_cnt;
        hci_send_event(62, "1212111121LB", 21, sync_handle, report->data_channel_index,
            report->rssi, report->rssi_ant_id, report->cte_type, report->slot_duartations,
            report->packet_status, report->conn_event_counter, samples, samples << 1, report->i_sample);
    }
}

void le_private_iq_report_event(struct le_ext_scan_link *link, u8 *report, u16 len) LL_CODE;
void le_private_iq_report_event(struct le_ext_scan_link *link, u8 *report, u16 len)
{ (void)link; hci_send_ext_event(62, "1LB", 241, len, report); }

void le_connect_iq_report_event(struct le_link *link, struct le_cte_iq_sample_report *report) LL_CODE;
void le_connect_iq_report_event(struct le_link *link, struct le_cte_iq_sample_report *report)
{
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]con_handle:%x\nch_index:%x\nrssi:%x\nrssi_id:%x\ncte_type:%x\nslot_dura:%x\npkt_sta:%x\nevt_cnt:%x\nsmp_cnt:%x\n\r\n",
            link->handle, report->data_channel_index, report->rssi, report->rssi_ant_id,
            report->cte_type, report->slot_duartations, report->packet_status,
            report->conn_event_counter, report->sample_cnt);
    u8 samples = report->sample_cnt;
    hci_send_event(62, "12112111121LB", 22, link->handle, report->rx_phy,
        report->data_channel_index, report->rssi, report->rssi_ant_id,
        report->cte_type, report->slot_duartations, report->packet_status,
        report->conn_event_counter, samples, samples << 1, report->i_sample);
}

void le_advertising_set_terminated_event(u8 status, u8 advertising_handle, u16 connection_handle) LL_CODE;
void le_advertising_set_terminated_event(u8 status, u8 advertising_handle, u16 connection_handle)
{
    hci_send_event(62, "1121", 18, status, advertising_handle, connection_handle, 0);
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_ADVERTISING_SET_TERMINATED %x\r\n", status);
}

void le_scan_request_received_event(u16 adv_handle, u8 rxadd, u8 *data) LL_CODE;
void le_scan_request_received_event(u16 adv_handle, u8 rxadd, u8 *data)
{ hci_send_event(62, "111A", 19, adv_handle, rxadd, data); }

void le_channel_selection_algorithm_event(struct le_link *link, u8 algorithm) LL_CODE;
void le_channel_selection_algorithm_event(struct le_link *link, u8 algorithm)
{
    if (!(config_btctler_le_features & 16384)) return;
    hci_send_event(62, "121", 20, link->handle, algorithm);
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]HCI_SUBEVENT_LE_CHANNEL_SELECTION_ALGORITHM #%d\r\n", algorithm + 1);
}

void le_scan_timeout_event(void) LL_CODE;
void le_scan_timeout_event(void)
{
    __hci_emit_le_meta_event(17, "");
    if (log_tag_const_i_LL_E)
        log_print(2, NULL, "[Info]: [LL_E]le_scan_timeout_event\r\n");
}

void le_vendor_interval_complete_event(struct le_link *link, u8 status) LL_CODE;
void le_vendor_interval_complete_event(struct le_link *link, u8 status)
{
    if (link) __hci_emit_le_meta_event(-16, "1HH", status, link->handle, link->event_cnt);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

int config_asser, config_btctler_mode;
u64 config_btctler_le_features;
u8 log_tag_const_i_LL_E, log_tag_const_d_LL_E, log_tag_const_e_LL_E;
struct le_parameter le_param;
struct hci_parameter hci_param;
static struct ble_operation test_ble_ops;
const struct ble_operation *__ble_ops = &test_ble_ops;

static int seen_opcode, seen_subevent, seen_event_count;
static int seen_event_status, seen_event_enable, seen_num_packets, seen_iq_samples;
static int seen_report_length, filter_duplicates;
static u16 seen_handle, seen_version_manufacturer, seen_version_subversion;
static u8 seen_rxadd, seen_event_version;
static const u8 *seen_data;
static int seen_meta_code, seen_meta_status;
static u16 seen_meta_handle, seen_interval, seen_latency, seen_timeout;
static const void *seen_meta_data;
static u8 *mutate_version_bytes, *mutate_samples;
static int shown_version, shown_manufacturer, shown_subversion;
static int reset_count, disconnect_reason, ioctrl_count, logged_rssi;
static int log_order, address_after_name;

void p33_soft_reset(void) { reset_count++; }
void ll_disconnect_process(struct le_link *link, u8 reason)
{ (void)link; disconnect_reason = reason; }
void ll_read_local_resolvable_address(const u8 type, const u8 *addr, u8 *out)
{ (void)type; (void)addr; memset(out, 0xa1, 6); }
void ll_read_peer_resolvable_address(const u8 type, const u8 *addr, u8 *out)
{ (void)type; (void)addr; memset(out, 0xb2, 6); }
bool le_scan_filter_duplicates(struct ble_rx *rx) { (void)rx; return filter_duplicates != 0; }
char *btctler_bd_addr_to_str(const uint8_t *addr)
{ (void)addr; if (log_order == 1) address_after_name = 1; log_order = 2; return "address"; }
uint16_t btctler_little_endian_read_16(const uint8_t *p, int pos)
{ return (u16)(p[pos] | ((u16)p[pos + 1] << 8)); }
void hci_read_remote_version_information_show(const char *str, u8 version, u16 manufacturer, u16 subversion)
{ (void)str; shown_version = version; shown_manufacturer = manufacturer; shown_subversion = subversion; }
void printf_buf(const u8 *data, int len) { (void)data; (void)len; }
static void test_ioctrl(void *hw, int opcode, ...)
{ (void)hw; CHECK(opcode == 17); ioctrl_count++; }
void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag;
    if (mutate_samples) { *mutate_samples = 5; mutate_samples = NULL; }
    if (strstr(format, "HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_EVENT")) log_order = 1;
    if (strstr(format, "-RSSI : %d dBm")) {
        va_list ap; va_start(ap, format); logged_rssi = va_arg(ap, int); va_end(ap);
    }
}

int hci_send_event(int opcode, const char *format, ...)
{
    va_list ap;
    seen_opcode = opcode;
    va_start(ap, format);
    if (opcode == 62 && !strcmp(format, "111A")) {
        seen_subevent = va_arg(ap, int);
        seen_handle = (u16)va_arg(ap, int);
        seen_rxadd = (u8)va_arg(ap, int);
        seen_data = va_arg(ap, const u8 *);
    } else if (opcode == 19 && !strcmp(format, "1H2")) {
        seen_subevent = va_arg(ap, int);
        seen_handle = (u16)va_arg(ap, int);
        seen_num_packets = va_arg(ap, int);
    } else if (opcode == 8 && !strcmp(format, "1H1")) {
        seen_event_status = va_arg(ap, int);
        seen_handle = (u16)va_arg(ap, int);
        seen_event_enable = va_arg(ap, int);
    } else if (opcode == 48 && !strcmp(format, "1H")) {
        seen_event_status = va_arg(ap, int);
        seen_handle = (u16)va_arg(ap, int);
    } else if (opcode == 12 && !strcmp(format, "1H122")) {
        seen_event_status = va_arg(ap, int);
        seen_handle = (u16)va_arg(ap, int);
        seen_event_version = (u8)va_arg(ap, int);
        seen_version_manufacturer = (u16)va_arg(ap, int);
        seen_version_subversion = (u16)va_arg(ap, int);
        if (mutate_version_bytes) {
            mutate_version_bytes[0] = 0x7e;
            mutate_version_bytes[1] = 0xbc; mutate_version_bytes[2] = 0x9a;
            mutate_version_bytes[3] = 0xf0; mutate_version_bytes[4] = 0xde;
            mutate_version_bytes = NULL;
        }
    } else if (opcode == 62 && !strcmp(format, "1212111121LB")) {
        va_arg(ap, int); /* subevent */ va_arg(ap, int); /* sync handle */
        for (int i = 0; i < 7; i++) va_arg(ap, int);
        seen_iq_samples = va_arg(ap, int);
        seen_num_packets = va_arg(ap, int); /* IQ sample byte count */
        seen_data = va_arg(ap, const u8 *);
    } else if (opcode == 62 && !strcmp(format, "12112111121LB")) {
        va_arg(ap, int); /* subevent */ va_arg(ap, int); /* connection handle */
        for (int i = 0; i < 8; i++) va_arg(ap, int);
        seen_iq_samples = va_arg(ap, int);
        seen_num_packets = va_arg(ap, int); /* IQ sample byte count */
        seen_data = va_arg(ap, const u8 *);
    } else {
        CHECK(0);
    }
    va_end(ap);
    seen_event_count++;
    return 0;
}
bool __hci_emit_le_meta_event(u8 code, const char *format, ...)
{
    va_list ap;
    seen_meta_code = code;
    va_start(ap, format);
    if (code == 3 && !strcmp(format, "1H222")) {
        seen_meta_status = va_arg(ap, int);
        seen_meta_handle = (u16)va_arg(ap, int);
        seen_interval = (u16)va_arg(ap, int);
        seen_latency = (u16)va_arg(ap, int);
        seen_timeout = (u16)va_arg(ap, int);
    } else if (code == 4 && !strcmp(format, "1Hc08")) {
        seen_meta_status = va_arg(ap, int);
        seen_meta_handle = (u16)va_arg(ap, int);
        seen_meta_data = va_arg(ap, const void *);
    } else if (code == 1 && !strcmp(format, "1H11A2221")) {
        seen_meta_status = va_arg(ap, int);
        seen_meta_handle = (u16)va_arg(ap, int);
    } else if (code == 10 && !strcmp(format, "1H11AAA2221")) {
        seen_meta_status = va_arg(ap, int);
        seen_meta_handle = (u16)va_arg(ap, int);
    } else if (code == 11 && !strcmp(format, "111A1A1")) {
        va_arg(ap, int); va_arg(ap, int); va_arg(ap, int);
        va_arg(ap, const void *); va_arg(ap, int); va_arg(ap, const void *);
        va_arg(ap, int);
        seen_event_status = 1;
    } else if (code == 2 && !strcmp(format, "111AlB1")) {
        va_arg(ap, int); va_arg(ap, int); va_arg(ap, int);
        seen_data = va_arg(ap, const u8 *);
        seen_report_length = va_arg(ap, int);
        seen_meta_data = va_arg(ap, const void *);
        va_arg(ap, int);
    } else {
        CHECK(0);
    }
    va_end(ap);
    seen_event_count++;
    return true;
}
int main(void)
{
    const u8 data[] = { 0x5a, 0xa5 };
    seen_event_count = 0;
    le_scan_request_received_event(0x1234, 1, (u8 *)data);
    CHECK(seen_opcode == 62);
    CHECK(seen_subevent == 19);
    CHECK(seen_handle == 0x1234);
    CHECK(seen_rxadd == 1);
    CHECK(seen_data == data);

    struct le_link link = {0};
    link.handle = 0x2345;
    link.ll_data.interval = 24;
    link.ll_data.latency = 3;
    link.ll_data.timeout = 400;
    le_connection_update_complete_event(&link, NULL, LL_CONTROL_SUCCESS);
    CHECK(seen_meta_code == 3 && seen_meta_status == 0 && seen_meta_handle == 0x2345);
    CHECK(seen_interval == 24 && seen_latency == 3 && seen_timeout == 400);
    le_connection_update_complete_event(&link, NULL, LL_CONTROL_UNKNOW_RSP);
    CHECK(seen_meta_status == 26);
    union { struct ble_rx aligned; u8 raw[sizeof(struct ble_rx) + 8]; } storage = {0};
    struct ble_rx *rx = (struct ble_rx *)storage.raw;
    rx->data[1] = 0x0c;
    rx->data[2] = 0x11;
    le_connection_update_complete_event(&link, rx, LL_CONTROL_REJECT);
    CHECK(seen_meta_status == 0x0c);
    le_connection_update_complete_event(&link, rx, LL_CONTROL_EXT_REJECT);
    CHECK(seen_meta_status == 0x11);

    int raw_status = 0x12345678;
    le_connection_update_complete_event(&link, NULL, (LL_CONTROL_CASE)raw_status);
    CHECK(seen_meta_status == raw_status && reset_count == 1);
    le_read_remote_used_features_complete_event(&link, NULL, LL_CONTROL_SUCCESS);
    CHECK(seen_meta_code == 4 && seen_meta_status == 0 && seen_meta_handle == 0x2345);
    CHECK(seen_meta_data == NULL);
    le_read_remote_used_features_complete_event(&link, NULL, (LL_CONTROL_CASE)raw_status);
    CHECK(seen_meta_status == raw_status && reset_count == 2);

    le_disconnect_complete_event(&link, LL_CONTROL_SUCCESS);
    CHECK(disconnect_reason == 22);
    le_disconnect_complete_event(&link, LL_CONTROL_TIMEOUT);
    CHECK(disconnect_reason == 34);
    le_disconnect_complete_event(&link, (LL_CONTROL_CASE)raw_status);
    CHECK(disconnect_reason == 0x78 && reset_count == 3);

    config_btctler_le_features = 0;
    le_param.resolution_enable = 1;
    le_connection_complete_event_emit(&link);
    CHECK(seen_meta_code == 1);
    config_btctler_le_features = 64;
    le_param.resolution_enable = 0;
    int previous_events = seen_event_count;
    le_connection_complete_event_emit(&link);
    CHECK(seen_event_count == previous_events);
    le_param.resolution_enable = 1;
    log_tag_const_i_LL_E = 0;
    le_connection_complete_event_emit(&link);
    CHECK(seen_meta_code == 10);
    log_tag_const_i_LL_E = 1;
    log_order = 0; address_after_name = 0;
    le_enhanced_connection_complete_event(&link, 0);
    CHECK(address_after_name == 1);
    log_tag_const_i_LL_E = 0;

    hci_event_number_of_completed_packets(&link, 7);
    CHECK(seen_opcode == 19 && seen_subevent == 1 && seen_handle == 0x2345 && seen_num_packets == 7);

    hci_param.secure_conn_host_support = 0;
    test_ble_ops.ioctrl = test_ioctrl;
    link.encrypt_enable = 1;
    hci_event_encryption_change(&link, rx, 0);
    CHECK(seen_opcode == 8 && seen_event_status == 0);
    hci_event_encryption_change(&link, rx, 3);
    CHECK(seen_opcode == 8 && seen_event_status == 26 && seen_event_enable == 1);
    hci_event_encryption_change(&link, rx, 4);
    CHECK(seen_event_status == 34);
    rx->data[1] = 0x0b; rx->data[2] = 0x0c;
    hci_event_encryption_change(&link, rx, 1);
    CHECK(seen_event_status == 0x0b && ioctrl_count == 1);
    hci_event_encryption_change(&link, rx, 2);
    CHECK(seen_event_status == 0x0c && ioctrl_count == 2);
    hci_event_encryption_refresh_complete(&link, rx, 3);
    CHECK(seen_opcode == 48 && seen_event_status == 26);
    hci_param.secure_conn_host_support = 1;
    previous_events = seen_event_count;
    hci_event_encryption_change(&link, rx, 3);
    CHECK(seen_event_count == previous_events);
    hci_param.secure_conn_host_support = 0;

    mutate_version_bytes = &rx->data[1];
    rx->data[1] = 0x12; rx->data[2] = 0x34; rx->data[3] = 0x56;
    rx->data[4] = 0x78; rx->data[5] = 0x9a;
    hci_read_remote_version_information_complete(&link, rx, LL_CONTROL_SUCCESS);
    CHECK(seen_opcode == 12 && !strcmp("1H122", "1H122"));
    CHECK(seen_event_version == 0x12 && seen_version_manufacturer == 0x5634 && seen_version_subversion == 0x9a78);
    CHECK(shown_version == 0x7e && shown_manufacturer == 0x9abc && shown_subversion == 0xdef0);
    hci_read_remote_version_information_complete(&link, rx, (LL_CONTROL_CASE)raw_status);
    CHECK(seen_event_status == raw_status && reset_count == 4);

    union { struct ble_rx aligned; u8 raw[sizeof(struct ble_rx) + 12]; } adv_storage = {0};
    struct ble_rx *adv_rx = (struct ble_rx *)adv_storage.raw;
    adv_rx->len = 10;
    log_tag_const_i_LL_E = 1;
    logged_rssi = -1;
    le_direct_advertising_report_event(adv_rx, 250);
    CHECK(logged_rssi == 250);
    previous_events = seen_event_count;
    filter_duplicates = 1;
    le_direct_advertising_report_event(adv_rx, 250);
    CHECK(seen_event_count == previous_events);
    filter_duplicates = 0;
    le_advertising_report_event(adv_rx, 4, 9);
    CHECK(seen_meta_code == 2 && seen_report_length == 4);
    CHECK(seen_data == adv_rx->data && seen_meta_data == &adv_rx->data[6]);

    union { struct le_cte_iq_sample_report aligned; u8 raw[sizeof(struct le_cte_iq_sample_report) + 4]; } iq_storage = {0};
    struct le_cte_iq_sample_report *iq = (struct le_cte_iq_sample_report *)iq_storage.raw;
    iq->sample_cnt = 1;
    mutate_samples = &iq->sample_cnt;
    config_btctler_mode = 0;
    le_connless_iq_report_event(NULL, iq);
    CHECK(seen_event_count > 0 && seen_iq_samples == 5 && seen_num_packets == 10 && seen_data == (const u8 *)iq->i_sample);
    iq->sample_cnt = 1;
    mutate_samples = &iq->sample_cnt;
    le_connect_iq_report_event(&link, iq);
    CHECK(seen_iq_samples == 5 && seen_num_packets == 10 && seen_data == (const u8 *)iq->i_sample);

    log_tag_const_i_LL_E = 0;
    return TEST_DONE("btctrler_ll_events");
}
#endif
