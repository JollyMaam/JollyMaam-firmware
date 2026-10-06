/* Host ACL transmit and host-held packet ownership, from SDK lmp_rxtx.c.
 * A host ACL packet (4-byte HCI header, then payload) maps onto a bulk's
 * payload_head; the bulk allocator and radio queue remain SDK APIs. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

#ifndef UNITTEST
_Static_assert(offsetof(struct bredr_tx_bulk, payload_head) == 24 &&
               offsetof(struct bredr_rx_bulk, payload_head) == 24,
               "SDK bulk header offsets");
#endif

static u16 acl_handle(const u8 *packet) { return packet[0] | (packet[1] & 0x0f) << 8; }
/* Packet boundary 01 (continuing fragment) is LLID 1; anything else starts
 * an L2CAP message (LLID 2). */
static u8 acl_llid(const u8 *packet) { return (packet[1] & 0xf0) == 0x10 ? 1 : 2; }

static struct bredr_tx_bulk *tx_for_packet(u8 *packet)
{ return (struct bredr_tx_bulk *)(packet - offsetof(struct bredr_tx_bulk, payload_head)); }
static struct bredr_rx_bulk *rx_for_packet(u8 *packet)
{ return (struct bredr_rx_bulk *)(packet - offsetof(struct bredr_rx_bulk, payload_head)); }

/* The standard HCI path (h4_hci_push_data): copy the host's packet into a
 * new bulk.  The acknowledgement raises LMP_EVENT_ACL_TX_POST, which sends
 * the host Number Of Completed Packets. */
int lmp_hci_send_packet_standard(const u8 *packet, int len)
{
    struct lmp_connection *conn = lmp_conn_for_handle(acl_handle(packet));
    if (!conn) return 0;
    int size = len - 4;
    /* Off interrupts the allocator waits for space, so only an IRQ-context
     * caller sees NULL; the SDK then copied through it. */
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(size);
    if (!tx) { btctrler_assert_failed("ACL transmit allocation", size); return 0; }
    memcpy(tx->data, packet + 4, size);
    tx->offset = 0;
    tx->len = size;
    tx->event_post = LMP_EVENT_ACL_TX_POST;
    tx->llid = acl_llid(packet);
    link_conn_tx_bulk_avaiable(conn->link, tx);
    return 0;
}

/* The SDK's private zero-copy host API: the host fills a bulk it got from
 * lmp_private_get_tx_packet_buffer and hands it back to lmp_hci_send_packet. */
u32 lmp_private_get_tx_remain_buffer(void) { return get_bredr_tx_remain_size(); }

u8 *lmp_private_get_tx_packet_buffer(int size)
{
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(size - 4);
    if (!tx) return 0;
    tx->len = size - 4;
    return tx->payload_head;
}

int lmp_hci_send_packet(u8 *packet, int len)
{
    struct lmp_connection *conn = lmp_conn_for_handle(acl_handle(packet));
    struct bredr_tx_bulk *tx = tx_for_packet(packet);
    if (!conn) {
        bredr_tx_bulk_free(tx);
        return 0;
    }
    int size = len - 4;
    if (size > (s16)tx->len) btctrler_assert_failed("len - 4 <= tx->len", len);
    if (sniff_support_reset_anchor_point) lmp_sniff_pre_anchor_point_reset(conn);
    bredr_tx_bulk_realloc(tx, size);
    tx->offset = 0;
    tx->len = size;
    tx->event_post = LMP_EVENT_ACL_TX_POST;
    tx->llid = acl_llid(packet);
    link_conn_tx_bulk_avaiable(conn->link, tx);
    return 0;
}

/* Received packets the private host holds (by their payload_head, as given
 * to hci_send_acl_data): hold keeps a bulk, free marks it for deletion,
 * and try-free deletes it unless held. */
void lmp_hci_private_try_free_acl_packet(u8 *packet)
{
    struct bredr_rx_bulk *rx = rx_for_packet(packet);
    local_irq_disable();
    if (!rx->hold) rx->delete = 1;
    local_irq_enable();
}

void lmp_hci_private_hold_acl_packet(u8 *packet)
{
    local_irq_disable();
    rx_for_packet(packet)->hold = 1;
    local_irq_enable();
}

void lmp_hci_private_free_acl_packet(u8 *packet)
{
    local_irq_disable();
    rx_for_packet(packet)->delete = 1;
    local_irq_enable();
}

#ifdef UNITTEST
#include <stdlib.h>
#include "test.h"
int config_btctler_modules = 3;
const int sniff_support_reset_anchor_point = 1;
static struct lmp_connection conn;
static struct link_connection link;
static int found = 1, irq_depth, asserts, queued, freed, reallocated, anchors, fail_alloc;
static struct bredr_tx_bulk *last;
void local_irq_disable(void) { CHECK(!irq_depth); irq_depth = 1; }
void local_irq_enable(void) { CHECK(irq_depth == 1); irq_depth = 0; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ CHECK(handle == 0x234); return found ? &conn : 0; }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    if (fail_alloc) return 0;
    last = calloc(1, sizeof(*last) + (size > 0 ? size : 0)); CHECK(last);
    return last;
}
void bredr_tx_bulk_free(struct bredr_tx_bulk *tx) { CHECK(tx == last); freed++; free(tx); last = 0; }
void bredr_tx_bulk_realloc(struct bredr_tx_bulk *tx, int size) { CHECK(tx == last); reallocated = size; }
u32 get_bredr_tx_remain_size(void) { return 777; }
void lmp_sniff_pre_anchor_point_reset(struct lmp_connection *c) { CHECK(c == &conn); anchors++; }
void link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx)
{ CHECK(lc == &link && tx == last); queued++; }
static void reset(void) { asserts = queued = freed = reallocated = anchors = fail_alloc = 0; found = 1; free(last); last = 0; }
int main(void)
{
    conn.link = &link;
    /* Handle 0x234, start of an L2CAP message, 5 payload bytes. */
    u8 packet[9] = { 0x34, 0x22, 5, 0, 1, 2, 3, 4, 5 };
    reset(); CHECK(lmp_hci_send_packet_standard(packet, 9) == 0);
    CHECK(queued == 1 && last->len == 5 && last->llid == 2 && !last->offset);
    CHECK(last->event_post == LMP_EVENT_ACL_TX_POST && !memcmp(last->data, packet + 4, 5));
    packet[1] = 0x12;                       /* continuing fragment */
    reset(); lmp_hci_send_packet_standard(packet, 9); CHECK(last->llid == 1);
    packet[1] = 0x32;                       /* boundary 11 is not a continuation */
    reset(); lmp_hci_send_packet_standard(packet, 9); CHECK(last->llid == 2);
    reset(); found = 0; lmp_hci_send_packet_standard(packet, 9); CHECK(!queued && !last);
    reset(); fail_alloc = 1; lmp_hci_send_packet_standard(packet, 9); CHECK(asserts == 1 && !queued);

    reset(); CHECK(lmp_private_get_tx_remain_buffer() == 777);
    u8 *buffer = lmp_private_get_tx_packet_buffer(4 + 20);
    CHECK(buffer == last->payload_head && last->len == 20);
    memcpy(buffer, (u8[]){ 0x34, 0x22, 6, 0 }, 4);
    CHECK(lmp_hci_send_packet(buffer, 4 + 6) == 0);
    CHECK(queued == 1 && reallocated == 6 && last->len == 6 && last->llid == 2 && anchors == 1 && !asserts);
    CHECK(last->event_post == LMP_EVENT_ACL_TX_POST);
    CHECK(lmp_hci_send_packet(buffer, 4 + 21) == 0 && asserts == 1);
    found = 0; lmp_hci_send_packet(buffer, 10); CHECK(freed == 1 && !last);
    reset(); fail_alloc = 1; CHECK(!lmp_private_get_tx_packet_buffer(10));

    struct bredr_rx_bulk rx = {0};
    lmp_hci_private_try_free_acl_packet(rx.payload_head); CHECK(rx.delete && !rx.hold);
    rx.delete = 0; lmp_hci_private_hold_acl_packet(rx.payload_head); CHECK(rx.hold && !rx.delete);
    lmp_hci_private_try_free_acl_packet(rx.payload_head); CHECK(!rx.delete);
    lmp_hci_private_free_acl_packet(rx.payload_head); CHECK(rx.delete && rx.hold && rx.llid == 0);
    reset();
    return TEST_DONE("btctrler_lmp_acl_tx");
}
#endif
