/* BD19 SDK member tws_data_forwarding.c.o, occurrence 1. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_d_TWS;
extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS;
extern BTCTRLER_CONFIG u8 log_tag_const_v_TWS;
extern BTCTRLER_CONFIG int CONFIG_PHONE_RUN_SLOT;
extern void log_print(int level, const char *tag, const char *format, ...);

extern int printf(const char *, ...);
extern int puts(const char *);

typedef u8 forward_aligned_byte __attribute__((aligned(4), may_alias));

#define FWD_CODE __attribute__((section(".tws_data_forward_code")))
#define NODE_BULK(node) bulk_from_entry(node)

static struct tws_data_sync sync;

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

static int list_empty(void)
{
    return tws_conn.rx_pending.next == &tws_conn.rx_pending;
}

static inline __attribute__((always_inline)) struct bredr_rx_bulk *bulk_from_entry(struct list_head *entry)
{
    return (struct bredr_rx_bulk *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry));
}

static void cpu_reset(void)
{
    p33_soft_reset();
}

static void __slave_abandon_bulk_check(void) FWD_CODE;
static void __slave_abandon_bulk_check(void)
{
    struct list_head *node = tws_conn.rx_pending.next;

    while (node != &tws_conn.rx_pending) {
        struct list_head *next = node->next;
        struct bredr_rx_bulk *bulk = bulk_from_entry(node);

        if (!bulk->ready) {
            int matched = 0;
            u16 hash = sync.abandon_hash_s[0];
            for (;;) {
                if ((u32)bulk->hash == (u32)hash) {
                    __list_del_entry(node);
                    bredr_rx_bulk_free(bulk);
                    matched = 1;
                }
                sync.abandon_num_s--;
                if (sync.abandon_num_s == 0)
                    break;
                sync.abandon_hash_s[0] = sync.abandon_hash_s[1];
                hash = sync.abandon_hash_s[0];
                if (matched)
                    break;
            }
        }
        node = next;
    }
}

static int tws_data_sync_get_data(struct lmp_connection *, u8 *, int) FWD_CODE;
static int tws_data_sync_tx_data_list(struct lmp_connection *, u8 *, int, u8 *) FWD_CODE;
static void tws_data_sync_rx_data_list(struct lmp_connection *, u8 *, int) FWD_CODE;
int tws_data_sync_event_handler(struct lmp_connection *, int, int *) FWD_CODE;
static void tws_master_tx_bulk_suss(struct bredr_tx_bulk *) FWD_CODE;

const struct tws_sync_channel tws_acl_data_sync
    __attribute__((section(".tws_sync_channel.6"), aligned(4))) = {
        .channel = 6,
        .attribute = 0,
        .tx_sync_req = tws_data_sync_get_data,
        .tx_sync_req_suss = 0,
        .rx_sync_req_and_res = tws_data_sync_tx_data_list,
        .tx_sync_res_suss = 0,
        .rx_sync_res = tws_data_sync_rx_data_list,
        .event_handler = tws_data_sync_event_handler,
    };

static void FWD_CODE tws_master_tx_bulk_suss(struct bredr_tx_bulk *bulk)
{
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    unsigned end = bulk->len > 8 ? bulk->data[13] : 0;
    if (lmp == NULL) return;
    tws_conn_send_event_check();
    local_irq_disable();
    struct list_head *node = tws_conn.rx_pending.next;
    int verbose = log_tag_const_v_TWS != 0;
    while (node != &tws_conn.rx_pending) {
        struct list_head *next = node->next;
        struct bredr_rx_bulk *rx = NODE_BULK(node);
        if (!rx->ready || (rx->lost && !rx->send)) break;
        if (end && (s8)(end - rx->seqnum) < 0) break;
        if (verbose) log_print(0, NULL, "[Verb]: [TWS]add_tail: %d, %d\n\r\n",
                              rx->seqnum, (rx->data[6] << 8) | rx->data[7]);
        __list_del_entry(node);
        list_add_tail(node, &lmp->link->rx_head);
        if (tws_conn.send_event == 1) tws_conn.send_event = 2;
        node = next;
    }
    local_irq_enable();
    tws_conn_try_send_event_to_lmp();
}

int FWD_CODE tws_forward_rx_handler(struct bredr_rx_bulk *outer, u8 *data)
{
    struct tws_forward_head *frame = (void *)data;
    if (frame->head.opcode == 1) {
        int count = ((struct item_abandon *)(void *)data)->num;
        u16 *hashes = (void *)(data + 6);
        struct lmp_connection *lmp = tws_conn.lmp_conn;
        struct list_head *node = tws_conn.rx_pending.next;
        int verbose = log_tag_const_v_TWS != 0;
        for (;;) {
            struct list_head *next = node->next;
            if (node == &tws_conn.rx_pending) return 0;
            struct bredr_rx_bulk *rx = NODE_BULK(node);
            if (count == 255) {
                if (lmp->sbc_channel != (rx->data[2] | (rx->data[3] << 8))) {
                    node = next; continue;
                }
            } else {
                if (!rx->ready) return 0;
                if ((u32)rx->hash != *hashes) { node = next; continue; }
                hashes++; count--;
                if (verbose) log_print(0, NULL, "[Verb]: [TWS]--drop_bulk: %x, %x\n\r\n",
                                      rx->hash, rx->seqnum);
            }
            __list_del_entry(node);
            bredr_rx_bulk_free(rx);
            node = next;
        }
    }
    if (frame->head.opcode == 2 && outer == NULL) return -14;
    if (frame->head.opcode != 2 && frame->head.opcode != 3) return 0;
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    u16 total = frame->head.len;
    tws_conn_send_event_check();
    int verbose = log_tag_const_v_TWS != 0;
    if (verbose) log_print(0, NULL, "[Verb]: [TWS]step5: %d, %d, %d\n\r\n",
                          frame->end_flag, frame->end_seqnum, total + 4);
    if (frame->end_flag) tws_conn_channel_close(1);
    u16 previous = frame->prev_hash;
    if (previous) {
        if (verbose) log_print(0, NULL, "[Verb]: [TWS]acl_prev_hash: %x\n\r\n", previous);
        struct list_head *first = tws_conn.rx_pending.next;
        struct list_head *node = first;
        previous = frame->prev_hash;
        while (node != &tws_conn.rx_pending) {
            struct bredr_rx_bulk *rx = NODE_BULK(node);
            if ((u32)rx->hash == previous) {
                struct list_head *move = first;
                for (;;) {
                    struct list_head *next = move->next;
                    if (move == &tws_conn.rx_pending) break;
                    struct bredr_rx_bulk *moving = NODE_BULK(move);
                    __list_del_entry(move);
                    list_add_tail(move, &lmp->link->rx_head);
                    if (tws_conn.send_event == 1) tws_conn.send_event = 2;
                    if (verbose) log_print(0, NULL, "[Verb]: [TWS]add_tail_0: %x, %d\n\r\n",
                                          moving->hash, (moving->data[6] << 8) | moving->data[7]);
                    if (move == node) break;
                    move = next;
                }
                break;
            }
            node = node->next;
        }
        frame->prev_hash = 0;
    }
    if (outer != NULL) {
        struct bredr_rx_bulk *embedded = (void *)frame->data;
        embedded->id = outer->id;
        embedded->head_offset = 32;
        list_add_tail(&embedded->entry, &lmp->link->rx_head);
        if (tws_conn.send_event == 1) tws_conn.send_event = 2;
        if (verbose) log_print(0, NULL, "[Verb]: [TWS]add_tail_1: %x, %d\n\r\n",
                              embedded->hash, (embedded->data[6] << 8) | embedded->data[7]);
        tws_conn_history_hash_add(embedded->hash);
        if (list_empty() || !NODE_BULK(tws_conn.rx_pending.next)->ready) {
            if (sync.abandon_num_s == 2) {
                sync.abandon_num_s = 1;
                sync.abandon_hash_s[0] = sync.abandon_hash_s[1];
            }
            u8 index = sync.abandon_num_s;
            u16 hash = embedded->hash;
            sync.abandon_num_s = index + 1;
            sync.abandon_hash_s[index] = hash;
        }
    }
    if (frame->end_flag) {
        struct list_head *node = tws_conn.rx_pending.next;
        while (node != &tws_conn.rx_pending) {
            struct list_head *next = node->next;
            struct bredr_rx_bulk *rx = NODE_BULK(node);
            if (!rx->ready || !frame->end_seqnum || (s8)(frame->end_seqnum - rx->seqnum) < 0) {
                if (verbose) log_print(0, NULL, "[Verb]: [TWS]not ready: %d, %x, %d\n\r\n",
                                      rx->ready, rx->hash, rx->seqnum);
                break;
            }
            __list_del_entry(node);
            list_add_tail(node, &lmp->link->rx_head);
            if (verbose) log_print(0, NULL, "[Verb]: [TWS]add_tail_2: %x, %d\n\r\n",
                                  rx->hash, (rx->data[6] << 8) | rx->data[7]);
            if (tws_conn.send_event == 1) tws_conn.send_event = 2;
            node = next;
        }
    }
    tws_conn_try_send_event_to_lmp();
    return 0;
}

int FWD_CODE tws_data_sync_event_handler(struct lmp_connection *conn, int event, int *arg)
{
    (void)conn;
    struct lmp_connection *lmp;
    int delta, value;
    switch (event) {
    case 22:
        __builtin_memset(&sync, 0, sizeof(sync));
        sync.channel_open=1; sync.forward_suss=1; break;
    case 31: sync.abandon_num=0; break;
    case 32: tws_master_tx_bulk_suss((void *)arg); break;
    case 34:
        sync.forward_suss=1; tws_master_tx_bulk_suss((void *)arg);
        tws_conn_channel_close(1); break;
    case 20:
        lmp=tws_conn.lmp_conn;
        if (!lmp || tws_conn.role==1) break;
        if (!lmp->sbc_open || tws_conn.pure_monitor) {
            sync.single_bad_step=0; sync.single_bad_cnt=0; break;
        }
        if (sync.abandon_num_s) break;
        value=tws_link.link->super_timeout_begin;
        delta=bredr_clkn2offset(value, READ_SLOT_CLK());
        if (delta>199 && sync.single_bad_step==0) { sync.single_bad_step=1; goto step_one; }
        if (sync.single_bad_step==1) {
step_one:
            sync.master_last_rx_seqnum=0;
            tws_core_a2dp_packet_go(1);
            if (delta<CONFIG_PHONE_RUN_SLOT) {
                value=lmp_private_get_sbc_remain_time(lmp,0);
                int low=lmp_get_a2dp_data_cache_low(lmp);
                if (value>=low) { sync.single_bad_step=2; sync.single_bad_cnt=5; }
                else if (log_tag_const_d_TWS) log_print(1,NULL,"[Debug]: [TWS]tws_single_bad_1: msec = %d\n\r\n",value);
            } else if(log_tag_const_d_TWS) log_print(1,NULL,"[Debug]: [TWS]tws_single_bad_1: timeout = %d\n\r\n",delta);
        } else if(sync.single_bad_step==2) {
            if(lmp->clear_a2dp_packet_num) break;
            if(delta>CONFIG_PHONE_RUN_SLOT) { sync.single_bad_step=1; goto step_one; }
            value=lmp_get_sbc_remain_time_form_list(lmp,&tws_conn.rx_pending);
            if(log_tag_const_d_TWS) log_print(1,NULL,"[Debug]: [TWS]tws_single_bad_2: %d\n\r\n",value);
            if(value>99) { sync.single_bad_step=1; goto step_one; }
            if(--sync.single_bad_cnt==0) sync.single_bad_step=0;
        }
        break;
    case 21:
        lmp=tws_conn.lmp_conn;
        if(!lmp || tws_conn.role==1) break;
        if(!lmp->sbc_open || tws_conn.pure_monitor) { sync.single_bad_step=0; sync.single_bad_cnt=0; break; }
        if(sync.single_bad_step==0) {
            value=lmp_get_sbc_remain_time_form_list(lmp,&tws_conn.rx_pending);
            if(value>99) {
                sync.single_bad_step=1;
                if(log_tag_const_d_TWS) log_print(1,NULL,"[Debug]: [TWS]tws_single_bad: %d\n\r\n",value);
            }
        }
        break;
    case 25: return tws_forward_rx_handler(NULL,(void *)arg);
    case 26: return tws_forward_rx_handler((void *)arg,((struct bredr_rx_bulk *)(void *)arg)->data);
    case 27: {
        u8 *data=(void *)arg;
        if(data[3]==2 && ((*(u16 *)(void *)(data+10))&7)==2)
            return *(forward_aligned_byte *)(void *)(arg+8)+(data[33]<<8)+36;
        return *(u16 *)(void *)data+4;
    }
    }
    return 0;
}

static int tws_data_sync_get_data(struct lmp_connection *conn, u8 *out, int error)
{
    (void)conn;
    (void)error;
    if (sync.abandon_num_s != 0)
        __slave_abandon_bulk_check();
    if (sync.channel_open == 0) {
        sync.single_bad_cnt = 0;
        sync.single_bad_step = LMP_TX_NULL;
        return 0;
    }
    out[0] = sync.single_bad_step;
    if (sync.forward_suss == 0) {
        if (log_tag_const_i_TWS)
            log_print(2, NULL, "[Info]: [TWS]forward_suss = 0\n\r\n");
        return 1;
    }
    if (sync.single_bad_step == LMP_TX_NULL)
        tws_conn_channel_open(1);
    out[1] = sync.master_last_rx_seqnum;
    return 2;
}

static int tws_data_sync_tx_data_list(struct lmp_connection *conn, u8 *req,
                                      int len, u8 *reply)
{
    int count = 0;
    struct opcode_data_list_res *response = (void *)reply;
    struct acl_data_id *ids;
    u8 last_seqnum;
    struct list_head *node;

    sync.single_bad_step = LMP_TX_NULL;
    if (reply == NULL)
        return 0;
    if (req[0] == 1) {
        tws_core_a2dp_packet_go(0);
        return 0;
    }
    if (len == 1)
        return 0;

    last_seqnum = req[1];
    sync.master_last_rx_seqnum = 0;
    response->data_num = 0;
    response->rx_empty = (u8)link_conn_rx_empty(conn->link);
    ids = response->datas;
    if (sync.abandon_num_s != 0)
        __slave_abandon_bulk_check();
    node = tws_conn.rx_pending.next;
    for (;;) {
        struct list_head *next = node->next;
        if (node == &tws_conn.rx_pending) break;
        struct bredr_rx_bulk *bulk = bulk_from_entry(node);
        u8 seqnum = bulk->seqnum;
        if (last_seqnum != 0 && (s8)(seqnum - last_seqnum) < 0) { node=next; continue; }
        bulk->ready = 1;
        ids[count].hash = (u16)bulk->hash;
        ids[count].seqnum = seqnum;
        count++;
        response->data_num++;
        node = next;
    }
    tws_conn_channel_open(1);
    if (log_tag_const_v_TWS)
        log_print(0, NULL, "[Verb]: [TWS]step2: from_seqnum = %d, %d\n\r\n",
                  last_seqnum, response->data_num);
    return ((u32)response->data_num << 2) | 2;
}


/* The SDK updates the packed RX flags as a 16-bit word, sometimes using a
 * snapshot taken before callback-visible state changes. */
typedef u16 forward_flags_word __attribute__((may_alias));
#define RX_FLAGS(bulk) (*(forward_flags_word *)((u8 *)(bulk) + 2))

static void tws_data_sync_rx_data_list(struct lmp_connection *conn, u8 *data, int len)
{
    (void)conn; (void)len;
    struct opcode_data_list_res *remote = (void *)data;
    struct acl_data_id *ids = remote->datas;
    int original = remote->data_num, left = original, visited = 0;
    sync.abandon_num = 0;
    int verbose = log_tag_const_v_TWS != 0;
    if (verbose) log_print(0, NULL, "[Verb]: [TWS]step2: %x\n\r\n", sync.master_last_rx_seqnum);
    tws_conn_send_event_check();
    struct list_head *node = tws_conn.rx_pending.next;
    for (;;) {
        struct list_head *next = node->next;
        if (node == &tws_conn.rx_pending) break;
        struct bredr_rx_bulk *bulk = NODE_BULK(node);
        u16 flags = RX_FLAGS(bulk);
        if (flags & 512) { node=next; continue; }
        RX_FLAGS(bulk) = flags | 512;
        visited++;
        if (left == 0) {
            if (data[1]) { RX_FLAGS(bulk)=flags|1536; node=next; continue; }
            RX_FLAGS(bulk)=flags & (u16)~512;
            if (verbose) log_print(0,NULL,"[Verb]: [TWS]cmp-break\n\r\n");
            goto package_abandon;
        }
        u32 hash = bulk->hash;
        if (hash == ids->hash) {
            sync.master_last_rx_seqnum=ids->seqnum;
            ids++; left--;
            RX_FLAGS(bulk) &= (u16)~1024;
            node=next; continue;
        }
        int j;
        for (j=1; j<left; j++) if (hash==ids[j].hash) break;
        if (j < left) {
            sync.master_last_rx_seqnum=ids[j].seqnum;
            for (int i=0;i<j;i++) {
                u8 index=sync.abandon_num;
                if (index>11) goto collect_remaining;
                u16 item=ids[i].hash;
                sync.abandon_num=index+1;
                sync.abandon_hash[index]=item;
            }
            ids+=j+1; left-=j+1;
        } else {
            RX_FLAGS(bulk)=flags|1536;
        }
        node=next;
    }
collect_remaining:
    while (left>2 && sync.abandon_num<=11) {
        u8 index=sync.abandon_num;
        u16 item=ids->hash;
        sync.abandon_num=index+1;
        sync.abandon_hash[index]=item;
        ids++; left--;
    }
package_abandon:
    if (sync.abandon_num) {
        int size=2*(int)sync.abandon_num+6;
        struct bredr_tx_bulk *abandon=tws_tx_bulk_alloc(size);
        if (abandon==NULL) {
            if (config_asser) {
                printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_data_forwarding.c",502);
                puts("ASSERT-FAILD: 0 tws_forward_no_buf");
                cpu_assert_debug();
            } else cpu_reset();
            goto notify;
        }
        abandon->len=(u16)size;
        abandon->event_post=31;
        abandon->data[2]=6;
        abandon->data[3]=1;
        *(u16 *)(void *)abandon->data=(u16)(size-4);
        u8 bound=sync.abandon_num;
        abandon->data[4]=bound;
        if (bound) {
            u16 *out=(void *)(abandon->data+6);
            int debug=log_tag_const_d_TWS!=0;
            for (int i=0;i<bound;i++) {
                u16 item=sync.abandon_hash[i];
                out[i]=item;
                if (debug) {
                    log_print(1,NULL,"[Debug]: [TWS]abandon: %x\n\r\n",item);
                    bound=sync.abandon_num;
                }
            }
        }
        list_add_tail(&abandon->entry,&tws_link.link->tx_acl_u);
    }
    sync.forward_suss=0;
    node=tws_conn.rx_pending.next;
    int generated=0;
    u32 previous=0;
    struct bredr_tx_bulk *tx=NULL;
    struct tws_forward_head *frame=NULL;
    while (node!=&tws_conn.rx_pending) {
        struct list_head *next=node->next;
        struct bredr_rx_bulk *bulk=NODE_BULK(node);
        u16 flags=RX_FLAGS(bulk);
        if (!(flags&512)) break;
        if (flags&2048) { previous=0; node=next; continue; }
        if (!(flags&1024)) {
            u32 hash=bulk->hash;
            if (hash) previous=hash;
            node=next; continue;
        }
        if (generated) list_add_tail(&tx->entry,&tws_link.link->tx_acl_u);
        u16 rxlen=bulk->len;
        u8 offset=bulk->head_offset;
        tx=(void *)((u8 *)bulk-offset);
        __builtin_memset(tx,0,sizeof(*tx));
        tx->llid=2;
        tx->len=(u16)(rxlen+32);
        tx->event_post=32;
        tx->rx_head_offset=bulk->head_offset;
        frame=(void *)tx->data;
        frame->head.channel=6;
        frame->head.opcode=2;
        frame->head.len=(u16)(rxlen+28);
        frame->prev_hash=(u16)previous;
        frame->end_flag=0;
        generated=1;
        previous=0;
        RX_FLAGS(bulk)|=2048;
        bredr_rx_bulk_inc_ref(bulk);
        node=next;
    }
    if (!generated) {
        tx=tws_tx_bulk_alloc(8);
        if (!tx) goto log_result;
        tx->len=8;
        frame=(void *)tx->data;
        frame->head.channel=6;
        frame->head.opcode=3;
        frame->head.len=4;
        frame->prev_hash=0;
    }
    if (tx) {
        frame->end_flag=1;
        frame->end_seqnum=sync.master_last_rx_seqnum;
        tx->event_post=34;
        list_add_tail(&tx->entry,&tws_link.link->tx_acl_u);
    }
log_result:
    if (verbose) log_print(0,NULL,"[Verb]: [TWS]rx: %d, %d\n\r\n",visited,original);
notify:
    tws_conn_try_send_event_to_lmp();
}

void FWD_CODE tws_data_sync_clear_abandon_num(void) { sync.abandon_num_s = 0; }
void FWD_CODE tws_mastr_sync_sbc_data_to_remote_clk(struct bredr_tx_bulk *bulk) { (void)bulk; }
void FWD_CODE tws_mastr_sync_sbc_data_to_remote_timeout_check(struct bredr_tx_bulk *bulk) { (void)bulk; }
void FWD_CODE tws_data_sync_init(void) { }
void FWD_CODE tws_data_sync_channel_open(void)
{ sync.channel_open = 1; sync.master_last_rx_seqnum = 0; }
void FWD_CODE tws_data_sync_channel_close(void) { sync.channel_open = 0; }
int FWD_CODE tws_data_sync_channel_state(void) { return sync.channel_open; }


#ifdef UNITTEST
#include "test.h"
u8 log_tag_const_d_TWS, log_tag_const_i_TWS, log_tag_const_v_TWS;
int CONFIG_PHONE_RUN_SLOT=300, config_asser;
struct tws_connection tws_conn;
struct tws_link_connection tws_link;
static struct lmp_connection fixture_lmp;
static struct link_connection fixture_link, fixture_peer;
struct forward_rx_fixture { u8 prefix[64]; struct bredr_rx_bulk bulk; u8 tail[64]; };
struct forward_tx_fixture { struct bredr_tx_bulk bulk; u8 tail[128]; };
static struct forward_rx_fixture rx_objects[3];
static struct forward_tx_fixture tx_objects[4];
static int allocations, fail_allocation, frees, refs, notifications, checks, opens, closes;
static int resets, assertions, irq, go_value, delta, remain, cache_low, list_remain;
static int mutate_reply, mutate_hash, mutate_abandon, mutate_previous;
static struct tws_forward_head *previous_to_mutate;
static u8 *reply_to_mutate;
static struct bredr_rx_bulk *hash_to_mutate;
void p33_soft_reset(void) { resets++; }
void cpu_assert_debug(void) { assertions++; }
void local_irq_disable(void) { CHECK(irq==0); irq=1; }
void local_irq_enable(void) { CHECK(irq==1); irq=0; }
void tws_conn_send_event_check(void) { checks++; }
void tws_conn_try_send_event_to_lmp(void) { CHECK(irq==0); notifications++; }
void tws_conn_channel_open(u8 channel)
{ CHECK(channel==1); opens++; sync.master_last_rx_seqnum=0x72; }
void tws_conn_channel_close(u8 channel) { CHECK(channel==1); closes++; }
void tws_conn_history_hash_add(int hash)
{ (void)hash; if(mutate_hash) hash_to_mutate->hash=0x1234; }
void tws_core_a2dp_packet_go(int value) { go_value=value; }
u32 READ_SLOT_CLK(void) { return 123; }
int bredr_clkn2offset(int a,int b) { (void)a; (void)b; return delta; }
int lmp_private_get_sbc_remain_time(void *p,u8 include)
{ CHECK(p==&fixture_lmp && include==0); return remain; }
int lmp_get_a2dp_data_cache_low(struct lmp_connection *p)
{ CHECK(p==&fixture_lmp); return cache_low; }
int lmp_get_sbc_remain_time_form_list(struct lmp_connection *p,struct list_head *head)
{ CHECK(p==&fixture_lmp && head==&tws_conn.rx_pending); return list_remain; }
int link_conn_rx_empty(struct link_connection *p) { CHECK(p==&fixture_link); return 0x102; }
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk) { CHECK(bulk!=NULL); frees++; }
void bredr_rx_bulk_inc_ref(struct bredr_rx_bulk *bulk) { CHECK(bulk->send); refs++; }
struct bredr_tx_bulk *tws_tx_bulk_alloc(int size)
{
    CHECK(size>=0 && size<=128); allocations++;
    if(allocations==fail_allocation) return NULL;
    CHECK(allocations<=4);
    return &tx_objects[allocations-1].bulk;
}
void log_print(int level,const char *tag,const char *format,...)
{
    (void)level; (void)tag;
    if(mutate_reply && strstr(format,"from_seqnum")) reply_to_mutate[0]=5;
    if(mutate_abandon && strstr(format,"[TWS]abandon:")) sync.abandon_num=0;
    if(mutate_previous && strstr(format,"acl_prev_hash")) previous_to_mutate->prev_hash=0x2222;
}
static void forward_setup(void)
{
    memset(&sync,0,sizeof(sync)); memset(&tws_conn,0,sizeof(tws_conn));
    memset(&tws_link,0,sizeof(tws_link)); memset(&fixture_lmp,0,sizeof(fixture_lmp));
    memset(&fixture_link,0,sizeof(fixture_link)); memset(&fixture_peer,0,sizeof(fixture_peer));
    memset(rx_objects,0,sizeof(rx_objects)); memset(tx_objects,0,sizeof(tx_objects));
    tws_conn.lmp_conn=&fixture_lmp; fixture_lmp.link=&fixture_link; tws_link.link=&fixture_peer;
    tws_conn.rx_pending.next=tws_conn.rx_pending.prev=&tws_conn.rx_pending;
    fixture_link.rx_head.next=fixture_link.rx_head.prev=&fixture_link.rx_head;
    fixture_peer.tx_acl_u.next=fixture_peer.tx_acl_u.prev=&fixture_peer.tx_acl_u;
    allocations=fail_allocation=frees=refs=notifications=checks=opens=closes=0;
    resets=assertions=irq=go_value=mutate_reply=mutate_hash=mutate_abandon=mutate_previous=0;
    log_tag_const_d_TWS=log_tag_const_i_TWS=log_tag_const_v_TWS=0;
    config_asser=0; delta=remain=cache_low=list_remain=0;
}
static struct bredr_rx_bulk *forward_add(unsigned index,u32 hash,u8 seq)
{
    struct bredr_rx_bulk *bulk=&rx_objects[index].bulk;
    bulk->hash=hash; bulk->seqnum=seq; bulk->head_offset=64;
    list_add_tail(&bulk->entry,&tws_conn.rx_pending);
    return bulk;
}
int main(void)
{
    _Alignas(8) u8 packet[128]={0}, response[128];
    struct opcode_data_list_res *remote=(void *)packet;
    forward_setup(); sync.single_bad_cnt=4; sync.single_bad_step=2;
    CHECK(tws_acl_data_sync.tx_sync_req(NULL,NULL,0)==0 && sync.single_bad_cnt==0 && sync.single_bad_step==0);
    tws_data_sync_channel_open(); sync.forward_suss=1;
    CHECK(tws_acl_data_sync.tx_sync_req(NULL,response,0)==2 && response[0]==0 && response[1]==0x72 && opens==1);
    sync.forward_suss=0; response[1]=0xa5;
    CHECK(tws_acl_data_sync.tx_sync_req(NULL,response,0)==1 && response[1]==0xa5);
    tws_data_sync_channel_close(); CHECK(tws_data_sync_channel_state()==0);
    sync.channel_open=0x80; CHECK(tws_data_sync_channel_state()==128);
    sync.abandon_num_s=2; tws_data_sync_clear_abandon_num(); CHECK(sync.abandon_num_s==0);
    tws_data_sync_init(); tws_mastr_sync_sbc_data_to_remote_clk(NULL);
    tws_mastr_sync_sbc_data_to_remote_timeout_check(NULL); CHECK(sync.channel_open==0x80);

    forward_setup(); struct bredr_rx_bulk *first=forward_add(0,0x10001234,7);
    sync.abandon_num_s=1; sync.abandon_hash_s[0]=0x1234;
    __slave_abandon_bulk_check(); CHECK(frees==0 && sync.abandon_num_s==0);
    first->hash=0x1234; sync.abandon_num_s=1;
    __slave_abandon_bulk_check(); CHECK(frees==1 && list_empty());

    forward_setup(); first=forward_add(0,0x12345678,7);
    memset(response,0xa5,sizeof(response)); packet[0]=packet[1]=0;
    CHECK(tws_acl_data_sync.rx_sync_req_and_res(&fixture_lmp,packet,2,response)==6);
    struct opcode_data_list_res *out=(void *)response;
    CHECK(out->data_num==1 && out->rx_empty==2 && out->datas[0].hash==0x5678 && out->datas[0].seqnum==7);
    CHECK(response[5]==0xa5 && first->ready);
    log_tag_const_v_TWS=1; mutate_reply=1; reply_to_mutate=response;
    CHECK(tws_acl_data_sync.rx_sync_req_and_res(&fixture_lmp,packet,0,response)==22);
    sync.single_bad_step=2;
    CHECK(tws_acl_data_sync.rx_sync_req_and_res(NULL,NULL,0,NULL)==0 && sync.single_bad_step==0);

    forward_setup(); packet[0]=packet[1]=0;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,0);
    CHECK(allocations==1 && tx_objects[0].bulk.len==8 && tx_objects[0].bulk.event_post==34);
    struct tws_forward_head *frame=(void *)tx_objects[0].bulk.data;
    CHECK(frame->head.channel==6 && frame->head.opcode==3 && frame->head.len==4 && frame->end_flag==1);
    CHECK(sync.forward_suss==0 && notifications==1);
    forward_setup(); remote->data_num=3; remote->rx_empty=0;
    remote->datas[0].hash=0x1111; remote->datas[1].hash=0x2222; remote->datas[1].seqnum=9; remote->datas[2].hash=0x3333;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,14);
    CHECK(allocations==2 && sync.abandon_num==1 && sync.abandon_hash[0]==0x1111);
    CHECK(tx_objects[0].bulk.event_post==31 && tx_objects[0].bulk.len==8);
    CHECK(*(u16 *)(void *)(tx_objects[0].bulk.data+6)==0x1111);
    forward_setup(); first=forward_add(0,0x2222,3);
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,14);
    CHECK(first->ready && !first->lost && sync.master_last_rx_seqnum==9 && sync.abandon_num==1);
    CHECK(allocations==2 && refs==0);
    forward_setup(); first=forward_add(0,1,3); remote->data_num=0; remote->rx_empty=0;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,2);
    CHECK(!first->ready && allocations==1);

    /* Reuse reserved prefix space without overlapping the host RX object. */
    forward_setup(); first=forward_add(0,0x1234,1); first->ready=1;
    struct bredr_rx_bulk *second=forward_add(1,0x5678,2); second->ready=second->lost=1; second->len=5;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,2);
    struct bredr_tx_bulk *reused=(void *)rx_objects[1].prefix;
    frame=(void *)reused->data;
    CHECK(allocations==0 && refs==1 && second->send && reused->llid==2 && reused->len==37 && reused->rx_head_offset==64);
    CHECK(reused->event_post==34 && frame->head.len==33 && frame->head.opcode==2 && frame->prev_hash==0x1234 && frame->end_flag==1);

    forward_setup(); remote->data_num=3; fail_allocation=1; sync.forward_suss=1;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,14);
    CHECK(resets==1 && assertions==0 && notifications==1 && sync.forward_suss==1 && allocations==1);
    forward_setup(); fail_allocation=1; config_asser=1;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,14);
    CHECK(resets==0 && assertions==1 && notifications==1);

    forward_setup(); first=forward_add(0,0x1111,1); first->ready=1;
    second=forward_add(1,0x2222,2); second->ready=1;
    memset(packet,0,sizeof(packet)); packet[3]=1; packet[4]=1;
    *(u16 *)(void *)(packet+6)=0x1111; *(u16 *)(void *)(packet+8)=0x2222;
    CHECK(tws_forward_rx_handler(NULL,packet)==0 && frees==2); /* count continues below zero */
    packet[3]=2; CHECK(tws_forward_rx_handler(NULL,packet)==-14);
    forward_setup(); first=forward_add(0,0x2222,2); memset(packet,0,sizeof(packet));
    frame=(void *)packet; frame->head.opcode=3; frame->prev_hash=0x2222;
    CHECK(tws_forward_rx_handler(NULL,packet)==0 && frame->prev_hash==0 && list_empty());
    CHECK(fixture_link.rx_head.next==&first->entry && notifications==1);
    forward_setup(); memset(packet,0,sizeof(packet)); frame=(void *)packet; frame->head.opcode=3;
    struct bredr_rx_bulk *embedded=(void *)frame->data; embedded->hash=1;
    struct bredr_rx_bulk outer={0}; outer.id=1; mutate_hash=1; hash_to_mutate=embedded;
    CHECK(tws_forward_rx_handler(&outer,packet)==0 && embedded->id && embedded->head_offset==32);
    CHECK(sync.abandon_num_s==1 && sync.abandon_hash_s[0]==0x1234);

    forward_setup(); first=forward_add(0,1,5); first->ready=1;
    u8 *ack_data=(u8 *)&tx_objects[0]+offsetof(struct bredr_tx_bulk,data);
    tx_objects[0].bulk.len=9; ack_data[13]=4;
    tws_master_tx_bulk_suss(&tx_objects[0].bulk); CHECK(!list_empty());
    ack_data[13]=5;
    tws_master_tx_bulk_suss(&tx_objects[0].bulk); CHECK(list_empty() && notifications==2);
    memset(&sync,0xa5,sizeof(sync)); tws_data_sync_event_handler(NULL,22,NULL);
    CHECK(sync.channel_open==1 && sync.forward_suss==1 && sync.rx_bulk==NULL && sync.abandon_num==0);
    fixture_lmp.sbc_open=1; delta=200; remain=20; cache_low=20;
    tws_data_sync_event_handler(NULL,20,NULL); CHECK(sync.single_bad_step==2 && sync.single_bad_cnt==5 && go_value==1);
    for(unsigned i=0;i<5;i++) tws_data_sync_event_handler(NULL,20,NULL);
    CHECK(sync.single_bad_step==0 && sync.single_bad_cnt==0);
    list_remain=99; tws_data_sync_event_handler(NULL,21,NULL); CHECK(sync.single_bad_step==0);
    list_remain=100; tws_data_sync_event_handler(NULL,21,NULL); CHECK(sync.single_bad_step==1);
    memset(packet,0,sizeof(packet)); *(u16 *)(void *)packet=7;
    CHECK(tws_data_sync_event_handler(NULL,27,(int *)(void *)packet)==11);
    packet[3]=2; *(u16 *)(void *)(packet+10)=2; packet[32]=0x34; packet[33]=0x12;
    CHECK(tws_data_sync_event_handler(NULL,27,(int *)(void *)packet)==0x1234+36);
    forward_setup(); memset(packet,0,sizeof(packet));
    remote->data_num=4; remote->datas[0].hash=0x1111; remote->datas[1].hash=0x2222;
    log_tag_const_d_TWS=1; mutate_abandon=1;
    tws_acl_data_sync.rx_sync_res(&fixture_lmp,packet,18);
    CHECK(allocations==2 && tx_objects[0].bulk.len==10 && ack_data[4]==2);
    CHECK(sync.abandon_num==0 && *(u16 *)(void *)(tx_objects[0].bulk.data+6)==0x1111);
    CHECK(*(u16 *)(void *)(tx_objects[0].bulk.data+8)==0);
    forward_setup(); first=forward_add(0,0x1111,1); second=forward_add(1,0x2222,2);
    memset(packet,0,sizeof(packet)); frame=(void *)packet;
    frame->head.opcode=3; frame->prev_hash=0x1111;
    log_tag_const_v_TWS=1; mutate_previous=1; previous_to_mutate=frame;
    tws_forward_rx_handler(NULL,packet);
    CHECK(list_empty() && fixture_link.rx_head.next==&first->entry && fixture_link.rx_head.prev==&second->entry);
        return TEST_DONE("btctrler_bd19_tws_data_forwarding");
}
#endif
