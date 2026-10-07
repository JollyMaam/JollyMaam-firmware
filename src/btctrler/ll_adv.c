/* Legacy BLE advertiser state and packet handling. Keeps the SDK's legacy
 * advertising type mappings, timer lifetime, filtering and privacy decisions. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_ADV, log_tag_const_i_LL_ADV;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG u32 config_vendor_le_bb;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const u8 *data, int len);
extern int putchar(int c);
extern struct le_parameter le_param;

#define LL_CODE __attribute__((section(".ble_ll_code")))

static u8 adv_not_connect_same_master __attribute__((section(".ble_ll_bss"))) = 0;
static struct ll_adv_hdl *adv_hdl __attribute__((section(".ble_ll_data"), aligned(4))) = NULL;
static struct advertising_param adv_param __attribute__((section(".ble_ll_bss"))) = {0};
static struct advertising_data adv_data __attribute__((section(".ble_ll_bss"))) = {0};
static struct scan_response scan_resp_data __attribute__((section(".ble_ll_bss"))) = {0};
static void adv_rx_probe_handler(void *priv, struct ble_rx *rx) LL_CODE;
static const struct ble_handler adv_handler
    __attribute__((section(".ble_ll_const"), aligned(4))) = {
        .rx_probe_handler = adv_rx_probe_handler,
};

static void ll_adv_init(void) LL_CODE;
static void ll_adv_resume(void) LL_CODE;
static void __adv_rx_post_handler(void *priv, struct ble_rx *rx) LL_CODE;
static int __set_ll_adv_state(struct le_adv_link *adv_link) LL_CODE;
static void __set_ll_adv_local_addr(struct le_adv_link *adv_link) LL_CODE;
static void __set_ll_adv_peer_addr(struct le_adv_link *adv_link) LL_CODE;
static void ll_adv_timeout_handler(void *priv) LL_CODE;

void ll_adv_timeout_stop(struct le_adv_link *adv_link) LL_CODE;
void ll_adv_timeout_stop(struct le_adv_link *adv_link)
{
    if (adv_link->timeout) {
        sys_timer_remove(adv_link->timeout);
        adv_link->timeout = 0;
    }
}

void le_ll_adv_addr_process(void *priv, struct ble_rx *rx) LL_CODE;
void le_ll_adv_addr_process(void *priv, struct ble_rx *rx)
{
    struct resolving_list *resolved = NULL;
    u8 type = rx->type;

    if (type != LL_PDU_SCAN_REQ && type != LL_PDU_CONNECT_REQ)
        btctrler_assert_failed("((rx->type == SCAN_REQ) || (rx->type == CONNECT_REQ)) packet filter err %s", 334);

    ll_error_code = 0;
    if (config_btctler_le_features & 64) {
        if (!le_param.resolution_enable) {
            if (log_tag_const_e_LL_ADV)
                log_print(4, NULL, "<Error>: [LL_ADV]resolution still disable\r\n");
        } else if (!(resolved = ll_resolve_peer_addr(rx->data))) {
            ll_error_code = RESOLVE_ADDR_FAIL;
            if (log_tag_const_e_LL_ADV)
                log_print(4, NULL, "<Error>: [LL_ADV]RESOLVE_ADDR_FAIL\r\n");
        }
    }

    /* The SDK clears the resolution error here before running list filters. */
    ll_error_code = 0;
    if ((adv_param.Advertising_Filter_Policy & 1) && (type == LL_PDU_SCAN_REQ)) {
        int should_match = !(config_btctler_le_features & 64) ||
                           (resolved && le_param.resolution_enable);
        struct white_list *match = NULL;
        if (should_match) {
            if (config_btctler_le_features & 64)
                match = ll_white_list_match(priv, resolved->resolving_list_param.Peer_Identity_Address_Type,
                                           resolved->resolving_list_param.Peer_Identity_Address);
            else
                match = ll_white_list_match(priv, rx->txadd, rx->data);
        }
        if (should_match && !match) {
            if (log_tag_const_e_LL_ADV)
                log_print(4, NULL, "<Error>: [LL_ADV]filter policy : SCAN_REQ\r\n<Error>: [LL_ADV]white_list_t NULL : DEVICE_FILTER_FAILED\r\n");
            ll_error_code = DEVICE_FILTER_FAILED;
        }
    }
    if ((adv_param.Advertising_Filter_Policy & 2) && (type == LL_PDU_CONNECT_REQ)) {
        int should_match = !(config_btctler_le_features & 64) ||
                           (resolved && le_param.resolution_enable);
        struct white_list *match = NULL;
        if (should_match) {
            if (config_btctler_le_features & 64)
                match = ll_white_list_match(priv, resolved->resolving_list_param.Peer_Identity_Address_Type,
                                           resolved->resolving_list_param.Peer_Identity_Address);
            else
                match = ll_white_list_match(priv, rx->txadd, rx->data);
        }
        if (should_match && !match) {
            if (log_tag_const_e_LL_ADV)
                log_print(4, NULL, "<Error>: [LL_ADV]filter policy : CONNECT_REQ\r\n<Error>: [LL_ADV]white_list_t NULL : DEVICE_FILTER_FAILED\r\n");
            ll_error_code = DEVICE_FILTER_FAILED;
        }
    }
}

int adv_rx_post_handler(void) LL_CODE;
int adv_rx_post_handler(void)
{
    /* The SDK leaves this output word uninitialized before ioctrl writes it. */
    int empty;
    if (!(config_btctler_le_roles & 4) || !adv_hdl || !adv_hdl->adv_link || !adv_hdl->adv_link->hw)
        return 1;
    /* CONNECT_REQ transfers this radio to the connection and frees adv_hdl
     * inside upload_data. The SDK keeps the radio pointer across that call. */
    void *hw = adv_hdl->adv_link->hw;
    __ble_ops->upload_data(hw, __adv_rx_post_handler);
    __ble_ops->ioctrl(hw, 19, &empty);
    return empty;
}

static void __adv_rx_post_handler(void *priv, struct ble_rx *rx)
{
    if (rx->type == LL_PDU_SCAN_REQ) {
        ll_ext_legacy_adv_scan_req_received(priv, rx);
    } else if (rx->type == LL_PDU_CONNECT_REQ) {
        struct le_adv_link *link = adv_hdl->adv_link;
        if (adv_not_connect_same_master && ll_link_for_address(rx->txadd, rx->data)) {
            __ble_ops->close(link->hw);
            ll_adv_resume();
            putchar('R');
        } else {
            link->hw = NULL;
            ll_adv_close();
            ll_create_conn_link_post(priv);
            slave_rx_conn_post_handler(priv, rx);
            ((u8 *)priv)[312] = rx->rssi;
        }
    }
}

int ll_adv_set_params(const u8 *data) LL_CODE;
int ll_adv_set_params(const u8 *data)
{
    if (!(config_btctler_le_roles & 4)) return 12;
    if (!adv_hdl) ll_adv_init();
    if (adv_hdl->enable) {
        if (log_tag_const_e_LL_ADV)
            log_print(4, NULL, "<Error>: [LL_ADV]The Host shall not issue this command when advertising is enabled\r\n");
        return 12;
    }
    memcpy(&adv_param, data, 15);
    return 0;
}

static void ll_adv_init(void)
{
    if (adv_hdl) return;
    if (log_tag_const_i_LL_ADV) log_print(2, NULL, "[Info]: [LL_ADV]LL ADV - init\r\n");
    adv_hdl = __bt_malloc(40);
    if (!adv_hdl) btctrler_assert_failed("__this LL ADV HDL NULL", 63);
}

int ll_adv_set_adv_data(const u8 *data) LL_CODE;
int ll_adv_set_adv_data(const u8 *data)
{
    if (!(config_btctler_le_roles & 4)) return 12;
    if (!adv_hdl) ll_adv_init();
    struct le_adv_link *link = adv_hdl->adv_link;
    memset(&adv_data, 0, 32);
    adv_data.length = *data;
    /* Keep the SDK's unchecked length copy (the HCI path supplies the length). */
    memcpy(adv_data.data, data + 1, *data);
    if (log_tag_const_i_LL_ADV) printf_buf(adv_data.data, adv_data.length);
    if (link && link->hw) {
        __ble_ops->ioctrl(link->hw, 21, data);
        return 0;
    }
    return 0;
}

int ll_adv_set_scan_rsp_data(const u8 *data) LL_CODE;
int ll_adv_set_scan_rsp_data(const u8 *data)
{
    if (!(config_btctler_le_roles & 4)) return 12;
    if (!adv_hdl) ll_adv_init();
    struct le_adv_link *link = adv_hdl->adv_link;
    memset(&scan_resp_data, 0, 32);
    scan_resp_data.length = *data;
    /* Keep the SDK's unchecked length copy. */
    memcpy(scan_resp_data.data, data + 1, *data);
    if (log_tag_const_i_LL_ADV) printf_buf(scan_resp_data.data, scan_resp_data.length);
    if (link && link->hw) {
        __ble_ops->ioctrl(link->hw, 22, data);
        return 0;
    }
    return 0;
}

int ll_adv_open(void) LL_CODE;
int ll_adv_open(void)
{
    if (!(config_btctler_le_roles & 4)) return 12;
    if (!adv_hdl) ll_adv_init();
    if (adv_hdl->adv_link) return 12;
    struct le_adv_link *link = __bt_malloc(24);
    if (!link) return 12;
    adv_hdl->adv_link = link;
    link->hw = __ble_ops->open();
    if (!link->hw) return 58;
    __ble_ops->handler_register(link->hw, link, &adv_handler);
    if (config_btctler_le_features & 64)
        __ble_ops->ioctrl(link->hw, 12, le_param.resolution_enable);
    int err = __set_ll_adv_state(link);
    if (err) return err;
    adv_hdl->enable = 1;
    return 0;
}

static int __set_ll_adv_state(struct le_adv_link *link)
{
    /* For unsupported advertising types, the SDK passes these local fields
     * without initializing them. Preserve that behavior. */
    struct ble_adv adv;
    adv.channel_map = adv_param.Advertising_Channel_Map;
    adv.pkt_cnt = 0;
    for (int bit = 0; bit < 3; bit++)
        if (adv.channel_map & (1 << bit)) adv.pkt_cnt++;
    adv.filter_policy = adv_param.Advertising_Filter_Policy;
    switch (adv_param.Advertising_Type) {
    case 0: adv.interval = adv_param.Advertising_Interval_Max; adv.pdu_interval = 2; adv.adv_type = 0; break;
    case 1:
        adv.interval = 6; adv.pdu_interval = 2; adv.adv_type = 1; adv.filter_policy = 0;
        link->timeout = sys_timer_register(1280, ll_adv_timeout_handler);
        if (!link->timeout) btctrler_assert_failed("adv_link->timeout", 121);
        sys_timer_set_context(link->timeout, link);
        __set_ll_adv_peer_addr(link);
        break;
    case 2: adv.interval = adv_param.Advertising_Interval_Max; adv.pdu_interval = 2; adv.adv_type = 6; break;
    case 3: adv.interval = adv_param.Advertising_Interval_Max; adv.pdu_interval = 1; adv.adv_type = 2; break;
    case 4:
        adv.interval = adv_param.Advertising_Interval_Min; adv.pdu_interval = 3; adv.adv_type = 1; adv.filter_policy = 0;
        __set_ll_adv_peer_addr(link);
        break;
    default: break;
    }
    u32 interval_override = (config_vendor_le_bb >> 8) & 15;
    if (interval_override) adv.pdu_interval = interval_override;
    __set_ll_adv_local_addr(link);
    if (adv_data.length) {
        if (log_tag_const_i_LL_ADV) printf_buf(adv_data.data, adv_data.length);
        __ble_ops->ioctrl(link->hw, 21, &adv_data);
    }
    if (scan_resp_data.length) {
        if (log_tag_const_i_LL_ADV) printf_buf(scan_resp_data.data, scan_resp_data.length);
        __ble_ops->ioctrl(link->hw, 22, &scan_resp_data);
    }
    int err = __ble_ops->advertising(link->hw, &adv);
    /* The SDK maps any nonzero radio status to HCI error 3. */
    return err ? 3 : 0;
}

int ll_adv_close(void) LL_CODE;
int ll_adv_close(void)
{
    if (!(config_btctler_le_roles & 4)) return 12;
    if (!adv_hdl) return 0;
    struct le_adv_link *link = adv_hdl->adv_link;
    if (!link) return 2;
    if (link->hw) __ble_ops->close(link->hw);
    ll_adv_timeout_stop(link);
    __bt_free(link);
    adv_hdl->adv_link = NULL;
    adv_hdl->enable = 0;
    __bt_free(adv_hdl);
    adv_hdl = NULL;
    return 0;
}

struct le_adv_link *ll_adv_link_get(void) LL_CODE;
struct le_adv_link *ll_adv_link_get(void) { return adv_hdl ? adv_hdl->adv_link : NULL; }

int ll_adv_local_addr_refresh(void) LL_CODE;
int ll_adv_local_addr_refresh(void)
{
    if (!adv_hdl) return 12;
    if (!adv_hdl->adv_link) return 12;
    __set_ll_adv_local_addr(adv_hdl->adv_link);
    return 0;
}

static void __set_ll_adv_local_addr(struct le_adv_link *link)
{
    __set_ll_local_addr(link->hw, &link->local, adv_param.Own_Address_Type);
    if (!(config_btctler_le_features & 64) || !le_param.resolution_enable) return;
    if ((adv_param.Own_Address_Type & (u8)~1) != 2) return;
    struct resolving_list *entry = ll_resolving_list_match(adv_param.Peer_Address_Type, adv_param.Peer_Address);
    if (entry && ll_resolve_list_IRK_verify(entry->resolving_list_param.Local_IRK) == 1)
        ll_resolve_list_set_local_RPA(link->local.addr, entry);
    __ble_ops->ioctrl(link->hw, 2, link->local.addr_type, link->local.addr);
}

static void ll_adv_resume(void)
    __attribute__((noinline));
static void ll_adv_resume(void)
{
    struct le_adv_link *link = adv_hdl->adv_link;
    link->hw = __ble_ops->open();
    if (!link->hw) return;
    __ble_ops->handler_register(link->hw, link, &adv_handler);
    if (config_btctler_le_features & 64)
        __ble_ops->ioctrl(link->hw, 12, le_param.resolution_enable);
    int err = __set_ll_adv_state(link);
    if (!err) adv_hdl->enable = 1;
}

static void adv_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    if (!(config_btctler_le_roles & 4)) return;
    if (rx->type == LL_PDU_SCAN_REQ) {
        le_ll_adv_addr_process(priv, rx);
        if (!ll_error_code) __ble_ops->ioctrl(*(void **)priv, 14, rx, 0);
    } else if (rx->type == LL_PDU_CONNECT_REQ) {
        struct le_link *link = ll_create_conn_link(*(void **)priv, (struct le_addr *)((u8 *)priv + 4), 1,
                                                   rx->txadd, rx->data, &rx->data[12]);
        le_ll_adv_addr_process(*(void **)priv, rx);
        if (ll_error_code) {
            ll_close_conn_link(link);
            ll_adv_resume();
        } else {
            __ble_ops->ioctrl(*(void **)priv, 14, rx, 0);
        }
    }
    if (rx->llid != 1 || rx->len != 0)
        ll_thread_resume();
}

static void __set_ll_adv_peer_addr(struct le_adv_link *link)
{
    if (adv_param.Advertising_Type != 1 && adv_param.Advertising_Type != 4) return;
    link->peer.addr_type = adv_param.Peer_Address_Type;
    memcpy(link->peer.addr, adv_param.Peer_Address, 6);
    if (log_tag_const_i_LL_ADV)
        log_print(2, NULL, "[Info]: [LL_ADV]adv_peer_addr : %s\r\n",
                  btctler_bd_addr_to_str(link->peer.addr));
    struct resolving_list *entry = NULL;
    if (config_btctler_le_features & 64) {
        if (!le_param.resolution_enable) return;
        entry = ll_resolving_list_match(adv_param.Peer_Address_Type, adv_param.Peer_Address);
    }
    if ((adv_param.Own_Address_Type & (u8)~1) == 2 && entry &&
        ll_resolve_list_IRK_verify(entry->resolving_list_param.Peer_IRK) == 1) {
        ll_resolve_list_set_peer_RPA(link->peer.addr, entry);
        if (log_tag_const_i_LL_ADV)
            log_print(2, NULL, "[Info]: [LL_ADV]Privacy_peer_addr : %s\r\n",
                      btctler_bd_addr_to_str(link->peer.addr));
    }
    if ((adv_param.Own_Address_Type & (u8)~1) == 2)
        link->peer.addr_type = adv_param.Own_Address_Type;
    __ble_ops->ioctrl(link->hw, 3, link->peer.addr_type, link->peer.addr);
    __ble_ops->ioctrl(link->hw, 9, link->peer.addr_type, link->peer.addr);
}

static void ll_adv_timeout_handler(void *priv)
{
    if (!priv) return;
    ll_adv_close();
    le_connection_complete_event(NULL, 60);
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>
#include <stdarg.h>
int config_btctler_le_roles = 0;
u64 config_btctler_le_features = 0;
u32 config_vendor_le_bb = 0;
int config_asser = 1;
u8 log_tag_const_e_LL_ADV = 0, log_tag_const_i_LL_ADV = 0;
struct le_parameter le_param;
const struct ble_operation *__ble_ops;
static int alloc_size, frees, closes, uploads, empty_queries, resumes, scans;
static int conn_posts, slave_posts, timers_removed;
static int radio, received_type;
static int peer_types[2];
static struct ble_adv programmed;
static struct le_link connection;
static struct ble_rx received;
static char trace[32];
static int trace_len;
static void mark(char c) { trace[trace_len++] = c; trace[trace_len] = 0; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
void printf_buf(const u8 *data, int len) { (void)data; (void)len; }
void *__bt_malloc(int size)
{
    alloc_size = size;
    /* SDK allocation sizes are for 32-bit pointers; use the host layouts. */
    CHECK(size == 40 || size == 24);
    void *p = calloc(1, size == 40 ? sizeof(struct ll_adv_hdl) : sizeof(struct le_adv_link));
    CHECK(p != NULL);
    return p;
}
void __bt_free(void *p) { CHECK(p != NULL); frees++; mark('f'); free(p); }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; CHECK(0); }
static void *open_radio(void) { return &radio; }
static void close_radio(void *hw) { CHECK(hw == &radio); closes++; mark('c'); }
static void register_handler(void *hw, void *priv, const struct ble_handler *handler)
{
    CHECK(hw == &radio && priv == adv_hdl->adv_link && handler == &adv_handler);
}
static int advertise(void *hw, struct ble_adv *adv)
{
    CHECK(hw == &radio);
    programmed = *adv;
    return 0;
}
static void ioctrl(void *hw, int command, ...)
{
    CHECK(hw == &radio);
    va_list ap;
    va_start(ap, command);
    if (command == 19) {
        *va_arg(ap, int *) = 7;
        empty_queries++;
        mark('q');
    } else if (command == 3 || command == 9) {
        peer_types[command == 9] = va_arg(ap, int);
        CHECK(va_arg(ap, u8 *) == adv_hdl->adv_link->peer.addr);
    } else {
        CHECK(command == 14);
    }
    va_end(ap);
}
static void upload(void *hw, void (*callback)(void *, struct ble_rx *))
{
    CHECK(hw == &radio);
    uploads++;
    received.type = received_type;
    received.rssi = -42;
    callback(&connection, &received);
}
void __set_ll_local_addr(void *hw, struct le_addr *addr, u8 type)
{
    CHECK(hw == &radio);
    addr->addr_type = type;
}
void ll_create_conn_link_post(struct le_link *link) { CHECK(link == &connection); conn_posts++; mark('p'); }
void slave_rx_conn_post_handler(struct le_link *link, struct ble_rx *rx)
{
    CHECK(link == &connection && rx == &received); slave_posts++; mark('s');
}
void ll_ext_legacy_adv_scan_req_received(struct le_link *link, struct ble_rx *rx)
{
    CHECK(link == &connection && rx == &received); scans++;
}
void ll_thread_resume(void) { resumes++; }
struct resolving_list *ll_resolving_list_match(u8 type, const u8 *addr)
{
    (void)type; (void)addr; return NULL;
}
bool ll_resolve_list_IRK_verify(const u8 *irk) { (void)irk; return false; }
void ll_resolve_list_set_peer_RPA(u8 *addr, struct resolving_list *entry)
{
    (void)addr; (void)entry; CHECK(0);
}
void ll_resolve_list_set_local_RPA(u8 *addr, struct resolving_list *entry)
{
    (void)addr; (void)entry; CHECK(0);
}
void sys_timer_remove(sys_timer timer) { CHECK(timer == 123); timers_removed++; mark('t'); }
static const struct ble_operation operations = {
    .open = open_radio, .close = close_radio, .advertising = advertise,
    .handler_register = register_handler, .ioctrl = ioctrl, .upload_data = upload,
};
int main(void)
{
    u8 params[15] = {0};
    CHECK(ll_adv_link_get() == NULL);
    CHECK(ll_adv_set_params(params) == 12); /* role gate */
    CHECK(alloc_size == 0);
    config_btctler_le_roles = 4;
    CHECK(ll_adv_set_params(params) == 0);
    CHECK(alloc_size == 40 && ll_adv_link_get() == NULL);
    u8 data[1] = {0};
    CHECK(ll_adv_set_adv_data(data) == 0);
    CHECK(ll_adv_set_scan_rsp_data(data) == 0);
    CHECK(ll_adv_close() == 2); /* initialized handle without an open link */
    CHECK(frees == 0);
    __ble_ops = &operations;
    /* SDK loop starts at zero and counts only channel-map bits 0..2. */
    for (int map = 0; map < 8; map++) {
        adv_param.Advertising_Channel_Map = map | 0x80;
        CHECK(ll_adv_open() == 0);
        CHECK(programmed.pkt_cnt == ((map & 1) + ((map >> 1) & 1) + ((map >> 2) & 1)));
        CHECK(programmed.channel_map == (map | 0x80));
        trace_len = 0;
        CHECK(ll_adv_close() == 0 && !strcmp(trace, "cff"));
    }
    /* A scan callback keeps the advertiser; CONNECT_REQ releases it inside
     * upload_data, then SDK command 19 still uses the original radio. */
    CHECK(ll_adv_open() == 0);
    received_type = LL_PDU_SCAN_REQ;
    CHECK(adv_rx_post_handler() == 7 && scans == 1 && adv_hdl != NULL);
    int before = frees;
    received_type = LL_PDU_CONNECT_REQ;
    trace_len = 0;
    CHECK(adv_rx_post_handler() == 7);
    CHECK(adv_hdl == NULL && frees == before + 2 && !strcmp(trace, "ffpsq"));
    CHECK(conn_posts == 1 && slave_posts == 1 && ((s8 *)&connection)[312] == -42);
    CHECK(uploads == 2 && empty_queries == 2 && closes == 8);
    CHECK(adv_rx_post_handler() == 1 && uploads == 2);
    /* Timer removal precedes the frees on ordinary advertising shutdown. */
    CHECK(ll_adv_open() == 0);
    adv_hdl->adv_link->timeout = 123;
    trace_len = 0;
    CHECK(ll_adv_close() == 0 && !strcmp(trace, "ctff") && timers_removed == 1);
    /* The wake condition reads llid at the header, after the list entry. */
    memset(&received, 0, sizeof(received));
    memset(&received.entry, 0xff, sizeof(received.entry));
    received.llid = 1;
    adv_rx_probe_handler(NULL, &received);
    CHECK(resumes == 0);
    received.len = 1;
    adv_rx_probe_handler(NULL, &received);
    CHECK(resumes == 1);
    received.len = 0; received.llid = 0;
    adv_rx_probe_handler(NULL, &received);
    CHECK(resumes == 2);
    /* For directed advertising, both radio commands use the address type
     * after the SDK's Own_Address_Type 2/3 override, even without an IRK. */
    CHECK(ll_adv_open() == 0);
    adv_param.Advertising_Type = 4;
    adv_param.Peer_Address_Type = 1;
    for (int type = 0; type < 4; type++) {
        adv_param.Own_Address_Type = type;
        __set_ll_adv_peer_addr(adv_hdl->adv_link);
        CHECK(peer_types[0] == (type < 2 ? 1 : type));
        CHECK(peer_types[1] == peer_types[0]);
    }
    CHECK(ll_adv_close() == 0);
    TEST_DONE("btctrler_ll_adv");
}
#endif
