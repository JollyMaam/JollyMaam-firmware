/* BD19 SDK member tws_data_trans.c.o, occurrence 1. */
#include <string.h>
#include <stdlib.h>
#include "btctrler/glue.h"

extern int os_sem_set(OS_SEM *, u16);
extern int os_sem_pend(OS_SEM *, int);
extern int os_sem_create(OS_SEM *, int);
extern struct lbuff_head *lbuf_init(void *, u32, int, int);
extern void *lbuf_alloc(struct lbuff_head *, u32);
extern int lbuf_free(void *);
extern volatile u32 jiffies;
extern u8 jiffies_unit;
extern int printf(const char *, ...);
extern int puts(const char *);

#ifdef UNITTEST
static void *trans_test_malloc(size_t);
static void trans_test_free(void *);
#define malloc(n) trans_test_malloc(n)
#define free(p) trans_test_free(p)
#endif

#define TRANS_FUNC_ID 1413764179u

static u8 g_channel;
static struct list_head g_trans_head = { &g_trans_head, &g_trans_head };
static void data_trans_cmd_hanlder(u8 *, u16, u8);
/* This SDK occurrence takes an eight-bit rx flag, unlike the generated
 * common stub typedef (bool). Keep its actual callback ABI locally. */
struct data_trans_func_stub { u32 func_id; void (*func)(u8 *, u16, u8); };
static const struct data_trans_func_stub tws_data_trans_cmd
    __attribute__((used, section(".tws_func_stub"), aligned(4))) = {
        TRANS_FUNC_ID, data_trans_cmd_hanlder
    };

const struct tws_sync_channel tws_data_trans
    __attribute__((section(".tws_sync_channel.14"), aligned(4))) = {
        .channel = 14, .attribute = 2,
        .tx_sync_req = 0, .tx_sync_req_suss = 0,
        .rx_sync_req_and_res = 0, .tx_sync_res_suss = 0,
        .rx_sync_res = 0, .event_handler = tws_data_trans_event_handler,
    };

static __attribute__((always_inline)) inline struct tws_data_trans *find_trans(u8 channel)
{
    struct list_head *n;
    for (n = g_trans_head.next; n != &g_trans_head; n = n->next) {
        struct tws_data_trans *t = (void *)((u8 *)n - offsetof(struct tws_data_trans, entry));
        if (t->channel == channel) return t;
    }
    return 0;
}

static void data_frame_free(struct tws_data_trans *t, struct tws_data_frame *f)
{
    struct list_head *prev = f->entry.prev, *next = f->entry.next;
    next->prev = prev;
    prev->next = next;
    if (t->role == 1) lbuf_free(f);
    else lbuf_free((u8 *)f - offsetof(struct tws_tx_frame, data));
}

static int data_trans_frame_clear(struct tws_data_trans *t)
{
    struct list_head *h = &t->frame_head, *n = h->next;
    while (n != h) {
        struct list_head *next = n->next;
        struct tws_data_frame *f = (void *)((u8 *)n - offsetof(struct tws_data_frame, entry));
        if (f->send_state == 2 || f->pop_flag != 0) f->deleted = 1;
        else data_frame_free(t, f);
        n = next;
    }
    return h->next == h;
}

static void __release_trans(struct tws_data_trans *t)
{
    struct list_head *prev = t->entry.prev, *next = t->entry.next;
    next->prev = prev;
    prev->next = next;
    free(&t->channel);
    puts("tws_data_trans_release\n");
}

static struct tws_data_trans *data_trans_channel_open(u16 size)
{
    u32 trans_size = offsetof(struct tws_data_trans, data_pool);
    struct tws_data_trans *t = malloc((u32)size + trans_size);
    if (!t) return 0;
    __builtin_memset(t, 0, trans_size);
    lbuf_init(t->data_pool, size, 4, 0);
    os_sem_create(&t->cmd_sem, 0);
    t->frame_head.next = &t->frame_head;
    t->frame_head.prev = &t->frame_head;
    local_irq_disable();
    struct list_head *prev = g_trans_head.prev;
    g_trans_head.prev = &t->entry;
    t->entry.next = &g_trans_head;
    t->entry.prev = prev;
    prev->next = &t->entry;
    local_irq_enable();
    return t;
}

static void data_trans_channel_close(struct tws_data_trans *t)
{
    local_irq_disable();
    t->state = 0;
    if (data_trans_frame_clear(t)) __release_trans(t);
    local_irq_enable();
}

u8 tws_api_data_trans_open(u8 channel, enum tws_data_trans_attr attr, u16 size)
{
    u8 cmd[8] = {0};
    struct tws_data_trans *t;
    if (channel) t = find_trans(channel);
    else {
        t = data_trans_channel_open(size);
        if (!t) return 0;
        t->attr = (u8)attr;
        t->role = 0;
        u8 next = g_channel;
        do { channel = next; ++next; } while (!channel);
        g_channel = next;
        t->channel = channel;
    }
    if (!t) return 0;
    cmd[0] = 1; cmd[1] = t->channel; cmd[2] = size; cmd[3] = size >> 8; cmd[4] = (u8)attr;
    os_sem_set(&t->cmd_sem, 0);
    if (tws_api_send_data_to_sibling(cmd, 5, TRANS_FUNC_ID) == 0) {
        printf("data_trans_open: %d\n", t->channel);
        os_sem_pend(&t->cmd_sem, 500);
    }
    return t->channel;
}

int tws_api_data_trans_start(u8 channel, u8 *arg, u8 len)
{
    struct tws_data_trans *t = find_trans(channel);
    u8 cmd[12] = {2, channel};
    if (!t) return -22;
    if (len >= 8) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_data_trans.c", 507);
            printf("ASSERT-FAILD: len < 8 len = %d\n", len);
            cpu_assert_debug();
        } else p33_soft_reset();
    }
    __builtin_memcpy(cmd + 2, arg, len);
    os_sem_set(&t->cmd_sem, 0);
    int r = tws_api_send_data_to_sibling(cmd, (u16)(len + 2), TRANS_FUNC_ID);
    if (!r) { os_sem_pend(&t->cmd_sem, 500); t->state = 2; }
    return r ? r : 0;
}

int tws_api_data_trans_stop(u8 channel)
{
    struct tws_data_trans *t = find_trans(channel); u8 cmd[4] = {3, channel};
    if (!t) return -22;
    os_sem_set(&t->cmd_sem, 0);
    int r = tws_api_send_data_to_sibling(cmd, 2, TRANS_FUNC_ID);
    if (!r) { os_sem_pend(&t->cmd_sem, 500); t->state = 3; }
    return 0;
}

int tws_api_data_trans_close(u8 channel)
{
    struct tws_data_trans *t;
    u8 cmd[4] = {4, channel};
    local_irq_disable(); t = find_trans(channel);
    if (!t) { local_irq_enable(); return -22; }
    if (t->role == 1) { local_irq_enable(); return 0; }
    t->state = 0; local_irq_enable();
    os_sem_set(&t->cmd_sem, 0);
    int r = tws_api_send_data_to_sibling(cmd, 2, TRANS_FUNC_ID);
    if (r) data_trans_channel_close(t);
    else if (os_sem_pend(&t->cmd_sem, 500) == 0) data_trans_channel_close(t);
    else return -14;
    return 0;
}

void tws_api_data_trans_auto_drop(u8 channel, int enable)
{ local_irq_disable(); struct tws_data_trans *t=find_trans(channel); if(t)t->drop_frame_disable=(enable==0); local_irq_enable(); }

void *tws_api_data_trans_buf_alloc(u8 channel, int len)
{
    local_irq_disable(); struct tws_data_trans *t=find_trans(channel);
    if (!t) { local_irq_enable(); return 0; }
    u8 *b=lbuf_alloc(t->data_pool, len+offsetof(struct tws_tx_frame, data.data));
    if (!b) { local_irq_enable(); return 0; }
    __builtin_memset(__builtin_assume_aligned(b,4),0,offsetof(struct tws_tx_frame, data.data));
    struct tws_tx_frame *tx=(void *)b;
    tx->tx.hold=1; tx->tx.llid=2;
    tx->data.channel=channel; tx->data.send_state=0;
    struct list_head *head=&t->frame_head, *e=&tx->data.entry;
    struct list_head *prev=head->prev; head->prev=e; e->next=head; e->prev=prev; prev->next=e;
    local_irq_enable(); return tx->data.data;
}

int tws_api_data_trans_push(u8 channel, void *payload, int len)
{
    local_irq_disable();
    struct tws_data_trans *t = find_trans(channel);
    if (!t) { local_irq_enable(); return -22; }
    local_irq_disable();
    struct tws_data_frame *f = (void *)((u8 *)payload - offsetof(struct tws_data_frame, data));
    f->send_state = 1;
    f->head.len = (u16)(len + 12);
    struct list_head *entry = t->frame_head.next;
    while (entry != &t->frame_head) {
        struct list_head *next = entry->next;
        struct tws_data_frame *q = (void *)((u8 *)entry - offsetof(struct tws_data_frame, entry));
        u8 state = q->send_state;
        if (state != 3) {
            struct link_connection *link = tws_link.link;
            if (!link || !(t->remote_inited && tws_conn.connect_suss)) {
                q->send_state = 3;
            } else if (state != 2) {
                if (state == 0 || t->state == 3) break;
                q->send_state = 2;
                q->head.opcode = 0;
                q->head.channel = 14;
                struct bredr_tx_bulk *tx = (void *)((u8 *)q - offsetof(struct tws_tx_frame, data));
                tx->event_post = 68;
                tx->len = (u16)(q->head.len + 4);
                link_conn_tx_bulk_avaiable(link, tx);
            }
        }
        entry = next;
    }
    local_irq_enable();
    local_irq_enable();
    return 0;
}

int tws_api_data_trans_send(u8 ch,u8 *buf,int len)
{ void *p=tws_api_data_trans_buf_alloc(ch,len); if(!p)return 0; __builtin_memcpy(p,buf,len); tws_api_data_trans_push(ch,p,len); return len; }

void tws_api_data_trans_free(u8 channel, void *frame)
{
    local_irq_disable(); struct tws_data_trans *t=find_trans(channel);
    if(t){ data_frame_free(t,(void *)((u8*)frame - offsetof(struct tws_data_frame, data))); if(t->state==0 && data_trans_frame_clear(t)) __release_trans(t); }
    local_irq_enable();
}

static struct tws_data_frame *tws_api_first_media_frame(struct tws_data_trans *t,u8 pop)
{
    struct list_head *h=&t->frame_head,*n=h->next;
    while(n!=h){ struct list_head *next=n->next; struct tws_data_frame *f=(void*)((u8*)n - offsetof(struct tws_data_frame,entry));
        if(f->deleted) data_frame_free(t,f);
        else if(f->send_state==3){if(!f->pop_flag){f->pop_flag=pop;return f;}}
        else return 0;
        n=next;
    } return 0;
}

void *tws_api_data_trans_pop(u8 ch,int *len)
{ local_irq_disable(); struct tws_data_trans*t=find_trans(ch); struct tws_data_frame*f=0; if(t){t->drop_frame_disable=1;f=tws_api_first_media_frame(t,1);if(len)*len=f?(int)f->head.len-12:0;} local_irq_enable(); return f?f->data:0; }

void *tws_api_data_trans_fetch(u8 channel, void *prev, int *len)
{
    local_irq_disable();
    struct tws_data_trans *t = find_trans(channel);
    if (!t) { local_irq_enable(); return NULL; }
    struct tws_data_frame *f;
    if (!prev) f = tws_api_first_media_frame(t, 0);
    else {
        struct tws_data_frame *previous = (void *)((u8 *)prev - offsetof(struct tws_data_frame, data));
        struct list_head *next = previous->entry.next;
        if (next == &t->frame_head) { local_irq_enable(); return NULL; }
        f = (void *)((u8 *)next - offsetof(struct tws_data_frame, entry));
        if (f->send_state != 3) { local_irq_enable(); return NULL; }
    }
    local_irq_enable();
    if (!f) return NULL;
    if (len) *len = (int)f->head.len - 12;
    return f->data;
}

void tws_api_data_trans_clear(u8 ch)
{ local_irq_disable(); struct tws_data_trans*t=find_trans(ch); if(t)data_trans_frame_clear(t); local_irq_enable(); }

int tws_api_data_trans_check(u8 ch,u16 *ready,u16 *total)
{ local_irq_disable(); *ready=0; *total=0; struct tws_data_trans*t=find_trans(ch); if(!t){local_irq_enable();return -22;} for(struct list_head*n=t->frame_head.next;n!=&t->frame_head;n=n->next){struct tws_data_frame*f=(void*)((u8*)n - offsetof(struct tws_data_frame,entry));if(f->send_state && !f->deleted){*total=(u16)(f->head.len-12);if(f->send_state==3)*ready=(u16)(*ready+f->head.len-12);}} local_irq_enable();return 0; }

int tws_data_trans_event_handler(struct lmp_connection *c, int event, int *arg)
{
    (void)c;
    if (event == 25) {
        struct tws_data_frame *incoming = (void *)arg;
        struct tws_data_trans *t = find_trans(incoming->channel);
        if (!t || t->state == 3) return 0;
        struct tws_data_frame *f = lbuf_alloc(t->data_pool, incoming->head.len + 12);
        while (!f) {
            if (t->drop_frame_disable) {
                u32 deadline=t->rx_timeout_jiffies;
                u32 now = jiffies;
                if (!deadline) t->rx_timeout_jiffies=(30 / jiffies_unit)+now;
                else if ((s32)now < (s32)deadline) return -12;
            }
            struct list_head *n=t->frame_head.next;
            if (n==&t->frame_head) return 0;
            struct tws_data_frame *old=(void*)((u8*)n - offsetof(struct tws_data_frame,entry));
            if (!old->deleted && old->pop_flag==1) return 0;
            data_frame_free(t,old);
            f=lbuf_alloc(t->data_pool,incoming->head.len+12);
        }
        t->rx_timeout_jiffies = 0;
        __builtin_memcpy(__builtin_assume_aligned(f,4), __builtin_assume_aligned(incoming,4), incoming->head.len + 4);
        f->deleted = 0; f->pop_flag = 0; f->send_state = 3;
        struct list_head *e = &f->entry, *head = &t->frame_head;
        struct list_head *prev = head->prev; head->prev = e; e->next = head; e->prev = prev; prev->next = e;
        return 0;
    }
    if (event == 46) {
        local_irq_disable();
        struct list_head *n = g_trans_head.next;
        while (n != &g_trans_head) {
            struct list_head *next = n->next;
            struct tws_data_trans *t = (void *)((u8 *)n - offsetof(struct tws_data_trans, entry));
            for (struct list_head *e=t->frame_head.next; e!=&t->frame_head; e=e->next) {
                struct tws_data_frame *f=(void*)((u8*)e - offsetof(struct tws_data_frame,entry));
                if (f->send_state) f->send_state=3;
            }
            if (t->role == 1) {
                if (t->state == 2) { t->state=3; t->rx_timeout_jiffies=0; tws_conn_send_event(18,"11",t->channel,t->attr); }
                t->state=0;
            }
            if (data_trans_frame_clear(t) && t->state==0) __release_trans(t);
            n=next;
        }
        local_irq_enable();
        return 0;
    }
    if (event == 68) {
        struct tws_tx_frame *tx=(void *)arg;
        u8 channel=tx->data.channel;
        struct tws_data_trans *t=find_trans(channel);
        tx->data.send_state=3;
        if (t && t->state==0 && data_trans_frame_clear(t)) __release_trans(t);
    }
    return 0;
}
static void data_trans_cmd_hanlder(u8 *data, u16 len, u8 role)
{
    u8 channel;
    struct tws_data_trans *t;
    switch (data[0]) {
    case 1:
        channel = data[1];
        if (role) {
            t = data_trans_channel_open((u16)(((u16)data[2] | ((u16)data[3] << 8)) + 2048));
            if (t) { t->attr = data[4]; t->role = 1; t->channel = channel; }
            tws_conn_send_event(16, "111", channel, data[4], t ? 0 : -12);
        } else {
            t = find_trans(channel);
            if (t) { t->remote_inited = 1; os_sem_post(&t->cmd_sem); }
        }
        break;
    case 2:
        channel = data[1];
        t = find_trans(channel);
        if (!t) break;
        if (role) {
            if (t->state == 2) break;
            t->state = 2;
            /* The SDK truncates len-1 to u16, then subtracts one in i32. */
            u16 n = (u16)(len - 1);
            tws_conn_send_event(17, "11LB", channel, t->attr, (int)n - 1, data + 2);
        } else {
            t->state = 2;
            os_sem_post(&t->cmd_sem);
        }
        break;
    case 3:
        channel = data[1];
        t = find_trans(channel);
        if (!t) break;
        if (role) {
            t->drop_frame_disable = 0;
            t->state = 3;
            data_trans_frame_clear(t);
            tws_conn_send_event(18, "11", channel, t->attr);
        } else { t->state = 3; os_sem_post(&t->cmd_sem); }
        break;
    case 4:
        channel = data[1];
        t = find_trans(channel);
        if (!t) break;
        if (role) {
            u8 attr = t->attr;
            data_trans_channel_close(t);
            tws_conn_send_event(19, "11", channel, attr);
        } else os_sem_post(&t->cmd_sem);
        break;
    default:
        break;
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free
int config_asser;
volatile u32 jiffies;
u8 jiffies_unit;
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static struct link_connection test_link;
static int irq_depth, disables, enables, max_depth, malloc_fail;
static int channel_allocations, channel_frees, lbuf_calls, lbuf_frees, alloc_failures;
static int sem_create_calls, sem_set_calls, sem_post_calls, sem_pend_calls, pend_ret;
static int sibling_ret, sibling_calls, event_calls, event_number, event_args[3], tx_calls;
static int event_mutates_state, assertion_calls, reset_calls;
static void *event_data;
static u8 sibling_data[16];
static int sibling_len;
static u32 last_alloc_size, init_size;
static struct bredr_tx_bulk *last_tx;
static struct tws_data_trans *callback_trans;
static int mutate_tx_next;
static struct tws_data_frame *unlock_frame;
static void (*allocation_hook)(void);
static struct { void *ptr; u32 size; } allocations[64];

static void *trans_test_malloc(size_t size)
{
    if (malloc_fail) return NULL;
    ++channel_allocations;
    return malloc(size);
}
static void trans_test_free(void *ptr)
{ ++channel_frees; free(ptr); }
void local_irq_disable(void)
{ ++disables; ++irq_depth; if (max_depth < irq_depth) max_depth = irq_depth; }
void local_irq_enable(void)
{
    ++enables; --irq_depth; CHECK(irq_depth >= 0);
    if (unlock_frame) unlock_frame->head.len = 52;
}
struct lbuff_head *lbuf_init(void *pool, u32 size, int align, int priv)
{ CHECK(align == 4 && priv == 0); init_size = size; return pool; }
void *lbuf_alloc(struct lbuff_head *pool, u32 size)
{
    (void)pool; ++lbuf_calls; last_alloc_size = size;
    if (allocation_hook) allocation_hook();
    if (alloc_failures) { if (alloc_failures > 0) --alloc_failures; return NULL; }
    for (unsigned n = 0; n < 64; ++n) if (!allocations[n].ptr) {
        allocations[n].ptr = malloc(size); allocations[n].size = size;
        CHECK(allocations[n].ptr != NULL);
        memset(allocations[n].ptr, 0xa5, size);
        return allocations[n].ptr;
    }
    CHECK(0); return NULL;
}
int lbuf_free(void *ptr)
{
    for (unsigned n = 0; n < 64; ++n) if (allocations[n].ptr == ptr) {
        free(ptr); allocations[n].ptr = NULL; ++lbuf_frees; return 0;
    }
    CHECK(0); return -1;
}
int os_sem_create(OS_SEM *sem, int count)
{ (void)sem; CHECK(count == 0); ++sem_create_calls; return -7; }
int os_sem_set(OS_SEM *sem, u16 count)
{ (void)sem; CHECK(count == 0); ++sem_set_calls; return -8; }
int os_sem_post(OS_SEM *sem) { (void)sem; ++sem_post_calls; return 0; }
int os_sem_pend(OS_SEM *sem, int timeout)
{ (void)sem; CHECK(timeout == 500); ++sem_pend_calls; return pend_ret; }
int tws_api_send_data_to_sibling(void *data, u16 len, u32 id)
{
    CHECK(id == TRANS_FUNC_ID && len <= sizeof(sibling_data));
    ++sibling_calls; sibling_len = len; memcpy(sibling_data, data, len);
    return sibling_ret;
}
void tws_conn_send_event(int event, const char *format, ...)
{
    va_list args; va_start(args, format);
    ++event_calls; event_number = event;
    event_args[0] = va_arg(args, int); event_args[1] = va_arg(args, int);
    if (!strcmp(format, "111")) event_args[2] = va_arg(args, int);
    else if (!strcmp(format, "11LB")) {
        event_args[2] = va_arg(args, int); event_data = va_arg(args, void *);
    } else CHECK(!strcmp(format, "11"));
    va_end(args);
    if (event_mutates_state && callback_trans) callback_trans->state = 99;
}
void link_conn_tx_bulk_avaiable(struct link_connection *link, struct bredr_tx_bulk *bulk)
{
    CHECK(link == &test_link); last_tx = bulk; ++tx_calls; CHECK(irq_depth == 2);
    if (mutate_tx_next) {
        struct tws_tx_frame *tx = (void *)bulk;
        tx->data.entry.next = &callback_trans->frame_head;
    }
}
void cpu_assert_debug(void) { ++assertion_calls; }
void p33_soft_reset(void) { ++reset_calls; }

static void cleanup(void)
{
    while (g_trans_head.next != &g_trans_head) {
        struct tws_data_trans *t = (void *)((u8 *)g_trans_head.next - offsetof(struct tws_data_trans, entry));
        for (struct list_head *e = t->frame_head.next; e != &t->frame_head; e = e->next) {
            struct tws_data_frame *f = (void *)((u8 *)e - offsetof(struct tws_data_frame, entry));
            f->send_state = 3; f->pop_flag = 0;
        }
        data_trans_channel_close(t);
    }
    for (unsigned n = 0; n < 64; ++n) CHECK(allocations[n].ptr == NULL);
    CHECK(irq_depth == 0);
}
static void reset_fixture(void)
{
    cleanup();
    memset(&tws_link, 0, sizeof(tws_link)); memset(&tws_conn, 0, sizeof(tws_conn));
    g_channel = 0; jiffies = 100; jiffies_unit = 10;
    irq_depth = disables = enables = max_depth = malloc_fail = 0;
    channel_allocations = channel_frees = lbuf_calls = lbuf_frees = alloc_failures = 0;
    sem_create_calls = sem_set_calls = sem_post_calls = sem_pend_calls = pend_ret = 0;
    sibling_ret = sibling_calls = event_calls = event_number = tx_calls = 0;
    event_mutates_state = assertion_calls = reset_calls = config_asser = 0;
    callback_trans = NULL; allocation_hook = NULL; last_tx = NULL; mutate_tx_next = 0; unlock_frame = NULL;
}
static struct tws_data_trans *make_channel(u8 channel, u8 role)
{
    struct tws_data_trans *t = data_trans_channel_open(4096);
    CHECK(t != NULL); t->channel = channel; t->role = role; t->state = 2;
    return t;
}
static struct tws_data_frame *from_payload(void *payload)
{ return (void *)((u8 *)payload - offsetof(struct tws_data_frame, data)); }
static struct tws_data_frame *receive_frame(struct tws_data_trans *t, int payload)
{
    /* Host pointers widen the intrusive links. Pad storage and copied bytes;
     * target wire offsets/counts are independently checked in compiled IR. */
    _Alignas(struct tws_data_frame) u8 storage[128] = { 0 };
    struct tws_data_frame *incoming = (void *)storage;
    incoming->channel = t->channel; incoming->head.len = payload + 12;
    memset(incoming->data, 0x35, 32);
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)incoming) == 0);
    return (void *)((u8 *)t->frame_head.prev - offsetof(struct tws_data_frame, entry));
}

int main(void)
{
    reset_fixture();
    CHECK(tws_data_trans.channel == 14 && tws_data_trans.attribute == 2);
    CHECK(tws_data_trans_cmd.func_id == TRANS_FUNC_ID && tws_data_trans_cmd.func == data_trans_cmd_hanlder);
    int length = 71;
    CHECK(tws_api_data_trans_pop(99, &length) == NULL && length == 71);
    CHECK(tws_api_data_trans_fetch(99, NULL, &length) == NULL && length == 71);
    CHECK(tws_api_data_trans_start(99, NULL, 0) == -22);
    CHECK(tws_api_data_trans_stop(99) == -22 && tws_api_data_trans_close(99) == -22);
    CHECK(tws_api_data_trans_push(99, NULL, 1) == -22 && irq_depth == 0);
    u16 ready = 12, total = 13;
    CHECK(tws_api_data_trans_check(99, &ready, &total) == -22 && ready == 0 && total == 0);
    malloc_fail = 1; CHECK(tws_api_data_trans_open(0, 0, 256) == 0); malloc_fail = 0;
    pend_ret = -1;
    u8 channel = tws_api_data_trans_open(0, 0x13, 256);
    CHECK(channel == 1 && g_channel == 2 && init_size == 256 && sem_create_calls == 1);
    struct tws_data_trans *t = find_trans(channel);
    CHECK(t->state == 0 && t->attr == 0x13 && !t->remote_inited);
    CHECK(sibling_len == 5 && sibling_data[0] == 1 && sibling_data[1] == 1 && sibling_data[2] == 0 && sibling_data[3] == 1);
    u8 command[12] = { 1, 1, 0, 0, 0 };
    data_trans_cmd_hanlder(command, 0, 0);
    CHECK(t->remote_inited == 1 && sem_post_calls == 1);
    CHECK(tws_api_data_trans_open(1, 0x22, 512) == 1 && t->attr == 0x13 && sibling_data[4] == 0x22);
    u8 argument[8] = { 1, 2, 3 };
    sibling_ret = -9;
    CHECK(tws_api_data_trans_start(1, argument, 3) == -9 && t->state == 0);
    CHECK(tws_api_data_trans_stop(1) == 0 && t->state == 0);
    sibling_ret = 0;
    CHECK(tws_api_data_trans_start(1, argument, 3) == 0 && t->state == 2 && pend_ret == -1);
    CHECK(tws_api_data_trans_stop(1) == 0 && t->state == 3);
    CHECK(tws_api_data_trans_close(1) == -14 && t->state == 0 && find_trans(1) == t);
    sibling_ret = -2; CHECK(tws_api_data_trans_close(1) == 0 && !find_trans(1));

    /* Command rx flag is a full byte; short START length truncates before subtract. */
    reset_fixture(); command[0] = 1; command[1] = 9; command[2] = 0xff; command[3] = 0xff; command[4] = 7;
    data_trans_cmd_hanlder(command, 0, 2);
    t = find_trans(9); CHECK(t && t->role == 1 && init_size == 2047 && event_number == 16 && event_args[2] == 0);
    command[0] = 2;
    data_trans_cmd_hanlder(command, 0, 1);
    CHECK(event_number == 17 && event_args[2] == 65534 && event_data == command + 2 && t->state == 2);
    int before = event_calls; data_trans_cmd_hanlder(command, 5, 1); CHECK(event_calls == before);
    t->state = 0; data_trans_cmd_hanlder(command, 1, 1); CHECK(event_args[2] == -1);
    command[0] = 3; t->drop_frame_disable = 1; data_trans_cmd_hanlder(command, 2, 1);
    CHECK(t->state == 3 && !t->drop_frame_disable && event_number == 18);
    CHECK(tws_api_data_trans_close(9) == 0 && find_trans(9) == t);
    command[0] = 4; data_trans_cmd_hanlder(command, 2, 1);
    CHECK(!find_trans(9) && event_number == 19 && event_args[1] == 7);
    u8 unknown = 99; data_trans_cmd_hanlder(&unknown, 0, 0);

    reset_fixture(); sibling_ret = -1; g_channel = 255;
    CHECK(tws_api_data_trans_open(0, 0, 256) == 255 && g_channel == 0);
    CHECK(tws_api_data_trans_open(0, 0, 256) == 1 && g_channel == 2);
    g_channel = 1;
    CHECK(tws_api_data_trans_open(0, 0, 256) == 1); /* no collision check */

    /* TX metadata, nested IRQ order, wait/ACK/deferred close lifecycle. */
    reset_fixture(); t = make_channel(1, 0); t->remote_inited = 1;
    tws_link.link = &test_link; tws_conn.connect_suss = 1;
    void *p = tws_api_data_trans_buf_alloc(1, 32);
    struct tws_data_frame *f = from_payload(p);
    struct tws_tx_frame *tx = (void *)((u8 *)f - offsetof(struct tws_tx_frame, data));
    CHECK(last_alloc_size == 32 + offsetof(struct tws_tx_frame, data.data));
    CHECK(tx->tx.hold == 1 && tx->tx.rx_head_offset == 0 && tx->tx.llid == 2 && f->channel == 1);
    f->deleted = 1;
    CHECK(tws_api_data_trans_push(1, p, 32) == 0 && irq_depth == 0 && max_depth == 2);
    CHECK(tx_calls == 1 && last_tx == &tx->tx && f->send_state == 2 && f->deleted == 1);
    CHECK(f->head.channel == 14 && f->head.opcode == 0 && tx->tx.event_post == 68 && tx->tx.len == 48);
    data_trans_channel_close(t); CHECK(find_trans(1) == t && f->deleted == 1);
    CHECK(tws_data_trans_event_handler(NULL, 68, (int *)tx) == 0 && !find_trans(1) && lbuf_frees == 1);
    struct tws_tx_frame unknown_tx = { 0 }; unknown_tx.data.channel = 99;
    tws_data_trans_event_handler(NULL, 68, (int *)&unknown_tx); CHECK(unknown_tx.data.send_state == 3);

    /* An earlier INIT frame blocks later WAIT frames until pushed. */
    reset_fixture(); t = make_channel(1, 0); t->remote_inited = 1;
    tws_link.link = &test_link; tws_conn.connect_suss = 1;
    p = tws_api_data_trans_buf_alloc(1, 32); void *p2 = tws_api_data_trans_buf_alloc(1, 24);
    CHECK(tws_api_data_trans_push(1, p2, 24) == 0 && tx_calls == 0);
    CHECK(tws_api_data_trans_push(1, p, 32) == 0 && tx_calls == 2);
    from_payload(p)->send_state = from_payload(p2)->send_state = 3;
    tws_api_data_trans_free(1, p); tws_api_data_trans_free(1, p2); CHECK(find_trans(1) == t);
    t->state = 3; p = tws_api_data_trans_buf_alloc(1, 8);
    CHECK(tws_api_data_trans_push(1, p, 8) == 0 && from_payload(p)->send_state == 1);
    tws_link.link = NULL;
    CHECK(tws_api_data_trans_push(1, p, 8) == 0 && from_payload(p)->send_state == 3);
    CHECK(tws_api_data_trans_send(1, argument, 3) == 3);

    /* TX captures next before the queue hook can rewrite the current node. */
    reset_fixture(); t = make_channel(1, 0); t->remote_inited = 1;
    tws_link.link = &test_link; tws_conn.connect_suss = 1;
    p = tws_api_data_trans_buf_alloc(1, 32); p2 = tws_api_data_trans_buf_alloc(1, 24);
    from_payload(p)->send_state = 1;
    callback_trans = t; mutate_tx_next = 1;
    CHECK(tws_api_data_trans_push(1, p2, 24) == 0 && tx_calls == 2);
    mutate_tx_next = 0;
    /* Restore the intentionally mutated link for fixture cleanup. */
    from_payload(p)->entry.next = &from_payload(p2)->entry;

    /* RX allocation retry, signed deadline, protected popped head, and framing. */
    reset_fixture(); t = make_channel(7, 1);
    f = receive_frame(t, 32); CHECK(f->send_state == 3 && f->channel == 7 && !f->pop_flag && !f->deleted);
    CHECK(!memcmp(f->data, "5555555555555555", 16));
    _Alignas(struct tws_data_frame) u8 rx_storage[128] = { 0 };
    struct tws_data_frame *rx = (void *)rx_storage; rx->channel = 7; rx->head.len = 44;
    t->drop_frame_disable = 1; alloc_failures = 1;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == 0 && lbuf_frees == 1 && t->rx_timeout_jiffies == 0);
    alloc_failures = -1; t->rx_timeout_jiffies = 200;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == -12);
    t->rx_timeout_jiffies = 0x80000000u;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == 0 && t->frame_head.next == &t->frame_head);
    t->rx_timeout_jiffies = 0;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == 0 && t->rx_timeout_jiffies == 103);
    alloc_failures = 0; f = receive_frame(t, 32);
    length = 0; p = tws_api_data_trans_pop(7, &length); CHECK(p == f->data && length == 32 && f->pop_flag == 1);
    alloc_failures = -1; t->rx_timeout_jiffies = 0;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == 0 && t->frame_head.next == &f->entry);
    t->rx_timeout_jiffies = 200;
    CHECK(tws_data_trans_event_handler(NULL, 25, (int *)rx) == -12);
    alloc_failures = 0;

    /* Fetch advances next without filtering its deleted/pop flags; failure preserves len. */
    reset_fixture(); t = make_channel(7, 1);
    f = receive_frame(t, 32); struct tws_data_frame *f2 = receive_frame(t, 40);
    f2->deleted = f2->pop_flag = 1;
    length = 99; CHECK(tws_api_data_trans_fetch(7, f->data, &length) == f2->data && length == 40);
    length = 99; CHECK(tws_api_data_trans_fetch(7, f2->data, &length) == NULL && length == 99);
    f2->deleted = f2->pop_flag = 0;
    CHECK(tws_api_data_trans_check(7, &ready, &total) == 0 && ready == 72 && total == 40);
    u16 aliased = 0; tws_api_data_trans_check(7, &aliased, &aliased); CHECK(aliased == 80);
    p = tws_api_data_trans_pop(7, &length); CHECK(p == f->data);
    tws_api_data_trans_clear(7); CHECK(f->deleted == 1 && f->pop_flag == 1);
    tws_api_data_trans_free(7, p); CHECK(t->frame_head.next == &t->frame_head);

    /* Fetch reads len after IRQ enable; pop writes it before enabling. */
    reset_fixture(); t = make_channel(7, 1); f = receive_frame(t, 32);
    unlock_frame = f;
    CHECK(tws_api_data_trans_fetch(7, NULL, &length) == f->data && length == 40);
    unlock_frame = NULL; f->head.len = 44; unlock_frame = f;
    CHECK(tws_api_data_trans_pop(7, &length) == f->data && length == 32);
    unlock_frame = NULL;

    /* Remote detach emits STOP then writes CLOSED after the event callback. */
    reset_fixture(); t = make_channel(7, 1); callback_trans = t; event_mutates_state = 1;
    f = receive_frame(t, 32); f->pop_flag = 1;
    CHECK(tws_data_trans_event_handler(NULL, 46, NULL) == 0 && t->state == 0 && event_number == 18);
    CHECK(f->deleted == 1 && f->send_state == 3 && t->drop_frame_disable == 0);
    event_mutates_state = 0;
    tws_api_data_trans_free(7, f->data); CHECK(!find_trans(7));

    /* Returning assertion/reset hooks do not abort supported storage-length8. */
    reset_fixture(); t = make_channel(1, 0); sibling_ret = -3;
    CHECK(tws_api_data_trans_start(1, argument, 8) == -3 && reset_calls == 1 && assertion_calls == 0);
    config_asser = 1;
    CHECK(tws_api_data_trans_start(1, argument, 8) == -3 && reset_calls == 1 && assertion_calls == 1);
    cleanup();
    return TEST_DONE("btctrler_bd19_tws_data_trans");
}
#endif
