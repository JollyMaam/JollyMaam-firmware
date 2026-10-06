/* Classic packet buffers, from the SDK's link_bulk.c.
 *
 * Two lbuf pools the linker script reserves: transmit bulks (LMP and ACL
 * to the radio) and receive bulks (from the radio).  The link manager's own
 * allocations come from the receive pool too.  Two links share the receive
 * pool: each may be held to a percentage of it, and its use is counted
 * (link[].used_size).  While the receive pool is suspended (the SDK's
 * background mode), receive allocations come from the transmit pool.
 *
 * The SDK could also put the receive pool on the heap
 * (bredr_bulk_malloc_enable); nothing here enables that.  Its unused
 * helpers (reserved allocations, extra references, the mux pool's
 * allocator) are gone with the member. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

struct lbuff_head *lbuf_init(void *buf, u32 len, int align, int priv_head_len);
void *lbuf_alloc(struct lbuff_head *head, u32 len);
void *lbuf_realloc(void *lbuf, int size);
int lbuf_free(void *lbuf);
void lbuf_free_check(void *lbuf, u32 rets);
u32 lbuf_free_space(struct lbuff_head *head);
void lbuf_state(struct lbuff_head *head, struct lbuff_state *state);
int lbuf_avaliable(struct lbuff_head *head, int size);
int lbuf_real_size(void *lbuf);
int os_sem_create(OS_SEM *sem, int count);
int os_sem_pend(OS_SEM *sem, int timeout);
int puts(const char *s);

/* The pools (linker script: 5 * 512 bytes each). */
extern u32 acl_tx_pool[], acl_tx_pool_end[], acl_rx_pool[], acl_rx_pool_end[];
#define POOL_SIZE(pool) ((u32)(((u8 *)pool##_end - (u8 *)pool) / 4 * 4))

/* The bulk header before the payload: 28 bytes for both kinds. */
#define BULK_HEAD offsetof(struct bredr_rx_bulk, data)
#ifndef UNITTEST
_Static_assert(offsetof(struct bredr_rx_bulk, data) == 28 &&
               offsetof(struct bredr_tx_bulk, data) == 28, "SDK bulk header size");
#endif

/* The pools in use (sdk.h's types: lbuf heads at word-aligned memory). */
u32 *rx_bulk, *tx_bulk, *remain_rx_bulk;
u32 rx_bulk_size, tx_bulk_size, remain_rx_bulk_size;
#define LBUF(pool) ((struct lbuff_head *)(pool))
static struct link_bulk_used link[2];
static OS_SEM acl_tx_bulk_sem;          /* posted when a transmit bulk is freed */
static volatile bool rx_bulk_suspend, in_background_status;

/* The rets register: the caller's return address, for lbuf's double-free
 * report. */
#ifndef UNITTEST
static inline u32 return_address(void)
{
    u32 rets;
    __asm__ volatile("%0 = rets ;" : "=r"(rets));
    return rets;
}
#else
#define return_address() 0u
#endif

static bool pool_empty(u32 *pool, u32 size)
{
    struct lbuff_state st;
    lbuf_state(LBUF(pool), &st);
    return st.avaliable + sizeof(struct lbuff_head) == size;
}

/* ---- set-up ----------------------------------------------------------- */

int link_bulk_init(void)
{
    os_sem_create(&acl_tx_bulk_sem, 0);
    memset(link, 0, sizeof(link));
    rx_bulk_suspend = 0;
    rx_bulk = acl_rx_pool;
    tx_bulk = acl_tx_pool;
    rx_bulk_size = POOL_SIZE(acl_rx_pool);
    tx_bulk_size = POOL_SIZE(acl_tx_pool);
    lbuf_init(acl_tx_pool, tx_bulk_size, 4, 0);
    lbuf_init(rx_bulk, rx_bulk_size, 4, 0);
    remain_rx_bulk = 0;
    return 0;
}

/* Mode 0 swaps the pools (transmit from the receive pool and back); any
 * other mode puts them back. */
int link_change_bulk_init(u8 mode)
{
    local_irq_disable();
    memset(link, 0, sizeof(link));
    rx_bulk_suspend = 0;
    if (mode) {
        tx_bulk = acl_tx_pool; tx_bulk_size = POOL_SIZE(acl_tx_pool);
        rx_bulk = acl_rx_pool; rx_bulk_size = POOL_SIZE(acl_rx_pool);
    } else {
        tx_bulk = acl_rx_pool; tx_bulk_size = POOL_SIZE(acl_rx_pool);
        rx_bulk = acl_tx_pool; rx_bulk_size = POOL_SIZE(acl_tx_pool);
    }
    lbuf_init(tx_bulk, tx_bulk_size, 4, 0);
    lbuf_init(rx_bulk, rx_bulk_size, 4, 0);
    local_irq_enable();
    return 0;
}

/* A nonzero mode splits off the receive pool's upper part as the "mux"
 * pool.  As in the SDK, its first part is half the pool less 2 KB: this
 * needs a receive pool over 4 KB (ours is 2.5 KB; nothing calls it). */
int link_change_rx_bulk_size(u8 mode)
{
    local_irq_disable();
    memset(link, 0, sizeof(link));
    rx_bulk_suspend = 0;
    rx_bulk = acl_rx_pool;
    if (mode) {
        u32 words = (u32)(acl_rx_pool_end - acl_rx_pool);
        rx_bulk_size = POOL_SIZE(acl_rx_pool) / 2 - 2048;
        remain_rx_bulk = acl_rx_pool + (words / 2 - 512);
        remain_rx_bulk_size = POOL_SIZE(acl_rx_pool) - rx_bulk_size;
        lbuf_init(remain_rx_bulk, remain_rx_bulk_size, 4, 0);
    } else {
        rx_bulk_size = POOL_SIZE(acl_rx_pool);
        remain_rx_bulk = 0;
        remain_rx_bulk_size = 0;
    }
    lbuf_init(rx_bulk, rx_bulk_size, 4, 0);
    local_irq_enable();
    return 0;
}

int bredr_rx_remain_bulk_empty(void) { return pool_empty(remain_rx_bulk, remain_rx_bulk_size); }

/* ---- background mode (btctrler_suspend/resume) ------------------------ */

void bredr_goto_background(void) { in_background_status = 1; }
void bredr_goback_from_background(void) { in_background_status = 0; }
void bredr_rx_bulk_suspend(void) { rx_bulk_suspend = 1; }
void bredr_rx_bulk_resume(void)
{
    if (rx_bulk_suspend) {
        lbuf_init(rx_bulk, rx_bulk_size, 4, 0);
        rx_bulk_suspend = 0;
    }
    bredr_goback_from_background();
}

/* ---- link manager memory ---------------------------------------------- */

/* Zeroed memory from the receive pool.  While it is suspended: from the
 * transmit pool, or (task context) wait for the pool to come back. */
void *lmp_malloc(int size)
{
    if (rx_bulk_suspend) {
        void *p = bt_malloc(size);
        if (p || cpu_in_irq()) return p;
        do os_time_dly(2); while (rx_bulk_suspend);
    }
    void *p = lbuf_alloc(LBUF(rx_bulk), size);
    if (p) memset(p, 0, size);
    return p;
}

void lmp_free(void *buf)
{
    lbuf_free_check(buf, return_address());
    lbuf_free(buf);
}

/* Zeroed memory from the transmit pool (the radio's frame state). */
void *bt_malloc(int size)
{
    void *p = lbuf_alloc(LBUF(tx_bulk), size);
    if (p) memset(p, 0, size);
    return p;
}

void bt_free(void *buf)
{
    lbuf_free_check(buf, return_address());
    lbuf_free(buf);
}

/* ---- receive bulks ---------------------------------------------------- */

/* The SDK printed the pools' state here (the radio calls it when a receive
 * allocation fails). */
void bredr_rx_bulk_state(void) {}

static u32 share(int id) { return rx_bulk_size * link[id].persent / 100; }

/* Receive space a link may still use: at most its share, in whole bulks
 * of unit_size; 0 if less than one and a half are left. */
int bredr_rx_bulk_total_size(int id) { return link[id].persent ? (int)share(id) : (int)rx_bulk_size; }

int bredr_rx_bulk_remain_size(int id, int unit_size)
{
    int bulk = unit_size + (int)BULK_HEAD;
    if (rx_bulk_suspend) return 0;
    int remain = 0;
    if (link[id].persent) {
        remain = (int)(share(id) - link[id].used_size);
        if (remain <= bulk / 2 + bulk) return 0;
    }
    int fit = lbuf_avaliable(LBUF(rx_bulk), bulk) * unit_size;
    if (!remain || fit < remain) return fit;
    return remain - remain % bulk;
}

void bredr_rx_bulk_set_max_used_persent(int id, int persent)
{
    if (id >= 2) { btctrler_assert_failed("rx bulk link id", id); return; }
    link[id].persent = persent;
}

struct bredr_rx_bulk *__bredr_rx_bulk_alloc(void *pool, int size)
{
    struct bredr_rx_bulk *bulk = lbuf_alloc(pool, size + BULK_HEAD);
    if (bulk) memset(bulk, 0, BULK_HEAD);
    return bulk;
}

static struct bredr_rx_bulk *count(struct bredr_rx_bulk *bulk, int id)
{
    if (bulk) {
        bulk->id = id;
        link[id].used_size += lbuf_real_size(bulk);
    }
    return bulk;
}

/* Within the link's share. */
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id, int len)
{
    if (id >= 2) { btctrler_assert_failed("rx bulk link id", id); return 0; }
    if (link[id].persent && !((u32)link[id].used_size < share(id))) return 0;
    return count(__bredr_rx_bulk_alloc(rx_bulk_suspend ? tx_bulk : rx_bulk, len), id);
}

/* Regardless of the share, falling back to the transmit pool. */
struct bredr_rx_bulk *bredr_rx_bulk_alloc_for(int id, int len)
{
    struct bredr_rx_bulk *bulk = rx_bulk_suspend ? 0 : __bredr_rx_bulk_alloc(rx_bulk, len);
    if (!bulk) bulk = __bredr_rx_bulk_alloc(tx_bulk, len);
    return count(bulk, id);
}

void bredr_rx_bulk_push(struct list_head *head, struct bredr_rx_bulk *bulk)
{
    local_irq_disable();
    bulk->entry.prev = head->prev;
    bulk->entry.next = head;
    head->prev->next = &bulk->entry;
    head->prev = &bulk->entry;
    local_irq_enable();
}

struct bredr_rx_bulk *bredr_rx_bulk_pop(struct list_head *head)
{
    struct bredr_rx_bulk *bulk = 0;
    local_irq_disable();
    struct list_head *entry = head->next;
    if (entry != head) {
        bulk = (void *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry));
        entry->next->prev = entry->prev;
        entry->prev->next = entry->next;
    }
    local_irq_enable();
    return bulk;
}

/* A bulk may sit head_offset bytes into its allocation. */
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk)
{
    if (!(config_btctler_modules & 1)) return;
    int id = bulk->id;
    void *lbuf = (u8 *)bulk - bulk->head_offset;
    lbuf_free_check(lbuf, return_address());
    int size = lbuf_real_size(lbuf);
    if (lbuf_free(lbuf)) {
        s16 used = link[id].used_size - size;
        link[id].used_size = used > 0 ? used : 0;
    }
}

int bredr_rx_bulk_empty(void) { return pool_empty(rx_bulk, rx_bulk_size); }

/* ---- transmit bulks --------------------------------------------------- */

int bredr_tx_bulk_empty(void) { return pool_empty(tx_bulk, tx_bulk_size); }
u32 get_bredr_tx_remain_size(void) { return lbuf_free_space(LBUF(tx_bulk)); }

/* A zeroed bulk header and size bytes of payload.  In task context, wait
 * for a free (bredr_tx_bulk_free posts); with interrupts off, NULL. */
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    if (!(config_btctler_modules & 1)) return 0;
    for (;;) {
        struct bredr_tx_bulk *tx = lbuf_alloc(LBUF(tx_bulk), size + BULK_HEAD);
        if (tx) {
            memset(tx, 0, BULK_HEAD);
            tx->entry.next = tx->entry.prev = &tx->entry;
            return tx;
        }
        if (cpu_irq_disabled() || cpu_in_irq()) return 0;
        puts("pend_for_tx_bulk\n");
        if (os_sem_pend(&acl_tx_bulk_sem, 0)) return 0;
    }
}

void bredr_tx_bulk_realloc(struct bredr_tx_bulk *bulk, int size) { lbuf_realloc(bulk, size + BULK_HEAD); }

void bredr_tx_bulk_push(struct list_head *head, struct bredr_tx_bulk *bulk)
{
    local_irq_disable();
    bulk->entry.prev = head->prev;
    bulk->entry.next = head;
    head->prev->next = &bulk->entry;
    head->prev = &bulk->entry;
    local_irq_enable();
}

struct bredr_tx_bulk *bredr_tx_bulk_pop(struct list_head *head)
{
    struct list_head *entry = head->next;
    return entry == head ? 0 : (void *)((u8 *)entry - offsetof(struct bredr_tx_bulk, entry));
}

/* Unlinks the bulk.  A bulk made from a received one (rx_head_offset: the
 * receive bulk sits that far in, and points back) frees that one; a held
 * bulk stays allocated. */
void bredr_tx_bulk_free(struct bredr_tx_bulk *bulk)
{
    if (!(config_btctler_modules & 1)) return;
    lbuf_free_check(bulk, return_address());
    local_irq_disable();
    bulk->entry.next->prev = bulk->entry.prev;
    bulk->entry.prev->next = bulk->entry.next;
    local_irq_enable();
    if (bulk->rx_head_offset) {
        struct bredr_rx_bulk *rx = (void *)((u8 *)bulk + bulk->rx_head_offset);
        if (rx->head_offset != bulk->rx_head_offset)
            btctrler_assert_failed("(u8 *)rx - rx->head_offset == (u8 *)bulk", bulk->rx_head_offset);
        bredr_rx_bulk_free(rx);
    } else if (!bulk->hold) {
        lbuf_free(bulk);
        os_sem_post(&acl_tx_bulk_sem);
    }
}

#ifdef UNITTEST
#include <stdlib.h>
#include "test.h"
int config_btctler_modules = 1, config_asser;
/* 2560-byte pools with linker-style end symbols, as in the firmware. */
u32 acl_tx_pool[640], acl_rx_pool[640];
__asm__(".globl acl_tx_pool_end\n.set acl_tx_pool_end, acl_tx_pool + 2560\n"
        ".globl acl_rx_pool_end\n.set acl_rx_pool_end, acl_rx_pool + 2560\n");
static int asserts, posts, pends, irq_disabled_flag, in_irq_flag, irq_depth;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void local_irq_disable(void) { irq_depth++; }
void local_irq_enable(void) { irq_depth--; }
int cpu_in_irq(void) { return in_irq_flag; }
int cpu_irq_disabled(void) { return irq_disabled_flag; }
int os_sem_create(OS_SEM *s, int c) { (void)s; (void)c; return 0; }
int os_sem_pend(OS_SEM *s, int t) { (void)s; (void)t; pends++; return -1; }
int os_sem_post(OS_SEM *s) { (void)s; posts++; return 0; }
void os_time_dly(int t) { (void)t; rx_bulk_suspend = 0; }
int puts(const char *s) { (void)s; return 0; }
/* Fake lbuf: a pool is identified by its head; allocations carry a header. */
struct fake { void *pool; int size; };
static struct { void *pool; u32 cap, used; } pools[4];
static struct { void *pool; u32 cap, used; } *pool_of(void *head)
{
    for (int i = 0; i < 4; i++) if (pools[i].pool == head) return (void *)&pools[i];
    CHECK(0); return 0;
}
struct lbuff_head *lbuf_init(void *buf, u32 len, int align, int priv)
{
    (void)align; (void)priv;
    for (int i = 0; i < 4; i++) if (!pools[i].pool || pools[i].pool == buf) {
        pools[i].pool = buf; pools[i].cap = len; pools[i].used = 0; return buf;
    }
    return 0;
}
static int rounded(u32 len) { return (len + 3) & ~3u; }
void *lbuf_alloc(struct lbuff_head *head, u32 len)
{
    __typeof__(pools[0]) *p = (void *)pool_of(head);
    if (p->used + rounded(len) + 36 > p->cap) return 0;
    struct fake *f = malloc(sizeof(*f) + len + 64); f->pool = head; f->size = rounded(len);
    p->used += f->size; memset(f + 1, 0xa5, len); return f + 1;
}
int lbuf_real_size(void *lbuf) { return ((struct fake *)lbuf - 1)->size; }
int lbuf_free(void *lbuf)
{
    struct fake *f = (struct fake *)lbuf - 1;
    ((__typeof__(pools[0]) *)(void *)pool_of(f->pool))->used -= f->size; free(f); return 1;
}
void lbuf_free_check(void *lbuf, u32 rets) { (void)lbuf; (void)rets; }
void *lbuf_realloc(void *lbuf, int size) { ((struct fake *)lbuf - 1)->size = rounded(size); return lbuf; }
u32 lbuf_free_space(struct lbuff_head *head) { __typeof__(pools[0]) *p = (void *)pool_of(head); return p->cap - p->used - 36; }
void lbuf_state(struct lbuff_head *head, struct lbuff_state *st)
{ __typeof__(pools[0]) *p = (void *)pool_of(head); st->avaliable = p->cap - p->used - sizeof(struct lbuff_head); }
int lbuf_avaliable(struct lbuff_head *head, int size)
{ __typeof__(pools[0]) *p = (void *)pool_of(head); return (int)(p->cap - p->used - 36) / rounded(size); }
int main(void)
{
    link_bulk_init();
    CHECK(rx_bulk == acl_rx_pool && tx_bulk == acl_tx_pool && rx_bulk_size == 2560 && tx_bulk_size == 2560);
    CHECK(bredr_rx_bulk_empty() && bredr_tx_bulk_empty() && !irq_depth);

    /* Shares: link 0 limited to 10% (256 bytes) of the receive pool. */
    bredr_rx_bulk_set_max_used_persent(0, 10);
    CHECK(bredr_rx_bulk_total_size(0) == 256 && bredr_rx_bulk_total_size(1) == 2560);
    struct bredr_rx_bulk *a = bredr_rx_bulk_alloc(0, 100);
    CHECK(a && a->id == 0 && link[0].used_size == (s16)rounded(100 + BULK_HEAD));
    struct bredr_rx_bulk *b = bredr_rx_bulk_alloc(0, 100);
    CHECK(b && !bredr_rx_bulk_alloc(0, 100));                       /* share used up */
    CHECK(!bredr_rx_bulk_remain_size(0, 100));
    struct bredr_rx_bulk *c = bredr_rx_bulk_alloc_for(0, 100);    /* regardless of share */
    CHECK(c && link[0].used_size > 256);
    struct bredr_rx_bulk *d = bredr_rx_bulk_alloc(1, 40);
    CHECK(d && d->id == 1 && !d->llid && link[1].used_size == (s16)rounded(40 + BULK_HEAD));
    bredr_rx_bulk_free(a); bredr_rx_bulk_free(b); bredr_rx_bulk_free(c);
    CHECK(link[0].used_size == 0);
    int remain = bredr_rx_bulk_remain_size(0, 100);
    CHECK(remain == 256 - 256 % (100 + (int)BULK_HEAD));
    CHECK(bredr_rx_bulk_remain_size(1, 100) == lbuf_avaliable(LBUF(rx_bulk), 100 + BULK_HEAD) * 100);
    bredr_rx_bulk_free(d); CHECK(bredr_rx_bulk_empty() && !link[1].used_size);
    bredr_rx_bulk_set_max_used_persent(2, 1); CHECK(asserts == 1); asserts = 0;
    link[0].persent = 0;

    /* Queues. */
    struct list_head q = { &q, &q };
    a = bredr_rx_bulk_alloc(0, 8); b = bredr_rx_bulk_alloc(0, 8);
    bredr_rx_bulk_push(&q, a); bredr_rx_bulk_push(&q, b);
    CHECK(bredr_rx_bulk_pop(&q) == a && bredr_rx_bulk_pop(&q) == b && !bredr_rx_bulk_pop(&q) && !irq_depth);
    bredr_rx_bulk_free(a); bredr_rx_bulk_free(b);

    /* Suspended receive pool: allocations from the transmit pool. */
    bredr_rx_bulk_suspend();
    a = bredr_rx_bulk_alloc(1, 8); CHECK(a && ((struct fake *)a - 1)->pool == (void *)acl_tx_pool);
    CHECK(!bredr_rx_bulk_remain_size(1, 8));
    void *m = lmp_malloc(16); CHECK(m && ((struct fake *)m - 1)->pool == (void *)acl_tx_pool);
    lmp_free(m); bredr_rx_bulk_free(a);
    bredr_goto_background(); bredr_rx_bulk_resume(); CHECK(!rx_bulk_suspend && !in_background_status);
    m = lmp_malloc(16); CHECK(((struct fake *)m - 1)->pool == (void *)acl_rx_pool && !((u8 *)m)[15]); lmp_free(m);

    /* Transmit bulks: header zeroed, entry self-linked; a full pool waits
     * in task context, fails with interrupts off. */
    struct bredr_tx_bulk *t = bredr_tx_bulk_alloc(17);
    CHECK(t && !t->len && t->entry.next == &t->entry && !t->hold);
    struct list_head tq = { &tq, &tq };
    bredr_tx_bulk_push(&tq, t); CHECK(bredr_tx_bulk_pop(&tq) == t && bredr_tx_bulk_pop(&tq) == t);
    bredr_tx_bulk_free(t); CHECK(tq.next == &tq && posts == 1 && bredr_tx_bulk_empty());
    t = bredr_tx_bulk_alloc(17); t->hold = 1; bredr_tx_bulk_free(t); CHECK(posts == 1 && !bredr_tx_bulk_empty());
    t->hold = 0; t->entry.next = t->entry.prev = &t->entry; bredr_tx_bulk_free(t); CHECK(posts == 2);
    CHECK(!bredr_tx_bulk_alloc(5000) && pends == 1);
    irq_disabled_flag = 1; CHECK(!bredr_tx_bulk_alloc(5000) && pends == 1); irq_disabled_flag = 0;
    /* A transmit bulk made from a received one frees it: the receive bulk
     * sits rx_head_offset bytes in and points back. */
    t = bredr_tx_bulk_alloc(64);
    struct bredr_rx_bulk *inner = (void *)((u8 *)t + 32);
    memset(inner, 0, sizeof(*inner)); inner->head_offset = 32; inner->id = 1;
    t->rx_head_offset = 32; t->entry.next = t->entry.prev = &t->entry;
    link[1].used_size = 500;
    bredr_tx_bulk_free(t);
    CHECK(!asserts && posts == 2 && bredr_tx_bulk_empty() && link[1].used_size == 500 - rounded(64 + BULK_HEAD));

    /* Pool swap and back. */
    link_change_bulk_init(0); CHECK(tx_bulk == acl_rx_pool && rx_bulk == acl_tx_pool && !irq_depth);
    link_change_bulk_init(1); CHECK(tx_bulk == acl_tx_pool && rx_bulk == acl_rx_pool);
    link_change_rx_bulk_size(0); CHECK(!remain_rx_bulk && rx_bulk_size == 2560);
    return TEST_DONE("btctrler_link_bulk");
}
#endif
