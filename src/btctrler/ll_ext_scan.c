/* BLE 5 extended scanning: open the primary (legacy) and secondary (extended)
 * radio links for LE Set Extended Scan Enable, run the scan duration and
 * period timers, and turn the received advertising PDUs into HCI Extended
 * Advertising Report events (and periodic sync attempts).  SDK quirks kept:
 * the ext_pdu_t that ll_get_ext_adv_pdu fills is left partly uninitialised
 * (legacy PDUs and PDUs without an extended header never set ext_flag, yet
 * the report code reads it); the legacy report path never sets the report's
 * address type; a failed primary open leaves the half-built link
 * registered. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_EXT_SCAN, log_tag_const_i_LL_EXT_SCAN;
extern void log_print(int level, const char *tag, const char *format, ...);
extern struct ext_le_parameter ext_le_param;

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifndef UNITTEST
_Static_assert(sizeof(struct le_ext_scan_link) == 64, "SDK extended scan link size");
_Static_assert(offsetof(struct ble_ext_scan, ext_scan) == 8, "SDK ext_scan pointer");
_Static_assert(offsetof(struct ble_ext_scan, coded_s) == 12, "SDK coded_s");
_Static_assert(sizeof(ext_pdu_t) == 8, "SDK ext_pdu_t size");
_Static_assert(sizeof(struct __SyncInfo_field) == 18, "SDK sync info size");
_Static_assert(sizeof(struct __periodic_adv_list) == 8, "SDK periodic list entry");
_Static_assert(offsetof(struct __ext_scan, ext_scan_enable) == 4, "SDK ext_scan enable");
#endif

static void ll_ext_scan_rx_probe_handler(void *priv, struct ble_ext_rx *rx) LL_CODE;
static void ll_ext_scan_rx_probe_handler(void *priv, struct ble_ext_rx *rx)
{
    (void)priv;
    if ((config_btctler_le_roles & 1) == 0) return;
    if (rx->len != 0) ll_thread_resume();
}

const struct ble_ext_handler ll_ext_scan_handler __attribute__((section(".ble_ll_const"), aligned(4))) = {
    ll_ext_scan_rx_probe_handler,
    NULL,
};

/* Parse the PDU's extended header fields.  Nothing is written when the
 * receive has the data-status bit 2 set.  Legacy PDUs (type up to 6) only
 * get the type and the data pointer. */
void ll_get_ext_adv_pdu(struct ble_ext_rx *rx, ext_pdu_t *pdu) LL_CODE;
void ll_get_ext_adv_pdu(struct ble_ext_rx *rx, ext_pdu_t *pdu)
{
    if (rx->data_status & 4) return;
    pdu->type = rx->type;
    if (rx->type > 6) {
        pdu->ext_hdr_len = rx->data[0] & 63;
        pdu->advmode = rx->data[0] >> 6;
        if (pdu->ext_hdr_len != 0) pdu->ext_flag = rx->data[1];
    }
    pdu->data = rx->data;
}

/* Ask the radio to extract one extended header member into member_buf. */
static void ll_ext_header_resolve(const ext_pdu_t *pdu, u8 member, u8 *member_buf) LL_CODE;
static void ll_ext_header_resolve(const ext_pdu_t *pdu, u8 member, u8 *member_buf)
{
    __ble5_ops->ext_ioctrl(NULL, 12, (int)pdu->ext_flag, pdu->data + 2, (int)member, member_buf);
}

/* Fold an auxiliary (extended) PDU into the link's pending report.  Returns
 * 0 when the direct address does not match ours (drop), else 1 with *offset
 * set to the number of header bytes in front of the payload. */
bool build_ext_aux_adv_report(struct le_ext_scan_link *link, uint8_t data_status, u8 *offset, ext_pdu_t *pdu) LL_CODE;
bool build_ext_aux_adv_report(struct le_ext_scan_link *link, uint8_t data_status, u8 *offset, ext_pdu_t *pdu)
{
    struct le_ext_adv_report *report = &link->ext_adv_report;

    if (data_status & 4) {
        report->event_type = (report->event_type & 0x9f) | 64;
        *offset = 1;
        return 1;
    }
    u8 length = pdu->ext_hdr_len + 1;
    if (data_status & 8) {
        u8 mode = pdu->advmode;
        if ((u8)(mode - 1) < 2) report->event_type = mode;
    }
    u8 event_type = report->event_type;
    if (data_status & 16) {
        event_type |= 8;
        report->event_type = event_type;
    }
    report->event_type = (((event_type & 0x9f) | ((data_status << 5) & 32)) ^ 32);
    if (pdu->ext_hdr_len != 0) {
        ll_ext_header_resolve(pdu, 1, report->address);
        ll_ext_header_resolve(pdu, 64, &report->tx_power);
        ll_ext_header_resolve(pdu, 8, (u8 *)&report->advertising_adi);
        if (pdu->ext_flag & 2) {
            ll_ext_header_resolve(pdu, 2, report->direct_address);
            if (memcmp(report->direct_address, link->extended_scan.addr.addr, 6) != 0) return 0;
            report->event_type |= 4;
        }
    }
    *offset = length;
    return 1;
}

static int ll_ext_scan_timeout_start(int timeout) LL_CODE;
void ll_ext_scan_close(void) LL_CODE;
void destroy_le_ext_scan(void) LL_CODE;
int ll_ext_scan_open(void) LL_CODE;

/* The scan duration ran out: start the next period, or finish the scan when
 * the period is zero.  Duration and period are read before the close. */
void ll_ext_scan_duration_end(void) LL_CODE;
void ll_ext_scan_duration_end(void)
{
    struct __ext_scan *scan = ext_ll_hdl.ext_scan_link->extended_scan.ext_scan;
    u32 duration = scan->ext_scan_enable.Duration;
    u32 period = scan->ext_scan_enable.Period;

    if (period != 0) {
        ll_ext_scan_close();
        if (log_tag_const_i_LL_EXT_SCAN)
            log_print(2, NULL, "[Info]: [LL_EXT_SCAN]start new Scan Period\r\n");
        ll_ext_scan_timeout_start(period * 1280 + duration * -10);
    } else {
        destroy_le_ext_scan();
    }
}

static void ll_ext_scan_timeout_stop(void) LL_CODE;
static void ll_ext_scan_timeout_stop(void)
{
    if (ext_ll_hdl.ext_scan_timer != 0) {
        sys_timer_remove(ext_ll_hdl.ext_scan_timer);
        ext_ll_hdl.ext_scan_timer = 0;
    }
}

void ll_ext_scan_close(void)
{
    if (ext_ll_hdl.ext_scan_link == NULL) return;
    ll_ext_scan_timeout_stop();
    local_irq_disable();
    __ble5_ops->legacy_close(ext_ll_hdl.ext_scan_link->primary_legacy_hw);
    local_irq_enable();
    __ll_ext_link_close(ext_ll_hdl.ext_scan_link, ext_ll_hdl.ext_scan_link->hw);
    ext_ll_hdl.ext_scan_link = NULL;
}

static void ll_ext_scan_timer_handler(void *timer) LL_CODE;
/* Returns 7 (hardware failure) after closing the scan when no timer is left. */
static int ll_ext_scan_timeout_start(int timeout)
{
    if (log_tag_const_i_LL_EXT_SCAN)
        log_print(2, NULL, "[Info]: [LL_EXT_SCAN]ext scan timeout : %dms\r\n", timeout);
    sys_timer timer = sys_timer_register(timeout, ll_ext_scan_timer_handler);
    ext_ll_hdl.ext_scan_timer = timer;
    if (timer == 0) {
        if (log_tag_const_e_LL_EXT_SCAN)
            log_print(4, NULL, "<Error>: [LL_EXT_SCAN]create ext_scan timer fail\r\n");
        ll_ext_scan_close();
        return 7;
    }
    return 0;
}

/* Terminate a periodic sync that still hangs off the parameters, free them
 * and close the link. */
void destroy_le_ext_scan(void)
{
    struct __ext_scan *scan = ext_le_param.ext_scan;
    if (scan != NULL) {
        if (scan->period_sync != NULL) {
            ll_periodic_advertising_terminate_sync(scan->period_sync->sync_handle);
            scan = ext_le_param.ext_scan;
        }
        __bt_free(scan);
        ext_le_param.ext_scan = NULL;
    }
    ll_ext_scan_close();
}

static void ll_ext_scan_rx_post(void *priv, struct ble_ext_rx *rx) LL_CODE;

/* Link-layer thread: finish the duration, or drain the receive queue. */
void ll_ext_scan_rx_post_process(int *empty) LL_CODE;
void ll_ext_scan_rx_post_process(int *empty)
{
    struct le_ext_scan_link *link = ext_ll_hdl.ext_scan_link;
    if ((config_btctler_le_roles & 1) != 0 && link != NULL) {
        if (link->state == 13) {
            ll_ext_scan_duration_end();
        } else {
            le_periodic_sync_timeout_close(link);
            __ble5_ops->ext_upload_data(ext_ll_hdl.ext_scan_link->hw, ll_ext_scan_rx_post);
            __ble5_ops->ext_ioctrl(ext_ll_hdl.ext_scan_link->hw, 11, empty);
        }
    }
}

static void le_ext_adv_report_event_handle(struct le_ext_scan_link *link, struct ble_ext_rx *rx) LL_CODE;
static void ll_ext_scan_rx_post(void *priv, struct ble_ext_rx *rx)
{
    struct le_ext_scan_link *link = priv;
    switch (link->state) {
    case 12:
        le_ext_adv_report_event_handle(link, rx);
        break;
    case 14:
    case 15:
        le_periodic_adv_report_event_handle(link, rx);
        break;
    }
}

/* Open the secondary and primary radio links and start scanning.  The
 * duration timer (units of 10 ms) is started when the duration is not 0.
 * The link stays registered after a failed primary open. */
int ll_ext_scan_open(void)
{
    /* __new_ext_link plus the radio open */
    struct le_ext_scan_link *link = __new_ext_link(64);
    if (link != NULL) {
        link->hw = __ble5_ops->ext_open();
        if (link->hw == NULL) {
            __bt_free(link);
            link = NULL;
        }
    }
    if (link == NULL) {
        if (log_tag_const_e_LL_EXT_SCAN)
            log_print(4, NULL, "<Error>: [LL_EXT_SCAN]ext scan secondary create error\r\n");
        return 7;
    }
    ext_ll_hdl.ext_scan_link = link;
    __ble5_ops->ext_handler_register(link->hw, link, &ll_ext_scan_handler);
    link->extended_scan.ext_scan = ext_le_param.ext_scan;

    /* primary (legacy) link */
    link->primary_legacy_hw = __ble_ops->open();
    if (link->primary_legacy_hw == NULL) {
        if (log_tag_const_e_LL_EXT_SCAN)
            log_print(4, NULL, "<Error>: [LL_EXT_SCAN]ext scan primary create error\r\n");
        return 7;
    }
    __ble5_ops->ext_adv_ctrl(link->hw, 6, link->primary_legacy_hw);
    __ble5_ops->ext_adv_ctrl(link->primary_legacy_hw, 6, link->hw);

    /* ll_ext_scan_create_duration_timer */
    u16 duration = link->extended_scan.ext_scan->ext_scan_enable.Duration;
    if (duration != 0) {
        if (log_tag_const_i_LL_EXT_SCAN)
            log_print(2, NULL, "[Info]: [LL_EXT_SCAN]start new Scan Duration\r\n");
        int status = ll_ext_scan_timeout_start(duration * 10);
        if (status != 0) return status;
    }

    /* __set_ll_ext_scan_state */
    if (log_tag_const_i_LL_EXT_SCAN)
        log_print(2, NULL, "[Info]: [LL_EXT_SCAN]--func=%s\r\n", "__set_ll_ext_scan_state");
    __set_ll_local_addr(link->primary_legacy_hw, &link->extended_scan.addr,
                        link->extended_scan.ext_scan->ext_scan_param.Own_Address_Type);
    __set_ll_ext_local_addr(link->hw, &link->extended_scan.addr,
                            link->extended_scan.ext_scan->ext_scan_param.Own_Address_Type);
    link->extended_scan.coded_s = 1;
    link->state = 12;
    __ble5_ops->ext_scanning(link->hw, link->primary_legacy_hw, &link->extended_scan);
    return 0;
}

/* The scan duration timer fired (units of ms through sys_timer): move the
 * link to the duration-end state, or reopen the scan when it is gone. */
static void ll_ext_scan_timer_handler(void *timer)
{
    (void)timer;
    if (log_tag_const_e_LL_EXT_SCAN)
        log_print(4, NULL, "<Error>: [LL_EXT_SCAN]TO - ll_ext_scan_timer_handler\r\n");
    ll_ext_scan_timeout_stop();
    le_scan_timeout_event();
    struct le_ext_scan_link *link = ext_ll_hdl.ext_scan_link;
    if (link != NULL) {
        link->state = 13;
        ll_thread_resume();
    } else {
        ll_ext_scan_open();
    }
}

/* The HCI LE Extended Advertising Report event: one event per 229 bytes of
 * data, the first with the event type as built, the following ones with the
 * link's event type again (so "more data" bits restart); an over-long piece
 * is reported truncated-at-229 with the data-status bits set to 1. */
static void le_ext_adv_report_event_generate(struct le_ext_scan_link *link, struct ble_ext_rx *rx, u8 offset)
{
    struct __ext_adv_report_event report;
    u8 upload_length = rx->len - offset;
    u8 *data = upload_length == 0 ? NULL : &rx->data[offset];

    report.Subevent_Code = 13;
    report.Num_Reports = 1;
    report.Event_Type.event_type = link->ext_adv_report.event_type;
    report.Address_Type = link->ext_adv_report.addr_type;
    memset(report.Address, 0, 6);
    report.Primary_PHY = rx->primary_phy;
    report.Secondary_PHY = rx->secondary_phy;
    report.Advertising_SID = (u8)(link->ext_adv_report.advertising_adi >> 12);
    report.Tx_Power = link->ext_adv_report.tx_power;
    report.RSSI = (u8)rx->rssi;
    report.Periodic_Advertising_Interval = link->ext_adv_report.periodic_adv_interval;
    report.Direct_Address_Type = rx->rxadd;
    memcpy(report.Address, link->ext_adv_report.address, 6);
    /* The SDK clears Direct_Address[6], the byte after the array, which is
     * Data_Length (written again below) */
    report.Data_Length = 0;
    memcpy(report.Direct_Address, link->ext_adv_report.direct_address, 6);
    do {
        if (upload_length > 229) {
            report.Data_Length = 229;
            report.Event_Type.event_type = (report.Event_Type.event_type & 0xff9f) | 32;
        } else {
            report.Data_Length = upload_length;
            report.Event_Type.event_type = link->ext_adv_report.event_type;
        }
        le_extended_advertising_report_event(&report, data);
        u8 sent = report.Data_Length;
        data += sent;
        upload_length -= sent;
    } while (upload_length != 0);
}

/* Look the advertiser up in the periodic sync request (filter policy 0:
 * the one device; 1: the list) and open the sync, else tell the radio to
 * stop waiting for sync info (ext_ioctrl 14). */
static void le_periodic_sync_check(struct le_ext_scan_link *link, struct __SyncInfo_field *sync_info)
{
    struct ble_ext_period_sync *sync = link->period_sync;
    struct __periodic_adv_list entry;

    if (sync == NULL) return;
    entry.Advertiser_Address_Type = link->ext_adv_report.addr_type;
    entry.Advertising_SID = (u8)(link->ext_adv_report.advertising_adi >> 12);
    memcpy(entry.Advertiser_Address, link->ext_adv_report.address, 6);
    switch (sync->sync_param.Filter_Policy) {
    case 0:
        if (entry.Advertiser_Address_Type == sync->sync_param.Advertising_Address_Type &&
            entry.Advertising_SID == sync->sync_param.Advertising_SID &&
            memcmp(entry.Advertiser_Address, sync->sync_param.Advertiser_Address, 6) == 0) {
            ll_periodic_sync_open(link, sync_info);
            return;
        }
        break;
    case 1:
        for (int i = 0; i < 2; i++) {
            if (memcmp(&entry, &sync->list[i], 8) == 0) {
                ll_periodic_sync_open(link, sync_info);
                return;
            }
        }
        break;
    }
    __ble5_ops->ext_ioctrl(link->hw, 14);
}

/* Turn one received extended-advertising-channel PDU into reports.  With
 * data-status bit 5 the PDU is a legacy advertisement (the cases follow the
 * legacy PDU type); otherwise an auxiliary PDU is folded into the pending
 * report by build_ext_aux_adv_report. */
static void le_ext_adv_report_event_handle(struct le_ext_scan_link *link, struct ble_ext_rx *rx)
{
    u8 offset = 0;
    ext_pdu_t pdu;
    struct __SyncInfo_field sync_info;
    struct le_ext_adv_report *report = &link->ext_adv_report;

    ll_get_ext_adv_pdu(rx, &pdu);
    if (rx->data_status & 32) {
        offset = 6;
        memset(report->direct_address, 0, 15);
        switch (pdu.type) {
        case 0:
            report->event_type = 19;
            memcpy(report->address, pdu.data, 6);
            break;
        case 1: {
            u8 *target = pdu.data + 6;
            if (memcmp(target, link->extended_scan.addr.addr, 6) != 0) goto periodic;
            report->event_type = 21;
            memcpy(report->address, pdu.data, 6);
            memcpy(report->direct_address, target, 6);
            offset = 12;
            break;
        }
        case 2:
            report->event_type = 16;
            memcpy(report->address, pdu.data, 6);
            break;
        case 6:
            report->event_type = 18;
            memcpy(report->address, pdu.data, 6);
            break;
        case 4: {
            u8 previous = report->event_type;
            if (previous & 8) goto periodic;
            if (memcmp(report->address, pdu.data, 6) != 0) goto periodic;
            report->event_type = (previous & 1) | 26;
            memcpy(report->address, pdu.data, 6);
            break;
        }
        case 7:
            report->event_type = 0;
            if (pdu.ext_hdr_len == 0) {
                offset = 1;
            } else {
                if (pdu.ext_flag & 2) {
                    ll_ext_header_resolve(&pdu, 2, report->direct_address);
                    if (memcmp(report->direct_address, link->extended_scan.addr.addr, 6) != 0) goto periodic;
                }
                ll_ext_header_resolve(&pdu, 1, report->address);
                offset = pdu.ext_hdr_len + 1;
            }
            break;
        }
        report->event_type &= 0x9f;
        le_ext_adv_report_event_generate(link, rx, offset);
    } else {
        bool built = build_ext_aux_adv_report(link, rx->data_status, &offset, &pdu);
        if (rx->data_status & 64) {
            report->addr_type = rx->txadd;
            return;
        }
        /* ll_ext_sync_info_get */
        if (pdu.ext_hdr_len != 0 && (pdu.ext_flag & 32)) {
            ll_ext_header_resolve(&pdu, 32, (u8 *)&sync_info);
            report->periodic_adv_interval = sync_info.interval;
        }
        if (built == 1) le_ext_adv_report_event_generate(link, rx, offset);
    }
periodic:
    if (!(pdu.ext_flag & 32)) return;
    le_periodic_sync_check(link, &sync_info);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

struct ext_le_parameter ext_le_param;
struct ext_link_layer_handle ext_ll_hdl;
const struct ble5_operation *__ble5_ops;
const struct ble_operation *__ble_ops;
u8 log_tag_const_e_LL_EXT_SCAN = 1, log_tag_const_i_LL_EXT_SCAN = 1;
int config_btctler_le_roles = 1;

static char trace[1024];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static int errors, infos, resumes, timeouts, sync_opens, terminates, last_terminate, frees;
static void *freed;
static int new_link_fails, ext_open_null, primary_open_null, timer_fail;
static int timer_msec;
static void (*timer_cb)(void *);
static int removed_timer;
static int next_timer = 77;
static void *ext_hw = (void *)0xe000, *primary_hw = (void *)0xb000;
static void *registered_hw, *registered_priv;
static const struct ble_ext_handler *registered;
static void *ctrl_target[4];
static int nctrl;
static u8 local_type[2];
static struct le_addr *local_addr[2];
static struct le_ext_scan_link *closed_link;
static void *closed_hw;
static void *scan_hw, *scan_primary;
static struct ble_ext_scan *scan_arg;
static struct ble_ext_rx *upload_rx;

/* what the radio hands back for ext_ioctrl(12, ...) */
static struct { int flag; u8 *data; int member; u8 *buf; } resolves[8];
static int nresolves;
static u8 resolve_address[6] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6 };
static u8 resolve_direct[6] = { 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6 };
static u8 resolve_tx = 0xf7;
static u16 resolve_adi = 0x5123;
static u16 resolve_interval = 0x4321;
static int ioctrl_14, ioctrl_11;
static int *ioctrl_empty;

/* HCI reports captured from le_extended_advertising_report_event */
static struct { struct __ext_adv_report_event r; const u8 *data; int len_after; } reports[8];
static int nreports;
static int periodic_reports, periodic_closes;

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
    if (level == 4) errors++; else infos++;
}
void ll_thread_resume(void) { resumes++; }
void __bt_free(void *p) { freed = p; frees++; }
void local_irq_disable(void) { T("irqoff"); }
void local_irq_enable(void) { T("irqon"); }
void le_scan_timeout_event(void) { T("scantimeout"); }
bool ll_periodic_advertising_terminate_sync(u16 handle) { terminates++; last_terminate = handle; return 0; }
void le_periodic_sync_timeout_close(struct le_ext_scan_link *link) { (void)link; periodic_closes++; T("syncclose"); }
void le_periodic_adv_report_event_handle(struct le_ext_scan_link *link, struct ble_ext_rx *rx) { (void)link; (void)rx; periodic_reports++; }
void ll_periodic_sync_open(struct le_ext_scan_link *link, struct __SyncInfo_field *info) { (void)link; (void)info; sync_opens++; }
void le_extended_advertising_report_event(struct __ext_adv_report_event *report, u8 *data)
{
    reports[nreports].r = *report;
    reports[nreports].data = data;
    nreports++;
}
sys_timer sys_timer_register(u32 msec, void (*callback)(void *))
{
    timer_msec = msec; timer_cb = callback; timeouts++;
    return timer_fail ? 0 : next_timer;
}
void sys_timer_remove(sys_timer t) { removed_timer = t; T("timerdel"); }
void *__new_ext_link(u32 len)
{
    CHECK(len == 64);
    if (new_link_fails) return NULL;
    return calloc(1, sizeof(struct le_ext_scan_link));  /* the host struct is larger than the 64-byte SDK one */
}
void __ll_ext_link_close(void *link, void *hw) { closed_link = link; closed_hw = hw; T("linkclose"); }
void __set_ll_local_addr(void *hw, struct le_addr *a, u8 t) { CHECK(hw == primary_hw); local_type[0] = t; local_addr[0] = a; T("localaddr"); }
void __set_ll_ext_local_addr(void *hw, struct le_addr *a, u8 t) { CHECK(hw == ext_hw); local_type[1] = t; local_addr[1] = a; T("extlocaladdr"); }

static void *op_ext_open(void) { T("extopen"); return ext_open_null ? NULL : ext_hw; }
static void op_legacy_close(void *hw) { CHECK(hw == primary_hw); T("legacyclose"); }
static void *op_open(void) { T("open"); return primary_open_null ? NULL : primary_hw; }
static void op_register(void *hw, void *priv, const struct ble_ext_handler *h) { registered_hw = hw; registered_priv = priv; registered = h; }
static void op_adv_ctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    CHECK(cmd == 6);
    ctrl_target[nctrl++] = hw;
    ctrl_target[nctrl++] = va_arg(ap, void *);
    va_end(ap);
}
static void op_ext_scanning(void *hw, void *primary, struct ble_ext_scan *scan) { scan_hw = hw; scan_primary = primary; scan_arg = scan; T("scanning"); }
static void op_upload(void *hw, void (*callback)(void *, struct ble_ext_rx *))
{
    CHECK(hw == ext_hw);
    T("upload");
    callback(ext_ll_hdl.ext_scan_link, upload_rx);
}
static void op_ext_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    if (cmd == 12) {
        int i = nresolves++;
        CHECK(hw == NULL);
        resolves[i].flag = va_arg(ap, int);
        resolves[i].data = va_arg(ap, u8 *);
        resolves[i].member = va_arg(ap, int);
        resolves[i].buf = va_arg(ap, u8 *);
        switch (resolves[i].member) {
        case 1: memcpy(resolves[i].buf, resolve_address, 6); break;
        case 2: memcpy(resolves[i].buf, resolve_direct, 6); break;
        case 64: *resolves[i].buf = resolve_tx; break;
        case 8: memcpy(resolves[i].buf, &resolve_adi, 2); break;
        case 32: memcpy(resolves[i].buf + 2, &resolve_interval, 2); break;
        default: CHECK(0);
        }
    } else if (cmd == 14) {
        CHECK(hw == ext_hw);
        ioctrl_14++;
    } else if (cmd == 11) {
        CHECK(hw == ext_hw);
        ioctrl_11++;
        ioctrl_empty = va_arg(ap, int *);
    } else {
        CHECK(0);
    }
    va_end(ap);
}
static const struct ble5_operation ops5 = {
    .legacy_close = op_legacy_close, .ext_open = op_ext_open, .ext_ioctrl = op_ext_ioctrl,
    .ext_adv_ctrl = op_adv_ctrl, .ext_scanning = op_ext_scanning, .ext_handler_register = op_register,
    .ext_upload_data = op_upload,
};
static const struct ble_operation ops = { .open = (void *(*)(void))op_open };

static void reset(void)
{
    memset(&ext_ll_hdl, 0, sizeof(ext_ll_hdl));
    memset(&ext_le_param, 0, sizeof(ext_le_param));
    trace[0] = 0;
    errors = infos = resumes = timeouts = sync_opens = terminates = frees = 0;
    new_link_fails = ext_open_null = primary_open_null = timer_fail = 0;
    nctrl = nresolves = nreports = 0;
    ioctrl_14 = ioctrl_11 = periodic_reports = periodic_closes = 0;
    registered = NULL; closed_link = NULL; freed = NULL;
    __ble5_ops = &ops5;
    __ble_ops = &ops;
}

static struct __ext_scan *new_scan(u16 duration, u16 period, u8 own)
{
    struct __ext_scan *scan = calloc(1, sizeof(struct __ext_scan) + 16);
    scan->ext_scan_enable.Duration = duration;
    scan->ext_scan_enable.Period = period;
    scan->ext_scan_param.Own_Address_Type = own;
    return scan;
}

static u8 rxbuf[512] __attribute__((aligned(8)));
static struct ble_ext_rx *rx = (struct ble_ext_rx *)rxbuf;
static void make_rx(int type, int status, int len)
{
    memset(rxbuf, 0, sizeof(rxbuf));
    rx->type = type; rx->data_status = status; rx->len = len;
    rx->rxadd = 1; rx->txadd = 0; rx->rssi = -50;
    rx->primary_phy = 1; rx->secondary_phy = 2;
}

static struct le_ext_scan_link *new_link(void)
{
    struct le_ext_scan_link *link = calloc(1, sizeof(*link));
    link->hw = ext_hw;
    link->primary_legacy_hw = primary_hw;
    link->state = 12;
    memcpy(link->extended_scan.addr.addr, "\x01\x02\x03\x04\x05\x06", 6);
    link->ext_adv_report.addr_type = 0x7e;
    return link;
}

int main(void)
{
    ext_pdu_t pdu;

    /* ll_get_ext_adv_pdu */
    reset();
    make_rx(7, 4, 10);
    memset(&pdu, 0xaa, sizeof(pdu));
    ll_get_ext_adv_pdu(rx, &pdu);
    CHECK(pdu.type == 0xaa && pdu.ext_hdr_len == 0xaa && pdu.advmode == 0xaa && pdu.ext_flag == 0xaa);
    make_rx(3, 0, 10);
    memset(&pdu, 0xaa, sizeof(pdu));
    ll_get_ext_adv_pdu(rx, &pdu);
    CHECK(pdu.type == 3 && pdu.data == rx->data);
    CHECK(pdu.ext_hdr_len == 0xaa && pdu.advmode == 0xaa && pdu.ext_flag == 0xaa);
    make_rx(6, 0, 10);
    memset(&pdu, 0xaa, sizeof(pdu));
    ll_get_ext_adv_pdu(rx, &pdu);
    CHECK(pdu.type == 6 && pdu.ext_hdr_len == 0xaa);  /* type 6 is still a legacy PDU */
    make_rx(7, 0, 10);
    rx->data[0] = 0x8a; rx->data[1] = 0x5c;            /* advmode 2, header length 10 */
    memset(&pdu, 0xaa, sizeof(pdu));
    ll_get_ext_adv_pdu(rx, &pdu);
    CHECK(pdu.type == 7 && pdu.ext_hdr_len == 10 && pdu.advmode == 2 && pdu.ext_flag == 0x5c && pdu.data == rx->data);
    rx->data[0] = 0x40;                                 /* advmode 1, no header: ext_flag untouched */
    memset(&pdu, 0xaa, sizeof(pdu));
    ll_get_ext_adv_pdu(rx, &pdu);
    CHECK(pdu.ext_hdr_len == 0 && pdu.advmode == 1 && pdu.ext_flag == 0xaa);

    /* build_ext_aux_adv_report */
    reset();
    struct le_ext_scan_link *link = new_link();
    u8 offset = 0x55;
    struct le_ext_adv_report *rep = &link->ext_adv_report;
    pdu.ext_hdr_len = 0; pdu.advmode = 0; pdu.ext_flag = 0; pdu.data = rx->data;
    rep->event_type = 0xff;
    CHECK(build_ext_aux_adv_report(link, 4, &offset, &pdu) == 1);
    CHECK(rep->event_type == 0xdf && offset == 1 && nresolves == 0);
    rep->event_type = 0;
    CHECK(build_ext_aux_adv_report(link, 0, &offset, &pdu) == 1);
    CHECK(rep->event_type == 0x20 && offset == 1);
    rep->event_type = 0;
    build_ext_aux_adv_report(link, 1, &offset, &pdu);
    CHECK(rep->event_type == 0x00);
    rep->event_type = 0x60;
    build_ext_aux_adv_report(link, 3, &offset, &pdu);
    CHECK(rep->event_type == (((0x60 & 0x9f) | 32) ^ 32));
    pdu.advmode = 2;
    rep->event_type = 0;
    build_ext_aux_adv_report(link, 8, &offset, &pdu);
    CHECK(rep->event_type == 0x22);                     /* advmode 2 replaces the type, then bit 5 is set */
    pdu.advmode = 3;
    rep->event_type = 0x10;
    build_ext_aux_adv_report(link, 8, &offset, &pdu);
    CHECK(rep->event_type == 0x30);                     /* advmode 3 is not taken over */
    pdu.advmode = 1;
    rep->event_type = 0x80;
    build_ext_aux_adv_report(link, 8, &offset, &pdu);
    CHECK(rep->event_type == 0x21);                     /* the old type is replaced, not or-ed */
    rep->event_type = 0;
    build_ext_aux_adv_report(link, 16, &offset, &pdu);
    CHECK(rep->event_type == 0x28);
    /* header with members, no direct address */
    pdu.ext_hdr_len = 3; pdu.ext_flag = 0x49; offset = 0x55;
    rep->event_type = 0;
    CHECK(build_ext_aux_adv_report(link, 0, &offset, &pdu) == 1);
    CHECK(offset == 4 && nresolves == 3);
    CHECK(resolves[0].member == 1 && resolves[0].buf == rep->address);
    CHECK(resolves[1].member == 64 && resolves[1].buf == &rep->tx_power);
    CHECK(resolves[2].member == 8 && resolves[2].buf == (u8 *)&rep->advertising_adi);
    CHECK(resolves[0].flag == 0x49 && resolves[0].data == rx->data + 2);
    CHECK(memcmp(rep->address, resolve_address, 6) == 0 && rep->tx_power == 0xf7 && rep->advertising_adi == 0x5123);
    CHECK(rep->event_type == 0x20);
    /* direct address that is ours */
    nresolves = 0; pdu.ext_flag = 0x4b; offset = 0x55;
    memcpy(resolve_direct, link->extended_scan.addr.addr, 6);
    CHECK(build_ext_aux_adv_report(link, 0, &offset, &pdu) == 1);
    CHECK(nresolves == 4 && resolves[3].member == 2 && resolves[3].buf == rep->direct_address);
    CHECK(offset == 4 && rep->event_type == 0x24);
    /* direct address of somebody else: dropped, offset untouched */
    memcpy(resolve_direct, "\xd1\xd2\xd3\xd4\xd5\xd6", 6);
    offset = 0x55;
    CHECK(build_ext_aux_adv_report(link, 0, &offset, &pdu) == 0);
    CHECK(offset == 0x55);

    /* legacy advertising reports */
    reset();
    link = new_link();
    rep = &link->ext_adv_report;
    ext_ll_hdl.ext_scan_link = link;
    for (int i = 0; i < 40; i++) rx->data[i] = 0;
    make_rx(0, 32, 6 + 10);                              /* ADV_IND with ten payload bytes */
    for (int i = 0; i < 16; i++) rx->data[i] = 0x10 + i;
    memset(rep->direct_address, 0x99, 8);
    rep->tx_power = 0x99; rep->advertising_adi = 0x9999; rep->periodic_adv_interval = 0x9999;
    rep->event_type = 0xee;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1);
    CHECK(rep->event_type == 0x13 && memcmp(rep->address, rx->data, 6) == 0);
    CHECK(rep->tx_power == 0 && rep->advertising_adi == 0 && rep->periodic_adv_interval == 0);
    CHECK(reports[0].r.Subevent_Code == 13 && reports[0].r.Num_Reports == 1);
    CHECK(reports[0].r.Event_Type.event_type == 0x13);
    CHECK(reports[0].r.Address_Type == 0x7e);            /* stale: the legacy path never sets it */
    CHECK(memcmp(reports[0].r.Address, rx->data, 6) == 0);
    CHECK(reports[0].r.Primary_PHY == 1 && reports[0].r.Secondary_PHY == 2);
    CHECK(reports[0].r.Advertising_SID == 0 && reports[0].r.Tx_Power == 0);
    CHECK(reports[0].r.RSSI == (u8)-50 && reports[0].r.Periodic_Advertising_Interval == 0);
    CHECK(reports[0].r.Direct_Address_Type == 1);
    CHECK(reports[0].r.Data_Length == 10 && reports[0].data == rx->data + 6);
    CHECK(reports[0].r.Direct_Address[0] == 0 && reports[0].r.Direct_Address[5] == 0);
    CHECK(nresolves == 0);
    /* other legacy types */
    nreports = 0; make_rx(2, 32, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x10 && reports[0].r.Data_Length == 0 && reports[0].data == NULL);
    nreports = 0; make_rx(6, 32, 8);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x12 && reports[0].r.Data_Length == 2);
    /* unknown type: the previous type, with bits 5 and 6 cleared, is reported */
    nreports = 0; rep->event_type = 0xff; make_rx(3, 32, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x9f && reports[0].r.Event_Type.event_type == 0x9f);
    /* ADV_DIRECT_IND: ours */
    nreports = 0; make_rx(1, 32, 12 + 3);
    for (int i = 0; i < 6; i++) rx->data[i] = 0x20 + i;
    memcpy(rx->data + 6, link->extended_scan.addr.addr, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x15);
    CHECK(memcmp(rep->address, rx->data, 6) == 0 && memcmp(rep->direct_address, rx->data + 6, 6) == 0);
    CHECK(reports[0].r.Data_Length == 3 && reports[0].data == rx->data + 12);
    CHECK(memcmp(reports[0].r.Direct_Address, rx->data + 6, 6) == 0);
    /* ADV_DIRECT_IND for somebody else */
    nreports = 0; rx->data[6] ^= 0xff;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 0);
    /* SCAN_RSP (type 4) */
    make_rx(4, 32, 6 + 2);
    for (int i = 0; i < 8; i++) rx->data[i] = 0x30 + i;
    rep->event_type = 0x01; memcpy(rep->address, rx->data, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x1b && reports[0].r.Data_Length == 2);   /* (1 & 1) | 26 */
    nreports = 0; rep->event_type = 0x08;
    ll_ext_scan_rx_post(link, rx);                       /* bit 3 already set: ignored */
    CHECK(nreports == 0 && rep->event_type == 0x08);
    rep->event_type = 0x00; rep->address[0] ^= 1;
    ll_ext_scan_rx_post(link, rx);                       /* another advertiser: ignored */
    CHECK(nreports == 0 && rep->event_type == 0x00);
    /* legacy extended PDU (type 7): no header */
    make_rx(7, 32, 1 + 4);
    rx->data[0] = 0x00;
    rep->event_type = 0xff;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && rep->event_type == 0x00 && reports[0].r.Data_Length == 4 && reports[0].data == rx->data + 1);
    /* with header: flag 3 (address and direct address); direct is ours */
    nreports = 0; nresolves = 0;
    make_rx(7, 32, 9 + 2);
    rx->data[0] = 8; rx->data[1] = 0x03;
    memcpy(resolve_direct, link->extended_scan.addr.addr, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nresolves == 2 && resolves[0].member == 2 && resolves[1].member == 1);
    CHECK(nreports == 1 && reports[0].r.Data_Length == 2 && reports[0].data == rx->data + 9);
    CHECK(memcmp(rep->address, resolve_address, 6) == 0);
    /* with header, direct address of somebody else */
    nreports = 0; nresolves = 0;
    memcpy(resolve_direct, "\xd1\xd2\xd3\xd4\xd5\xd6", 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nresolves == 1 && nreports == 0);

    /* report chunking at 229 bytes */
    reset();
    link = new_link();
    rep = &link->ext_adv_report;
    ext_ll_hdl.ext_scan_link = link;
    make_rx(0, 32, 6 + 229);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && reports[0].r.Data_Length == 229 && reports[0].r.Event_Type.event_type == 0x13);
    nreports = 0; make_rx(0, 32, 6 + 230);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 2);
    CHECK(reports[0].r.Data_Length == 229 && reports[0].r.Event_Type.event_type == 0x33 && reports[0].data == rx->data + 6);
    CHECK(reports[1].r.Data_Length == 1 && reports[1].r.Event_Type.event_type == 0x13 && reports[1].data == rx->data + 6 + 229);
    nreports = 0; make_rx(0, 32, 6 + 250);
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 2 && reports[0].r.Data_Length == 229 && reports[1].r.Data_Length == 21);

    /* auxiliary (non-legacy) PDUs */
    reset();
    link = new_link();
    rep = &link->ext_adv_report;
    ext_ll_hdl.ext_scan_link = link;
    make_rx(7, 0, 1 + 5 + 4);
    rx->data[0] = 0x45; rx->data[1] = 0x00;              /* advmode 1, header length 5, no members */
    rep->event_type = 0;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 1 && reports[0].r.Event_Type.event_type == 0x20);
    CHECK(reports[0].r.Data_Length == 4 && reports[0].data == rx->data + 6);   /* offset = header length + 1 */
    CHECK(reports[0].r.Address_Type == 0x7e);
    /* data-status bit 6: only the address type is taken, no report */
    nreports = 0;
    make_rx(7, 64, 8); rx->data[0] = 0x45; rx->txadd = 1;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nreports == 0 && rep->addr_type == 1);
    /* sync info: interval picked up and reported */
    nreports = 0; nresolves = 0;
    make_rx(7, 0, 20); rx->data[0] = 0x45; rx->data[1] = 0x20;
    ll_ext_scan_rx_post(link, rx);
    CHECK(nresolves == 4 && resolves[3].member == 32 && rep->periodic_adv_interval == 0x4321);
    CHECK(nreports == 1 && reports[0].r.Periodic_Advertising_Interval == 0x4321);
    CHECK(ioctrl_14 == 0);                               /* flag 32 set, but no sync requested: nothing to do */
    /* periodic sync request, filter policy 0 */
    struct ble_ext_period_sync *sync = calloc(1, sizeof(*sync));
    link->period_sync = sync;
    rep->addr_type = 3;
    nreports = 0; nresolves = 0; ioctrl_14 = 0;
    sync->sync_param.Filter_Policy = 0;
    sync->sync_param.Advertising_Address_Type = 3;
    sync->sync_param.Advertising_SID = 5;               /* adi 0x5123 >> 12 */
    ll_ext_scan_rx_post(link, rx);
    CHECK(memcmp(rep->address, resolve_address, 6) == 0);
    memcpy(sync->sync_param.Advertiser_Address, resolve_address, 6);
    ll_ext_scan_rx_post(link, rx);
    CHECK(sync_opens == 1 && ioctrl_14 == 1);           /* first call: address differed */
    sync->sync_param.Advertising_SID = 6;
    rep->addr_type = 3;
    ll_ext_scan_rx_post(link, rx);
    CHECK(sync_opens == 1 && ioctrl_14 == 2);
    /* filter policy 1: list of two entries */
    sync->sync_param.Filter_Policy = 1;
    ll_ext_scan_rx_post(link, rx);
    CHECK(sync_opens == 1 && ioctrl_14 == 3);
    sync->list[1].Advertiser_Address_Type = 3;
    memcpy(sync->list[1].Advertiser_Address, resolve_address, 6);
    sync->list[1].Advertising_SID = 5;
    rep->addr_type = 3;
    ll_ext_scan_rx_post(link, rx);
    CHECK(sync_opens == 2 && ioctrl_14 == 3);
    /* other filter policy: nothing matches, the radio is told */
    sync->sync_param.Filter_Policy = 2;
    ll_ext_scan_rx_post(link, rx);
    CHECK(sync_opens == 2 && ioctrl_14 == 4);
    /* no sync info flag: the periodic check is skipped */
    nreports = 0; make_rx(7, 0, 8); rx->data[0] = 0x45; rx->data[1] = 0x00;
    ll_ext_scan_rx_post(link, rx);
    CHECK(ioctrl_14 == 4 && nreports == 1);

    /* periodic states go to the periodic handler */
    link->state = 14;
    ll_ext_scan_rx_post(link, rx);
    link->state = 15;
    ll_ext_scan_rx_post(link, rx);
    link->state = 13;
    ll_ext_scan_rx_post(link, rx);
    CHECK(periodic_reports == 2 && nreports == 1);

    /* probe handler */
    reset();
    make_rx(0, 0, 3);
    ll_ext_scan_handler.ext_rx_probe_handler(NULL, rx);
    CHECK(resumes == 1 && ll_ext_scan_handler.ext_event_probe_handler == NULL);
    rx->len = 0;
    ll_ext_scan_handler.ext_rx_probe_handler(NULL, rx);
    CHECK(resumes == 1);
    config_btctler_le_roles = 0; rx->len = 3;
    ll_ext_scan_handler.ext_rx_probe_handler(NULL, rx);
    CHECK(resumes == 1);
    config_btctler_le_roles = 1;

    /* ll_ext_scan_open */
    reset();
    ext_le_param.ext_scan = new_scan(300, 0, 2);
    CHECK(ll_ext_scan_open() == 0);
    link = ext_ll_hdl.ext_scan_link;
    CHECK(link != NULL && link->hw == ext_hw && link->primary_legacy_hw == primary_hw);
    CHECK(registered == &ll_ext_scan_handler && registered_hw == ext_hw && registered_priv == link);
    CHECK(link->extended_scan.ext_scan == ext_le_param.ext_scan);
    CHECK(nctrl == 4 && ctrl_target[0] == ext_hw && ctrl_target[1] == primary_hw);
    CHECK(ctrl_target[2] == primary_hw && ctrl_target[3] == ext_hw);
    CHECK(timeouts == 1 && timer_msec == 3000 && timer_cb != NULL && ext_ll_hdl.ext_scan_timer == 77);
    CHECK(local_type[0] == 2 && local_type[1] == 2 && local_addr[0] == &link->extended_scan.addr && local_addr[1] == &link->extended_scan.addr);
    CHECK(link->extended_scan.coded_s == 1 && link->state == 12);
    CHECK(scan_hw == ext_hw && scan_primary == primary_hw && scan_arg == &link->extended_scan);
    CHECK(strcmp(trace, "extopen open localaddr extlocaladdr scanning ") == 0);
    CHECK(errors == 0);
    /* no duration: no timer */
    reset();
    ext_le_param.ext_scan = new_scan(0, 0, 1);
    CHECK(ll_ext_scan_open() == 0 && timeouts == 0 && ext_ll_hdl.ext_scan_timer == 0 && ext_ll_hdl.ext_scan_link->state == 12);
    /* secondary link allocation fails */
    reset(); new_link_fails = 1;
    ext_le_param.ext_scan = new_scan(0, 0, 1);
    CHECK(ll_ext_scan_open() == 7 && ext_ll_hdl.ext_scan_link == NULL && errors == 1 && frees == 0);
    /* secondary radio open fails: the link is freed */
    reset(); ext_open_null = 1;
    ext_le_param.ext_scan = new_scan(0, 0, 1);
    CHECK(ll_ext_scan_open() == 7 && ext_ll_hdl.ext_scan_link == NULL && errors == 1 && frees == 1 && freed != NULL);
    /* primary radio open fails: the link stays registered, half built */
    reset(); primary_open_null = 1;
    ext_le_param.ext_scan = new_scan(5, 0, 1);
    CHECK(ll_ext_scan_open() == 7 && ext_ll_hdl.ext_scan_link != NULL && errors == 1 && registered != NULL && nctrl == 0 && timeouts == 0);
    CHECK(ext_ll_hdl.ext_scan_link->primary_legacy_hw == NULL && ext_ll_hdl.ext_scan_link->state == 0);
    /* duration timer cannot be created: closes the link, returns 7, no scanning */
    reset(); timer_fail = 1;
    ext_le_param.ext_scan = new_scan(5, 0, 1);
    CHECK(ll_ext_scan_open() == 7 && timer_msec == 50 && ext_ll_hdl.ext_scan_link == NULL && errors == 1);
    CHECK(strstr(trace, "scanning") == NULL && strstr(trace, "linkclose") != NULL);
    CHECK(ext_ll_hdl.ext_scan_timer == 0);

    /* close */
    reset();
    ll_ext_scan_close();
    CHECK(trace[0] == 0);                                /* nothing to do without a link */
    link = new_link();
    ext_ll_hdl.ext_scan_link = link;
    ext_ll_hdl.ext_scan_timer = 9;
    ll_ext_scan_close();
    CHECK(strcmp(trace, "timerdel irqoff legacyclose irqon linkclose ") == 0);
    CHECK(removed_timer == 9 && ext_ll_hdl.ext_scan_timer == 0 && closed_link == link && closed_hw == ext_hw);
    CHECK(ext_ll_hdl.ext_scan_link == NULL);
    ext_ll_hdl.ext_scan_link = link;
    trace[0] = 0;
    ll_ext_scan_close();
    CHECK(strcmp(trace, "irqoff legacyclose irqon linkclose ") == 0);   /* no timer to remove */

    /* destroy */
    reset();
    ext_le_param.ext_scan = new_scan(0, 0, 0);
    ext_le_param.ext_scan->period_sync = calloc(1, sizeof(struct ble_ext_period_sync));
    ext_le_param.ext_scan->period_sync->sync_handle = 0x0123;
    struct __ext_scan *scan = ext_le_param.ext_scan;
    link = new_link();
    ext_ll_hdl.ext_scan_link = link;
    destroy_le_ext_scan();
    CHECK(terminates == 1 && last_terminate == 0x0123 && freed == scan && ext_le_param.ext_scan == NULL);
    CHECK(ext_ll_hdl.ext_scan_link == NULL && strstr(trace, "linkclose") != NULL);
    reset();
    destroy_le_ext_scan();                               /* nothing allocated, nothing linked */
    CHECK(terminates == 0 && frees == 0 && trace[0] == 0);

    /* duration end */
    reset();
    ext_le_param.ext_scan = new_scan(100, 20, 0);
    link = new_link();
    link->extended_scan.ext_scan = ext_le_param.ext_scan;
    ext_ll_hdl.ext_scan_link = link;
    ll_ext_scan_duration_end();
    CHECK(timer_msec == 20 * 1280 - 100 * 10 && timeouts == 1 && ext_ll_hdl.ext_scan_link == NULL);
    CHECK(strstr(trace, "linkclose") != NULL && frees == 0 && ext_le_param.ext_scan != NULL);
    reset();
    ext_le_param.ext_scan = new_scan(100, 0, 0);
    scan = ext_le_param.ext_scan;
    link = new_link();
    link->extended_scan.ext_scan = scan;
    ext_ll_hdl.ext_scan_link = link;
    ll_ext_scan_duration_end();                          /* period 0: the scan is destroyed */
    CHECK(timeouts == 0 && freed == scan && ext_le_param.ext_scan == NULL && ext_ll_hdl.ext_scan_link == NULL);

    /* timer handler */
    reset();
    link = new_link();
    ext_ll_hdl.ext_scan_link = link;
    ext_ll_hdl.ext_scan_timer = 31;
    ll_ext_scan_timer_handler(NULL);
    CHECK(removed_timer == 31 && ext_ll_hdl.ext_scan_timer == 0 && link->state == 13 && resumes == 1);
    CHECK(errors == 1 && strstr(trace, "scantimeout") != NULL && ext_ll_hdl.ext_scan_link == link);
    reset();                                             /* no link: the scan is opened again */
    ext_le_param.ext_scan = new_scan(0, 0, 0);
    ll_ext_scan_timer_handler(NULL);
    CHECK(ext_ll_hdl.ext_scan_link != NULL && ext_ll_hdl.ext_scan_link->state == 12 && resumes == 0);

    /* rx post process */
    reset();
    int empty = 0;
    ll_ext_scan_rx_post_process(&empty);                 /* no link */
    CHECK(trace[0] == 0);
    link = new_link();
    ext_ll_hdl.ext_scan_link = link;
    config_btctler_le_roles = 0;
    ll_ext_scan_rx_post_process(&empty);                 /* role bit clear */
    CHECK(trace[0] == 0);
    config_btctler_le_roles = 1;
    make_rx(0, 32, 6 + 3);
    upload_rx = rx;
    ll_ext_scan_rx_post_process(&empty);
    CHECK(periodic_closes == 1 && ioctrl_11 == 1 && ioctrl_empty == &empty && strcmp(trace, "syncclose upload ") == 0);
    CHECK(nreports == 1 && reports[0].r.Data_Length == 3);   /* the uploaded PDU is reported */
    reset();
    ext_le_param.ext_scan = new_scan(100, 0, 0);
    link = new_link();
    link->extended_scan.ext_scan = ext_le_param.ext_scan;
    link->state = 13;
    ext_ll_hdl.ext_scan_link = link;
    ll_ext_scan_rx_post_process(&empty);                 /* duration end, period 0 */
    CHECK(periodic_closes == 0 && ioctrl_11 == 0 && ext_ll_hdl.ext_scan_link == NULL && ext_le_param.ext_scan == NULL);

    return TEST_DONE("btctrler_ll_ext_scan");
}
#endif
