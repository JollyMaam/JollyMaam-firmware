/* BD19 TWS key-event buffering and channel callbacks, recovered from
 * tws_event.c.o occurrence 1.  The SDK uses two 64-byte queues and stores
 * three bytes per key record; its timeout walks the queue with unusual
 * reload and stride behavior that is preserved here. */
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_IRQ_CODE __attribute__((section(".tws_irq_code")))

static bool event_lock __attribute__((aligned(4)));
static u8 event_len[2];
static u8 rx_index;
static u8 tx_index;
static u8 key_events[2][64];
static u16 hi_timer;
static u8 tx_seqn;
static u8 rx_seqn;

extern void log_print(int level, const char *tag, const char *format, ...);
extern void sys_event_notify(struct sys_event *event);
extern u16 usr_timeout_add(void *priv, void (*callback)(void *), int msec,
                           u8 repeat);
extern int usr_timeout_modify(u16 id, int msec);
extern void usr_timeout_del(u16 id);
extern int tws_in_sniff_state(void);

static void tws_key_event_timeout(void *priv) TWS_CODE;
static int tws_event_sync_tx_events(struct lmp_connection *conn, u8 *buffer,
                                    int err) TWS_IRQ_CODE;
static int tws_event_sync_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_CODE;
static void tws_event_sync_rx_events(struct lmp_connection *conn, u8 *data,
                                     int len) TWS_CODE;
static int key_event_sync_event_hander(struct lmp_connection *conn, int event,
                                       int *arg) TWS_CODE;
static void tws_event_sync_tx_events_suss(void) TWS_CODE;

int sys_event_recode(struct sys_event *evt) TWS_CODE;
int sys_event_recode(struct sys_event *evt)
{
    if (evt->type != 1 || evt->arg == (void *)(unsigned long)1415009024u)
        return 0;

    struct link_connection *link = tws_link.link;
    if (event_lock || link == 0)
        return 0;

    tws_tx_unsniff_req();
    u8 log_index = rx_index;
    u8 log_length = event_len[log_index];
    u8 log_tx_index = tx_index;
    log_print(1, 0, "[Debug]: [TWS_KEY]tws_key: %d, %d, %d, %d\n\r\n",
              (int)evt->u.key.value, (int)log_length, (int)log_index,
              (int)log_tx_index);

    local_irq_disable();
    u8 index = rx_index;
    u8 length = event_len[index];
    if ((u32)length + 3u >= 65u) {
        local_irq_enable();
        return 0;
    }

    key_events[index][length] = evt->u.key.type;
    key_events[index][length + 1] = (u8)evt->u.key.event;
    key_events[index][length + 2] = (u8)evt->u.key.value;
    event_len[index] = (u8)(length + 3);

    int delay = 500;
    struct lmp_connection *conn = lmp_conn_for_address(0);
    if (conn != 0 && conn->core.operation_mode == SNIFF_MODE) {
        delay = 1000;
    } else if (tws_in_sniff_state()) {
        delay = 1000;
    }

    if (hi_timer == 0) {
        hi_timer = usr_timeout_add(0, tws_key_event_timeout, delay, 1);
    } else {
        usr_timeout_modify(hi_timer, delay);
    }
    local_irq_enable();
    return 1;
}

static void tws_key_event_timeout(void *priv)
{
    (void)priv;
    struct sys_event evt;
    __builtin_memset(&evt, 0, sizeof(evt));
    evt.arg = (void *)(unsigned long)1262835968u;
    evt.type = 1;

    local_irq_disable();
    event_lock = 1;
    u8 index = tx_index & 1;
    for (;;) {
        u8 length = event_len[index];
        if (length != 0) {
            int offset = 0;
            u8 *record = key_events[index];
            do {
                evt.u.key.type = record[0];
                evt.u.key.event = record[1];
                evt.u.key.value = record[2];
                sys_event_notify(&evt);
                offset += 3;
                record += 3;
                /* SDK reloads length after each notification. */
                length = event_len[index];
            } while (offset < length);
        }
        event_len[index] = 0;
        /* Reload rx_index each pass; callbacks may have changed it. */
        u8 current_rx_index = rx_index;
        if (current_rx_index == index)
            break;
        index = current_rx_index;
    }

    hi_timer = 0;
    rx_index = 0;
    tx_index = 0;
    event_lock = 0;
    local_irq_enable();
}

static int tws_event_sync_tx_events(struct lmp_connection *conn, u8 *buffer,
                                    int err)
{
    (void)conn;
    (void)err;
    local_irq_disable();
    u8 index = tx_index & 1;
    u8 length = event_len[index];
    if (length != 0) {
        rx_index = index ^ 1;
        tx_index |= 2;
        buffer[0] = tx_seqn;
        __builtin_memcpy(buffer + 1, key_events[index], length);
    }
    local_irq_enable();

    /* SDK reloads event_len after IRQs are enabled. */
    u8 remaining = event_len[index];
    return remaining == 0 ? 0 : (int)remaining + 1;
}

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
    __builtin_memset(&evt, 0, sizeof(evt));
    u8 sequence = data[0];
    if (sequence == rx_seqn)
        return;
    rx_seqn = sequence;
    evt.arg = (void *)(unsigned long)1415009024u;
    evt.type = 1;
    tws_tx_unsniff_req();

    u8 *record = data;
    for (int i = 1; i < len; i += 3, record += 3) {
        evt.u.key.type = record[1];
        evt.u.key.event = record[2];
        evt.u.key.value = record[3];
        sys_event_notify(&evt);
    }
}

static int key_event_sync_event_hander(struct lmp_connection *conn, int event,
                                       int *arg)
{
    (void)conn;
    (void)arg;
    if (event == 22) {
        rx_seqn = 0;
        tx_seqn = 1;
        rx_index = 0;
        tx_index = 0;
        event_lock = 0;
        event_len[0] = 0;
        event_len[1] = 0;
    }
    return 0;
}

void tws_event_sync_init(void) TWS_CODE;
void tws_event_sync_init(void)
{
}

static void tws_event_sync_tx_events_suss(void)
{
    struct sys_event evt;
    __builtin_memset(&evt, 0, sizeof(evt));
    evt.arg = (void *)(unsigned long)1262835968u;
    evt.type = 1;

    local_irq_disable();
    u8 index = tx_index & 1;
    if ((tx_index & 2) != 0) {
        event_lock = 1;
        u8 length = event_len[index];
        int offset = 0;
        u8 *record = key_events[index];
        while (offset < length) {
            evt.u.key.type = record[0];
            evt.u.key.event = record[1];
            evt.u.key.value = record[2];
            sys_event_notify(&evt);
            offset += 3;
            record += 3;
            /* Notification may mutate the queue length. */
            length = event_len[index];
        }
        event_lock = 0;
        event_len[index] = 0;
        tx_index = index ^ 1;
        tx_seqn++;
    }

    u8 current_rx_index = rx_index;
    if (event_len[current_rx_index] == 0 && hi_timer != 0) {
        usr_timeout_del(hi_timer);
        hi_timer = 0;
    }
    local_irq_enable();
}

const struct tws_sync_channel tws_event_sync
    __attribute__((section(".tws_sync_channel.8"))) = {
        .channel = 8,
        .attribute = 15,
        .tx_sync_req = tws_event_sync_tx_events,
        .tx_sync_req_suss = tws_event_sync_tx_events_suss,
        .rx_sync_req_and_res = tws_event_sync_rx_events_and_tx_events,
        .tx_sync_res_suss = tws_event_sync_tx_events_suss,
        .rx_sync_res = tws_event_sync_rx_events,
        .event_handler = key_event_sync_event_hander,
    };

#ifdef UNITTEST
#include "test.h"
#include <assert.h>
#include <string.h>

struct tws_link_connection tws_link;
static struct lmp_connection *test_lmp_conn;
static int notify_count;
static struct sys_event last_notified;
static int unsniff_count;
static int log_count;
static int timeout_add_count;
static int timeout_modify_count;
static int timeout_delete_count;
static int timeout_delay;
static u16 timeout_id = 37;
static void (*timeout_callback)(void *);
static int notify_mode;
static int enable_clear, sniff_value, sniff_calls;

void local_irq_disable(void) { }
void local_irq_enable(void)
{ if (enable_clear) { event_len[tx_index & 1]=0; enable_clear=0; } }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{
    (void)addr;
    return test_lmp_conn;
}
int tws_in_sniff_state(void) { sniff_calls++; return sniff_value; }
void tws_tx_unsniff_req(void) { unsniff_count++; }
void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag; (void)format;
    log_count++;
}
u16 usr_timeout_add(void *priv, void (*callback)(void *), int msec, u8 repeat)
{
    (void)priv; (void)repeat;
    timeout_add_count++;
    timeout_delay = msec;
    timeout_callback = callback;
    return timeout_id;
}
int usr_timeout_modify(u16 id, int msec)
{
    assert(id == timeout_id);
    timeout_modify_count++;
    timeout_delay = msec;
    return 0;
}
void usr_timeout_del(u16 id)
{
    assert(id == timeout_id);
    timeout_delete_count++;
}
void sys_event_notify(struct sys_event *event)
{
    last_notified = *event;
    notify_count++;
    if (notify_mode == 1 && notify_count == 1) {
        /* Timeout reloads length and rx_index after each callback. */
        event_len[0] = 6;
        rx_index = 1;
        event_len[1] = 3;
        key_events[1][0] = 0x71;
        key_events[1][1] = 0x72;
        key_events[1][2] = 0x73;
    } else if (notify_mode == 3) {
        if (notify_count == 1) event->u.key.init = 0x81;
        else assert(event->u.key.init == 0x81);
    } else if (notify_mode == 2) {
        assert(event_lock == 1); /* ACK holds its lock during callbacks. */
    }
}

int main(void)
{
    struct sys_event evt;
    __builtin_memset(&evt, 0, sizeof(evt));
    struct link_connection link;
    __builtin_memset(&link, 0, sizeof(link));
    struct lmp_connection conn;
    __builtin_memset(&conn, 0, sizeof(conn));
    conn.link = &link;
    tws_link.link = &link;
    test_lmp_conn = &conn;

    assert(tws_event_sync.channel == 8 && tws_event_sync.attribute == 15);
    assert(sys_event_recode(&evt) == 0); /* non-key event */
    evt.type = 1;
    evt.arg = (void *)(unsigned long)1415009024u;
    assert(sys_event_recode(&evt) == 0); /* looped-back event */

    evt.arg = 0;
    evt.u.key.type = 0x41;
    evt.u.key.event = 0x1234;
    evt.u.key.value = 0x56;
    assert(sys_event_recode(&evt) == 1);
    assert(log_count == 1 && unsniff_count == 1);
    assert(event_len[0] == 3);
    assert(key_events[0][0] == 0x41 && key_events[0][1] == 0x34 &&
           key_events[0][2] == 0x56);
    assert(timeout_add_count == 1 && timeout_delay == 500 && timeout_callback);

    u8 tx[65] = { 0 };
    assert(tws_event_sync_tx_events(0, tx, 0) == 4);
    assert(tx[0] == 0 && tx[1] == 0x41 && tx[2] == 0x34 && tx[3] == 0x56);
    notify_mode = 2;
    tws_event_sync_tx_events_suss();
    notify_mode = 0;
    assert(event_len[0] == 0 && tx_seqn == 1 && tx_index == 1);
    assert(timeout_delete_count == 1 && hi_timer == 0);

    /* Receive records use byte offsets 1, 4, ... and notify at stride 3. */
    u8 incoming[7] = { 8, 0x51, 0x23, 0x77, 0x52, 0x34, 0x78 };
    notify_count = 0;
    tws_event_sync_rx_events(0, incoming, sizeof(incoming));
    assert(notify_count == 2 && rx_seqn == 8);
    assert(last_notified.arg == (void *)(unsigned long)1415009024u);
    assert(last_notified.type == 1 && last_notified.u.key.init == 0 &&
           last_notified.u.key.type == 0x52 &&
           last_notified.u.key.event == 0x34 &&
           last_notified.u.key.value == 0x78);
    tws_event_sync_rx_events(0, incoming, sizeof(incoming));
    assert(notify_count == 2); /* repeated sequence is discarded */

    /* Timeout reloads queue length and rx_index after callbacks. */
    key_event_sync_event_hander(0, 22, 0);
    event_len[0] = 3;
    key_events[0][0] = 0x61;
    key_events[0][1] = 0x62;
    key_events[0][2] = 0x63;
    rx_index = 0;
    tx_index = 0;
    hi_timer = timeout_id;
    notify_count = 0;
    notify_mode = 1;
    tws_key_event_timeout(0);
    notify_mode = 0;
    assert(notify_count == 3 && last_notified.arg == (void *)(unsigned long)1262835968u);
    assert(last_notified.u.key.type == 0x71 && last_notified.u.key.event == 0x72 &&
           last_notified.u.key.value == 0x73);
    assert(event_len[0] == 0 && event_len[1] == 0 && rx_index == 0 &&
           tx_index == 0 && hi_timer == 0 && event_lock == 0);

    /* Event 22 reset intentionally leaves the timer alone. */
    hi_timer = timeout_id;
    event_len[0] = 3;
    key_event_sync_event_hander(0, 22, 0);
    assert(rx_seqn == 0 && tx_seqn == 1 && rx_index == 0 && tx_index == 0 &&
           event_len[0] == 0 && event_len[1] == 0 && hi_timer == timeout_id);
    /* One initialization per invocation: notification mutations survive. */
    notify_mode=3; notify_count=0; incoming[0]=9;
    tws_event_sync_rx_events(NULL,incoming,7);
    assert(notify_count==2 && last_notified.u.key.init==0x81);
    notify_mode=0;
    incoming[0]=10; notify_count=0;
    tws_event_sync_rx_events(NULL,incoming,-1);
    assert(rx_seqn==10 && notify_count==0);
    incoming[0]=11; tws_event_sync_rx_events(NULL,incoming,0);
    assert(rx_seqn==11 && notify_count==0);
    assert(tws_event_sync_rx_events_and_tx_events(NULL,NULL,0,NULL)==0);

    key_event_sync_event_hander(NULL,22,NULL); hi_timer=0;
    evt.arg=NULL; evt.type=1;
    for (unsigned i=0;i<21;i++) assert(sys_event_recode(&evt)==1);
    assert(event_len[0]==63 && sys_event_recode(&evt)==0 && event_len[0]==63);
    enable_clear=1;
    assert(tws_event_sync_tx_events(NULL,tx,0)==0); /* reload after IRQ enable */
    assert(tx[0]==1 && tx[1]==evt.u.key.type);

    key_event_sync_event_hander(NULL,22,NULL); hi_timer=0;
    conn.core.operation_mode=2; int before=sniff_calls;
    assert(sys_event_recode(&evt)==1 && timeout_delay==1000 && sniff_calls==before);
    conn.core.operation_mode=0; sniff_value=1;
    assert(sys_event_recode(&evt)==1 && timeout_delay==1000 && timeout_modify_count>0);
    sniff_value=0; timeout_id=0; hi_timer=0;
    assert(sys_event_recode(&evt)==1 && hi_timer==0);
    before=timeout_add_count;
    assert(sys_event_recode(&evt)==1 && timeout_add_count==before+1);
    event_lock=1; assert(sys_event_recode(&evt)==0); event_lock=0;
    tws_link.link=NULL; assert(sys_event_recode(&evt)==0); tws_link.link=&link;
    before=timeout_delete_count; tws_key_event_timeout(NULL);
    assert(timeout_delete_count==before);
    event_len[0]=3; tws_event_sync_init(); assert(event_len[0]==3);
    return TEST_DONE("btctrler_bd19_tws_event");
}
#endif
