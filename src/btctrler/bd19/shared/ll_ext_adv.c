/* BLE 5 extended advertising sets: open the primary (legacy channels) and
 * secondary (auxiliary channel) radio links of an advertising set, program
 * the radio from the Host's parameters, run the set's duration/event-count
 * timeout, answer scan requests and connection requests, and tear the
 * links down.  Also the lookup of a set by handle.  SDK quirks kept: a set
 * whose secondary link fails to open leaves its primary link registered
 * without a timer; the legacy-PDU copy puts the whole Host advertising data
 * (up to 255 bytes) into a 31-byte buffer; the legacy exit hook closes only
 * the first set; a scan request on a legacy set reads the secondary link
 * that such a set does not have; the data update reads the secondary link
 * without checking it and passes an uninitialised periodic block to the
 * radio; the receive-post and probe handlers of the SDK's other members are
 * not touched. */
#include "btctrler/glue.h"
#include <stdint.h>
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_EXT_ADV, log_tag_const_i_LL_EXT_ADV;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_vendor_le_bb;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);

#define LL_CODE __attribute__((section(".ble_ll_code")))
#ifdef CONFIG_CPU_BD19
#define LL_EXT_ADV_MEMCPY(dst, src, size) __builtin_memcpy((dst), (src), (size))
#define LL_EXT_ADV_MEMSET(dst, value, size) __builtin_memset((dst), (value), (size))
#else
#define LL_EXT_ADV_MEMCPY(dst, src, size) memcpy((dst), (src), (size))
#define LL_EXT_ADV_MEMSET(dst, value, size) memset((dst), (value), (size))
#endif

#ifndef UNITTEST
_Static_assert(sizeof(struct le_ext_primary_link) == 28, "SDK primary link size");
_Static_assert(sizeof(struct le_ext_secondary_link) == 16, "SDK secondary link size");
_Static_assert(offsetof(struct le_ext_primary_link, hw) == 8, "SDK primary hw");
_Static_assert(offsetof(struct le_ext_primary_link, legacy_adv_link) == 12, "SDK primary legacy link");
_Static_assert(offsetof(struct le_ext_primary_link, secondary_link) == 16, "SDK primary secondary link");
_Static_assert(offsetof(struct le_ext_primary_link, adv_timeout) == 20, "SDK primary timer");
_Static_assert(offsetof(struct le_ext_primary_link, adv_timeout_type) == 24, "SDK primary timeout type");
_Static_assert(offsetof(struct le_ext_primary_link, adv_handle) == 25, "SDK primary handle");
_Static_assert(offsetof(struct le_ext_secondary_link, connect_link) == 4, "SDK secondary connect link");
_Static_assert(offsetof(struct le_ext_secondary_link, periodic_link) == 8, "SDK secondary periodic link");
_Static_assert(offsetof(struct le_ext_secondary_link, adv_handle) == 12, "SDK secondary handle");
_Static_assert(offsetof(struct le_ext_periodic_link, hw) == 8, "SDK periodic hw");
_Static_assert(offsetof(struct le_ext_periodic_link, sync_hw_info) == 12, "SDK periodic access address");
_Static_assert(sizeof(struct ble_ext_adv) == 64, "SDK ble_ext_adv size");
_Static_assert(offsetof(struct ble_ext_adv, sync_info) == 44, "SDK sync info");
_Static_assert(offsetof(struct __ext_adv_group, ext_adv_en) == 292, "SDK extended advertising enable");
_Static_assert(offsetof(struct __ext_adv_group, periodic_info) == 296, "SDK periodic info");
#endif

static void ll_ext_adv_rx_probe_handler(void *priv, struct ble_ext_rx *rx) LL_CODE;
const struct ble_ext_handler ll_ext_adv_handler __attribute__((section(".ble_ll_const"), aligned(4))) = {
    ll_ext_adv_rx_probe_handler,
    NULL,
};

/* The SDK currently reports periodic advertising disabled for every handle. */
u8 LctrIsPerAdvEnabled(u8 adv_handle) LL_CODE;
u8 LctrIsPerAdvEnabled(u8 adv_handle)
{
    (void)adv_handle;
    return 0;
}

static void ll_acad_msg_handler(u8 msg_id, void *msg) LL_CODE;
static void ll_acad_msg_handler(u8 msg_id, void *msg)
{
    (void)msg;
    switch (msg_id) {
    case 0:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_CREATE_BIG\r\n");
        break;
    case 2:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_TERMINATED_BIG\r\n");
        break;
    case 1:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_CHANNEL_MAP\r\n");
        break;
    }
}

void ll_acad_init(void) LL_CODE;
void ll_acad_init(void)
{
    ll_dispatch_msg_table[8] = ll_acad_msg_handler;
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

static void list_add_tail(struct list_head *new)
{
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    struct list_head *prev = head->prev;
    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

/* The radio saw a CONNECT_REQ on the auxiliary channel: open a legacy
 * connection link on the secondary radio with the peer in the request. */
static void ll_ext_adv_rx_probe_handler(void *priv, struct ble_ext_rx *rx)
{
    if (!(config_btctler_le_roles & 2)) return;
    if (rx->type == 5) {
        struct le_ext_secondary_link *link = priv;
        struct le_addr local_addr;
        local_addr.addr_type = rx->rxadd;
        LL_EXT_ADV_MEMCPY(local_addr.addr, &rx->data[6], 6);
        __ble5_ops->legacy_open(link->hw);
        __ble5_ops->ext_ioctrl(link->hw, 15);
        link->connect_link = ll_create_conn_link(link->hw, &local_addr, 1, rx->txadd, rx->data, &rx->data[12]);
    }
    if (rx->len != 0) ll_thread_resume();
}

struct le_ext_primary_link *ext_primary_link_for_handle(u8 adv_handle) LL_CODE;
struct le_ext_primary_link *ext_primary_link_for_handle(u8 adv_handle)
{
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    for (struct list_head *p = head->next; p != head; p = p->next) {
        struct le_ext_primary_link *link = (struct le_ext_primary_link *)p;
        if (link->adv_handle == adv_handle) return link;
    }
    return NULL;
}

struct le_ext_periodic_link *ext_periodic_link_for_handle(u8 adv_handle) LL_CODE;
struct le_ext_periodic_link *ext_periodic_link_for_handle(u8 adv_handle)
{
    struct list_head *head = &ext_ll_hdl.ext_periodic_adv_links;
    for (struct list_head *p = head->next; p != head; p = p->next) {
        struct le_ext_periodic_link *link = (struct le_ext_periodic_link *)p;
        if (link->adv_handle == adv_handle) return link;
    }
    return NULL;
}

/* Stop the set's timeout timer; returns the primary link, NULL when there is
 * none. */
static struct le_ext_primary_link *ll_ext_adv_timeout_stop(u8 adv_handle) LL_CODE;
static struct le_ext_primary_link *ll_ext_adv_timeout_stop(u8 adv_handle)
{
    struct le_ext_primary_link *link = ext_primary_link_for_handle(adv_handle);
    if (link == NULL) return NULL;
    if (link->adv_timeout) {
        sys_timer_remove(link->adv_timeout);
        link->adv_timeout = 0;
    }
    return link;
}

static void ll_ext_adv_timer_handler(void *timer) LL_CODE;
static void ll_ext_adv_timer_handler(void *timer)
{
    /* The SDK passes the timer id as a 32-bit integer in the callback's argument. */
    struct le_ext_primary_link *link = sys_timer_get_context((sys_timer)(intptr_t)timer);
    if (log_tag_const_e_LL_EXT_ADV)
        log_print(4, NULL, "<Error>: [LL_EXT_ADV]TO - ll_ext_adv_timer_handler\r\n");
    link->adv_timeout_type |= 8;
    ll_thread_resume();
}

/* Arm the set's timeout from Duration (10 ms units) and Max Extended
 * Advertising Events (times the primary interval, at least 32 slots).  The
 * shorter of the two applies; zero means no limit. */
static int ll_ext_adv_create_alive_timer(struct le_ext_primary_link *link, struct __ext_adv_group *p) LL_CODE;
static int ll_ext_adv_create_alive_timer(struct le_ext_primary_link *link, struct __ext_adv_group *p)
{
    u32 duration_ms = p->ext_adv_en.duration * 10;
    u32 interval = btctler_little_endian_read_24(p->ext_adv_param.Primary_Advertising_Interval_Min, 0);
    u32 events_ms = ((((interval > 32) ? interval : 32) * 625) / 1000) * p->ext_adv_en.max_ext_adv_events;
    u32 timeout_ms;
    if (p->ext_adv_en.duration != 0 && events_ms != 0)
        timeout_ms = (duration_ms < events_ms) ? duration_ms : events_ms;
    else
        timeout_ms = events_ms | duration_ms;
    link->adv_timeout_type = (timeout_ms == duration_ms) ? 2 : 4;
    if (timeout_ms == 0) return 0;
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]ext adv timeout : %dms\r\n", timeout_ms);
    link->adv_timeout = sys_timer_register(timeout_ms, ll_ext_adv_timer_handler);
    if (link->adv_timeout == 0) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]create ext_adv timer fail\r\n");
        ll_ext_adv_close(link->adv_handle);
        return 7;
    }
    sys_timer_set_context(link->adv_timeout, link);
    return 0;
}

int ll_ext_adv_alive_timer_reset(u8 adv_handle, struct __ext_adv_group *p) LL_CODE;
int ll_ext_adv_alive_timer_reset(u8 adv_handle, struct __ext_adv_group *p)
{
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "ll_ext_adv_alive_timer_reset");
    struct le_ext_primary_link *link = ll_ext_adv_timeout_stop(adv_handle);
    if (link == NULL) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]find primary link fail when reset timer\r\n");
        return 3;
    }
    return ll_ext_adv_create_alive_timer(link, p);
}

/* Tell the Host the set ended and forget the Host's parameters for it. */
static void ll_advertising_set_terminated(u16 connection_handle, u8 status, u8 advertising_handle) LL_CODE;
static void ll_advertising_set_terminated(u16 connection_handle, u8 status, u8 advertising_handle)
{
    ll_ext_adv_timeout_stop(advertising_handle);
    struct __ext_adv_group *set = ll_match_ext_adv_set_addr(advertising_handle);
    if (set != NULL) ll_remove_assigned_adv_set(set);
    le_advertising_set_terminated_event(status, advertising_handle, connection_handle);
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]ll_advertising_set_terminated succ\r\n");
}

/* A legacy connection took over: report the first set as terminated by it
 * and close it.  The SDK does not look at which set the connection came
 * from. */
void ll_exit_ext_legacy_adv_status(u16 connection_handle) LL_CODE;
void ll_exit_ext_legacy_adv_status(u16 connection_handle)
{
    if (!(config_btctler_le_features & 4096)) return;
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "ll_exit_ext_legacy_adv_status");
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    if (head->next == head) return;
    struct le_ext_primary_link *link = (struct le_ext_primary_link *)head->next;
    ll_advertising_set_terminated(connection_handle, 0, link->adv_handle);
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "ll_exit_ext_adv_status");
    link->legacy_adv_link = NULL;
    ll_ext_adv_close(link->adv_handle);
}

/* Close every extended set the Host defined. */
void le_ext_adv_destroy(void) LL_CODE;
void le_ext_adv_destroy(void)
{
    ll_remove_all_adv_set();
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    struct list_head *p = head->next;
    while (p != head) {
        struct list_head *next = p->next;
        u8 adv_handle = ((struct le_ext_primary_link *)p)->adv_handle;
        ll_ext_adv_close(adv_handle);
        if (config_btctler_le_features & 8192) ll_periodic_adv_close(adv_handle);
        p = next;
    }
}

void ll_remove_all_adv_set(void) LL_CODE;
void ll_remove_all_adv_set(void)
{
    struct list_head *head = &ext_le_param.ext_adv_list.head;
    struct list_head *p = head->next;
    while (p != head) {
        struct list_head *next = p->next;
        struct __ext_adv_group *set = (struct __ext_adv_group *)p;
        list_del(&set->entry);
        if (set->periodic_info) __bt_free(set->periodic_info);
        __bt_free(set);
        p = next;
    }
    ext_le_param.ext_adv_list.nums_set = 0;
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]ll_remove_all_adv_set succ\n\r\n");
}

/* Stop the set's timer and radio links and free them.  Always returns 0. */
int ll_ext_adv_close(u8 adv_handle) LL_CODE;
int ll_ext_adv_close(u8 adv_handle)
{
    struct le_ext_primary_link *link = ll_ext_adv_timeout_stop(adv_handle);
    if (link == NULL) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]find primary link fail when close\r\n");
        return 0;
    }
    if (link->legacy_adv_link) ll_adv_close();
    struct le_ext_secondary_link *secondary = link->secondary_link;
    if (secondary) {
        __ble5_ops->ext_adv_ctrl(secondary->hw, 6, NULL);
        if (secondary->periodic_link) __ble5_ops->ext_adv_ctrl(secondary->periodic_link->hw, 6, NULL);
    }
    list_del(&link->entry);
    __ll_ext_link_close(link, link->hw);
    if (secondary) __ll_ext_link_close(secondary, secondary->hw);
    return 0;
}

/* The radio delivered auxiliary-channel PDUs: a scan request is reported to
 * the Host, a connect request starts the legacy connection. */
static void ll_scan_request_received(u16 adv_handle, u8 rxadd, u8 *data) LL_CODE;
static void ll_ext_adv_rx_post(void *priv, struct ble_ext_rx *rx) LL_CODE;
static void ll_ext_adv_rx_post(void *priv, struct ble_ext_rx *rx)
{
    const struct le_ext_secondary_link *link = priv;
    switch (rx->type) {
    case 3:
        ll_scan_request_received(link->adv_handle, rx->rxadd, rx->data);
        break;
    case 5:
        ll_ext_create_legacy_connection(link->connect_link);
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]ext adv post rx AUX_CONNECT_REQ\r\n");
        break;
    }
}

/* Poll every set's secondary radio.  A set whose connection came up, or
 * whose duration or event limit was reached (timeout type bit 3), is
 * reported terminated and closed. */
void ll_ext_adv_rx_post_process(int *empty) LL_CODE;
void ll_ext_adv_rx_post_process(int *empty)
{
    if (!(config_btctler_le_roles & 2)) return;
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    struct list_head *p = head->next;
    while (p != head) {
        struct list_head *safe = p->next;
        struct le_ext_primary_link *primary = (struct le_ext_primary_link *)p;
        struct le_ext_secondary_link *secondary = primary->secondary_link;
        if (secondary != NULL) {
            __ble5_ops->ext_upload_data(secondary->hw, ll_ext_adv_rx_post);
            struct le_link *connect_link = primary->secondary_link->connect_link;
            u16 connection_handle;
            u8 status;
            if (connect_link != NULL) {
                primary->legacy_adv_link = NULL;
                connection_handle = connect_link->handle;
                if (log_tag_const_i_LL_EXT_ADV)
                    log_print(2, NULL, "[Info]: [LL_EXT_ADV]connection_handle=0x%x\r\n", connection_handle);
                status = 0;
            } else if (primary->adv_timeout_type & 8) {
                status = (primary->adv_timeout_type & 2) ? 60 : 67;
                connection_handle = 0;
            } else {
                __ble5_ops->ext_ioctrl(secondary->hw, 11, empty);
                p = safe;
                continue;
            }
            ll_advertising_set_terminated(connection_handle, status, primary->adv_handle);
            ll_ext_adv_close(primary->adv_handle);
        }
        p = safe;
    }
}

/* The Host's parameters of the set with this handle, NULL when unknown. */
void *ll_match_ext_adv_set_addr(u8 adv_handle) LL_CODE;
void *ll_match_ext_adv_set_addr(u8 adv_handle)
{
    struct list_head *head = &ext_le_param.ext_adv_list.head;
    for (struct list_head *p = head->next; p != head; p = p->next) {
        struct __ext_adv_group *set = (struct __ext_adv_group *)p;
        if (set->ext_adv_param.Advertising_Handle == adv_handle) return set;
    }
    return NULL;
}

void ll_remove_assigned_adv_set(struct __ext_adv_group *p) LL_CODE;
void ll_remove_assigned_adv_set(struct __ext_adv_group *p)
{
    list_del(&p->entry);
    if (p->periodic_info) __bt_free(p->periodic_info);
    __bt_free(p);
    ext_le_param.ext_adv_list.nums_set--;
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]ll_remove_assigned_adv_set succ\n\r\n");
}

/* Map the Host's advertising event properties to the radio's event type and
 * say whether an auxiliary PDU is needed. */
static void __set_ll_ext_primary_adv_param(struct __ext_adv_group *adv_set, struct ble_ext_adv *primary_adv) LL_CODE;
static void __set_ll_ext_primary_adv_param(struct __ext_adv_group *adv_set, struct ble_ext_adv *primary_adv)
{
    u8 channel_map = adv_set->ext_adv_param.Primary_Advertising_Channel_Map;
    primary_adv->channel_map = channel_map;
    primary_adv->pkt_cnt = 0;
    for (int i = 0; i < 3; i++)
        if (channel_map & (1 << i)) primary_adv->pkt_cnt++;
    primary_adv->filter_policy = adv_set->ext_adv_param.Advertising_Filter_Policy;

    primary_adv->is_need_aux = 1;
    switch (adv_set->ext_adv_param.Advertising_Event_Properties) {
    case 1:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]CONNECTABLE_UNDIRECTED\n\r\n");
        primary_adv->event_type = 5;
        break;
    case 5:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]CONNECTABLE_DIRECTED\n\r\n");
        primary_adv->event_type = 6;
        break;
    case 2:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]SCANNABLE_UNDIRECTED\n\r\n");
        primary_adv->event_type = 7;
        break;
    case 6:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]SCANNABLE_DIRECTED\n\r\n");
        primary_adv->event_type = 8;
        break;
    case 0:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]NON_CONNECT_NON_SCAN\n\r\n");
        primary_adv->event_type = 2;
        if (adv_set->ext_adv_data.cur_len == 0 && adv_set->periodic_info == NULL) {
            primary_adv->event_type = 1;
            primary_adv->is_need_aux = 0;
        }
        break;
    case 4:
        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]NON_CONNECT_NON_SCAN_DIRECTED\n\r\n");
        primary_adv->event_type = 4;
        if (adv_set->ext_adv_data.cur_len == 0 && adv_set->periodic_info == NULL) {
            primary_adv->event_type = 3;
            primary_adv->is_need_aux = 0;
        }
        break;
    default:
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]get_ext_event_type error\n\r\n");
        break;
    }

    LL_EXT_ADV_MEMCPY(&primary_adv->peer_addr.addr_type, &adv_set->ext_adv_param.Peer_Address_Type, 7);
    __set_ll_local_addr_common(&primary_adv->addr, adv_set->ext_adv_param.Own_Address_Type);
    primary_adv->sid = adv_set->ext_adv_param.Advertising_SID & 15;
    pseudo_random_genrate(&primary_adv->channel_index, 1);
    primary_adv->channel_index = btctler_get_rand_from_assign_range(primary_adv->channel_index, 0, 36);
    primary_adv->primary_phy = adv_set->ext_adv_param.Primary_Advertising_PHY - 1;
    primary_adv->secondary_phy = adv_set->ext_adv_param.Secondary_Advertising_PHY - 1;
    primary_adv->coded_s = ll_set_code_type;
    primary_adv->secondary_max_skip = adv_set->ext_adv_param.Secondary_Advertising_Max_Skip;
    primary_adv->max_ext_adv_events = adv_set->ext_adv_en.max_ext_adv_events;
    primary_adv->duration = adv_set->ext_adv_en.duration;
    primary_adv->interval = btctler_little_endian_read_24(adv_set->ext_adv_param.Primary_Advertising_Interval_Min, 0);
    primary_adv->pdu_interval = (primary_adv->primary_phy == 2) ? 1364 : 1250;
    if (log_tag_const_i_LL_EXT_ADV) {
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv pdu interval : %u us\r\n", primary_adv->pdu_interval);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv interval (N*625us): 0x%x\r\n", primary_adv->interval);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv filter policy: 0x%x\r\n", primary_adv->filter_policy);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv sid: 0x%x\r\n", primary_adv->sid);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv channel_index: 0x%x\r\n", primary_adv->channel_index);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv primary_phy: 0x%x\r\n", primary_adv->primary_phy);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv secondary_phy: 0x%x\r\n", primary_adv->secondary_phy);
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv channel map: 0x%x\r\n", primary_adv->channel_map);
    }
}

/* Draw a 12-bit advertising data ID; the primary PDU carries no data of its
 * own.  SDK bug kept: when the draw equals the previous ID the SDK increments
 * a local copy and then stores the unincremented ID anyway. */
static void __set_ll_ext_primary_adv_data(struct ble_ext_adv *primary_adv) LL_CODE;
static void __set_ll_ext_primary_adv_data(struct ble_ext_adv *primary_adv)
{
    u16 did;
    pseudo_random_genrate((u8 *)&did, 2);
    did &= 4095;
    u16 drawn = did;
    if (did == primary_adv->did) did = did + 1;
    primary_adv->did = drawn;
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]primary_adv did: 0x%x\r\n", drawn);
    primary_adv->adv_data_len = 0;
    primary_adv->adv_data = NULL;
}

/* Fill in what the secondary radio link needs: its address, the peer for a
 * directed set, and for a set with periodic advertising the sync info (the
 * interval is raised to 6 unless vendor bit 0 is set, fixed channel map and
 * CRC init, and the periodic link's access address, generated on first use). */
static void __set_ll_ext_secondary_adv_param(struct le_ext_secondary_link *link, struct __ext_adv_group *adv_set, struct ble_ext_adv *ext_adv) LL_CODE;
static void __set_ll_ext_secondary_adv_param(struct le_ext_secondary_link *link, struct __ext_adv_group *adv_set, struct ble_ext_adv *ext_adv)
{
    static const u8 channel[5] = {0xff, 0xff, 0xff, 0xff, 0x1f};
    static const u8 crcinit[3] = {0xae, 0x83, 0x19};

    __set_ll_local_addr_common(&ext_adv->addr, adv_set->ext_adv_param.Own_Address_Type);
    if (ext_adv->event_type == 6 || ext_adv->event_type == 8) {
        LL_EXT_ADV_MEMCPY(ext_adv->peer_addr.addr, adv_set->ext_adv_param.Peer_Address, 6);
        if (log_tag_const_i_LL_EXT_ADV) {
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]peer addr type=0x%x\n\r\n", ext_adv->peer_addr.addr_type);
            printf_buf(ext_adv->peer_addr.addr, 6);
        }
    }
    if (link->periodic_link != NULL) {
        u16 min = adv_set->periodic_info->Periodic_Advertising_Interval_Min;
        ext_adv->sync_info.interval = (config_vendor_le_bb & 1) ? min : (min > 6 ? min : 6);
        LL_EXT_ADV_MEMCPY(ext_adv->sync_info.chm, channel, 5);
        LL_EXT_ADV_MEMCPY(ext_adv->sync_info.crcinit, crcinit, 3);
        if (link->periodic_link->sync_hw_info.aa == 0)
            access_addr_generate((u8 *)&link->periodic_link->sync_hw_info.aa);
        ext_adv->sync_info.aa = link->periodic_link->sync_hw_info.aa;
        if (log_tag_const_i_LL_EXT_ADV) {
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]sync adv aa = \n\r\n");
            printf_buf(&ext_adv->sync_info.aa, 4);
        }
    }
}

/* Replace the running set's advertising data: rebuild the radio's view of
 * the set and hand it over (command 8).  Returns 3 when no primary link has
 * this handle (the set is not running). */
int ll_ext_adv_data_update(u8 adv_handle, struct __ext_adv_group *adv_set) LL_CODE;
int ll_ext_adv_data_update(u8 adv_handle, struct __ext_adv_group *adv_set)
{
    struct ble_ext_adv ext_adv;
    struct ble_ext_period_adv sync_adv;   /* left uninitialised, as in the SDK */
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "ll_ext_adv_data_update");
    struct le_ext_primary_link *link = ext_primary_link_for_handle(adv_handle);
    if (link == NULL) return 3;
    LL_EXT_ADV_MEMSET(&ext_adv, 0, 64);
    __set_ll_ext_primary_adv_param(adv_set, &ext_adv);
    __set_ll_ext_primary_adv_data(&ext_adv);
    __set_ll_ext_secondary_adv_param(link->secondary_link, adv_set, &ext_adv);
    ext_adv.adv_data_len = adv_set->ext_adv_data.cur_len;
    ext_adv.adv_data = adv_set->ext_adv_data.data;
    if (link->secondary_link->periodic_link != NULL) ll_periodic_adv_data_update(adv_set, &sync_adv);
    __ble5_ops->ext_adv_ctrl(link->hw, 8, &ext_adv, &sync_adv);
    return 0;
}

/* Translate the Host's extended parameters into the legacy advertiser's:
 * the event properties without the legacy bit (16) select the PDU type, and
 * the whole Host advertising data goes to both the advertising and the scan
 * response buffer.  SDK bug kept: those buffers hold 31 bytes, the copy takes
 * up to 255. */
static void exchange_ExtAdvParam_2_LegacyAdvParam(struct __ext_adv_group *p) LL_CODE;
static void exchange_ExtAdvParam_2_LegacyAdvParam(struct __ext_adv_group *p)
{
    struct advertising_param adv_param;
    struct advertising_data adv_data;
    struct scan_response scan_resp_data;
    u8 type;
    adv_param.Advertising_Interval_Min = btctler_little_endian_read_16(p->ext_adv_param.Primary_Advertising_Interval_Min, 0);
    adv_param.Advertising_Interval_Max = btctler_little_endian_read_16(p->ext_adv_param.Primary_Advertising_Interval_Max, 0);
    switch (p->ext_adv_param.Advertising_Event_Properties & ~16) {
    case 3: type = 0; break;
    case 5: type = 4; break;
    case 13: type = 1; break;
    case 2: type = 2; break;
    case 0: type = 3; break;
    default:
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]Advertising_Event_Propertiese error\r\n\r\n");
        type = 0;
        break;
    }
    adv_param.Advertising_Type = type;
    adv_param.Own_Address_Type = p->ext_adv_param.Own_Address_Type;
    adv_param.Peer_Address_Type = p->ext_adv_param.Peer_Address_Type;
    LL_EXT_ADV_MEMCPY(adv_param.Peer_Address, p->ext_adv_param.Peer_Address, 6);
    adv_param.Advertising_Channel_Map = p->ext_adv_param.Primary_Advertising_Channel_Map;
    adv_param.Advertising_Filter_Policy = p->ext_adv_param.Advertising_Filter_Policy;
    ll_adv_set_params((const u8 *)&adv_param);

    u16 len = p->ext_adv_data.cur_len;
    adv_data.length = len;
    LL_EXT_ADV_MEMCPY(adv_data.data, p->ext_adv_data.data, len & 255);
    ll_adv_set_adv_data(&adv_data.length);

    len = p->ext_adv_data.cur_len;
    scan_resp_data.length = len;
    LL_EXT_ADV_MEMCPY(scan_resp_data.data, p->ext_adv_data.data, len & 255);
    ll_adv_set_scan_rsp_data(&scan_resp_data.length);
}

/* Start a legacy-PDU set (event properties bit 4) on the legacy advertiser.
 * Returns 7 when the link cannot be allocated, else the advertiser's status. */
int ll_ext_legacy_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set) LL_CODE;
int ll_ext_legacy_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set)
{
    struct le_ext_primary_link *link = __new_ext_link(28);
    if (link == NULL) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]Legacy adv create error\r\n");
        return 7;
    }
    link->adv_handle = adv_handle;
    list_add_tail(&link->entry);
    exchange_ExtAdvParam_2_LegacyAdvParam(adv_set);
    int status = ll_adv_open();
    link->legacy_adv_link = ll_adv_link_get();
    return status;
}

/* Start an extended set: open the primary radio link and program it, open
 * the secondary link (and tie in the periodic link) when the set needs an
 * auxiliary PDU, arm the timeout, then start the radios.  Returns 7 when a
 * link or timer cannot be created.  A failed secondary leaves the primary
 * link registered. */
int ll_ext_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set) LL_CODE;
int ll_ext_adv_open(u8 adv_handle, struct __ext_adv_group *adv_set)
{
    struct ble_ext_adv ext_adv;
    LL_EXT_ADV_MEMSET(&ext_adv, 0, 64);

    struct le_ext_primary_link *link = __new_ext_link(28);
    if (link != NULL) {
        link->hw = __ble5_ops->ext_open();
        if (link->hw == NULL) {
            __bt_free(link);
            link = NULL;
        }
    }
    if (link == NULL) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]ext adv primary create error\r\n");
        return 7;
    }
    link->adv_handle = adv_handle;
    list_add_tail(&link->entry);

    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "__set_ll_ext_primary_adv_state");
    __set_ll_ext_primary_adv_param(adv_set, &ext_adv);
    __set_ll_ext_primary_adv_data(&ext_adv);
#ifdef CONFIG_CPU_BD19
    /* BD19 caches this pointer across the two radio callbacks. */
    void *primary_hw = link->hw;
    __ble5_ops->ext_ioctrl(primary_hw, 9, ext_adv.addr.addr_type, ext_adv.addr.addr);
    __ble5_ops->ext_adv_ctrl(primary_hw, 0, &ext_adv);
#else
    __ble5_ops->ext_ioctrl(link->hw, 9, ext_adv.addr.addr_type, ext_adv.addr.addr);
    __ble5_ops->ext_adv_ctrl(link->hw, 0, &ext_adv);
#endif

    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]is_need_secondary=0x%x\n\r\n", ext_adv.is_need_aux);
    if (ext_adv.is_need_aux) {
        u8 handle = link->adv_handle;
        struct le_ext_secondary_link *secondary = __new_ext_link(16);
        if (secondary != NULL) {
            secondary->hw = __ble5_ops->ext_open();
            if (secondary->hw == NULL) {
                __bt_free(secondary);
                secondary = NULL;
            }
        }
        if (secondary == NULL) {
            if (log_tag_const_e_LL_EXT_ADV)
                log_print(4, NULL, "<Error>: [LL_EXT_ADV]ext adv secondary create error\r\n");
            return 7;
        }
        secondary->adv_handle = handle;
        link->secondary_link = secondary;
        __ble5_ops->ext_adv_ctrl(link->hw, 6, secondary->hw);
        __ble5_ops->ext_adv_ctrl(secondary->hw, 6, link->hw);
        __ble5_ops->ext_handler_register(secondary->hw, secondary, &ll_ext_adv_handler);
        struct le_ext_periodic_link *periodic = ext_periodic_link_for_handle(link->adv_handle);
        if (periodic != NULL) {
            secondary->periodic_link = periodic;
            __ble5_ops->ext_adv_ctrl(periodic->hw, 6, secondary->hw);
            __ble5_ops->ext_adv_ctrl(secondary->hw, 7, periodic->hw);
        }

        if (log_tag_const_i_LL_EXT_ADV)
            log_print(2, NULL, "[Info]: [LL_EXT_ADV]--func=%s\r\n", "__set_ll_ext_secondary_adv_state");
        __set_ll_ext_secondary_adv_param(secondary, adv_set, &ext_adv);
        ext_adv.adv_data_len = adv_set->ext_adv_data.cur_len;
        ext_adv.adv_data = adv_set->ext_adv_data.data;
        void *hw = secondary->hw;
        __ble5_ops->ext_ioctrl(hw, 9, ext_adv.addr.addr_type, ext_adv.addr.addr);
        if (ext_adv.event_type == 6 || ext_adv.event_type == 8)
            __ble5_ops->ext_ioctrl(hw, 10, ext_adv.peer_addr.addr_type, ext_adv.peer_addr.addr);
        __ble5_ops->ext_adv_ctrl(hw, 1, &ext_adv);
    }

    int status = ll_ext_adv_create_alive_timer(link, adv_set);
    if (status != 0) return status;
    __ble5_ops->ext_adv_ctrl(link->hw, 3, 1);
    if (link->secondary_link == NULL) return 0;
    __ble5_ops->ext_adv_ctrl(link->secondary_link->hw, 4, 1);
    if (link->secondary_link->periodic_link == NULL) return 0;
    __ble5_ops->ext_adv_ctrl(link->secondary_link->periodic_link->hw, 5, 1);
    return 0;
}

/* Report a scan request to the Host when the set asked for notifications.
 * SDK quirk kept: an unknown handle ends the walk on the list head, which is
 * then read as if it were a set. */
static void ll_scan_request_received(u16 adv_handle, u8 rxadd, u8 *data)
{
    struct list_head *head = &ext_le_param.ext_adv_list.head;
    struct __ext_adv_group *set = (struct __ext_adv_group *)head->next;
    while (&set->entry != head) {
        if (adv_handle == set->ext_adv_param.Advertising_Handle) break;
        set = (struct __ext_adv_group *)set->entry.next;
    }
    if (set->ext_adv_param.Scan_Request_Notification_Enable)
        le_scan_request_received_event(adv_handle, rxadd, data);
}

/* The legacy advertiser got a scan request: report it for the extended set
 * that owns the link.  SDK bug kept: the handle comes from the set's
 * secondary link, which a legacy-PDU set does not have. */
void ll_ext_legacy_adv_scan_req_received(struct le_link *link, struct ble_rx *rx) LL_CODE;
void ll_ext_legacy_adv_scan_req_received(struct le_link *link, struct ble_rx *rx)
{
    if (!(config_btctler_le_features & 4096)) return;
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    for (struct list_head *p = head->next; p != head; p = p->next) {
        struct le_ext_primary_link *primary = (struct le_ext_primary_link *)p;
        if ((void *)primary->legacy_adv_link == (void *)link) {
            ll_scan_request_received(primary->secondary_link->adv_handle, rx->rxadd, rx->data);
            return;
        }
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct ext_le_parameter ext_le_param;
struct ext_link_layer_handle ext_ll_hdl;
ll_dispatch_msg_t ll_dispatch_msg_table[15];
const struct ble5_operation *__ble5_ops;
u8 log_tag_const_e_LL_EXT_ADV = 1, log_tag_const_i_LL_EXT_ADV = 0;
u64 config_btctler_le_features = 4096 | 8192;
int config_btctler_le_roles = 2;
int config_vendor_le_bb;
u8 ll_set_code_type;

static char trace[1024];
static void TF(const char *format, ...)
{
    va_list ap;
    size_t used = strlen(trace);
    va_start(ap, format);
    vsnprintf(trace + used, sizeof(trace) - used, format, ap);
    va_end(ap);
}
#define HW(n) ((void *)(uintptr_t)(n))
#define ID(p) ((int)(uintptr_t)(p))

static int errors, infos, resumes, frees, new_link_fail, new_link_len, open_count, open_fail_at;
static int mutate_primary_hw_during_ioctrl, primary_config_hw, primary_start_hw;
static const char *last_log_format;
static int timer_result = 77, timer_msec, timer_context_id, timer_removed;
static void (*timer_callback)(void *);
static void *timer_context;
static u32 random_pattern, rand_arg0, rand_arg1, rand_arg2;
static u32 rand_result = 11;
static struct ble_ext_adv ctrl0, ctrl1;
static u8 sync_adv_seen;
static u8 term_status, term_handle, term_count;
static u16 term_conn;
static int scan_events, scan_event_handle, scan_event_rxadd;
static u8 *scan_event_data;
static int legacy_open_hw, ioctrl15_hw, legacy_conns;
static struct le_link *created_link;
static struct le_addr created_local;
static int created_role, created_txadd;
static u8 *created_peer, *created_data;
static u8 adv_params[15], adv_data_pdu[32], scan_rsp_pdu[32];
static int adv_open_result, periodic_updates;
static struct le_adv_link *adv_link_result;
static u8 *empty_seen;
static void *upload_priv;
static struct ble_ext_rx *upload_rx;
static struct le_link *upload_connect;
static struct le_ext_secondary_link *upload_secondary;
static struct le_ext_secondary_link *handler_registered_priv;
static void *handler_registered_hw;
static const struct ble_ext_handler *handler_registered;

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag;
    last_log_format = format;
    if (level == 4) errors++; else infos++;
}
void printf_buf(const void *buffer, int size) { (void)buffer; TF("pb%d ", size); }
void ll_thread_resume(void) { resumes++; }
void __bt_free(void *p) { (void)p; frees++; }
void *__new_ext_link(u32 len)
{
    new_link_len = (int)len;
    if (new_link_fail) return NULL;
    return calloc(1, 256);
}
void __ll_ext_link_close(void *link, void *hw) { (void)link; TF("lc%d ", ID(hw)); }
uint16_t btctler_little_endian_read_16(const uint8_t *b, int pos) { return b[pos] | (b[pos + 1] << 8); }
uint32_t btctler_little_endian_read_24(const uint8_t *b, int pos) { return b[pos] | (b[pos + 1] << 8) | ((u32)b[pos + 2] << 16); }
sys_timer sys_timer_register(u32 msec, void (*callback)(void *))
{
    timer_msec = (int)msec; timer_callback = callback;
    TF("treg ");
    return timer_result;
}
void sys_timer_set_context(sys_timer t, void *context) { timer_context_id = t; timer_context = context; }
void *sys_timer_get_context(sys_timer t) { CHECK(t == timer_context_id); return timer_context; }
void sys_timer_remove(sys_timer t) { timer_removed = t; TF("trem "); }
void pseudo_random_genrate(uint8_t *dest, unsigned int size)
{
    for (unsigned i = 0; i < size; i++) dest[i] = (random_pattern >> (8 * i)) & 0xff;
}
uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max)
{
    rand_arg0 = rand; rand_arg1 = min; rand_arg2 = max;
    return rand_result;
}
void __set_ll_local_addr_common(struct le_addr *addr, u8 type)
{
    addr->addr_type = type;
    for (int i = 0; i < 6; i++) addr->addr[i] = 0xa0 + i;
}
void access_addr_generate(u8 *aa) { u32 v = 0x12345678; memcpy(aa, &v, 4); TF("aa "); }
void le_advertising_set_terminated_event(u8 status, u8 handle, u16 conn)
{
    term_status = status; term_handle = handle; term_conn = conn; term_count++;
    TF("term%d ", handle);
}
void le_scan_request_received_event(uint16_t handle, uint8_t rxadd, uint8_t *data)
{
    scan_events++; scan_event_handle = handle; scan_event_rxadd = rxadd; scan_event_data = data;
}
void ll_ext_create_legacy_connection(struct le_link *link) { (void)link; legacy_conns++; TF("legacyconn "); }
struct le_link *ll_create_conn_link(void *hw, struct le_addr *local_addr, u8 role, u8 peer_addr_type, u8 *peer_addr, void *ll_data)
{
    (void)hw;
    created_local = *local_addr; created_role = role; created_txadd = peer_addr_type;
    created_peer = peer_addr; created_data = ll_data;
    return created_link;
}
int ll_adv_set_params(const u8 *data) { memcpy(adv_params, data, 15); return 0; }
int ll_adv_set_adv_data(const u8 *data) { memcpy(adv_data_pdu, data, 32); return 0; }
int ll_adv_set_scan_rsp_data(const u8 *data) { memcpy(scan_rsp_pdu, data, 32); return 0; }
int ll_adv_open(void) { TF("advopen "); return adv_open_result; }
int ll_adv_close(void) { TF("advclose "); return 0; }
struct le_adv_link *ll_adv_link_get(void) { return adv_link_result; }
void ll_periodic_adv_data_update(struct __ext_adv_group *s, struct ble_ext_period_adv *p) { (void)s; (void)p; periodic_updates++; }
void ll_periodic_adv_close(u8 handle) { TF("pclose%d ", handle); }

static void *test_ext_open(void)
{
    open_count++;
    if (open_count == open_fail_at) return NULL;
    return HW(0x1000 + open_count);
}
static void test_legacy_open(void *hw) { legacy_open_hw = ID(hw); }
static void test_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    if (cmd == 9 || cmd == 10) {
        int type = va_arg(ap, int);
        u8 *addr = va_arg(ap, u8 *);
        if (cmd == 9 && mutate_primary_hw_during_ioctrl) {
            struct le_ext_primary_link *primary = ext_primary_link_for_handle(1);
            CHECK(primary != NULL);
            primary->hw = HW(0x1bad);
            mutate_primary_hw_during_ioctrl = 0;
        }
        TF("io%d_%d(%d,%02x) ", ID(hw), cmd, type, addr[0]);
    } else if (cmd == 11) {
        empty_seen = va_arg(ap, u8 *);
        TF("io%d_11 ", ID(hw));
    } else if (cmd == 15) {
        ioctrl15_hw = ID(hw);
    }
    va_end(ap);
}
static void test_adv_ctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    switch (cmd) {
    case 0: primary_config_hw = ID(hw); ctrl0 = *va_arg(ap, struct ble_ext_adv *); TF("c%d_0 ", ID(hw)); break;
    case 1: ctrl1 = *va_arg(ap, struct ble_ext_adv *); TF("c%d_1 ", ID(hw)); break;
    case 3: case 4: case 5:
        if (cmd == 3) primary_start_hw = ID(hw);
        TF("c%d_%d(%d) ", ID(hw), cmd, va_arg(ap, int)); break;
    case 6: case 7: TF("c%d_%d(%d) ", ID(hw), cmd, ID(va_arg(ap, void *))); break;
    case 8: (void)va_arg(ap, void *); sync_adv_seen = 1; TF("c%d_8 ", ID(hw)); break;
    default: CHECK(0);
    }
    va_end(ap);
}
static void test_register(void *hw, void *priv, const struct ble_ext_handler *h)
{
    handler_registered_hw = hw; handler_registered_priv = priv; handler_registered = h;
}
static void test_upload(void *hw, void (*callback)(void *, struct ble_ext_rx *))
{
    TF("up%d ", ID(hw));
    if (upload_connect) upload_secondary->connect_link = upload_connect;
    if (upload_rx) callback(upload_priv, upload_rx);
}
static const struct ble5_operation operations = {
    .legacy_open = test_legacy_open, .ext_open = test_ext_open, .ext_ioctrl = test_ioctrl,
    .ext_adv_ctrl = test_adv_ctrl, .ext_handler_register = test_register, .ext_upload_data = test_upload,
};

static void reset(void)
{
    ext_le_param.ext_adv_list.head.next = ext_le_param.ext_adv_list.head.prev = &ext_le_param.ext_adv_list.head;
    ext_le_param.ext_adv_list.nums_set = 0;
    ext_ll_hdl.ext_adv_links.next = ext_ll_hdl.ext_adv_links.prev = &ext_ll_hdl.ext_adv_links;
    ext_ll_hdl.ext_periodic_adv_links.next = ext_ll_hdl.ext_periodic_adv_links.prev = &ext_ll_hdl.ext_periodic_adv_links;
    trace[0] = 0;
    errors = infos = resumes = frees = term_count = scan_events = legacy_conns = periodic_updates = 0;
    new_link_fail = open_count = open_fail_at = 0;
    mutate_primary_hw_during_ioctrl = primary_config_hw = primary_start_hw = 0;
    timer_result = 77; timer_removed = 0; timer_msec = 0;
    upload_rx = NULL; upload_connect = NULL;
    config_btctler_le_features = 4096 | 8192;
    config_btctler_le_roles = 2;
    config_vendor_le_bb = 0;
}

static struct __ext_adv_group *add_set(u8 handle, u16 props)
{
    struct __ext_adv_group *s = calloc(1, sizeof(*s));
    s->ext_adv_param.Advertising_Handle = handle;
    s->ext_adv_param.Advertising_Event_Properties = props;
    struct list_head *head = &ext_le_param.ext_adv_list.head;
    s->entry.next = head; s->entry.prev = head->prev;
    head->prev->next = &s->entry; head->prev = &s->entry;
    ext_le_param.ext_adv_list.nums_set++;
    return s;
}
static struct le_ext_primary_link *add_primary(u8 handle)
{
    struct le_ext_primary_link *l = calloc(1, sizeof(*l));
    l->adv_handle = handle;
    struct list_head *head = &ext_ll_hdl.ext_adv_links;
    l->entry.next = head; l->entry.prev = head->prev;
    head->prev->next = &l->entry; head->prev = &l->entry;
    return l;
}
static struct le_ext_secondary_link *add_secondary(struct le_ext_primary_link *p, int hw)
{
    struct le_ext_secondary_link *s = calloc(1, sizeof(*s));
    s->hw = HW(hw); s->adv_handle = p->adv_handle;
    p->secondary_link = s;
    return s;
}
static struct le_ext_periodic_link *add_periodic(u8 handle, int hw)
{
    struct le_ext_periodic_link *l = calloc(1, sizeof(*l));
    l->adv_handle = handle; l->hw = HW(hw);
    struct list_head *head = &ext_ll_hdl.ext_periodic_adv_links;
    l->entry.next = head; l->entry.prev = head->prev;
    head->prev->next = &l->entry; head->prev = &l->entry;
    return l;
}
static int in_list(struct list_head *head, struct list_head *e)
{
    for (struct list_head *p = head->next; p != head; p = p->next) if (p == e) return 1;
    return 0;
}

int main(void)
{
    __ble5_ops = &operations;
    reset();

    /* The SDK's dormant periodic-advertising query always returns zero. */
    CHECK(LctrIsPerAdvEnabled(0) == 0 && LctrIsPerAdvEnabled(0xff) == 0);
    CHECK(ll_dispatch_msg_table[8] == NULL);
    ll_acad_init();
    CHECK(ll_dispatch_msg_table[8] != NULL);
    log_tag_const_i_LL_EXT_ADV = 1;
    infos = 0;
    ll_dispatch_msg_table[8](0, NULL);
    CHECK(infos == 1 && !strcmp(last_log_format, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_CREATE_BIG\r\n"));
    ll_dispatch_msg_table[8](2, NULL);
    CHECK(infos == 2 && !strcmp(last_log_format, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_TERMINATED_BIG\r\n"));
    ll_dispatch_msg_table[8](1, NULL);
    CHECK(infos == 3 && !strcmp(last_log_format, "[Info]: [LL_EXT_ADV]LL_ACAD_MSG_CHANNEL_MAP\r\n"));
    ll_dispatch_msg_table[8](3, NULL);
    CHECK(infos == 3);
    log_tag_const_i_LL_EXT_ADV = 0;

    /* The handler table: only the receive hook is set; it sits in the SDK's const section. */
    CHECK(ll_ext_adv_handler.ext_rx_probe_handler != NULL && ll_ext_adv_handler.ext_event_probe_handler == NULL);

    /* Lookups walk the lists from the first entry; no match gives NULL. */
    struct le_ext_primary_link *p1 = add_primary(1), *p2 = add_primary(2), *p3 = add_primary(3);
    struct le_ext_periodic_link *q5 = add_periodic(5, 0x2005), *q6 = add_periodic(6, 0x2006);
    CHECK(ext_primary_link_for_handle(1) == p1 && ext_primary_link_for_handle(3) == p3);
    CHECK(ext_primary_link_for_handle(4) == NULL && ext_periodic_link_for_handle(5) == q5);
    CHECK(ext_periodic_link_for_handle(6) == q6 && ext_periodic_link_for_handle(1) == NULL);
    struct __ext_adv_group *s1 = add_set(1, 0), *s2 = add_set(2, 0), *s3 = add_set(3, 0);
    CHECK(ll_match_ext_adv_set_addr(2) == s2 && ll_match_ext_adv_set_addr(1) == s1 && ll_match_ext_adv_set_addr(9) == NULL);

    /* Removing a set unlinks it and frees its periodic block (if any) and itself. */
    s2->periodic_info = calloc(1, sizeof(*s2->periodic_info));
    ll_remove_assigned_adv_set(s2);
    CHECK(frees == 2 && ext_le_param.ext_adv_list.nums_set == 2 && ll_match_ext_adv_set_addr(2) == NULL);
    CHECK(ll_match_ext_adv_set_addr(3) == s3);
    ll_remove_assigned_adv_set(s3);
    CHECK(frees == 3 && ext_le_param.ext_adv_list.nums_set == 1);
    /* Removing everything: one free per set plus one per periodic block; the count is reset. */
    struct __ext_adv_group *s4 = add_set(4, 0), *s5 = add_set(5, 0);
    s4->periodic_info = calloc(1, sizeof(*s4->periodic_info));
    (void)s5;
    frees = 0;
    ll_remove_all_adv_set();
    CHECK(frees == 4 && ext_le_param.ext_adv_list.nums_set == 0);
    CHECK(ext_le_param.ext_adv_list.head.next == &ext_le_param.ext_adv_list.head);

    /* Timeout timer: duration is in 10 ms, events are times the primary interval (at least 32 slots of 0.625 ms). */
    reset();
    p1 = add_primary(4);
    struct __ext_adv_group *s = add_set(4, 0);
    s->ext_adv_param.Primary_Advertising_Interval_Min[0] = 0x20;
    s->ext_adv_en.duration = 3; s->ext_adv_en.max_ext_adv_events = 5;
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0);
    CHECK(timer_msec == 30 && p1->adv_timeout == 77 && p1->adv_timeout_type == 2);   /* min(30, 20 * 5) */
    CHECK(timer_context == p1 && timer_context_id == 77 && timer_callback != NULL);
    s->ext_adv_en.duration = 0;                                                       /* 5 events * 20 ms */
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0);
    CHECK(timer_msec == 100 && p1->adv_timeout_type == 4 && timer_removed == 77);     /* the old timer goes first */
    s->ext_adv_en.max_ext_adv_events = 0;                                             /* no limit at all */
    timer_msec = -1; trace[0] = 0;
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0);
    CHECK(timer_msec == -1 && p1->adv_timeout == 0 && p1->adv_timeout_type == 2 && !strcmp(trace, "trem "));
    s->ext_adv_en.duration = 4;                                                       /* duration only */
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0 && timer_msec == 40 && p1->adv_timeout_type == 2);
    s->ext_adv_param.Primary_Advertising_Interval_Min[0] = 16;                        /* raised to 32 slots = 20 ms */
    s->ext_adv_en.duration = 0; s->ext_adv_en.max_ext_adv_events = 1;
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0 && timer_msec == 20 && p1->adv_timeout_type == 4);
    s->ext_adv_param.Primary_Advertising_Interval_Min[0] = 0; s->ext_adv_param.Primary_Advertising_Interval_Min[1] = 1;
    s->ext_adv_en.max_ext_adv_events = 2;                                             /* 256 slots = 160 ms, twice */
    s->ext_adv_en.duration = 40;
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0 && timer_msec == 320 && p1->adv_timeout_type == 4);
    s->ext_adv_en.duration = 32;                                                      /* equal: the duration wins the type */
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 0 && timer_msec == 320 && p1->adv_timeout_type == 2);
    errors = 0;
    CHECK(ll_ext_adv_alive_timer_reset(9, s) == 3 && errors == 1);
    /* A refused timer closes the set and reports status 7. */
    timer_result = 0; trace[0] = 0; errors = 0;
    CHECK(ll_ext_adv_alive_timer_reset(4, s) == 7 && errors == 1);
    CHECK(!strcmp(trace, "trem treg lc0 ") && ext_primary_link_for_handle(4) == NULL);

    /* Closing a set: stop the timer, legacy advertiser, radio links (periodic too), then free. */
    reset();
    p1 = add_primary(7); p1->hw = HW(0x11); p1->adv_timeout = 5; p1->legacy_adv_link = (struct le_adv_link *)1;
    struct le_ext_secondary_link *sec = add_secondary(p1, 0x12);
    sec->periodic_link = add_periodic(7, 0x13);
    CHECK(ll_ext_adv_close(7) == 0);
    CHECK(!strcmp(trace, "trem advclose c18_6(0) c19_6(0) lc17 lc18 ") && ext_primary_link_for_handle(7) == NULL);
    errors = 0; trace[0] = 0;
    CHECK(ll_ext_adv_close(7) == 0 && errors == 1 && !trace[0]);
    p2 = add_primary(8); p2->hw = HW(0x21);
    trace[0] = 0;
    CHECK(ll_ext_adv_close(8) == 0 && !strcmp(trace, "lc33 "));

    /* Opening a connectable undirected set: primary then secondary radio setup, in the SDK's order. */
    reset();
    s = add_set(7, 1);
    s->ext_adv_param.Primary_Advertising_Channel_Map = 7; s->ext_adv_param.Advertising_Filter_Policy = 1;
    s->ext_adv_param.Advertising_SID = 0x1b; s->ext_adv_param.Primary_Advertising_PHY = 1;
    s->ext_adv_param.Secondary_Advertising_PHY = 2; s->ext_adv_param.Secondary_Advertising_Max_Skip = 3;
    s->ext_adv_param.Primary_Advertising_Interval_Min[0] = 0x30; s->ext_adv_param.Peer_Address_Type = 1;
    for (int i = 0; i < 6; i++) s->ext_adv_param.Peer_Address[i] = 0x50 + i;
    s->ext_adv_data.cur_len = 4;
    ll_set_code_type = 5; random_pattern = 0x1234;
    CHECK(ll_ext_adv_open(7, s) == 0 && new_link_len == 16);
    CHECK(!strcmp(trace, "io4097_9(0,a0) c4097_0 c4097_6(4098) c4098_6(4097) io4098_9(0,a0) c4098_1 c4097_3(1) c4098_4(1) "));
    CHECK(ctrl0.event_type == 5 && ctrl0.pkt_cnt == 3 && ctrl0.channel_map == 7 && ctrl0.filter_policy == 1);
    CHECK(ctrl0.sid == 0x0b && ctrl0.did == 0x234 && ctrl0.interval == 0x30 && ctrl0.pdu_interval == 1250);
    CHECK(ctrl0.primary_phy == 0 && ctrl0.secondary_phy == 1 && ctrl0.coded_s == 5 && ctrl0.secondary_max_skip == 3);
    CHECK(ctrl0.is_need_aux == 1 && ctrl0.channel_index == 11 && ctrl0.adv_data_len == 0 && ctrl0.adv_data == NULL);
    CHECK(rand_arg0 == 0x34 && rand_arg1 == 0 && rand_arg2 == 36);
    CHECK(ctrl1.adv_data_len == 4 && ctrl1.adv_data == s->ext_adv_data.data);
    p1 = ext_primary_link_for_handle(7);
    CHECK(p1 && p1->hw == HW(0x1001) && p1->secondary_link && p1->secondary_link->hw == HW(0x1002));
    CHECK(p1->secondary_link->adv_handle == 7 && p1->adv_timeout_type == 2 && p1->adv_timeout == 0);
    CHECK(handler_registered == &ll_ext_adv_handler && handler_registered_hw == HW(0x1002) && handler_registered_priv == p1->secondary_link);
    /* Directed: the secondary radio also gets the peer address (command 10); the PDU type is 6. */
    reset(); s = add_set(7, 5);
    s->ext_adv_param.Peer_Address_Type = 1;
    for (int i = 0; i < 6; i++) s->ext_adv_param.Peer_Address[i] = 0x50 + i;
    CHECK(ll_ext_adv_open(7, s) == 0 && ctrl0.event_type == 6);
    CHECK(strstr(trace, "io4098_9(0,a0) io4098_10(1,50) c4098_1 ") != NULL && strstr(trace, "pb6 ") == NULL);
    infos = 0; log_tag_const_i_LL_EXT_ADV = 1;
    reset(); s = add_set(7, 5);
    CHECK(ll_ext_adv_open(7, s) == 0 && strstr(trace, "pb6 ") != NULL && infos > 10);
    log_tag_const_i_LL_EXT_ADV = 0;
    /* Event type table. */
    static const struct { u16 props; u8 type; } types[] = { {1, 5}, {5, 6}, {2, 7}, {6, 8} };
    for (unsigned i = 0; i < 4; i++) {
        reset(); s = add_set(1, types[i].props);
        CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == types[i].type && ctrl0.is_need_aux == 1);
    }
    /* Non-connectable, non-scannable: no data and no periodic info needs no secondary PDU. */
    reset(); s = add_set(1, 0);
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == 1 && ctrl0.is_need_aux == 0);
    CHECK(!strcmp(trace, "io4097_9(0,a0) c4097_0 c4097_3(1) ") && ext_primary_link_for_handle(1)->secondary_link == NULL);
    reset(); s = add_set(1, 0); s->ext_adv_data.cur_len = 1;
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == 2 && ctrl0.is_need_aux == 1);
    reset(); s = add_set(1, 0); s->periodic_info = calloc(1, sizeof(*s->periodic_info));
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == 2 && ctrl0.is_need_aux == 1);
    /* The SDK reuses the primary HW pointer across these two callbacks, then
     * reloads link->hw before its later start command. */
    reset(); s = add_set(1, 0); mutate_primary_hw_during_ioctrl = 1;
    CHECK(ll_ext_adv_open(1, s) == 0);
#ifdef CONFIG_CPU_BD19
    CHECK(primary_config_hw == ID(HW(0x1001)) && primary_start_hw == ID(HW(0x1bad)));
#else
    CHECK(primary_config_hw == ID(HW(0x1bad)) && primary_start_hw == ID(HW(0x1bad)));
#endif
    reset(); s = add_set(1, 4);
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == 3 && ctrl0.is_need_aux == 0);
    reset(); s = add_set(1, 4); s->ext_adv_data.cur_len = 1;
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.event_type == 4 && ctrl0.is_need_aux == 1);
    /* An unknown property combination logs an error, leaves the type at zero and still wants a secondary. */
    reset(); errors = 0; s = add_set(1, 9);
    CHECK(ll_ext_adv_open(1, s) == 0 && errors == 1 && ctrl0.event_type == 0 && ctrl0.is_need_aux == 1);
    /* Coded PHY as primary: the PDU interval is 1364 us; channel-map bits above 2 are not counted. */
    reset(); s = add_set(1, 1);
    s->ext_adv_param.Primary_Advertising_PHY = 3; s->ext_adv_param.Primary_Advertising_Channel_Map = 0x25;
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.primary_phy == 2 && ctrl0.pdu_interval == 1364 && ctrl0.pkt_cnt == 2);
    /* A drawn data ID of zero equals the zeroed previous one; the SDK still stores the unincremented value. */
    reset(); s = add_set(1, 1); random_pattern = 0x5000;
    CHECK(ll_ext_adv_open(1, s) == 0 && ctrl0.did == 0);
    random_pattern = 0x1234;
    /* With a periodic link: tie the links together, sync info, access address on first use. */
    reset(); s = add_set(7, 1);
    s->periodic_info = calloc(1, sizeof(*s->periodic_info));
    s->periodic_info->Periodic_Advertising_Interval_Min = 3;
    q5 = add_periodic(7, 0x2007);
    CHECK(ll_ext_adv_open(7, s) == 0);
    CHECK(!strcmp(trace, "io4097_9(0,a0) c4097_0 c4097_6(4098) c4098_6(4097) c8199_6(4098) c4098_7(8199) aa io4098_9(0,a0) c4098_1 c4097_3(1) c4098_4(1) c8199_5(1) "));
    CHECK(ctrl1.sync_info.interval == 6 && ctrl1.sync_info.aa == 0x12345678 && q5->sync_hw_info.aa == 0x12345678);
    CHECK(!memcmp(ctrl1.sync_info.chm, "\xff\xff\xff\xff\x1f", 5) && !memcmp(ctrl1.sync_info.crcinit, "\xae\x83\x19", 3));
    CHECK(ext_primary_link_for_handle(7)->secondary_link->periodic_link == q5);
    ll_ext_adv_close(7);
    trace[0] = 0; config_vendor_le_bb = 1; q5->sync_hw_info.aa = 0xdeadbeef;     /* vendor bit 0: no minimum, aa kept */
    CHECK(ll_ext_adv_open(7, s) == 0 && ctrl1.sync_info.interval == 3 && ctrl1.sync_info.aa == 0xdeadbeef && strstr(trace, "aa ") == NULL);
    /* Failures. */
    reset(); s = add_set(1, 1); new_link_fail = 1; errors = 0;
    CHECK(ll_ext_adv_open(1, s) == 7 && errors == 1 && ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links);
    new_link_fail = 0; open_fail_at = 1; errors = 0; frees = 0;
    CHECK(ll_ext_adv_open(1, s) == 7 && errors == 1 && frees == 1 && ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links);
    open_count = 0; open_fail_at = 2; errors = 0; frees = 0; trace[0] = 0;       /* secondary radio fails: primary stays */
    CHECK(ll_ext_adv_open(1, s) == 7 && errors == 1 && frees == 1 && ext_ll_hdl.ext_adv_links.next != &ext_ll_hdl.ext_adv_links);
    CHECK(strstr(trace, "c4097_3") == NULL);
    reset(); s = add_set(1, 1); s->ext_adv_en.duration = 1; timer_result = 0;     /* timer refused: the set is closed again */
    CHECK(ll_ext_adv_open(1, s) == 7 && ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links);
    CHECK(strstr(trace, "treg c4098_6(0) lc4097 lc4098 ") != NULL && strstr(trace, "c4097_3") == NULL);
    reset(); s = add_set(1, 1); s->ext_adv_en.duration = 1;                       /* timer armed: 10 ms, context is the primary */
    CHECK(ll_ext_adv_open(1, s) == 0 && timer_msec == 10 && timer_context == ext_primary_link_for_handle(1));

    /* Data update: unknown set gives 3; otherwise rebuild and send command 8 with the new data. */
    reset(); s = add_set(1, 1); s->ext_adv_data.cur_len = 4;
    CHECK(ll_ext_adv_data_update(1, s) == 3 && trace[0] == 0);
    CHECK(ll_ext_adv_open(1, s) == 0);
    trace[0] = 0; s->ext_adv_data.cur_len = 9;
    CHECK(ll_ext_adv_data_update(1, s) == 0 && !strcmp(trace, "c4097_8 ") && sync_adv_seen && periodic_updates == 0);
    ext_primary_link_for_handle(1)->secondary_link->periodic_link = q5;
    s->periodic_info = calloc(1, sizeof(*s->periodic_info)); s->periodic_info->Periodic_Advertising_Interval_Min = 9;
    CHECK(ll_ext_adv_data_update(1, s) == 0 && periodic_updates == 1);

    /* Legacy-PDU sets: the PDU type from the properties without bit 4. */
    static const struct { u16 props; u8 type; int err; } legacy[] = {
        {0x13, 0, 0}, {0x15, 4, 0}, {0x1d, 1, 0}, {0x12, 2, 0}, {0x10, 3, 0}, {0x03, 0, 0}, {0x20, 0, 1}, {0x11, 0, 1},
    };
    for (unsigned i = 0; i < 8; i++) {
        reset(); errors = 0; s = add_set(3, legacy[i].props);
        s->ext_adv_param.Primary_Advertising_Interval_Min[0] = 0x40; s->ext_adv_param.Primary_Advertising_Interval_Min[1] = 0x06;
        s->ext_adv_param.Primary_Advertising_Interval_Max[0] = 0x80; s->ext_adv_param.Primary_Advertising_Interval_Max[1] = 0x0c;
        s->ext_adv_param.Own_Address_Type = 1; s->ext_adv_param.Peer_Address_Type = 1;
        for (int j = 0; j < 6; j++) s->ext_adv_param.Peer_Address[j] = 0x60 + j;
        s->ext_adv_param.Primary_Advertising_Channel_Map = 5; s->ext_adv_param.Advertising_Filter_Policy = 2;
        s->ext_adv_data.cur_len = 3; memcpy(s->ext_adv_data.data, "\x02\x01\x06", 3);
        adv_open_result = 9; adv_link_result = (struct le_adv_link *)0x1234;
        memset(adv_params, 0xee, 15);
        CHECK(ll_ext_legacy_adv_open(3, s) == 9 && new_link_len == 28 && errors == legacy[i].err);
        CHECK(adv_params[0] == 0x40 && adv_params[1] == 0x06 && adv_params[2] == 0x80 && adv_params[3] == 0x0c);
        CHECK(adv_params[4] == legacy[i].type && adv_params[5] == 1 && adv_params[6] == 1 && adv_params[7] == 0x60 && adv_params[12] == 0x65);
        CHECK(adv_params[13] == 5 && adv_params[14] == 2);
        CHECK(adv_data_pdu[0] == 3 && !memcmp(&adv_data_pdu[1], "\x02\x01\x06", 3) && scan_rsp_pdu[0] == 3 && !memcmp(&scan_rsp_pdu[1], "\x02\x01\x06", 3));
        p1 = ext_primary_link_for_handle(3);
        CHECK(p1 && p1->legacy_adv_link == (struct le_adv_link *)0x1234 && p1->secondary_link == NULL && !strcmp(trace, "advopen "));
    }
    reset(); new_link_fail = 1; errors = 0; s = add_set(3, 0x13);
    CHECK(ll_ext_legacy_adv_open(3, s) == 7 && errors == 1 && trace[0] == 0);

    /* Receive post: terminated sets are reported and closed; the rest are polled. */
    reset(); config_btctler_le_roles = 1;
    p1 = add_primary(1); add_secondary(p1, 0x31); add_set(1, 0);
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(trace[0] == 0);                                                          /* advertiser role gate */
    config_btctler_le_roles = 2;
    p2 = add_primary(2);                                                           /* no secondary link: skipped */
    p3 = add_primary(3); add_secondary(p3, 0x33); add_set(3, 0);
    p3->adv_timeout_type = 8 | 2;
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(!strcmp(trace, "up49 io49_11 up51 term3 c51_6(0) lc0 lc51 ") && empty_seen == (u8 *)0x99);
    CHECK(term_status == 60 && term_conn == 0 && term_handle == 3 && ext_primary_link_for_handle(3) == NULL);
    CHECK(ext_le_param.ext_adv_list.nums_set == 1 && ll_match_ext_adv_set_addr(3) == NULL && ll_match_ext_adv_set_addr(1) != NULL);
    p1->adv_timeout_type = 8; trace[0] = 0;                                        /* maximum events reached: status 0x43 */
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(term_status == 67 && term_handle == 1 && term_conn == 0 && ext_primary_link_for_handle(1) == NULL && ext_primary_link_for_handle(2) == p2);
    /* A connection that appears during the upload ends the set with its handle; the link is read after upload. */
    reset(); struct le_link conn; memset(&conn, 0, sizeof(conn)); conn.handle = 0x42;
    p1 = add_primary(1); sec = add_secondary(p1, 0x31); add_set(1, 0); p1->legacy_adv_link = (struct le_adv_link *)1;
    upload_connect = &conn; upload_secondary = sec; p1->adv_timeout_type = 8;
    log_tag_const_i_LL_EXT_ADV = 1; infos = 0;
    ll_ext_adv_rx_post_process((int *)0x99);
    log_tag_const_i_LL_EXT_ADV = 0;
    CHECK(term_status == 0 && term_conn == 0x42 && term_handle == 1 && infos == 3 && strstr(trace, "advclose") == NULL);
    CHECK(ext_primary_link_for_handle(1) == NULL);
    /* Scan request and connect request PDUs delivered during the upload. */
    reset(); p1 = add_primary(6); sec = add_secondary(p1, 0x31);
    s = add_set(6, 0); s->ext_adv_param.Scan_Request_Notification_Enable = 1;
    u8 buf[sizeof(struct ble_ext_rx) + 16]; struct ble_ext_rx *rx = (struct ble_ext_rx *)buf;
    memset(buf, 0, sizeof(buf)); rx->type = 3; rx->rxadd = 1;
    upload_rx = rx; upload_priv = sec; created_link = &conn;
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(scan_events == 1 && scan_event_handle == 6 && scan_event_rxadd == 1 && scan_event_data == rx->data);
    s->ext_adv_param.Scan_Request_Notification_Enable = 0;
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(scan_events == 1);
    rx->type = 5; legacy_conns = 0;
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(legacy_conns == 1);
    rx->type = 7; legacy_conns = 0; scan_events = 0;                               /* other PDU types are ignored */
    ll_ext_adv_rx_post_process((int *)0x99);
    CHECK(legacy_conns == 0 && scan_events == 0);

    /* Legacy connection exit: only the first set is ended, whatever connection came. */
    reset(); upload_rx = NULL;
    p1 = add_primary(1); p1->legacy_adv_link = (struct le_adv_link *)0x5; add_set(1, 0);
    p2 = add_primary(2); p2->legacy_adv_link = (struct le_adv_link *)0x6; add_set(2, 0);
    config_btctler_le_features = 0;
    ll_exit_ext_legacy_adv_status(0x33);
    CHECK(term_count == 0 && trace[0] == 0);                                       /* feature bit 12 gate */
    config_btctler_le_features = 4096;
    ll_exit_ext_legacy_adv_status(0x33);
    CHECK(term_count == 1 && term_handle == 1 && term_status == 0 && term_conn == 0x33);
    CHECK(!strcmp(trace, "term1 lc0 ") && ext_primary_link_for_handle(2) == p2 && p2->legacy_adv_link != NULL);
    CHECK(ll_match_ext_adv_set_addr(1) == NULL && ll_match_ext_adv_set_addr(2) != NULL);
    ll_exit_ext_legacy_adv_status(0x34);
    CHECK(term_count == 2 && term_handle == 2 && term_conn == 0x34 && ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links);
    ll_exit_ext_legacy_adv_status(0x35);
    CHECK(term_count == 2);                                                        /* empty list */

    /* Scan request seen by the legacy advertiser: reported for the set that owns the link. */
    reset(); memset(buf, 0, sizeof(buf)); rx->rxadd = 1;
    p1 = add_primary(5); p1->legacy_adv_link = (struct le_adv_link *)0x71; add_secondary(p1, 0x41);
    p2 = add_primary(6); p2->legacy_adv_link = (struct le_adv_link *)0x72; add_secondary(p2, 0x42);
    s1 = add_set(5, 0); s2 = add_set(6, 0); s2->ext_adv_param.Scan_Request_Notification_Enable = 1;
    config_btctler_le_features = 0;
    ll_ext_legacy_adv_scan_req_received((struct le_link *)0x72, (struct ble_rx *)rx);
    CHECK(scan_events == 0);
    config_btctler_le_features = 4096;
    ll_ext_legacy_adv_scan_req_received((struct le_link *)0x71, (struct ble_rx *)rx);
    CHECK(scan_events == 0);                                                       /* set 5 did not ask for notifications */
    ll_ext_legacy_adv_scan_req_received((struct le_link *)0x99, (struct ble_rx *)rx);
    CHECK(scan_events == 0);
    {
        struct ble_rx *br = (struct ble_rx *)buf;
        memset(buf, 0, sizeof(buf));
        br->rxadd = 1;
        ll_ext_legacy_adv_scan_req_received((struct le_link *)0x72, br);
        CHECK(scan_events == 1 && scan_event_handle == 6 && scan_event_rxadd == 1 && scan_event_data == br->data);
        br->rxadd = 0;
        ll_ext_legacy_adv_scan_req_received((struct le_link *)0x72, br);
        CHECK(scan_events == 2 && scan_event_rxadd == 0);
    }

    /* Probe handler: a connect request starts the legacy connection on the secondary radio. */
    reset(); memset(buf, 0, sizeof(buf));
    sec = calloc(1, sizeof(*sec)); sec->hw = HW(0x55);
    rx->type = 5; rx->rxadd = 1; rx->txadd = 1; rx->len = 0;
    for (int i = 0; i < 16; i++) rx->data[i] = (u8)(0x10 + i);
    created_link = &conn; config_btctler_le_roles = 1;
    ll_ext_adv_handler.ext_rx_probe_handler(sec, rx);
    CHECK(legacy_open_hw == 0 && resumes == 0 && sec->connect_link == NULL);       /* role gate */
    config_btctler_le_roles = 2;
    ll_ext_adv_handler.ext_rx_probe_handler(sec, rx);
    CHECK(legacy_open_hw == 0x55 && ioctrl15_hw == 0x55 && sec->connect_link == &conn && resumes == 0);
    CHECK(created_local.addr_type == 1 && !memcmp(created_local.addr, &rx->data[6], 6) && created_role == 1 && created_txadd == 1);
    CHECK(created_peer == rx->data && created_data == &rx->data[12]);
    rx->len = 1; rx->type = 1; sec->connect_link = NULL;
    ll_ext_adv_handler.ext_rx_probe_handler(sec, rx);
    CHECK(sec->connect_link == NULL && resumes == 1);
    rx->len = 0; ll_ext_adv_handler.ext_rx_probe_handler(sec, rx);
    CHECK(resumes == 1);

    /* Timeout callback: marks the link (keeping other bits) and wakes the controller thread. */
    reset(); errors = 0;
    p1 = add_primary(1); p1->adv_timeout_type = 2;
    timer_context = p1; timer_context_id = 77;
    add_set(1, 0);
    p1->adv_timeout = 77; timer_result = 77;
    s = ll_match_ext_adv_set_addr(1); s->ext_adv_en.duration = 1;
    ll_ext_adv_alive_timer_reset(1, s);
    timer_callback(HW(77));
    CHECK(p1->adv_timeout_type == (2 | 8) && resumes == 1 && errors == 1);

    /* Destroy: all sets freed, every primary closed, periodic advertising closed when feature bit 13 is set. */
    reset(); add_set(1, 0); add_set(2, 0);
    p1 = add_primary(1); p1->hw = HW(0x61); p2 = add_primary(2); p2->hw = HW(0x62);
    le_ext_adv_destroy();
    CHECK(!strcmp(trace, "lc97 pclose1 lc98 pclose2 ") && frees == 2 && ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links);
    reset(); p1 = add_primary(1); p1->hw = HW(0x61); config_btctler_le_features = 4096;
    le_ext_adv_destroy();
    CHECK(!strcmp(trace, "lc97 "));
    (void)in_list; (void)q6;
    return TEST_DONE("ll_ext_adv");
}
#endif
