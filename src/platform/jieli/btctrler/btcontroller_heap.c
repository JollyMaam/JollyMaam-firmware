/* SDK heap wrappers: NV memory uses the system heap, zeroes successful
 * allocations, and asserts on failure. __bt_malloc asserts a second time
 * if the first assertion returns. The unused static pools keep SDK sizes. */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* All three SDK pools exist in both target members as aligned BSS. */
u8 pNVRAM_static[2048] __attribute__((aligned(4)));
u8 pClassic_mem_pool_static[3072] __attribute__((aligned(4)));
u8 pClassic_hci_mem_pool_static[512] __attribute__((aligned(4)));

#ifdef UNITTEST
static void *heap_test_malloc(size_t size);
static void heap_test_free(void *p);
#define malloc heap_test_malloc
#define free heap_test_free
#endif

#ifdef CONFIG_CPU_BD19

/* BD19's SDK keeps two classic-controller lbuf pools here. The flag and
 * pointers are object-local state in btcontroller_heap.c.o. */
static u8 classic_apply __attribute__((aligned(4)));
static struct lbuff_head_btctrler *classic_pool __attribute__((aligned(4)));
static struct lbuff_head_btctrler *classic_hci_pool __attribute__((aligned(4)));

static void classic_assert(int line, const char *function, const char *message)
{
    printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/btcontroller_heap.c", line);
    printf(message, function);
    cpu_assert_debug();
}

void btctler_memory_apply(u8 mode)
{
    if (mode & 1) {
        classic_pool = (struct lbuff_head_btctrler *)pClassic_mem_pool_static;
        (void)lbuf_init_btctrler(pClassic_mem_pool_static, sizeof pClassic_mem_pool_static);
        classic_hci_pool = (struct lbuff_head_btctrler *)pClassic_hci_mem_pool_static;
        (void)lbuf_init_btctrler(pClassic_hci_mem_pool_static, sizeof pClassic_hci_mem_pool_static);
        classic_apply = 1;
    }
}

void btctler_memory_release(u8 mode)
{
    if (mode & 1) {
        classic_apply = 0;
        if (classic_hci_pool) {
            lbuf_clear_btctrler(classic_hci_pool);
            classic_hci_pool = NULL;
        }
        if (classic_pool) {
            lbuf_clear_btctrler(classic_pool);
            classic_pool = NULL;
        }
    }
}

void *bt_classic_malloc(u8 type, int size)
{
    /* The SDK snapshots both fields before any assertion/reset callback. */
    int asser = config_asser;
    u8 applied = classic_apply;
    if (!asser && !applied)
        p33_soft_reset();
    else if (asser && !applied)
        classic_assert(102, "bt_classic_malloc", "ASSERT-FAILD: __this->classic_apply %s");

    if (type == 0) {
        void *p = lbuf_alloc_btctrler(classic_pool, size);
        if (!p) {
            for (;;) { }
        }
        memset(p, 0, size);
        return p;
    }
    if (type == 1) {
        void *p = lbuf_alloc_btctrler(classic_hci_pool, size);
        if (!p && asser) {
            classic_assert(116, "bt_classic_malloc", "ASSERT-FAILD: p != NULL %s\r\n");
            return NULL;
        }
        if (!p)
            p33_soft_reset();
        return p;
    }
    if (!asser)
        p33_soft_reset();
    else {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/btcontroller_heap.c", 118);
        puts("ASSERT-FAILD: 0 bt_malloc error\r");
        cpu_assert_debug();
    }
    return NULL;
}

void bt_classic_free(void *p)
{
    int asser = config_asser;
    u8 applied = classic_apply;
    if (!asser && !applied)
        p33_soft_reset();
    else if (asser && !applied)
        classic_assert(127, "bt_classic_free", "ASSERT-FAILD: __this->classic_apply %s\r\n");
    lbuf_free_btctrler(p);
}

void btctler_nv_memory_release(void)
{
}

void btctler_nv_memory_debug(u8 flag)
{
    (void)flag;
}

void btctler_nv_memory_apply(void)
{
}

void *btctler_nv_memory_malloc(int size)
{
    void *p = malloc(size);
    if (p) {
        memset(p, 0, size);
    } else {
        u32 rets;
#ifndef UNITTEST
        __asm__ volatile("%0 = rets" : "=r"(rets));
#else
        rets = 0;
#endif
        printf("RETS = %08x \r\n", rets);
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/btcontroller_heap.c", 237);
            printf("ASSERT-FAILD: 0 nv-ram malloc null");
            cpu_assert_debug();
        } else {
            p33_soft_reset();
        }
    }
    return p;
}

void btctler_nv_memory_free(void *p)
{
    if (p)
        free(p);
}

void *__bt_malloc(int size)
{
    void *p = btctler_nv_memory_malloc(size);
    if (!config_asser && !p)
        p33_soft_reset();
    else if (config_asser && !p) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/btcontroller_heap.c", 263);
        printf("ASSERT-FAILD: p != NULL ");
        cpu_assert_debug();
    }
    return p;
}

void __bt_free(void *p)
{
    if (p)
        free(p);
}

#else

void btctler_nv_memory_apply(void)
{
}

void *btctler_nv_memory_malloc(int size)
{
    void *p = malloc(size);
    if (p) {
        memset(p, 0, size);
    } else {
        u32 rets;
#ifndef UNITTEST
        __asm__ volatile("%0 = rets" : "=r"(rets));
#else
        rets = 0;
#endif
        printf("RETS = %08x \r\n", rets);
        btctrler_assert_failed("0 nv-ram malloc null", 0);
    }
    return p;
}

void btctler_nv_memory_free(void *p)
{
    free(p);
}

void *__bt_malloc(int size)
{
    void *p = btctler_nv_memory_malloc(size);
    if (!p)
        btctrler_assert_failed("p != NULL", 0);
    return p;
}

void __bt_free(void *p)
{
    btctler_nv_memory_free(p);
}

#endif /* CONFIG_CPU_BD19 */

#ifdef UNITTEST
#include "test.h"
#ifdef CONFIG_CPU_BD19
static u8 classic_allocation[64], hci_allocation[64];
static int fail_alloc, change_config_on_alloc, assertions, resets, initializes, clears, frees;
static size_t requested;
static u8 *last_pool;
static void *heap_test_malloc(size_t size)
{
    requested = size;
    return fail_alloc ? NULL : classic_allocation;
}
static void heap_test_free(void *p) { (void)p; frees++; }
int config_asser = 1;
void p33_soft_reset(void) { resets++; }
void cpu_assert_debug(void) { assertions++; }
struct lbuff_head_btctrler *lbuf_init_btctrler(void *buf, u32 len)
{
    CHECK((len == 3072 && buf == pClassic_mem_pool_static) ||
          (len == 512 && buf == pClassic_hci_mem_pool_static));
    initializes++;
    return (struct lbuff_head_btctrler *)buf;
}
void lbuf_clear_btctrler(struct lbuff_head_btctrler *head) { (void)head; clears++; }
void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 len)
{
    requested = len;
    last_pool = (u8 *)head;
    if (change_config_on_alloc) config_asser = 0;
    if (fail_alloc) return NULL;
    return head == (struct lbuff_head_btctrler *)pClassic_hci_mem_pool_static
        ? hci_allocation : classic_allocation;
}
void lbuf_free_btctrler(void *p) { (void)p; frees++; }
int main(void)
{
    memset(classic_allocation, 0xa5, sizeof classic_allocation);
    btctler_memory_apply(2);
    CHECK(initializes == 0);
    btctler_memory_apply(1);
    CHECK(initializes == 2);
    CHECK(bt_classic_malloc(0, 7) == classic_allocation);
    CHECK(requested == 7 && last_pool == pClassic_mem_pool_static);
    for (int i = 0; i < 7; ++i) CHECK(classic_allocation[i] == 0);
    CHECK(classic_allocation[7] == 0xa5);
    CHECK(bt_classic_malloc(1, 5) == hci_allocation);
    CHECK(last_pool == pClassic_hci_mem_pool_static);
    fail_alloc = 1;
    CHECK(bt_classic_malloc(1, 9) == NULL && assertions == 1);
    config_asser = 1;
    change_config_on_alloc = 1;
    CHECK(bt_classic_malloc(1, 9) == NULL && assertions == 2 && resets == 0);
    change_config_on_alloc = 0;
    config_asser = 1;
    CHECK(bt_classic_malloc(2, 9) == NULL && assertions == 3);
    fail_alloc = 0;
    CHECK(btctler_nv_memory_malloc(0) == classic_allocation && requested == 0);
    CHECK(btctler_nv_memory_malloc(7) == classic_allocation && requested == 7);
    fail_alloc = 1;
    CHECK(btctler_nv_memory_malloc(1) == NULL && assertions == 4);
    CHECK(__bt_malloc(1) == NULL && assertions == 6);
    btctler_nv_memory_free(NULL);
    __bt_free(NULL);
    CHECK(frees == 0);
    bt_classic_free(NULL);
    CHECK(frees == 1);
    btctler_memory_release(2);
    CHECK(clears == 0);
    btctler_memory_release(1);
    CHECK(clears == 2);
    config_asser = 0;
    CHECK(bt_classic_malloc(1, 9) == NULL && resets == 2);
    bt_classic_free(NULL);
    CHECK(resets == 3 && frees == 2);
    btctler_memory_apply(1);
    btctler_memory_release(1);
    CHECK(resets == 3);
    return TEST_DONE("btctrler_btcontroller_heap_bd19");
}
#else
static u8 allocation[32];
static size_t requested;
static int fail_alloc, assertions, frees;
static void *freed;
static const char *assertion_messages[3];
static void *heap_test_malloc(size_t size)
{
    requested = size;
    return fail_alloc ? NULL : allocation;
}
static void heap_test_free(void *p)
{
    freed = p;
    frees++;
}
void btctrler_assert_failed(const char *what, int value)
{
    CHECK(value == 0);
    CHECK(strcmp(what, "0 nv-ram malloc null") == 0 ||
          strcmp(what, "p != NULL") == 0);
    if (assertions < 3) assertion_messages[assertions] = what;
    assertions++;
}
int main(void)
{
    /* IR memset length is exactly size; zero and odd lengths have no
     * rounding. The allocator sees the signed int converted to size_t. */
    memset(allocation, 0xa5, sizeof allocation);
    CHECK(btctler_nv_memory_malloc(0) == allocation);
    CHECK(requested == 0 && allocation[0] == 0xa5);
    CHECK(btctler_nv_memory_malloc(7) == allocation);
    CHECK(requested == 7);
    for (int i = 0; i < 7; i++) CHECK(allocation[i] == 0);
    CHECK(allocation[7] == 0xa5);
    CHECK(__bt_malloc(32) == allocation);
    for (int i = 0; i < 32; i++) CHECK(allocation[i] == 0);
    CHECK(assertions == 0);
    fail_alloc = 1;
    CHECK(btctler_nv_memory_malloc(1) == NULL && assertions == 1);
    CHECK(__bt_malloc(1) == NULL && assertions == 3);
    CHECK(strcmp(assertion_messages[0], "0 nv-ram malloc null") == 0);
    CHECK(strcmp(assertion_messages[1], "0 nv-ram malloc null") == 0);
    CHECK(strcmp(assertion_messages[2], "p != NULL") == 0);
    btctler_nv_memory_free(allocation);
    CHECK(frees == 1 && freed == allocation);
    __bt_free(NULL);
    CHECK(frees == 2 && freed == NULL);
    btctler_nv_memory_apply();
    return TEST_DONE("btctrler_btcontroller_heap");
}
#endif
#endif
