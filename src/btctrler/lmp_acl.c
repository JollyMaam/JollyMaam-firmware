/* Host delivery, queue cleanup and buffer policy, recovered from SDK lmp.c.
 * Radio reception and the bulk allocator remain SDK implementations. */
#include <stddef.h>
#include "btctrler/glue.h"

_Static_assert(BTCFG_HCI_STANDARD, "standard HCI host marks copied ACL packets for deletion");
#ifndef UNITTEST
_Static_assert(offsetof(struct bredr_rx_bulk, entry) == 16 &&
               offsetof(struct bredr_rx_bulk, payload_head) == 24 &&
               offsetof(struct bredr_rx_bulk, data) == 28,
               "SDK receive-bulk list and packet offsets");
#endif

static struct bredr_rx_bulk *bulk_for_entry(struct list_head *entry)
{ return (struct bredr_rx_bulk *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry)); }

static void remove_entry(struct list_head *entry)
{ entry->next->prev = entry->prev; entry->prev->next = entry->next; }

void lmp_send_acl_u_packet_to_host(struct lmp_connection *conn)
{
    local_irq_disable();
    struct list_head *head = &conn->rx_acl_u;
    for (struct list_head *entry = head->next, *next; entry != head; entry = next) {
        next = entry->next;
        struct bredr_rx_bulk *bulk = bulk_for_entry(entry);
        if (bulk->delete) {
            remove_entry(entry);
            link_conn_rx_bulk_avaliable(conn->link, bulk);
            continue;
        }
        if (bulk->send2host) continue;
        bulk->send2host = 1;
        local_irq_enable();
        int result = hci_send_acl_data(bulk->payload_head, (u16)(bulk->len + 4));
        local_irq_disable();
        if (result) bulk->send2host = 0; /* Host busy: retry next call. */
        else bulk->delete = 1;          /* Copied: reclaim on the next call. */
        local_irq_enable();
        return; /* At most one new packet per pass. */
    }
    local_irq_enable();
}

int lmp_private_abandon_sbc_data(struct lmp_connection *conn, struct list_head *rx_head, u16 seqn_number)
{
    if (!(config_btctler_modules & 1)) return 0;
    int last_seq = -1, free_continuations = 0;
    for (struct list_head *entry = rx_head->next, *next; entry != rx_head; entry = next) {
        next = entry->next;
        struct bredr_rx_bulk *bulk = bulk_for_entry(entry);
        if (bulk->llid == 2) {
            free_continuations = 0;
            /* Fix: truncated L2CAP/RTP headers must not supply CID/sequence
             * bytes from outside the received payload. */
            if (bulk->sbc_flag == 2 || bulk->delete || bulk->len < 8) continue;
            const u8 *data = bulk->data;
            if (conn->sbc_channel != (data[2] | (data[3] << 8))) continue;
            u16 seq = (data[6] << 8) | data[7];
            if (seqn_number && (s16)(seq - seqn_number) >= 0) return seq;
            last_seq = seq;
            remove_entry(entry);
            bredr_rx_bulk_free(bulk);
            free_continuations = 1;
        } else if (free_continuations && bulk->llid == 1) {
            remove_entry(entry);
            bredr_rx_bulk_free(bulk);
        }
    }
    return last_seq;
}

void lmp_send_aclu_en(struct lmp_connection *conn, u8 enable)
{
    if (!conn->link) return;
    if (enable) conn->link->send_acl_enable |= 1;
    else conn->link->send_acl_enable &= ~1;
}

int lmp_private_get_rx_buffer_total_size(void *priv)
{
    if (!(config_btctler_modules & 1)) return 0;
    struct lmp_connection *conn = priv;
    local_irq_disable();
    int size = conn && conn->link ? link_conn_rx_bulk_total_size(conn->link) : 0;
    local_irq_enable();
    return size;
}

int lmp_private_get_rx_buffer_remain_size(void *priv)
{
    if (!(config_btctler_modules & 1)) return 0;
    struct lmp_connection *conn = priv;
    int size = 0;
    local_irq_disable();
    if (conn && conn->link) {
        size = link_conn_rx_bulk_remain_size(conn->link, conn->sbc_packet_size);
        if (!link_conn_rx_empty(conn->link)) {
            if (++conn->rxbuf_full > 9) {
                conn->rxbuf_full = 0;
                size = 0;
            }
        } else if (size > 2048) conn->rxbuf_full = 0;
    }
    local_irq_enable();
    return size;
}

void lmp_private_set_max_rx_buf_persent(u8 *addr, int persent)
{
    local_irq_disable();
    struct lmp_connection *conn = addr ? lmp_conn_for_address(addr) : lmp_acl_link;
    if (conn && conn->link) link_conn_set_max_rx_bulk_persent(conn->link, persent);
    local_irq_enable();
}

#ifdef UNITTEST
#include <stdlib.h>
#include <string.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
int config_btctler_modules = 3;
static struct link_connection link;
static int irq_depth, released, freed, sent, send_result, remain_size, is_empty;
static int space_calls, found = 1, percentage, packet_size;
static struct bredr_rx_bulk *sent_bulk;
void local_irq_disable(void) { CHECK(!irq_depth); irq_depth = 1; }
void local_irq_enable(void) { CHECK(irq_depth == 1); irq_depth = 0; }
void link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *bulk)
{ CHECK(irq_depth && lc == &link); released++; free(bulk); }
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk) { freed++; free(bulk); }
int hci_send_acl_data(u8 *packet, u16 size)
{
    CHECK(!irq_depth && sent_bulk && packet == sent_bulk->payload_head && size == sent_bulk->len + 4);
    CHECK(sent_bulk->send2host); sent++; return send_result;
}
int link_conn_rx_bulk_total_size(struct link_connection *lc)
{ CHECK(irq_depth && lc == &link); space_calls++; return 6400; }
int link_conn_rx_bulk_remain_size(struct link_connection *lc, int unit)
{ CHECK(irq_depth && lc == &link); space_calls++; packet_size = unit; return remain_size; }
int link_conn_rx_empty(struct link_connection *lc) { CHECK(irq_depth && lc == &link); return is_empty; }
void link_conn_set_max_rx_bulk_persent(struct link_connection *lc, int percent)
{ CHECK(irq_depth && lc == &link); percentage = percent; space_calls++; }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(irq_depth && addr); return found ? lmp_acl_link : 0; }
static void init_list(struct list_head *head) { head->next = head->prev = head; }
static struct bredr_rx_bulk *append(struct list_head *head, int llid, int channel, int seq, int len)
{
    struct bredr_rx_bulk *bulk = calloc(1, sizeof(*bulk) + len);
    CHECK(bulk != 0); bulk->llid = llid; bulk->len = len;
    if (len >= 8) {
        bulk->data[2] = channel; bulk->data[3] = channel >> 8;
        bulk->data[6] = seq >> 8; bulk->data[7] = seq;
    }
    bulk->entry.prev = head->prev; bulk->entry.next = head;
    head->prev->next = &bulk->entry; head->prev = &bulk->entry;
    return bulk;
}
static void clear_list(struct list_head *head)
{
    while (head->next != head) {
        struct list_head *entry = head->next; remove_entry(entry); free(bulk_for_entry(entry));
    }
}
int main(void)
{
    struct lmp_connection *c = lmp_acl_link;
    c->link = &link; init_list(&c->rx_acl_u);
    lmp_send_acl_u_packet_to_host(c); CHECK(!sent && !released && !irq_depth);
    struct bredr_rx_bulk *old = append(&c->rx_acl_u, 2, 0, 0, 8); old->delete = 1;
    struct bredr_rx_bulk *pending = append(&c->rx_acl_u, 1, 0, 0, 8); pending->send2host = 1;
    sent_bulk = append(&c->rx_acl_u, 2, 0, 0, 8); send_result = 1;
    lmp_send_acl_u_packet_to_host(c);
    CHECK(released == 1 && sent == 1 && !sent_bulk->send2host && !sent_bulk->delete && !irq_depth);
    send_result = 0; lmp_send_acl_u_packet_to_host(c);
    CHECK(sent == 2 && sent_bulk->send2host && sent_bulk->delete && released == 1);
    lmp_send_acl_u_packet_to_host(c); CHECK(sent == 2 && released == 2 && c->rx_acl_u.next == &pending->entry);
    pending->delete = 1; lmp_send_acl_u_packet_to_host(c); CHECK(released == 3 && c->rx_acl_u.next == &c->rx_acl_u);
    sent_bulk = append(&c->rx_acl_u, 2, 0, 0, 8);
    struct bredr_rx_bulk *second = append(&c->rx_acl_u, 2, 0, 0, 8);
    lmp_send_acl_u_packet_to_host(c); CHECK(sent_bulk->delete && !second->send2host);
    sent_bulk = second; lmp_send_acl_u_packet_to_host(c); CHECK(released == 4 && second->delete);
    lmp_send_acl_u_packet_to_host(c); CHECK(released == 5 && !irq_depth);
    c->sbc_channel = 0x1234;
    CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 0) == -1);
    for (int mask = 0; mask < 8; mask++) {
        struct bredr_rx_bulk *bulk = append(&c->rx_acl_u, 2, mask & 1 ? 0x1235 : 0x1234, 20, 8);
        bulk->delete = !!(mask & 2); bulk->sbc_flag = mask & 4 ? 2 : 0;
        append(&c->rx_acl_u, 1, 0, 0, 4);
        int before = freed;
        CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 0) == (mask ? -1 : 20));
        CHECK(freed == before + (mask ? 0 : 2)); clear_list(&c->rx_acl_u);
    }
    /* A new unrelated header prevents its continuations being discarded. */
    append(&c->rx_acl_u, 2, 0x1234, 20, 8); append(&c->rx_acl_u, 1, 0, 0, 4);
    struct bredr_rx_bulk *keep = append(&c->rx_acl_u, 2, 0x1235, 21, 8); append(&c->rx_acl_u, 1, 0, 0, 4);
    CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 0) == 20 && c->rx_acl_u.next == &keep->entry);
    clear_list(&c->rx_acl_u);
    /* Wrapped RTP sequence ordering and early stop at the requested sequence. */
    append(&c->rx_acl_u, 2, 0x1234, 0xfffe, 8); append(&c->rx_acl_u, 1, 0, 0, 4);
    keep = append(&c->rx_acl_u, 2, 0x1234, 1, 8); append(&c->rx_acl_u, 1, 0, 0, 4);
    CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 1) == 1 && c->rx_acl_u.next == &keep->entry);
    clear_list(&c->rx_acl_u);
    for (int len = 0; len < 8; len++) {
        keep = append(&c->rx_acl_u, 2, 0x1234, 20, len);
        CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 0) == -1 && c->rx_acl_u.next == &keep->entry);
        clear_list(&c->rx_acl_u);
    }
    keep = append(&c->rx_acl_u, 2, 0x1234, 20, 8); config_btctler_modules = 2;
    CHECK(lmp_private_abandon_sbc_data(c, &c->rx_acl_u, 0) == 0 && c->rx_acl_u.next == &keep->entry);
    clear_list(&c->rx_acl_u);
    for (int modules = 0; modules < 4; modules++) {
        config_btctler_modules = modules; space_calls = 0;
        CHECK(lmp_private_get_rx_buffer_total_size(c) == (modules & 1 ? 6400 : 0));
        CHECK(space_calls == !!(modules & 1) && !irq_depth);
        space_calls = 0; c->sbc_packet_size = 512; remain_size = 3072; is_empty = 1;
        CHECK(lmp_private_get_rx_buffer_remain_size(c) == (modules & 1 ? 3072 : 0));
        CHECK(space_calls == !!(modules & 1) && !irq_depth);
    }
    CHECK(packet_size == 512); config_btctler_modules = 3;
    space_calls = 0;
    CHECK(!lmp_private_get_rx_buffer_total_size(0) && !lmp_private_get_rx_buffer_remain_size(0));
    c->link = 0; CHECK(!lmp_private_get_rx_buffer_total_size(c) && !lmp_private_get_rx_buffer_remain_size(c));
    CHECK(!space_calls && !irq_depth); c->link = &link;
    is_empty = 0; c->rxbuf_full = 0;
    for (int i = 1; i <= 10; i++) CHECK(lmp_private_get_rx_buffer_remain_size(c) == (i == 10 ? 0 : 3072));
    CHECK(c->rxbuf_full == 0 && !irq_depth);
    c->rxbuf_full = 7; is_empty = 1; remain_size = 2048;
    CHECK(lmp_private_get_rx_buffer_remain_size(c) == 2048 && c->rxbuf_full == 7);
    remain_size = 2049; CHECK(lmp_private_get_rx_buffer_remain_size(c) == 2049 && !c->rxbuf_full);
    for (int enable = 0; enable < 256; enable++) {
        link.send_acl_enable = 0xa4; lmp_send_aclu_en(c, enable);
        CHECK(link.send_acl_enable == (enable ? 0xa5 : 0xa4));
    }
    c->link = 0; lmp_send_aclu_en(c, 1); c->link = &link;
    u8 addr[6] = {0}; space_calls = 0;
    lmp_private_set_max_rx_buf_persent(0, 60); CHECK(space_calls == 1 && percentage == 60);
    lmp_private_set_max_rx_buf_persent(addr, 45); CHECK(space_calls == 2 && percentage == 45);
    found = 0; lmp_private_set_max_rx_buf_persent(addr, 90); CHECK(space_calls == 2 && !irq_depth);
    return TEST_DONE("btctrler_lmp_acl");
}
#endif
