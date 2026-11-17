/* The parts of the SDK's TWS ACL data forwarding (tws_data_forwarding.c)
 * that retained TWS code (tws_pair, tws_task, tws_low_latency) still names:
 * init, channel open/close, the resume hook, clearing the slave's abandon
 * count and the receive path that reassembles forwarded ACL data from the
 * peer.  The channel table, event handler and master side are earbud code
 * with no callers and go with the member.  Quirks kept as the SDK has them:
 * tws_slave_data_abandon keeps matching after its count reaches zero (the
 * count is a plain int and only 255 means "by channel"), reading hashes past
 * the abandon list; the copy length size - offset - 20 is not checked for
 * being negative; the receive allocation retries after discarding a queued
 * frame of the SBC channel and gives up with -12 once alloc has failed five
 * times in a row; asserts on size and fragment length continue after the
 * report. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define EDR2DH5_SIZE 679

int os_taskq_post_type(const char *name, int type, int argc, int *argv);

/* Both file-static in the SDK, no section: the forwarder's state (zero) and
 * the slot timer taken by tws_data_sync_init. */
static struct tws_data_sync sync;
static u8 slot_timer;

#define NODE_BULK(n) ((struct bredr_rx_bulk *)((u8 *)(n) - offsetof(struct bredr_rx_bulk, entry)))

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

static void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

/* The SDK's list_empty() here is hard-wired to the pending queue. */
static int list_empty(void)
{
    return tws_conn.rx_pending.next == &tws_conn.rx_pending;
}

/* Move a queued frame to the Classic link's receive list, flagging the
 * event as pending when none was. */
static void to_link_rx(struct list_head *node) TWS_CODE;
static void to_link_rx(struct list_head *node)
{
    __list_del_entry(node);
    list_add_tail(node, &tws_conn.lmp_conn->link->rx_head);
    if (tws_conn.send_event == 1)
        tws_conn.send_event = 2;
}

/* Drop queued frames the peer reports abandoned: by hash, in order, while
 * frames are ready; or (count 255) every frame of the SBC channel. */
static void tws_slave_data_abandon(struct item_abandon *abandon) TWS_CODE;
static void tws_slave_data_abandon(struct item_abandon *abandon)
{
    int num = abandon->num;
    u32 *hash = abandon->hash;
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    struct list_head *node = tws_conn.rx_pending.next, *next;

    for (;;) {
        next = node->next;
        if (node == &tws_conn.rx_pending)
            return;
        if (num == 255) {
            struct bredr_rx_bulk *bulk = NODE_BULK(node);
            if (lmp->sbc_channel != (bulk->data[2] | bulk->data[3] << 8)) {
                node = next;
                continue;
            }
        } else {
            struct bredr_rx_bulk *bulk = NODE_BULK(node);
            if (!bulk->ready)
                return;
            if ((u32)bulk->hash != *hash) {
                node = next;
                continue;
            }
            hash++;
            num--;
        }
        __list_del_entry(node);
        bredr_rx_bulk_free(NODE_BULK(node));
        node = next;
    }
}

void tws_data_sync_clear_abandon_num(void) TWS_CODE;
void tws_data_sync_clear_abandon_num(void)
{
    sync.abandon_num_s = 0;
}

/* After a resume on the master (role 0): frames the radio flagged lost and
 * sent are made sendable again from offset 0. */
void tws_data_sync_resume(void) TWS_CODE;
void tws_data_sync_resume(void)
{
    struct list_head *node;

    slot_timer_disable(slot_timer);
    if (tws_conn.role == 0) {
        for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = node->next) {
            struct bredr_rx_bulk *bulk = NODE_BULK(node);
            if (bulk->lost && bulk->send) {
                bulk->send = 0;
                bulk->offset = 0;
            }
        }
    }
}

/* Receive a forwarded sync packet from the peer (opcode_data_sync at data,
 * size bytes): optional abandon item, then one ACL data item, reassembling
 * fragments into sync.rx_bulk and passing finished frames to the Classic
 * link.  Returns -12 when no buffer could be had, else 0. */
int tws_data_sync_rx_bulk_alloc(u8 *data, int size) TWS_CODE;
int tws_data_sync_rx_bulk_alloc(u8 *data, int size)
{
    struct list_head *node, *next;
    struct bredr_rx_bulk *bulk;
    u8 *item;
    int offset, len, sub;
    u32 hash, prev_hash;

    if (tws_conn.monitor_start != 2)
        return 0;
    if (size > EDR2DH5_SIZE)
        btctrler_assert_failed("size <= EDR2DH5_SIZE", size);
    tws_conn_send_event_check();
    if (data[4] == 1) {
        data[4] = 2;
        if (tws_conn.instant != 0)
            slot_timer_set(slot_timer, (void (*)(void *))(void *)tws_conn_channel_close, (void *)1, 8);
    }
    if (size < 13)
        goto end;
    if (data[12] == 1) {
        u8 num = data[13];
        if (num == 0xff) {
            tws_slave_data_abandon((struct item_abandon *)&data[12]);
            data[13] = 0;
            num = 0;
        } else if (num != 0) {
            struct item_abandon *abandon = (struct item_abandon *)&data[12];
            if (abandon->hash[0] != 0) {
                tws_slave_data_abandon(abandon);
                abandon->hash[0] = 0;
                num = data[13];
            }
        }
        offset = (num << 2) + 16;
    } else {
        offset = 12;
    }
    if (!(offset < size))
        goto end;
    item = data + offset;
    if (item[0] != 0) {
        /* Not data: drop a half-built frame. */
        if (sync.rx_bulk) {
            bredr_rx_bulk_free(sync.rx_bulk);
            sync.rx_bulk = NULL;
        }
        return 0;
    }
    {
        struct item_acl_data *acl = (struct item_acl_data *)item;
        u32 clkn = acl->clkn;

        if (sync.slave_last_rx_clkn == clkn && acl->offset == 0)
            return 0;
        if (sync.slave_last_rx_clkn > clkn)
            goto end;
        if (acl->llid == 2)
            len = (acl->data[1] << 8 | acl->data[0]) + 4;
        else
            len = acl->len;
        bulk = sync.rx_bulk;
        if (bulk) {
            if ((u32)bulk->hash == acl->hash && acl->len + bulk->offset == bulk->len)
                goto data_copy;
            bredr_rx_bulk_free(bulk);
            sync.rx_bulk = NULL;
        }
        if (len > 2047)
            return 0;
        prev_hash = acl->prev_hash;
        if (prev_hash != 0) {
            /* Everything up to the frame the peer says came before this one
             * goes to the link first. */
            for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = node->next) {
                if ((u32)NODE_BULK(node)->hash != prev_hash)
                    continue;
                for (struct list_head *in = tws_conn.rx_pending.next;; in = next) {
                    next = in->next;
                    if (in == &tws_conn.rx_pending)
                        break;
                    to_link_rx(in);
                    if (in == node)
                        break;
                }
                break;
            }
            acl->prev_hash = 0;
        }
        for (;;) {
            bulk = link_conn_rx_bulk_alloc_for(tws_conn.lmp_conn->link, len);
            sync.rx_bulk = bulk;
            if (bulk)
                break;
            sync.no_rx_bulk++;
            if (sync.no_rx_bulk == 50)
                os_taskq_post_type("tws", 45, 0, NULL);
            if (sync.no_rx_bulk < 5)
                return -12;
            {
                struct lmp_connection *lmp = tws_conn.lmp_conn;
                int freed = 0;
                for (node = tws_conn.rx_pending.prev; node != &tws_conn.rx_pending; node = node->prev) {
                    struct bredr_rx_bulk *old = NODE_BULK(node);
                    if ((old->data[2] | old->data[3] << 8) == lmp->sbc_channel) {
                        __list_del_entry(node);
                        bredr_rx_bulk_free(old);
                        freed = 1;
                        break;
                    }
                }
                if (!freed)
                    return -12;
            }
        }
        sync.no_rx_bulk = 0;
        bulk->len = (u16)len;
    data_copy:
        hash = acl->hash;
        sub = size - offset - 20;
        sync.slave_last_rx_clkn = acl->clkn;
        sync.slave_last_rx_offset = (u16)(acl->offset + sub);
        {
            struct lmp_connection *lmp = tws_conn.lmp_conn;

            /* tws_slave_data_copy: first fragment sets the frame's header,
             * later ones must continue exactly where it left off. */
            if (bulk->offset == 0) {
                bulk->len = acl->len;
                bulk->llid = acl->llid;
                bulk->hash = acl->hash;
                bulk->clkn = acl->clkn;
            } else if (!(bulk->offset + sub == bulk->len)) {
                btctrler_assert_failed("len + bulk->offset == bulk->len", 0);
            }
            memcpy(&bulk->data[bulk->offset], acl->data, sub);
            bulk->offset = (u16)(bulk->offset + sub);
            if (bulk->len == bulk->offset) {
                sync.rx_bulk = NULL;
                list_add_tail(&bulk->entry, &lmp->link->rx_head);
                if (tws_conn.send_event == 1)
                    tws_conn.send_event = 2;
            }
        }
        if (acl->offset != 0)
            return 0;
        tws_conn_history_hash_add(hash);
        if (list_empty() || !NODE_BULK(tws_conn.rx_pending.next)->ready) {
            u8 n = sync.abandon_num_s;
            if (n == 2) {
                sync.abandon_num_s = 1;
                sync.abandon_hash_s[0] = sync.abandon_hash_s[1];
                n = 1;
            }
            sync.abandon_num_s = n + 1;
            sync.abandon_hash_s[n] = hash;
        }
    }
end:
    if (data[4] != 0) {
        for (node = tws_conn.rx_pending.next;; node = next) {
            next = node->next;
            if (node == &tws_conn.rx_pending)
                break;
            if (!NODE_BULK(node)->ready)
                break;
            if (NODE_BULK(node)->clkn > ((struct opcode_data_sync *)data)->clkn)
                break;
            to_link_rx(node);
        }
    }
    tws_conn_try_send_event_to_lmp();
    return 0;
}

/* Take a slot timer for the data channel. */
void tws_data_sync_init(void) TWS_CODE;
void tws_data_sync_init(void)
{
    slot_timer = slot_timer_get();
}

void tws_data_sync_channel_open(void) TWS_CODE;
void tws_data_sync_channel_open(void)
{
    sync.channel_open = 1;
    sync.master_last_rx_clkn = 0;
}

void tws_data_sync_channel_close(void) TWS_CODE;
void tws_data_sync_channel_close(void)
{
    sync.channel_open = 0;
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

struct tws_connection tws_conn;
static struct lmp_connection lmp;
static struct link_connection lcon;

static char trace[512];
static int frees, alloc_calls, alloc_fail_budget, alloc_len, taskq_posts, taskq_last_type, send_checks, try_sends, close_calls;
static int asserts, assert_value, set_calls, set_slots, set_timer, disable_timer, next_timer, hash_adds, hash_added;
static void *set_priv;
static void (*set_handler)(void *);
static struct bredr_rx_bulk *freed_bulk[16], *last_alloc;
static char assert_msg[64];

static void tr(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }
void slot_timer_disable(u8 i) { disable_timer = i; tr('x'); }
void slot_timer_set(u8 i, void (*h)(void *), void *priv, int slots) { set_timer = i; set_handler = h; set_priv = priv; set_slots = slots; set_calls++; tr('S'); }
u8 slot_timer_get(void) { return (u8)next_timer; }
void bredr_rx_bulk_free(struct bredr_rx_bulk *b) { if (frees < 16) freed_bulk[frees] = b; frees++; tr('F'); }
struct bredr_rx_bulk *link_conn_rx_bulk_alloc_for(struct link_connection *lc, int len)
{
    CHECK(lc == &lcon);
    alloc_calls++; alloc_len = len; tr('A');
    if (alloc_fail_budget > 0) { alloc_fail_budget--; return NULL; }
    last_alloc = calloc(1, sizeof(struct bredr_rx_bulk) + 4096);
    return last_alloc;
}
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    CHECK(!strcmp(name, "tws") && argc == 0 && argv == NULL);
    taskq_posts++; taskq_last_type = type; tr('Q'); return 0;
}
void tws_conn_send_event_check(void) { send_checks++; tr('c'); }
void tws_conn_try_send_event_to_lmp(void) { try_sends++; tr('T'); }
void tws_conn_history_hash_add(int hash) { hash_adds++; hash_added = hash; tr('H'); }
void tws_conn_channel_close(u8 c) { (void)c; close_calls++; }
void btctrler_assert_failed(const char *what, int value)
{
    asserts++; assert_value = value; snprintf(assert_msg, sizeof(assert_msg), "%s", what); tr('!');
}

static struct bredr_rx_bulk *new_bulk(u32 hash, u32 clkn, int ready, u16 cid)
{
    struct bredr_rx_bulk *b = calloc(1, sizeof(*b) + 64);
    b->hash = hash; b->clkn = clkn; b->ready = ready; b->data[2] = cid & 0xff; b->data[3] = cid >> 8;
    return b;
}
static void pend(struct bredr_rx_bulk *b) { list_add_tail(&b->entry, &tws_conn.rx_pending); }
static int count(struct list_head *h) { int n = 0; for (struct list_head *e = h->next; e != h; e = e->next) n++; return n; }
static struct bredr_rx_bulk *nth(struct list_head *h, int i)
{
    struct list_head *e = h->next;
    while (i--) e = e->next;
    return NODE_BULK(e);
}

static void reset(void)
{
    memset(&sync, 0, sizeof(sync)); slot_timer = 0x21;
    memset(&tws_conn, 0, sizeof(tws_conn)); memset(&lmp, 0, sizeof(lmp)); memset(&lcon, 0, sizeof(lcon));
    tws_conn.rx_pending.next = tws_conn.rx_pending.prev = &tws_conn.rx_pending;
    lcon.rx_head.next = lcon.rx_head.prev = &lcon.rx_head;
    lmp.link = &lcon; tws_conn.lmp_conn = &lmp; tws_conn.monitor_start = 2; lmp.sbc_channel = 0x0041;
    memset(trace, 0, sizeof(trace)); frees = alloc_calls = alloc_fail_budget = alloc_len = taskq_posts = 0;
    send_checks = try_sends = close_calls = asserts = set_calls = hash_adds = 0;
    memset(freed_bulk, 0, sizeof(freed_bulk)); last_alloc = NULL; next_timer = 0x33;
}

/* A sync packet: opcode_data_sync header (flag at 4, clkn at 8) and items from 12. */
static u8 pkt[1024] __attribute__((aligned(4)));
static struct item_acl_data *item_at(int off) { return (struct item_acl_data *)&pkt[off]; }
static void clear_pkt(u8 flag, u32 clkn)
{
    memset(pkt, 0, sizeof(pkt)); pkt[4] = flag; ((struct opcode_data_sync *)pkt)->clkn = clkn;
}
static void make_item(int off, u8 llid, u16 len, u16 offset, u32 hash, u32 clkn, u32 prev, const u8 *payload, int n)
{
    struct item_acl_data *a = item_at(off);
    a->type = 0; a->llid = llid; a->len = len; a->offset = offset; a->hash = hash; a->clkn = clkn; a->prev_hash = prev;
    memcpy(a->data, payload, n);
}

int main(void)
{
    static const u8 one[1] = { 0 };

    /* trivial entry points */
    reset(); sync.abandon_num_s = 2; tws_data_sync_clear_abandon_num(); CHECK(sync.abandon_num_s == 0);
    reset(); sync.master_last_rx_clkn = 77; tws_data_sync_channel_open(); CHECK(sync.channel_open == 1 && sync.master_last_rx_clkn == 0);
    tws_data_sync_channel_close(); CHECK(sync.channel_open == 0);
    reset(); next_timer = 0x44; tws_data_sync_init(); CHECK(slot_timer == 0x44);
    next_timer = 0xff; tws_data_sync_init(); CHECK(slot_timer == 0xff);

    /* resume: the timer is disabled; on role 0 lost+sent frames become sendable again from offset 0 */
    {
        struct bredr_rx_bulk *a, *b, *c, *d;
        reset(); a = new_bulk(1, 0, 1, 0); b = new_bulk(2, 0, 1, 0); c = new_bulk(3, 0, 1, 0); d = new_bulk(4, 0, 1, 0);
        a->lost = 1; a->send = 1; a->offset = 9; b->lost = 1; b->offset = 8; c->send = 1; c->offset = 7;
        d->lost = d->send = 1; d->offset = 6; d->ready = 0;
        pend(a); pend(b); pend(c); pend(d);
        tws_conn.role = 0;
        tws_data_sync_resume();
        CHECK(disable_timer == 0x21 && !strcmp(trace, "x"));
        CHECK(!a->send && a->lost && a->offset == 0);
        CHECK(!b->send && b->lost && b->offset == 8);       /* only lost */
        CHECK(c->send && !c->lost && c->offset == 7);       /* only sent */
        CHECK(!d->send && d->offset == 0 && !d->ready);     /* ready is not consulted */
        a->send = 1; a->offset = 5; tws_conn.role = 1;
        tws_data_sync_resume();
        CHECK(a->send && a->offset == 5);                   /* other roles: queue untouched */
    }

    /* receive: not monitoring */
    reset(); tws_conn.monitor_start = 1; clear_pkt(1, 0);
    CHECK(tws_data_sync_rx_bulk_alloc(pkt, 50) == 0 && trace[0] == 0 && pkt[4] == 1);

    /* size over 679 is reported and processing continues; flag 1 becomes 2 and arms the timer when instant is set */
    reset(); clear_pkt(1, 0); tws_conn.instant = 5;
    CHECK(tws_data_sync_rx_bulk_alloc(pkt, 680) == 0 && asserts == 1 && assert_value == 680 && !strcmp(assert_msg, "size <= EDR2DH5_SIZE"));
    CHECK(pkt[4] == 2 && set_calls == 1 && set_timer == 0x21 && set_slots == 8 && set_priv == (void *)1);
    CHECK((void *)set_handler == (void *)tws_conn_channel_close);
    CHECK(!strcmp(trace, "!cS"));   /* then the empty item at 12 (clock 0 == last, offset 0) ends it with no event */
    reset(); clear_pkt(1, 0);
    CHECK(tws_data_sync_rx_bulk_alloc(pkt, 679) == 0 && asserts == 0 && set_calls == 0 && pkt[4] == 2);
    reset(); clear_pkt(3, 0);
    tws_data_sync_rx_bulk_alloc(pkt, 12);          /* size < 13: straight to the ending */
    CHECK(pkt[4] == 3 && !strcmp(trace, "cT"));
    reset(); clear_pkt(0, 0);
    tws_data_sync_rx_bulk_alloc(pkt, 12);
    CHECK(!strcmp(trace, "cT"));

    /* one complete frame: llid 2, L2CAP length 3 => frame of 7 bytes in one 7-byte fragment */
    {
        static const u8 payload[7] = { 3, 0, 0x41, 0, 0xa0, 0xa1, 0xa2 };
        reset(); clear_pkt(0, 0);
        make_item(12, 2, 7, 0, 0x1111, 100, 0, payload, 7);
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 7) == 0);
        CHECK(alloc_calls == 1 && alloc_len == 7 && last_alloc->len == 7 && last_alloc->offset == 7 && last_alloc->llid == 2);
        CHECK(last_alloc->hash == 0x1111 && last_alloc->clkn == 100 && !memcmp(last_alloc->data, payload, 7));
        CHECK(sync.rx_bulk == NULL && count(&lcon.rx_head) == 1 && nth(&lcon.rx_head, 0) == last_alloc);
        CHECK(sync.slave_last_rx_clkn == 100 && sync.slave_last_rx_offset == 7);
        CHECK(hash_adds == 1 && hash_added == 0x1111 && sync.abandon_num_s == 1 && sync.abandon_hash_s[0] == 0x1111);
        CHECK(!strcmp(trace, "cAHT") && try_sends == 1);
        /* the same packet again: same clock and offset 0 is a duplicate */
        reset(); sync.slave_last_rx_clkn = 100; clear_pkt(0, 0);
        make_item(12, 2, 7, 0, 0x1111, 100, 0, payload, 7);
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 39) == 0 && !strcmp(trace, "c") && alloc_calls == 0);
        /* an older clock goes straight to the ending */
        reset(); sync.slave_last_rx_clkn = 101; clear_pkt(0, 0);
        make_item(12, 2, 7, 0, 0x1111, 100, 0, payload, 7);
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 39) == 0 && !strcmp(trace, "cT") && alloc_calls == 0);
    }

    /* two fragments: length 5 + 4 = 9; 5 bytes then 4 */
    {
        static const u8 first[5] = { 5, 0, 0x41, 0, 0xb0 };
        static const u8 second[4] = { 0xb1, 0xb2, 0xb3, 0xb4 };
        struct bredr_rx_bulk *b;
        reset(); clear_pkt(0, 0);
        make_item(12, 2, 9, 0, 0x2222, 100, 0, first, 5);
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 5);
        b = sync.rx_bulk;
        CHECK(b != NULL && alloc_len == 9 && b->len == 9 && b->offset == 5 && count(&lcon.rx_head) == 0);
        CHECK(sync.slave_last_rx_offset == 5 && hash_adds == 1);
        clear_pkt(0, 0); trace[0] = 0;
        make_item(12, 1, 4, 5, 0x2222, 200, 0, second, 4);
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 4) == 0);
        CHECK(alloc_calls == 1 && sync.rx_bulk == NULL && count(&lcon.rx_head) == 1 && b->offset == 9 && asserts == 0);
        CHECK(!memcmp(&b->data[5], second, 4) && b->data[0] == 5 && b->llid == 2);
        CHECK(sync.slave_last_rx_clkn == 200 && sync.slave_last_rx_offset == 9);
        CHECK(!strcmp(trace, "c") && hash_adds == 1 && try_sends == 1);   /* (the first call's one) a continuation returns before the hash and ending steps */
        /* a continuation whose remaining length does not fit: the old frame is freed and a new one started */
        reset(); clear_pkt(0, 0);
        make_item(12, 2, 9, 0, 0x2222, 100, 0, first, 5);
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 5);
        b = sync.rx_bulk; trace[0] = 0;
        clear_pkt(0, 0);
        make_item(12, 1, 3, 5, 0x2222, 200, 0, second, 4);                /* 3 + 5 != 9 */
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 4);
        CHECK(frees == 1 && freed_bulk[0] == b && alloc_calls == 2 && alloc_len == 3);
        CHECK(!strcmp(trace, "cFA"));
        /* a different hash is also freed */
        reset(); clear_pkt(0, 0);
        make_item(12, 2, 9, 0, 0x2222, 100, 0, first, 5);
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 5);
        b = sync.rx_bulk; clear_pkt(0, 0);
        make_item(12, 1, 4, 5, 0x3333, 200, 0, second, 4);
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 4);
        CHECK(frees == 1 && freed_bulk[0] == b);
    }

    /* not a data item: a half-built frame is dropped and nothing else happens */
    {
        struct bredr_rx_bulk *b = new_bulk(1, 0, 0, 0);
        reset(); clear_pkt(0, 0); pkt[12] = 2; sync.rx_bulk = b;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 40) == 0 && frees == 1 && freed_bulk[0] == b && sync.rx_bulk == NULL && !strcmp(trace, "cF"));
        reset(); clear_pkt(0, 0); pkt[12] = 2;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 40) == 0 && frees == 0 && !strcmp(trace, "c"));
        /* an item must start inside the packet: at size 13 the item at 12 is read (clock 0 == last, offset 0) */
        reset(); clear_pkt(0, 0);
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 13) == 0 && !strcmp(trace, "c"));
    }

    /* frame length limit: 2047 is accepted, 2048 is dropped */
    reset(); clear_pkt(0, 0); make_item(12, 1, 2048, 0, 1, 100, 0, one, 1);
    CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == 0 && alloc_calls == 0 && !strcmp(trace, "c"));
    reset(); clear_pkt(0, 0); make_item(12, 1, 2047, 0, 1, 100, 0, one, 1);
    tws_data_sync_rx_bulk_alloc(pkt, 33);
    CHECK(alloc_calls == 1 && alloc_len == 2047);

    /* allocation failures: four misses give -12; from the fifth queued frames of the SBC channel are discarded */
    {
        struct bredr_rx_bulk *a, *b, *c;
        reset(); clear_pkt(0, 0); make_item(12, 1, 10, 0, 1, 100, 0, one, 1);
        alloc_fail_budget = 1;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == -12 && sync.no_rx_bulk == 1 && sync.rx_bulk == NULL && alloc_calls == 1 && taskq_posts == 0);
        sync.no_rx_bulk = 3; alloc_fail_budget = 1; alloc_calls = 0;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == -12 && sync.no_rx_bulk == 4);
        /* the fifth failure with nothing to discard */
        sync.no_rx_bulk = 4; alloc_fail_budget = 1;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == -12 && sync.no_rx_bulk == 5 && frees == 0);
        /* with queued frames: the one nearest the tail on the SBC channel (0x41) goes and the allocation is retried */
        reset(); a = new_bulk(1, 0, 1, 0x41); b = new_bulk(2, 0, 1, 0x40); c = new_bulk(3, 0, 1, 0x41);
        pend(a); pend(b); pend(c);
        clear_pkt(0, 0); make_item(12, 1, 10, 0, 1, 100, 0, one, 1);
        sync.no_rx_bulk = 4; alloc_fail_budget = 1;
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(frees == 1 && freed_bulk[0] == c && count(&tws_conn.rx_pending) == 2 && alloc_calls == 2 && sync.no_rx_bulk == 0 && last_alloc != NULL);
        /* reaching 50 misses posts a message to the task and carries on */
        reset(); clear_pkt(0, 0); make_item(12, 1, 10, 0, 1, 100, 0, one, 1);
        sync.no_rx_bulk = 49; alloc_fail_budget = 1;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == -12 && taskq_posts == 1 && taskq_last_type == 45 && sync.no_rx_bulk == 50);
        /* the counter wraps as a byte: 255 -> 0 is below 5, so it gives up */
        reset(); clear_pkt(0, 0); make_item(12, 1, 10, 0, 1, 100, 0, one, 1);
        sync.no_rx_bulk = 255; alloc_fail_budget = 1;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 33) == -12 && sync.no_rx_bulk == 0 && alloc_calls == 1);
    }

    /* prev_hash: queued frames up to and including the matching one go to the link first */
    {
        struct bredr_rx_bulk *a, *b, *c;
        reset(); a = new_bulk(1, 0, 1, 0); b = new_bulk(2, 0, 1, 0); c = new_bulk(3, 0, 1, 0);
        pend(a); pend(b); pend(c); tws_conn.send_event = 1;
        clear_pkt(0, 0); make_item(12, 1, 10, 0, 9, 100, 2, one, 1);
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(count(&lcon.rx_head) == 2 && nth(&lcon.rx_head, 0) == a && nth(&lcon.rx_head, 1) == b);
        CHECK(count(&tws_conn.rx_pending) == 1 && nth(&tws_conn.rx_pending, 0) == c && tws_conn.send_event == 2);
        CHECK(item_at(12)->prev_hash == 0);
        /* no match: nothing moves, the hash is still cleared */
        reset(); a = new_bulk(1, 0, 1, 0); pend(a);
        clear_pkt(0, 0); make_item(12, 1, 10, 0, 9, 100, 77, one, 1);
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(count(&lcon.rx_head) == 0 && count(&tws_conn.rx_pending) == 1 && item_at(12)->prev_hash == 0);
    }

    /* abandon by hash: ready matching frames are discarded in order; matching goes on past the count (SDK quirk) */
    {
        struct bredr_rx_bulk *a, *b, *c, *d;
        reset(); a = new_bulk(11, 0, 1, 0); b = new_bulk(12, 0, 1, 0); c = new_bulk(13, 0, 1, 0); d = new_bulk(14, 0, 0, 0);
        pend(a); pend(b); pend(c); pend(d);
        clear_pkt(0, 0); pkt[12] = 1; pkt[13] = 2;
        ((u32 *)&pkt[16])[0] = 11; ((u32 *)&pkt[16])[1] = 12; ((u32 *)&pkt[16])[2] = 13;   /* the third is past the count of 2 */
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 25) == 0);
        CHECK(frees == 3 && freed_bulk[0] == a && freed_bulk[1] == b && freed_bulk[2] == c);
        CHECK(count(&tws_conn.rx_pending) == 1 && nth(&tws_conn.rx_pending, 0) == d);
        CHECK(((u32 *)&pkt[16])[0] == 0);                    /* the first hash is cleared after use */
        /* a frame that does not match is skipped and the walk goes on; a frame that is not ready ends it */
        reset(); a = new_bulk(11, 0, 1, 0); b = new_bulk(99, 0, 1, 0); c = new_bulk(12, 0, 1, 0); d = new_bulk(12, 0, 0, 0);
        pend(a); pend(b); pend(c); pend(d);
        clear_pkt(0, 0); pkt[12] = 1; pkt[13] = 2; ((u32 *)&pkt[16])[0] = 11; ((u32 *)&pkt[16])[1] = 12;
        tws_data_sync_rx_bulk_alloc(pkt, 25);
        CHECK(frees == 2 && freed_bulk[0] == a && freed_bulk[1] == c && count(&tws_conn.rx_pending) == 2);
        CHECK(nth(&tws_conn.rx_pending, 0) == b && nth(&tws_conn.rx_pending, 1) == d);
        /* first hash 0 means nothing to abandon even with a count */
        reset(); a = new_bulk(0, 0, 1, 0); pend(a);
        clear_pkt(0, 0); pkt[12] = 1; pkt[13] = 1;
        tws_data_sync_rx_bulk_alloc(pkt, 25);
        CHECK(frees == 0 && count(&tws_conn.rx_pending) == 1);
        /* count 0: nothing to abandon, the data item starts at 16 */
        reset(); clear_pkt(0, 0); pkt[12] = 1; pkt[13] = 0;
        CHECK(tws_data_sync_rx_bulk_alloc(pkt, 17) == 0 && !strcmp(trace, "c"));
    }
    /* abandon by channel (count 255): every queued frame of the SBC channel goes, ready or not; the count is rewritten to 0 */
    {
        struct bredr_rx_bulk *a, *b, *c;
        reset(); a = new_bulk(1, 0, 0, 0x41); b = new_bulk(2, 0, 1, 0x42); c = new_bulk(3, 0, 1, 0x41);
        pend(a); pend(b); pend(c);
        clear_pkt(0, 0); pkt[12] = 1; pkt[13] = 0xff;
        tws_data_sync_rx_bulk_alloc(pkt, 17);
        CHECK(frees == 2 && freed_bulk[0] == a && freed_bulk[1] == c && count(&tws_conn.rx_pending) == 1 && nth(&tws_conn.rx_pending, 0) == b);
        CHECK(pkt[13] == 0);
    }

    /* hash recording after a finished frame: when nothing is queued or the first queued frame is not ready */
    {
        struct bredr_rx_bulk *a;
        reset(); a = new_bulk(5, 0, 1, 0); pend(a);          /* first queued frame is ready: no record */
        clear_pkt(0, 0); make_item(12, 1, 1, 0, 0x77, 100, 0, one, 1);
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(hash_adds == 1 && sync.abandon_num_s == 0);
        reset(); a = new_bulk(5, 0, 0, 0); pend(a);          /* not ready: record */
        clear_pkt(0, 0); make_item(12, 1, 1, 0, 0x77, 100, 0, one, 1);
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(sync.abandon_num_s == 1 && sync.abandon_hash_s[0] == 0x77);
        /* the list holds two: a third shifts the older one out */
        sync.abandon_num_s = 2; sync.abandon_hash_s[0] = 1; sync.abandon_hash_s[1] = 2;
        clear_pkt(0, 0); make_item(12, 1, 1, 0, 0x88, 101, 0, one, 1);
        tws_data_sync_rx_bulk_alloc(pkt, 33);
        CHECK(sync.abandon_num_s == 2 && sync.abandon_hash_s[0] == 2 && sync.abandon_hash_s[1] == 0x88);
    }

    /* the ending: with the flag set, ready frames no later than the packet's clock go to the link */
    {
        struct bredr_rx_bulk *a, *b, *c, *d;
        reset(); a = new_bulk(1, 10, 1, 0); b = new_bulk(2, 20, 1, 0); c = new_bulk(3, 30, 1, 0); d = new_bulk(4, 5, 0, 0);
        pend(a); pend(b); pend(c); pend(d); tws_conn.send_event = 1;
        clear_pkt(2, 20);
        tws_data_sync_rx_bulk_alloc(pkt, 12);
        CHECK(count(&lcon.rx_head) == 2 && nth(&lcon.rx_head, 0) == a && nth(&lcon.rx_head, 1) == b);
        CHECK(count(&tws_conn.rx_pending) == 2 && tws_conn.send_event == 2 && !strcmp(trace, "cT"));
        /* a not-ready first frame stops it; flag 0 skips it entirely */
        reset(); a = new_bulk(1, 10, 0, 0); pend(a); clear_pkt(2, 100);
        tws_data_sync_rx_bulk_alloc(pkt, 12);
        CHECK(count(&lcon.rx_head) == 0);
        reset(); a = new_bulk(1, 10, 1, 0); pend(a); clear_pkt(0, 100);
        tws_data_sync_rx_bulk_alloc(pkt, 12);
        CHECK(count(&lcon.rx_head) == 0 && count(&tws_conn.rx_pending) == 1);
        /* send_event values other than 1 are left alone */
        reset(); a = new_bulk(1, 10, 1, 0); pend(a); tws_conn.send_event = 3; clear_pkt(2, 100);
        tws_data_sync_rx_bulk_alloc(pkt, 12);
        CHECK(tws_conn.send_event == 3 && count(&lcon.rx_head) == 1);
    }

    /* the fragment-length assert is reported but the copy still happens */
    {
        static const u8 payload[2] = { 1, 2 };
        struct bredr_rx_bulk *b = new_bulk(0x5, 0, 0, 0);
        reset(); b->offset = 3; b->len = 9; sync.rx_bulk = b; clear_pkt(0, 0);
        make_item(12, 1, 6, 3, 0x5, 200, 0, payload, 2);     /* 6 + 3 == 9 passes the match test */
        tws_data_sync_rx_bulk_alloc(pkt, 12 + 20 + 1);       /* but only 1 byte arrived: 3 + 1 != 9 */
        CHECK(asserts == 1 && !strcmp(assert_msg, "len + bulk->offset == bulk->len") && assert_value == 0 && b->offset == 4 && b->data[3] == 1);
    }
    return TEST_DONE("btctrler_tws_data_forwarding");
}
#endif
