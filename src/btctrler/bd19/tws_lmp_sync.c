/* BD19 recovery of tws_lmp_sync.c.o.  The sync channel callbacks and their
 * unusual wire widths/return values follow the occurrence-specific SDK IR. */
#include <string.h>
#include "btctrler/glue.h"

#define CLASSIC_TWS __attribute__((section(".classic_tws_code")))
#define TWS_IRQ __attribute__((section(".tws_irq_code")))

/* Supplied by the retained BD19 tws_pair.c.o member. */
extern void do_phone_link_detach(void *priv);

static int tws_conn_sync_tx_core_data(struct lmp_connection *conn,
                                      u8 *txbuffer, int prev_req_err) TWS_IRQ;
static void tws_conn_sync_tx_suss(void) CLASSIC_TWS;
static int tws_lmp_slot_master_tx(struct lmp_connection *conn, u8 *tx,
                                  int prev_req_err) TWS_IRQ;

void tws_lmp_sync_timer_disable(void) CLASSIC_TWS;
void tws_lmp_sync_timer_disable(void)
{
    u8 timer = tws_conn.sync_timer;
    if (timer != 0xff) {
        slot_timer_put(timer);
        tws_conn.sync_timer = 0xff;
    }
}

int tws_conn_sync_rx_core_data(struct lmp_connection *conn, u8 *rxbuffer,
                               int len, u8 *txbuffer) CLASSIC_TWS;
int tws_conn_sync_rx_core_data(struct lmp_connection *conn, u8 *rxbuffer,
                               int len, u8 *txbuffer)
{
    u8 esco_state;
    (void)txbuffer;
    if (conn == NULL)
        return 0;
    esco_state = conn->core.esco_state;
    if (len == 5) {
        if (tws_conn.phone_link_detach_instant != 0)
            return 0;
        tws_conn.detach_code = rxbuffer[4];
        tws_conn.phone_link_detach_instant = *(u32 *)(void *)rxbuffer;
        {
            u32 clk = tws_link_read_slot_clk();
            s32 offset = bredr_clkn2offset(clk, *(u32 *)(void *)rxbuffer);
            u8 timer = slot_timer_get();
            tws_conn.sync_timer = timer;
            slot_timer_set(timer, do_phone_link_detach,
                           (void *)(uintptr_t)timer, offset > 2 ? offset : 2);
        }
    } else {
        __builtin_memcpy(&conn->core.stage, rxbuffer, 44);
        if (esco_state != 0)
            conn->core.esco_state = esco_state;
    }
    return 0;
}

static int tws_conn_sync_tx_core_data(struct lmp_connection *conn,
                                      u8 *txbuffer, int prev_req_err)
{
    u32 instant;
    (void)prev_req_err;
    if ((tws_conn.phone_link_detach & 1) != 0) {
        instant = tws_conn.phone_link_detach_instant;
        if (instant == 0) {
            instant = tws_link_read_slot_clk() + 300;
            tws_conn.phone_link_detach_instant = instant;
            {
                u8 timer = slot_timer_get();
                tws_conn.sync_timer = timer;
                slot_timer_set(timer, do_phone_link_detach,
                               (void *)(uintptr_t)timer, 300);
            }
            instant = tws_conn.phone_link_detach_instant;
        }
        *(u32 *)(void *)txbuffer = instant;
        txbuffer[4] = conn->err_code;
        return 5;
    }
    if (tws_conn.lmp_core_data_sync == 0)
        return 0;
    __builtin_memcpy(txbuffer, &conn->core.stage, 44);
    return 44;
}

static void tws_conn_sync_tx_suss(void)
{
    tws_conn.lmp_core_data_sync = 0;
}

void tws_lmp_core_sync_enable(void) CLASSIC_TWS;
void tws_lmp_core_sync_enable(void) { }

int tws_lmp_slot_sync_rx(struct lmp_connection *conn, u8 *rx, int len,
                         u8 *tx) TWS_IRQ;
int tws_lmp_slot_sync_rx(struct lmp_connection *conn, u8 *rx, int len,
                         u8 *tx)
{
    int working = is_tws_esco_working();
    u32 slot_clk = tws_link.esco_rx_lmp_slot_clk;
    u16 cmd;
    (void)conn; (void)rx; (void)len;
    if (working == 0 || slot_clk == 0)
        return 0;
    cmd = tws_link.esco_rx_lmp_cmd;
    __builtin_memcpy(tx, &slot_clk, 4);
    __builtin_memcpy(tx + 4, &cmd, 2);
    tws_link.esco_rx_lmp_slot_clk = 0;
    return 24;
}

void tws_lmp_slot_sync_master_rx(struct lmp_connection *conn, u8 *rx,
                                 int len) CLASSIC_TWS;
void tws_lmp_slot_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len)
{
    u32 instant = *(u32 *)(void *)rx;
    u32 slot_clk;
    u16 cmd;
    int suppress;
    (void)len;
    if (instant == 0)
        return;
    slot_clk = tws_link.esco_rx_lmp_slot_clk;
    suppress = (instant + 4 > slot_clk) && (instant - 4 < slot_clk);
    tws_link.esco_rx_lmp_slot_clk = 0;
    tws_link.esco_rx_lmp_cmd = 0;
    if (suppress)
        return;
    cmd = *(const u16 *)(const void *)((const u8 *)__builtin_assume_aligned(rx, 4) + 4);
    switch (cmd) {
    case 92: lmp_response(conn, "111", 8, 46, 30); break;
    case 13: tws_master_notice_slave_step_fun(2, 19, 0); break;
    case 62: lmp_rx_incr_power(conn); break;
    case 64: lmp_rx_decr_power(conn); break;
    }
}

static int tws_lmp_slot_master_tx(struct lmp_connection *conn, u8 *tx,
                                  int prev_req_err)
{
    (void)conn; (void)tx; (void)prev_req_err;
    return 0;
}

const struct tws_sync_channel tws_conn_sync
    __attribute__((section(".tws_sync_channel.2"), aligned(4))) = {
    .channel = 2, .attribute = 12,
    .tx_sync_req = (int (*)(struct lmp_connection *, u8 *, int))tws_conn_sync_tx_core_data,
    .tx_sync_req_suss = tws_conn_sync_tx_suss,
    .rx_sync_req_and_res = tws_conn_sync_rx_core_data,
};

const struct tws_sync_channel tws_lmp_slot_sync
    __attribute__((section(".tws_sync_channel.13"), aligned(4))) = {
    .channel = 13, .attribute = 13,
    .tx_sync_req = tws_lmp_slot_master_tx,
    .rx_sync_req_and_res = tws_lmp_slot_sync_rx,
    .rx_sync_res = tws_lmp_slot_sync_master_rx,
};

#ifdef UNITTEST
#include "test.h"

struct tws_connection tws_conn;
struct tws_link_connection tws_link;
static int puts_seen, timer_seen, clock_seen, offset_seen, action_seen;
static u8 timer_field_during_put;
static u8 timer_value;
static u32 clock_value;
static int offset_value, esco_working;
static int set_instant_after_timer;
static u32 *rx_instant_to_mutate, offset_end_seen;

void slot_timer_put(u8 timer)
{ puts_seen++; timer_seen = timer; timer_field_during_put = tws_conn.sync_timer; }
u8 slot_timer_get(void) { return timer_value; }
void slot_timer_set(u8 timer, void (*cb)(void *), void *priv, int delay)
{
    timer_seen = timer; (void)cb; (void)priv; offset_seen = delay;
    if (set_instant_after_timer)
        tws_conn.phone_link_detach_instant = 0xabcdef01;
}
u32 tws_link_read_slot_clk(void)
{
    clock_seen++;
    if (rx_instant_to_mutate != NULL) {
        *rx_instant_to_mutate = 0x76543210;
        rx_instant_to_mutate = NULL;
    }
    return clock_value;
}
int bredr_clkn2offset(int a, int b)
{ (void)a; offset_end_seen = (u32)b; return offset_value; }
int is_tws_esco_working(void) { return esco_working; }
int lmp_response(struct lmp_connection *c, const char *s, ...)
{ (void)c; (void)s; action_seen = 92; return 0; }
u8 tws_master_notice_slave_step_fun(u8 a, u8 b, u8 c)
{ (void)a; (void)b; (void)c; action_seen = 13; return 0; }
void lmp_rx_incr_power(struct lmp_connection *c) { (void)c; action_seen = 62; }
void lmp_rx_decr_power(struct lmp_connection *c) { (void)c; action_seen = 64; }
void do_phone_link_detach(void *p) { (void)p; }

int main(void)
{
    struct lmp_connection conn = {0};
    _Alignas(4) u8 rx[48] = {0};
    _Alignas(4) u8 out[48];
    u32 instant = 1000;
    u16 cmd = 62;

    memset(&tws_conn, 0, sizeof(tws_conn));
    memset(&tws_link, 0, sizeof(tws_link));
    memset(out, 0xa5, sizeof(out));
    tws_conn.sync_timer = 0xff;
    tws_lmp_sync_timer_disable();
    CHECK(puts_seen == 0 && tws_conn.sync_timer == 0xff);
    tws_conn.sync_timer = 7;
    tws_lmp_sync_timer_disable();
    CHECK(puts_seen == 1 && timer_seen == 7 && timer_field_during_put == 7 &&
          tws_conn.sync_timer == 0xff);

    esco_working = 1;
    tws_link.esco_rx_lmp_slot_clk = 0x12345678;
    tws_link.esco_rx_lmp_cmd = 0x4321;
    CHECK(tws_lmp_slot_sync_rx(&conn, NULL, 0, out + 1) == 24);
    CHECK(out[1] == 0x78 && out[4] == 0x12 && out[5] == 0x21 && out[6] == 0x43);
    CHECK(out[7] == 0xa5 && tws_link.esco_rx_lmp_slot_clk == 0);
    for (unsigned i = 7; i < sizeof(out); i++) CHECK(out[i] == 0xa5);
    esco_working = 0;
    tws_link.esco_rx_lmp_slot_clk = 5;
    CHECK(tws_lmp_slot_sync_rx(&conn, NULL, 0, out + 1) == 0);
    CHECK(tws_link.esco_rx_lmp_slot_clk == 5);

    memcpy(rx, &instant, 4); memcpy(rx + 4, &cmd, 2);
    tws_link.esco_rx_lmp_slot_clk = instant + 3;
    tws_link.esco_rx_lmp_cmd = 13;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 0 && tws_link.esco_rx_lmp_slot_clk == 0 &&
          tws_link.esco_rx_lmp_cmd == 0);
    tws_link.esco_rx_lmp_slot_clk = 2000;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 62);
    tws_link.esco_rx_lmp_slot_clk = instant - 4;
    action_seen = 0;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 62); /* strict lower boundary */
    tws_link.esco_rx_lmp_slot_clk = instant + 4;
    action_seen = 0;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 62); /* strict upper boundary */
    instant = 0xfffffffe; memcpy(rx, &instant, 4);
    tws_link.esco_rx_lmp_slot_clk = 1; action_seen = 0;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 62); /* unsigned wrap makes only one comparison true */
    action_seen = 0; instant = 0; memcpy(rx, &instant, 4);
    tws_link.esco_rx_lmp_slot_clk = 9;
    tws_lmp_slot_sync_master_rx(&conn, rx, 0);
    CHECK(action_seen == 0 && tws_link.esco_rx_lmp_slot_clk == 9);

    memset(&tws_conn, 0, sizeof(tws_conn));
    instant = 0x10203040; rx[4] = 0x55;
    memcpy(rx, &instant, 4); timer_value = 0xff; clock_value = 0x55667788;
    offset_value = -12; clock_seen = 0; rx_instant_to_mutate = (u32 *)(void *)rx;
    CHECK(tws_conn_sync_rx_core_data(&conn, rx, 5, NULL) == 0);
    CHECK(tws_conn.detach_code == 0x55 && tws_conn.phone_link_detach_instant == instant &&
          offset_end_seen == 0x76543210);
    CHECK(clock_seen == 1 && timer_seen == 0xff && offset_seen == 2);
    clock_seen = 0;
    tws_conn_sync_rx_core_data(&conn, rx, 5, NULL);
    CHECK(clock_seen == 0);

    /* The tx callback's SDK return values differ from bytes written. */
    memset(&tws_conn, 0, sizeof(tws_conn));
    memset(&conn, 0x3c, sizeof(conn));
    memset(out, 0xa5, sizeof(out));
    tws_conn.phone_link_detach = 1;
    clock_value = 0xfffffff0; timer_value = 0xff; set_instant_after_timer = 1;
    CHECK(tws_conn_sync.tx_sync_req(&conn, out, 0) == 5);
    CHECK(tws_conn.phone_link_detach_instant == 0xabcdef01 &&
          timer_seen == 0xff && offset_seen == 300);
    CHECK(out[0] == 0x01 && out[1] == 0xef && out[2] == 0xcd && out[3] == 0xab &&
          out[4] == conn.err_code);
    set_instant_after_timer = 0;
    tws_conn.phone_link_detach_instant = 7;
    CHECK(tws_conn_sync.tx_sync_req(&conn, out, 0) == 5);
    CHECK(clock_seen == 1 && out[0] == 7);
    tws_conn.phone_link_detach = 0; tws_conn.lmp_core_data_sync = 1;
    memset(&conn.core.stage, 0x6d, 44);
    CHECK(tws_conn_sync.tx_sync_req(&conn, out, 0) == 44 && out[0] == 0x6d && out[43] == 0x6d);
    tws_conn_sync.tx_sync_req_suss();
    CHECK(tws_conn.lmp_core_data_sync == 0 && tws_conn_sync.tx_sync_req(&conn, out, 0) == 0);

    /* Core receive preserves the eSCO byte across the SDK's 44-byte copy. */
    memset(&conn, 0, sizeof(conn));
    conn.core.esco_state = 0x80;
    memset(rx, 0x31, sizeof(rx));
    CHECK(tws_conn_sync_rx_core_data(&conn, rx, 44, NULL) == 0);
    CHECK(conn.core.esco_state == 0x80 && ((u8 *)&conn.core.stage)[0] == 0x31 &&
          ((u8 *)&conn.core.stage)[43] == 0x31);

    conn.core.esco_state = 0;
    CHECK(tws_conn_sync_rx_core_data(&conn, rx, -1, NULL) == 0);
    CHECK(conn.core.esco_state == 0x31);
    CHECK(tws_conn_sync_rx_core_data(NULL, NULL, 5, NULL) == 0);
    tws_conn.phone_link_detach = 2;
    tws_conn.lmp_core_data_sync = 0;
    CHECK(tws_conn_sync.tx_sync_req(NULL, NULL, 0) == 0);
    CHECK(tws_lmp_slot_sync.tx_sync_req(NULL, NULL, 0) == 0);
    tws_conn.lmp_core_data_sync = 7;
    tws_lmp_core_sync_enable();
    CHECK(tws_conn.lmp_core_data_sync == 7);

    /* Each action command runs outside the strict +/-4 suppression window. */
    instant = 1000; memcpy(rx, &instant, 4);
    {
      const u16 ops[] = { 92, 13, 62, 64 };
      for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        u16 op = ops[i];
        memcpy(rx + 4, &op, 2); tws_link.esco_rx_lmp_slot_clk = 2000;
        tws_link.esco_rx_lmp_cmd = op; action_seen = 0;
        tws_lmp_slot_sync_master_rx(&conn, rx, 0);
        CHECK(action_seen == op);
      }
    }
    return TEST_DONE("bd19_tws_lmp_sync");
}
#endif
