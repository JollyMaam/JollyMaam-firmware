/* BD19 local_media_sync.c.o, occurrence 1. Recovered from its pinned IR. */
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "btctrler/glue.h"

extern struct tws_link_connection tws_link;
extern struct tws_connection tws_conn;
extern volatile u32 jiffies;
extern u8 jiffies_unit;
extern int os_sem_create(OS_SEM *, int);
extern int os_sem_pend(OS_SEM *, int);
extern int os_taskq_post_type(const char *, int, int, int *);
extern struct lbuff_head *lbuf_init(void *, u32, int, int);
extern void lbuf_dump(struct lbuff_head *);
extern void *lbuf_alloc(struct lbuff_head *, u32);
extern int lbuf_free(void *);
extern int printf(const char *, ...);

#define MEDIA_CMD_ID 1280136259u
#define MEDIA_CODE __attribute__((section(".tws_media_sync_code")))
#define MEDIA_RX_ALLOC_SIZE(len) ((len) + offsetof(struct tws_media_frame, data) - 2)

static struct list_head frame_head = { &frame_head, &frame_head };
static _Bool remote_inited __attribute__((aligned(4)));
static u8 local_media_state;
static u8 drop_frame_disable;
static u8 *media_pool;
static _Bool media_sync_inited __attribute__((aligned(4)));
static struct xSTATIC_QUEUE cmd_sem;
static u32 rx_timeout_jiffies;

/* SDK byte layout is fixed at 44 bytes on BD19. Pointer-bearing offsets are
 * expressed through the target structure, whose generated asserts pin them. */
#ifndef UNITTEST
_Static_assert(sizeof(struct tws_media_frame) == 44, "BD19 media frame size");
_Static_assert(offsetof(struct tws_media_frame, head) == 28, "BD19 media head");
_Static_assert(offsetof(struct tws_media_frame, entry) == 32, "BD19 media entry");
_Static_assert(offsetof(struct tws_media_frame, data) == 42, "BD19 media data");
#endif

/* The callback has the SDK's i8 argument ABI, despite the common stub type. */
struct media_func_stub { u32 id; void (*func)(u8 *, u16, u8); };
static void local_media_sync_cmd_hanlder(u8 *, u16, u8);
static const struct media_func_stub tws_local_media_sync_cmd
    __attribute__((used, section(".tws_func_stub"), aligned(4))) = {
        MEDIA_CMD_ID, local_media_sync_cmd_hanlder
    };
static int tws_local_media_req_and_res(struct lmp_connection *, u8 *, int, u8 *);
static int tws_local_media_req(struct lmp_connection *, u8 *, int);
static void tws_local_media_res(struct lmp_connection *, u8 *, int);
int tws_local_media_event_handler(struct lmp_connection *, int, int *);
const struct tws_sync_channel tws_local_media_sync
    __attribute__((section(".tws_sync_channel.12"), aligned(4))) = {
        .channel = 12, .attribute = 3,
        .tx_sync_req = (void *)tws_local_media_req,
        .rx_sync_req_and_res = (void *)tws_local_media_req_and_res,
        .rx_sync_res = (void *)tws_local_media_res,
        .event_handler = tws_local_media_event_handler,
    };

static __attribute__((always_inline)) inline struct tws_media_frame *frame_from_entry(struct list_head *n)
{
    return (struct tws_media_frame *)((u8 *)n - offsetof(struct tws_media_frame, entry));
}
static void __list_del_entry(struct list_head *n)
{
    struct list_head *prev = n->prev, *next = n->next;
    next->prev = prev; prev->next = next;
}
static void list_add_tail(struct list_head *n)
{
    struct list_head *prev = frame_head.prev;
    frame_head.prev = n; n->next = &frame_head; n->prev = prev; prev->next = n;
}
static void INIT_LIST_HEAD(void) { frame_head.next = &frame_head; frame_head.prev = &frame_head; }
static void cpu_reset(void) { p33_soft_reset(); }
static MEDIA_CODE struct tws_media_frame *tws_api_first_media_frame(void)
{
    if (!media_sync_inited) return NULL;
    struct list_head *n = frame_head.next;
    while (n != &frame_head) {
        struct tws_media_frame *f = frame_from_entry(n);
        if ((f->send_state == 2 || tws_link.link == NULL) && f->deleted) {
            __list_del_entry(n);
            lbuf_free(&f->tx.enc_prob);
            n = frame_head.next;
            continue;
        }
        if (tws_link.link == NULL || f->send_state == 2) return f;
        return NULL;
    }
    return NULL;
}

__attribute__((weak, section(".tws_media_sync_code")))
void tws_api_local_media_sync_rx_handler_notify(void) { }

int MEDIA_CODE tws_local_media_send(void)
{
    struct list_head *n = frame_head.next;
    int sent = 0;
    while (n != &frame_head) {
        struct list_head *next = n->next;
        struct tws_media_frame *f = frame_from_entry(n);
        u8 state = f->send_state;
        if (state != 2) {
            if (!(tws_link.link && remote_inited && tws_conn.connect_suss)) {
                f->send_state = (f->send_state & (u8)~3) | 2;
            } else if (state != 1 && local_media_state != 3) {
                f->send_state = (f->send_state & (u8)~3) | 1;
                f->head.channel = 12;
                f->head.opcode = 0;
                f->tx.event_post = 60;
                f->tx.len = f->head.len + 4;
                link_conn_tx_bulk_avaiable(tws_link.link, &f->tx);
                sent = 1;
            }
        }
        n = next;
    }
    return sent;
}

static int MEDIA_CODE tws_local_media_req_and_res(struct lmp_connection *c, u8 *d, int len, u8 *r)
{ (void)c; (void)d; (void)len; (void)r; remote_inited = 1; tws_local_media_send(); return media_sync_inited; }
static int MEDIA_CODE tws_local_media_req(struct lmp_connection *c, u8 *d, int len)
{ (void)c; (void)d; (void)len; return media_sync_inited; }
static void MEDIA_CODE tws_local_media_res(struct lmp_connection *c, u8 *d, int len)
{ (void)c; (void)d; (void)len; remote_inited = 1; tws_local_media_send(); }

int MEDIA_CODE tws_local_media_event_handler(struct lmp_connection *c, int event, int *arg)
{
    (void)c;
    switch (event) {
    case 25: {
        u8 *input = (u8 *)arg;
        if (local_media_state == 4 || !media_sync_inited || !media_pool) return 0;
        struct tws_media_frame *f = lbuf_alloc((struct lbuff_head *)media_pool, MEDIA_RX_ALLOC_SIZE(*(u16 *)input));
        while (!f) {
            if (drop_frame_disable) {
                u32 deadline = rx_timeout_jiffies;
                u32 now = jiffies;
                if (!deadline) rx_timeout_jiffies = (30 / jiffies_unit) + now;
                else if ((s32)now < (s32)deadline) return -12;
            }
            f = tws_api_first_media_frame();
            if (!f) return 0;
            __list_del_entry(&f->entry);
            lbuf_free(&f->tx.enc_prob);
            f = lbuf_alloc((struct lbuff_head *)media_pool, MEDIA_RX_ALLOC_SIZE(*(u16 *)input));
        }
        rx_timeout_jiffies = 0;
        __builtin_memcpy(__builtin_assume_aligned((u8 *)f + offsetof(struct tws_media_frame, head), 2), __builtin_assume_aligned(input, 2), *(u16 *)input + 4);
        f->send_state = (f->send_state & (u8)~3) | 2;
        list_add_tail(&f->entry);
        os_taskq_post_type("tws", 62, 0, 0);
        return 0;
    }
    case 46:
        remote_inited = 1;
        tws_local_media_send();
        os_taskq_post_type("tws", 62, 0, 0);
        if (local_media_state == 2) { drop_frame_disable = 0; tws_conn_send_event(12, 0); }
        local_media_state = 0;
        return 0;
    case 60: {
        struct tws_media_frame *f = (void *)arg;
        f->send_state = (f->send_state & (u8)~3) | 2;
        if (local_media_state == 3) f->deleted = 1;
        else os_taskq_post_type("tws", 62, 0, 0);
        return 0;
    }
    case 62: tws_api_local_media_sync_rx_handler_notify(); return 0;
    default: return 0;
    }
}

void MEDIA_CODE tws_api_local_media_trans_set_buf(void *buf, int size)
{ if (media_pool != buf) { media_pool = buf; if (buf) lbuf_init(buf, size, 4, 0); } }
int MEDIA_CODE tws_api_local_media_trans_get_total_buffer_size(void)
{ return media_pool ? *(int *)(media_pool + offsetof(struct lbuff_head, total_size)) : 0; }
void *MEDIA_CODE tws_api_local_media_trans_alloc(int len)
{
    if (!media_pool) return NULL;
    u8 *base = lbuf_alloc((struct lbuff_head *)media_pool,
                          len + offsetof(struct tws_media_frame, data) + 2);
    if (!base) return NULL;
    __builtin_memset(__builtin_assume_aligned(base, 4), 0, offsetof(struct tws_media_frame, head));
    struct tws_media_frame *f = (void *)base;
    f->tx.hold = 1; f->tx.llid = 2;
    return base + offsetof(struct tws_media_frame, data);
}
void MEDIA_CODE tws_api_local_media_trans_free(void *data)
{ lbuf_free((u8 *)data - offsetof(struct tws_media_frame, data)); }
void MEDIA_CODE tws_api_local_media_trans_packet_del(void *data)
{ u8 *p = data; local_irq_disable(); __list_del_entry((struct list_head *)(p - offsetof(struct tws_media_frame, data) + offsetof(struct tws_media_frame, entry))); lbuf_free(p - offsetof(struct tws_media_frame, data)); local_irq_enable(); }
void MEDIA_CODE tws_api_local_media_trans_push(void *data, int len)
{
    u8 *p = data; struct tws_media_frame *f = (void *)(p - offsetof(struct tws_media_frame, data));
    *((u8 *)__builtin_assume_aligned((u8 *)f + offsetof(struct tws_media_frame, data) - 2, 4)) &= (u8)~7;
    f->head.len = (u16)((u32)len + 12);
    local_irq_disable(); list_add_tail(&f->entry);
    if (!tws_link.plink) tws_local_media_send();
    local_irq_enable();
}
void *MEDIA_CODE tws_api_local_media_trans_pop(int *len)
{
    local_irq_disable(); drop_frame_disable = 1;
    struct tws_media_frame *f = tws_api_first_media_frame();
    if (f) { if (len) *len = f->head.len - 12; __list_del_entry(&f->entry); }
    else if (len) *len = 0;
    local_irq_enable(); return f ? f->data : NULL;
}
void *MEDIA_CODE tws_api_local_media_trans_fetch(void *prev, int *len)
{
    local_irq_disable(); struct tws_media_frame *f;
    if (!prev) f = tws_api_first_media_frame();
    else {
        struct tws_media_frame *current = (void *)((u8 *)prev - offsetof(struct tws_media_frame, data));
        struct list_head *next = current->entry.next;
        f = next == &frame_head ? NULL : frame_from_entry(next);
    }
    local_irq_enable(); if (!f) return NULL;
    if (len) *len = f->head.len - 12; return f->data;
}
void MEDIA_CODE tws_api_local_media_trans_start(void)
{ if (!media_sync_inited) { media_sync_inited = 1; INIT_LIST_HEAD(); } }
void MEDIA_CODE tws_api_local_media_trans_stop(void)
{
    local_irq_disable(); struct list_head *n = frame_head.next;
    while (n != &frame_head) { struct list_head *next = n->next; __list_del_entry(n); lbuf_free((u8 *)n - offsetof(struct tws_media_frame, entry)); n = next; }
    media_sync_inited = 0; remote_inited = 0; media_pool = NULL; local_irq_enable();
}
void MEDIA_CODE tws_api_local_media_trans_clear(void)
{
    local_irq_disable(); struct list_head *n = frame_head.next;
    while (n != &frame_head) { struct list_head *next = n->next; struct tws_media_frame *f = frame_from_entry(n);
        if (tws_link.link && f->send_state == 1) f->deleted = 1;
        else { __list_del_entry(n); lbuf_free((u8 *)f + offsetof(struct tws_media_frame, tx.enc_prob)); }
        n = next; }
    local_irq_enable();
}
void MEDIA_CODE tws_api_local_media_trans_clear_no_ready(void)
{
    local_irq_disable(); struct list_head *n = frame_head.next;
    while (n != &frame_head) { struct list_head *next = n->next; struct tws_media_frame *f = frame_from_entry(n);
        if ((f->send_state & 3) == 0) { __list_del_entry(n); lbuf_free((u8 *)f + offsetof(struct tws_media_frame, tx.enc_prob)); }
        n = next; }
    local_irq_enable();
}
int MEDIA_CODE tws_api_local_media_trans_check_total(u8 mode)
{
    u32 total = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n); total += mode ? f->head.len : f->head.len - 12; }
    local_irq_enable(); return (s32)total;
}
int MEDIA_CODE tws_api_local_media_trans_check_ready_total(void)
{
    u32 total = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n);
        if (!f->deleted && (f->send_state == 2 || !tws_link.link)) total += f->head.len - 12; }
    local_irq_enable(); return (s32)total;
}
void MEDIA_CODE tws_api_local_media_trans_check(void)
{
    u16 go = 0, ready = 0, waiting = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n);
        switch (f->send_state & 3) { case 1: ready++; break; case 2: go++; break; default: waiting++; break; } }
    local_irq_enable(); printf("go:%d, rd:%d, nw:%d \n", go, ready, waiting);
}
int MEDIA_CODE tws_api_local_media_trans_bulk_push(u8 *data, int len)
{ u8 *p = tws_api_local_media_trans_alloc(len); if (!p) return 0; __builtin_memcpy(p, data, len); tws_api_local_media_trans_push(p, len); return len; }
int MEDIA_CODE tws_api_local_media_push_with_sequence(u8 *data, int len, u16 seq)
{ u8 *p = tws_api_local_media_trans_alloc((u32)len + 2); if (!p) return 0; __builtin_memcpy(p, &seq, 2); __builtin_memcpy(p + 2, data, len); tws_api_local_media_trans_push(p, (u32)len + 2); return len; }
int MEDIA_CODE tws_api_local_media_push_with_timestamp(u8 *data, int len, u32 timestamp)
{ u8 *p = tws_api_local_media_trans_alloc((u32)len + 4); if (!p) return 0; __builtin_memcpy(p, &timestamp, 4); __builtin_memcpy(p + 4, data, len); tws_api_local_media_trans_push(p, (u32)len + 4); return len; }
static int MEDIA_CODE local_media_trans_open(u16 size, u8 notify)
{
    if (!media_pool) { media_pool = malloc(size); if (!media_pool) return -12; lbuf_init(media_pool, size, 4, 0); }
    if (!media_sync_inited) { media_sync_inited = 1; INIT_LIST_HEAD(); }
    if (!notify) return 0;
    os_sem_create(&cmd_sem, 0); u8 msg[4]; msg[0] = 0; msg[1] = size; msg[2] = size >> 8;
    int ret = tws_api_send_data_to_sibling(msg, 3, MEDIA_CMD_ID); if (!ret) os_sem_pend(&cmd_sem, 500);
    printf("err: %d\n", ret); return 0;
}
int MEDIA_CODE tws_api_local_media_trans_open(u16 size) { return local_media_trans_open(size, 1); }
int MEDIA_CODE tws_api_local_media_dec_stop(void)
{ u8 cmd[4]; cmd[0] = 2; if (!tws_api_send_data_to_sibling(cmd, 1, MEDIA_CMD_ID)) { os_sem_pend(&cmd_sem, 500); local_media_state = 3; } return 0; }
int MEDIA_CODE tws_api_local_media_dec_start(u8 *data, u8 len)
{
    if (config_asser && len >= 8) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/local_media_sync.c", 676); printf("ASSERT-FAILD: len < 8 len = %d\n", len); cpu_assert_debug(); }
    else if (len >= 8) cpu_reset();
    os_sem_create(&cmd_sem, 0); u8 msg[12]; msg[0] = 1; __builtin_memcpy(msg + 1, data, len);
    int ret = tws_api_send_data_to_sibling(msg, (u16)(len + 1), MEDIA_CMD_ID);
    if (!ret) { os_sem_pend(&cmd_sem, 500); local_media_state = 1; } return ret;
}
void MEDIA_CODE tws_api_auto_drop_frame_enable(int enable) { drop_frame_disable = (enable == 0); }

static void MEDIA_CODE local_media_sync_cmd_hanlder(u8 *data, u16 len, u8 rx)
{
    switch (data[0]) {
    case 0:
        if (rx) local_media_trans_open((u16)(((data[2] << 8) | data[1]) + 2048), 0);
        else { os_sem_post(&cmd_sem); remote_inited = 1; }
        break;
    case 1:
        lbuf_dump((struct lbuff_head *)media_pool);
        if (rx) { if (local_media_state != 2) { local_media_state = 2; tws_conn_send_event(11, "11LB", tws_conn.role, tws_conn.lmp_conn != NULL, len - 1, data + 1); } }
        else { local_media_state = 1; os_sem_post(&cmd_sem); }
        break;
    case 2:
        if (rx) { drop_frame_disable = 0; local_media_state = 4; tws_api_local_media_trans_clear(); tws_conn_send_event(12, 0); }
        else { os_sem_post(&cmd_sem); local_media_state = 3; }
        break;
    }
}
#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

int config_asser;
volatile u32 jiffies;
u8 jiffies_unit;
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static struct link_connection fixture_link;
static int irq_depth, max_irq, alloc_fail, sem_creates, sem_pends, posts;
static int sibling_ret, sibling_calls, sibling_len, event_calls, event_no, event_a[4];
static int tx_calls, lbuf_inits, lbuf_frees, taskq_calls, reset_calls;
static u32 pool_size, alloc_size;
static struct bredr_tx_bulk *last_tx;
static u8 sibling_bytes[16];
static struct { void *p; u32 size; } blocks[128];
static struct tws_media_frame *unlock_frame;
static u16 *retry_input;
static int mutate_next, post_state, event_state, tx_irq;


void local_irq_disable(void) { ++irq_depth; if (max_irq < irq_depth) max_irq = irq_depth; }
void local_irq_enable(void) { CHECK(irq_depth > 0); --irq_depth; if (unlock_frame) unlock_frame->head.len = 52; }
struct lbuff_head *lbuf_init(void *pool, u32 size, int align, int priv)
{ CHECK(align == 4 && priv == 0); ++lbuf_inits; pool_size = size; if (pool) *(u32 *)((u8 *)pool + offsetof(struct lbuff_head, total_size)) = size; return pool; }
void *lbuf_alloc(struct lbuff_head *pool, u32 size)
{
    (void)pool; alloc_size = size; if (alloc_fail) { --alloc_fail; return NULL; }
    for (unsigned i = 0; i < 128; ++i) if (!blocks[i].p) {
        blocks[i].p = malloc(size + 64); blocks[i].size = size + 64; CHECK(blocks[i].p); memset(blocks[i].p,0xa5,size+64); return blocks[i].p;
    }
    CHECK(0); return NULL;
}
int lbuf_free(void *p)
{
    if (!p) return 0;
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].p == p) { free(p); blocks[i].p = NULL; ++lbuf_frees; if (retry_input) *retry_input = 48; return 0; }
    CHECK(0); return -1;
}
int os_sem_create(OS_SEM *s, int n) { (void)s; CHECK(n == 0); ++sem_creates; return 0; }
int os_sem_pend(OS_SEM *s, int t) { (void)s; CHECK(t == 500); ++sem_pends; return 0; }
int os_sem_post(OS_SEM *s) { (void)s; ++posts; return 0; }
int tws_api_send_data_to_sibling(void *p, u16 n, u32 id)
{ CHECK(id == MEDIA_CMD_ID && n <= sizeof(sibling_bytes)); ++sibling_calls; sibling_len=n; memcpy(sibling_bytes,p,n); return sibling_ret; }
void tws_conn_send_event(int e, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt); ++event_calls; event_no=e;
    if (fmt && !strcmp(fmt,"11LB")) { for (int i=0;i<3;i++) event_a[i]=va_arg(ap,int); (void)va_arg(ap,void *); }
    va_end(ap); if (event_state) local_media_state = event_state;
}

void link_conn_tx_bulk_avaiable(struct link_connection *l, struct bredr_tx_bulk *tx)
{ CHECK(l == &fixture_link); ++tx_calls; last_tx=tx; tx_irq = irq_depth;
    if (mutate_next) ((struct tws_media_frame *)tx)->entry.next = &frame_head; }
int os_taskq_post_type(const char *name, int type, int n, int *args)
{ (void)args; CHECK(!strcmp(name,"tws") && (type==62 || type==0)); CHECK(n==0); ++taskq_calls; if (post_state) local_media_state = post_state; return 0; }
void lbuf_dump(struct lbuff_head *p) { (void)p; }
void p33_soft_reset(void) { ++reset_calls; }
void cpu_assert_debug(void) { ++reset_calls; }

static void fixture_cleanup(void)
{
    u8 *owned_pool = media_pool;
    if (media_sync_inited) tws_api_local_media_trans_stop();
    if (owned_pool) free(owned_pool); media_pool=NULL;
    for (unsigned i=0;i<128;i++) if (blocks[i].p) { free(blocks[i].p); blocks[i].p=NULL; }
    CHECK(irq_depth==0);
}
static void fixture_reset(void)
{
    fixture_cleanup(); unlock_frame=NULL; retry_input=NULL; mutate_next=post_state=event_state=tx_irq=0; memset(&tws_link,0,sizeof(tws_link)); memset(&tws_conn,0,sizeof(tws_conn));
    memset(&fixture_link,0,sizeof(fixture_link)); memset(&frame_head,0,sizeof(frame_head));
    frame_head.next=frame_head.prev=&frame_head; media_sync_inited=remote_inited=0;
    local_media_state=drop_frame_disable=0; rx_timeout_jiffies=0; media_pool=NULL;
    irq_depth=max_irq=alloc_fail=sem_creates=sem_pends=posts=0; sibling_ret=sibling_calls=sibling_len=0;
    event_calls=event_no=tx_calls=lbuf_inits=lbuf_frees=taskq_calls=reset_calls=0;
    pool_size=alloc_size=0; last_tx=NULL; jiffies=100; jiffies_unit=10; config_asser=0;
}
static struct tws_media_frame *frame_for(void *p)
{ return (void *)((u8 *)p - offsetof(struct tws_media_frame,data)); }
static struct tws_media_frame *rx_frame(u16 payload_len, u8 fill)
{
    _Alignas(struct tws_media_frame) u8 input[256]={0}; *(u16 *)input=payload_len;
    struct tws_channel_head *h=(void *)input; h->channel=8; h->opcode=3; h->len=payload_len+12;
    memset(input+4,fill,payload_len);
    CHECK(tws_local_media_event_handler(NULL,25,(int *)input)==0);
    return frame_from_entry(frame_head.prev);
}
int main(void)
{
    fixture_reset();
    CHECK(tws_local_media_sync.channel==12 && tws_local_media_sync.attribute==3);
    CHECK(tws_api_local_media_trans_get_total_buffer_size()==0);
    tws_api_local_media_trans_start(); CHECK(media_sync_inited);
    CHECK(tws_api_local_media_trans_open(1024)==0 && sibling_calls==1 && sibling_len==3);
    CHECK(pool_size==1024 && media_sync_inited && sem_creates==1 && sem_pends==1);
    u8 remote_open[3]={0,7,0}; local_media_sync_cmd_hanlder(remote_open,3,0);
    CHECK(remote_inited && posts==1);

    /* Alloc clears only the metadata prefix; push masks flags and records len. */
    void *p=tws_api_local_media_trans_alloc(20); CHECK(p);
    struct tws_media_frame *f=frame_for(p);
    CHECK(alloc_size==20+offsetof(struct tws_media_frame,data)+2);
    CHECK(f->tx.hold==1 && f->tx.llid==2 && ((u8 *)&f->head)[0]==0xa5 && ((u8 *)p)[0]==0xa5);
    memset(p,0x5a,20); *((u8 *)p-2)=0xff;
    tws_api_local_media_trans_push(p,20);
    CHECK(f->head.len==32 && (*((u8 *)p-2)&7)==2 && f->entry.next==&frame_head);
    CHECK(tws_api_local_media_trans_check_total(0)==20);
    tws_api_local_media_trans_packet_del(p); CHECK(lbuf_frees==1);

    /* Data allocation send and callback event transitions; link state gating. */
    p=tws_api_local_media_trans_alloc(12); f=frame_for(p); memset(p,0x31,12);
    tws_link.link=&fixture_link; tws_conn.connect_suss=1; remote_inited=1;
    tws_api_local_media_trans_push(p,12);
    CHECK(tx_calls==1 && last_tx==&f->tx && f->send_state==1 && f->head.channel==12);
    CHECK(f->head.opcode==0 && f->tx.event_post==60 && f->tx.len==28);
    CHECK(tws_api_local_media_trans_check_ready_total()==0);
    CHECK(tws_local_media_send()==0);
    CHECK(tws_local_media_event_handler(NULL,60,(int *)f)==0 && f->send_state==2 && taskq_calls==1);
    CHECK(tws_api_local_media_trans_check_ready_total()==12);
    int len=-1; CHECK(tws_api_local_media_trans_pop(&len)==p && len==12);
    CHECK(tws_api_local_media_trans_fetch(NULL,&len)==NULL);
    tws_api_local_media_trans_free(p);

    /* RX copy length, head readiness, allocation retry deadline and timeout. */
    f=rx_frame(16,0x6c); CHECK(f->send_state==2 && f->head.len==28);
    CHECK(tws_api_local_media_trans_check_ready_total()==16);
    CHECK(tws_api_local_media_trans_fetch(NULL,&len)==f->data && len==16);
    CHECK(tws_api_local_media_trans_pop(&len)==f->data);
    alloc_fail=1; _Alignas(int) u8 in[32]={0}; *(u16 *)in=8;
    drop_frame_disable=1; rx_timeout_jiffies=0;
    CHECK(tws_local_media_event_handler(NULL,25,(int *)in)==0 && rx_timeout_jiffies==103);
    alloc_fail=1; CHECK(tws_local_media_event_handler(NULL,25,(int *)in)==-12);
    CHECK(rx_timeout_jiffies==103);
    jiffies=102; alloc_fail=1; CHECK(tws_local_media_event_handler(NULL,25,(int *)in)==-12);
    jiffies=103; alloc_fail=1; CHECK(tws_local_media_event_handler(NULL,25,(int *)in)==0);

    /* Clear retains an in-flight frame as deleted; stop drains pool frames. */
    p=tws_api_local_media_trans_alloc(4); f=frame_for(p); tws_api_local_media_trans_push(p,4);
    f->send_state=1; tws_api_local_media_trans_clear(); CHECK(f->deleted==1);
    u8 *owned = media_pool; tws_api_local_media_trans_stop(); CHECK(!media_sync_inited && !remote_inited && !media_pool); free(owned);
    fixture_cleanup();

    /* Decoder start/stop, event callback and zero length command argument. */
    fixture_reset();
    u8 start[2]={0xaa,0xbb}; sibling_ret=0;
    CHECK(tws_api_local_media_dec_start(start,2)==0 && sibling_len==3 && sibling_bytes[0]==1 && local_media_state==1);
    CHECK(tws_api_local_media_dec_stop()==0 && local_media_state==3);
    u8 rxstart[2]={1,2}; local_media_sync_cmd_hanlder(rxstart,1,1);
    CHECK(event_no==11 && event_calls==1);
    local_media_state=0; rxstart[0]=1; local_media_sync_cmd_hanlder(rxstart,0,1);
    CHECK(event_no==11 && event_calls==2 && event_a[2]==-1);
    fixture_cleanup();
    /* Head readiness blocks later frames, while totals and next-fetch include them. */
    fixture_reset(); CHECK(tws_api_local_media_trans_open(1024)==0);
    tws_link.link=&fixture_link; tws_link.plink=&fixture_link;
    p=tws_api_local_media_trans_alloc(16); f=frame_for(p);
    void *p2=tws_api_local_media_trans_alloc(24); struct tws_media_frame *f2=frame_for(p2);
    tws_api_local_media_trans_push(p,16); tws_api_local_media_trans_push(p2,24);
    f->send_state=1; f2->send_state=2;
    CHECK(tws_api_first_media_frame()==NULL && tws_api_local_media_trans_check_ready_total()==24);
    len=99; CHECK(tws_api_local_media_trans_pop(&len)==NULL && len==0);
    f2->deleted=1; CHECK(tws_api_local_media_trans_fetch(p,&len)==p2 && len==24);
    len=99; CHECK(tws_api_local_media_trans_fetch(p2,&len)==NULL && len==99);
    f->send_state=2; unlock_frame=f;
    CHECK(tws_api_local_media_trans_fetch(NULL,&len)==p && len==40);
    f->head.len=28;
    CHECK(tws_api_local_media_trans_pop(&len)==p && len==16);
    unlock_frame=NULL; tws_api_local_media_trans_free(p);
    CHECK(tws_api_first_media_frame()==NULL && frame_head.next==&frame_head);

    /* Queue hook mutation must not discard the cached next frame. */
    p=tws_api_local_media_trans_alloc(8); p2=tws_api_local_media_trans_alloc(8);
    f=frame_for(p); f2=frame_for(p2);
    tws_api_local_media_trans_push(p,8); tws_api_local_media_trans_push(p2,8);
    remote_inited=1; tws_conn.connect_suss=1; mutate_next=1;
    CHECK(tws_local_media_send()==1 && tx_calls==2 && tx_irq==0);
    mutate_next=0; f->entry.next=&f2->entry;
    CHECK(f->send_state==1 && f2->send_state==1);
    tws_api_local_media_trans_clear(); CHECK(f->deleted && f2->deleted);
    local_media_state=3; int old_posts=taskq_calls;
    tws_local_media_event_handler(NULL,60,(int *)f);
    CHECK(f->send_state==2 && f->deleted && taskq_calls==old_posts);
    CHECK(tws_api_first_media_frame()==NULL && frame_head.next==&f2->entry);

    /* A free hook can change the caller's input length before RX retry. */
    fixture_reset(); CHECK(tws_api_local_media_trans_open(1024)==0);
    p=tws_api_local_media_trans_alloc(8); tws_api_local_media_trans_push(p,8);
    _Alignas(struct tws_media_frame) u8 input[128]={0}; *(u16 *)input=32;
    retry_input=(u16 *)input; alloc_fail=1;
    CHECK(tws_local_media_event_handler(NULL,25,(int *)input)==0);
    CHECK(alloc_size==MEDIA_RX_ALLOC_SIZE(48) && frame_from_entry(frame_head.next)->head.len==48);
    retry_input=NULL;
    post_state=2; event_state=7; local_media_state=1; drop_frame_disable=1;
    tws_local_media_event_handler(NULL,46,NULL);
    CHECK(event_no==12 && local_media_state==0 && drop_frame_disable==0);
    post_state=event_state=0;

    /* Failure returns, byte stores, wrap and returning reset/assert hooks. */
    fixture_reset(); sibling_ret=-7;
    CHECK(tws_api_local_media_trans_open(1024)==0 && sem_pends==0);
    CHECK(tws_api_local_media_dec_stop()==0 && local_media_state==0);
    u8 data[8]={1,2,3,4,5,6,7,8};
    CHECK(tws_api_local_media_dec_start(data,8)==-7 && reset_calls==1);
    config_asser=1;
    CHECK(tws_api_local_media_dec_start(data,8)==-7 && reset_calls==2);
    CHECK(tws_api_local_media_push_with_timestamp(data,2,0x12345678)==2);
    p=frame_from_entry(frame_head.prev)->data;
    CHECK(!memcmp(p,"\x78\x56\x34\x12\x01\x02",6));
    CHECK(tws_api_local_media_push_with_sequence(data,2,0x5678)==2);
    p=frame_from_entry(frame_head.prev)->data; CHECK(!memcmp(p,"\x78\x56\x01\x02",4));
    CHECK(tws_api_local_media_trans_bulk_push(data,2)==2);
    p=tws_api_local_media_trans_alloc(0); tws_api_local_media_trans_push(p,0x7fffffff);
    CHECK(frame_for(p)->head.len==11);
    fixture_cleanup();
    return TEST_DONE("btctrler_bd19_local_media_sync");
}
#endif
