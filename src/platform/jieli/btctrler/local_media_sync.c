/* The parts of the SDK's TWS local media sync (local_media_sync.c) that
 * retained TWS code (tws_pair) still names: counting and flushing the queue
 * of received media frames, the receive path that queues them, and the pool
 * hooks.  The channel is TWS earbud audio and is never opened here
 * (trans_start and media_pool are only set by the API functions that went
 * with the member), but the code is kept as the SDK has it.  Quirks kept:
 * slot_timer 0xff means "none"; the media frame sequence check is
 * ((3 - previous) + new) < 4 in unsigned arithmetic; the receive hook for
 * the application (tws_api_local_media_sync_rx_handler_notify) is a weak
 * stub that returns 0; list walks load the next node before the body. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

void *lbuf_alloc(struct lbuff_head *head, u32 len);
int lbuf_free(void *lbuf);

/* Queue of received frames (their bredr_rx_bulk entries), and state.  The
 * SDK's globals are file-static with no section. */
static struct list_head frame_head = { &frame_head, &frame_head };
static u8 slot_timer = 0xff;
static _Bool trans_start __attribute__((aligned(4)));
static u8 *media_pool;
static u8 rx_seqn;
static u32 pre_tws_local_seq_num;
static u8 local_tws_rx_cnt;

/* Weak: an application hook for a received frame; the SDK's does nothing
 * (the host test supplies its own). */
#ifndef UNITTEST
__attribute__((weak)) int tws_api_local_media_sync_rx_handler_notify(u8 *data, int len, u8 rx)
{
    (void)data; (void)len; (void)rx;
    return 0;
}
#endif

static void list_add_tail(struct list_head *new)
{
    struct list_head *prev = frame_head.prev;
    frame_head.prev = new;
    new->next = &frame_head;
    new->prev = prev;
    prev->next = new;
}

/* The queued frame a list node belongs to: the node is the frame's rx.entry. */
#define NODE_FRAME(n) ((struct tws_media_frame *)((u8 *)(n) - offsetof(struct tws_media_frame, rx.entry)))

/* Frames queued with go set count as packets; the rest are waiting to be
 * sent.  The wait count is a byte and wraps. */
int tws_api_local_media_packet_cnt(u8 *rx_packet_cnt, u8 *wait_send_pcaket_cnt)
{
    int packet_cnt = 0;
    u8 wait_send_cnt = 0;
    struct list_head *n, *next;

    local_irq_disable();
    for (n = frame_head.next; next = n->next, n != &frame_head; n = next) {
        if (NODE_FRAME(n)->head.go == 1)
            packet_cnt++;
        else
            wait_send_cnt++;
    }
    if (rx_packet_cnt)
        *rx_packet_cnt = local_tws_rx_cnt;
    if (wait_send_pcaket_cnt)
        *wait_send_pcaket_cnt = wait_send_cnt;
    local_irq_enable();
    return packet_cnt;
}

/* total_size of the pool (the lbuf head at +24), 0 without a pool. */
int tws_api_local_media_trans_get_total_buffer_size(void)
{
    return media_pool ? *(int *)(media_pool + 24) : 0;
}

/* A frame for an incoming media packet: 36 bytes of header plus the packet's
 * 16-bit length; the 28-byte rx header is cleared.  No pool or transfer not
 * started: NULL with err 0; pool full: NULL with err -12. */
void *tws_local_media_sync_rx_bulk_alloc(u8 *data, int size, int *err)
{
    void *frame;

    (void)size;
    if (!(trans_start && media_pool)) {
        *err = 0;
        return NULL;
    }
    frame = lbuf_alloc((struct lbuff_head *)media_pool, *(u16 *)data + 36);
    if (!frame) {
        *err = -12;
        return NULL;
    }
    memset(frame, 0, 28);
    return frame;
}

/* Close the channel (2) now when no slot timer exists, else on the timer. */
static void channel_close_later(u32 slots)
{
    u8 timer = slot_timer;
    if (timer == 0xff)
        tws_conn_channel_close(2);
    else
        slot_timer_set(timer, (void (*)(void *))(void *)tws_conn_channel_close, (void *)2, slots);
}

/* Receive interrupt: end flags 1 and 2 close the channel (2 after 10 slots
 * and carry on, 2 after 8 slots with the frame dropped); a frame is queued
 * only when started, new in sequence and not within 3 of the last. */
void tws_local_media_sync_rx_handler_in_irq(struct bredr_rx_bulk *rx)
{
    struct tws_media_frame *frame = (struct tws_media_frame *)rx;
    u8 seqn;

    switch (frame->head.endflag) {
    case 1:
        channel_close_later(10);
        break;
    case 2:
        lbuf_free(rx);
        channel_close_later(8);
        return;
    }
    if (!trans_start || rx_seqn == frame->head.seqn) {
        lbuf_free(rx);
        return;
    }
    seqn = frame->head.seqn;
    rx_seqn = seqn;
    if ((u32)(3 - pre_tws_local_seq_num + seqn) < 4) {
        lbuf_free(rx);
        return;
    }
    pre_tws_local_seq_num = seqn;
    frame->head.go = 1;
    frame->head.th.len -= 4;
    list_add_tail(&rx->entry);
    tws_api_local_media_sync_rx_handler_notify(frame->data, frame->head.th.len, 1);
}

/* After a TWS resume: with the transfer started, drop the slot timer when an
 * eSCO link exists, else (re)acquire and disable one and reopen channel 2.
 * Then un-ready every queued frame that was never given to the consumer
 * (go clear), stopping at the first frame that is not ready. */
void tws_local_media_sync_resume(void)
{
    struct list_head *n, *next;

    if (trans_start) {
        struct lmp_esco *esco = tws_conn.lmp_conn->esco_conn;
        u8 timer = slot_timer;

        if (esco) {
            if (timer != 0xff) {
                slot_timer_put(timer);
                slot_timer = 0xff;
            }
        } else {
            int disable = 1;
            if (timer == 0xff) {
                timer = slot_timer_get();
                slot_timer = timer;
                if (timer == 0xff)
                    disable = 0;
            }
            if (disable)
                slot_timer_disable(timer);
            tws_conn_channel_open(2);
        }
    }
    for (n = frame_head.next; next = n->next, n != &frame_head; n = next) {
        struct tws_media_frame *frame = NODE_FRAME(n);
        if (frame->head.ready == 0)
            break;
        if (frame->head.go == 0)
            frame->head.ready = 0;
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

struct tws_connection tws_conn;
static struct lmp_connection lmp;
static struct lmp_esco esco;

static char trace[128];
static int alloc_fail, freed, alloc_calls, notify_calls, notify_len, notify_rx;
static u32 alloc_len;
static void *alloc_head;
static u8 *notify_data;
static u8 next_timer, put_timer, disabled_timer, set_timer, set_slots;
static void *set_priv;
static void (*set_handler)(void *);
static int irq_depth, closes, opens, close_arg, open_arg;
static void *freed_ptr[8];

static void tr(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }
void local_irq_disable(void) { irq_depth++; tr('d'); }
void local_irq_enable(void) { irq_depth--; tr('e'); }
void *lbuf_alloc(struct lbuff_head *head, u32 len)
{
    alloc_head = head; alloc_len = len; alloc_calls++;
    return alloc_fail ? NULL : memset(malloc(len), 0xAA, len);
}
int lbuf_free(void *p) { freed_ptr[freed++] = p; tr('F'); return 0; }
void slot_timer_set(u8 i, void (*h)(void *), void *priv, int slots) { set_timer = i; set_handler = h; set_priv = priv; set_slots = slots; tr('S'); }
u8 slot_timer_get(void) { tr('g'); return next_timer; }
void slot_timer_put(u8 i) { put_timer = i; tr('p'); }
void slot_timer_disable(u8 i) { disabled_timer = i; tr('x'); }
void tws_conn_channel_open(u8 c) { opens++; open_arg = c; tr('O'); }
void tws_conn_channel_close(u8 c) { closes++; close_arg = c; tr('C'); }
int tws_api_local_media_sync_rx_handler_notify(u8 *d, int len, u8 rx) { notify_calls++; notify_data = d; notify_len = len; notify_rx = rx; tr('N'); return 7; }

static void reset(void)
{
    frame_head.next = frame_head.prev = &frame_head;
    slot_timer = 0xff; trans_start = 0; media_pool = NULL; rx_seqn = 0; pre_tws_local_seq_num = 0;
    local_tws_rx_cnt = 0;
    memset(trace, 0, sizeof(trace)); alloc_fail = 0; freed = 0; alloc_calls = 0; notify_calls = 0;
    next_timer = 0xff; put_timer = disabled_timer = set_timer = 0; irq_depth = 0; closes = opens = 0;
    set_handler = NULL; set_priv = NULL; set_slots = 0;
    memset(&tws_conn, 0, sizeof(tws_conn)); memset(&lmp, 0, sizeof(lmp)); memset(&esco, 0, sizeof(esco));
    tws_conn.lmp_conn = &lmp;
}

static struct tws_media_frame frames[4];
static void queue(int i, u8 ready, u8 go)
{
    memset(&frames[i], 0, sizeof(frames[i]));
    frames[i].head.ready = ready; frames[i].head.go = go;
    list_add_tail(&frames[i].rx.entry);
}

int main(void)
{
    u8 rxc = 0x77, wait = 0x77;

    /* Packet counts. */
    reset();
    CHECK(tws_api_local_media_packet_cnt(&rxc, &wait) == 0 && rxc == 0 && wait == 0);
    queue(0, 1, 1); queue(1, 1, 0); queue(2, 0, 1); queue(3, 0, 0);
    local_tws_rx_cnt = 9;
    CHECK(tws_api_local_media_packet_cnt(&rxc, &wait) == 2 && rxc == 9 && wait == 2);
    CHECK(irq_depth == 0 && !strcmp(trace, "dede"));
    /* go values other than 1 count as waiting; NULL out-pointers are skipped. */
    reset(); queue(0, 1, 2); queue(1, 1, 1);
    CHECK(tws_api_local_media_packet_cnt(NULL, NULL) == 1);
    reset(); queue(0, 1, 1);
    rxc = wait = 0x55;
    CHECK(tws_api_local_media_packet_cnt(&rxc, NULL) == 1 && rxc == 0 && wait == 0x55);
    CHECK(tws_api_local_media_packet_cnt(NULL, &wait) == 1 && wait == 0);

    /* Buffer size comes from the pool head's +24. */
    reset();
    CHECK(tws_api_local_media_trans_get_total_buffer_size() == 0);
    {
        u32 pool[8] = { 0 }; pool[6] = 1234; media_pool = (u8 *)pool;
        CHECK(tws_api_local_media_trans_get_total_buffer_size() == 1234);
    }

    /* rx_bulk_alloc: needs both trans_start and a pool. */
    {
        int err = 99; u8 data[4] = { 0x34, 0x12 }; void *p; u32 pool[8] = { 0 };
        reset();
        CHECK(tws_local_media_sync_rx_bulk_alloc(data, 5, &err) == NULL && err == 0 && alloc_calls == 0);
        err = 99; trans_start = 1;
        CHECK(tws_local_media_sync_rx_bulk_alloc(data, 5, &err) == NULL && err == 0 && alloc_calls == 0);
        err = 99; trans_start = 0; media_pool = (u8 *)pool;
        CHECK(tws_local_media_sync_rx_bulk_alloc(data, 5, &err) == NULL && err == 0 && alloc_calls == 0);
        trans_start = 1; alloc_fail = 1; err = 99;
        CHECK(tws_local_media_sync_rx_bulk_alloc(data, 5, &err) == NULL && err == -12);
        CHECK(alloc_len == 0x1234 + 36 && alloc_head == (void *)pool);
        alloc_fail = 0; err = 99;
        p = tws_local_media_sync_rx_bulk_alloc(data, 5, &err);
        CHECK(p != NULL && err == 99);
        for (int i = 0; i < 28; i++) CHECK(((u8 *)p)[i] == 0);
        CHECK(((u8 *)p)[28] == 0xAA && ((u8 *)p)[35] == 0xAA);   /* only 28 bytes cleared */
        free(p);
    }

    /* rx handler: no end flag, not started: freed. */
    reset();
    memset(&frames[0], 0, sizeof(frames[0])); frames[0].head.seqn = 5;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 1 && freed_ptr[0] == &frames[0] && frame_head.next == &frame_head && notify_calls == 0);
    /* started, same sequence number as the last: freed. */
    reset(); trans_start = 1; rx_seqn = 5;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 1 && notify_calls == 0);
    /* started, new sequence: window test ((3 - pre) + seq) < 4. pre=10: seq 7..10 are inside. */
    reset(); trans_start = 1; pre_tws_local_seq_num = 10; frames[0].head.seqn = 7; frames[0].head.endflag = 0;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 1 && rx_seqn == 7 && pre_tws_local_seq_num == 10 && notify_calls == 0);
    reset(); trans_start = 1; pre_tws_local_seq_num = 10; frames[0].head.seqn = 10; rx_seqn = 3;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 1 && rx_seqn == 10 && notify_calls == 0);
    /* seq 6 is outside (3-10+6 = -1 wraps to a huge value): queued; so is 11 below (3-10+11 = 4). */
    reset(); trans_start = 1; pre_tws_local_seq_num = 10; frames[0].head.seqn = 6;
    frames[0].head.th.len = 20; frames[0].head.go = 0; frames[0].head.endflag = 0;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 0 && rx_seqn == 6 && pre_tws_local_seq_num == 6 && frames[0].head.go == 1);
    CHECK(frames[0].head.th.len == 16 && frame_head.prev == &frames[0].rx.entry && frame_head.next == &frames[0].rx.entry);
    CHECK(frames[0].rx.entry.next == &frame_head && frames[0].rx.entry.prev == &frame_head);
    CHECK(notify_calls == 1 && notify_data == frames[0].data && notify_len == 16 && notify_rx == 1);
    /* a second frame is appended at the tail */
    frames[1].head.seqn = 20; frames[1].head.th.len = 10; frames[1].head.endflag = 0;
    tws_local_media_sync_rx_handler_in_irq(&frames[1].rx);
    CHECK(frame_head.prev == &frames[1].rx.entry && frames[0].rx.entry.next == &frames[1].rx.entry && frames[1].rx.entry.prev == &frames[0].rx.entry);
    reset(); trans_start = 1; pre_tws_local_seq_num = 10; frames[0].head.seqn = 11; frames[0].head.endflag = 0; frames[0].head.th.len = 8;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(freed == 0 && notify_calls == 1 && rx_seqn == 11 && pre_tws_local_seq_num == 11);   /* 3-10+11 = 4, not < 4 */

    /* end flag 2: frame freed, close now with no timer; returns. */
    reset(); trans_start = 1; frames[0].head.endflag = 2; frames[0].head.seqn = 1;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(!strcmp(trace, "FC") && closes == 1 && close_arg == 2 && notify_calls == 0 && frame_head.next == &frame_head);
    /* end flag 2 with a timer: set it to close after 8 slots. */
    reset(); slot_timer = 3; frames[0].head.endflag = 2;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(!strcmp(trace, "FS") && set_timer == 3 && set_slots == 8 && set_priv == (void *)2 && (void *)set_handler == (void *)tws_conn_channel_close && closes == 0);
    /* end flag 1: close (now or in 10 slots) and carry on to the normal path (not started: freed). */
    reset(); frames[0].head.endflag = 1;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(!strcmp(trace, "CF") && closes == 1);
    reset(); slot_timer = 4; frames[0].head.endflag = 1;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(!strcmp(trace, "SF") && set_timer == 4 && set_slots == 10);
    /* unknown end flag behaves as 0 */
    reset(); frames[0].head.endflag = 3;
    tws_local_media_sync_rx_handler_in_irq(&frames[0].rx);
    CHECK(!strcmp(trace, "F"));

    /* resume, not started: only the frame walk. */
    reset(); queue(0, 1, 0); queue(1, 1, 1); queue(2, 0, 0); queue(3, 1, 0);
    tws_local_media_sync_resume();
    CHECK(frames[0].head.ready == 0 && frames[1].head.ready == 1 && frames[2].head.ready == 0 && frames[3].head.ready == 1);
    CHECK(trace[0] == 0);
    /* resume, started, eSCO link, timer held: put and forget it. */
    reset(); trans_start = 1; lmp.esco_conn = &esco; slot_timer = 6;
    tws_local_media_sync_resume();
    CHECK(!strcmp(trace, "p") && put_timer == 6 && slot_timer == 0xff && opens == 0);
    /* eSCO link, no timer: nothing. */
    reset(); trans_start = 1; lmp.esco_conn = &esco;
    tws_local_media_sync_resume();
    CHECK(trace[0] == 0);
    /* no eSCO link, timer held: disable it and open channel 2. */
    reset(); trans_start = 1; slot_timer = 6;
    tws_local_media_sync_resume();
    CHECK(!strcmp(trace, "xO") && disabled_timer == 6 && slot_timer == 6 && open_arg == 2);
    /* no eSCO link, no timer: get one, store it, disable it, open. */
    reset(); trans_start = 1; next_timer = 9;
    tws_local_media_sync_resume();
    CHECK(!strcmp(trace, "gxO") && slot_timer == 9 && disabled_timer == 9);
    /* no timer available: stored as 0xff, not disabled, channel still opened. */
    reset(); trans_start = 1;
    tws_local_media_sync_resume();
    CHECK(!strcmp(trace, "gO") && slot_timer == 0xff);
    return TEST_DONE("btctrler_local_media_sync");
}
#endif
