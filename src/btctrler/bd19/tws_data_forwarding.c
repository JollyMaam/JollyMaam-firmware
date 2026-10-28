/* BD19 SDK member tws_data_forwarding.c.o, occurrence 1. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_d_TWS;
extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS;
extern BTCTRLER_CONFIG u8 log_tag_const_v_TWS;
extern BTCTRLER_CONFIG int CONFIG_PHONE_RUN_SLOT;
extern void log_print(int level, const char *tag, const char *format, ...);

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

static struct bredr_rx_bulk *bulk_from_entry(struct list_head *entry)
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
        int count = data[4];
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
            return data[32]+(data[33]<<8)+36;
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
    reply[0] = 0;
    reply[1] = (u8)link_conn_rx_empty(conn->link);
    ids = (struct acl_data_id *)(reply + 2);
    if (sync.abandon_num_s != 0)
        __slave_abandon_bulk_check();
    for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = node->next) {
        struct bredr_rx_bulk *bulk = bulk_from_entry(node);
        u8 seqnum = bulk->seqnum;
        if (last_seqnum != 0 && (s8)(seqnum - last_seqnum) < 0)
            continue;
        bulk->ready = 1;
        ids[count].hash = (u16)bulk->hash;
        ids[count].seqnum = seqnum;
        count++;
        reply[0]++;
    }
    tws_conn_channel_open(1);
    if (log_tag_const_v_TWS)
        log_print(0, NULL, "[Verb]: [TWS]step2: from_seqnum = %d, %d\n\r\n",
                  last_seqnum, reply[0]);
    return ((u32)reply[0] << 2) | 2;
}

