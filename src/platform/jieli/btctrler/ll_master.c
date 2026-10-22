/* BLE master (central) role link-layer receive handling: the connection
 * complete after an initiating link receives its CONNECT_IND answer, and the
 * post-processing of received data and LL control PDUs on a master link.
 * SDK quirks kept: the slave-only opcodes assert (reset when config_asser is
 * off) and then fall through to the control-step machinery; the automatic
 * connection update computes its window offset with the link's old interval
 * and in signed 32-bit arithmetic, so the remainder can be negative; the
 * instant is a 16-bit sum.  The SDK's two-line assertion print (file, line
 * and expression) is the shared btctrler_assert_failed here. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_M, log_tag_const_i_LL_M;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_btctler_le_master_multilink;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern struct le_parameter le_param;
/* The IR (and the retained callees) use unsigned bytes where sdk.h says bool or enum. */
extern u8 ll_current_no_step_u8(struct le_link *link) __asm__("ll_current_no_step");
extern u8 le_remote_connection_parameter_request_event_u8(struct le_link *link, struct ble_rx *rx)
    __asm__("le_remote_connection_parameter_request_event");

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifndef UNITTEST
_Static_assert(offsetof(struct le_link, role) == 61, "SDK link role offset");
_Static_assert(offsetof(struct le_link, event_cnt) == 64, "SDK link event_cnt offset");
_Static_assert(offsetof(struct le_link, encrypt_enable) == 67, "SDK link encrypt_enable offset");
_Static_assert(offsetof(struct le_link, current_procedure) == 116, "SDK link current_procedure offset");
_Static_assert(offsetof(struct le_link, long_term_key) == 188, "SDK link ltk offset");
_Static_assert(offsetof(struct le_link, skdm) == 204, "SDK link skdm offset");
_Static_assert(offsetof(struct le_link, skds) == 212, "SDK link skds offset");
_Static_assert(offsetof(struct le_link, ivm) == 220, "SDK link ivm offset");
_Static_assert(offsetof(struct le_link, ivs) == 224, "SDK link ivs offset");
_Static_assert(offsetof(struct le_link, conn_param_req) == 260, "SDK conn_param_req offset");
_Static_assert(offsetof(struct le_link, reason) == 313, "SDK link reason offset");
_Static_assert(offsetof(struct connection_param_request, PreferredPeriodicity) == 8, "SDK periodicity offset");
_Static_assert(offsetof(struct connection_param_request, ReferenceConnEventCount) == 10, "SDK reference count offset");
_Static_assert(offsetof(struct connection_param_request, offset) == 12, "SDK offsets offset");
#endif
_Static_assert(sizeof(((struct le_parameter *)0)->remote_features) == 8, "SDK remote features size");
_Static_assert(sizeof(((struct le_link *)0)->skds) == 8, "SDK skds size");

void master_rx_conn_post_handler(struct le_link *link, struct ble_rx *rx) LL_CODE;
void master_rx_conn_post_handler(struct le_link *link, struct ble_rx *rx)
{
    if ((config_btctler_le_roles & 1) == 0) return;
    le_ll_init_addr_process((struct le_init_link *)link, rx);
    if (ll_error_code != 0) {
        if (log_tag_const_e_LL_M)
            log_print(4, NULL, "<Error>: [LL_M]AdvA resolve fail\r\n");
        return;
    }
    ll_debug_info(link);
    if (log_tag_const_i_LL_M)
        log_print(2, NULL, "[Info]: [LL_M]link->state : %s\r\n",
                  link->state == 6 ? "LL_CONNECTION_CREATE" : "LL_CONNECTION_ESTABLISHED");
    ll_supervision_timeout_start(link, link->ll_data.timeout * 10);
    le_connection_complete_event_emit(link);
    refresh_phy_param(link, NULL, NULL);
    le_channel_selection_algorithm_event(link, rx->chsel_algorithm);
}

static void __master_receive_terminte_ind_done(struct le_link *link) LL_CODE;
static void __master_receive_terminte_ind_done(struct le_link *link)
{
    ll_disconnect_process(link, link->reason);
}

static void __master_ll_send_conn_update_req_auto_done(struct le_link *link) LL_CODE;
static void __master_ll_send_conn_update_req_auto_done(struct le_link *link)
{
    le_connection_update_complete_event(link, NULL, 0);
}

static void __master_ll_receive_start_enc_req(struct le_link *link) LL_CODE;
static void __master_ll_receive_start_enc_req(struct le_link *link)
{
    u8 iv[8];
    u8 session_key[16];
    u8 skd[16];

    memcpy(iv, link->ivm, 4);
    memcpy(iv + 4, link->ivs, 4);
    __ble_ops->ioctrl(link->hw, 0, iv);
    memcpy(skd, link->skdm, 8);
    memcpy(skd + 8, link->skds, 8);
    aes128_start(link->long_term_key, skd, session_key);
    if (log_tag_const_i_LL_M) {
        log_print(2, NULL, "[Info]: [LL_M]STK : \r\n");
        printf_buf(link->long_term_key, 16);
    }
    btctler_reverse_128(session_key, skd);
    __ble_ops->ioctrl(link->hw, 1, skd);
    __ble_ops->ioctrl(link->hw, 10, 1);
    __ble_ops->ioctrl(link->hw, 11, 1);
    link->encrypt_enable = 1;
}

static void __master_ll_send_conn_update_req_auto(struct le_link *link) LL_CODE;
static void __master_ll_send_conn_update_req_auto(struct le_link *link)
{
    u16 instant = (u16)((u16)(link->event_cnt + 10) + (u16)(link->ll_data.latency * 10));
    u16 old_interval = link->ll_data.interval;
    u16 interval_max = link->conn_param_req.interval_max;
    u16 interval_min = link->conn_param_req.interval_min;
    u16 interval = old_interval;

    if (interval_max == interval_min) {
        interval = interval_max;
        link->ll_data.interval = interval;
    } else {
        for (u16 k = 1; k != 255; k++) {
            u16 candidate = (u16)(link->conn_param_req.PreferredPeriodicity * k);
            if (candidate < interval_min || candidate > interval_max) continue;
            interval = candidate;
            link->ll_data.interval = interval;
            break;
        }
    }

    u16 reference = link->conn_param_req.ReferenceConnEventCount;
    u16 offset0 = link->conn_param_req.offset[0];
    int t;
    if (reference > instant) {
        t = (((int)reference - (int)instant) * (int)old_interval + (int)offset0) % (int)interval;
    } else {
        int rem = (((int)instant - (int)reference) * (int)old_interval - (int)offset0) % (int)interval;
        t = (int)interval - rem;
    }

    u8 winsize;
    u16 winoffset;
    if (((u32)t & 65532) > 3) {
        winoffset = (u16)(t + 65535);
        winsize = 2;
    } else {
        winoffset = 0;
        winsize = 4;
    }
    link->ll_data.winoffset = winoffset;
    link->ll_data.winsize = winsize;
    u16 latency = link->conn_param_req.latency;
    link->ll_data.latency = latency;
    u16 timeout = link->conn_param_req.timeout;
    link->ll_data.timeout = timeout;
    if (config_btctler_le_master_multilink != 0) {
        link->ll_data.winoffset = 0;
        winoffset = 0;
    }
    ll_send_control_data(link, 0, "122222", (int)winsize, (int)winoffset, (int)interval, (int)latency,
                         (int)timeout, (int)instant);
    __ble_ops->ioctrl(link->hw, 4, (int)instant, 0);
    __event_oneshot_add(link, __connection_upadte, (int)instant - 1);
    __event_oneshot_post_add(link, __master_ll_send_conn_update_req_auto_done, (int)instant);
}

/* A master that was sent a slave-only control opcode. */
static inline __attribute__((always_inline)) void ll_m_assert(const char *what)
{
    btctrler_assert_failed(what, 0);
}

static void __conn_master_rx_post_handler(void *priv, struct ble_rx *rx) LL_CODE;
static void __conn_master_rx_post_handler(void *priv, struct ble_rx *rx)
{
    struct le_link *link = priv;

    switch (rx->llid) {
    case 0:
        return;
    case 1:
    case 2:
        conn_rx_acl_u_post_handler(link, rx);
        return;
    default:
        break;
    }

    if ((config_btctler_le_roles & 1) == 0) return;
    u8 opcode = rx->data[0];
    u8 *params = &rx->data[1];
    switch (opcode) {
    case 0:
        ll_m_assert("M LL_CONNECTION_UPDATE_REQ");
        break;
    case 1:
        ll_m_assert("M LL_CHANNEL_MAP_REQ");
        break;
    case 2:
        link->reason = params[0];
        __event_oneshot_add(link, __master_receive_terminte_ind_done, link->event_cnt + 1);
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Recv - LL_TERMINATE_IND reason : %x\r\n", (int)params[0]);
        break;
    case 3:
        ll_m_assert("M LL_ENC_REQ");
        break;
    case 4:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Recv - LL_ENC_RSP\r\n");
        if (config_btctler_le_features & 1) {
            if (link->role == 1) btctrler_assert_failed("!ROLE_IS_SLAVE(link)", 0);
            memcpy(link->skds, params, 8);
            memcpy(link->ivs, &rx->data[9], 4);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 5:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Recv - LL_START_ENC_REQ\r\n");
        if (config_btctler_le_features & 1) {
            __master_ll_receive_start_enc_req(link);
            ll_send_control_data(link, 6, NULL);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 6:
    case 13:
    case 17:
    case 19:
        break;
    case 7:
        if (log_tag_const_i_LL_M) {
            log_print(2, NULL, "[Info]: [LL_M]Recv - LL_UNKNOWN_RSP\r\n");
            printf_buf(rx->data, rx->len);
        }
        break;
    case 8:
        ll_m_assert("M LL_FEATURE_REQ");
        break;
    case 9:
        memcpy(le_param.remote_features, params, 8);
        ll_remote_feature(link);
        break;
    case 10:
        ll_m_assert("M LL_PAUSE_ENC_REQ");
        break;
    case 11:
        if (config_btctler_le_features & 1) {
            __ble_ops->ioctrl(link->hw, 10, 0);
            __ble_ops->ioctrl(link->hw, 11, 0);
            link->encrypt_enable = 0;
            ll_send_control_data(link, 11, NULL);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 12:
        le_param.remote_versnr = params[0];
        le_param.remote_compld = btctler_little_endian_read_16(params, 1);
        le_param.remote_subversnr = btctler_little_endian_read_16(params, 3);
        if (ll_current_no_step_u8(link) != 0)
            ll_send_control_data(link, 12, "122", (int)le_param.versnr, (int)le_param.compld,
                                 (int)le_param.subversnr);
        break;
    case 14:
        if (config_btctler_le_features & 8) {
            memcpy(le_param.remote_features, params, 8);
            ll_remote_feature(link);
            ll_send_control_data(link, 9, "c08", le_param.features);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 15: {
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Recv - M CONNECTION_PARAM_REQ\r\n");
        u32 procedure = link->current_procedure;
        int no_param_req = (config_btctler_le_features & 4) == 0;
        if (!((((s16)procedure >= 0) && (procedure & 1) == 0) || no_param_req))
            ll_send_control_data(link, 17, "11", (int)opcode, 35);
        if (!(no_param_req || (link->current_procedure & 2) == 0))
            ll_send_control_data(link, 17, "11", (int)opcode, 42);
        if ((config_btctler_le_features & 2) == 0) {
            ll_send_control_data(link, 7, "1", (int)opcode);
            break;
        }
        switch (__ll_receive_connection_param_req(link, rx)) {
        case 0:
            __master_ll_send_conn_update_req_auto(link);
            break;
        case 1: {
            /* The SDK asks the host even when the reject is disabled. */
            u8 host_answered = le_remote_connection_parameter_request_event_u8(link, rx);
            if (!(no_param_req || host_answered != 0))
                ll_send_control_data(link, 17, "11", (int)opcode, 26);
            break;
        }
        case 2:
            if (!no_param_req) {
                if (log_tag_const_i_LL_M)
                    log_print(2, NULL, "[Info]: [LL_M]reject - LL_CONNECTION_PARAM_REQ\r\n");
                ll_send_control_data(link, 17, "11", (int)opcode, 32);
            }
            break;
        case 3:
            if (!no_param_req) {
                if (log_tag_const_i_LL_M)
                    log_print(2, NULL, "[Info]: [LL_M]invailid - LL_CONNECTION_PARAM_REQ\r\n");
                ll_send_control_data(link, 17, "11", (int)opcode, 30);
            }
            break;
        default:
            break;
        }
        break;
    }
    case 16:
        if (config_btctler_le_features & 2) {
            link->conn_param_req.interval_min = btctler_little_endian_read_16(params, 0);
            link->conn_param_req.interval_max = btctler_little_endian_read_16(params, 2);
            link->conn_param_req.latency = btctler_little_endian_read_16(params, 4);
            link->conn_param_req.timeout = btctler_little_endian_read_16(params, 6);
            link->conn_param_req.PreferredPeriodicity = rx->data[9];
            link->conn_param_req.ReferenceConnEventCount = btctler_little_endian_read_16(params, 9);
            for (int i = 0; i != 6; i++)
                link->conn_param_req.offset[i] = btctler_little_endian_read_16(params, (i << 1) + 11);
            if (ll_current_no_step_u8(link) != 0)
                __master_ll_send_conn_update_req_auto(link);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 18:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Recv - LL_PING_REQ\r\n");
        if (config_btctler_le_features & 16)
            ll_send_control_data(link, 19, NULL);
        else
            ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    case 20:
        if (config_btctler_le_features & 32) {
            if (log_tag_const_i_LL_M)
                log_print(2, NULL, "[Info]: [LL_M]Recv - LL_LENGTH_REQ\r\n");
            __ll_receive_length_req(link, rx);
            __ll_send_length_rsp_auto(link);
            le_data_length_change_event(link);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 21:
        if (config_btctler_le_features & 32) {
            if (log_tag_const_i_LL_M)
                log_print(2, NULL, "[Info]: [LL_M]Recv - LL_LENGTH_RSP\r\n");
            __ll_receive_length_rsp(link, rx);
            le_data_length_change_event(link);
        } else {
            ll_send_control_data(link, 7, "1", (int)opcode);
        }
        break;
    case 22:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Master Recv - LL_PHY_REQ\r\n");
        master_ll_receive_phy_req(link, rx);
        break;
    case 23:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Master Recv - LL_PHY_RSP\r\n");
        master_ll_receive_phy_rsp(link, params);
        break;
    case 64:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]--LL_VENDOR_JL_CHIP_REQ\n\r\n");
        __ll_send_vendor_jl_chip_rsp(link, rx);
        break;
    case 73:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]--LL_VENDOR_BT_FRE_OFFSET\n\r\n");
        __ll_send_fre_offset_rsp(link, rx);
        break;
    case 32:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Master Recv - LL_PDU_CIS_RSP\r\n");
        break;
    case 81:
        if (log_tag_const_i_LL_M)
            log_print(2, NULL, "[Info]: [LL_M]Master Recv - LL_VENDOR_WIFI_DETECT_REQ\r\n");
        ll_master_rx_wifi_detect_report(link, params, (u16)(rx->len - 1));
        break;
    default:
        if (log_tag_const_e_LL_M) {
            log_print(4, NULL, "<Error>: [LL_M]Recv - Unkonw-opcode: %x\r\n", (int)opcode);
            printf_buf(rx->data, rx->len);
        }
        ll_send_control_data(link, 7, "1", (int)opcode);
        break;
    }
    ll_control_step_verify((u16)(opcode | 16384), link, rx);
    ll_send_control_data_state_machine(link, rx);
}

void conn_master_rx_post_handler(struct le_link *link) LL_CODE;
void conn_master_rx_post_handler(struct le_link *link)
{
    __ble_ops->upload_data(link->hw, __conn_master_rx_post_handler);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdio.h>

struct le_parameter le_param;
const struct ble_operation *__ble_ops;
u8 log_tag_const_e_LL_M, log_tag_const_i_LL_M;
u64 config_btctler_le_features = 0xffff;
int config_btctler_le_master_multilink;
int config_btctler_le_roles = 1;
u8 ll_error_code;

static char trace[2048];
static void TR(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(trace);
    va_start(ap, fmt);
    vsnprintf(trace + n, sizeof(trace) - n, fmt, ap);
    va_end(ap);
}
static int no_step_result = 1, param_req_result, param_event_result;
static int logs_e, logs_i;
static void (*oneshot_funcs[8])(struct le_link *);
static int oneshot_args[8], noneshots;
static void (*post_funcs[8])(struct le_link *);
static int post_args[8];

void log_print(int level, const char *tag, const char *format, ...) { (void)tag; (void)format; if (level == 4) logs_e++; else logs_i++; }
void printf_buf(const void *b, int n) { (void)b; TR("pbuf(%d) ", n); }
void btctrler_assert_failed(const char *what, int value) { CHECK(value == 0); TR("assert(%s) ", what); }
void le_ll_init_addr_process(struct le_init_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("addrproc "); }
void ll_debug_info(struct le_link *l) { (void)l; TR("dbg "); }
void ll_supervision_timeout_start(struct le_link *l, u32 t) { (void)l; TR("svto(%u) ", t); }
void le_connection_complete_event_emit(struct le_link *l) { (void)l; TR("complete "); }
void refresh_phy_param(struct le_link *l, u8 *a, u8 *b) { (void)l; CHECK(!a && !b); TR("phy "); }
void le_channel_selection_algorithm_event(struct le_link *l, u8 a) { (void)l; TR("csa(%u) ", a); }
void conn_rx_acl_u_post_handler(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("acl_u "); }
void ll_control_step_verify(ll_step_extend s, struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("verify(%x) ", (unsigned)s); }
void ll_send_control_data_state_machine(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("sm "); }
void ll_send_control_data(struct le_link *l, int op, const char *fmt, ...)
{
    va_list ap;
    (void)l;
    va_start(ap, fmt);
    TR("scd(%d", op);
    if (fmt && !strcmp(fmt, "c08")) { (void)va_arg(ap, void *); TR(",ptr"); }
    else for (const char *c = fmt; c && *c; c++) TR(",%d", va_arg(ap, int));
    va_end(ap);
    TR(") ");
}
void master_ll_receive_phy_req(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("phyreq "); }
void master_ll_receive_phy_rsp(struct le_link *l, u8 *d) { (void)l; TR("phyrsp(%u) ", d[0]); }
void __ll_send_vendor_jl_chip_rsp(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("jlchip "); }
void __ll_send_fre_offset_rsp(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("freoff "); }
void ll_master_rx_wifi_detect_report(struct le_link *l, u8 *b, u16 len) { (void)l; TR("wifi(%u,%u) ", b[0], len); }
void __event_oneshot_add(struct le_link *l, void (*f)(struct le_link *), int instant) { (void)l; oneshot_funcs[noneshots] = f; oneshot_args[noneshots++] = instant; TR("oneshot(%d) ", instant); }
void __event_oneshot_post_add(struct le_link *l, void (*f)(struct le_link *), int instant) { (void)l; post_funcs[0] = f; post_args[0] = instant; TR("post(%d) ", instant); }
void __connection_upadte(struct le_link *l) { (void)l; }
void ll_disconnect_process(struct le_link *l, u8 reason) { (void)l; TR("disc(%u) ", reason); }
void aes128_start(u8 *key, u8 *pt, u8 *out) { TR("aes "); for (int i = 0; i < 16; i++) out[i] = (u8)(key[i] ^ pt[i]); }
void btctler_reverse_128(const uint8_t *src, uint8_t *dst) { TR("rev "); for (int i = 0; i < 16; i++) dst[i] = src[15 - i]; }
void ll_remote_feature(struct le_link *l) { (void)l; TR("remfeat "); }
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return (uint16_t)(b[pos] | b[pos + 1] << 8); }
u8 ll_current_no_step_u8(struct le_link *l) { (void)l; return (u8)no_step_result; }
CONN_PARAM_RES __ll_receive_connection_param_req(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("pr_req "); return (CONN_PARAM_RES)param_req_result; }
u8 le_remote_connection_parameter_request_event_u8(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("pr_event "); return (u8)param_event_result; }
void le_connection_update_complete_event(struct le_link *l, struct ble_rx *rx, LL_CONTROL_CASE s) { (void)l; CHECK(!rx); TR("updcomplete(%d) ", (int)s); }
void __ll_receive_length_req(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("lenreq "); }
void __ll_send_length_rsp_auto(struct le_link *l) { (void)l; TR("lenrsp_auto "); }
void le_data_length_change_event(struct le_link *l) { (void)l; TR("lenchg "); }
void __ll_receive_length_rsp(struct le_link *l, struct ble_rx *rx) { (void)l; (void)rx; TR("lenrsp "); }

static void m_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    CHECK(hw == (void *)0x4444);
    if (cmd == 0 || cmd == 1) { u8 *p = va_arg(ap, u8 *); TR("ioctrl(%d,%02x%02x%02x%02x%02x%02x%02x%02x) ", cmd, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
    else if (cmd == 4) { int a = va_arg(ap, int); int b = va_arg(ap, int); TR("ioctrl(4,%d,%d) ", a, b); }
    else { int a = va_arg(ap, int); TR("ioctrl(%d,%d) ", cmd, a); }
    va_end(ap);
}
static void (*uploaded)(void *, struct ble_rx *);
static void m_upload(void *hw, void (*cb)(void *, struct ble_rx *)) { CHECK(hw == (void *)0x4444); uploaded = cb; TR("upload "); }

static struct { struct ble_rx rx; u8 buf[40]; } R;
static struct le_link L;

static void put(int llid, int len, const u8 *pdu)
{
    memset(&R, 0, sizeof(R));
    R.rx.llid = llid;
    R.rx.len = (u8)len;
    memcpy(R.rx.data, pdu, (size_t)len);
    trace[0] = 0;
}
static const char *run(u8 opcode, const u8 *params, int nparams)
{
    u8 pdu[40];
    pdu[0] = opcode;
    memcpy(pdu + 1, params, (size_t)nparams);
    put(3, nparams + 1, pdu);
    uploaded(&L, &R.rx);
    return trace;
}
static void auto_update(u16 reference, u16 offset0)
{
    L.event_cnt = 100; L.ll_data.latency = 3; L.ll_data.interval = 24;
    L.conn_param_req.ReferenceConnEventCount = reference;
    L.conn_param_req.offset[0] = offset0;
    run(15, NULL, 0);
}
#define IS(str) CHECK(!strcmp(trace, str))
#define RUN(op, p, n, str) do { run(op, p, n); if (strcmp(trace, str)) printf("  got: %s\n", trace); CHECK(!strcmp(trace, str)); } while (0)

int main(void)
{
    struct ble_operation ops;
    memset(&ops, 0, sizeof(ops));
    ops.ioctrl = m_ioctrl;
    ops.upload_data = m_upload;
    __ble_ops = &ops;
    L.hw = (void *)0x4444;
    trace[0] = 0;
    conn_master_rx_post_handler(&L);
    IS("upload ");
    CHECK(uploaded != NULL);

    /* master_rx_conn_post_handler */
    L.ll_data.timeout = 400;
    L.state = 6;
    R.rx.chsel_algorithm = 1;
    trace[0] = 0;
    master_rx_conn_post_handler(&L, &R.rx);
    IS("addrproc dbg svto(4000) complete phy csa(1) ");
    ll_error_code = 7;
    log_tag_const_e_LL_M = 1;
    trace[0] = 0;
    master_rx_conn_post_handler(&L, &R.rx);
    IS("addrproc ");
    CHECK(logs_e == 1);
    log_tag_const_e_LL_M = 0;
    master_rx_conn_post_handler(&L, &R.rx);
    CHECK(logs_e == 1);
    ll_error_code = 0;
    config_btctler_le_roles = 2;
    trace[0] = 0;
    master_rx_conn_post_handler(&L, &R.rx);
    IS("");
    config_btctler_le_roles = 1;
    log_tag_const_i_LL_M = 1;
    L.ll_data.timeout = 0xffff;
    master_rx_conn_post_handler(&L, &R.rx);
    CHECK(logs_i == 1 && strstr(trace, "svto(655350)"));
    log_tag_const_i_LL_M = 0;

    /* Data and control dispatch by llid. */
    put(0, 3, (const u8 *)"abc"); uploaded(&L, &R.rx); IS("");
    put(1, 3, (const u8 *)"abc"); uploaded(&L, &R.rx); IS("acl_u ");
    put(2, 3, (const u8 *)"abc"); uploaded(&L, &R.rx); IS("acl_u ");
    config_btctler_le_roles = 2;
    run(18, NULL, 0); IS("");
    config_btctler_le_roles = 1;

    /* Slave-only opcodes assert; the step machinery still runs, opcode | 0x4000. */
    RUN(0, NULL, 0, "assert(M LL_CONNECTION_UPDATE_REQ) verify(4000) sm ");
    RUN(1, NULL, 0, "assert(M LL_CHANNEL_MAP_REQ) verify(4001) sm ");
    RUN(3, NULL, 0, "assert(M LL_ENC_REQ) verify(4003) sm ");
    RUN(8, NULL, 0, "assert(M LL_FEATURE_REQ) verify(4008) sm ");
    RUN(10, NULL, 0, "assert(M LL_PAUSE_ENC_REQ) verify(400a) sm ");

    /* Terminate: reason stored, disconnect scheduled for event_cnt + 1. */
    L.event_cnt = 65535;
    RUN(2, (const u8 *)"\x13", 1, "oneshot(65536) verify(4002) sm ");
    CHECK(L.reason == 0x13 && oneshot_funcs[0] != NULL);
    trace[0] = 0;
    oneshot_funcs[0](&L);
    IS("disc(19) ");
    noneshots = 0;

    /* Encryption response, start request, pause response. */
    u8 enc[16];
    for (int i = 0; i < 16; i++) enc[i] = (u8)(0x10 + i);
    L.role = 0;
    RUN(4, enc, 12, "verify(4004) sm ");
    CHECK(!memcmp(L.skds, enc, 8) && !memcmp(L.ivs, enc + 8, 4));
    L.role = 1;
    RUN(4, enc, 12, "assert(!ROLE_IS_SLAVE(link)) verify(4004) sm ");
    config_btctler_le_features = 0xfffe;
    RUN(4, enc, 12, "scd(7,4) verify(4004) sm ");
    config_btctler_le_features = 0xffff;
    for (int i = 0; i < 4; i++) L.ivm[i] = (u8)(0xa0 + i), L.ivs[i] = (u8)(0xb0 + i);
    for (int i = 0; i < 8; i++) L.skdm[i] = (u8)(0x20 + i), L.skds[i] = (u8)(0x30 + i);
    for (int i = 0; i < 16; i++) L.long_term_key[i] = (u8)i;
    RUN(5, NULL, 0, "ioctrl(0,a0a1a2a3b0b1b2b3) aes rev ioctrl(1,3838383838383838) ioctrl(10,1) ioctrl(11,1) scd(6) verify(4005) sm ");
    CHECK(L.encrypt_enable == 1);
    config_btctler_le_features = 0xfffe;
    RUN(5, NULL, 0, "scd(7,5) verify(4005) sm ");
    config_btctler_le_features = 0xffff;
    RUN(11, NULL, 0, "ioctrl(10,0) ioctrl(11,0) scd(11) verify(400b) sm ");
    CHECK(L.encrypt_enable == 0);
    config_btctler_le_features = 0xfffe;
    RUN(11, NULL, 0, "scd(7,11) verify(400b) sm ");
    config_btctler_le_features = 0xffff;

    /* Opcodes that only step the machinery. */
    RUN(6, NULL, 0, "verify(4006) sm ");
    RUN(13, NULL, 0, "verify(400d) sm ");
    RUN(17, NULL, 0, "verify(4011) sm ");
    RUN(19, NULL, 0, "verify(4013) sm ");
    log_tag_const_i_LL_M = 1;
    RUN(7, (const u8 *)"\x02\x03", 2, "pbuf(3) verify(4007) sm ");
    RUN(32, NULL, 0, "verify(4020) sm ");
    log_tag_const_i_LL_M = 0;

    /* Features and version. */
    u8 feat[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    RUN(9, feat, 8, "remfeat verify(4009) sm ");
    CHECK(!memcmp(le_param.remote_features, feat, 8));
    config_btctler_le_features = 8;
    memset(le_param.remote_features, 0, 8);
    le_param.features[0] = 0x55;
    RUN(14, feat, 8, "remfeat scd(9,ptr) verify(400e) sm ");
    CHECK(!memcmp(le_param.remote_features, feat, 8));
    config_btctler_le_features = 0;
    RUN(14, feat, 8, "scd(7,14) verify(400e) sm ");
    config_btctler_le_features = 0xffff;
    le_param.versnr = 9; le_param.compld = 0x0102; le_param.subversnr = 0x0304;
    RUN(12, (const u8 *)"\x0a\x34\x12\x78\x56", 5, "scd(12,9,258,772) verify(400c) sm ");
    CHECK(le_param.remote_versnr == 10 && le_param.remote_compld == 0x1234 && le_param.remote_subversnr == 0x5678);
    no_step_result = 0;
    RUN(12, (const u8 *)"\x0b\x34\x12\x78\x56", 5, "verify(400c) sm ");
    CHECK(le_param.remote_versnr == 11);
    no_step_result = 1;

    /* Ping, length, PHY and vendor opcodes. */
    RUN(18, NULL, 0, "scd(19) verify(4012) sm ");
    config_btctler_le_features = 0xffef;
    RUN(18, NULL, 0, "scd(7,18) verify(4012) sm ");
    config_btctler_le_features = 0xffdf;
    RUN(20, NULL, 0, "scd(7,20) verify(4014) sm ");
    RUN(21, NULL, 0, "scd(7,21) verify(4015) sm ");
    config_btctler_le_features = 0xffff;
    RUN(20, NULL, 0, "lenreq lenrsp_auto lenchg verify(4014) sm ");
    RUN(21, NULL, 0, "lenrsp lenchg verify(4015) sm ");
    RUN(22, NULL, 0, "phyreq verify(4016) sm ");
    RUN(23, (const u8 *)"\x05", 1, "phyrsp(5) verify(4017) sm ");
    RUN(64, NULL, 0, "jlchip verify(4040) sm ");
    RUN(73, NULL, 0, "freoff verify(4049) sm ");
    RUN(81, (const u8 *)"\x09\x08\x07", 3, "wifi(9,3) verify(4051) sm ");
    RUN(81, NULL, 0, "wifi(0,0) verify(4051) sm ");
    log_tag_const_e_LL_M = 1;
    logs_e = 0;
    RUN(99, (const u8 *)"\x01\x02", 2, "pbuf(3) scd(7,99) verify(4063) sm ");
    CHECK(logs_e == 1);
    log_tag_const_e_LL_M = 0;
    RUN(99, NULL, 0, "scd(7,99) verify(4063) sm ");

    /* Connection parameter request: reject checks, then the handler's verdict. */
    L.current_procedure = 1;
    param_req_result = 9;
    RUN(15, NULL, 0, "scd(17,15,35) pr_req verify(400f) sm ");
    L.current_procedure = 0x8000;
    RUN(15, NULL, 0, "scd(17,15,35) pr_req verify(400f) sm ");
    L.current_procedure = 2;
    RUN(15, NULL, 0, "scd(17,15,42) pr_req verify(400f) sm ");
    L.current_procedure = 3;
    RUN(15, NULL, 0, "scd(17,15,35) scd(17,15,42) pr_req verify(400f) sm ");
    L.current_procedure = 0;
    param_req_result = 1; param_event_result = 0;
    RUN(15, NULL, 0, "pr_req pr_event scd(17,15,26) verify(400f) sm ");
    param_event_result = 2;
    RUN(15, NULL, 0, "pr_req pr_event verify(400f) sm ");
    param_req_result = 2;
    RUN(15, NULL, 0, "pr_req scd(17,15,32) verify(400f) sm ");
    param_req_result = 3;
    RUN(15, NULL, 0, "pr_req scd(17,15,30) verify(400f) sm ");
    config_btctler_le_features = 0xfffb;      /* no bit 2: no rejects; the event is still asked for */
    L.current_procedure = 3;
    param_req_result = 1; param_event_result = 0;
    RUN(15, NULL, 0, "pr_req pr_event verify(400f) sm ");
    param_req_result = 2;
    RUN(15, NULL, 0, "pr_req verify(400f) sm ");
    param_req_result = 3;
    RUN(15, NULL, 0, "pr_req verify(400f) sm ");
    config_btctler_le_features = 0xfffd;      /* no bit 1: unknown response */
    RUN(15, NULL, 0, "scd(17,15,35) scd(17,15,42) scd(7,15) verify(400f) sm ");
    config_btctler_le_features = 0xffff;
    L.current_procedure = 0;
    log_tag_const_i_LL_M = 1;
    param_req_result = 2;
    logs_i = 0;
    RUN(15, NULL, 0, "pr_req scd(17,15,32) verify(400f) sm ");
    CHECK(logs_i == 2);
    log_tag_const_i_LL_M = 0;

    /* Connection parameter response stores the request and updates the connection.
     * interval 30..40, preferred periodicity 8: first multiple in range is 32.
     * Instant = (100 + 10) + 3 * 10 = 140; old interval 24. */
    u8 rsp[32];
    memset(rsp, 0, sizeof(rsp));
    rsp[0] = 30; rsp[2] = 40; rsp[4] = 5; rsp[6] = 0xf4; rsp[7] = 0x01;          /* min 30, max 40, latency 5, timeout 500 */
    rsp[8] = 8;                                                                    /* periodicity */
    rsp[9] = 150;                                                                  /* reference 150 */
    for (int i = 0; i < 6; i++) { rsp[11 + 2 * i] = (u8)(10 + i); }                /* offsets 10..15 */
    L.event_cnt = 100; L.ll_data.latency = 3; L.ll_data.interval = 24;
    run(16, rsp, 23);
    CHECK(L.conn_param_req.interval_min == 30 && L.conn_param_req.interval_max == 40 && L.conn_param_req.latency == 5);
    CHECK(L.conn_param_req.timeout == 500 && L.conn_param_req.PreferredPeriodicity == 8);
    CHECK(L.conn_param_req.ReferenceConnEventCount == 150);
    for (int i = 0; i < 6; i++) CHECK(L.conn_param_req.offset[i] == 10 + i);
    /* reference 150 > 140: t = ((150-140)*24 + 10) % 32 = 250 % 32 = 26; winoffset 25, winsize 2 */
    CHECK(strstr(trace, "scd(0,2,25,32,5,500,140) ioctrl(4,140,0) oneshot(139) post(140) verify(4010) sm ") != NULL);
    CHECK(L.ll_data.interval == 32 && L.ll_data.winoffset == 25 && L.ll_data.winsize == 2);
    CHECK(L.ll_data.latency == 5 && L.ll_data.timeout == 500);
    CHECK(noneshots == 1 && post_funcs[0] != NULL);
    trace[0] = 0;
    post_funcs[0](&L);
    IS("updcomplete(0) ");
    noneshots = 0;

    /* No step in progress: stored but nothing sent. */
    no_step_result = 0;
    run(16, rsp, 23);
    IS("verify(4010) sm ");
    no_step_result = 1;
    config_btctler_le_features = 0xfffd;
    run(16, rsp, 23);
    IS("scd(7,16) verify(4010) sm ");
    config_btctler_le_features = 0xffff;

    /* Automatic update (through opcode 15 with a "no conflict" verdict).  Every case starts from
     * event 100, latency 3 (instant 140), old interval 24, request 30..40 / periodicity 8 / latency 5 /
     * timeout 500. */
    param_req_result = 0;
#define AUTO(ref, off0) auto_update(ref, off0)
    AUTO(150, 10);   /* t = (10*24 + 10) % 32 = 26 -> window 25 */
    CHECK(strstr(trace, "pr_req scd(0,2,25,32,5,500,140) ioctrl(4,140,0) oneshot(139) post(140) ") != NULL);
    AUTO(100, 10);   /* t = 32 - ((40*24 - 10) % 32) = 32 - 22 = 10 -> window 9 */
    CHECK(strstr(trace, "scd(0,2,9,32,5,500,140)") != NULL);
    AUTO(138, 17);   /* t = 32 - ((2*24 - 17) % 32) = 1 -> no window, size 4 */
    CHECK(strstr(trace, "scd(0,4,0,32,5,500,140)") != NULL);
    CHECK(L.ll_data.winoffset == 0 && L.ll_data.winsize == 4);
    AUTO(140, 2);    /* equal: (0*24 - 2) % 32 = -2 (signed), t = 34 -> window 33 */
    CHECK(strstr(trace, "scd(0,2,33,32,5,500,140)") != NULL);
    config_btctler_le_master_multilink = 1;
    AUTO(150, 10);   /* multilink: stored and sent window offsets are 0 */
    CHECK(strstr(trace, "scd(0,2,0,32,5,500,140)") != NULL && L.ll_data.winoffset == 0 && L.ll_data.winsize == 2);
    config_btctler_le_master_multilink = 0;
    L.conn_param_req.interval_min = L.conn_param_req.interval_max = 36;
    AUTO(150, 10);   /* equal min and max: 36 without a search; t = 250 % 36 = 34 -> window 33 */
    CHECK(L.ll_data.interval == 36 && strstr(trace, "scd(0,2,33,36,5,500,140)") != NULL);
    L.conn_param_req.PreferredPeriodicity = 0;
    L.conn_param_req.interval_min = 30; L.conn_param_req.interval_max = 40;
    AUTO(150, 10);   /* no candidate (0 is below the minimum): old interval 24; t = 250 % 24 = 10 -> window 9 */
    CHECK(L.ll_data.interval == 24 && strstr(trace, "scd(0,2,9,24,5,500,140)") != NULL);
    L.conn_param_req.PreferredPeriodicity = 8;
    L.event_cnt = 65530; L.ll_data.latency = 1;
    run(15, NULL, 0);    /* 16-bit instant: (65530 + 10) wraps to 4, + 10 = 14 */
    CHECK(strstr(trace, "ioctrl(4,14,0) oneshot(13) post(14)") != NULL);
    L.event_cnt = 65526; L.ll_data.latency = 0;
    run(15, NULL, 0);    /* instant 0: the oneshot gets -1 */
    CHECK(strstr(trace, "ioctrl(4,0,0) oneshot(-1) post(0)") != NULL);
    return TEST_DONE("btctrler_ll_master");
}
#endif
