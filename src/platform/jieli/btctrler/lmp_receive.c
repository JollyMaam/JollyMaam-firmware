/* Classic receive dispatch and L2CAP reassembly, recovered from SDK lmp.c.
 * The retained radio layer allocates an LLID-2 bulk for the complete L2CAP
 * length (and rejects totals above 2047), before queuing it here. */
#include <string.h>
#include "btctrler/glue.h"

_Static_assert(!BTCFG_TWS, "receive dispatch excludes TWS phone/media hooks");

static unsigned le16(const u8 *p) { return p[0] | (p[1] << 8); }

void lmp_rx_handler(struct lmp_connection *conn)
{
    int cmd = 0;
    if (!conn || !conn->link) return;
    conn->busy = 1;
    struct bredr_rx_bulk *bulk;
    while ((bulk = bredr_rx_bulk_pop(&conn->link->rx_head))) {
        if (conn->timer) sys_timer_modify(conn->timer, 10000);
        if (bulk->llid == 3) {
            lmp_role_machine(conn, bulk, &cmd);
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            continue;
        }
        if (lmp_dut_en && !lmp_dut_phone_en) {
            /* The SDK stopped after popping this bulk without releasing it. */
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            break;
        }
        if (g_lmp_update_start_flag) {
            if (lmp_update_rx_handler) lmp_update_rx_handler(conn->core_s.handle, bulk);
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            continue;
        }
        if (bulk->llid == 2) {
            if (conn->acl_s_bulk) {
                link_conn_rx_bulk_avaliable(conn->link, conn->acl_s_bulk);
                conn->acl_s_bulk = 0;
            }
            /* Fix: the SDK read the length and CID even from short headers. */
            if (bulk->len < 4 || le16(bulk->data) + 4 > 2047) {
                link_conn_rx_bulk_avaliable(conn->link, bulk);
                continue;
            }
            if (bulk->len < le16(bulk->data) + 4) {
                conn->acl_s_bulk = bulk;
                continue;
            }
        } else {
            struct bredr_rx_bulk *first = conn->acl_s_bulk;
            if (!first) {
                link_conn_rx_bulk_avaliable(conn->link, bulk);
                continue;
            }
            unsigned total = le16(first->data) + 4;
            if ((unsigned)first->len + bulk->len > total) {
                link_conn_rx_bulk_avaliable(conn->link, first);
                conn->acl_s_bulk = 0;
                link_conn_rx_bulk_avaliable(conn->link, bulk);
                continue;
            }
            memcpy(first->data + first->len, bulk->data, bulk->len);
            first->len += bulk->len;
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            if (first->len < total) continue;
            conn->acl_s_bulk = 0;
            bulk = first;
        }
        bulk->payload_head[0] = conn->core_s.handle;
        bulk->payload_head[1] = (conn->core_s.handle >> 8) | (bulk->llid == 1 ? 0x10 : 0x20);
        bulk->payload_head[2] = bulk->len;
        bulk->payload_head[3] = bulk->len >> 8;
        unsigned channel = le16(bulk->data + 2);
        if (acl_u_packet_analyse(bulk->data, bulk->len) < 0) {
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            continue;
        }
        local_irq_disable();
        if (channel == conn->sbc_channel) {
            if (conn->clear_a2dp_packet_num) {
                /* Fix: sequence bytes exist only in packets of at least 8 bytes. */
                if (bulk->len < 8 || (s16)(((bulk->data[6] << 8) | bulk->data[7]) -
                                         conn->clear_a2dp_packet_num) < 0) {
                    local_irq_enable();
                    link_conn_rx_bulk_avaliable(conn->link, bulk);
                    continue;
                }
                conn->clear_a2dp_packet_num = 0;
            }
            if (conn->sbc_open) {
                bulk->hold = bulk->send2host = 1;
                bulk->sbc_flag = 1;
                a2dp_rx_notice_to_decode();
                os_sem_post(&conn->sbc_sem);
            }
        }
        struct list_head *head = &conn->rx_acl_u, *entry = &bulk->entry;
        entry->prev = head->prev; entry->next = head;
        head->prev->next = entry; head->prev = entry;
        local_irq_enable();
        lmp_send_acl_u_packet_to_host(conn);
    }
    conn->busy = 0;
}

#ifdef UNITTEST
#include <stddef.h>
#include <stdlib.h>
#include "test.h"
u8 lmp_dut_en, lmp_dut_phone_en, g_lmp_update_start_flag;
void (*lmp_update_rx_handler)(int, struct bredr_rx_bulk *);
static struct lmp_connection conn;
static struct link_connection link;
static int irq, released, roles, updates, analysed, delivered, notices, sems, timers, reject;
static u8 expected[64];
static unsigned expected_len;
static struct bredr_rx_bulk *delivered_bulk;
static void list_init(struct list_head *h) { h->next = h->prev = h; }
static struct bredr_rx_bulk *from_entry(struct list_head *e)
{ return (void *)((u8 *)e - offsetof(struct bredr_rx_bulk, entry)); }
static void append(struct list_head *h, struct bredr_rx_bulk *b)
{
    b->entry.next = h; b->entry.prev = h->prev;
    h->prev->next = &b->entry; h->prev = &b->entry;
}
struct bredr_rx_bulk *bredr_rx_bulk_pop(struct list_head *h)
{
    CHECK(!irq && conn.busy);
    if (h->next == h) return 0;
    struct list_head *e = h->next;
    h->next = e->next; e->next->prev = h;
    return from_entry(e);
}
void link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *b)
{ CHECK(!irq && lc == &link && b); released++; free(b); }
void lmp_role_machine(struct lmp_connection *c, struct bredr_rx_bulk *b, int *cmd)
{ CHECK(c == &conn && c->busy && b->llid == 3 && !irq && !*cmd); roles++; }
int sys_timer_modify(u16 id, u32 msec)
{ CHECK(id == 17 && msec == 10000 && conn.busy && !irq); timers++; return 0; }
int acl_u_packet_analyse(u8 *data, u16 len)
{
    CHECK(!irq && conn.busy && len == expected_len && !memcmp(data, expected, len));
    analysed++; return reject ? -1 : 0;
}
void local_irq_disable(void) { CHECK(!irq); irq = 1; }
void local_irq_enable(void) { CHECK(irq); irq = 0; }
void a2dp_rx_notice_to_decode(void) { CHECK(irq && conn.busy); notices++; }
int os_sem_post(OS_SEM *sem) { CHECK(irq && sem == &conn.sbc_sem); sems++; return 0; }
void lmp_send_acl_u_packet_to_host(struct lmp_connection *c)
{
    CHECK(!irq && c == &conn && c->busy);
    delivered_bulk = from_entry(c->rx_acl_u.prev); delivered++;
    CHECK(delivered_bulk->payload_head[0] == 0x34 && delivered_bulk->payload_head[1] == 0x22);
    CHECK(le16(delivered_bulk->payload_head + 2) == expected_len);
}
static void update(int handle, struct bredr_rx_bulk *b)
{ CHECK(handle == 0x234 && b && conn.busy && !irq); updates++; }
static struct bredr_rx_bulk *queue(int llid, const u8 *data, unsigned len, unsigned capacity)
{
    CHECK(capacity >= len);
    struct bredr_rx_bulk *b = calloc(1, sizeof(*b) + capacity);
    CHECK(b); b->llid = llid; b->len = len;
    if (len) memcpy(b->data, data, len);
    append(&link.rx_head, b); return b;
}
static void reset(void)
{
    while (link.rx_head.next && link.rx_head.next != &link.rx_head) {
        struct list_head *e = link.rx_head.next;
        link.rx_head.next = e->next; free(from_entry(e));
    }
    while (conn.rx_acl_u.next && conn.rx_acl_u.next != &conn.rx_acl_u) {
        struct list_head *e = conn.rx_acl_u.next;
        conn.rx_acl_u.next = e->next; free(from_entry(e));
    }
    if (conn.acl_s_bulk) free(conn.acl_s_bulk);
    memset(&conn, 0, sizeof(conn)); memset(&link, 0, sizeof(link));
    conn.link = &link; conn.core_s.handle = 0x234;
    list_init(&link.rx_head); list_init(&conn.rx_acl_u);
    irq = released = roles = updates = analysed = delivered = notices = sems = timers = reject = 0;
    lmp_dut_en = lmp_dut_phone_en = g_lmp_update_start_flag = 0;
    lmp_update_rx_handler = 0; delivered_bulk = 0;
    expected_len = 32;
    for (unsigned i = 0; i < expected_len; i++) expected[i] = i;
    expected[0] = expected_len - 4; expected[1] = 0; expected[2] = 0x41; expected[3] = 0;
}
int main(void)
{
    reset(); lmp_rx_handler(0); conn.link = 0; lmp_rx_handler(&conn);
    CHECK(!conn.busy); conn.link = &link; lmp_rx_handler(&conn); CHECK(!conn.busy);
    queue(3, expected, 1, 1); conn.timer = 17; lmp_rx_handler(&conn);
    CHECK(roles == 1 && timers == 1 && released == 1 && !conn.busy);
    /* Every two-fragment split, with exactly the radio's reserved capacity. */
    for (unsigned split = 4; split < 32; split++) {
        reset(); struct bredr_rx_bulk *first = queue(2, expected, split, 32);
        lmp_rx_handler(&conn); CHECK(conn.acl_s_bulk == first && !analysed && !conn.busy);
        queue(1, expected + split, 32 - split, 32 - split); lmp_rx_handler(&conn);
        CHECK(!conn.acl_s_bulk && analysed == 1 && delivered == 1 && released == 1 && !conn.busy);
        CHECK(delivered_bulk == first);
    }
    reset(); queue(2, expected, 4, 32);
    for (unsigned i = 4; i < 32; i++) queue(1, expected + i, 1, 1);
    lmp_rx_handler(&conn); CHECK(analysed == 1 && released == 28 && delivered == 1);
    /* A new start discards the incomplete packet; orphan/oversized tails are returned. */
    reset(); queue(2, expected, 4, 32); queue(2, expected, 32, 32);
    lmp_rx_handler(&conn); CHECK(released == 1 && delivered == 1 && !conn.acl_s_bulk);
    reset(); queue(1, expected, 4, 4); lmp_rx_handler(&conn); CHECK(released == 1 && !analysed);
    reset(); queue(2, expected, 4, 32); queue(1, expected, 29, 29);
    lmp_rx_handler(&conn); CHECK(released == 2 && !conn.acl_s_bulk && !analysed);
    for (unsigned len = 0; len < 4; len++) {
        reset(); queue(2, expected, len, len); lmp_rx_handler(&conn);
        CHECK(released == 1 && !conn.acl_s_bulk && !analysed);
    }
    reset(); expected[0] = 0xff; expected[1] = 0xff;
    queue(2, expected, 4, 4); lmp_rx_handler(&conn); CHECK(released == 1 && !conn.acl_s_bulk);
    reset(); reject = 1; queue(2, expected, 32, 32); lmp_rx_handler(&conn);
    CHECK(analysed == 1 && released == 1 && !delivered);
    for (int mask = 0; mask < 4; mask++) {
        reset(); lmp_dut_en = mask & 1; lmp_dut_phone_en = mask & 2;
        queue(2, expected, 32, 32); queue(3, expected, 1, 1); lmp_rx_handler(&conn);
        CHECK(!conn.busy && !irq);
        CHECK(delivered == (mask == 1 ? 0 : 1) && roles == (mask == 1 ? 0 : 1));
        CHECK(released == 1);
    }
    for (int handler = 0; handler < 2; handler++) {
        reset(); g_lmp_update_start_flag = 1; lmp_update_rx_handler = handler ? update : 0;
        queue(2, expected, 1, 1); lmp_rx_handler(&conn);
        CHECK(updates == handler && released == 1 && !analysed && !conn.busy);
    }
    for (int open = 0; open < 2; open++) {
        reset(); conn.sbc_channel = 0x41; conn.sbc_open = open;
        queue(2, expected, 32, 32); lmp_rx_handler(&conn);
        CHECK(delivered == 1 && notices == open && sems == open);
        CHECK(delivered_bulk->hold == open && delivered_bulk->send2host == open &&
              delivered_bulk->sbc_flag == open && !irq);
    }
    /* RTP wrap ordering; a short SBC header cannot supply a sequence. */
    for (int seq = 0; seq < 3; seq++) {
        reset(); conn.sbc_channel = 0x41; conn.clear_a2dp_packet_num = 0xfffe;
        expected[6] = seq ? 0 : 0xff; expected[7] = seq ? seq - 1 : 0xfd;
        queue(2, expected, 32, 32); lmp_rx_handler(&conn);
        CHECK(delivered == !!seq && released == !seq && !irq);
        CHECK(conn.clear_a2dp_packet_num == (seq ? 0 : 0xfffe));
    }
    for (unsigned len = 4; len < 8; len++) {
        reset(); expected_len = len; expected[0] = len - 4;
        conn.sbc_channel = 0x41; conn.clear_a2dp_packet_num = 1;
        queue(2, expected, len, len); lmp_rx_handler(&conn);
        CHECK(released == 1 && !delivered && !irq && conn.clear_a2dp_packet_num == 1);
    }
    reset(); return TEST_DONE("btctrler_lmp_receive");
}
#endif
