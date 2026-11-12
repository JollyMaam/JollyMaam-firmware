/* The HCI receive queue: fixed slots, each as large as the largest packet.
 * The writer moves only `write`, the reader only `read`; one slot stays
 * empty to tell full from empty. */
#include <string.h>
#include "hci_queue.h"

#ifdef UNITTEST
#define DEPTH 4
#define SLOT  16
#else
#include "btstack_config.h"
#define DEPTH RX_QUEUE_DEPTH
#define SLOT  HCI_INCOMING_PACKET_BUFFER_SIZE
#endif

#define EVENT 0x04               /* HCI_EVENT_PACKET */

static struct { u8 type; u16 size; u8 data[SLOT]; } q[DEPTH];
static volatile u8 rd, wr;
static struct hci_queue_stats st;

static void count(u16 *n)
{
    if (*n != 0xffff) (*n)++;
}

/* Empty; the counts stay (they are since boot). */
void hciq_clear(void)
{
    rd = wr = 0;
}

u8 *hciq_put(u8 type, const u8 *packet, u16 size)
{
    u8 at = wr, next = (u8)((at + 1) % DEPTH), used;

    if (next == rd || size > SLOT) {
        count(type == EVENT ? &st.dropped_events : &st.dropped_acl);
        return 0;
    }
    q[at].type = type;
    q[at].size = size;
    memcpy(q[at].data, packet, size);
    wr = next;
    used = (u8)((next + DEPTH - rd) % DEPTH);
    if (used > st.most) st.most = used;
    return q[at].data;
}

int hciq_peek(u8 *type, u8 **data, u16 *size)
{
    u8 at = rd;

    if (at == wr) return 0;
    *type = q[at].type;
    *data = q[at].data;
    *size = q[at].size;
    return 1;
}

void hciq_pop(void)
{
    if (rd != wr) rd = (u8)((rd + 1) % DEPTH);
}

u32 hciq_free(void)
{
    return (u32)(DEPTH - 1 - (wr + DEPTH - rd) % DEPTH);
}

void hciq_held(void)
{
    count(&st.held_acl);
}

void hciq_stats(struct hci_queue_stats *out)
{
    *out = st;
    out->slots = DEPTH - 1;      /* one stays empty */
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    u8 p[SLOT + 1], type, *d;
    u16 size;
    u32 i;
    struct hci_queue_stats s;

    for (i = 0; i < sizeof(p); i++) p[i] = (u8)i;
    CHECK(!hciq_peek(&type, &d, &size) && hciq_free() == DEPTH - 1);
    /* DEPTH - 1 fit; the next is dropped and counted by type */
    for (i = 0; i < DEPTH - 1; i++) CHECK(hciq_put(i ? 0x02 : EVENT, p + i, 3) != 0);
    CHECK(hciq_free() == 0);
    CHECK(hciq_put(EVENT, p, 3) == 0 && hciq_put(0x02, p, 3) == 0);
    hciq_stats(&s);
    CHECK(s.dropped_events == 1 && s.dropped_acl == 1 && s.most == DEPTH - 1 && s.slots == DEPTH - 1);
    /* in order, then room again */
    CHECK(hciq_peek(&type, &d, &size) && type == EVENT && size == 3 && d[0] == 0);
    hciq_pop();
    CHECK(hciq_free() == 1);
    CHECK(hciq_put(EVENT, p + 9, 2) != 0);
    for (i = 1; i < DEPTH; i++) {
        CHECK(hciq_peek(&type, &d, &size));
        CHECK(i < DEPTH - 1 ? type == 0x02 && d[0] == i : type == EVENT && d[0] == 9 && size == 2);
        hciq_pop();
    }
    CHECK(!hciq_peek(&type, &d, &size));
    hciq_pop();                  /* empty: no effect */
    CHECK(!hciq_peek(&type, &d, &size));
    /* a full-size packet fits, one byte more is dropped */
    d = hciq_put(0x02, p, SLOT);
    CHECK(d && d[SLOT - 1] == SLOT - 1);
    CHECK(hciq_put(0x02, p, SLOT + 1) == 0);
    hciq_stats(&s);
    CHECK(s.dropped_acl == 2 && s.most == DEPTH - 1);
    hciq_held();
    hciq_stats(&s);
    CHECK(s.held_acl == 1 && s.dropped_events == 1);
    hciq_clear();
    CHECK(!hciq_peek(&type, &d, &size) && hciq_free() == DEPTH - 1);
    return TEST_DONE("hci_queue");
}
#endif
