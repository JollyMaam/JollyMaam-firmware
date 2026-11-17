/* BLE initiator (LE Create Connection): open one radio link that scans for
 * the peer, resolve and filter the advertiser's address, hand the CONNECT_IND
 * parameters to the legacy link layer when the radio reports an ADV_IND, and
 * tear the link down when the connection is up.  SDK quirks kept: the
 * connection window offset reads a free-running hardware counter and takes it
 * modulo half the interval (zero when the interval is 0 or 1); the parameter
 * copy takes all 32 bytes of the sample connection parameters including the
 * afh pointer; the filter step clears the error code the resolving step set;
 * a failed radio open leaves the half-built link registered. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_INIT, log_tag_const_i_LL_INIT;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_btctler_hci_standard, config_btctler_le_afh_en;
extern void log_print(int level, const char *tag, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
extern volatile u32 ll_init_random_counter;
#define RANDOM_COUNTER ll_init_random_counter
#else
#define RANDOM_COUNTER (*(volatile u32 *)0x1e3b00)
#endif

#ifndef UNITTEST
_Static_assert(sizeof(struct ble_conn_param) == 32, "SDK connection parameter size");
_Static_assert(sizeof(struct ble_conn) == 40, "SDK ble_conn size");
_Static_assert(sizeof(struct le_init_link) == 52, "SDK init link size");
_Static_assert(sizeof(struct connection_parameter) == 25, "SDK initiating parameter size");
_Static_assert(offsetof(struct ll_init_hdl, conn_param) == 4, "SDK init handle offsets");
_Static_assert(offsetof(struct le_init_link, ll_data) == 20, "SDK init link ll_data");
_Static_assert(offsetof(struct ble_conn_param, winsize) == 7, "SDK winsize offset");
_Static_assert(offsetof(struct ble_conn_param, winoffset) == 8, "SDK winoffset offset");
_Static_assert(offsetof(struct white_list, entry) == 8, "SDK white list entry");
#endif

static struct ll_init_hdl *init_hdl __attribute__((section(".ble_ll_data"))) = NULL;

static void init_rx_probe_handler(void *priv, struct ble_rx *rx) LL_CODE;
static void init_sync_param_handler(void *priv, u8 *data, u16 size) LL_CODE;

static const struct ble_handler init_handler __attribute__((section(".ble_ll_const"), aligned(4))) = {
    init_rx_probe_handler,
    NULL,
    NULL,
    NULL,
    init_sync_param_handler,
    NULL,
};

/* Resolve the advertiser's (and for a directed advertisement our own)
 * address against the resolving list and program the radio with the result.
 * The caller reads the verdict from ll_error_code. */
struct resolving_list *ll_init_state_RPA_process(struct le_init_link *link, struct ble_rx *rx) LL_CODE;
struct resolving_list *ll_init_state_RPA_process(struct le_init_link *link, struct ble_rx *rx)
{
    ll_error_code = 0;
    if (!(config_btctler_le_features & 64)) return NULL;
    if (le_param.resolution_enable == 0) {
        if (log_tag_const_e_LL_INIT)
            log_print(4, NULL, "<Error>: [LL_INIT]resolution still disable\r\n");
        return NULL;
    }
    struct resolving_list *peer = ll_resolve_peer_addr(rx->data);
    if (!peer) {
        ll_error_code = 1;
        return NULL;
    }
    if (rx->type == 1) {
        struct resolving_list *local = ll_resolve_local_addr(&rx->data[6]);
        if (!local) {
            ll_error_code = 1;
            return NULL;
        }
        if (peer != local) {
            ll_error_code = 4;
            return NULL;
        }
    }
    /* __set_ll_init_local_RPA */
    if ((init_hdl->conn_param.own_address_type & 0xfe) == 2) {
        if (ll_resolve_list_IRK_verify(peer->resolving_list_param.Local_IRK) == 1)
            ll_resolve_list_set_local_RPA(link->local.addr, peer);
    }
    __ble_ops->ioctrl(link->hw, 2, (int)rx->rxadd, &rx->data[6]);
    __ble_ops->ioctrl(link->hw, 13, (int)link->local.addr_type, link->local.addr);
    return peer;
}

/* The advertising address filter: resolve it, then apply the white list when
 * the initiator filter policy asks for one.  ll_error_code is 2 for a
 * white-list miss.  The SDK clears the code the resolving step set. */
void le_ll_init_addr_process(struct le_init_link *link, struct ble_rx *rx) LL_CODE;
void le_ll_init_addr_process(struct le_init_link *link, struct ble_rx *rx)
{
    if (rx->type == 5 || rx->type == 3)
        btctrler_assert_failed("(rx->type != LL_PDU_SCAN_REQ) && (rx->type != LL_PDU_CONNECT_REQ)", 0);
    struct resolving_list *resolved = ll_init_state_RPA_process(link, rx);

    /* ll_init_state_filter_process */
    ll_error_code = 0;
    if (init_hdl->conn_param.initiator_filter_policy != 1) return;
    struct white_list *match;
    if (!(config_btctler_le_features & 64)) {
        match = ll_white_list_match(link->hw, rx->txadd, rx->data);
    } else {
        if ((resolved == NULL) | (le_param.resolution_enable == 0)) return;
        match = ll_white_list_match(link->hw, resolved->resolving_list_param.Peer_Identity_Address_Type,
                                    resolved->resolving_list_param.Peer_Identity_Address);
    }
    if (match == NULL) ll_error_code = 2;
}

/* Runs in the link-layer thread: detach the radio link from the initiator
 * state, finish the connection and let ll_init_close free the rest. */
static void __init_rx_post_handler(void *priv, struct ble_rx *rx) LL_CODE;
static void __init_rx_post_handler(void *priv, struct ble_rx *rx)
{
    if ((rx->type & 14) < 2) {
        struct le_link *link = priv;
        init_hdl->init_link->hw = NULL;
        ll_create_conn_link_post(link);
        master_rx_conn_post_handler(link, rx);
        ll_init_close();
        link->rssi = rx->rssi;
    }
}

int init_rx_post_handler(void) LL_CODE;
int init_rx_post_handler(void)
{
    int init_rx_empty;
    if ((config_btctler_le_roles & 16) == 0) return 1;
    if (init_hdl == NULL) return 1;
    if (init_hdl->init_link == NULL) return 1;
    void *hw = init_hdl->init_link->hw;
    __ble_ops->upload_data(hw, __init_rx_post_handler);
    __ble_ops->ioctrl(hw, 19, &init_rx_empty);
    return init_rx_empty;
}

/* Fill in the connection parameters and start scanning for the peer. */
static void __set_ll_init_state(struct le_init_link *init_link) LL_CODE;
static void __set_ll_init_state(struct le_init_link *init_link)
{
    struct ble_conn conn;
    u8 info = log_tag_const_i_LL_INIT;
    if (info) log_print(2, NULL, "[Info]: [LL_INIT]>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    struct ll_init_hdl *hdl = init_hdl;
    u16 scan_interval = 5, scan_window = 2;
    if (config_btctler_hci_standard == 0) {
        scan_interval = hdl->conn_param.le_scan_interval;
        scan_window = hdl->conn_param.le_scan_window;
    }
    conn.scan_interval = scan_interval;
    conn.scan_windows = scan_window;
    u8 filter_policy = hdl->conn_param.initiator_filter_policy;
    conn.filter_policy = filter_policy;
    __set_ll_local_addr(init_link->hw, &init_link->local, hdl->conn_param.own_address_type);

    /* __set_ll_init_peer_addr */
    struct ll_init_hdl *h = init_hdl;
    u8 peer_address_type = h->conn_param.peer_address_type;
    init_link->peer.addr_type = peer_address_type;
    memcpy(init_link->peer.addr, h->conn_param.peer_address, 6);
    /* ll_init_set_peer_wla */
    if (h->conn_param.initiator_filter_policy != 0) {
        if (ll_white_list_match(init_link->hw, peer_address_type, init_link->peer.addr) == NULL) {
            if (log_tag_const_e_LL_INIT)
                log_print(4, NULL, "<Error>: [LL_INIT]Filter policy no match\r\n");
            struct list_head *first = le_param.white_list_head.next;
            if (first != &le_param.white_list_head) {
                struct white_list *entry = (struct white_list *)((u8 *)first - offsetof(struct white_list, entry));
                init_link->peer.addr_type = entry->white_list_param.Address_Type;
                memcpy(init_link->peer.addr, entry->white_list_param.Address, 6);
            }
        }
        if (info) {
            char *str = btctler_bd_addr_to_str(init_link->peer.addr);
            log_print(2, NULL, "[Info]: [LL_INIT]WL device: %d / %s\r\n", (int)init_link->peer.addr_type, str);
        }
    }
    __ble_ops->ioctrl(init_link->hw, 9, (int)init_link->peer.addr_type, init_link->peer.addr);
    __ble_ops->ioctrl(init_link->hw, 3, (int)init_link->peer.addr_type, init_link->peer.addr);
    if (info) {
        char *str = btctler_bd_addr_to_str(init_link->peer.addr);
        log_print(2, NULL, "[Info]: [LL_INIT]Remote device: %d / %s\r\n", (int)init_link->peer.addr_type, str);
    }

    struct ble_conn_param *ll_data = &init_link->ll_data;
    memcpy(ll_data, &sample_conn_param, 32);
    if (config_btctler_le_afh_en == 0) memcpy(ll_data->channel, ll_get_fixed_channel_map(), 5);
    access_addr_generate(ll_data->aa);
    h = init_hdl;
    ll_data->latency = h->conn_param.conn_latency;
    ll_data->timeout = h->conn_param.supervision_timeout;
    ll_data->interval = h->conn_param.conn_interval_max;
    ll_data->sca = 0;
    ll_data->widening = (u16)(((((u32)master_sca[0] + 500) * ll_data->interval * ((u32)ll_data->latency + 1) / 800) << 1) + 2);
    ll_data->hop = hop_generarate();
    u32 half = (u16)(ll_data->interval >> 1);
    ll_data->winoffset = (u16)(RANDOM_COUNTER % half + half);
    memcpy(&conn.ll_data, ll_data, 32);
    if (info) {
        log_print(2, NULL, "[Info]: [LL_INIT]init scan_interval: %x\r\n", (int)scan_interval);
        log_print(2, NULL, "[Info]: [LL_INIT]init scan_windows: %x\r\n", (int)scan_window);
        log_print(2, NULL, "[Info]: [LL_INIT]init filter_policy: %x\r\n", (int)filter_policy);
        log_print(2, NULL, "[Info]: [LL_INIT]init interval: %x\r\n", (int)conn.ll_data.interval);
        log_print(2, NULL, "[Info]: [LL_INIT]init latency: %x\r\n", (int)conn.ll_data.latency);
        log_print(2, NULL, "[Info]: [LL_INIT]init timeout: %x\r\n", (int)conn.ll_data.timeout);
        log_print(2, NULL, "[Info]: [LL_INIT]init widening: %x\r\n", (int)conn.ll_data.widening);
    }
    __ble_ops->initiating(init_link->hw, &conn);
    if (info) log_print(2, NULL, "[Info]: [LL_INIT]>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
}

/* Radio receive callback while scanning: on an ADV_IND/ADV_DIRECT_IND start
 * the connection, after resolving the address when privacy is in use. */
static void init_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    struct le_init_link *link = priv;
    if ((config_btctler_le_roles & 16) == 0) return;
    if ((rx->type & 14) < 2) {
        if (config_btctler_le_features & 64) {
            if (le_param.resolution_enable == 0) {
                if (log_tag_const_e_LL_INIT)
                    log_print(4, NULL, "<Error>: [LL_INIT]resolution not enable\r\n");
                return;
            }
            if (__ble_ops->is_init_enter_conn(link->hw) == 0) {
                le_ll_init_addr_process(link, rx);
                if (ll_error_code != 0) {
                    if (log_tag_const_e_LL_INIT)
                        log_print(4, NULL, "<Error>: [LL_INIT]AdvA resolve fail\r\n");
                    return;
                }
                __ble_ops->ioctrl(link->hw, 14, rx, 0);
                return;
            }
        }
        ll_create_conn_link(link->hw, &link->local, 0, rx->txadd, rx->data, &link->ll_data);
        if (log_tag_const_i_LL_INIT) log_print(2, NULL, "[Info]: [LL_INIT]ADV_IND\r\n");
    }
    if (rx->llid != 1 || rx->len != 0) ll_thread_resume();
}

/* The radio's connection-window parameters for the request being sent. */
static void init_sync_param_handler(void *priv, u8 *data, u16 size)
{
    (void)size;
    struct le_init_link *link = priv;
    if ((config_btctler_le_roles & 16) == 0) return;
    link->ll_data.winsize = data[0];
    link->ll_data.winoffset = (u16)(data[2] << 8 | data[1]);
}

int ll_init_open(const u8 *data) LL_CODE;
int ll_init_open(const u8 *data)
{
    if ((config_btctler_le_roles & 16) == 0) {
        if (log_tag_const_e_LL_INIT) log_print(4, NULL, "<Error>: [LL_INIT]Not support\r\n");
        return 12;
    }
    u8 info = log_tag_const_i_LL_INIT;
    if (init_hdl == NULL) {
        /* ll_init_init */
        if (info) log_print(2, NULL, "[Info]: [LL_INIT]LL Init - init\r\n");
        init_hdl = __bt_malloc(32);
    }
    if (info) log_print(2, NULL, "[Info]: [LL_INIT]ll_init_open\r\n");
    if (init_hdl->init_link != NULL) {
        if (log_tag_const_e_LL_INIT) log_print(4, NULL, "<Error>: [LL_INIT]init link open twice\r\n");
        return 12;
    }
    struct le_init_link *link = __bt_malloc(52);
    if (link == NULL) {
        if (log_tag_const_e_LL_INIT) log_print(4, NULL, "<Error>: [LL_INIT]init link NULL\r\n");
        return 12;
    }
    init_hdl->init_link = link;
    link->hw = __ble_ops->open();
    if (link->hw == NULL) {
        if (log_tag_const_e_LL_INIT) log_print(4, NULL, "<Error>: [LL_INIT]init hw NULL\r\n");
        return 58;
    }
    __ble_ops->handler_register(link->hw, link, &init_handler);
    if (config_btctler_le_features & 64) __ble_ops->ioctrl(link->hw, 12, (int)le_param.resolution_enable);
    memcpy(&init_hdl->conn_param, data, 25);
    __set_ll_init_state(link);
    return 0;
}

int ll_init_close(void) LL_CODE;
int ll_init_close(void)
{
    if ((config_btctler_le_roles & 16) == 0) {
        if (log_tag_const_e_LL_INIT) log_print(4, NULL, "<Error>: [LL_INIT]Not support\r\n");
        return 12;
    }
    if (init_hdl == NULL) {
        if (log_tag_const_i_LL_INIT) log_print(2, NULL, "[Info]: [LL_INIT]LL Init already destroy\r\n");
        return 0;
    }
    struct le_init_link *link = init_hdl->init_link;
    if (link == NULL) return 2;
    if (link->hw != NULL) __ble_ops->close(link->hw);
    __bt_free(link);
    init_hdl->init_link = NULL;
    /* ll_init_destroy */
    if (init_hdl != NULL) {
        if (log_tag_const_i_LL_INIT) log_print(2, NULL, "[Info]: [LL_INIT]LL Init - destroy\r\n");
        __bt_free(init_hdl);
        init_hdl = NULL;
    }
    return 0;
}

struct le_init_link *ll_init_link_get(void) LL_CODE;
struct le_init_link *ll_init_link_get(void)
{
    if (init_hdl == NULL) {
        if (log_tag_const_e_LL_INIT)
            log_print(4, NULL, "<Error>: [LL_INIT]LL Init not init yet\r\n");
        return NULL;
    }
    return init_hdl->init_link;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

u8 ll_error_code;
struct le_parameter le_param;
const struct ble_operation *__ble_ops;
const struct ble_conn_param sample_conn_param = { { 1, 2, 3, 4 }, { 5, 6, 7 }, 8, 9, 10, 11, 12, { 1, 2, 3, 4, 5 }, .hop = 0x17, .sca = 5, 99, 98, (void *)0x1234 };
const u16 master_sca[8] = { 100, 0, 0, 0, 0, 0, 0, 0 };
u8 log_tag_const_e_LL_INIT = 1, log_tag_const_i_LL_INIT = 0;
volatile u32 ll_init_random_counter;
int config_btctler_le_roles = 16;
int config_btctler_hci_standard, config_btctler_le_afh_en;
u64 config_btctler_le_features;

static char trace[1024];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static int errors, infos, asserts, resume_count, hop_value, malloc_fail_at, malloc_calls, nfreed;
static void *freed[8];
static void *hw_token = (void *)0xbeef;
static int open_null;
static struct resolving_list peer_entry, local_entry;
static struct resolving_list *peer_result = &peer_entry, *local_result = &local_entry;
static int irk_ok;
static struct white_list *wl_result;
static struct white_list wl_a;
static u8 wl_type_seen; static const u8 *wl_addr_seen; static void *wl_hw_seen; static int wl_calls;
static const u8 fixed_map[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
static int ioctl_cmd[8], ioctl_a[8], ioctl_n; static const void *ioctl_p[8];
static struct ble_conn initiating_conn; static int initiating_calls;
static void *registered_priv; static const struct ble_handler *registered;
static void *closed_hw; static int close_calls;
static void *uploaded_hw; static void (*uploaded_cb)(void *, struct ble_rx *);
static int enter_conn;
static int init_rx_empty_value = 77;
static struct { void *hw; struct le_addr *local; u8 role, txadd; u8 *peer; void *data; } created;
static struct le_link *post_link, *master_link; static struct ble_rx *master_rx;
static struct le_link conn_link;
static int local_set_type = -1;
static u8 irk_arg_first;

void log_print(int level, const char *tag, const char *format, ...) { (void)tag; (void)format; if (level == 4) errors++; else infos++; }
void btctrler_assert_failed(const char *what, int value) { (void)what; CHECK(value == 0); asserts++; }
void ll_thread_resume(void) { resume_count++; }
u8 hop_generarate(void) { return (u8)hop_value; }
char *btctler_bd_addr_to_str(const uint8_t *addr) { (void)addr; return "addr"; }
void *__bt_malloc(int size)
{
    if (malloc_fail_at && ++malloc_calls == malloc_fail_at) return NULL;
    return calloc(1, size > 64 ? size : 64);   /* the host structs are larger than the SDK ones */
}
void __bt_free(void *p) { freed[nfreed++] = p; }
struct resolving_list *ll_resolve_peer_addr(const u8 *addr) { (void)addr; T("resolvepeer"); return peer_result; }
struct resolving_list *ll_resolve_local_addr(const u8 *addr) { (void)addr; T("resolvelocal"); return local_result; }
bool ll_resolve_list_IRK_verify(const u8 *irk) { irk_arg_first = irk[0]; T("irk"); return irk_ok; }
void ll_resolve_list_set_local_RPA(u8 *addr, struct resolving_list *r) { CHECK(r == &peer_entry); addr[0] = 0x99; T("setrpa"); }
struct white_list *ll_white_list_match(void *priv, u8 addr_type, const u8 *addr)
{
    wl_calls++; wl_hw_seen = priv; wl_type_seen = addr_type; wl_addr_seen = addr; T("wlmatch");
    return wl_result;
}
void __set_ll_local_addr(void *hw, struct le_addr *addr, u8 own) { CHECK(hw == hw_token); (void)addr; local_set_type = own; T("localaddr"); }
u8 *ll_get_fixed_channel_map(void) { return (u8 *)fixed_map; }
void access_addr_generate(u8 *aa) { aa[0] = 0xaa; T("aa"); }
struct le_link *ll_create_conn_link(void *hw, struct le_addr *local, u8 role, u8 peer_addr_type, u8 *peer_addr, void *ll_data)
{
    created.hw = hw; created.local = local; created.role = role; created.txadd = peer_addr_type; created.peer = peer_addr; created.data = ll_data;
    T("createconn");
    return &conn_link;
}
void ll_create_conn_link_post(struct le_link *l) { post_link = l; T("connpost"); }
void master_rx_conn_post_handler(struct le_link *l, struct ble_rx *rx) { master_link = l; master_rx = rx; T("masterrx"); }

static void *m_open(void) { T("open"); return open_null ? NULL : hw_token; }
static void m_close(void *hw) { closed_hw = hw; close_calls++; T("close"); }
static void m_handler_register(void *hw, void *priv, const struct ble_handler *h) { CHECK(hw == hw_token); registered_priv = priv; registered = h; T("handlerreg"); }
static int m_initiating(void *hw, struct ble_conn *c) { CHECK(hw == hw_token); initiating_conn = *c; initiating_calls++; T("initiating"); return 0; }
static void m_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    (void)hw;
    ioctl_cmd[ioctl_n] = cmd;
    if (cmd == 19) {
        *va_arg(ap, int *) = init_rx_empty_value;
    } else if (cmd == 14) {
        ioctl_p[ioctl_n] = va_arg(ap, void *);
        ioctl_a[ioctl_n] = va_arg(ap, int);
    } else if (cmd == 12) {
        ioctl_a[ioctl_n] = va_arg(ap, int);
    } else {
        ioctl_a[ioctl_n] = va_arg(ap, int);
        ioctl_p[ioctl_n] = va_arg(ap, void *);
    }
    va_end(ap);
    ioctl_n++;
    T("ioctrl");
}
static void m_upload(void *hw, void (*cb)(void *, struct ble_rx *)) { uploaded_hw = hw; uploaded_cb = cb; T("upload"); }
static int m_enter_conn(void *hw) { CHECK(hw == hw_token); return enter_conn; }
static struct ble_operation ops;

static void reset_trace(void) { trace[0] = 0; ioctl_n = 0; }

int main(void)
{
    ops.open = m_open; ops.close = m_close; ops.handler_register = m_handler_register;
    ops.initiating = m_initiating; ops.ioctrl = m_ioctrl; ops.upload_data = m_upload;
    ops.is_init_enter_conn = m_enter_conn;
    __ble_ops = &ops;
    le_param.white_list_head.next = le_param.white_list_head.prev = &le_param.white_list_head;

    /* The parameters of a LE Create Connection: interval/window 0x30/0x20,
     * filter policy 0, peer type 1 / 11:12:..:16, own type 2, interval 24,
     * latency 4, timeout 400. */
    u8 params[25] = { 0x30, 0x00, 0x20, 0x00, 0, 1, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 2,
                      24, 0, 24, 0, 4, 0, 0x90, 0x01, 0, 0, 0, 0 };

    /* No initiator role. */
    config_btctler_le_roles = 0;
    CHECK(ll_init_open(params) == 12 && errors == 1 && init_hdl == NULL);
    CHECK(ll_init_close() == 12 && errors == 2);
    CHECK(init_rx_post_handler() == 1);
    config_btctler_le_roles = 16;
    CHECK(ll_init_close() == 0 && nfreed == 0);                 /* never opened */

    /* Open: widening = ((100 + 500) * 24 * 5 / 800 << 1) + 2 = 182; window
     * offset = counter % (24 >> 1) + 12 = 30 % 12 + 12 = 18; the hop is masked
     * to 5 bits; the standard-HCI-off scan interval and window come from the
     * request; the sample copy keeps the sample's unfilled fields. */
    config_btctler_le_features = 0x40; le_param.resolution_enable = 1;
    hop_value = 0x2d; ll_init_random_counter = 30;
    reset_trace();
    CHECK(ll_init_open(params) == 0);
    struct le_init_link *l = init_hdl->init_link;
    CHECK(ll_init_link_get() == l);
    CHECK(l && l->hw == hw_token && registered == &init_handler && registered_priv == l);
    CHECK(ioctl_n == 3 && ioctl_cmd[0] == 12 && ioctl_a[0] == 1);
    CHECK(ioctl_cmd[1] == 9 && ioctl_a[1] == 1 && ioctl_p[1] == l->peer.addr);
    CHECK(ioctl_cmd[2] == 3 && ioctl_a[2] == 1 && ioctl_p[2] == l->peer.addr);
    CHECK(!strcmp(trace, "open handlerreg ioctrl localaddr ioctrl ioctrl aa initiating "));
    CHECK(local_set_type == 2 && l->peer.addr_type == 1 && l->peer.addr[0] == 0x11 && l->peer.addr[5] == 0x16);
    CHECK(init_hdl->conn_param.conn_latency == 4 && init_hdl->conn_param.minimum_ce_length == 0);
    CHECK(l->ll_data.channel[0] == 0x11 && l->ll_data.channel[4] == 0x55);   /* afh_en = 0: the fixed map */
    CHECK(l->ll_data.aa[0] == 0xaa && l->ll_data.aa[1] == 2 && l->ll_data.crcinit[2] == 7 && l->ll_data.winsize == 8);
    CHECK(l->ll_data.interval == 24 && l->ll_data.latency == 4 && l->ll_data.timeout == 400);
    CHECK(l->ll_data.sca == 0 && l->ll_data.hop == 0x0d && l->ll_data.widening == 182 && l->ll_data.winoffset == 18);
    CHECK(l->ll_data.widening_offset == 98);
    CHECK(initiating_calls == 1 && initiating_conn.scan_interval == 0x30 && initiating_conn.scan_windows == 0x20);
    CHECK(initiating_conn.filter_policy == 0);
    CHECK(initiating_conn.ll_data.widening == 182 && initiating_conn.ll_data.winoffset == 18 && initiating_conn.ll_data.aa[0] == 0xaa);
    CHECK(wl_calls == 0 && errors == 2 && infos == 0);

    /* Open again: refused; the first link stays. */
    CHECK(ll_init_open(params) == 12 && errors == 3 && init_hdl->init_link == l);

    /* Close: the radio closes first, then the link and the handle are freed. */
    reset_trace(); nfreed = 0;
    CHECK(ll_init_close() == 0 && closed_hw == hw_token && close_calls == 1);
    CHECK(nfreed == 2 && freed[0] == l && init_hdl == NULL);
    CHECK(ll_init_close() == 0 && nfreed == 2);                  /* already destroyed */

    /* Standard HCI host and afh: fixed scan interval 5 and window 2, the
     * channel map stays as the sample's; filter policy 1 with a white-list
     * miss and an empty list keeps the requested peer. */
    config_btctler_hci_standard = 1; config_btctler_le_afh_en = 1; config_btctler_le_features = 0;
    params[4] = 1; ll_init_random_counter = 3;
    reset_trace(); wl_calls = 0; wl_result = NULL; errors = 0;
    CHECK(ll_init_open(params) == 0);
    l = init_hdl->init_link;
    CHECK(initiating_conn.scan_interval == 5 && initiating_conn.scan_windows == 2 && initiating_conn.filter_policy == 1);
    CHECK(l->ll_data.channel[0] == 1 && l->ll_data.channel[4] == 5);
    CHECK(wl_calls == 1 && wl_hw_seen == hw_token && wl_type_seen == 1 && wl_addr_seen == l->peer.addr && errors == 1);
    CHECK(l->peer.addr_type == 1 && l->peer.addr[0] == 0x11);
    CHECK(ioctl_n == 2);                                         /* no resolution command without the feature */
    CHECK(l->ll_data.winoffset == 3 % 12 + 12);
    ll_init_close();

    /* ...and with a white list: the first entry replaces the peer. */
    wl_a.white_list_param.Address_Type = 3;
    for (int i = 0; i < 6; i++) wl_a.white_list_param.Address[i] = (u8)(0x70 + i);
    le_param.white_list_head.next = &wl_a.entry; le_param.white_list_head.prev = &wl_a.entry;
    wl_a.entry.next = wl_a.entry.prev = &le_param.white_list_head;
    reset_trace(); errors = 0;
    CHECK(ll_init_open(params) == 0);
    l = init_hdl->init_link;
    CHECK(errors == 1 && l->peer.addr_type == 3 && l->peer.addr[0] == 0x70 && l->peer.addr[5] == 0x75);
    CHECK(ioctl_cmd[0] == 9 && ioctl_a[0] == 3);                 /* the radio gets the replaced peer */
    ll_init_close();
    wl_result = &wl_a;                                            /* a match keeps the requested peer */
    reset_trace(); errors = 0;
    CHECK(ll_init_open(params) == 0);
    l = init_hdl->init_link;
    CHECK(errors == 0 && l->peer.addr_type == 1 && l->peer.addr[0] == 0x11);
    ll_init_close();
    le_param.white_list_head.next = le_param.white_list_head.prev = &le_param.white_list_head;

    /* Failures: no memory for the link, and a radio that does not open (the
     * half-built link stays registered, close then reports it). */
    errors = 0; nfreed = 0;
    malloc_fail_at = 2; malloc_calls = 0;                        /* the handle is made, the link is not */
    CHECK(ll_init_open(params) == 12 && errors == 1 && init_hdl && init_hdl->init_link == NULL);
    malloc_fail_at = 0;
    CHECK(ll_init_close() == 2 && init_hdl);                     /* nothing to close, the handle stays */
    CHECK(ll_init_open(params) == 0);                            /* and is reused */
    ll_init_close();
    open_null = 1; errors = 0; nfreed = 0; close_calls = 0;
    CHECK(ll_init_open(params) == 58 && errors == 1 && init_hdl && init_hdl->init_link && init_hdl->init_link->hw == NULL);
    CHECK(ll_init_open(params) == 12);                           /* open twice */
    CHECK(ll_init_close() == 0 && close_calls == 0 && nfreed == 2);   /* no radio to close */
    open_null = 0;
    CHECK(ll_init_open(params) == 0);
    struct le_init_link *keep = init_hdl->init_link;
    init_hdl->init_link = NULL;
    CHECK(ll_init_close() == 2);                                 /* handle without link */
    init_hdl->init_link = keep;
    ll_init_close();

    /* Address processing. */
    CHECK(ll_init_open(params) == 0);
    l = init_hdl->init_link;
    struct { struct ble_rx rx; u8 pdu[16]; } r;
    memset(&r, 0, sizeof(r));
    u8 *rd = (u8 *)&r.rx + offsetof(struct ble_rx, data);   /* the PDU follows the header; the host struct has padding */
    r.rx.type = 0; r.rx.txadd = 1;
    for (int i = 0; i < 12; i++) rd[i] = (u8)(0xa0 + i);

    init_hdl->conn_param.initiator_filter_policy = 0;
    wl_calls = 0; ll_error_code = 9; config_btctler_le_features = 0;
    le_ll_init_addr_process(l, &r.rx);
    CHECK(ll_error_code == 0 && wl_calls == 0 && asserts == 0);
    init_hdl->conn_param.initiator_filter_policy = 1;
    wl_result = NULL;
    le_ll_init_addr_process(l, &r.rx);
    CHECK(ll_error_code == 2 && wl_calls == 1 && wl_type_seen == 1 && wl_addr_seen == r.rx.data && wl_hw_seen == hw_token);
    wl_result = &wl_a;
    le_ll_init_addr_process(l, &r.rx);
    CHECK(ll_error_code == 0);
    r.rx.type = 5; le_ll_init_addr_process(l, &r.rx); CHECK(asserts == 1);
    r.rx.type = 3; le_ll_init_addr_process(l, &r.rx); CHECK(asserts == 2);
    r.rx.type = 0;

    /* With resolution: the white list sees the identity address. */
    config_btctler_le_features = 0x40; le_param.resolution_enable = 1;
    peer_entry.resolving_list_param.Peer_Identity_Address_Type = 1;
    for (int i = 0; i < 6; i++) peer_entry.resolving_list_param.Peer_Identity_Address[i] = (u8)(0x30 + i);
    peer_entry.resolving_list_param.Local_IRK[0] = 0x5a;
    init_hdl->conn_param.own_address_type = 0;
    init_hdl->conn_param.initiator_filter_policy = 1;
    wl_result = NULL; wl_calls = 0; reset_trace();
    le_ll_init_addr_process(l, &r.rx);
    CHECK(ll_error_code == 2 && wl_type_seen == 1 && wl_addr_seen == peer_entry.resolving_list_param.Peer_Identity_Address);
    CHECK(!strcmp(trace, "resolvepeer ioctrl ioctrl wlmatch "));
    CHECK(ioctl_cmd[0] == 2 && ioctl_a[0] == 0 && ioctl_p[0] == rd + 6);   /* rxadd 0 */
    CHECK(ioctl_cmd[1] == 13 && ioctl_p[1] == l->local.addr);
    /* A local random address type with a matching IRK sets the RPA first. */
    init_hdl->conn_param.own_address_type = 3; irk_ok = 1; reset_trace();
    le_ll_init_addr_process(l, &r.rx);
    CHECK(l->local.addr[0] == 0x99 && irk_arg_first == 0x5a);
    CHECK(!strcmp(trace, "resolvepeer irk setrpa ioctrl ioctrl wlmatch "));
    irk_ok = 0; init_hdl->conn_param.own_address_type = 2; reset_trace(); l->local.addr[0] = 0;
    le_ll_init_addr_process(l, &r.rx);
    CHECK(l->local.addr[0] == 0 && !strcmp(trace, "resolvepeer irk ioctrl ioctrl wlmatch "));
    init_hdl->conn_param.own_address_type = 1;
    /* The peer not on the resolving list: no white-list lookup, the filter clears the code. */
    peer_result = NULL; wl_calls = 0; reset_trace();
    le_ll_init_addr_process(l, &r.rx);
    CHECK(ll_error_code == 0 && wl_calls == 0 && !strcmp(trace, "resolvepeer "));
    CHECK(ll_init_state_RPA_process(l, &r.rx) == NULL && ll_error_code == 1);
    /* Directed advertisements also resolve our own address. */
    peer_result = &peer_entry; r.rx.type = 1; r.rx.rxadd = 1; local_result = NULL; reset_trace();
    CHECK(ll_init_state_RPA_process(l, &r.rx) == NULL && ll_error_code == 1 && !strcmp(trace, "resolvepeer resolvelocal "));
    local_result = &local_entry;
    CHECK(ll_init_state_RPA_process(l, &r.rx) == NULL && ll_error_code == 4);
    local_result = &peer_entry; reset_trace();
    CHECK(ll_init_state_RPA_process(l, &r.rx) == &peer_entry && ll_error_code == 0);
    CHECK(ioctl_a[0] == 1 && ioctl_n == 2);
    r.rx.type = 0; r.rx.rxadd = 0;
    le_param.resolution_enable = 0;
    errors = 0;
    CHECK(ll_init_state_RPA_process(l, &r.rx) == NULL && errors == 1 && ll_error_code == 0);
    le_param.resolution_enable = 1; peer_result = &peer_entry;

    /* Radio receive: ADV_IND with resolution waits for the radio to enter
     * the connection before creating it; an unresolvable one is dropped. */
    ll_error_code = 0; wl_result = &wl_a; init_hdl->conn_param.initiator_filter_policy = 1;
    reset_trace(); resume_count = 0; enter_conn = 0; errors = 0;
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(!strcmp(trace, "resolvepeer ioctrl ioctrl wlmatch ioctrl ") && ioctl_cmd[2] == 14 && ioctl_p[2] == &r.rx && ioctl_a[2] == 0);
    CHECK(resume_count == 0 && errors == 0);
    wl_result = NULL; reset_trace();
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(errors == 1 && resume_count == 0 && ioctl_n == 2);
    le_param.resolution_enable = 0; errors = 0;
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(errors == 1 && resume_count == 0);
    le_param.resolution_enable = 1;
    enter_conn = 1; r.rx.txadd = 1; reset_trace();
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(!strcmp(trace, "createconn ") && created.hw == hw_token && created.local == &l->local && created.role == 0);
    CHECK(created.txadd == 1 && created.peer == r.rx.data && created.data == &l->ll_data && resume_count == 1);
    config_btctler_le_features = 0; r.rx.txadd = 0; reset_trace();
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(!strcmp(trace, "createconn ") && created.txadd == 0 && resume_count == 2);
    /* Other types only reach the resume check: link-layer data with no payload is skipped. */
    r.rx.type = 2; r.rx.llid = 1; r.rx.len = 0; reset_trace();
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(!trace[0] && resume_count == 2);
    r.rx.len = 3;
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(!trace[0] && resume_count == 3);
    r.rx.llid = 2; r.rx.len = 0;
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(resume_count == 4);
    config_btctler_le_roles = 0;
    init_handler.rx_probe_handler(l, &r.rx);
    CHECK(resume_count == 4);
    config_btctler_le_roles = 16;

    /* Connection window parameters from the radio. */
    u8 sync[3] = { 5, 0x34, 0x12 };
    init_handler.sync_param_handler(l, sync, 3);
    CHECK(l->ll_data.winsize == 5 && l->ll_data.winoffset == 0x1234);
    config_btctler_le_roles = 0; sync[0] = 6;
    init_handler.sync_param_handler(l, sync, 3);
    CHECK(l->ll_data.winsize == 5);
    config_btctler_le_roles = 16;

    /* Receive post-processing: the empty flag comes from the radio; a
     * CONNECT/ADV post finishes the connection and closes the initiator. */
    reset_trace();
    CHECK(init_rx_post_handler() == 77 && uploaded_hw == hw_token && uploaded_cb != NULL && ioctl_cmd[0] == 19);
    struct le_init_link *linkp = init_hdl->init_link;
    memset(&conn_link, 0, sizeof(conn_link));
    r.rx.type = 2; r.rx.rssi = -40; reset_trace(); close_calls = 0; nfreed = 0;
    uploaded_cb(&conn_link, &r.rx);
    CHECK(!trace[0] && init_hdl);
    r.rx.type = 1;
    uploaded_cb(&conn_link, &r.rx);
    CHECK(!strcmp(trace, "connpost masterrx ") && post_link == &conn_link && master_link == &conn_link && master_rx == &r.rx);
    CHECK(close_calls == 0 && nfreed == 2 && freed[0] == linkp && init_hdl == NULL && conn_link.rssi == -40);
    CHECK(init_rx_post_handler() == 1);
    int errors_before_get = errors;
    CHECK(ll_init_link_get() == NULL && errors == errors_before_get + 1);
    CHECK(ll_init_open(params) == 0);
    init_hdl->init_link = NULL;
    CHECK(init_rx_post_handler() == 1);
    init_hdl->init_link = linkp;

    return TEST_DONE("btctrler_ll_init");
}
#endif
