/* BLE slave link-layer receive path and update-handle accessors.  SDK quirks
 * kept: the connection-update widening uses the old interval, the channel-map
 * instant can apply immediately when already lost, and slave ENC_REQ uses the
 * SDK's split random key generation and reject ordering. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_S, log_tag_const_e_LL_S;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_btctler_le_slave_conn_update_winden;
extern BTCTRLER_CONFIG u32 config_vendor_le_bb;
extern struct le_parameter le_param;
extern const struct ble_operation *__ble_ops;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern u8 get_bt_osc_offset_flag(void);
extern void bt_fre_offset_value_init(void);
extern void ble_pll_comp_reset(void *hw);
extern int hci_send_event(int event, const char *format, ...);
extern u8 get_jl_chip_id(void);
extern void bt_osc_offset_set(s32 value);
extern void bt_osc_offset_save(void);
extern int bt_fre_offset_get(void);
extern void ll_disconnect_process(struct le_link *link, u8 reason);
extern void ll_remote_feature(struct le_link *link);
extern void pseudo_random_genrate(u8 *buffer, u32 length);

#define LL_CODE __attribute__((section(".ble_ll_code")))

static u16 ll_update_handle __attribute__((section(".ble_ll_data"), aligned(2)));
void (*ll_vendor_update_req_handle)(u8 *, u16)
    __attribute__((section(".ble_ll_data"), aligned(4)));
u16 (*ble_vendor_custom_cmds_handle)(u8 *, u8 *)
    __attribute__((section(".ble_ll_data"), aligned(4)));
u16 (*ble_vendor_priv_cmd_handle)(u8 *, u8 *)
    __attribute__((section(".ble_ll_data"), aligned(4)));
static const char test_name[10] __attribute__((section(".ble_ll_const"), aligned(1))) = "autotest";

int ll_get_update_handle(void) LL_CODE;
int ll_get_update_handle(void) { return ll_update_handle; }

void ll_set_update_handle(u16 handle) LL_CODE;
void ll_set_update_handle(u16 handle) { ll_update_handle = handle; }

static void __slave_ll_receive_conn_update_req_done(struct le_link *link) LL_CODE;
static void __slave_ll_receive_conn_update_req_done(struct le_link *link)
{
    le_connection_update_complete_event(link, NULL, 0);
}

static void __slave_receive_terminte_ind_done(struct le_link *link) LL_CODE;
static void __slave_receive_terminte_ind_done(struct le_link *link)
{
    ll_disconnect_process(link, link->reason);
}

static void __slave_ll_send_reject_ind_auto(struct le_link *link, u8 reason) LL_CODE;
static void __slave_ll_send_reject_ind_auto(struct le_link *link, u8 reason)
{
    ll_send_control_data(link, 13, "1", (int)reason);
}

static void cpu_reset(void) LL_CODE;
static void cpu_reset(void) { p33_soft_reset(); }

static void __slave_ll_send_enc_rsp_callback(void *priv) LL_CODE;
static void __slave_ll_send_enc_rsp_callback(void *priv)
{
    __ble_ops->ioctrl(*(void **)priv, 17, 1);
}

void __ll_send_vendor_jl_chip_rsp(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __ll_send_vendor_jl_chip_rsp(struct le_link *link, struct ble_rx *rx)
{
    (void)rx;
    u8 osc_flag = get_bt_osc_offset_flag();
    int flag = osc_flag == 0 ? 0 : 255;
    (void)flag;
    bt_fre_offset_value_init();
    ble_pll_comp_reset(link->hw);
    hci_send_event(254, "11", 2, 1);
    if (log_tag_const_i_LL_S) {
        u8 chip_id = get_jl_chip_id();
        log_print(2, NULL, "[Info]: [LL_S]jl_chip_req:%x %x %s %x\n\r\n",
                  flag, flag, test_name, chip_id);
    }
    u8 chip_id = get_jl_chip_id();
    ll_send_control_data(link, 65, "111", flag, flag, chip_id);
}

void __ll_send_vendor_priv_cmd_rsp(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __ll_send_vendor_priv_cmd_rsp(struct le_link *link, struct ble_rx *rx)
{
    u8 rsp_buf[34];
    u8 opcode = rx->data[0];
    u8 *params = &rx->data[1];
    u8 subopcode = rx->data[1];
    int result;
    if (ble_vendor_priv_cmd_handle) {
        result = ble_vendor_priv_cmd_handle(params, rsp_buf);
    } else if (ble_vendor_custom_cmds_handle && (subopcode == 5 || subopcode == 7)) {
        result = ble_vendor_custom_cmds_handle(params, rsp_buf);
    } else {
        result = 0;
    }
    u8 len = (u8)result;
    if (len > sizeof(rsp_buf) && log_tag_const_e_LL_S)
        log_print(4, NULL, "<Error>: [LL_S]rsp len over size:%d\n\r\n", (int)sizeof(rsp_buf));
    if (len != 0) {
        ll_send_control_data(link, 254, "LB", result & 255, rsp_buf);
    } else {
        ll_send_control_data(link, 7, "1", (int)opcode);
    }
}

void __ll_send_fre_offset_rsp(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __ll_send_fre_offset_rsp(struct le_link *link, struct ble_rx *rx)
{
    u32 value = ((u32)rx->data[1]) | ((u32)rx->data[2] << 8) |
                ((u32)rx->data[3] << 16) | ((u32)rx->data[4] << 24);
    if (get_bt_osc_offset_flag() != 0) {
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]>>>>>>>>>>>>>bt_osc_offset_flag ok<<<<<\n\r\n");
        return;
    }
    bt_osc_offset_set(value);
    if (rx->data[5] != 0) {
        bt_osc_offset_save();
        int offset = bt_fre_offset_get();
        hci_send_event(246, "2", offset);
    }
    ble_pll_comp_reset(link->hw);
    ll_send_control_data(link, 80, NULL);
}

void slave_rx_conn_post_handler(struct le_link *link, struct ble_rx *rx) LL_CODE;
void slave_rx_conn_post_handler(struct le_link *link, struct ble_rx *rx)
{
    if ((config_btctler_le_roles & 2) == 0) return;
    if (rx->type == 3) {
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]SCAN_REQ\r\n");
        return;
    }
    if (rx->type == 5 && log_tag_const_i_LL_S)
        log_print(2, NULL, "[Info]: [LL_S]CONNECT_REQ\r\n");
    ll_debug_info(link);
    if (log_tag_const_i_LL_S)
        log_print(2, NULL, "[Info]: [LL_S]link->state : %s\r\n",
                  link->state == 6 ? "LL_CONNECTION_CREATE" : "LL_CONNECTION_ESTABLISHED");
    ll_supervision_timeout_start(link, link->ll_data.timeout * 10);
    le_connection_complete_event_emit(link);
    tws_phone_ble_link_connection(link);
    refresh_phy_param(link, NULL, NULL);
    le_channel_selection_algorithm_event(link, rx->chsel_algorithm);
    ll_exit_ext_legacy_adv_status(link->handle);
}

static void __conn_slave_rx_post_handler(void *priv, struct ble_rx *rx) LL_CODE;
static void __conn_slave_rx_post_handler(void *priv, struct ble_rx *rx)
{
    struct le_link *link = priv;
    if (link->rx_update_handle) {
        link->rx_update_handle(priv, rx->data, rx->len);
        if (rx->llid == 1 || rx->llid == 2) {
            hw_dma_free(rx);
            return;
        }
    }
    if (rx->llid == 0) return;
    if (rx->llid == 1 || rx->llid == 2) {
        conn_rx_acl_u_post_handler(link, rx);
        return;
    }
    if (rx->llid != 3 || (config_btctler_le_roles & 2) == 0) return;

    u8 opcode = rx->data[0];
    u8 *p = &rx->data[1];
    switch (opcode) {
    case 0: { /* LL_CONNECTION_UPDATE_REQ */
        u16 instant = btctler_little_endian_read_16(p, 9);
        if (log_tag_const_i_LL_S) {
            log_print(2, NULL, "[Info]: [LL_S]--LL_CONNECTION_UPDATE_REQ\r\n");
            log_print(2, NULL, "[Info]: [LL_S]instant=0x%x\r\n", (int)instant);
        }
        if (__instant_link_lost(instant, rx->event_count) && log_tag_const_e_LL_S)
            log_print(4, NULL, "<Error>: [LL_S]-C INSTANT_PASSED %d %d\r\n", instant, rx->event_count);
        u16 old_interval = link->ll_data.interval;
        link->ll_data.winsize = p[0];
        link->ll_data.winoffset = btctler_little_endian_read_16(p, 1);
        link->ll_data.interval = btctler_little_endian_read_16(p, 3);
        link->ll_data.latency = btctler_little_endian_read_16(p, 5);
        link->ll_data.timeout = btctler_little_endian_read_16(p, 7);
        u16 widening_before = link->ll_data.widening;
        u32 widen = (((u32)(master_sca[link->master_clock_accuracy >> 5] + 500) *
                     ((config_vendor_le_bb & 16) ? 625u : 1250u) *
                     ((u32)link->ll_data.winoffset + old_interval)) / 1000000u) << 1;
        u32 instant_widen = widen + 2;
        ll_slave_cal_widening(&link->ll_data);
        if (config_btctler_le_slave_conn_update_winden < (s32)link->ll_data.widening)
            link->ll_data.widening = (u16)config_btctler_le_slave_conn_update_winden;
        if (widening_before > instant_widen)
            link->ll_data.widening_offset = (u16)(widening_before + 65534u - widen + (instant_widen << 1));
        else
            link->ll_data.widening_offset = 0;
        __ble_ops->ioctrl(link->hw, 4, (int)instant, 0);
        __event_oneshot_add(link, __connection_upadte, (int)instant - 1);
        __event_oneshot_post_add(link, __slave_ll_receive_conn_update_req_done, instant);
        if (log_tag_const_i_LL_S) {
            log_print(2, NULL, "[Info]: [LL_S]widen [pass] = %d\r\n", (int)widening_before);
            log_print(2, NULL, "[Info]: [LL_S]widen [instant]= %d\r\n", (int)instant_widen);
            log_print(2, NULL, "[Info]: [LL_S]widen [now] == %d\r\n", (int)link->ll_data.widening);
            log_print(2, NULL, "[Info]: [LL_S]widen [offset] == %d \r\n", (int)link->ll_data.widening_offset);
            log_print(2, NULL, "[Info]: [LL_S]winoffset  == %d \r\n", (int)link->ll_data.winoffset);
        }
        break;
    }
    case 1: { /* LL_CHANNEL_MAP_REQ */
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_CHANNEL_MAP_REQ\r\n");
        memcpy(link->ll_data.channel, p, 5);
        u16 instant = btctler_little_endian_read_16(p, 5);
        if (__instant_link_lost(instant, rx->event_count)) {
            if (log_tag_const_e_LL_S)
                log_print(4, NULL, "<Error>: [LL_S]-M INSTANT_PASSED %d %d\r\n", instant, rx->event_count);
            __channel_map_upadte(link);
        } else {
            __ble_ops->ioctrl(link->hw, 4, (int)instant, 1);
            __event_oneshot_add(link, __channel_map_upadte, (int)instant - 1);
        }
        break;
    }
    case 2:
        link->reason = p[0];
        __event_oneshot_add(link, __slave_receive_terminte_ind_done, link->event_cnt);
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_TERMINATE_IND reason : %x\r\n", (int)p[0]);
        break;
    case 3:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_ENC_REQ\r\n");
        if (config_btctler_le_features & 1) {
            if (link->role != 1) {
                if (config_asser) btctrler_assert_failed("ROLE_IS_SLAVE(link)", 0);
                else cpu_reset();
            }
            memcpy(link->skdm, &rx->data[11], 8);   /* SKDm: one 8-byte copy in the SDK */
            memcpy(link->ivm, &rx->data[19], 4);    /* IVm: one 4-byte copy */
            if (link->role != 1) {
                if (config_asser) btctrler_assert_failed("ROLE_IS_SLAVE(link)", 0);
                else cpu_reset();
            }
            u32 ivs;
            u64 skds;
            pseudo_random_genrate((u8 *)&ivs, 4);
            *(u32 *)link->ivs = ivs;
            pseudo_random_genrate((u8 *)&skds, 8);
            *(u64 *)link->skds = skds;
            ll_send_control_data_callback(link, __slave_ll_send_enc_rsp_callback, 4, "c08c04", link->skds, link->ivs);
            if (!le_long_term_key_request_event(link, rx)) {
                if (config_btctler_le_features & 4)
                    ll_send_control_data(link, 17, "11", (int)opcode, 6);
                else
                    __slave_ll_send_reject_ind_auto(link, 6);
            }
        } else {
            __slave_ll_send_reject_ind_auto(link, 26);
        }
        break;
    case 4:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - S LL_ENC_RSP?\r\n");
        break;
    case 5:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - S LL_START_ENC_REQ?\r\n");
        break;
    case 6:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_START_ENC_RSP\r\n\r\n");
        if (config_btctler_le_features & 1) {
            __ble_ops->ioctrl(link->hw, 10, 1);
            link->encrypt_enable = 1;
            ll_send_control_data(link, 6, NULL);
        } else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 7:
        if (log_tag_const_i_LL_S) {
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_UNKNOWN_RSP\r\n");
            printf_buf(rx->data, rx->len);
        }
        break;
    case 8:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_FEATURE_REQ\r\n");
        memcpy(le_param.remote_features, p, 8);   /* one 8-byte copy in the SDK */
        ll_remote_feature(link);
        ll_send_control_data(link, 9, "c08", le_param.features);
        break;
    case 9:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_FEATURE_RSP\r\n");
        memcpy(le_param.remote_features, p, 8);   /* one 8-byte copy in the SDK */
        ll_remote_feature(link);
        break;
    case 10:
        if (config_btctler_le_features & 1) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]Recv - LL_PAUSE_ENC_REQ\r\n");
            __ble_ops->ioctrl(link->hw, 11, 0);
            ll_send_control_data(link, 11, NULL);
        } else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 11:
        if (config_btctler_le_features & 1) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]Recv - LL_PAUSE_ENC_RSP\r\n");
            __ble_ops->ioctrl(link->hw, 10, 0);
            link->encrypt_enable = 0;
        } else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 12:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_VERSION_IND\r\n");
        le_param.remote_versnr = p[0];
        le_param.remote_compld = btctler_little_endian_read_16(p, 1);
        le_param.remote_subversnr = btctler_little_endian_read_16(p, 3);
        /* The SDK answered only with no procedure of ours running: one that was
         * (a length request just after connecting) left the peer's exchange
         * unanswered for good, and an Android central then never encrypts. */
        if (!ll_version_exchange_pending(link))
            ll_send_control_data(link, 12, "122", (int)le_param.versnr, (int)le_param.compld, (int)le_param.subversnr);
        break;
    case 13:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]LL REJECT_IND reason:%x\n\r\n", (int)p[0]);
        break;
    case 14:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_SLAVE_FEATURE_REQ\r\n");
        break;
    case 15: {
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_CONNECTION_PARAM_REQ\r\n");
        u64 features = config_btctler_le_features;
        if ((features & 2) == 0) {
            ll_send_control_data(link, 7, "1", (int)opcode);
            break;
        }
        CONN_PARAM_RES result = __ll_receive_connection_param_req(link, rx);
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_CONNECTION_PARAM_REQ\r\n");
        if (result == 0) {
            ll_send_control_data(link, 16, "222212222222", link->conn_param_req.interval_max,
                link->conn_param_req.interval_min, link->conn_param_req.latency, link->conn_param_req.timeout,
                link->conn_param_req.PreferredPeriodicity, link->conn_param_req.ReferenceConnEventCount,
                link->conn_param_req.offset[0], link->conn_param_req.offset[1], link->conn_param_req.offset[2],
                link->conn_param_req.offset[3], link->conn_param_req.offset[4], link->conn_param_req.offset[5]);
        } else if (result == 1) {
            if (!le_remote_connection_parameter_request_event(link, rx) && (features & 4))
                ll_send_control_data(link, 17, "11", (int)opcode, 26);
        } else if (result == 2 && (features & 4)) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]reject - LL_CONNECTION_PARAM_REQ\r\n");
            ll_send_control_data(link, 17, "11", (int)opcode, 32);
        } else if (result == 3 && (features & 4)) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]invailid - LL_CONNECTION_PARAM_REQ\r\n");
            ll_send_control_data(link, 17, "11", (int)opcode, 30);
        }
        if ((features & 2) == 0 && log_tag_const_e_LL_S)
            log_print(4, NULL, "<Error>: [LL_S]UNSUPPORT LL_CONNECTION_PARAM_REQ\r\n");
        break;
    }
    case 16:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_CONNECTION_PARAM_RSP\r\n");
        break;
    case 17:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_REJECT_IND_EXT\r\n");
        break;
    case 19:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_PING_RSP\r\n");
        break;
    case 18:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_PING_REQ\r\n");
        if (config_btctler_le_features & 16) ll_send_control_data(link, 19, NULL);
        else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 20:
        if (config_btctler_le_features & 32) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]Recv - LL_LENGTH_REQ\r\n");
            __ll_receive_length_req(link, rx);
            __ll_send_length_rsp_auto(link);
            le_data_length_change_event(link);
        } else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 21:
        if (config_btctler_le_features & 32) {
            if (log_tag_const_i_LL_S)
                log_print(2, NULL, "[Info]: [LL_S]Recv - LL_LENGTH_RSP\r\n");
            __ll_receive_length_rsp(link, rx);
            le_data_length_change_event(link);
        } else ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 22:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Slave Recv - LL_PHY_REQ\r\n");
        slave_ll_receive_phy_req(link, rx);
        break;
    case 24:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Slave Recv - LL_PHY_UPDATE_IND\r\n");
        break;
    case 31:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Slave Recv - LL_PDU_CIS_REQ\r\n");
        break;
    case 33:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Slave Recv - LL_PDU_CIS_IND\r\n");
        break;
    case 64:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]--LL_VENDOR_JL_CHIP_REQ\n\r\n");
        __ll_send_vendor_jl_chip_rsp(link, rx);
        break;
    case 69:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]-- LL_VENDOR_UPDATE_REQ\n\r\n");
        if (ll_vendor_update_req_handle)
            ll_vendor_update_req_handle(rx->data, rx->len);
        ll_update_handle = link->handle;
        ll_send_control_data(link, 72, "111", get_jl_chip_id(), 1, 0);
        btctrler_hci_cmd_to_task(4194315, 0);
        break;
    case 73:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]--LL_VENDOR_BT_FRE_OFFSET\n\r\n");
        __ll_send_fre_offset_rsp(link, rx);
        break;
    case 254:
        if (log_tag_const_i_LL_S)
            log_print(2, NULL, "[Info]: [LL_S]Recv - LL_VERSION_PRIV_CMD\r\n");
        __ll_send_vendor_priv_cmd_rsp(link, rx);
        break;
    default:
        if (log_tag_const_e_LL_S) {
            log_print(4, NULL, "<Error>: [LL_S]Recv - Unkonw-opcode: %x\r\n", (int)opcode);
            printf_buf(rx->data, rx->len);
        }
        ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    }
    ll_control_step_verify((u16)(opcode | 16384), link, rx);
    ll_send_control_data_state_machine(link, rx);
}

void conn_slave_rx_post_handler(struct le_link *link) LL_CODE;
void conn_slave_rx_post_handler(struct le_link *link)
{
    __ble_ops->upload_data(link->hw, __conn_slave_rx_post_handler);
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    CHECK(ll_get_update_handle() == 0);
    ll_set_update_handle(0x1234);
    CHECK(ll_get_update_handle() == 0x1234);
    ll_set_update_handle(0xffff);
    CHECK(ll_get_update_handle() == 0xffff);
    ll_set_update_handle(0);
    CHECK(ll_get_update_handle() == 0);
    return TEST_DONE("btctrler_ll_slave");
}
#endif
