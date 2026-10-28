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

#define TRANS_FUNC_ID 1413764179u
#define TRANS_CODE __attribute__((section(".tws_data_trans_code")))

static u8 g_channel;
static struct list_head g_trans_head = { &g_trans_head, &g_trans_head };
static void data_trans_cmd_hanlder(u8 *, u16, u8);
static const struct tws_func_stub tws_data_trans_cmd
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

static struct tws_data_trans *find_trans(u8 channel)
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
    f->entry.next->prev = f->entry.prev;
    f->entry.prev->next = f->entry.next;
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
    t->entry.next->prev = t->entry.prev;
    t->entry.prev->next = t->entry.next;
    free(&t->channel);
    puts("tws_data_trans_release\n");
}

static struct tws_data_trans *data_trans_channel_open(u16 size)
{
    u32 trans_size = offsetof(struct tws_data_trans, data_pool);
    struct tws_data_trans *t = malloc((u32)size + trans_size);
    if (!t) return 0;
    memset(t, 0, trans_size);
    lbuf_init(t->data_pool, size, 4, 0);
    os_sem_create(&t->cmd_sem, 0);
    t->frame_head.next = t->frame_head.prev = &t->frame_head;
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
        do { channel = g_channel++; } while (!channel);
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
    if (config_asser ? len >= 8 : len >= 8) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_data_trans.c", 507);
        printf("ASSERT-FAILD: len < 8 len = %d\n", len);
        if (config_asser) cpu_assert_debug(); else p33_soft_reset();
    }
    memcpy(cmd + 2, arg, len);
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
    memset(b,0,offsetof(struct tws_tx_frame, data.data));
    struct tws_tx_frame *tx=(void *)b;
    tx->tx.rx_head_offset=32; tx->tx.llid=2; tx->tx.len=0;
    tx->data.channel=channel;
    struct list_head *head=&t->frame_head, *e=&tx->data.entry;
    e->next=head; e->prev=head->prev; head->prev->next=e; head->prev=e;
    local_irq_enable(); return tx->data.data;
}

int tws_api_data_trans_push(u8 channel, void *_frame, int len)
{
    u8 *frame = _frame;
    local_irq_disable(); struct tws_data_trans *t=find_trans(channel);
    if(!t){local_irq_enable();return -22;}
    local_irq_disable();
    struct tws_data_frame *f=(void *)(frame - offsetof(struct tws_data_frame, data));
    f->send_state=1; f->head.len=(u16)(len+12);
    struct list_head *n;
    for(n=t->frame_head.next;n!=&t->frame_head;n=n->next){
        struct tws_data_frame *q=(void *)((u8*)n - offsetof(struct tws_data_frame,entry));
        if(q->send_state==3) continue;
        struct lmp_connection *link=tws_link.link;
        if(!link || !(t->remote_inited && tws_conn.connect_suss)) { q->send_state=3; continue; }
        if(q->send_state==2) continue;
        if(q->send_state==0) break;
        if(t->state==3) break;
        q->send_state=2; q->deleted=0; q->head.opcode=0; q->head.channel=14;
        struct bredr_tx_bulk *tx=(void *)((u8 *)q - offsetof(struct tws_tx_frame, data));
        tx->event_post=68; tx->len=(u16)(q->head.len+4);
        link_conn_tx_bulk_avaiable(link,tx);
    }
    local_irq_enable(); local_irq_enable(); return 0;
}

int tws_api_data_trans_send(u8 ch,u8 *buf,int len)
{ void *p=tws_api_data_trans_buf_alloc(ch,len); if(!p)return 0; memcpy(p,buf,len); tws_api_data_trans_push(ch,p,len); return len; }

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

void *tws_api_data_trans_fetch(u8 ch,void *prev,int *len)
{ local_irq_disable(); struct tws_data_trans*t=find_trans(ch); struct tws_data_frame*f=0; if(t){if(!prev)f=tws_api_first_media_frame(t,0);else {struct list_head*n=*(struct list_head **)((u8*)prev-12); if(n!=&t->frame_head){f=(void*)((u8*)n-4);if(f->send_state!=3)f=0;}}} local_irq_enable(); if(f && len)*len=(int)f->head.len-12; return f?f->data:0; }

void tws_api_data_trans_clear(u8 ch)
{ local_irq_disable(); struct tws_data_trans*t=find_trans(ch); if(t)data_trans_frame_clear(t); local_irq_enable(); }

int tws_api_data_trans_check(u8 ch,u16 *ready,u16 *total)
{ local_irq_disable(); *ready=*total=0; struct tws_data_trans*t=find_trans(ch); if(!t){local_irq_enable();return -22;} for(struct list_head*n=t->frame_head.next;n!=&t->frame_head;n=n->next){struct tws_data_frame*f=(void*)((u8*)n - offsetof(struct tws_data_frame,entry));if(f->send_state && !f->deleted){*total=(u16)(f->head.len-12);if(f->send_state==3)*ready=(u16)(*ready+f->head.len-12);}} local_irq_enable();return 0; }

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
                if (!deadline) t->rx_timeout_jiffies=(30 / jiffies_unit)+jiffies;
                else if ((s32)jiffies < (s32)deadline) return -12;
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
        e->next = head; e->prev = head->prev; head->prev->next = e; head->prev = e;
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
        tx->data.send_state=3;
        struct tws_data_trans *t=find_trans(channel);
        if (t && t->state==0 && data_trans_frame_clear(t)) __release_trans(t);
    }
    return 0;
}
static void data_trans_cmd_hanlder(u8 *data, u16 len, u8 role)
{
    u8 channel = data[1];
    struct tws_data_trans *t;
    switch (data[0]) {
    case 1:
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
