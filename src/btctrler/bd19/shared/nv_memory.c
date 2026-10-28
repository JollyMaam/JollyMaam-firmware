/* BR23's nonvolatile controller allocator is a 256-byte lbuf pool. The SDK
 * zeroes successful allocations and spins forever when allocation fails. */
#include "btctrler/glue.h"
#include <string.h>

/* Both SDK objects use this private, zero-initialized 256-byte pool in
 * .non_volatile_ram with byte alignment. */
static u8 nv_memory_pool[256]
    __attribute__((section(".non_volatile_ram"), aligned(1)));

void nv_memory_apply(void)
{
    (void)lbuf_init_btctrler(nv_memory_pool, 256);
}

void nv_memory_release(void)
{
    lbuf_free_btctrler(nv_memory_pool);
}

void *nv_memory_malloc(int size)
{
    u8 *p = lbuf_alloc_btctrler((struct lbuff_head_btctrler *)nv_memory_pool,
                                 (u32)size);
    if (p) {
        memset(p, 0, (u32)size);
        return p;
    }
    for (;;) {
    }
}

void nv_memory_free(void *p)
{
    lbuf_free_btctrler(p);
}

#ifdef UNITTEST
#include "test.h"

static u8 allocation[24];
static void *pool;
static void *freed[2];
static u32 allocated_size, pool_size;
static int alloc_calls, init_calls, free_calls;

struct lbuff_head_btctrler *lbuf_init_btctrler(void *buf, u32 buf_len)
{
    pool = buf;
    pool_size = buf_len;
    init_calls++;
    return (struct lbuff_head_btctrler *)buf;
}

void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 len)
{
    CHECK((void *)head == pool);
    allocated_size = len;
    alloc_calls++;
    return allocation;
}

void lbuf_free_btctrler(void *p)
{
    if (free_calls < 2) freed[free_calls] = p;
    free_calls++;
}

int main(void)
{
    memset(allocation, 0xa5, sizeof(allocation));
    nv_memory_apply();
    CHECK(init_calls == 1 && pool_size == 256 && pool != NULL);
    for (u32 i = 0; i < 256; i++) CHECK(((u8 *)pool)[i] == 0);

    CHECK(nv_memory_malloc(7) == allocation);
    CHECK(alloc_calls == 1 && allocated_size == 7);
    for (int i = 0; i < 7; i++) CHECK(allocation[i] == 0);
    CHECK(allocation[7] == 0xa5);

    memset(allocation, 0x5a, sizeof(allocation));
    CHECK(nv_memory_malloc(0) == allocation);
    CHECK(alloc_calls == 2 && allocated_size == 0);
    CHECK(allocation[0] == 0x5a);

    nv_memory_free(allocation);
    nv_memory_release();
    CHECK(free_calls == 2 && freed[0] == allocation && freed[1] == pool);
    return TEST_DONE("btctrler_nv_memory");
}
#endif
