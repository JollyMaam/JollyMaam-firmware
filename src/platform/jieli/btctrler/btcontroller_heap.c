/* SDK heap wrappers: NV memory uses the system heap, zeroes successful
 * allocations, and asserts on failure. __bt_malloc asserts a second time
 * if the first assertion returns. The unused static pools keep SDK sizes. */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SDK globals have no explicit section, are mutable zero-filled BSS,
 * and each has four-byte alignment. */
u8 pNVRAM_static[2048] __attribute__((aligned(4)));
u8 pClassic_mem_pool_static[3072] __attribute__((aligned(4)));
u8 pClassic_hci_mem_pool_static[512] __attribute__((aligned(4)));

#ifdef UNITTEST
static void *heap_test_malloc(size_t size);
static void heap_test_free(void *p);
#define malloc heap_test_malloc
#define free heap_test_free
#endif

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

#ifdef UNITTEST
#include "test.h"
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
