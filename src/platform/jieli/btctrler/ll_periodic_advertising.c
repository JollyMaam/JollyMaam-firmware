/* LE periodic advertising and periodic advertising sync: the scanner side
 * (create sync, cancel, terminate, the sync timeout timer and the periodic
 * advertising report events) and the advertiser side (open and close the
 * periodic link, refresh its data).  SDK quirks kept: the sync timeout timer
 * is (Sync_Timeout, at least 10) * 10 ms; creating a sync dereferences the
 * scan parameters' period_sync without a check; the report paths read the
 * period_sync pointers fresh after each callback; the stack PDU is cleared
 * as one 8-byte store; a failed radio open frees the link without unlinking
 * it (it was never linked); the assertion text is the SDK's expression. */
#include "btctrler/glue.h"
#include <stdio.h>
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_PADV, log_tag_const_i_LL_PADV;
extern BTCTRLER_CONFIG u32 config_vendor_le_bb;
extern void log_print(int level, const char *tag, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifndef UNITTEST
_Static_assert(offsetof(struct le_ext_scan_link, period_sync) == 48, "SDK scan link period_sync");
_Static_assert(sizeof(struct ble_ext_period_adv) == 12, "SDK periodic advertising parameter size");
_Static_assert(sizeof(struct le_ext_periodic_link) == 20, "SDK periodic link size");
_Static_assert(offsetof(struct le_ext_periodic_link, hw) == 8, "SDK periodic link hw");
_Static_assert(offsetof(struct le_ext_periodic_link, adv_handle) == 16, "SDK periodic link adv_handle");
_Static_assert(sizeof(ext_pdu_t) == 8, "SDK ext PDU size");
#endif

static void list_add_tail(struct list_head *entry)
{
    struct list_head *prev = ext_ll_hdl.ext_periodic_adv_links.prev;
    ext_ll_hdl.ext_periodic_adv_links.prev = entry;
    entry->next = &ext_ll_hdl.ext_periodic_adv_links;
    entry->prev = prev;
    prev->next = entry;
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

/* The sync timeout fired: mark the sync as timed out and wake the link
 * layer thread, which reports it through le_periodic_sync_timeout_close. */
static void ll_periodic_sync_timer_handler(void *priv) LL_CODE;
static void ll_periodic_sync_timer_handler(void *priv)
{
    if (log_tag_const_e_LL_PADV)
        log_print(4, NULL, "<Error>: [LL_PADV]TO - ll_periodic_sync_timer_handler\r\n");
    struct le_ext_scan_link *link = priv;
    struct ble_ext_period_sync *sync = link->period_sync;
    if (sync) {
        sync->sync_status = 3;
        ll_thread_resume();
    }
}

/* Stop the timeout timer, stop the radio sync, free the sync parameters and
 * detach them from the scan parameters. */
static void ll_periodic_sync_close(struct le_ext_scan_link *link) LL_CODE;
static void ll_periodic_sync_close(struct le_ext_scan_link *link)
{
    /* ll_periodic_sync_timeout_stop */
    u32 timeout = link->period_sync->sync_timeout;
    if (timeout) {
        sys_timer_remove(timeout);
        link->period_sync->sync_timeout = 0;
    }
    /* ll_periodic_sync_terminate: states 14 and 15 */
    if ((link->state & 0xfe) == 14) {
        if (log_tag_const_i_LL_PADV)
            log_print(2, NULL, "[Info]: [LL_PADV]--func=%s\r\n", "ll_periodic_sync_terminate");
        __ble5_ops->ext_ioctrl(link->hw, 14);
        link->state = 12;
    }
    struct ble_ext_period_sync *sync = link->period_sync;
    if (sync)
        __bt_free(&sync->sync_param.Filter_Policy);
    link->period_sync = NULL;
    ext_le_param.ext_scan->period_sync = NULL;
}

/* The link layer thread saw the sync timeout flag: report a lost sync (when
 * established) or a failed establishment, then close. */
void le_periodic_sync_timeout_close(struct le_ext_scan_link *link) LL_CODE;
void le_periodic_sync_timeout_close(struct le_ext_scan_link *link)
{
    struct ble_ext_period_sync *sync = link->period_sync;
    if (sync == NULL) return;
    if (sync->sync_status != 3) return;
    if (link->state == 15) {
        le_periodic_advertising_sync_lost_event(link->period_sync->sync_handle);
        if (log_tag_const_i_LL_PADV)
            log_print(2, NULL, "[Info]: [LL_PADV]sync_lost: 0x%x\r\n", link->period_sync->sync_handle);
    } else {
        le_periodic_advertising_sync_established_event(link, NULL, 62);
        if (log_tag_const_i_LL_PADV)
            log_print(2, NULL, "[Info]: [LL_PADV]sync_established timeout\r\n");
    }
    ll_periodic_sync_close(link);
}

/* A periodic advertising packet arrived: the first one establishes the sync
 * (handle from the ADI's top four bits), then the payload is reported in
 * chunks of at most 247 bytes, or handed to the ISO callback. */
void le_periodic_adv_report_event_handle(struct le_ext_scan_link *link, struct ble_ext_rx *rx) LL_CODE;
void le_periodic_adv_report_event_handle(struct le_ext_scan_link *link, struct ble_ext_rx *rx)
{
    u8 offset = 0;
    ext_pdu_t pdu = {0};

    if (link->state == 14) {
        link->state = 15;
        link->period_sync->sync_status = 2;
        link->period_sync->sync_handle = link->ext_adv_report.advertising_adi >> 12;
        le_periodic_advertising_sync_established_event(link, rx, 0);
        __ble5_ops->ext_ioctrl(link->hw, 17, 60, 60);
    }
    ll_get_ext_adv_pdu(rx, &pdu);
    build_ext_aux_adv_report(link, rx->data_status, &offset, &pdu);
    if (config_vendor_le_bb & 1) {
        if (link->rx_padv_iso_callback)
            link->rx_padv_iso_callback(&rx->data[offset], rx->len - offset);
    } else {
        /* le_periodic_adv_report_event_generate */
        u8 upload_length = rx->len - offset;
        u8 *data = upload_length == 0 ? NULL : &rx->data[offset];
        struct __period_adv_report_event report;
        report.Subevent_Code = 15;
        report.Sync_Handle = link->period_sync->sync_handle;
        report.Tx_Power = link->ext_adv_report.tx_power;
        report.RSSI = (u8)rx->rssi;
        report.CTE_Type = link->ext_adv_report.cte_type;
        report.Data_Status = 0;
        report.Data_Length = 0;
        do {
            if (upload_length > 247) {
                report.Data_Length = 247;
                report.Data_Status = 1;
            } else {
                report.Data_Length = upload_length;
                report.Data_Status = link->ext_adv_report.event_type >> 5;
            }
            le_periodic_advertising_report_event(&report, data);
            data += report.Data_Length;
            upload_length -= report.Data_Length;
        } while (upload_length != 0);
    }
    /* ll_periodic_sync_timeout_reset */
    u32 timeout = link->period_sync->sync_timeout;
    if (timeout)
        sys_timer_reset(timeout);
}

/* Ask the radio to synchronise to the advertiser described by sync_info. */
void ll_periodic_sync_open(struct le_ext_scan_link *link, struct __SyncInfo_field *sync_info) LL_CODE;
void ll_periodic_sync_open(struct le_ext_scan_link *link, struct __SyncInfo_field *sync_info)
{
    if (log_tag_const_i_LL_PADV)
        log_print(2, NULL, "[Info]: [LL_PADV]--func=%s\r\n", "ll_periodic_sync_open");
    __ble5_ops->ext_ioctrl(link->hw, 13, sync_info);
    link->state = 14;
}

/* LE Periodic Advertising Create Sync: adopt the stored parameters and arm
 * the sync timeout timer. */
void ll_periodic_advertising_create_sync(void) LL_CODE;
#ifdef CONFIG_CPU_BD19
static void periodic_sync_assert_failed(int asser) LL_CODE;
static void periodic_sync_assert_failed(int asser)
{
    if (asser) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_periodic_advertising.c", 73);
        printf("ASSERT-FAILD: link->period_sync->sync_timeout ");
        cpu_assert_debug();
    } else {
        p33_soft_reset();
    }
}
#endif
void ll_periodic_advertising_create_sync(void)
{
    struct le_ext_scan_link *link = ext_ll_hdl.ext_scan_link;
    if (link == NULL) return;
    link->period_sync = ext_le_param.ext_scan->period_sync;
    /* ll_periodic_sync_timeout_start */
    u16 timeout = btctler_little_endian_read_16(link->period_sync->sync_param.Sync_Timeout, 0);
    u32 timer_id;
#ifdef CONFIG_CPU_BD19
    u16 timeout_ms = (u16)((timeout > 10 ? timeout : 10) * 10u);
    timer_id = sys_timer_register(timeout_ms, ll_periodic_sync_timer_handler);
#else
    timer_id = sys_timer_register((timeout > 10 ? timeout : 10) * 10,
                                 ll_periodic_sync_timer_handler);
#endif
    link->period_sync->sync_timeout = timer_id;
    int log_timeout = log_tag_const_i_LL_PADV;
    if (log_timeout)
        log_print(2, NULL, "[Info]: [LL_PADV]link->period_sync->sync_timeout=0x%x\r\n",
                  timer_id);
#ifdef CONFIG_CPU_BD19
    /* The SDK checks the returned id unless logging ran; logging can call out,
     * so in that case it reloads the field.  Its ASSERT also snapshots the
     * configuration after that callout. */
    u32 checked_id = log_timeout ? (u32)link->period_sync->sync_timeout : timer_id;
    int asser = config_asser;
    if (checked_id == 0) {
        periodic_sync_assert_failed(asser);
    }
#else
    if (!link->period_sync->sync_timeout)
        btctrler_assert_failed("link->period_sync->sync_timeout", 0);
#endif
    sys_timer_set_context(link->period_sync->sync_timeout, link);
}

/* LE Periodic Advertising Create Sync Cancel. */
void ll_periodic_advertising_create_sync_cancel(void) LL_CODE;
void ll_periodic_advertising_create_sync_cancel(void)
{
    if (ext_ll_hdl.ext_scan_link == NULL) return;
    le_periodic_advertising_sync_established_event(ext_ll_hdl.ext_scan_link, NULL, 68);
    ll_periodic_sync_close(ext_ll_hdl.ext_scan_link);
}

/* LE Periodic Advertising Terminate Sync: true when the handle matched and
 * the sync was closed. */
bool ll_periodic_advertising_terminate_sync(u16 sync_handle) LL_CODE;
bool ll_periodic_advertising_terminate_sync(u16 sync_handle)
{
    struct le_ext_scan_link *link = ext_ll_hdl.ext_scan_link;
    if (link == NULL) return false;
    if (link->period_sync == NULL) return false;
    if (link->period_sync->sync_handle != sync_handle) return false;
    ll_periodic_sync_close(link);
    return true;
}

/* Pick the periodic chain's channel index (0..36), PHY and coding. */
static void __set_ll_ext_periodic_adv_param(struct __ext_adv_group *p, struct ble_ext_period_adv *sync_adv) LL_CODE;
static void __set_ll_ext_periodic_adv_param(struct __ext_adv_group *p, struct ble_ext_period_adv *sync_adv)
{
    pseudo_random_genrate(&sync_adv->chain_channel_index, 1);
    uint32_t channel = btctler_get_rand_from_assign_range(sync_adv->chain_channel_index, 0, 36);
    sync_adv->chain_channel_index = channel;
    sync_adv->chain_phy = p->ext_adv_param.Secondary_Advertising_PHY - 1;
    sync_adv->chain_coded_s = ll_set_code_type;
    if (log_tag_const_i_LL_PADV) {
        log_print(2, NULL, "[Info]: [LL_PADV]sync adv channel_index: 0x%x\r\n", channel & 0xff);
        log_print(2, NULL, "[Info]: [LL_PADV]sync adv chain_phy: 0x%x\r\n", sync_adv->chain_phy);
    }
}

/* Refresh the periodic advertising parameters and point them at the set's
 * advertising data. */
void ll_periodic_adv_data_update(struct __ext_adv_group *adv_set, struct ble_ext_period_adv *sync_adv) LL_CODE;
void ll_periodic_adv_data_update(struct __ext_adv_group *adv_set, struct ble_ext_period_adv *sync_adv)
{
    if (log_tag_const_i_LL_PADV)
        log_print(2, NULL, "[Info]: [LL_PADV]--func=%s\r\n", "ll_periodic_adv_data_update");
    __set_ll_ext_periodic_adv_param(adv_set, sync_adv);
    sync_adv->adv_data_len = adv_set->ext_adv_data.cur_len;
    sync_adv->adv_data = adv_set->ext_adv_data.data;
}

/* Open the radio link for a periodic advertising set and start it.  Returns
 * 7 (memory capacity exceeded) when the link or radio cannot be allocated. */
int ll_periodic_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set) LL_CODE;
int ll_periodic_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set)
{
    /* ll_new_periodic_link */
    struct le_ext_periodic_link *link = __new_ext_link(20);
    if (link) {
        link->hw = __ble5_ops->ext_open();
        if (link->hw == NULL) {
            __bt_free(link);
            link = NULL;
        }
    }
    if (link == NULL) {
        if (log_tag_const_e_LL_PADV)
            log_print(4, NULL, "<Error>: [LL_PADV]periodic_link creat error\r\n");
        return 7;
    }
    link->adv_handle = adv_handle;
    list_add_tail(&link->entry);
    /* __set_ll_ext_periodic_adv_state */
    if (log_tag_const_i_LL_PADV)
        log_print(2, NULL, "[Info]: [LL_PADV]--func=%s\r\n", "__set_ll_ext_periodic_adv_state");
    struct ble_ext_period_adv sync_adv;
    memset(&sync_adv, 0, 12);
    __set_ll_ext_periodic_adv_param(adv_set, &sync_adv);
    sync_adv.adv_data_len = adv_set->ext_adv_data.cur_len;
    sync_adv.adv_data = adv_set->ext_adv_data.data;
    __ble5_ops->ext_adv_ctrl(link->hw, 2, &sync_adv);
    return 0;
}

/* Close the extended advertising set, then the periodic link of the same
 * advertising handle. */
void ll_periodic_adv_close(u8 adv_handle) LL_CODE;
void ll_periodic_adv_close(u8 adv_handle)
{
    ll_ext_adv_close(adv_handle);
    /* ll_destroy_periodic_link */
    struct list_head *head = &ext_ll_hdl.ext_periodic_adv_links;
    for (struct le_ext_periodic_link *link = (struct le_ext_periodic_link *)head->next;
         &link->entry != head; link = (struct le_ext_periodic_link *)link->entry.next) {
        if (link->adv_handle == adv_handle) {
            list_del(&link->entry);
            __ll_ext_link_close(link, link->hw);
            break;
        }
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

u8 log_tag_const_e_LL_PADV = 0, log_tag_const_i_LL_PADV = 0;
u32 config_vendor_le_bb = 0;
int config_asser = 1;
u8 ll_set_code_type = 0;
struct ext_link_layer_handle ext_ll_hdl;
struct ext_le_parameter ext_le_param;
const struct ble5_operation *__ble5_ops;

static struct { const char *format; int arg; } logs[16];
static int nlogs, asserts, assert_value, cpu_asserts, soft_resets, resumes, frees, closes, ext_adv_closed;
static int timer_registered, timer_msec, timer_context_set, timer_removed, timer_reset;
static void *timer_ctx;
static void (*timer_cb)(void *);
static int timer_result = 77;
static int new_link_calls, new_link_fail, open_fail, radio;
static int ioctl_calls, ioctl_cmd, ioctl_a, ioctl_b;
static void *ioctl_hw, *ioctl_ptr;
static int adv_ctrl_calls, adv_ctrl_cmd;
static void *adv_ctrl_hw;
static struct ble_ext_period_adv adv_ctrl_copy;
static void *freed_ptr, *closed_link, *closed_hw;
static int lost_calls, established_calls, established_status, report_calls;
static u16 lost_handle;
static struct ble_ext_rx *established_rx;
static struct { u8 code; u16 handle; u8 tx, rssi, cte, status, length; const u8 *data; } reports[8];
static u8 rand_in, rand_range_min, rand_range_max;
static u8 aux_offset = 3;
static int aux_calls, pdu_calls;
static u8 aux_status;
static const void *cb_data;
static size_t cb_len;
static int cb_calls;

void log_print(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    (void)tag;
    CHECK(level == 2 || level == 4);
    va_start(ap, format);
    if (nlogs < 16) { logs[nlogs].format = format; logs[nlogs].arg = strstr(format, "%x") ? va_arg(ap, int) : -1; nlogs++; }
    va_end(ap);
}
static int logged(const char *text, int arg)
{
    for (int i = 0; i < nlogs; i++) if (strstr(logs[i].format, text) && (arg < 0 || logs[i].arg == arg)) return 1;
    return 0;
}
void btctrler_assert_failed(const char *what, int value) { CHECK(strstr(what, "sync_timeout")); assert_value = value; asserts++; }
void cpu_assert_debug(void) { cpu_asserts++; }
void p33_soft_reset(void) { soft_resets++; }
void ll_thread_resume(void) { resumes++; }
sys_timer sys_timer_register(u32 msec, void (*cb)(void *)) { timer_registered++; timer_msec = msec; timer_cb = cb; return timer_result; }
void sys_timer_remove(sys_timer t) { CHECK(t == 77); timer_removed++; }
void sys_timer_reset(sys_timer t) { CHECK(t == 77); timer_reset++; }
void sys_timer_set_context(sys_timer t, void *ctx) { CHECK(t == timer_result); timer_context_set++; timer_ctx = ctx; }
void __bt_free(void *p) { frees++; freed_ptr = p; }
void *__new_ext_link(u32 len)
{
    CHECK(len == 20);
    new_link_calls++;
    if (new_link_fail) return NULL;
    return calloc(1, sizeof(struct le_ext_periodic_link));
}
void __ll_ext_link_close(void *link, void *hw) { closes++; closed_link = link; closed_hw = hw; }
int ll_ext_adv_close(u8 h) { (void)h; ext_adv_closed++; return 0; }
void le_periodic_advertising_sync_lost_event(u16 h) { lost_calls++; lost_handle = h; }
void le_periodic_advertising_sync_established_event(struct le_ext_scan_link *l, struct ble_ext_rx *rx, u8 st)
{
    (void)l; established_calls++; established_rx = rx; established_status = st;
}
void le_periodic_advertising_report_event(struct __period_adv_report_event *r, u8 *d)
{
    if (report_calls < 8) {
        reports[report_calls].code = r->Subevent_Code; reports[report_calls].handle = r->Sync_Handle;
        reports[report_calls].tx = r->Tx_Power; reports[report_calls].rssi = r->RSSI; reports[report_calls].cte = r->CTE_Type;
        reports[report_calls].status = r->Data_Status; reports[report_calls].length = r->Data_Length; reports[report_calls].data = d;
    }
    report_calls++;
}
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return b[pos] | (b[pos + 1] << 8); }
uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max)
{
    rand_in = rand; rand_range_min = min; rand_range_max = max;
    return 0x125;  /* the SDK stores the low byte */
}
void pseudo_random_genrate(uint8_t *dest, unsigned int size) { CHECK(size == 1); *dest = 0xa7; }
void ll_get_ext_adv_pdu(struct ble_ext_rx *rx, ext_pdu_t *pdu) { (void)rx; pdu_calls++; CHECK(pdu->data == NULL && pdu->type == 0 && pdu->ext_flag == 0); }
bool build_ext_aux_adv_report(struct le_ext_scan_link *l, uint8_t st, u8 *offset, ext_pdu_t *pdu)
{
    (void)l; (void)pdu; aux_calls++; aux_status = st; *offset = aux_offset; return true;
}
static void *radio_open(void) { return open_fail ? NULL : &radio; }
static void radio_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    ioctl_calls++; ioctl_hw = hw; ioctl_cmd = cmd;
    if (cmd == 13) ioctl_ptr = va_arg(ap, void *);
    if (cmd == 17) { ioctl_a = va_arg(ap, int); ioctl_b = va_arg(ap, int); }
    va_end(ap);
}
static void radio_adv_ctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    adv_ctrl_calls++; adv_ctrl_hw = hw; adv_ctrl_cmd = cmd;
    adv_ctrl_copy = *va_arg(ap, struct ble_ext_period_adv *);
    va_end(ap);
}
static void iso_cb(const void *const data, size_t len) { cb_calls++; cb_data = data; cb_len = len; }

static struct ble5_operation ops = { .ext_open = radio_open, .ext_ioctrl = radio_ioctrl, .ext_adv_ctrl = radio_adv_ctrl };

static struct __ext_scan scan_params;
static struct ble_ext_period_sync sync_block;
static struct le_ext_scan_link scan_link;

static void reset(void)
{
    nlogs = asserts = cpu_asserts = soft_resets = resumes = frees = closes = ext_adv_closed = 0;
    timer_registered = timer_context_set = timer_removed = timer_reset = 0;
    new_link_calls = new_link_fail = open_fail = ioctl_calls = adv_ctrl_calls = 0;
    lost_calls = established_calls = report_calls = aux_calls = pdu_calls = cb_calls = 0;
    log_tag_const_i_LL_PADV = log_tag_const_e_LL_PADV = 0;
    config_vendor_le_bb = 0;
    config_asser = 1;
    timer_result = 77;
    aux_offset = 3;
    memset(&sync_block, 0, sizeof sync_block);
    memset(&scan_link, 0, sizeof scan_link);
    memset(&scan_params, 0, sizeof scan_params);
    scan_params.period_sync = &sync_block;
    ext_le_param.ext_scan = &scan_params;
    ext_ll_hdl.ext_scan_link = &scan_link;
    ext_ll_hdl.ext_periodic_adv_links.next = ext_ll_hdl.ext_periodic_adv_links.prev = &ext_ll_hdl.ext_periodic_adv_links;
    __ble5_ops = &ops;
}

int main(void)
{
    /* Create sync: the timer is max(Sync_Timeout, 10) * 10 ms, context is the link. */
    reset();
    sync_block.sync_param.Sync_Timeout[0] = 5;
    ll_periodic_advertising_create_sync();
    CHECK(scan_link.period_sync == &sync_block);
    CHECK(timer_registered == 1 && timer_msec == 100);
    CHECK(timer_cb == ll_periodic_sync_timer_handler);
    CHECK(sync_block.sync_timeout == 77 && timer_context_set == 1 && timer_ctx == &scan_link && asserts == 0);
    sync_block.sync_param.Sync_Timeout[0] = 0x2c; sync_block.sync_param.Sync_Timeout[1] = 0x01;  /* 300 */
    ll_periodic_advertising_create_sync();
    CHECK(timer_msec == 3000);
    sync_block.sync_param.Sync_Timeout[0] = 10; sync_block.sync_param.Sync_Timeout[1] = 0;
    ll_periodic_advertising_create_sync();
    CHECK(timer_msec == 100);
    sync_block.sync_param.Sync_Timeout[0] = 11;
    ll_periodic_advertising_create_sync();
    CHECK(timer_msec == 110);
#ifdef CONFIG_CPU_BD19
    /* The SDK multiplies in i16 before zero extending the timer period. */
    sync_block.sync_param.Sync_Timeout[0] = 0x9a; sync_block.sync_param.Sync_Timeout[1] = 0x19; /* 6554 */
    ll_periodic_advertising_create_sync();
    CHECK(timer_msec == 4);
#endif
    /* Logging reads the timeout fresh. */
    log_tag_const_i_LL_PADV = 1;
    ll_periodic_advertising_create_sync();
    CHECK(logged("sync_timeout=0x%x", 77));
    /* A zero timer handle asserts and still sets the context. */
    reset();
    timer_result = 0;
    ll_periodic_advertising_create_sync();
#ifdef CONFIG_CPU_BD19
    CHECK(asserts == 0 && cpu_asserts == 1 && soft_resets == 0 && timer_context_set == 1);
    reset();
    timer_result = 0;
    config_asser = 0;
    ll_periodic_advertising_create_sync();
    CHECK(asserts == 0 && cpu_asserts == 0 && soft_resets == 1 && timer_context_set == 1);
#else
    CHECK(asserts == 1 && assert_value == 0 && timer_context_set == 1);
#endif
    /* No scan link: nothing happens. */
    reset();
    ext_ll_hdl.ext_scan_link = NULL;
    ll_periodic_advertising_create_sync();
    CHECK(timer_registered == 0 && ioctl_calls == 0);

    /* Timer handler. */
    reset();
    log_tag_const_e_LL_PADV = 1;
    scan_link.period_sync = &sync_block;
    ll_periodic_sync_timer_handler(&scan_link);
    CHECK(sync_block.sync_status == 3 && resumes == 1 && logged("TO - ll_periodic_sync_timer_handler", -1));
    scan_link.period_sync = NULL;
    ll_periodic_sync_timer_handler(&scan_link);
    CHECK(resumes == 1);

    /* Timeout close. */
    reset();
    scan_link.period_sync = NULL;
    le_periodic_sync_timeout_close(&scan_link);
    CHECK(lost_calls == 0 && established_calls == 0);
    scan_link.period_sync = &sync_block;
    sync_block.sync_status = 2;
    le_periodic_sync_timeout_close(&scan_link);
    CHECK(lost_calls == 0 && established_calls == 0 && scan_link.period_sync == &sync_block);
    sync_block.sync_status = 3; sync_block.sync_handle = 0x0abc; sync_block.sync_timeout = 77;
    scan_link.state = 15; scan_link.hw = &radio;
    le_periodic_sync_timeout_close(&scan_link);
    CHECK(lost_calls == 1 && lost_handle == 0x0abc && established_calls == 0);
    CHECK(timer_removed == 1 && ioctl_calls == 1 && ioctl_cmd == 14 && ioctl_hw == &radio && scan_link.state == 12);
    CHECK(frees == 1 && freed_ptr == &sync_block && scan_link.period_sync == NULL && scan_params.period_sync == NULL);
    reset();
    scan_link.period_sync = &sync_block;
    sync_block.sync_status = 3; sync_block.sync_timeout = 0;
    scan_link.state = 14;
    le_periodic_sync_timeout_close(&scan_link);
    CHECK(lost_calls == 0 && established_calls == 1 && established_status == 62 && established_rx == NULL);
    CHECK(timer_removed == 0 && ioctl_cmd == 14 && scan_link.state == 12 && frees == 1);
    reset();
    scan_link.period_sync = &sync_block;
    sync_block.sync_status = 3;
    scan_link.state = 13;
    le_periodic_sync_timeout_close(&scan_link);
    CHECK(established_calls == 1 && ioctl_calls == 0 && scan_link.state == 13 && frees == 1);

    /* Cancel and terminate. */
    reset();
    scan_link.period_sync = &sync_block; sync_block.sync_handle = 5; scan_link.state = 14; scan_link.hw = &radio;
    ll_periodic_advertising_create_sync_cancel();
    CHECK(established_calls == 1 && established_status == 68 && scan_link.period_sync == NULL && scan_link.state == 12);
    reset();
    ext_ll_hdl.ext_scan_link = NULL;
    ll_periodic_advertising_create_sync_cancel();
    CHECK(established_calls == 0);
    CHECK(ll_periodic_advertising_terminate_sync(5) == false);
    ext_ll_hdl.ext_scan_link = &scan_link;
    CHECK(ll_periodic_advertising_terminate_sync(5) == false);
    scan_link.period_sync = &sync_block; sync_block.sync_handle = 5; scan_link.state = 15;
    CHECK(ll_periodic_advertising_terminate_sync(6) == false && frees == 0);
    CHECK(ll_periodic_advertising_terminate_sync(5) == true && frees == 1 && scan_link.state == 12 && ioctl_cmd == 14);

    /* Sync open. */
    reset();
    log_tag_const_i_LL_PADV = 1;
    scan_link.hw = &radio;
    struct __SyncInfo_field info;
    ll_periodic_sync_open(&scan_link, &info);
    CHECK(ioctl_cmd == 13 && ioctl_hw == &radio && ioctl_ptr == &info && scan_link.state == 14);
    CHECK(logged("--func=%s", -1));

    /* Report handling: establish on the first packet. */
    {
        static u8 buf[sizeof(struct ble_ext_rx) + 512];
        struct ble_ext_rx *rx = (struct ble_ext_rx *)buf;
        for (int i = 0; i < 512; i++) rx->data[i] = (u8)i;
        reset();
        scan_link.state = 14; scan_link.hw = &radio; scan_link.period_sync = &sync_block;
        scan_link.ext_adv_report.advertising_adi = 0x5123;
        scan_link.ext_adv_report.tx_power = 9; scan_link.ext_adv_report.cte_type = 2;
        scan_link.ext_adv_report.event_type = 0x40;  /* >> 5 == 2 */
        sync_block.sync_timeout = 77; sync_block.list_cnt = 5;
        rx->data_status = 0x21; rx->rssi = -40; rx->len = 20;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(scan_link.state == 15 && sync_block.sync_status == 2 && sync_block.list_cnt == 5);
        CHECK(sync_block.sync_handle == 5);
        CHECK(established_calls == 1 && established_rx == rx && established_status == 0);
        CHECK(ioctl_cmd == 17 && ioctl_a == 60 && ioctl_b == 60);
        CHECK(pdu_calls == 1 && aux_calls == 1 && aux_status == 0x21);
        CHECK(report_calls == 1 && reports[0].code == 15 && reports[0].handle == 5 && reports[0].tx == 9);
        CHECK(reports[0].rssi == 0xd8 && reports[0].cte == 2 && reports[0].status == 2 && reports[0].length == 17);
        CHECK(reports[0].data == &rx->data[3]);
        CHECK(timer_reset == 1);

        /* Already established: no second establishment; empty payload reports a NULL pointer. */
        reset();
        scan_link.state = 15; scan_link.period_sync = &sync_block; sync_block.sync_handle = 7; sync_block.sync_timeout = 0;
        aux_offset = 20; rx->len = 20;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(established_calls == 0 && ioctl_calls == 0 && timer_reset == 0);
        CHECK(report_calls == 1 && reports[0].length == 0 && reports[0].data == NULL && reports[0].handle == 7);

        /* Exactly 247 bytes fit one report. */
        reset();
        scan_link.state = 15; scan_link.period_sync = &sync_block; scan_link.ext_adv_report.event_type = 0x20;
        aux_offset = 1; rx->len = 248;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(report_calls == 1 && reports[0].length == 247 && reports[0].status == 1 && reports[0].data == &rx->data[1]);
        /* 248 bytes: 247 (more data) then 1 with the event type's status. */
        reset();
        scan_link.state = 15; scan_link.period_sync = &sync_block; scan_link.ext_adv_report.event_type = 0x40;
        aux_offset = 1; rx->len = 249;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(report_calls == 2);
        CHECK(reports[0].length == 247 && reports[0].status == 1 && reports[0].data == &rx->data[1]);
        CHECK(reports[1].length == 1 && reports[1].status == 2 && reports[1].data == &rx->data[248]);
        /* 300 bytes: 247 + 53. */
        reset();
        scan_link.state = 15; scan_link.period_sync = &sync_block; scan_link.ext_adv_report.event_type = 0;
        aux_offset = 0; rx->len = 300 > 255 ? 255 : 300;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(report_calls == 2 && reports[0].length == 247 && reports[1].length == 8 && reports[1].status == 0);

        /* Vendor ISO path: the callback gets the payload, no report events. */
        reset();
        config_vendor_le_bb = 1;
        scan_link.state = 15; scan_link.period_sync = &sync_block; sync_block.sync_timeout = 77;
        scan_link.rx_padv_iso_callback = iso_cb;
        aux_offset = 4; rx->len = 30;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(report_calls == 0 && cb_calls == 1 && cb_data == &rx->data[4] && cb_len == 26 && timer_reset == 1);
        scan_link.rx_padv_iso_callback = NULL;
        le_periodic_adv_report_event_handle(&scan_link, rx);
        CHECK(report_calls == 0 && cb_calls == 1 && timer_reset == 2);
    }

    /* Data update. */
    {
        struct __ext_adv_group set;
        struct ble_ext_period_adv adv;
        memset(&set, 0, sizeof set);
        memset(&adv, 0xee, sizeof adv);
        reset();
        set.ext_adv_param.Secondary_Advertising_PHY = 2;
        set.ext_adv_data.cur_len = 31;
        ll_set_code_type = 1;
        log_tag_const_i_LL_PADV = 1;
        ll_periodic_adv_data_update(&set, &adv);
        CHECK(rand_in == 0xa7 && rand_range_min == 0 && rand_range_max == 36);
        CHECK(adv.chain_channel_index == 0x25 && adv.chain_phy == 1 && adv.chain_coded_s == 1);
        CHECK(adv.adv_data_len == 31 && adv.adv_data == set.ext_adv_data.data);
        CHECK(logged("channel_index", 0x25) && logged("chain_phy", 1) && logged("--func=%s", -1));

        /* Open: failure to get memory, failure to open the radio, success. */
        reset();
        log_tag_const_e_LL_PADV = 1;
        new_link_fail = 1;
        CHECK(ll_periodic_adv_open(3, &set) == 7 && frees == 0 && adv_ctrl_calls == 0 && logged("periodic_link creat error", -1));
        reset();
        open_fail = 1;
        CHECK(ll_periodic_adv_open(3, &set) == 7 && frees == 1 && adv_ctrl_calls == 0);
        CHECK(ext_ll_hdl.ext_periodic_adv_links.next == &ext_ll_hdl.ext_periodic_adv_links);
        reset();
        set.ext_adv_param.Secondary_Advertising_PHY = 3;
        set.ext_adv_data.cur_len = 10;
        CHECK(ll_periodic_adv_open(4, &set) == 0);
        struct le_ext_periodic_link *l1 = (struct le_ext_periodic_link *)ext_ll_hdl.ext_periodic_adv_links.next;
        CHECK(l1 != (void *)&ext_ll_hdl.ext_periodic_adv_links && l1->adv_handle == 4 && l1->hw == &radio);
        CHECK(l1->entry.next == &ext_ll_hdl.ext_periodic_adv_links && ext_ll_hdl.ext_periodic_adv_links.prev == &l1->entry);
        CHECK(adv_ctrl_calls == 1 && adv_ctrl_cmd == 2 && adv_ctrl_hw == &radio);
        CHECK(adv_ctrl_copy.adv_data_len == 10 && adv_ctrl_copy.adv_data == set.ext_adv_data.data && adv_ctrl_copy.chain_phy == 2);
        CHECK(adv_ctrl_copy.chain_channel_index == 0x25);
        CHECK(ll_periodic_adv_open(9, &set) == 0);
        struct le_ext_periodic_link *l2 = (struct le_ext_periodic_link *)l1->entry.next;
        CHECK(l2->adv_handle == 9 && ext_ll_hdl.ext_periodic_adv_links.prev == &l2->entry && l2->entry.prev == &l1->entry);

        /* Close: unknown handle leaves the list; known handle unlinks and closes. */
        ll_periodic_adv_close(1);
        CHECK(ext_adv_closed == 1 && closes == 0 && l1->entry.next == &l2->entry);
        ll_periodic_adv_close(4);
        CHECK(ext_adv_closed == 2 && closes == 1 && closed_link == l1 && closed_hw == &radio);
        CHECK(ext_ll_hdl.ext_periodic_adv_links.next == &l2->entry && l2->entry.prev == &ext_ll_hdl.ext_periodic_adv_links);
        CHECK(l1->entry.next == &l1->entry && l1->entry.prev == &l1->entry);
        ll_periodic_adv_close(9);
        CHECK(closes == 2 && ext_ll_hdl.ext_periodic_adv_links.next == &ext_ll_hdl.ext_periodic_adv_links);
    }
    return TEST_DONE("ll_periodic_advertising");
}
#endif
