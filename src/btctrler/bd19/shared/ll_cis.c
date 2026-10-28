/* LE Connected Isochronous Stream (CIG/CIS) bookkeeping and lifecycle.  SDK
 * quirks kept:
 * ll_cig_free returns at once when the group's CIS list is empty (it only
 * frees a group that still has streams), so ll_cis_destory never frees the
 * group of its last stream; ll_cis_by_id searches the shared stream list by
 * stream id alone before it looks at the group; CIS handles are 96 + a bit
 * index of a six-bit map and exhaustion asserts then still returns 102;
 * interleaved duration dereferences the result of ll_cis_by_id unchecked. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS, log_tag_const_i_LL_CIS;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int printf(const char *format, ...);
#include <string.h>
#ifdef UNITTEST
#include <stdlib.h>
void *ll_cis_test_malloc(u32 size);
void ll_cis_test_free(void *ptr);
#define malloc ll_cis_test_malloc
#define free ll_cis_test_free
#else
extern void *malloc(u32 size);
extern void free(void *ptr);
#endif

#define LL_CODE __attribute__((section(".ble_ll_code")))
#ifdef CONFIG_CPU_BD19
#define CIS_MEMSET __builtin_memset
/* The SDK event emitter returns a byte; the recovered header says bool. */
extern u8 cis_emit_meta_event(u8, const char *, ...) __asm__("__hci_emit_le_meta_event");
#define CIS_EMIT_META_EVENT cis_emit_meta_event
#else
#define CIS_MEMSET memset
#define CIS_EMIT_META_EVENT __hci_emit_le_meta_event
#endif
/* Message handles are aligned to two bytes and may alias packet storage. */
struct cis_message { u16 handle; u8 status; } __attribute__((may_alias));

#ifndef UNITTEST
_Static_assert(sizeof(le_cis_t) == 356, "SDK CIS allocation size");
_Static_assert(sizeof(le_cig_t) == 56, "SDK CIG allocation size");
_Static_assert(offsetof(le_cis_t, cig) == 12, "SDK CIS cig offset");
_Static_assert(offsetof(le_cis_t, cisHandle) == 22, "SDK CIS handle offset");
_Static_assert(offsetof(le_cis_t, cisId) == 25, "SDK CIS id offset");
_Static_assert(offsetof(le_cis_t, delayUsec) == 184, "SDK CIS delay offset");
_Static_assert(offsetof(le_cis_t, phyMToS) == 196, "SDK CIS phy offset");
_Static_assert(offsetof(le_cis_t, nse) == 224, "SDK CIS nse offset");
_Static_assert(offsetof(le_cis_t, subIntervUsec) == 228, "SDK CIS sub interval offset");
_Static_assert(offsetof(le_cis_t, transLatUsec) == 236, "SDK CIS trans latency offset");
_Static_assert(offsetof(le_cis_t, dataPathInCtx) == 244, "SDK CIS in data path offset");
_Static_assert(offsetof(le_cis_t, dataPathOutCtx) == 248, "SDK CIS out data path offset");
_Static_assert(offsetof(le_cig_t, cis_head) == 8, "SDK CIG cis list offset");
_Static_assert(offsetof(le_cig_t, cigId) == 21, "SDK CIG id offset");
_Static_assert(offsetof(le_cig_t, cis_number) == 16, "SDK CIG count offset");
_Static_assert(offsetof(le_cig_t, packing) == 17, "SDK CIG packing offset");
_Static_assert(offsetof(le_cig_t, headCisRmved) == 33, "SDK CIG head-removed offset");
_Static_assert(offsetof(le_cig_t, offsetUsec) == 36, "SDK CIG offset field");
#endif
_Static_assert(offsetof(le_cis_t, entry) == 0, "SDK CIS list entry offset");
_Static_assert(offsetof(le_cig_t, entry) == 0, "SDK CIG list entry offset");

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static void list_add_tail(struct list_head *_new, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = _new;
    _new->next = head;
    _new->prev = prev;
    prev->next = _new;
}

static void list_add(struct list_head *_new, struct list_head *head)
{
    struct list_head *next = head->next;
    next->prev = _new;
    _new->next = next;
    _new->prev = head;
    head->next = _new;
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

static int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static int list_is_head(const struct list_head *head, const struct list_head *member)
{
    return head->next == member;
}

le_cig_t *ll_cig_by_id(u8 id) LL_CODE;
le_cig_t *ll_cig_by_id(u8 id)
{
    for (struct list_head *node = ll_iso_hdl.cig_head.next; node != &ll_iso_hdl.cig_head; node = node->next) {
        le_cig_t *cig = (le_cig_t *)node;
        if (cig->cigId == id) return cig;
    }
    return NULL;
}

le_cig_t *ll_cig_alloc(u8 id) LL_CODE;
le_cig_t *ll_cig_alloc(u8 id)
{
    le_cig_t *cig = malloc(56);
    if (!cig) return NULL;
    CIS_MEMSET(cig, 0, 56);
    cig->cigId = id;
    INIT_LIST_HEAD(&cig->cis_head);
    list_add_tail(&cig->entry, &ll_iso_hdl.cig_head);
    return cig;
}

void ll_cis_free(le_cis_t *cis) LL_CODE;
void ll_cis_free(le_cis_t *cis)
{
    ll_iso_hdl.cis_number = (u8)(ll_iso_hdl.cis_number - 1);
    list_del(&cis->entry);
    free(cis);
}

void ll_cig_free(le_cig_t *cig) LL_CODE;
void ll_cig_free(le_cig_t *cig)
{
    struct list_head *head = &cig->cis_head;
    if (list_empty(head)) return;
    struct list_head *node = head->next;
    while (node != head) {
        struct list_head *next = node->next;
        ll_cis_free((le_cis_t *)node);
        node = next;
    }
    list_del(&cig->entry);
    free(cig);
}

void ll_cig_add_cis(le_cig_t *cig, le_cis_t *cis) LL_CODE;
void ll_cig_add_cis(le_cig_t *cig, le_cis_t *cis)
{
    list_del(&cis->entry);
    cig->cis_number = (u8)(cig->cis_number + 1);
    list_add(&cis->entry, &cig->cis_head);
}

void ll_cig_add_cis_tail(le_cig_t *cig, le_cis_t *cis) LL_CODE;
void ll_cig_add_cis_tail(le_cig_t *cig, le_cis_t *cis)
{
    list_del(&cis->entry);
    cig->cis_number = (u8)(cig->cis_number + 1);
    list_add_tail(&cis->entry, &cig->cis_head);
}

le_cis_t *ll_cis_by_handle(u16 handle) LL_CODE;
le_cis_t *ll_cis_by_handle(u16 handle)
{
    if (!list_empty(&ll_iso_hdl.cis_head)) {
        for (struct list_head *node = ll_iso_hdl.cis_head.next; node != &ll_iso_hdl.cis_head; node = node->next) {
            le_cis_t *cis = (le_cis_t *)node;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]CIS by handle 0x%x\r\n", (int)cis->cisHandle);
            if (cis->cisHandle == handle) return cis;
        }
    }
    if (list_empty(&ll_iso_hdl.cig_head)) return NULL;
    for (struct list_head *node = ll_iso_hdl.cig_head.next; node != &ll_iso_hdl.cig_head; node = node->next) {
        le_cig_t *cig = (le_cig_t *)node;
        for (struct list_head *inner = cig->cis_head.next; inner != &cig->cis_head; inner = inner->next) {
            le_cis_t *cis = (le_cis_t *)inner;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]CIG CIS by handle 0x%x\r\n", (int)cis->cisHandle);
            if (cis->cisHandle == handle) return cis;
        }
    }
    return NULL;
}

uint16_t ll_cis_handle_alloc(void) LL_CODE;
uint16_t ll_cis_handle_alloc(void)
{
    u8 bitmap = ll_iso_hdl.cis_bitmap;
    int i;
    for (i = 0; i < 6; i++) {
        u32 bit = 1u << i;
        if ((bitmap & bit) == 0) {
            ll_iso_hdl.cis_bitmap = (u8)(bitmap | bit);
            break;
        }
    }
    if (i >= 6) btctrler_assert_failed("i < LL_MAX_CIS alloc_cis_handle error", 0);
    return (uint16_t)(i + 96);
}

le_cis_t *ll_cis_alloc(le_cig_t *cig) LL_CODE;
le_cis_t *ll_cis_alloc(le_cig_t *cig)
{
    le_cis_t *cis = malloc(356);
    if (!cis) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]CIS No memory\r\n");
        return NULL;
    }
    CIS_MEMSET(cis, 0, 356);
    uint16_t handle = ll_cis_handle_alloc();
    cis->cisHandle = handle;
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]CIS alloc handle : 0x%x\r\n", (int)handle);
    cis->dataPathInCtx.id = (LlIsoDataPath_t)255;
    cis->dataPathOutCtx.id = (LlIsoDataPath_t)255;
    cis->transLatUsec = 234;
    cis->cig = cig;
    ll_iso_hdl.cis_number = (u8)(ll_iso_hdl.cis_number + 1);
    list_add_tail(&cis->entry, &ll_iso_hdl.cis_head);
    return cis;
}

le_cis_t *ll_cis_by_id(uint8_t cigId, uint8_t cisId) LL_CODE;
le_cis_t *ll_cis_by_id(uint8_t cigId, uint8_t cisId)
{
    if (!list_empty(&ll_iso_hdl.cis_head)) {
        for (struct list_head *node = ll_iso_hdl.cis_head.next; node != &ll_iso_hdl.cis_head; node = node->next) {
            le_cis_t *cis = (le_cis_t *)node;
            if (cis->cisId == cisId) return cis;
        }
    }
    if (list_empty(&ll_iso_hdl.cig_head)) return NULL;
    le_cig_t *cig = ll_cig_by_id(cigId);
    if (!cig) return NULL;
    for (struct list_head *node = cig->cis_head.next; node != &cig->cis_head; node = node->next) {
        le_cis_t *cis = (le_cis_t *)node;
        if (cis->cisId == cisId) return cis;
    }
    return NULL;
}

uint8_t lctrCisIsHeadCis(const struct list_head *head, le_cis_t *cis) LL_CODE;
uint8_t lctrCisIsHeadCis(const struct list_head *head, le_cis_t *cis)
{
    if (list_empty(head)) return 0;
    return (uint8_t)list_is_head(head, &cis->entry);
}

void ll_cis_destory(le_cis_t *cis) LL_CODE;
void ll_cis_destory(le_cis_t *cis)
{
    le_cig_t *cig = cis->cig;
    if (!cig) btctrler_assert_failed("cig", 0);
    struct list_head *cis_head = &cig->cis_head;
    if (lctrCisIsHeadCis(cis_head, cis) == 1) {
        cig->headCisRmved = 1;
        u32 offset;
        if (cig->packing == 1)
            offset = cis->delayUsec;
        else
            offset = (u32)cis->nse * cis->subIntervUsec;
        cig->offsetUsec = offset;
    }
    ll_cis_free(cis);
    if (list_empty(cis_head)) ll_cig_free(cig);
}

uint8_t ll_cis_is_established(le_cis_t *cis) LL_CODE;
uint8_t ll_cis_is_established(le_cis_t *cis)
{
    return cis->state == 1;
}

uint8_t ll_get_number_enable_cis(LlCisCigParams_t *param) LL_CODE;
uint8_t ll_get_number_enable_cis(LlCisCigParams_t *param)
{
    u8 count = 0;
    for (u32 i = 0; i != param->numCis; i++) {
        le_cis_t *cis = ll_cis_by_id(param->cigId, param->pCisParam[i].cisId);
        count = (u8)(count + (cis != NULL));
    }
    return count;
}

uint8_t ll_get_number_avail_cis(void) LL_CODE;
uint8_t ll_get_number_avail_cis(void)
{
    return (uint8_t)(ll_iso_hdl.max_cis_number - ll_iso_hdl.cis_number);
}

uint32_t ll_cis_calc_subevt_sequential_us(uint8_t phyMToS, uint8_t phySToM, uint8_t plMToS, uint8_t plSToM) LL_CODE;
uint32_t ll_cis_calc_subevt_sequential_us(uint8_t phyMToS, uint8_t phySToM, uint8_t plMToS, uint8_t plSToM)
{
    u32 duration;
    switch (phyMToS) {
    case 1: duration = ((u32)plMToS << 3) + 112; break;
    case 2: duration = ((u32)plMToS << 2) + 60; break;
    case 4: duration = ((u32)plMToS << 6) + 976; break;
    default: duration = 0; break;
    }
    duration += 150;
    switch (phySToM) {
    case 1: duration += ((u32)plSToM << 3) + 112; break;
    case 2: duration += ((u32)plSToM << 2) + 60; break;
    case 4: duration += ((u32)plSToM << 6) + 976; break;
    default: break;
    }
    return duration + 150 + ll_iso_hdl.cisSubEvtSpaceDelay;
}

uint32_t ll_cis_calc_subevt_interleaved_us(LlCisCigParams_t *pSetCigParam) LL_CODE;
uint32_t ll_cis_calc_subevt_interleaved_us(LlCisCigParams_t *pSetCigParam)
{
    u32 count = pSetCigParam->numCis;
    u32 duration = 0;
    for (u32 i = 0; i != count; i++) {
        le_cis_t *cis = ll_cis_by_id(pSetCigParam->cigId, pSetCigParam->pCisParam[i].cisId);
        duration += ll_cis_calc_subevt_sequential_us(cis->phyMToS, cis->phySToM, (u8)cis->pduSizeMToS,
                                                     (u8)cis->pduSizeSToM);
    }
    return duration;
}

void ll_cig_remove_cis(le_cig_t *cig, le_cis_t *cis) LL_CODE;
void ll_cig_remove_cis(le_cig_t *cig, le_cis_t *cis)
{
    if (list_empty(&cig->cis_head)) return;
    cig->cis_number = (u8)(cig->cis_number - 1);
    list_del(&cis->entry);
}

le_cis_t *ll_cig_get_head_cis(le_cig_t *cig) LL_CODE;
le_cis_t *ll_cig_get_head_cis(le_cig_t *cig)
{
    if (list_empty(&cig->cis_head)) return NULL;
    return (le_cis_t *)cig->cis_head.next;
}

static inline void cpu_reset(void)
{
    p33_soft_reset();
}

void ll_cis_established(le_cis_t *cis) LL_CODE;
void ll_cis_established(le_cis_t *cis)
{
    le_cig_t *cig = cis->cig;
    if (!cig) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_cis.c", 361);
            printf("ASSERT-FAILD: cig ");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    (void)ll_link_for_handle(cis->aclHandle);
    cig->numCisEsted = (u8)(cig->numCisEsted + 1);
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]established : cig->numCisEsted=%u\r\n", (int)cig->numCisEsted);
}

void ll_cis_established_fail(le_cis_t *cis) LL_CODE;
void ll_cis_established_fail(le_cis_t *cis)
{
    ll_cis_destory(cis);
}

uint8_t ll_get_number_enable_cis_test(LlCisCigParamsTest_t *param) LL_CODE;
uint8_t ll_get_number_enable_cis_test(LlCisCigParamsTest_t *param)
{
    u8 count = 0;
    for (u32 i = 0; i < param->numCis; i++)
        count = (u8)(count + (ll_cis_by_id(param->cigId, param->pCisParam[i].cisId) != NULL));
    return count;
}

void ll_setup_channel_param(le_cis_t *cis, uint64_t chanMask) LL_CODE;
void ll_setup_channel_param(le_cis_t *cis, uint64_t chanMask)
{
    (void)cis;
    (void)chanMask;
}

uint8_t ll_cis_tx_test(le_cis_t *cis, u8 payload_type) LL_CODE;
uint8_t ll_cis_tx_test(le_cis_t *cis, u8 payload_type)
{
    if ((!cis->bnMToS && cis->role == 0) || (!cis->bnSToM && cis->role == 1)) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL, "[Debug]: [LL_CIS]Transmit burst number must be greater than 0\r\n");
        return 17;
    }
    if (payload_type > 2) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL, "[Debug]: [LL_CIS]Unknown payload type=%u\r\n", (int)payload_type);
        return 18;
    }
    return 0;
}

uint8_t ll_cis_rx_test(le_cis_t *cis, u8 payload_type) LL_CODE;
uint8_t ll_cis_rx_test(le_cis_t *cis, u8 payload_type)
{
    cis->rxPendInit = (payload_type != 0);
    cis->rxTestEnabled = 1;
    cis->numRxSuccess = 0;
    cis->numRxMissed = 0;
    cis->numRxFailed = 0;
    cis->isoRxPldType = payload_type;
    return 0;
}

uint8_t ll_cis_read_test_counters(le_cis_t *cis, LlIsoTestCtrs_t *counters) LL_CODE;
uint8_t ll_cis_read_test_counters(le_cis_t *cis, LlIsoTestCtrs_t *counters)
{
    if (!cis->rxTestEnabled) {
        if (!cis->txTestEnabled) {
            if (log_tag_const_d_LL_CIS)
                log_print(1, NULL, "[Debug]: [LL_CIS]Invalid ISO Test state, test mode must be enabled\r\n");
            return 12;
        }
        counters->numSuccess = 0;
        counters->numMissed = 0;
        counters->numFailed = 0;
    } else {
        counters->numSuccess = cis->numRxSuccess;
        counters->numMissed = cis->numRxMissed;
        counters->numFailed = cis->numRxFailed;
    }
    return 0;
}

static void hci_le_cis_established_event(le_cis_t *cis, u8 status) LL_CODE;
static void hci_le_cis_established_event(le_cis_t *cis, u8 status)
{
    CIS_EMIT_META_EVENT(25, "1H33331111111222", status, cis->cisHandle,
                             cis->cigSyncDelayUsec, cis->cisSyncDelayUsec,
                             cis->transLatUsec, cis->transLatUsec, cis->phyMToS, cis->phySToM,
                             cis->nse, cis->bnMToS, cis->bnMToS, cis->ftMToS, cis->ftSToM,
                             cis->pduSizeMToS, cis->pduSizeSToM, cis->isoInterval);
}

static void ll_cis_msg_handler(u8 type, void *message) LL_CODE;
static void ll_cis_msg_handler(u8 type, void *message)
{
    const struct cis_message *data = message;
    switch (type) {
    case 0: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> LL_CIS_MSG_CREATE_CIS_FAIL 0x%x\r\n", (int)data->status);
        le_cis_t *cis = ll_cis_by_handle(data->handle);
        hci_le_cis_established_event(cis, data->status);
        ll_cis_destory(cis);
        break;
    }
    case 1: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> LL_CIS_MSG_CREATE_CIS_SUCCESS\r\n");
        le_cis_t *cis = ll_cis_by_handle(data->handle);
        hci_le_cis_established_event(cis, 0);
        ll_cis_established(cis);
        break;
    }
    case 2:
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> LL_CIS_MSG_MIC_FAILED\r\n");
        (void)ll_cis_by_handle(data->handle);
        break;
    }
}

void ll_cis_init(void) LL_CODE;
void ll_cis_init(void)
{
    INIT_LIST_HEAD(&ll_iso_hdl.cig_head);
    ll_iso_hdl.cig_number = 0;
    ll_iso_hdl.max_cig_number = 2;
    INIT_LIST_HEAD(&ll_iso_hdl.cis_head);
    ll_iso_hdl.cis_number = 0;
    ll_iso_hdl.max_cis_number = 6;
    ll_cis_master_init();
    ll_cis_slave_init();
    ll_dispatch_msg_table[12] = ll_cis_msg_handler;
}

void ll_cis_close(void) LL_CODE;
void ll_cis_close(void)
{
    struct list_head *node = ll_iso_hdl.cis_head.next;
    while (node != &ll_iso_hdl.cis_head) {
        struct list_head *next = node->next;
        ll_cis_free((le_cis_t *)node);
        node = next;
    }
    node = ll_iso_hdl.cig_head.next;
    while (node != &ll_iso_hdl.cig_head) {
        struct list_head *next = node->next;
        ll_cig_free((le_cig_t *)node);
        node = next;
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free

ll_iso_handle_t ll_iso_hdl;
u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS, log_tag_const_i_LL_CIS;

static int logs_e, logs_i, assert_count, malloc_fail, nfreed;
static u32 malloc_sizes[32];
static int nmalloc;
int config_asser;
ll_dispatch_msg_t ll_dispatch_msg_table[15];
static int master_calls, slave_calls, handle_calls, events;
static u32 event_words[16];
static u8 *mutable_message;
static le_cig_t *mutable_group;
static void noop_message(u8 type, void *message) { (void)type; (void)message; }
void p33_soft_reset(void) { CHECK(0); }
void cpu_assert_debug(void) { CHECK(0); }
void ll_cis_master_init(void)
{
    CHECK(ll_iso_hdl.cig_head.next == &ll_iso_hdl.cig_head && ll_iso_hdl.cig_head.prev == &ll_iso_hdl.cig_head);
    CHECK(ll_iso_hdl.cis_head.next == &ll_iso_hdl.cis_head && ll_iso_hdl.cis_head.prev == &ll_iso_hdl.cis_head);
    CHECK(!ll_iso_hdl.cig_number && ll_iso_hdl.max_cig_number == 2);
    CHECK(!ll_iso_hdl.cis_number && ll_iso_hdl.max_cis_number == 6 && ll_iso_hdl.cis_bitmap == 0xa5);
    CHECK(ll_dispatch_msg_table[12] == noop_message); ++master_calls;
}
void ll_cis_slave_init(void)
{ CHECK(master_calls == 1 && ll_dispatch_msg_table[12] == noop_message); ++slave_calls; }
struct le_link *ll_link_for_handle(int handle)
{
    CHECK(handle == 0x1234); ++handle_calls;
    if (mutable_group) mutable_group->numCisEsted = 255;
    return NULL; /* SDK establishment ignores the result. */
}
#ifdef CONFIG_CPU_BD19
u8 cis_emit_meta_event(u8 subevent, const char *format, ...)
#else
bool __hci_emit_le_meta_event(u8 subevent, const char *format, ...)
#endif
{
    CHECK(subevent == 25 && !strcmp(format, "1H33331111111222"));
    va_list ap; va_start(ap, format);
    for (unsigned i = 0; i < 16; ++i) event_words[i] = va_arg(ap, unsigned);
    va_end(ap); ++events; return 0;
}
static void *freed[32];

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag; if (level == 4) logs_e++; else logs_i++;
    if (mutable_message) {
        if (strstr(format, "MSG_CREATE_CIS_FAIL")) {
            mutable_message[0] = 0x61; mutable_message[1] = 0; mutable_message[2] = 0x55;
        } else if (strstr(format, "by handle")) mutable_message[2] = 0xa7;
    }
}
void btctrler_assert_failed(const char *what, int value) { (void)what; CHECK(value == 0); assert_count++; }
void *ll_cis_test_malloc(u32 size)
{
    malloc_sizes[nmalloc++] = size;
    if (malloc_fail) return NULL;
    void *p = malloc((size_t)size * 4);
    memset(p, 0xaa, (size_t)size * 4);
    return p;
}
void ll_cis_test_free(void *p) { freed[nfreed++] = p; }

static LlCisCigParams_t params;

int main(void)
{
    INIT_LIST_HEAD(&ll_iso_hdl.cig_head);
    INIT_LIST_HEAD(&ll_iso_hdl.cis_head);

    /* Handles: 96 + the first clear bit of a six-bit map; exhaustion asserts and returns 102. */
    ll_iso_hdl.cis_bitmap = 0;
    for (int i = 0; i < 6; i++) CHECK(ll_cis_handle_alloc() == 96 + i);
    CHECK(ll_cis_handle_alloc() == 102 && assert_count == 1 && ll_iso_hdl.cis_bitmap == 0x3f);
    ll_iso_hdl.cis_bitmap = 0x0b;                 /* bits 0, 1, 3 taken: bit 2 first */
    CHECK(ll_cis_handle_alloc() == 98 && ll_iso_hdl.cis_bitmap == 0x0f);
    ll_iso_hdl.cis_bitmap = 0;
    assert_count = 0;

    /* CIG allocation zeroes 56 bytes, stores the id and appends to the global list. */
    le_cig_t *cig1 = ll_cig_alloc(1);
    le_cig_t *cig2 = ll_cig_alloc(2);
    CHECK(cig1 && cig2 && malloc_sizes[0] == 56 && cig1->cigId == 1 && cig2->cigId == 2 && cig1->packing == 0 && cig1->cis_number == 0);
    CHECK(ll_iso_hdl.cig_head.next == &cig1->entry && cig1->entry.next == &cig2->entry && cig2->entry.next == &ll_iso_hdl.cig_head);
    CHECK(cig1->cis_head.next == &cig1->cis_head && cig1->cis_head.prev == &cig1->cis_head);
    CHECK(ll_cig_by_id(2) == cig2 && ll_cig_by_id(1) == cig1 && ll_cig_by_id(3) == NULL);
    malloc_fail = 1;
    CHECK(ll_cig_alloc(9) == NULL);
    malloc_fail = 0;

    /* CIS allocation: zeroed, handle, disabled data paths, transport latency 234, counted, appended. */
    ll_iso_hdl.cis_number = 0;
    log_tag_const_i_LL_CIS = 1;
    le_cis_t *a = ll_cis_alloc(cig1);
    le_cis_t *b = ll_cis_alloc(cig1);
    le_cis_t *c = ll_cis_alloc(cig2);
    log_tag_const_i_LL_CIS = 0;
    CHECK(a && b && c && malloc_sizes[3] == 356 && malloc_sizes[4] == 356 && logs_i == 3);
    CHECK(a->cisHandle == 96 && b->cisHandle == 97 && c->cisHandle == 98 && a->cig == cig1 && c->cig == cig2);
    CHECK(a->dataPathInCtx.id == 255 && a->dataPathOutCtx.id == 255 && a->transLatUsec == 234);
    CHECK(a->state == 0 && a->role == 0 && a->cisId == 0 && a->aclHandle == 0 && ll_iso_hdl.cis_number == 3);
    CHECK(ll_iso_hdl.cis_head.next == &a->entry && a->entry.next == &b->entry && b->entry.next == &c->entry && c->entry.next == &ll_iso_hdl.cis_head);
    malloc_fail = 1;
    log_tag_const_e_LL_CIS = 1;
    CHECK(ll_cis_alloc(cig1) == NULL && logs_e == 1 && ll_iso_hdl.cis_number == 3);
    malloc_fail = 0;
    log_tag_const_e_LL_CIS = 0;

    /* Lookups on the shared list (handles 96..98, ids assigned here). */
    a->cisId = 1; b->cisId = 2; c->cisId = 1;
    CHECK(ll_cis_by_handle(97) == b && ll_cis_by_handle(98) == c && ll_cis_by_handle(99) == NULL);
    log_tag_const_i_LL_CIS = 1;
    logs_i = 0;
    CHECK(ll_cis_by_handle(98) == c && logs_i == 3);          /* logs each entry it looks at */
    CHECK(ll_cis_by_handle(0x160) == NULL);                   /* the 16-bit handle is compared as is */
    log_tag_const_i_LL_CIS = 0;
    /* By id, the shared list is searched by cisId alone: the first with that id wins whatever the cig. */
    CHECK(ll_cis_by_id(2, 1) == a && ll_cis_by_id(1, 2) == b && ll_cis_by_id(7, 1) == a);
    CHECK(ll_cis_by_id(1, 5) == NULL);                        /* not on the list; cig 1 exists but has no streams */

    /* Moving streams into their groups. */
    ll_cig_add_cis(cig1, a);
    ll_cig_add_cis_tail(cig1, b);
    ll_cig_add_cis(cig2, c);
    CHECK(cig1->cis_number == 2 && cig2->cis_number == 1);
    CHECK(cig1->cis_head.next == &a->entry && a->entry.next == &b->entry && b->entry.next == &cig1->cis_head);
    CHECK(ll_iso_hdl.cis_head.next == &ll_iso_hdl.cis_head);   /* the shared list is now empty */
    ll_cig_add_cis(cig1, b);                                   /* re-adding moves it to the front and counts again */
    CHECK(cig1->cis_head.next == &b->entry && b->entry.next == &a->entry && cig1->cis_number == 3);
    cig1->cis_number = 2;
    ll_cig_add_cis_tail(cig1, b);
    CHECK(cig1->cis_head.next == &a->entry && a->entry.next == &b->entry);
    cig1->cis_number = 2;
    CHECK(ll_cis_by_handle(97) == b && ll_cis_by_handle(98) == c && ll_cis_by_handle(96) == a && ll_cis_by_handle(1) == NULL);
    CHECK(ll_cis_by_id(1, 2) == b && ll_cis_by_id(2, 1) == c && ll_cis_by_id(1, 1) == a && ll_cis_by_id(2, 2) == NULL && ll_cis_by_id(9, 1) == NULL);
    CHECK(ll_cig_get_head_cis(cig1) == a);
    ll_cig_remove_cis(cig1, b);
    CHECK(cig1->cis_number == 1 && a->entry.next == &cig1->cis_head);
    ll_cig_add_cis_tail(cig1, b);
    CHECK(cig1->cis_number == 2 && ll_cig_get_head_cis(cig1) == a);

    /* Established state and counts. */
    a->state = 1; b->state = 2;
    CHECK(ll_cis_is_established(a) == 1 && ll_cis_is_established(b) == 0);
    memset(&params, 0, sizeof(params));
    params.cigId = 1; params.numCis = 3;
    params.pCisParam[0].cisId = 1; params.pCisParam[1].cisId = 2; params.pCisParam[2].cisId = 9;
    CHECK(ll_get_number_enable_cis(&params) == 2);
    params.numCis = 0;
    CHECK(ll_get_number_enable_cis(&params) == 0);
    LlCisCigParamsTest_t test_params;
    memset(&test_params, 0, sizeof(test_params));
    test_params.cigId = 1;
    test_params.numCis = 2;
    test_params.pCisParam[0].cisId = 1;
    test_params.pCisParam[1].cisId = 9;
    CHECK(ll_get_number_enable_cis_test(&test_params) == 1);
    a->role = 0; a->bnMToS = 0;
    CHECK(ll_cis_tx_test(a, 1) == 17);
    a->bnMToS = 1;
    CHECK(ll_cis_tx_test(a, 2) == 0 && ll_cis_tx_test(a, 3) == 18);
    a->role = 1; a->bnSToM = 0;
    CHECK(ll_cis_tx_test(a, 0) == 17);
    a->bnSToM = 1;
    CHECK(ll_cis_tx_test(a, 0) == 0);
    a->rxTestEnabled = 0; a->rxPendInit = 0xff;
    CHECK(ll_cis_rx_test(a, 1) == 0 && a->rxTestEnabled == 1 && a->rxPendInit == 1);
    CHECK(a->numRxSuccess == 0 && a->numRxMissed == 0 && a->numRxFailed == 0 && a->isoRxPldType == 1);
    LlIsoTestCtrs_t counters = { 1, 2, 3 };
    CHECK(ll_cis_read_test_counters(a, &counters) == 0);
    CHECK(counters.numSuccess == 0 && counters.numMissed == 0 && counters.numFailed == 0);
    a->numRxSuccess = 4; a->numRxMissed = 5; a->numRxFailed = 6;
    CHECK(ll_cis_read_test_counters(a, &counters) == 0);
    CHECK(counters.numSuccess == 4 && counters.numMissed == 5 && counters.numFailed == 6);
    a->rxTestEnabled = 0; a->txTestEnabled = 0;
    CHECK(ll_cis_read_test_counters(a, &counters) == 12);
    ll_iso_hdl.max_cis_number = 8;
    CHECK(ll_get_number_avail_cis() == 5);                     /* 3 allocated */
    ll_iso_hdl.max_cis_number = 1;
    CHECK(ll_get_number_avail_cis() == 254);                   /* unsigned byte wrap */

    /* Head test. */
    CHECK(lctrCisIsHeadCis(&cig1->cis_head, a) == 1 && lctrCisIsHeadCis(&cig1->cis_head, b) == 0);
    struct list_head empty_head;
    INIT_LIST_HEAD(&empty_head);
    CHECK(lctrCisIsHeadCis(&empty_head, a) == 0);

    /* Destroy: the head stream records the group's offset (packing 1: delay; else nse * subinterval). */
    cig1->packing = 1; a->delayUsec = 777; a->nse = 3; a->subIntervUsec = 1000; b->nse = 4; b->subIntervUsec = 500;
    nfreed = 0;
    ll_iso_hdl.cis_number = 3;
    ll_cis_destory(a);
    CHECK(cig1->headCisRmved == 1 && cig1->offsetUsec == 777 && nfreed == 1 && freed[0] == a && ll_iso_hdl.cis_number == 2);
    CHECK(cig1->cis_head.next == &b->entry && b->entry.prev == &cig1->cis_head);
    cig1->packing = 0; cig1->headCisRmved = 0;
    ll_cis_destory(b);                                         /* b is now the head: 4 * 500 */
    CHECK(cig1->headCisRmved == 1 && cig1->offsetUsec == 2000 && nfreed == 2 && freed[1] == b);
    /* The group is emptied, ll_cig_free is called and returns at once: it stays listed and is not freed. */
    CHECK(nfreed == 2 && ll_iso_hdl.cig_head.next == &cig1->entry && cig1->cis_head.next == &cig1->cis_head);
    /* Destroying a stream that is not the head leaves the group offset alone. */
    le_cis_t *d = ll_cis_alloc(cig2);
    le_cis_t *e = ll_cis_alloc(cig2);
    ll_cig_add_cis_tail(cig2, d);                              /* cig2 now has c, d */
    ll_cig_add_cis_tail(cig2, e);                              /* c, d, e */
    cig2->offsetUsec = 5; cig2->headCisRmved = 0;
    ll_cis_destory(d);
    CHECK(cig2->offsetUsec == 5 && cig2->headCisRmved == 0 && cig2->cis_head.next == &c->entry && c->entry.next == &e->entry);

    /* Freeing a group with streams frees each of them, unlinks the group and frees it. */
    nfreed = 0;
    ll_iso_hdl.cis_number = 5;
    ll_cig_free(cig2);
    CHECK(nfreed == 3 && freed[0] == c && freed[1] == e && freed[2] == cig2 && ll_iso_hdl.cis_number == 3);
    CHECK(cig1->entry.next == &ll_iso_hdl.cig_head && ll_iso_hdl.cig_head.next == &cig1->entry);
    nfreed = 0;
    ll_cig_free(cig1);                                         /* empty: nothing happens */
    CHECK(nfreed == 0 && ll_iso_hdl.cig_head.next == &cig1->entry);

    /* A stream whose group is null asserts and then dereferences the group. */
    /* (not run on the host: the SDK continues into the null group's list.) */

    /* Duration arithmetic.  1M: 8 us/byte + 112; 2M: 4 us/byte + 60; coded: 64 us/byte + 976; 150 us between. */
    ll_iso_hdl.cisSubEvtSpaceDelay = 100;
    CHECK(ll_cis_calc_subevt_sequential_us(1, 2, 10, 20) == (10 * 8 + 112) + 150 + (20 * 4 + 60) + 150 + 100);
    CHECK(ll_cis_calc_subevt_sequential_us(4, 1, 1, 0) == (1 * 64 + 976) + 150 + (0 + 112) + 150 + 100);
    CHECK(ll_cis_calc_subevt_sequential_us(2, 4, 255, 255) == (255 * 4 + 60) + 150 + (255 * 64 + 976) + 150 + 100);
    CHECK(ll_cis_calc_subevt_sequential_us(3, 8, 50, 50) == 150 + 150 + 100);                 /* unknown PHYs contribute nothing */
    CHECK(ll_cis_calc_subevt_sequential_us(1, 7, 0, 99) == 112 + 150 + 150 + 100);
    ll_iso_hdl.cisSubEvtSpaceDelay = 0xffff;
    CHECK(ll_cis_calc_subevt_sequential_us(0, 0, 0, 0) == 150 + 150 + 0xffff);
    ll_iso_hdl.cisSubEvtSpaceDelay = 100;

    /* Interleaved: the sum over the group's streams (found by id). */
    le_cis_t *f = ll_cis_alloc(cig1);
    le_cis_t *g = ll_cis_alloc(cig1);
    ll_cig_add_cis_tail(cig1, f);
    ll_cig_add_cis_tail(cig1, g);
    f->cisId = 11; f->phyMToS = 1; f->phySToM = 1; f->pduSizeMToS = 10; f->pduSizeSToM = 20;
    g->cisId = 12; g->phyMToS = 2; g->phySToM = 4; g->pduSizeMToS = 0x105; g->pduSizeSToM = 3;   /* PDU sizes pass as bytes: 0x105 -> 5 */
    memset(&params, 0, sizeof(params));
    params.cigId = 1; params.numCis = 2; params.pCisParam[0].cisId = 11; params.pCisParam[1].cisId = 12;
    u32 expect_f = (10 * 8 + 112) + 150 + (20 * 8 + 112) + 150 + 100;
    u32 expect_g = (5 * 4 + 60) + 150 + (3 * 64 + 976) + 150 + 100;
    CHECK(ll_cis_calc_subevt_interleaved_us(&params) == expect_f + expect_g);
    params.numCis = 0;
    CHECK(ll_cis_calc_subevt_interleaved_us(&params) == 0);
    /* Lifecycle init preserves the SDK's existing allocation bitmap. */
    memset(&ll_iso_hdl, 0x5a, sizeof(ll_iso_hdl)); ll_iso_hdl.cis_bitmap = 0xa5;
    for (unsigned i = 0; i < 15; ++i) ll_dispatch_msg_table[i] = noop_message;
    ll_cis_init(); CHECK(master_calls == 1 && slave_calls == 1);
    for (unsigned i = 0; i < 15; ++i)
        CHECK(ll_dispatch_msg_table[i] == (i == 12 ? ll_cis_msg_handler : noop_message));

    static le_cig_t group;
    static le_cis_t stream1, stream2;
    memset(&group, 0, sizeof(group)); memset(&stream1, 0, sizeof(stream1));
    memset(&stream2, 0, sizeof(stream2)); INIT_LIST_HEAD(&group.cis_head);
    INIT_LIST_HEAD(&stream1.entry); INIT_LIST_HEAD(&stream2.entry);
    group.cigId = 7; stream1.cig = stream2.cig = &group;
    stream1.cisHandle = 96; stream2.cisHandle = 97;
    stream1.aclHandle = stream2.aclHandle = 0x1234;
    list_add_tail(&stream1.entry, &ll_iso_hdl.cis_head);
    list_add_tail(&stream2.entry, &ll_iso_hdl.cis_head); ll_iso_hdl.cis_number = 2;
    stream2.cigSyncDelayUsec = 0x10203; stream2.cisSyncDelayUsec = 0x40506;
    stream2.transLatUsec = 0x70809; stream2.phyMToS = 1; stream2.phySToM = 2;
    stream2.nse = 3; stream2.bnMToS = 4; stream2.bnSToM = 99;
    stream2.ftMToS = 5; stream2.ftSToM = 6;
    stream2.pduSizeMToS = 0x1234; stream2.pduSizeSToM = 0x5678; stream2.isoInterval = 0xabcd;
    _Alignas(u16) u8 message[] = {96, 0, 0x33, 0};
    /* Logging changes the handle, and lookup logging changes status again. */
    log_tag_const_i_LL_CIS = 1; mutable_message = message; nfreed = 0;
    ll_dispatch_msg_table[12](0, message); mutable_message = NULL;
    static const u32 event_expected[] = {0xa7, 97, 0x10203, 0x40506, 0x70809, 0x70809,
                                        1, 2, 3, 4, 4, 5, 6, 0x1234, 0x5678, 0xabcd};
    CHECK(events == 1 && !memcmp(event_words, event_expected, sizeof(event_expected)));
    CHECK(nfreed == 1 && freed[0] == &stream2 && ll_iso_hdl.cis_number == 1);
    /* Success emits first, then establishes even with no ACL link result. */
    message[0] = 96; message[2] = 0;
    mutable_group = &group; ll_dispatch_msg_table[12](1, message); mutable_group = NULL;
    CHECK(events == 2 && !event_words[0] && event_words[1] == 96 && handle_calls == 1);
    CHECK(group.numCisEsted == 0); /* callback-updated 255 wraps to zero */
    ll_dispatch_msg_table[12](2, message); CHECK(events == 2 && nfreed == 1);
    ll_dispatch_msg_table[12](255, NULL); CHECK(events == 2 && nfreed == 1);
    log_tag_const_i_LL_CIS = 0;

    /* Close preserves empty CIGs, but frees streams/groups with the SDK list quirks. */
    list_add_tail(&group.entry, &ll_iso_hdl.cig_head);
    ll_cis_close(); CHECK(nfreed == 2 && freed[1] == &stream1 && !ll_iso_hdl.cis_number);
    CHECK(ll_iso_hdl.cig_head.next == &group.entry); /* empty group is retained */
    INIT_LIST_HEAD(&stream1.entry); ll_cig_add_cis_tail(&group, &stream1);
    ll_iso_hdl.cis_number = 1; nfreed = 0;
    ll_cis_close(); CHECK(nfreed == 2 && freed[0] == &stream1 && freed[1] == &group);
    CHECK(list_empty(&ll_iso_hdl.cis_head) && list_empty(&ll_iso_hdl.cig_head));
    return TEST_DONE("btctrler_ll_cis");
}
#endif
