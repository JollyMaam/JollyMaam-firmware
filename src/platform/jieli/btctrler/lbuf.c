/* The controller's block allocator and packet queues, from the SDK's
 * lbuf.c (the *_btctrler variant, not the system library's lbuf).
 *
 * A pool is a struct lbuff_head_btctrler followed by blocks.  Free blocks
 * (struct hfree) sit on the free list in address order and merge with
 * their neighbours when freed; allocation takes the first that fits,
 * splitting it unless the rest would be too small to hold a header.  An
 * allocated block starts with a struct hentry; callers get the memory
 * after it.  A pushed block is on its pool's queue, where pop marks it
 * taken (state 1) without removing it: freeing does.  The *_queue
 * functions keep blocks on a caller's list instead. */
#include <stddef.h>
#include "btctrler/glue.h"

#define HENTRY sizeof(struct hentry)        /* 16 */
#ifndef UNITTEST
_Static_assert(sizeof(struct hentry) == 16 && sizeof(struct hfree) == 12 &&
               sizeof(struct lbuff_head_btctrler) == 16, "SDK lbuf headers");
#endif

static void list_init(struct list_head *e) { e->next = e->prev = e; }
static void list_unlink(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; }
static void list_insert_before(struct list_head *e, struct list_head *at)
{
    e->prev = at->prev;
    e->next = at;
    at->prev->next = e;
    at->prev = e;
}

static struct hentry *entry_of(void *lbuf) { return (struct hentry *)((u8 *)lbuf - HENTRY); }
static u32 round4(u32 n) { return (n + 3) & ~3u; }

/* The pool starts at the first word boundary in buf; its single free block
 * leaves 12 bytes of buf unused, as in the SDK (len - 28). */
struct lbuff_head_btctrler *lbuf_init_btctrler(void *buf, u32 buf_len)
{
    u32 misalign = (uintptr_t)buf & 3;
    if (misalign) {
        buf = (u8 *)buf + 4 - misalign;
        buf_len -= 4 - misalign;
    }
    struct lbuff_head_btctrler *head = buf;
    struct hfree *block = (struct hfree *)(head + 1);
    block->len = buf_len - sizeof(*head) - 12;
    list_init(&head->head);
    list_init(&head->free);
    list_insert_before(&block->entry, &head->free);
    return head;
}

void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 len)
{
    u32 need = round4(len + HENTRY);
    void *ret = 0;
    local_irq_disable();
    for (struct list_head *e = head->free.next; e != &head->free; e = e->next) {
        struct hfree *p = (struct hfree *)e;
        if (p->len < need) continue;
        if (p->len > need + sizeof(struct hfree)) {
            /* The rest takes the block's place on the free list. */
            struct hfree *rest = (struct hfree *)((u8 *)p + need);
            rest->len = p->len - need;
            rest->entry.next = p->entry.next;
            rest->entry.prev = p->entry.prev;
            p->entry.next->prev = &rest->entry;
            p->entry.prev->next = &rest->entry;
        } else {
            list_unlink(&p->entry);
            need = p->len;
        }
        struct hentry *a = (struct hentry *)p;
        a->head = head;
        a->state = 0;
        a->len = need;
        a->ref = 0;
        list_init(&a->entry);
        ret = a + 1;
        break;
    }
    local_irq_enable();
    return ret;
}

/* Shrinks only: the end goes back to the pool if it can hold a block. */
void *lbuf_realloc_btctrler(void *lbuf, int size)
{
    struct hentry *a = entry_of(lbuf);
    size = round4(size);
    if (!(size < a->len)) btctrler_assert_failed("lbuf realloc grows", size);
    if ((u32)size > (u32)(a->len - HENTRY)) return 0;
    local_irq_disable();
    if ((u32)(size + 2 * HENTRY) < a->len) {
        struct hentry *tail = (struct hentry *)((u8 *)lbuf + size);
        tail->head = a->head;
        tail->len = a->len - size - HENTRY;
        tail->ref = 0;
        list_init(&tail->entry);
        a->len = size + HENTRY;
        lbuf_free_btctrler(tail + 1);
    }
    local_irq_enable();
    return lbuf;
}

void lbuf_free_btctrler(void *lbuf)
{
    if (!lbuf) return;
    local_irq_disable();
    struct hentry *a = entry_of(lbuf);
    struct lbuff_head_btctrler *head = a->head;
    list_unlink(&a->entry);
    struct hfree *block = (struct hfree *)a;
    block->len = a->len;
    struct list_head *e;
    for (e = head->free.next; e != &head->free; e = e->next) {
        struct hfree *p = (struct hfree *)e;
        if ((u8 *)p > (u8 *)block) break;
        if ((u8 *)p + p->len > (u8 *)block) goto out;   /* inside a free block: double free */
    }
    list_insert_before(&block->entry, e);
    /* Merge each free block that ends where the next one starts. */
    struct hfree *prev = 0;
    for (e = head->free.next; e != &head->free; e = e->next) {
        struct hfree *p = (struct hfree *)e;
        if (prev && (u8 *)prev + prev->len == (u8 *)p) {
            prev->len += p->len;
            list_unlink(&p->entry);
        } else {
            prev = p;
        }
    }
out:
    local_irq_enable();
}

/* ---- the pool's queue ------------------------------------------------- */

int lbuf_empty_btctrler(struct lbuff_head_btctrler *head)
{
    local_irq_disable();
    struct list_head *first = head->head.next;
    local_irq_enable();
    return first == &head->head;
}

void lbuf_clear_btctrler(struct lbuff_head_btctrler *head)
{
    local_irq_disable();
    for (struct list_head *e = head->head.next, *n; e != &head->head; e = n) {
        n = e->next;
        lbuf_free_btctrler((struct hentry *)e + 1);
    }
    local_irq_enable();
}

/* Pushing a block already queued only counts it again. */
void lbuf_push_btctrler(void *lbuf)
{
    struct hentry *a = entry_of(lbuf);
    local_irq_disable();
    a->state = 0;
    if (a->ref++ == 0) list_insert_before(&a->entry, &a->head->head);
    local_irq_enable();
}

static struct hentry *first_with_state(struct lbuff_head_btctrler *head, u8 state)
{
    for (struct list_head *e = head->head.next; e != &head->head; e = e->next)
        if (((struct hentry *)e)->state == state) return (struct hentry *)e;
    return 0;
}

void *lbuf_pop_btctrler(struct lbuff_head_btctrler *head)
{
    local_irq_disable();
    struct hentry *a = first_with_state(head, 0);
    if (a) a->state = 1;
    local_irq_enable();
    return a ? a + 1 : 0;
}

void *lbuf_peek_btctrler(struct lbuff_head_btctrler *head)
{
    local_irq_disable();
    struct hentry *a = first_with_state(head, 0);
    local_irq_enable();
    return a ? a + 1 : 0;
}

/* Whether a block of len bytes would fit. */
u32 lbuf_remain_len_btctrler(struct lbuff_head_btctrler *head, u32 len)
{
    u32 need = round4(len + HENTRY);
    int fits = 0;
    local_irq_disable();
    for (struct list_head *e = head->free.next; e != &head->free; e = e->next)
        if (((struct hfree *)e)->len >= need) { fits = 1; break; }
    local_irq_enable();
    return fits;
}

/* The SDK logged the pool's blocks here. */
void lbuf_debug_btctrler(struct lbuff_head_btctrler *head, u8 flag) { (void)head; (void)flag; }

/* ---- a caller's queue ------------------------------------------------- */

void lbuf_push_queue_btctrler(struct list_head *head, void *lbuf)
{
    local_irq_disable();
    list_insert_before(&entry_of(lbuf)->entry, head);
    local_irq_enable();
}

void *lbuf_pop_queue_btctrler(struct list_head *head)
{
    void *lbuf = 0;
    local_irq_disable();
    struct list_head *e = head->next;
    if (e != head) {
        list_unlink(e);
        list_init(e);
        lbuf = (struct hentry *)e + 1;
    }
    local_irq_enable();
    return lbuf;
}

void *lbuf_peek_queue_btctrler(struct list_head *head)
{
    local_irq_disable();
    struct list_head *e = head->next;
    local_irq_enable();
    return e == head ? 0 : (struct hentry *)e + 1;
}

void lbuf_clear_queue_btctrler(struct list_head *head)
{
    local_irq_disable();
    for (struct list_head *e = head->next, *n; e != head; e = n) {
        n = e->next;
        list_unlink(e);
        list_init(e);
        lbuf_free_btctrler((struct hentry *)e + 1);
    }
    local_irq_enable();
}

#ifdef UNITTEST
#include <stdlib.h>
#include "test.h"
int config_asser;
static int asserts, irq;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void local_irq_disable(void) { irq++; }
void local_irq_enable(void) { irq--; }
static union { u8 b[4096 + 8]; u64 align; } mem;
/* Free blocks in address order, none adjacent, none overlapping a live one. */
static int check(struct lbuff_head_btctrler *h, void **live, int n)
{
    u8 *last_end = 0; int total = 0;
    for (struct list_head *e = h->free.next; e != &h->free; e = e->next) {
        struct hfree *p = (struct hfree *)e;
        if ((u8 *)p <= last_end && last_end) return -1;          /* order / adjacency */
        for (int i = 0; i < n; i++) if (live[i]) {
            u8 *a = (u8 *)entry_of(live[i]);
            if (a < (u8 *)p + p->len && (u8 *)p < a + entry_of(live[i])->len) return -2;
        }
        last_end = (u8 *)p + p->len; total += p->len;
    }
    return total;
}
int main(void)
{
    /* Misaligned buffer: moved to the next word. */
    struct lbuff_head_btctrler *h = lbuf_init_btctrler(mem.b + 1, 4096);
    CHECK((u8 *)h == mem.b + 4 && !irq);
    const int whole = 4096 - 3 - (int)sizeof(*h) - 12;
    CHECK(check(h, 0, 0) == whole && lbuf_empty_btctrler(h));

    /* Random allocations and frees keep the free list consistent and
     * coalesce back to a single block. */
    void *live[64] = { 0 };
    srand(1);
    for (int round = 0; round < 20000; round++) {
        int i = rand() % 64;
        if (live[i]) {
            lbuf_free_btctrler(live[i]); live[i] = 0;
        } else {
            u32 len = rand() % 200;
            live[i] = lbuf_alloc_btctrler(h, len);
            if (live[i]) {
                CHECK(((uintptr_t)live[i] & 3) == 0 && entry_of(live[i])->len >= round4(len + HENTRY));
                CHECK(entry_of(live[i])->head == h && !entry_of(live[i])->state);
                memset(live[i], round, len);
            }
        }
        if (round % 97 == 0) CHECK(check(h, live, 64) >= 0);
    }
    for (int i = 0; i < 64; i++) { lbuf_free_btctrler(live[i]); live[i] = 0; }
    CHECK(check(h, 0, 0) == whole && h->free.next->next == &h->free && !irq);

    /* No fit; a block too small to split is taken whole. */
    CHECK(!lbuf_alloc_btctrler(h, 5000));
    CHECK(lbuf_remain_len_btctrler(h, (whole & ~3) - HENTRY) && !lbuf_remain_len_btctrler(h, whole));
    void *all = lbuf_alloc_btctrler(h, whole - HENTRY - 8);
    CHECK(all && entry_of(all)->len == whole && h->free.next == &h->free);
    lbuf_free_btctrler(all);

    /* Shrinking returns the end; growing does not. */
    void *a = lbuf_alloc_btctrler(h, 400);
    void *b = lbuf_alloc_btctrler(h, 16);
    CHECK(lbuf_realloc_btctrler(a, 100) == a && entry_of(a)->len == 100 + HENTRY);
    CHECK(lbuf_alloc_btctrler(h, 200) == (u8 *)a + 100 + HENTRY);   /* the freed end, first fit */
    CHECK(!lbuf_realloc_btctrler(b, 64) && asserts == 1); asserts = 0;
    lbuf_realloc_btctrler(b, 8); CHECK(entry_of(b)->len == round4(16 + HENTRY));  /* too small to split */
    lbuf_free_btctrler(a); lbuf_free_btctrler(b);
    lbuf_free_btctrler((u8 *)a + 100 + HENTRY);
    CHECK(check(h, 0, 0) == whole);

    /* A double free is detected and changes nothing. */
    a = lbuf_alloc_btctrler(h, 32); b = lbuf_alloc_btctrler(h, 32);
    lbuf_free_btctrler(a);
    int before = check(h, 0, 0);
    entry_of(a)->head = h; lbuf_free_btctrler(a);
    CHECK(check(h, 0, 0) == before && !irq);
    lbuf_free_btctrler(b); CHECK(check(h, 0, 0) == whole);

    /* The pool's queue: pop marks taken, free removes. */
    a = lbuf_alloc_btctrler(h, 8); b = lbuf_alloc_btctrler(h, 8);
    lbuf_push_btctrler(a); lbuf_push_btctrler(b); lbuf_push_btctrler(a);   /* again: counted */
    CHECK(entry_of(a)->ref == 2 && !lbuf_empty_btctrler(h));
    CHECK(lbuf_peek_btctrler(h) == a && lbuf_pop_btctrler(h) == a && lbuf_pop_btctrler(h) == b);
    CHECK(!lbuf_pop_btctrler(h) && !lbuf_peek_btctrler(h));
    lbuf_push_btctrler(a); CHECK(lbuf_pop_btctrler(h) == a);              /* state back to 0 */
    lbuf_free_btctrler(a); CHECK(lbuf_peek_btctrler(h) == 0 && !lbuf_empty_btctrler(h));
    lbuf_clear_btctrler(h); CHECK(lbuf_empty_btctrler(h) && check(h, 0, 0) == whole);

    /* A caller's queue. */
    struct list_head q = { &q, &q };
    a = lbuf_alloc_btctrler(h, 8); b = lbuf_alloc_btctrler(h, 8);
    lbuf_push_queue_btctrler(&q, a); lbuf_push_queue_btctrler(&q, b);
    CHECK(lbuf_peek_queue_btctrler(&q) == a && lbuf_pop_queue_btctrler(&q) == a);
    CHECK(entry_of(a)->entry.next == &entry_of(a)->entry);
    lbuf_free_btctrler(a);
    lbuf_clear_queue_btctrler(&q); CHECK(!lbuf_peek_queue_btctrler(&q) && check(h, 0, 0) == whole && !irq);
    return TEST_DONE("btctrler_lbuf");
}
#endif
