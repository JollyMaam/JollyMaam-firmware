#include <stddef.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
struct tx_sync_record { u8 seqn; u16 len; int hash; };
typedef u64 sync_word __attribute__((aligned(4), may_alias));

static u8 tx_seqn, rx_index, tx_sync_num, tx_index, tx_num, rx_seqn;
static int slave_last_hash;
static struct tx_sync_record tx_sync_list[8];
#define BULK_FROM_ENTRY(n) ((struct bredr_tx_bulk *)((u8 *)(n) - offsetof(struct bredr_tx_bulk, entry)))

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry; entry->next = head; entry->prev = prev; prev->next = entry;
}

static void __list_del_entry(const struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev; prev->next = next;
}

int TWS_CODE tws_conn_phone_link_tx_add(struct link_connection *conn, struct bredr_tx_bulk *bulk)
{
    int added = 0;
    if (tws_link.link != 0 && tws_conn.monitor_start == 2) {
        if (bulk->llid == 3 && !is_tws_esco_working()) {
            if (tws_conn.role != 0) { link_conn_tx_post_handler(conn, bulk); added = 1; }
        } else {
            (void)is_tws_esco_working();
            int role = tws_conn.role;
            int clock = tws_link_read_slot_clk();
            int offset = role == 0 ? 300 : 500;
            *(u32 *)bulk->payload_head = (u32)bredr_offset2clkn(clock, offset);
            list_add_tail(&bulk->entry, &tws_conn.tx_pending);
            added = 1;
        }
    }
    return added;
}

void TWS_CODE tws_conn_phone_link_tx_suss(struct link_connection *conn, struct bredr_tx_bulk *bulk)
{
    (void)conn;
    if (bulk->llid != 3 && tws_link.link != 0 && tws_conn.monitor_start == 2) {
        u8 seq = ++tx_seqn, index = rx_index;
        tx_sync_list[index].seqn = seq;
        tx_sync_list[index].len = bulk->len;
        u32 hash = JBHash(bulk->data, bulk->len);
        tx_sync_list[rx_index].hash = (int)hash;
        u8 next = rx_index + 1;
        rx_index = next == 8 ? 0 : next;
        ++tx_sync_num;
    }
}

static int TWS_CODE tws_tx_sync_tx_events(struct lmp_connection *lmp, u8 *out, int len)
{
    (void)len;
    u8 index = tx_index, count;
    struct tx_sync_record *record = (void *)out;
    if (tws_conn.role_switch < 3) {
        if (slave_last_hash != 0) {
            struct list_head *node = tws_conn.tx_pending.next;
            while (node != &tws_conn.tx_pending) {
                struct bredr_tx_bulk *bulk = BULK_FROM_ENTRY(node);
                u16 size = bulk->len;
                int hash = (int)JBHash(bulk->data, size);
                if (hash == slave_last_hash) {
                    struct list_head *matched = node;
                    node = tws_conn.tx_pending.next;
                    while (node != &tws_conn.tx_pending) {
                        struct list_head *next = node->next;
                        __list_del_entry(node);
                        list_add_tail(node, &lmp->link->tx_acl_u);
                        link_conn_tx_bulk_avaiable(lmp->link, 0);
                        if (node == matched) break;
                        node = next;
                    }
                    break;
                }
                node = node->next;
            }
            slave_last_hash = 0;
        }
        int clock = tws_link_read_slot_clk();
        struct list_head *node = tws_conn.tx_pending.next;
        while (node != &tws_conn.tx_pending) {
            struct list_head *next = node->next;
            if (bredr_clkn_before(clock, *(u32 *)(void *)BULK_FROM_ENTRY(node)->payload_head)) break;
            else {
                __list_del_entry(node);
                list_add_tail(node, &lmp->link->tx_acl_u);
                link_conn_tx_bulk_avaiable(lmp->link, 0);
            }
            node = next;
        }
    }
    count = tx_sync_num;
    if (count == 0) return 0;
    tx_num = count;
    for (u8 i = 0; i < count; ++i) {
        *(sync_word *)(void *)record = *(const sync_word *)(const void *)&tx_sync_list[index];
        u8 next = index + 1;
        index = next == 8 ? 0 : next;
        ++record;
    }
    return count << 3;
}

static int TWS_CODE tws_tx_sync_rx_events_and_tx_events(struct lmp_connection *lmp, u8 *in, int len, u8 *res)
{
    if (len > 0) {
        struct tx_sync_record *record = (void *)in;
        for (int off = 0; off < len; off += 8, ++record) {
            u8 seq = record->seqn;
            if (rx_seqn < seq || (u32)rx_seqn - (u32)seq >= 8) {
                rx_seqn = seq;
                struct list_head *node = tws_conn.tx_pending.next;
                while (node != &tws_conn.tx_pending) {
                    struct bredr_tx_bulk *bulk = BULK_FROM_ENTRY(node);
                    u16 size = bulk->len;
                    int hash = (int)JBHash(bulk->data, size);
                    size = BULK_FROM_ENTRY(node)->len;
                    if (size == record->len && hash == record->hash) {
                        struct list_head *move = tws_conn.tx_pending.next;
                        while (move != &tws_conn.tx_pending) {
                            struct list_head *next = move->next;
                            struct bredr_tx_bulk *moving = BULK_FROM_ENTRY(move);
                            (void)JBHash(moving->data, moving->len);
                            __list_del_entry(move);
                            link_conn_tx_post_handler(lmp->link, moving);
                            if (move == node) break;
                            move = next;
                        }
                        break;
                    }
                    node = node->next;
                }
            }
        }
    }
    if (res == 0) return 0;
    struct list_head *tail = tws_conn.tx_pending.prev;
    if (tail == &tws_conn.tx_pending) return 0;
    int clock = tws_link_read_slot_clk();
    struct bredr_tx_bulk *last = BULK_FROM_ENTRY(tail);
    if (bredr_clkn_before(clock, *(u32 *)(void *)last->payload_head)) {
        *(u32 *)res = JBHash(last->data, last->len);
        return 4;
    }
    struct list_head *node = tws_conn.tx_pending.next;
    while (node != &tws_conn.tx_pending) {
        struct list_head *next = node->next;
        __list_del_entry(node);
        link_conn_tx_post_handler(lmp->link, BULK_FROM_ENTRY(node));
        node = next;
    }
    link_conn_tx_bulk_avaiable(lmp->link, 0);
    return 0;
}

static void TWS_CODE tws_tx_sync_master_rx_res(struct lmp_connection *lmp, u8 *data, int len)
{ (void)lmp; (void)len; slave_last_hash = *(int *)data; }

static int TWS_CODE key_tx_sync_event_hander(struct lmp_connection *lmp, int event, int *data)
{
    (void)lmp; (void)data;
    if (event == 22) {
        rx_seqn = 0;
        tx_seqn = 0;
        rx_index = 0;
        tx_index = 0;
        tx_num = 0;
        tx_sync_num = 0;
    }
    return 0;
}

void TWS_CODE tws_tx_sync_init(void) { }

static void TWS_CODE tws_tx_sync_tx_events_suss(void)
{
    u8 count = tx_num;
    if (count != 0) {
        u8 next = tx_index + count;
        tx_index = next > 7 ? next - 8 : next;
        tx_sync_num -= count;
        tx_num = 0;
    }
}

const struct tws_sync_channel tws_tx_sync __attribute__((section(".tws_sync_channel.4"), aligned(4))) = {
    4, 1, tws_tx_sync_tx_events, tws_tx_sync_tx_events_suss,
    tws_tx_sync_rx_events_and_tx_events, 0, tws_tx_sync_master_rx_res, key_tx_sync_event_hander
};

#ifdef UNITTEST
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static struct link_connection test_link;
static struct lmp_connection test_lmp;
static int test_hash_calls, test_clock_calls, test_offset, test_post_calls, test_available_calls;
static int test_esco_result, test_before_mode, test_mutate_role, test_mutate_rx_index;
int is_tws_esco_working(void) { return test_esco_result; }
u32 tws_link_read_slot_clk(void)
{
    ++test_clock_calls;
    if (test_mutate_role) tws_conn.role = 1;
    return 10;
}
int bredr_offset2clkn(int clock, int offset) { test_offset = offset; return clock + offset; }
u32 JBHash(u8 *data, int len)
{
    (void)data; ++test_hash_calls;
    if (test_mutate_rx_index) { rx_index = 2; test_mutate_rx_index = 0; }
    return 0x12345678u + (u32)len;
}
void link_conn_tx_post_handler(struct link_connection *lc, struct bredr_tx_bulk *tx)
{ (void)lc; (void)tx; ++test_post_calls; }
void link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx)
{ (void)lc; (void)tx; ++test_available_calls; }
int bredr_clkn_before(int a, int b)
{ return test_before_mode > 0 ? 1 : test_before_mode < 0 ? 0 : a < b; }

int main(void)
{
    struct { struct bredr_tx_bulk bulk; u8 payload[3]; } test_bulk = {0}, second_bulk = {0};
    struct bredr_tx_bulk *bulk = &test_bulk.bulk, *second = &second_bulk.bulk;
    struct { u8 seqn; u16 len; u32 hash; } record = {0};
    tws_link.link = &test_link;
    tws_conn.monitor_start = 2;
    tws_conn.role = 0;
    tws_conn.role_switch = 3;
    tws_conn.tx_pending.next = tws_conn.tx_pending.prev = &tws_conn.tx_pending;
    test_link.tx_acl_u.next = test_link.tx_acl_u.prev = &test_link.tx_acl_u;
    test_lmp.link = &test_link;
    bulk->llid = 1; bulk->len = 3; bulk->data[0] = 1; bulk->data[1] = 2; bulk->data[2] = 3;
    test_mutate_role = 1;
    if (tws_conn_phone_link_tx_add(&test_link, bulk) != 1 || test_offset != 300 ||
        test_clock_calls != 1 || *(u32 *)bulk->payload_head != 310 ||
        tws_conn.tx_pending.next != &bulk->entry) return 1;
    test_mutate_role = 0; tws_conn.role = 0;
    test_mutate_rx_index = 1;
    tws_conn_phone_link_tx_suss(&test_link, bulk);
    if (tws_tx_sync.tx_sync_req(0, (u8 *)&record, sizeof(record)) != 8 ||
        record.seqn != 1 || record.len != 3 || record.hash != 0 ||
        tx_sync_list[2].hash != 0x1234567bu || test_hash_calls != 1) return 2;
    tws_tx_sync.tx_sync_req_suss();
    if (tws_tx_sync.tx_sync_req(0, (u8 *)&record, sizeof(record)) != 0) return 3;
    /* Event 22 resets counters only; subsequent add checks the type-3 post path. */
    tws_tx_sync.event_handler(0, 22, 0);
    bulk->llid = 3; test_esco_result = 0; tws_conn.role = 1;
    if (tws_conn_phone_link_tx_add(&test_link, bulk) != 1 || test_post_calls != 1 ||
        test_clock_calls != 1) return 4;
    /* An available deadline gets moved; a future deadline leaves the queue intact. */
    tws_conn.tx_pending.next = tws_conn.tx_pending.prev = &tws_conn.tx_pending;
    second->llid = 1; second->len = 3; second->payload_head[0] = 20;
    list_add_tail(&second->entry, &tws_conn.tx_pending);
    tws_conn.role_switch = 2; test_before_mode = 1;
    (void)tws_tx_sync.tx_sync_req(&test_lmp, (u8 *)&record, sizeof(record));
    if (tws_conn.tx_pending.next != &second->entry) return 5;
    test_before_mode = -1;
    (void)tws_tx_sync.tx_sync_req(&test_lmp, (u8 *)&record, sizeof(record));
    if (tws_conn.tx_pending.next != &tws_conn.tx_pending ||
        test_link.tx_acl_u.next != &second->entry || test_available_calls == 0) return 6;
    /* Zero input length still requests a hash response when res is non-null. */
    __list_del_entry(&second->entry);
    second->payload_head[0] = 20; list_add_tail(&second->entry, &tws_conn.tx_pending);
    test_before_mode = 1;
    u32 response = 0;
    if (tws_tx_sync.rx_sync_req_and_res(&test_lmp, 0, 0, (u8 *)&response) != 4 ||
        response != 0x1234567bu) return 7;
    if (tws_tx_sync.rx_sync_req_and_res(&test_lmp, 0, 0, 0) != 0) return 8;
    return 0;
}
#endif
