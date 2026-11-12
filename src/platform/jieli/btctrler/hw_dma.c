/* DMA buffer pools and queues. Preserve the SDK's assertion fall-through,
 * interrupt masking, and TX-before-RX destroy order. */
#include <stdio.h>
#include "btctrler/glue.h"

static struct dma_handle hdl;

static void cpu_reset(void)
{
    p33_soft_reset();
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

void hw_dma_init(void *rx_dma, u32 rx_size, void *tx_dma, u32 tx_size)
{
    hdl.rx = lbuf_init_btctrler(rx_dma, rx_size);
    hdl.rx_size = rx_size;
    hdl.tx = lbuf_init_btctrler(tx_dma, tx_size);
    hdl.tx_size = tx_size;
}

void *hw_dma_alloc(u8 type, u32 size)
{
    struct lbuff_head_btctrler *pool;
    const char *message;
    int line;

    if (type == DMA_TYPE_RX) {
        pool = hdl.rx;
        message = "ASSERT-FAILD: __this->rx [error] rx dma not valid\r";
        line = 47;
    } else {
        pool = hdl.tx;
        message = "ASSERT-FAILD: __this->tx [error] tx dma not valid\r";
        line = 51;
    }

    if (!pool) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/hw_dma.c", line);
            puts(message);
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    return lbuf_alloc_btctrler(pool, size);
}

void hw_dma_free(void *dma)
{
    lbuf_free_btctrler(dma);
}

void hw_dma_push(struct _dma_head *head, struct _dma *dma)
{
    local_irq_disable();
    struct list_head *entry = &dma->entry;
    struct list_head *head_entry = &head->head;
    struct list_head *prev = head_entry->prev;
    head_entry->prev = entry;
    entry->next = head_entry;
    entry->prev = prev;
    prev->next = entry;
    local_irq_enable();
}

void *hw_dma_pop(struct _dma_head *head)
{
    struct list_head *head_entry = &head->head;
    struct list_head *entry = head_entry->next;
    if (entry == head_entry) return NULL;
    local_irq_disable();
    list_del(entry);
    local_irq_enable();
    return entry;
}

void *btctrlermod_dma_first(struct _dma_head *head)
{
    struct list_head *entry = head->head.next;
    return entry == &head->head ? NULL : entry;
}

int hw_dma_empty(struct _dma_head *head)
{
    return head->head.next == &head->head;
}

void hw_dma_clear(struct _dma_head *head)
{
    struct _dma *dma = (struct _dma *)head->head.next;
    while (&dma->entry != &head->head) {
        struct _dma *next = *(struct _dma **)dma;
        list_del(&dma->entry);
        lbuf_free_btctrler(dma);
        dma = next;
    }
}

void hw_dma_destory(void)
{
    if (hdl.tx && hdl.rx) {
        lbuf_clear_btctrler(hdl.tx);
        lbuf_clear_btctrler(hdl.rx);
    }
}

#ifdef UNITTEST
#include "test.h"

static struct lbuff_head_btctrler pools[2];
static struct lbuff_head_btctrler *last_alloc_pool;
static u32 last_alloc_size;
static void *last_freed;
static struct lbuff_head_btctrler *cleared[2];
static unsigned int clear_count, irq_disable_count, irq_enable_count;

struct lbuff_head_btctrler *lbuf_init_btctrler(void *buf, u32 len)
{
    CHECK(buf != NULL);
    CHECK(len != 0);
    static unsigned int next_pool;
    return &pools[next_pool++ % 2];
}

void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *pool, u32 size)
{
    last_alloc_pool = pool;
    last_alloc_size = size;
    return (void *)((uintptr_t)pool + size);
}

void lbuf_free_btctrler(void *p) { last_freed = p; }
void lbuf_clear_btctrler(struct lbuff_head_btctrler *pool)
{
    cleared[clear_count++] = pool;
}
void local_irq_disable(void) { ++irq_disable_count; }
void local_irq_enable(void) { ++irq_enable_count; }
int config_asser;

int main(void)
{
    u8 rx[32], tx[32];
    struct _dma_head head = { 0 };
    struct _dma first = { 0 }, second = { 0 };

    hw_dma_init(rx, sizeof(rx), tx, sizeof(tx));
    CHECK(hw_dma_alloc(DMA_TYPE_RX, 13) == (void *)((uintptr_t)&pools[0] + 13));
    CHECK(last_alloc_pool == &pools[0] && last_alloc_size == 13);
    CHECK(hw_dma_alloc(DMA_TYPE_TX, 7) == (void *)((uintptr_t)&pools[1] + 7));
    CHECK(last_alloc_pool == &pools[1] && last_alloc_size == 7);

    head.head.next = head.head.prev = &head.head;
    CHECK(hw_dma_empty(&head));
    hw_dma_push(&head, &first);
    hw_dma_push(&head, &second);
    CHECK(!hw_dma_empty(&head));
    CHECK(btctrlermod_dma_first(&head) == &first && btctrlermod_dma_first(&head) == &first);
    CHECK(hw_dma_pop(&head) == &first);
    CHECK(first.entry.next == &first.entry && first.entry.prev == &first.entry);
    CHECK(hw_dma_pop(&head) == &second);
    CHECK(hw_dma_pop(&head) == NULL && btctrlermod_dma_first(&head) == NULL);
    CHECK(hw_dma_empty(&head));
    CHECK(irq_disable_count == 4 && irq_enable_count == 4);

    hw_dma_push(&head, &first);
    hw_dma_push(&head, &second);
    hw_dma_clear(&head);
    CHECK(hw_dma_empty(&head));
    CHECK(last_freed == &second);

    clear_count = 0;
    hw_dma_destory();
    CHECK(clear_count == 2 && cleared[0] == &pools[1] && cleared[1] == &pools[0]);
    return TEST_DONE("btctrler_hw_dma");
}
#endif
