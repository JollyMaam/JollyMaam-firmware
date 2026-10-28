/* LE Broadcast Isochronous Stream (BIG/BIS) bookkeeping: lifecycle, handle
 * and object allocation, data path setup, test helpers, and group key/address
 * helpers. SDK quirks kept: BIG and BIS objects come from malloc and are not cleared
 * (callers fill them in); BIS handles are 112 + a bit index of a six-bit map
 * and the exhaustion path asserts and still returns 118; ll_bis_by_handle is a
 * stub that always returns null and ll_bis_setup does nothing; the BIS and
 * BIG free paths unlink with list_del and a BIG free tears each BIS's data
 * path down before freeing it. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_BIS, log_tag_const_i_LL_BIS;
extern BTCTRLER_CONFIG u8 log_tag_const_d_LL_BIS;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int aes_cmac(u8 *out, const u8 *key, const u8 *msg, int length);
extern void ll_bis_slave_init(void);
#ifdef UNITTEST
#include <stdlib.h>
void *ll_bis_test_malloc(u32 size);
void ll_bis_test_free(void *ptr);
#define malloc ll_bis_test_malloc
#define free ll_bis_test_free
#else
extern void *malloc(u32 size);
extern void free(void *ptr);
#endif

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifndef UNITTEST
_Static_assert(sizeof(le_bis_t) == 52, "SDK BIS allocation size");
_Static_assert(offsetof(le_bis_t, bisNum) == 8, "SDK BIS number offset");
_Static_assert(offsetof(le_bis_t, handle) == 10, "SDK BIS handle offset");
_Static_assert(offsetof(le_bis_t, big) == 12, "SDK BIS big offset");
_Static_assert(offsetof(le_bis_t, test.numRxFailed) == 36, "SDK BIS path follows the test block at 40");
_Static_assert(sizeof(le_big_t) == 168, "SDK BIG allocation size");
_Static_assert(offsetof(le_big_t, bis_head) == 8, "SDK BIG bis list offset");
_Static_assert(offsetof(le_big_t, handle) == 17, "SDK BIG handle offset");
_Static_assert(sizeof(PalCodecSreamParam_t) == 20, "SDK codec parameter size");
#endif
_Static_assert(offsetof(le_bis_t, entry) == 0, "SDK BIS list entry offset");
_Static_assert(offsetof(le_big_t, entry) == 0, "SDK BIG list entry offset");

static const u8 lctrBisCalcGroupSessionKey_big1[16] __attribute__((section(".ble_ll_const"))) = { '1', 'G', 'I', 'B' };
static const u8 lctrBisCalcGroupSessionKey_big2[4] __attribute__((section(".ble_ll_const"))) = { '2', 'G', 'I', 'B' };
static const u8 lctrBisCalcGroupSessionKey_big3[4] __attribute__((section(".ble_ll_const"))) = { '3', 'G', 'I', 'B' };

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static void list_add_tail(struct list_head *_new)
{
    struct list_head *head = &ll_iso_hdl.big_head;
    struct list_head *prev = head->prev;
    head->prev = _new;
    _new->next = head;
    _new->prev = prev;
    prev->next = _new;
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

static int list_empty(struct list_head *head)
{
    return head->next == head;
}

static void ll_bis_msg_handler(u8 type, void *msg) LL_CODE;
static void ll_bis_msg_handler(u8 type, void *msg)
{
    (void)type;
    (void)msg;
}

void ll_bis_init(void) LL_CODE;
void ll_bis_init(void)
{
    INIT_LIST_HEAD(&ll_iso_hdl.big_head);
    ll_iso_hdl.big_number = 0;
    ll_iso_hdl.max_big_number = 2;
    INIT_LIST_HEAD(&ll_iso_hdl.bis_head);
    ll_iso_hdl.bis_number = 0;
    ll_iso_hdl.max_bis_number = 6;
    ll_bis_slave_init();
    ll_dispatch_msg_table[9] = ll_bis_msg_handler;
}

void ll_bis_close(void) LL_CODE;
void ll_bis_close(void)
{
    /* The SDK closes through the BIG list and passes each BIG as a BIS.
     * Keep this odd cast and its ll_bis_free side effects. */
    struct list_head *head = &ll_iso_hdl.big_head;
    if (list_empty(head)) return;
    struct list_head *node = head->next;
    while (node != head) {
        struct list_head *next = node->next;
        ll_bis_free((le_bis_t *)node);
        node = next;
    }
}

uint32_t lctrComputeBisAccessAddr(uint32_t seedAccAddr, uint8_t bisNum) LL_CODE;
uint32_t lctrComputeBisAccessAddr(uint32_t seedAccAddr, uint8_t bisNum)
{
    uint32_t value = (uint32_t)bisNum * 35u + 42u;
    uint32_t bit0 = value & 1u;
    uint32_t bit1 = (value >> 1) & 1u;
    uint32_t spread =
        (bit0 << 30) | (value << 31) | (bit0 << 29) | (bit0 << 28) |
        (bit0 << 27) | (bit0 << 26) | ((value << 18) & 0x01000000u) |
        ((value << 16) & 0x00200000u) | ((value << 16) & 0x00100000u) |
        ((value << 15) & 0x00040000u) | ((value << 15) & 0x00020000u) |
        (bit1 << 25) | (bit1 << 23);
    return spread ^ seedAccAddr;
}

uint8_t ll_bis_tx_test(le_bis_t *bis, uint8_t payload_type) LL_CODE;
uint8_t ll_bis_tx_test(le_bis_t *bis, uint8_t payload_type)
{
    (void)payload_type;
    if (!bis->test.enabled && !bis->test.term) return 0;
    if (log_tag_const_d_LL_BIS)
        log_print(0, NULL, "[Debug]: [LL_BIS]Invalid ISO Test state, test mode already enabled\r\n");
    return 12;
}

uint8_t ll_bis_rx_test(le_bis_t *bis, uint8_t payload_type) LL_CODE;
uint8_t ll_bis_rx_test(le_bis_t *bis, uint8_t payload_type)
{
    if (!bis->test.enabled && !bis->test.term) {
        bis->test.enabled = 1;
        bis->test.term = 0;
        bis->test.pldType = LL_ISO_TEST_PL_LEN_VAR;
        bis->test.util.unframed.payloadOffset = payload_type;
        return 0;
    }
    if (log_tag_const_d_LL_BIS)
        log_print(0, NULL, "[Debug]: [LL_BIS]Invalid ISO Test state, test mode already enabled\r\n");
    return 12;
}

uint8_t ll_bis_read_test_counters(le_bis_t *bis, LlIsoTestCtrs_t *counters) LL_CODE;
uint8_t ll_bis_read_test_counters(le_bis_t *bis, LlIsoTestCtrs_t *counters)
{
    if (!bis->test.enabled) {
        if (log_tag_const_d_LL_BIS)
            log_print(0, NULL, "[Debug]: [LL_BIS]Invalid ISO Test state, test mode must be enabled\r\n");
        return 12;
    }
    if (bis->test.term != 1) return 0;
    counters->numSuccess = bis->test.numRxSuccess;
    counters->numMissed = bis->test.numRxMissed;
    counters->numFailed = bis->test.numRxFailed;
    return 0;
}

le_bis_t *ll_bis_by_handle(uint16_t handle) LL_CODE;
le_bis_t *ll_bis_by_handle(uint16_t handle)
{
    (void)handle;
    return NULL;
}

uint16_t ll_bis_handle_alloc(void) LL_CODE;
uint16_t ll_bis_handle_alloc(void)
{
    u8 bitmap = ll_iso_hdl.bis_bitmap;
    int i;
    for (i = 0; i < 6; i++) {
        u32 bit = 1u << i;
        if ((bitmap & bit) == 0) {
            ll_iso_hdl.bis_bitmap = (u8)(bitmap | bit);
            break;
        }
    }
    if (i >= 6) btctrler_assert_failed("i < LL_MAX_BIS alloc_bis_handle error", 0);
    return (uint16_t)(i + 112);
}

le_bis_t *ll_bis_alloc(le_big_t *big) LL_CODE;
le_bis_t *ll_bis_alloc(le_big_t *big)
{
    le_bis_t *bis = malloc(52);
    if (!bis) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]BIS No memory\r\n");
        return NULL;
    }
    uint16_t handle = ll_bis_handle_alloc();
    bis->handle = handle;
    if (log_tag_const_i_LL_BIS)
        log_print(2, NULL, "[Info]: [LL_BIS]BIS handle : 0x%x\r\n", (int)handle);
    u8 number = (u8)(big->numBis + 1);
    bis->bisNum = number;
    big->numBis = number;
    bis->path = (LlIsoDataPath_t)0xff;     /* the SDK stores 0xffff over path and lastLlid */
    bis->lastLlid = (LlIsoLlid_t)0xff;
    bis->big = big;
    ll_iso_hdl.bis_number = (u8)(ll_iso_hdl.bis_number + 1);
    return bis;
}

void ll_bis_free(le_bis_t *bis) LL_CODE;
void ll_bis_free(le_bis_t *bis)
{
    ll_iso_hdl.bis_number = (u8)(ll_iso_hdl.bis_number - 1);
    bis->big->numBis = (u8)(bis->big->numBis - 1);
    list_del(&bis->entry);
    free(bis);
}

le_big_t *ll_big_by_handle(uint16_t handle) LL_CODE;
le_big_t *ll_big_by_handle(uint16_t handle)
{
    for (struct list_head *node = ll_iso_hdl.big_head.next; node != &ll_iso_hdl.big_head; node = node->next) {
        le_big_t *big = (le_big_t *)node;
        if (big->handle == handle) return big;
    }
    return NULL;
}

le_big_t *ll_big_alloc(uint8_t handle) LL_CODE;
le_big_t *ll_big_alloc(uint8_t handle)
{
    le_big_t *big = malloc(168);
    if (!big) return NULL;
    big->handle = handle;
    INIT_LIST_HEAD(&big->bis_head);
    list_add_tail(&big->entry);
    ll_iso_hdl.big_number = (u8)(ll_iso_hdl.big_number + 1);
    return big;
}

void ll_big_free(le_big_t *big) LL_CODE;
void ll_big_free(le_big_t *big)
{
    struct list_head *head = &big->bis_head;
    struct list_head *node = head->next;
    struct list_head *next;
    for (;; node = next) {
        next = node->next;
        if (node == head) break;
        le_bis_t *bis = (le_bis_t *)node;
        switch (big->role) {
        case 1:
            ll_bis_setup_iso_data_path(bis, 0, 255);
            break;
        case 0:
            ll_bis_setup_iso_data_path(bis, 1, 255);
            break;
        default:
            break;
        }
        ll_bis_free(bis);
    }
    ll_iso_hdl.big_number = (u8)(ll_iso_hdl.big_number - 1);
    list_del(&big->entry);
    free(big);
}

u8 ll_bis_setup_iso_data_path(le_bis_t *bis, LlIsoDataPathDir_t dpDir, LlIsoDataPath_t dpId) LL_CODE;
u8 ll_bis_setup_iso_data_path(le_bis_t *bis, LlIsoDataPathDir_t dpDir, LlIsoDataPath_t dpId)
{
    PalCodecSreamParam_t param;
    u32 current = bis->path;

    if (current == (u32)dpId) return 0;
    if (current == 1) {
        if (!ll_codec.stop) return 18;
        ll_codec.stop(bis->handle);
    }
    bis->path = dpId;
    switch ((u32)dpId) {
    case 1:
        if (!ll_codec.start) {
            if (log_tag_const_e_LL_BIS)
                log_print(4, NULL, "<Error>: [LL_BIS]Codec not found, dpId=%u\r\n", 1);
            return 18;
        }
        param.dir = dpDir != 0;
        param.chMask = 3;
        param.intervalUsec = bis->big->isoInterUsec;
        param.pktCtr = (u32)((u64)bis->big->bn * (bis->big->eventCounter + 1));
        param.rdyCback = lctrIsoSendCodecSdu;
        if (ll_codec.start(bis->handle, &param) == 0) {
            if (log_tag_const_e_LL_BIS)
                log_print(4, NULL, "<Error>: [LL_BIS]Failed to start the codec, dpId=%u\r\n", 1);
            bis->path = (LlIsoDataPath_t)0xff;
            return 13;
        }
        return 0;
    case 255:
    case 0:
        return 0;
    default:
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]Unknown Data Path, dpId=%u\r\n", (int)dpId);
        return 18;
    }
}

uint8_t ll_get_number_avail_bis(void) LL_CODE;
uint8_t ll_get_number_avail_bis(void)
{
    return (uint8_t)(ll_iso_hdl.max_bis_number - ll_iso_hdl.bis_number);
}

void lctrBisCalcGroupSessionKey(const uint8_t *pGSKD, const uint8_t *pBC, uint8_t *pGSK) LL_CODE;
void lctrBisCalcGroupSessionKey(const uint8_t *pGSKD, const uint8_t *pBC, uint8_t *pGSK)
{
    u8 igltk[16];
    u8 gltk[16];
    u8 ik[16];

    aes_cmac(igltk, lctrBisCalcGroupSessionKey_big1, pBC, 16);
    aes_cmac(gltk, igltk, lctrBisCalcGroupSessionKey_big2, 4);
    aes_cmac(ik, pGSKD, gltk, 16);
    aes_cmac(pGSK, ik, lctrBisCalcGroupSessionKey_big3, 4);
}

void ll_bis_setup(le_bis_t *bis, uint32_t seedAccAddr, uint16_t baseCrcInit, uint64_t chMap, LlPhy_t phy) LL_CODE;
void ll_bis_setup(le_bis_t *bis, uint32_t seedAccAddr, uint16_t baseCrcInit, uint64_t chMap, LlPhy_t phy)
{
    (void)bis; (void)seedAccAddr; (void)baseCrcInit; (void)chMap; (void)phy;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free

ll_iso_handle_t ll_iso_hdl;
ll_codec_t ll_codec;
ll_dispatch_msg_t ll_dispatch_msg_table[15];
u8 log_tag_const_e_LL_BIS, log_tag_const_i_LL_BIS;
u8 log_tag_const_d_LL_BIS;

static int slave_init_calls;
void ll_bis_slave_init(void) { slave_init_calls++; }

static int logs_e, logs_i, assert_count, malloc_fail, nfreed;
static u32 malloc_sizes[16];
static int nmalloc;
static void *freed[16];
static char trace[256];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }

void log_print(int level, const char *tag, const char *format, ...) { (void)tag; (void)format; if (level == 4) logs_e++; else logs_i++; }
void btctrler_assert_failed(const char *what, int value) { (void)what; CHECK(value == 0); assert_count++; }
void *ll_bis_test_malloc(u32 size) { malloc_sizes[nmalloc++] = size; return malloc_fail ? NULL : calloc(1, (size_t)size * 4); }
void ll_bis_test_free(void *p) { freed[nfreed++] = p; }
void lctrIsoSendCodecSdu(u16 h) { (void)h; }

static struct { u8 call; u8 key[16]; u8 msg[16]; int len; u8 *out; const u8 *keyp, *msgp; } cmac[4];
static int ncmac;
int aes_cmac(u8 *out, const u8 *key, const u8 *msg, int len)
{
    int n = ncmac++;
    cmac[n].call = (u8)n; cmac[n].out = out; cmac[n].keyp = key; cmac[n].msgp = msg; cmac[n].len = len;
    memcpy(cmac[n].key, key, 16);                 /* the SDK passes 16-byte keys */
    memcpy(cmac[n].msg, msg, (size_t)len);
    for (int i = 0; i < 16; i++) out[i] = (u8)(0x10 * (n + 1) + i);
    return 0;
}

static u16 stop_handle;
static int stop_calls, start_calls, start_result;
static PalCodecSreamParam_t start_param;
static u16 start_handle;
static void codec_stop(u16 h) { stop_handle = h; stop_calls++; }
static u8 codec_start(u16 h, PalCodecSreamParam_t *p) { start_handle = h; start_param = *p; start_calls++; return (u8)start_result; }

int main(void)
{
    /* Init resets only the BIG/BIS bookkeeping, delegates slave init, and
     * installs the SDK's intentionally empty message handler in slot 9. */
    memset(&ll_iso_hdl, 0xa5, sizeof(ll_iso_hdl));
    memset(ll_dispatch_msg_table, 0, sizeof(ll_dispatch_msg_table));
    slave_init_calls = 0;
    ll_bis_init();
    CHECK(ll_iso_hdl.big_head.next == &ll_iso_hdl.big_head && ll_iso_hdl.big_head.prev == &ll_iso_hdl.big_head);
    CHECK(ll_iso_hdl.bis_head.next == &ll_iso_hdl.bis_head && ll_iso_hdl.bis_head.prev == &ll_iso_hdl.bis_head);
    CHECK(ll_iso_hdl.big_number == 0 && ll_iso_hdl.max_big_number == 2);
    CHECK(ll_iso_hdl.bis_number == 0 && ll_iso_hdl.max_bis_number == 6);
    CHECK(slave_init_calls == 1 && ll_dispatch_msg_table[9] != NULL);
    CHECK(lctrComputeBisAccessAddr(0x12345678, 0) == 0x10905678); /* bit1 contributes bits 25 and 23 */
    CHECK(lctrComputeBisAccessAddr(0x12345678, 1) == 0xef325678);
    CHECK(lctrComputeBisAccessAddr(0x12345678, 63) == 0xedb65678);

    INIT_LIST_HEAD(&ll_iso_hdl.big_head);
    CHECK(ll_bis_by_handle(0x70) == NULL && ll_bis_by_handle(0) == NULL);

    /* Handles: 112 + the first clear bit of a six-bit map; exhaustion asserts and returns 118. */
    ll_iso_hdl.bis_bitmap = 0;
    for (int i = 0; i < 6; i++) CHECK(ll_bis_handle_alloc() == 112 + i);
    CHECK(ll_iso_hdl.bis_bitmap == 0x3f && assert_count == 0);
    CHECK(ll_bis_handle_alloc() == 118 && assert_count == 1 && ll_iso_hdl.bis_bitmap == 0x3f);
    ll_iso_hdl.bis_bitmap = 0x05;              /* bits 0 and 2 taken: bit 1 first */
    CHECK(ll_bis_handle_alloc() == 113 && ll_iso_hdl.bis_bitmap == 0x07);
    CHECK(ll_bis_handle_alloc() == 115 && ll_iso_hdl.bis_bitmap == 0x0f);
    ll_iso_hdl.bis_bitmap = 0xc0;              /* bits above the six are ignored */
    CHECK(ll_bis_handle_alloc() == 112 && ll_iso_hdl.bis_bitmap == 0xc1);
    ll_iso_hdl.bis_bitmap = 0;
    assert_count = 0;

    /* BIG allocation: 168 bytes, handle stored, bis list empty, appended to the global list. */
    ll_iso_hdl.big_number = 0;
    le_big_t *big1 = ll_big_alloc(3);
    le_big_t *big2 = ll_big_alloc(4);
    CHECK(big1 && big2 && nmalloc == 2 && malloc_sizes[0] == 168 && malloc_sizes[1] == 168);
    CHECK(big1->handle == 3 && big2->handle == 4 && ll_iso_hdl.big_number == 2);
    CHECK(big1->bis_head.next == &big1->bis_head && big1->bis_head.prev == &big1->bis_head);
    CHECK(ll_iso_hdl.big_head.next == &big1->entry && big1->entry.next == &big2->entry && big2->entry.next == &ll_iso_hdl.big_head);
    CHECK(ll_iso_hdl.big_head.prev == &big2->entry && big2->entry.prev == &big1->entry && big1->entry.prev == &ll_iso_hdl.big_head);
    CHECK(ll_big_by_handle(3) == big1 && ll_big_by_handle(4) == big2 && ll_big_by_handle(5) == NULL);
    CHECK(ll_big_by_handle(0x103) == NULL);       /* the 16-bit handle is compared with the byte */
    malloc_fail = 1;
    CHECK(ll_big_alloc(9) == NULL && ll_iso_hdl.big_number == 2);
    malloc_fail = 0;

    /* BIS allocation: 52 bytes, handle from the map, number from the BIG's count, path/lastLlid 0xffff. */
    ll_iso_hdl.bis_number = 0;
    log_tag_const_i_LL_BIS = 1;
    big1->numBis = 0;
    le_bis_t *bisA = ll_bis_alloc(big1);
    le_bis_t *bisB = ll_bis_alloc(big1);
    CHECK(bisA && bisB && malloc_sizes[3] == 52 && malloc_sizes[4] == 52);
    CHECK(bisA->handle == 112 && bisB->handle == 113 && bisA->bisNum == 1 && bisB->bisNum == 2 && big1->numBis == 2);
    CHECK(bisA->path == 255 && bisA->lastLlid == 255 && bisA->big == big1 && ll_iso_hdl.bis_number == 2 && logs_i == 2);
    log_tag_const_i_LL_BIS = 0;
    malloc_fail = 1;
    log_tag_const_e_LL_BIS = 1;
    CHECK(ll_bis_alloc(big1) == NULL && logs_e == 1 && big1->numBis == 2 && ll_iso_hdl.bis_number == 2);
    malloc_fail = 0;
    log_tag_const_e_LL_BIS = 0;
    big1->numBis = 255;                        /* the BIS number is a byte */
    le_bis_t *bisW = ll_bis_alloc(big1);
    CHECK(bisW->bisNum == 0 && big1->numBis == 0);
    big1->numBis = 2;
    ll_iso_hdl.bis_number = 2;
    free(bisW);

    /* The callers link the BIS entries; free unlinks, counts down and frees. */
    bisA->entry.next = &bisB->entry; bisA->entry.prev = &big1->bis_head;
    bisB->entry.next = &big1->bis_head; bisB->entry.prev = &bisA->entry;
    big1->bis_head.next = &bisA->entry; big1->bis_head.prev = &bisB->entry;
    nfreed = 0;
    ll_bis_free(bisA);
    CHECK(nfreed == 1 && freed[0] == bisA && ll_iso_hdl.bis_number == 1 && big1->numBis == 1);
    CHECK(big1->bis_head.next == &bisB->entry && bisB->entry.prev == &big1->bis_head);
    CHECK(bisA->entry.next == &bisA->entry && bisA->entry.prev == &bisA->entry);

    /* Available BIS count. */
    ll_iso_hdl.max_bis_number = 6; ll_iso_hdl.bis_number = 1;
    CHECK(ll_get_number_avail_bis() == 5);
    ll_iso_hdl.max_bis_number = 1; ll_iso_hdl.bis_number = 3;
    CHECK(ll_get_number_avail_bis() == 254);       /* unsigned byte wrap */
    ll_iso_hdl.bis_number = 1;

    /* Test helpers retain the SDK's state checks, byte-sized payload offset,
     * and conditional counter copy. */
    le_bis_t test_bis = {0};
    LlIsoTestCtrs_t counters = { 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc };
    CHECK(ll_bis_tx_test(&test_bis, 7) == 0);
    CHECK(ll_bis_rx_test(&test_bis, 0x23) == 0);
    CHECK(test_bis.test.enabled == 1 && test_bis.test.term == 0);
    CHECK((int)test_bis.test.pldType == (int)LL_ISO_TEST_PL_LEN_VAR && test_bis.test.util.unframed.payloadOffset == 0x23);
    CHECK(ll_bis_tx_test(&test_bis, 9) == 12 && ll_bis_rx_test(&test_bis, 10) == 12);
    CHECK(ll_bis_read_test_counters(&test_bis, &counters) == 0);
    CHECK(counters.numSuccess == 0xaaaaaaaa && counters.numMissed == 0xbbbbbbbb && counters.numFailed == 0xcccccccc);
    test_bis.test.term = 1;
    test_bis.test.numRxSuccess = 0x12345678;
    test_bis.test.numRxMissed = 0x87654321;
    test_bis.test.numRxFailed = 0xdeadbeef;
    CHECK(ll_bis_read_test_counters(&test_bis, &counters) == 0);
    CHECK(counters.numSuccess == 0x12345678 && counters.numMissed == 0x87654321 && counters.numFailed == 0xdeadbeef);
    test_bis.test.enabled = 0;
    CHECK(ll_bis_read_test_counters(&test_bis, &counters) == 12);

    /* SDK quirk: close walks the BIG list, casts each BIG to BIS and frees it
     * through ll_bis_free. That decrements counters and follows the pointer
     * overlapping BIG.bis_head.prev. */
    le_big_t close_a = {0}, close_b = {0};
    close_a.bis_head.prev = (struct list_head *)&close_b;
    close_b.bis_head.prev = (struct list_head *)&close_a;
    close_a.numBis = 5; close_b.numBis = 6;
    ll_iso_hdl.bis_number = 2;
    INIT_LIST_HEAD(&ll_iso_hdl.big_head);
    close_a.entry.prev = &ll_iso_hdl.big_head; close_a.entry.next = &close_b.entry;
    close_b.entry.prev = &close_a.entry; close_b.entry.next = &ll_iso_hdl.big_head;
    ll_iso_hdl.big_head.next = &close_a.entry; ll_iso_hdl.big_head.prev = &close_b.entry;
    nfreed = 0;
    ll_bis_close();
    CHECK(nfreed == 2 && freed[0] == &close_a && freed[1] == &close_b);
    CHECK(ll_iso_hdl.bis_number == 0 && close_a.numBis == 4 && close_b.numBis == 5);
    CHECK(ll_iso_hdl.big_head.next == &ll_iso_hdl.big_head && ll_iso_hdl.big_head.prev == &ll_iso_hdl.big_head);
    /* Restore the later BIG-free test's list. */
    big1->entry.prev = &ll_iso_hdl.big_head; big1->entry.next = &big2->entry;
    big2->entry.prev = &big1->entry; big2->entry.next = &ll_iso_hdl.big_head;
    ll_iso_hdl.big_head.next = &big1->entry; ll_iso_hdl.big_head.prev = &big2->entry;

    /* Data path. */
    bisB->path = LL_ISO_DATA_PATH_DISABLED; bisB->handle = 0x71;
    CHECK(ll_bis_setup_iso_data_path(bisB, 1, 255) == 0 && stop_calls == 0 && start_calls == 0);   /* same path */
    ll_codec.start = codec_start; ll_codec.stop = codec_stop;
    big1->isoInterUsec = 10000; big1->bn = 3; big1->eventCounter = 0x100000001ull;     /* (eventCounter + 1) * bn, truncated to 32 bits */
    start_result = 1;
    CHECK(ll_bis_setup_iso_data_path(bisB, 1, 1) == 0 && stop_calls == 0 && start_calls == 1);
    CHECK(bisB->path == 1 && start_handle == 0x71 && start_param.dir == 1 && start_param.chMask == 3);
    CHECK(start_param.intervalUsec == 10000 && start_param.pktCtr == (u32)(3ull * 0x100000002ull) && start_param.rdyCback == lctrIsoSendCodecSdu);
    CHECK(ll_bis_setup_iso_data_path(bisB, 1, 1) == 0 && start_calls == 1);             /* unchanged */
    bisB->lastLlid = (LlIsoLlid_t)2;
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 0) == 0 && stop_calls == 1 && stop_handle == 0x71 && bisB->path == 0 && bisB->lastLlid == 2);
    ll_codec.stop = NULL;
    bisB->path = (LlIsoDataPath_t)1;
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 255) == 18 && bisB->path == 1);           /* cannot stop: refused, path unchanged */
    ll_codec.stop = codec_stop;
    start_result = 0;
    bisB->path = (LlIsoDataPath_t)255;
    log_tag_const_e_LL_BIS = 1;
    logs_e = 0;
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 1) == 13 && logs_e == 1 && bisB->path == 255 && start_param.dir == 0);   /* start failed */
    bisB->path = (LlIsoDataPath_t)0;
    ll_codec.start = NULL;
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 1) == 18 && logs_e == 2 && bisB->path == 1);   /* no codec: path was already set */
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 2) == 18 && logs_e == 3 && bisB->path == 2);   /* unknown path */
    CHECK(stop_calls == 2);                    /* leaving path 1 stopped the codec each time */
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 0) == 0 && bisB->path == 0);
    CHECK(ll_bis_setup_iso_data_path(bisB, 0, 255) == 0 && bisB->path == 255);
    log_tag_const_e_LL_BIS = 0;

    /* BIG free: each BIS gets its data path set to disabled (direction by role), then is freed. */
    ll_codec.start = codec_start;
    bisA = ll_bis_alloc(big1);
    bisB = ll_bis_alloc(big1);                 /* bisB handle reuse is irrelevant here */
    big1->bis_head.next = &bisA->entry; big1->bis_head.prev = &bisB->entry;
    bisA->entry.next = &bisB->entry; bisA->entry.prev = &big1->bis_head;
    bisB->entry.next = &big1->bis_head; bisB->entry.prev = &bisA->entry;
    bisA->path = (LlIsoDataPath_t)1; bisB->path = (LlIsoDataPath_t)1;
    big1->role = (LlRole_t)1;
    stop_calls = 0; nfreed = 0;
    unsigned before_big = ll_iso_hdl.big_number;
    ll_big_free(big1);
    CHECK(stop_calls == 2 && nfreed == 3 && freed[nfreed - 3] == bisA && freed[nfreed - 2] == bisB && freed[nfreed - 1] == big1);
    CHECK(ll_iso_hdl.big_number == before_big - 1 && ll_iso_hdl.big_head.next == &big2->entry && big2->entry.prev == &ll_iso_hdl.big_head);
    CHECK(bisA->path == 255 && bisB->path == 255);
    /* role 0 and other roles: other roles skip the data path teardown. */
    le_bis_t *bisC = ll_bis_alloc(big2);
    big2->bis_head.next = big2->bis_head.prev = &bisC->entry;
    bisC->entry.next = bisC->entry.prev = &big2->bis_head;
    bisC->path = (LlIsoDataPath_t)1;
    big2->role = (LlRole_t)2;
    stop_calls = 0;
    ll_big_free(big2);
    CHECK(stop_calls == 0 && ll_iso_hdl.big_head.next == &ll_iso_hdl.big_head);

    /* Group session key: the four CMAC calls chain through the stack buffers. */
    u8 gskd[16], bc[16], gsk[16];
    for (int i = 0; i < 16; i++) { gskd[i] = (u8)(0xa0 + i); bc[i] = (u8)(0xb0 + i); }
    ncmac = 0;
    lctrBisCalcGroupSessionKey(gskd, bc, gsk);
    CHECK(ncmac == 4);
    CHECK(!memcmp(cmac[0].key, "1GIB\0\0\0\0\0\0\0\0\0\0\0\0", 16) && cmac[0].msgp == bc && cmac[0].len == 16);
    CHECK(cmac[1].keyp == cmac[0].out && !memcmp(cmac[1].msg, "2GIB", 4) && cmac[1].len == 4);
    CHECK(cmac[2].keyp == gskd && cmac[2].msgp == cmac[1].out && cmac[2].len == 16);
    CHECK(cmac[3].keyp == cmac[2].out && !memcmp(cmac[3].msg, "3GIB", 4) && cmac[3].len == 4 && cmac[3].out == gsk);
    CHECK(gsk[0] == 0x40 && gsk[15] == 0x4f);

    ll_bis_setup(NULL, 0, 0, 0, (LlPhy_t)0);   /* stub */
    return TEST_DONE("btctrler_ll_bis");
}
#endif
