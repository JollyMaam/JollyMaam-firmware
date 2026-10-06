/* TWS key-event buffering and channel callbacks.  Keep two 16-byte buffers,
 * each capped at two four-byte records, sequence-byte arithmetic, the
 * sentinel argument, and the SDK's receive-loop stride of 12 bytes. */
/* sdk.h also declares the unrelated two-argument TWS task handler under this
 * name.  Keep that declaration available without colliding with the SDK
 * event member's private one-argument handler below. */
#define tws_key_event_handler tws_task_key_event_handler_sdk
#include "btctrler/glue.h"
#undef tws_key_event_handler
#include <string.h>

#define TWS_CODE __attribute__((section(".classic_tws_code")))

static u8 event_len[2];
static u8 rx_index;
static u8 tx_index;
static u8 key_events[2][16];
static u8 tx_seqn;
static u8 rx_seqn;

extern BTCTRLER_CONFIG u8 log_tag_const_d_TWS_LMP;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void sys_key_event_consume(struct key_event *event);
extern void sys_event_notify(struct sys_event *event);

static void tws_key_event_handler(struct sys_event *evt)
    TWS_CODE;
static int tws_event_sync_tx_events(struct lmp_connection *conn, u8 *buffer,
                                    int err) TWS_CODE;
static void tws_event_sync_rx_events(struct lmp_connection *conn, u8 *data,
                                     int len) TWS_CODE;
static void tws_event_sync_tx_events_suss(void) TWS_CODE;

static void tws_key_event_handler(struct sys_event *evt)
{
    int is_local = evt->arg != (void *)(unsigned long)1415009024u;
    struct link_connection *link = tws_link.link;
    if (!(is_local & (link != 0)))
        return;

    if (log_tag_const_d_TWS_LMP) {
        u8 index = rx_index;
        log_print(1, 0, "[Debug]: [TWS_LMP]tws_key: %d, %d, %d, %d\n\r\n\r\n",
                  (int)evt->u.key.value, (int)event_len[index], (int)index,
                  (int)tx_index);
    }

    local_irq_disable();
    u8 index = rx_index;
    u8 length = event_len[index];
    if ((u32)length + 12u < 17u) {
        key_events[index][length] = evt->u.key.type;
        key_events[index][length + 1] = (u8)(evt->u.key.event >> 8);
        key_events[index][length + 2] = (u8)evt->u.key.event;
        key_events[index][length + 3] = (u8)evt->u.key.value;
        event_len[index] = (u8)(length + 4);
    } else {
        sys_key_event_consume(&evt->u.key);
    }
    local_irq_enable();
}

void tws_event_key_handler(struct sys_event *evt)
    __attribute__((alias("tws_key_event_handler")));

static int tws_event_sync_tx_events(struct lmp_connection *conn, u8 *buffer,
                                    int err)
{
    (void)conn;
    (void)err;
    local_irq_disable();
    u8 old_index = tx_index;
    u8 index = old_index & 1;
    u8 length = event_len[index];
    if (length != 0) {
        rx_index = index ^ 1;
        tx_index = old_index | 2;
        buffer[0] = tx_seqn;
        memcpy(buffer + 1, key_events[index], length);
    }
    local_irq_enable();

    /* The SDK reloads event_len after re-enabling interrupts. */
    u8 remaining = event_len[index];
    return remaining == 0 ? 0 : (int)remaining + 1;
}

static int tws_event_sync_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_CODE;
static int tws_event_sync_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    if (len != 0)
        tws_event_sync_rx_events(conn, rx, len);
    if (tx == 0)
        return 0;
    return tws_event_sync_tx_events(conn, tx, 0);
}

static void tws_event_sync_rx_events(struct lmp_connection *conn, u8 *data,
                                     int len)
{
    (void)conn;
    struct sys_event evt;
    /* The SDK zeroes exactly 24 bytes for the BR23 sys_event layout. */
    memset(&evt, 0, 24);
    u8 sequence = data[0];
    if (sequence == rx_seqn)
        return;
    rx_seqn = sequence;
    evt.arg = (void *)(unsigned long)1415009024u;
    evt.type = 1;

    u8 *p = data;
    for (int i = 1; i < len; i += 12, p += 4) {
        evt.u.key.init = 0;
        evt.u.key.type = p[1];
        evt.u.key.event = (u16)(((u16)p[2] << 8) | p[3]);
        evt.u.key.value = p[4];
        sys_event_notify(&evt);
    }
}

static void key_event_sync_event_hander(struct lmp_connection *conn,
                                        int event, int *arg) TWS_CODE;
static void key_event_sync_event_hander(struct lmp_connection *conn,
                                        int event, int *arg)
{
    (void)conn;
    (void)arg;
    if (event == 22) {
        rx_seqn = 0;
        tx_seqn = 1;
        rx_index = 0;
        tx_index = 0;
        event_len[0] = 0;
        event_len[1] = 0;
    }
}

static void tws_event_sync_tx_events_suss(void)
{
    local_irq_disable();
    u8 old_index = tx_index;
    u8 index = old_index & 1;
    if ((old_index & 2) != 0) {
        event_len[index] = 0;
        tx_index = index ^ 1;
        tx_seqn++;
    }
    local_irq_enable();
}

const struct static_event_handler __event_handler_tws_key_event_handler
    __attribute__((section(".sys_event.4.handler"), used)) = {
        .event_type = 1,
        .handler = tws_event_key_handler,
    };

const struct tws_sync_channel tws_event_sync
    __attribute__((section(".tws_sync_channel.8"))) = {
        .channel = 8,
        .attribute = 1,
        .tx_sync_req = tws_event_sync_tx_events,
        .tx_sync_req_suss = tws_event_sync_tx_events_suss,
        .rx_sync_req_and_res = tws_event_sync_rx_events_and_tx_events,
        .tx_sync_res_suss = tws_event_sync_tx_events_suss,
        .rx_sync_res = tws_event_sync_rx_events,
        /* The SDK bitcasts its void callback to this int-returning slot. */
        .event_handler = (int (*)(struct lmp_connection *, int, int *))
            key_event_sync_event_hander,
    };

void tws_event_sync_init(void) TWS_CODE;
void tws_event_sync_init(void)
{
}

#ifdef UNITTEST
#include "test.h"
#include <stdint.h>

struct tws_link_connection tws_link;
u8 log_tag_const_d_TWS_LMP;

static int notifications;
static struct sys_event notified;
static int consumed;
static int mutate_after_irq_enable;

void local_irq_disable(void) { }
void local_irq_enable(void)
{
    if (mutate_after_irq_enable) {
        event_len[tx_index & 1] = 0;
        mutate_after_irq_enable = 0;
    }
}
void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag; (void)format;
}
void sys_key_event_consume(struct key_event *event)
{
    (void)event;
    consumed++;
}
void sys_event_notify(struct sys_event *event)
{
    notified = *event;
    notifications++;
}

int main(void)
{
    struct link_connection link = { 0 };
    struct sys_event evt = { 0 };
    u8 tx[17] = { 0 };
    tws_link.link = &link;
    CHECK(tws_event_sync.channel == 8 && tws_event_sync.attribute == 1);
    CHECK(tws_event_sync.tx_sync_req == tws_event_sync_tx_events &&
          tws_event_sync.rx_sync_res == tws_event_sync_rx_events);
    CHECK(__event_handler_tws_key_event_handler.event_type == 1 &&
          __event_handler_tws_key_event_handler.handler == tws_event_key_handler);

    key_event_sync_event_hander(0, 22, 0);
    evt.arg = (void *)(uintptr_t)9;
    evt.u.key.type = 0x31;
    evt.u.key.event = 0x1234;
    evt.u.key.value = 0x56;
    tws_event_key_handler(&evt);
    CHECK(event_len[0] == 4);
    CHECK(memcmp(key_events[0], (u8[]){ 0x31, 0x12, 0x34, 0x56 }, 4) == 0);

    CHECK(tws_event_sync_tx_events(0, tx, 0) == 5);
    CHECK(tx[0] == 1 && memcmp(tx + 1, key_events[0], 4) == 0);
    tws_event_sync_tx_events_suss();
    CHECK(event_len[0] == 0 && tx_seqn == 2 && tx_index == 1);

    /* The post-unlock reload determines the returned length. */
    rx_index = 1;
    evt.u.key.type = 0x42;
    evt.u.key.event = 0xabcd;
    evt.u.key.value = 0x78;
    tws_event_key_handler(&evt);
    mutate_after_irq_enable = 1;
    CHECK(tws_event_sync_tx_events(0, tx, 0) == 0);
    CHECK(tx[0] == 2 && memcmp(tx + 1, (u8[]){ 0x42, 0xab, 0xcd, 0x78 }, 4) == 0);

    u8 incoming[5] = { 1, 0x51, 0x23, 0x77, 0x77 };
    notifications = 0;
    tws_event_sync_rx_events(0, incoming, sizeof(incoming));
    CHECK(notifications == 1 && notified.arg == (void *)(uintptr_t)1415009024u);
    CHECK(notified.type == 1 && notified.u.key.init == 0 &&
          notified.u.key.type == 0x51 && notified.u.key.event == 0x2377);
    tws_event_sync_rx_events(0, incoming, sizeof(incoming));
    CHECK(notifications == 1);
    /* A 9-byte two-record frame only delivers record one: the SDK's loop
     * condition advances its length counter by 12 for each 4-byte record. */
    u8 incoming_two[9] = { 2, 0x61, 1, 2, 3, 0x62, 4, 5, 6 };
    tws_event_sync_rx_events(0, incoming_two, sizeof(incoming_two));
    CHECK(notifications == 2 && notified.u.key.type == 0x61 &&
          notified.u.key.event == 0x0102 && notified.u.key.value == 3);

    event_len[0] = 8;
    rx_index = 0;
    tws_event_key_handler(&evt);
    CHECK(consumed == 1 && event_len[0] == 8);
    key_event_sync_event_hander(0, 22, 0);
    CHECK(rx_seqn == 0 && tx_seqn == 1 && rx_index == 0 && tx_index == 0 &&
          event_len[0] == 0 && event_len[1] == 0);
    return TEST_DONE("btctrler_tws_event");
}
#endif
