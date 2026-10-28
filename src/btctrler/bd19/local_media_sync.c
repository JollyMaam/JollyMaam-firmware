/* BD19 local_media_sync.c.o, occurrence 1. Recovered from its pinned IR. */
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "btctrler/glue.h"

extern struct tws_link_connection tws_link;
extern struct tws_connection tws_conn;
extern u32 jiffies;
extern u8 jiffies_unit;
extern int os_sem_create(OS_SEM *, int);
extern int os_sem_pend(OS_SEM *, int);
extern int os_taskq_post_type(const char *, int, int, int *);
extern struct lbuff_head *lbuf_init(void *, u32, int, int);
extern void lbuf_dump(void *);
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
    n->next = &frame_head; n->prev = prev; prev->next = n; frame_head.prev = n;
}
static void INIT_LIST_HEAD(void) { frame_head.next = frame_head.prev = &frame_head; }
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
        u16 len = *(u16 *)input;
        struct tws_media_frame *f = lbuf_alloc((struct lbuff_head *)media_pool, MEDIA_RX_ALLOC_SIZE(len));
        while (!f) {
            if (drop_frame_disable) {
                u32 now = jiffies;
                if (!rx_timeout_jiffies) rx_timeout_jiffies = (30 / jiffies_unit) + now;
                else if ((s32)now < (s32)rx_timeout_jiffies) return -12;
            }
            f = tws_api_first_media_frame();
            if (!f) return 0;
            __list_del_entry(&f->entry);
            lbuf_free(&f->tx.enc_prob);
            f = lbuf_alloc((struct lbuff_head *)media_pool, MEDIA_RX_ALLOC_SIZE(len));
        }
        rx_timeout_jiffies = 0;
        __builtin_memcpy((u8 *)f + offsetof(struct tws_media_frame, head), (u16 *)input, len + 4);
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
    __builtin_memset(base, 0, offsetof(struct tws_media_frame, head));
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
    *((u8 *)f + offsetof(struct tws_media_frame, data) - 2) &= (u8)~7;
    f->head.len = (u16)(len + 12);
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
    media_sync_inited = remote_inited = 0; media_pool = NULL; local_irq_enable();
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
    int total = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n); total += mode ? f->head.len : f->head.len - 12; }
    local_irq_enable(); return total;
}
int MEDIA_CODE tws_api_local_media_trans_check_ready_total(void)
{
    int total = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n);
        if (!f->deleted && (f->send_state == 2 || !tws_link.link)) total += f->head.len - 12; }
    local_irq_enable(); return total;
}
void MEDIA_CODE tws_api_local_media_trans_check(void)
{
    int go = 0, ready = 0, waiting = 0; local_irq_disable();
    for (struct list_head *n = frame_head.next; n != &frame_head; n = n->next) { struct tws_media_frame *f = frame_from_entry(n);
        switch (f->send_state & 3) { case 1: ready++; break; case 2: go++; break; default: waiting++; break; } }
    local_irq_enable(); printf("go:%d, rd:%d, nw:%d \n", go, ready, waiting);
}
int MEDIA_CODE tws_api_local_media_trans_bulk_push(u8 *data, int len)
{ u8 *p = tws_api_local_media_trans_alloc(len); if (!p) return 0; __builtin_memcpy(p, data, len); tws_api_local_media_trans_push(p, len); return len; }
int MEDIA_CODE tws_api_local_media_push_with_sequence(u8 *data, int len, u16 seq)
{ u8 *p = tws_api_local_media_trans_alloc(len + 2); if (!p) return 0; *(u16 *)(void *)p = seq; __builtin_memcpy(p + 2, data, len); tws_api_local_media_trans_push(p, len + 2); return len; }
int MEDIA_CODE tws_api_local_media_push_with_timestamp(u8 *data, int len, u32 timestamp)
{ u8 *p = tws_api_local_media_trans_alloc(len + 4); if (!p) return 0; __builtin_memcpy(p, &timestamp, 4); __builtin_memcpy(p + 4, data, len); tws_api_local_media_trans_push(p, len + 4); return len; }
static int MEDIA_CODE local_media_trans_open(u16 size, u8 notify)
{
    if (!media_pool) { media_pool = malloc(size); if (!media_pool) return -12; lbuf_init(media_pool, size, 4, 0); }
    if (!media_sync_inited) { media_sync_inited = 1; INIT_LIST_HEAD(); }
    if (!notify) return 0;
    os_sem_create(&cmd_sem, 0); u8 msg[4] = { 0, size, size >> 8, 0 };
    int ret = tws_api_send_data_to_sibling(msg, 3, MEDIA_CMD_ID); if (!ret) os_sem_pend(&cmd_sem, 500);
    printf("err: %d\n", ret); return 0;
}
int MEDIA_CODE tws_api_local_media_trans_open(u16 size) { return local_media_trans_open(size, 1); }
int MEDIA_CODE tws_api_local_media_dec_stop(void)
{ u8 cmd[4] = { 2 }; if (!tws_api_send_data_to_sibling(cmd, 1, MEDIA_CMD_ID)) { os_sem_pend(&cmd_sem, 500); local_media_state = 3; } return 0; }
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
        lbuf_dump(media_pool);
        if (rx) { if (local_media_state != 2) { local_media_state = 2; tws_conn_send_event(11, "11LB", tws_conn.role, tws_conn.lmp_conn != NULL, len - 1, data + 1); } }
        else { local_media_state = 1; os_sem_post(&cmd_sem); }
        break;
    case 2:
        if (rx) { drop_frame_disable = 0; local_media_state = 4; tws_api_local_media_trans_clear(); tws_conn_send_event(12, 0); }
        else { os_sem_post(&cmd_sem); local_media_state = 3; }
        break;
    }
}
