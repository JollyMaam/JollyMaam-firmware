/* Legacy BLE scanner: scan parameters, the radio link, the duplicate-report
 * filter and the receive paths that turn scanner PDUs into HCI reports.
 * Keeps the SDK's quirks: the handle is allocated without being cleared, the
 * duplicate filter keeps ten CRC16 entries with a repeat count of ten, the
 * Scanning_Filter_Policy is remapped when the feature bit 7 is clear, and
 * the receive-empty word read after upload_data is left unset by the SDK. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_SCAN, log_tag_const_i_LL_SCAN;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_btctler_hci_standard;
extern void log_print(int level, const char *tag, const char *format, ...);
extern struct le_parameter le_param;

#define LL_CODE __attribute__((section(".ble_ll_code")))

static struct ll_scan_hdl *scan_hdl __attribute__((section(".ble_ll_data"), aligned(4))) = NULL;
static void scan_rx_probe_handler(void *priv, struct ble_rx *rx) LL_CODE;
static const struct ble_handler scan_handler
    __attribute__((section(".ble_ll_const"), aligned(4))) = {
        .rx_probe_handler = scan_rx_probe_handler,
};

static void ll_scan_init(void) LL_CODE;
static void __scan_rx_post_handler(void *priv, struct ble_rx *rx) LL_CODE;

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

void le_scan_filter_duplicates_init(void) LL_CODE;
void le_scan_filter_duplicates_init(void)
{
    INIT_LIST_HEAD(&scan_hdl->adv_report_filter_head);
}

void le_scan_filter_duplicates_query(void) LL_CODE;
void le_scan_filter_duplicates_query(void)
{
    int count = 0;
    struct list_head *head = &scan_hdl->adv_report_filter_head;
    for (struct adv_report *p = (struct adv_report *)head->next; &p->entry != head;
         p = (struct adv_report *)p->entry.next)
        count++;
    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]scan_filter_duplicates_query : %d\r\n", count);
}

/* Returns 1 to drop a report whose payload CRC16 was seen recently. */
bool le_scan_filter_duplicates(struct ble_rx *rx) LL_CODE;
bool le_scan_filter_duplicates(struct ble_rx *rx)
{
    u16 crc = crc_get_16bit(&rx->data[6], rx->len - 6);
    u8 count = 0, bingo = 0;

    if (!scan_hdl->scan_enable_param.filter_duplicates) return 0;
    struct list_head *head = &scan_hdl->adv_report_filter_head;
    for (struct adv_report *p = (struct adv_report *)head->next; &p->entry != head;
         p = (struct adv_report *)p->entry.next) {
        if (p->crc16 == crc) {
            /* Every entry is still visited after a hit; the tenth repeat
             * is let through and restarts the count. */
            u8 repeat = p->repeat_count + 1;
            p->repeat_count = repeat;
            if (repeat >= 10) {
                p->repeat_count = 0;
                return 0;
            }
            bingo = 1;
        }
        count++;
    }
    if (bingo == 1) return 1;
    if (count > 9) {
        struct list_head *oldest = head->next;
        list_del(oldest);
        __bt_free(oldest);
    }
    struct adv_report *entry = __bt_malloc(12);
    if (!entry) btctrler_assert_failed("p Adv Report NULL", 98);
    entry->crc16 = crc;
    entry->repeat_count = 0;
    list_add_tail(&entry->entry, &scan_hdl->adv_report_filter_head);
    return 0;
}

void le_scan_filter_duplicates_destroy(void) LL_CODE;
void le_scan_filter_duplicates_destroy(void)
{
    struct list_head *p = scan_hdl->adv_report_filter_head.next;
    while (p != &scan_hdl->adv_report_filter_head) {
        struct list_head *next = p->next;
        list_del(p);
        __bt_free(p);
        p = next;
    }
}

void ll_scan_destroy(void) LL_CODE;
void ll_scan_destroy(void)
{
    ll_scan_close();
    if (scan_hdl) {
        if (log_tag_const_i_LL_SCAN)
            log_print(2, NULL, "[Info]: [LL_SCAN]LL Scan - destroy\r\n");
        __bt_free(scan_hdl);
        scan_hdl = NULL;
    }
}

int ll_scan_close(void) LL_CODE;
int ll_scan_close(void)
{
    if (!(config_btctler_le_roles & 8)) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]Not support\r\n");
        return 12;
    }
    if (!scan_hdl) {
        if (log_tag_const_i_LL_SCAN)
            log_print(2, NULL, "[Info]: [LL_SCAN]LL Scan already destroy\r\n");
        return 0;
    }
    struct le_scan_link *link = scan_hdl->scan_link;
    if (!link) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return 2;
    }
    if (link->hw)
        __ble_ops->close(link->hw);
    le_scan_filter_duplicates_destroy();
    __bt_free(link);
    scan_hdl->scan_link = NULL;
    return 0;
}

int scan_rx_post_handler(void) LL_CODE;
int scan_rx_post_handler(void)
{
    /* The SDK leaves this output word uninitialized before ioctrl writes it. */
    int empty;
    if (!(config_btctler_le_roles & 8) || !scan_hdl || !scan_hdl->scan_link)
        return 1;
    struct le_scan_link *link = scan_hdl->scan_link;
    __ble_ops->upload_data(link->hw, __scan_rx_post_handler);
    __ble_ops->ioctrl(link->hw, 19, &empty);
    return empty;
}

static void __scan_rx_post_handler(void *priv, struct ble_rx *rx)
{
    u8 rssi = rx->rssi;
    struct le_scan_link *link = priv;

    switch (rx->type) {
    case LL_PDU_ADV_IND:
        le_advertising_report_event(rx, 0, rssi);
        break;
    case LL_PDU_ADV_DIRECT_IND:
        if ((config_btctler_le_features & 0x80) &&
            (scan_hdl->scan_param.Scanning_Filter_Policy & (u8)~1) == 2 &&
            ll_resolvable_private_addr_verify(&rx->data[6]) == 1) {
            le_direct_advertising_report_event(rx, rssi);
            break;
        }
        /* Directed to our own address: report it with the address only. */
        if (!memcmp(&rx->data[6], link->local.addr, 6) && rx->rxadd == link->local.addr_type) {
            rx->len = 6;
            le_advertising_report_event(rx, 1, rssi);
        }
        break;
    case LL_PDU_ADV_SCAN_IND:
        le_advertising_report_event(rx, 2, rssi);
        break;
    case LL_PDU_ADV_NONCONN_IND:
        le_advertising_report_event(rx, 3, rssi);
        break;
    case LL_PDU_SCAN_RSP:
        le_advertising_report_event(rx, 4, rssi);
        break;
    default:
        break;
    }
}

int ll_scan_set_params(const u8 *data) LL_CODE;
int ll_scan_set_params(const u8 *data)
{
    if (!(config_btctler_le_roles & 8)) return 12;
    if (!scan_hdl) ll_scan_init();
    if (scan_hdl->enable) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]The Host shall not issue this command when scanning is enabled\r\n");
        return 12;
    }
    memcpy(&scan_hdl->scan_param, data, 7);
    return 0;
}

static void ll_scan_init(void)
{
    if (scan_hdl) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]LL Scan - already init\r\n");
        return;
    }
    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]LL Scan- init\r\n");
    /* Not cleared: the SDK leaves enable and scan_link as the heap gave them. */
    scan_hdl = __bt_malloc(32);
    if (!scan_hdl) btctrler_assert_failed("__this LL SCAN HDL NULL", 127);
}

/* Programs the radio's scan window and own address. */
static int __set_ll_scan_state(struct le_scan_link *link)
{
    struct ble_scan scan;

    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    scan.type = scan_hdl->scan_param.LE_Scan_Type;
    if (!config_btctler_hci_standard) {
        scan.interval = scan_hdl->scan_param.LE_Scan_Interval;
        scan.window = scan_hdl->scan_param.LE_Scan_Window;
    } else {
        scan.interval = 5;
        scan.window = 2;
    }
    scan.filter_policy = scan_hdl->scan_param.Scanning_Filter_Policy;
    if (log_tag_const_i_LL_SCAN) {
        log_print(2, NULL, "[Info]: [LL_SCAN]scan active: %x\r\n", scan.type);
        log_print(2, NULL, "[Info]: [LL_SCAN]scan interval (N*625us): %x\r\n", scan.interval);
        log_print(2, NULL, "[Info]: [LL_SCAN]scan window: %x\r\n", scan.window);
        log_print(2, NULL, "[Info]: [LL_SCAN]scan filter_policy: %x\r\n", scan.filter_policy);
    }
    if (!(config_btctler_le_features & 0x80)) {
        if (scan.filter_policy == 2) scan.filter_policy = 0;
        else if (scan.filter_policy == 3) scan.filter_policy = 1;
    }
    scan.filter_duplicates = scan_hdl->scan_enable_param.filter_duplicates;
    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]filter_duplicates : %x\r\n", scan.filter_duplicates);
    __set_ll_local_addr(link->hw, &link->local, scan_hdl->scan_param.Own_Address_Type);
    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    return __ble_ops->scanning(link->hw, &scan);
}

int ll_scan_open(const u8 *data) LL_CODE;
int ll_scan_open(const u8 *data)
{
    if (!(config_btctler_le_roles & 8)) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]Not support\r\n");
        return 12;
    }
    if (!scan_hdl) ll_scan_init();
    if (log_tag_const_i_LL_SCAN)
        log_print(2, NULL, "[Info]: [LL_SCAN]ll_scan_open\r\n");
    if (scan_hdl->scan_link) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]scan link open twice\r\n");
        return 12;
    }
    struct le_scan_link *link = __bt_malloc(20);
    if (!link) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]scan link NULL\r\n");
        return 12;
    }
    scan_hdl->scan_link = link;
    link->hw = __ble_ops->open();
    if (!link->hw) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]scan hw NULL\r\n");
        return 58;
    }
    __ble_ops->handler_register(link->hw, link, &scan_handler);
    if (config_btctler_le_features & 64)
        __ble_ops->ioctrl(link->hw, 12, le_param.resolution_enable);
    /* enable and filter_duplicates, copied as one unaligned halfword. */
    memcpy(&scan_hdl->scan_enable_param, data, 2);
    le_scan_filter_duplicates_init();
    __set_ll_scan_state(link);
    return 0;
}

struct le_scan_link *ll_scan_link_get(void) LL_CODE;
struct le_scan_link *ll_scan_link_get(void)
{
    if (!scan_hdl) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]LL Scan not init yet\r\n");
        return NULL;
    }
    return scan_hdl->scan_link;
}

/* Resolves the advertiser's address and hands our RPA to the radio. */
static struct resolving_list *ll_scan_state_RPA_process(void *priv, struct ble_rx *rx)
{
    struct le_scan_link *link = priv;

    ll_error_code = 0;
    if (!(config_btctler_le_features & 64)) return NULL;
    if (!le_param.resolution_enable) {
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]resolution still disable\r\n");
        return NULL;
    }
    struct resolving_list *resolved = ll_resolve_peer_addr(rx->data);
    if (!resolved) {
        ll_error_code = RESOLVE_ADDR_FAIL;
        if (log_tag_const_e_LL_SCAN)
            log_print(4, NULL, "<Error>: [LL_SCAN]RESOLVE_ADDR_FAIL\r\n");
        return NULL;
    }
    if ((scan_hdl->scan_param.Own_Address_Type & (u8)~1) == 2 &&
        ll_resolve_list_IRK_verify(resolved->resolving_list_param.Local_IRK) == 1)
        ll_resolve_list_set_local_RPA(link->local.addr, resolved);
    __ble_ops->ioctrl(link->hw, 2, link->local.addr_type, link->local.addr);
    return resolved;
}

/* Device filter by white list; the SDK clears the resolution error first. */
static void ll_scan_state_filter_process(void *hw, struct ble_rx *rx, struct resolving_list *resolved)
{
    struct white_list *match;

    ll_error_code = 0;
    switch (scan_hdl->scan_param.Scanning_Filter_Policy) {
    case 1:
        if (config_btctler_le_features & 64) {
            if (!resolved || !le_param.resolution_enable) return;
            match = ll_white_list_match(hw, resolved->resolving_list_param.Peer_Identity_Address_Type,
                                        resolved->resolving_list_param.Peer_Identity_Address);
        } else {
            match = ll_white_list_match(hw, rx->txadd, rx->data);
        }
        if (!match) ll_error_code = DEVICE_FILTER_FAILED;
        break;
    case 2:
        if (config_btctler_le_features & 0x80) return;
        btctrler_assert_failed("0 ll_scan_state_filter_process", 175);
        break;
    case 3:
        if (config_btctler_le_features & 0x80) {
            if (config_btctler_le_features & 64) {
                if (!resolved || !le_param.resolution_enable) return;
                match = ll_white_list_match(hw, resolved->resolving_list_param.Peer_Identity_Address_Type,
                                            resolved->resolving_list_param.Peer_Identity_Address);
            } else {
                match = ll_white_list_match(hw, rx->txadd, rx->data);
            }
            if (!match) ll_error_code = DEVICE_FILTER_FAILED;
        } else {
            btctrler_assert_failed("0 ll_scan_state_filter_process", 199);
        }
        break;
    default:
        break;
    }
}

static void scan_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    if (!(config_btctler_le_roles & 8)) return;
    if (rx->type == LL_PDU_SCAN_REQ || rx->type == LL_PDU_CONNECT_REQ) {
        if (rx->llid != 1 || rx->len != 0)
            ll_thread_resume();
        return;
    }
    struct resolving_list *resolved = ll_scan_state_RPA_process(priv, rx);
    ll_scan_state_filter_process(*(void **)priv, rx, resolved);
    if (ll_error_code) {
        ll_thread_resume();
        return;
    }
    __ble_ops->ioctrl(*(void **)priv, 14, rx, 0);
    if (rx->llid != 1 || rx->len != 0)
        ll_thread_resume();
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>
int config_btctler_le_roles = 0;
u64 config_btctler_le_features = 0;
int config_btctler_hci_standard = 0;
int config_asser = 1;
u8 log_tag_const_e_LL_SCAN = 0, log_tag_const_i_LL_SCAN = 0;
u8 ll_error_code;
struct le_parameter le_param;
const struct ble_operation *__ble_ops;

static struct { const char *format; int arg; } logs[64];
static int nlogs, mallocs, frees, fail_malloc, asserts, resumes, query_count;
static int radio, opens, closes, ioctl_n, direct_events, local_addr_calls;
static void *open_result = &radio;
static int scan_result = 5, empty_value = 7, last_ioctl_cmd, last_ioctl_a, ioctl_12_value = -1;
static int crc_calls, crc_len, white_calls, rpa_sets, irk_verifies, verify_result, irk_result;
static u16 crc_next;
static const u8 *crc_ptr, *white_addr, *rpa_addr;
static u8 white_type, ioctl2_type;
static const u8 *ioctl2_addr;
static void *white_hw, *last_ioctl_ptr, *registered_priv, *registered_hw;
static const struct ble_handler *registered;
static struct white_list *white_result;
static struct resolving_list *resolved_entry, *rpa_entry;
static struct ble_scan programmed;
static u8 local_type_seen;
static struct { struct ble_rx *rx; int event; u8 rssi; } events[16];
static int nevents;
static struct ble_rx *upload_rx;

void log_print(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    (void)tag;
    CHECK(level == 2 || level == 4);
    va_start(ap, format);
    if (nlogs < 64) { logs[nlogs].format = format; logs[nlogs].arg = strstr(format, "%") ? va_arg(ap, int) : -1; nlogs++; }
    if (strstr(format, "scan_filter_duplicates_query")) query_count = logs[nlogs - 1].arg;
    va_end(ap);
}
static int logged(const char *text) { for (int i = 0; i < nlogs; i++) if (strstr(logs[i].format, text)) return 1; return 0; }
void *__bt_malloc(int size)
{
    size_t host = size == 32 ? sizeof(struct ll_scan_hdl) : size == 20 ? sizeof(struct le_scan_link) : sizeof(struct adv_report);
    CHECK(size == 32 || size == 20 || size == 12);
    if (fail_malloc) return NULL;
    mallocs++;
    return calloc(1, host);
}
void __bt_free(void *p) { CHECK(p != NULL); frees++; free(p); }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
u16 crc_get_16bit(const void *src, u32 len) { crc_calls++; crc_ptr = src; crc_len = len; return crc_next; }
void ll_thread_resume(void) { resumes++; }
void __set_ll_local_addr(void *hw, struct le_addr *addr, u8 own_addr_type)
{
    CHECK(hw == &radio);
    local_addr_calls++;
    local_type_seen = own_addr_type;
    addr->addr_type = 1;
    for (int i = 0; i < 6; i++) addr->addr[i] = 0x10 + i;
}
void le_advertising_report_event(struct ble_rx *rx, u8 event_type, u8 rssi)
{
    events[nevents].rx = rx; events[nevents].event = event_type; events[nevents].rssi = rssi; nevents++;
}
void le_direct_advertising_report_event(struct ble_rx *rx, u8 rssi)
{
    events[nevents].rx = rx; events[nevents].event = 100; events[nevents].rssi = rssi; nevents++; direct_events++;
}
bool ll_resolvable_private_addr_verify(const u8 *addr) { (void)addr; return verify_result; }
struct resolving_list *ll_resolve_peer_addr(const u8 *addr) { (void)addr; return resolved_entry; }
bool ll_resolve_list_IRK_verify(const u8 *irk) { irk_verifies++; CHECK(resolved_entry && irk == resolved_entry->resolving_list_param.Local_IRK); return irk_result; }
void ll_resolve_list_set_local_RPA(u8 *addr, struct resolving_list *entry) { rpa_sets++; rpa_addr = addr; rpa_entry = entry; }
struct white_list *ll_white_list_match(void *priv, u8 addr_type, const u8 *addr)
{
    white_calls++; white_hw = priv; white_type = addr_type; white_addr = addr; return white_result;
}
static void *op_open(void) { opens++; return open_result; }
static void op_close(void *hw) { CHECK(hw == &radio); closes++; }
static void op_register(void *hw, void *priv, const struct ble_handler *handler)
{
    registered_hw = hw; registered_priv = priv; registered = handler;
}
static int op_scanning(void *hw, struct ble_scan *scan) { CHECK(hw == &radio); programmed = *scan; return scan_result; }
static void op_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    CHECK(hw == &radio);
    ioctl_n++; last_ioctl_cmd = cmd;
    va_start(ap, cmd);
    if (cmd == 12) ioctl_12_value = va_arg(ap, int);
    else if (cmd == 2) { ioctl2_type = va_arg(ap, int); ioctl2_addr = va_arg(ap, const u8 *); }
    else if (cmd == 14) { last_ioctl_ptr = va_arg(ap, void *); last_ioctl_a = va_arg(ap, int); }
    else if (cmd == 19) *va_arg(ap, int *) = empty_value;
    else CHECK(0);
    va_end(ap);
}
static void op_upload(void *hw, void (*callback)(void *, struct ble_rx *))
{
    CHECK(hw == &radio);
    callback(scan_hdl->scan_link, upload_rx);
}
static const struct ble_operation operations = {
    .open = op_open, .close = op_close, .handler_register = op_register, .scanning = op_scanning,
    .ioctrl = op_ioctrl, .upload_data = op_upload,
};

static u8 rxbuf[128] __attribute__((aligned(8)));
static struct ble_rx *rx = (struct ble_rx *)rxbuf;
static void make_rx(int type, int llid, int len)
{
    memset(rxbuf, 0, sizeof(rxbuf));
    rx->type = type; rx->llid = llid; rx->len = len; rx->rssi = -40;
}
static int list_count(void)
{
    int n = 0;
    for (struct list_head *p = scan_hdl->adv_report_filter_head.next; p != &scan_hdl->adv_report_filter_head; p = p->next) n++;
    return n;
}
static struct adv_report *list_nth(int n)
{
    struct list_head *p = scan_hdl->adv_report_filter_head.next;
    while (n--) p = p->next;
    return (struct adv_report *)p;
}
static void reset_counts(void)
{
    nlogs = nevents = resumes = asserts = ioctl_n = white_calls = rpa_sets = irk_verifies = direct_events = 0;
    ll_error_code = 0;
}

int main(void)
{
    __ble_ops = &operations;
    u8 params[7] = {1, 0x30, 0x00, 0x12, 0x00, 2, 3};
    u8 enable[2] = {1, 1};

    /* Without the LE central role (bit 3) every entry point refuses. */
    CHECK(ll_scan_set_params(params) == 12 && ll_scan_open(enable) == 12 && ll_scan_close() == 12);
    CHECK(scan_rx_post_handler() == 1 && mallocs == 0 && ll_scan_link_get() == NULL);
    make_rx(0, 2, 5);
    scan_rx_probe_handler(&radio, rx);
    CHECK(resumes == 0 && ioctl_n == 0);
    config_btctler_le_roles = 8;
    CHECK(ll_scan_close() == 0);                       /* nothing to close */
    CHECK(scan_rx_post_handler() == 1);
    CHECK(ll_scan_link_get() == NULL);

    /* Parameters: 7 bytes, refused while scanning is marked enabled. */
    log_tag_const_i_LL_SCAN = 1;
    CHECK(ll_scan_set_params(params) == 0 && scan_hdl && mallocs == 1);
    CHECK(logged("LL Scan- init"));
    log_tag_const_i_LL_SCAN = 0;
    CHECK(scan_hdl->scan_param.LE_Scan_Type == 1 && scan_hdl->scan_param.LE_Scan_Interval == 0x30);
    CHECK(scan_hdl->scan_param.LE_Scan_Window == 0x12 && scan_hdl->scan_param.Own_Address_Type == 2);
    CHECK(scan_hdl->scan_param.Scanning_Filter_Policy == 3);
    scan_hdl->enable = 1;
    u8 other[7] = {0, 9, 9, 9, 9, 9, 9};
    CHECK(ll_scan_set_params(other) == 12 && scan_hdl->scan_param.LE_Scan_Type == 1);
    scan_hdl->enable = 0;
    CHECK(ll_scan_close() == 2);                       /* handle without a link */
    CHECK(ll_scan_link_get() == NULL);

    /* Open without the privacy and extended-policy features: policy 3 maps to 1. */
    CHECK(ll_scan_open(enable) == 0);                  /* scanning() status 5 is not returned */
    struct le_scan_link *link = scan_hdl->scan_link;
    CHECK(link && link->hw == &radio && ll_scan_link_get() == link && mallocs == 2);
    CHECK(registered_hw == &radio && registered_priv == link && registered == &scan_handler);
    CHECK(ioctl_n == 0);
    CHECK(scan_hdl->scan_enable_param.enable == 1 && scan_hdl->scan_enable_param.filter_duplicates == 1);
    CHECK(list_count() == 0 && local_addr_calls == 1 && local_type_seen == 2);
    CHECK(programmed.type == 1 && programmed.interval == 0x30 && programmed.window == 0x12);
    CHECK(programmed.filter_policy == 1 && programmed.filter_duplicates == 1);
    CHECK(link->local.addr_type == 1 && link->local.addr[5] == 0x15);
    CHECK(ll_scan_open(enable) == 12);                 /* open twice */

    /* Duplicate filter: ten CRC16 entries, tenth repeat passes. */
    make_rx(0, 2, 20);
    crc_next = 0x1111;
    scan_hdl->scan_enable_param.filter_duplicates = 0;
    CHECK(le_scan_filter_duplicates(rx) == 0 && list_count() == 0);
    CHECK(crc_calls == 1 && crc_ptr == &rx->data[6] && crc_len == 14);
    scan_hdl->scan_enable_param.filter_duplicates = 1;
    int before = mallocs;
    CHECK(le_scan_filter_duplicates(rx) == 0 && mallocs == before + 1);
    CHECK(list_count() == 1 && list_nth(0)->crc16 == 0x1111 && list_nth(0)->repeat_count == 0);
    for (int hit = 1; hit <= 9; hit++) {
        CHECK(le_scan_filter_duplicates(rx) == 1);
        CHECK(list_nth(0)->repeat_count == hit);
    }
    CHECK(le_scan_filter_duplicates(rx) == 0 && list_nth(0)->repeat_count == 0);
    CHECK(le_scan_filter_duplicates(rx) == 1 && list_nth(0)->repeat_count == 1);
    CHECK(mallocs == before + 1);
    for (int i = 0; i < 9; i++) { crc_next = 0x2000 + i; CHECK(le_scan_filter_duplicates(rx) == 0); }
    CHECK(list_count() == 10);
    log_tag_const_i_LL_SCAN = 1;
    nlogs = 0;
    le_scan_filter_duplicates_query();
    CHECK(query_count == 10 && nlogs == 1);
    log_tag_const_i_LL_SCAN = 0;
    nlogs = 0;
    le_scan_filter_duplicates_query();
    CHECK(nlogs == 0);
    int freed = frees;
    crc_next = 0x3000;                                  /* eleventh: the oldest goes */
    CHECK(le_scan_filter_duplicates(rx) == 0 && frees == freed + 1 && list_count() == 10);
    CHECK(list_nth(0)->crc16 == 0x2000 && list_nth(9)->crc16 == 0x3000);
    crc_next = 0x1111;                                  /* forgotten: new again, evicts 0x2000 */
    CHECK(le_scan_filter_duplicates(rx) == 0 && frees == freed + 2);
    CHECK(list_nth(0)->crc16 == 0x2001 && list_nth(9)->crc16 == 0x1111 && list_nth(9)->repeat_count == 0);
    crc_next = 0x3000;
    CHECK(le_scan_filter_duplicates(rx) == 1 && list_count() == 10);

    /* Receive path: reports by PDU type with the stored RSSI. */
    upload_rx = rx;
    reset_counts();
    for (int t = 0; t < 8; t++) {
        make_rx(t, 2, 20);
        CHECK(scan_rx_post_handler() == 7);
    }
    /* In type order 0, 2, 4, 6 the events are 0, 3, 4, 2; type 1 needs an address match; 3, 5, 7 are ignored. */
    CHECK(nevents == 4);
    CHECK(events[0].event == 0 && events[1].event == 3 && events[2].event == 4 && events[3].event == 2);
    CHECK(events[0].rssi == 0xd8 && events[3].rssi == 0xd8 && events[0].rx == rx);
    /* Directed advertising to our own address (public/random bit = rxadd). */
    reset_counts();
    make_rx(1, 2, 20);
    for (int i = 0; i < 6; i++) rx->data[6 + i] = 0x10 + i;
    rx->rxadd = 1;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 1 && rx->len == 6);
    reset_counts();
    make_rx(1, 2, 20);
    for (int i = 0; i < 6; i++) rx->data[6 + i] = 0x10 + i;
    rx->rxadd = 0;
    scan_rx_post_handler();
    CHECK(nevents == 0 && rx->len == 20);
    rx->rxadd = 1; rx->data[11] ^= 1;
    scan_rx_post_handler();
    CHECK(nevents == 0);
    rx->data[11] ^= 1;
    /* Resolvable-address verification only with feature bit 7 and policy 2 or 3. */
    verify_result = 1;
    scan_hdl->scan_param.Scanning_Filter_Policy = 2;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 1);       /* bit 7 clear: plain match */
    nevents = 0; make_rx(1, 2, 20);
    for (int i = 0; i < 6; i++) rx->data[6 + i] = 0x10 + i;
    rx->rxadd = 1;
    config_btctler_le_features = 0x80;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 100 && rx->len == 20);
    nevents = 0; scan_hdl->scan_param.Scanning_Filter_Policy = 3;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 100);
    nevents = 0; scan_hdl->scan_param.Scanning_Filter_Policy = 1;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 1 && rx->len == 6);
    nevents = 0; rx->len = 20; scan_hdl->scan_param.Scanning_Filter_Policy = 2; verify_result = 0;
    scan_rx_post_handler();
    CHECK(nevents == 1 && events[0].event == 1);
    config_btctler_le_features = 0;

    /* Probe handler: SCAN_REQ and CONNECT_REQ only wake the thread. */
    reset_counts();
    make_rx(3, 1, 0);
    scan_rx_probe_handler(link, rx);
    CHECK(resumes == 0 && ioctl_n == 0);
    rx->len = 1; scan_rx_probe_handler(link, rx);
    CHECK(resumes == 1);
    make_rx(5, 2, 0); scan_rx_probe_handler(link, rx);
    CHECK(resumes == 2 && ioctl_n == 0);
    /* Ordinary report: forwarded to the radio with a zero second word. */
    make_rx(0, 1, 0);
    scan_hdl->scan_param.Scanning_Filter_Policy = 0;
    scan_rx_probe_handler(link, rx);
    CHECK(resumes == 2 && ioctl_n == 1 && last_ioctl_cmd == 14 && last_ioctl_ptr == rx && last_ioctl_a == 0);
    make_rx(0, 1, 5);
    scan_rx_probe_handler(link, rx);
    CHECK(resumes == 3 && ioctl_n == 2);
    /* White-list policy 1 without privacy: txadd and the advertiser address. */
    reset_counts();
    make_rx(0, 1, 5); rx->txadd = 1;
    scan_hdl->scan_param.Scanning_Filter_Policy = 1;
    white_result = NULL;
    scan_rx_probe_handler(link, rx);
    CHECK(white_calls == 1 && white_hw == &radio && white_type == 1 && white_addr == rx->data);
    CHECK(ll_error_code == DEVICE_FILTER_FAILED && resumes == 1 && ioctl_n == 0);
    reset_counts();
    white_result = (struct white_list *)&radio;
    scan_rx_probe_handler(link, rx);
    CHECK(ll_error_code == 0 && resumes == 1 && ioctl_n == 1);
    /* Policy 2 needs feature bit 7 (else the SDK asserts and carries on); 3 filters with it. */
    reset_counts();
    scan_hdl->scan_param.Scanning_Filter_Policy = 2;
    scan_rx_probe_handler(link, rx);
    CHECK(asserts == 1 && white_calls == 0 && ioctl_n == 1);
    reset_counts();
    scan_hdl->scan_param.Scanning_Filter_Policy = 3;
    scan_rx_probe_handler(link, rx);
    CHECK(asserts == 1 && white_calls == 0 && ioctl_n == 1);
    config_btctler_le_features = 0x80;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(asserts == 0 && white_calls == 1 && ioctl_n == 1);
    white_result = NULL;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(white_calls == 1 && ioctl_n == 0 && resumes == 1 && ll_error_code == DEVICE_FILTER_FAILED);
    scan_hdl->scan_param.Scanning_Filter_Policy = 2;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(asserts == 0 && white_calls == 0 && ioctl_n == 1);
    /* Privacy: the resolved identity feeds the white list; the local RPA goes to the radio. */
    static struct resolving_list peer;
    peer.resolving_list_param.Peer_Identity_Address_Type = 3;
    for (int i = 0; i < 6; i++) peer.resolving_list_param.Peer_Identity_Address[i] = 0x70 + i;
    config_btctler_le_features = 64;
    le_param.resolution_enable = 1;
    resolved_entry = &peer;
    scan_hdl->scan_param.Own_Address_Type = 3; irk_result = 1;
    scan_hdl->scan_param.Scanning_Filter_Policy = 1;
    white_result = (struct white_list *)&radio;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(irk_verifies == 1 && rpa_sets == 1 && rpa_addr == link->local.addr && rpa_entry == &peer);
    CHECK(ioctl2_type == link->local.addr_type && ioctl2_addr == link->local.addr);
    CHECK(white_calls == 1 && white_type == 3 && white_addr == peer.resolving_list_param.Peer_Identity_Address);
    CHECK(ioctl_n == 2 && last_ioctl_cmd == 14);
    irk_result = 0; scan_hdl->scan_param.Own_Address_Type = 1;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(irk_verifies == 0 && rpa_sets == 0 && ioctl_n == 2);
    /* A failed resolution sets the error, but the filter stage clears it again. */
    resolved_entry = NULL;
    scan_hdl->scan_param.Scanning_Filter_Policy = 0;
    reset_counts();
    log_tag_const_e_LL_SCAN = 1;
    scan_rx_probe_handler(link, rx);
    CHECK(logged("RESOLVE_ADDR_FAIL") && ll_error_code == 0 && ioctl_n == 1 && resumes == 1);
    /* With the white list and nothing resolved, the filter passes the report. */
    scan_hdl->scan_param.Scanning_Filter_Policy = 1;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(white_calls == 0 && ioctl_n == 1);
    le_param.resolution_enable = 0;
    reset_counts();
    scan_rx_probe_handler(link, rx);
    CHECK(logged("resolution still disable") && white_calls == 0 && ioctl_n == 1);
    log_tag_const_e_LL_SCAN = 0;
    config_btctler_le_features = 0;

    /* Open variants: privacy command, standard-HCI window, feature bit 7. */
    CHECK(ll_scan_close() == 0 && scan_hdl->scan_link == NULL);
    CHECK(closes == 1 && list_count() == 0);
    config_btctler_le_features = 64;
    le_param.resolution_enable = 1;
    config_btctler_hci_standard = 1;
    CHECK(ll_scan_open(enable) == 0);
    CHECK(ioctl_12_value == 1 && programmed.interval == 5 && programmed.window == 2);
    CHECK(programmed.filter_policy == 1);
    CHECK(ll_scan_close() == 0);
    config_btctler_le_features = 0x80;
    config_btctler_hci_standard = 0;
    scan_hdl->scan_param.Scanning_Filter_Policy = 3;
    CHECK(ll_scan_open(enable) == 0 && programmed.filter_policy == 3);
    CHECK(programmed.interval == 0x30 && programmed.window == 0x12);
    CHECK(ll_scan_close() == 0);
    config_btctler_le_features = 0;
    scan_hdl->scan_param.Scanning_Filter_Policy = 2;
    CHECK(ll_scan_open(enable) == 0 && programmed.filter_policy == 0);
    CHECK(ll_scan_close() == 0);
    scan_hdl->scan_param.Scanning_Filter_Policy = 0;
    CHECK(ll_scan_open(enable) == 0 && programmed.filter_policy == 0);
    CHECK(ll_scan_close() == 0);
    /* Logs of one open, in the SDK's order. */
    log_tag_const_i_LL_SCAN = 1;
    scan_hdl->scan_param.Scanning_Filter_Policy = 3;
    nlogs = 0;
    CHECK(ll_scan_open(enable) == 0);
    CHECK(nlogs == 8 && strstr(logs[0].format, "ll_scan_open") && strstr(logs[1].format, ">>>>"));
    CHECK(strstr(logs[2].format, "scan active") && logs[2].arg == 1);
    CHECK(strstr(logs[3].format, "scan interval") && logs[3].arg == 0x30);
    CHECK(strstr(logs[4].format, "scan window") && logs[4].arg == 0x12);
    CHECK(strstr(logs[5].format, "scan filter_policy") && logs[5].arg == 3);
    CHECK(strstr(logs[6].format, "filter_duplicates") && logs[6].arg == 1 && strstr(logs[7].format, ">>>>"));
    CHECK(ll_scan_close() == 0);
    log_tag_const_i_LL_SCAN = 0;
    /* Errors: no heap for the link, no radio (the link stays set). */
    fail_malloc = 1;
    CHECK(ll_scan_open(enable) == 12 && scan_hdl->scan_link == NULL);
    fail_malloc = 0;
    open_result = NULL;
    CHECK(ll_scan_open(enable) == 58 && scan_hdl->scan_link && scan_hdl->scan_link->hw == NULL);
    int closed = closes;
    CHECK(ll_scan_close() == 0 && closes == closed && scan_hdl->scan_link == NULL);
    open_result = &radio;

    /* Destroy closes the radio, frees the entries and the handle. */
    CHECK(ll_scan_open(enable) == 0);
    crc_next = 1; CHECK(le_scan_filter_duplicates(rx) == 0);
    crc_next = 2; CHECK(le_scan_filter_duplicates(rx) == 0);
    CHECK(list_count() == 2);
    freed = frees; closed = closes;
    ll_scan_destroy();
    CHECK(scan_hdl == NULL && frees == freed + 4 && closes == closed + 1);
    freed = frees;
    ll_scan_destroy();
    CHECK(frees == freed);
    return TEST_DONE("btctrler_ll_scan");
}
#endif
