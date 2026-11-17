/* TWS timed-call queue and channel callbacks.  Keep the SDK's queue limit
 * check (> 8), byte-sized sequence arithmetic, and special UUID 68 payload
 * bit handling as recovered. */
#include "btctrler/glue.h"
#include <string.h>

#define TWS_CODE __attribute__((section(".classic_tws_code")))

static u32 sync_rec_instant_time;
static u8 sync_call_num;
static u8 tx_seqn;
static struct tws_sync_call_data sync_call_list[8];
static u8 rx_index;
static u8 tx_index;
static u8 tx_num;
static u8 rx_seqn;

static void tws_sync_call_rx_events(struct lmp_connection *conn, u8 *data,
                                    int len) TWS_CODE;
static void tws_sync_call_tx_events_suss(void) TWS_CODE;

extern const struct tws_sync_call tws_sync_call_begin[];
extern const struct tws_sync_call tws_sync_call_end[];
extern u32 tws_link_read_slot_clk(void);
extern u16 usr_timeout_add(void *priv, void (*func)(void *), u32 msec,
                           u8 reload);

static void tws_cmd_sync_call(struct tws_sync_call_data *sc, u8 tranid) TWS_CODE;
static void tws_cmd_sync_call(struct tws_sync_call_data *sc, u8 tranid)
{
    int clock = (int)tws_link_read_slot_clk();
    int offset = bredr_clkn2offset(clock, sc->instant);
    const struct tws_sync_call *p;
    void (*func)(void *) = 0;
    int uuid = sc->uuid;

    for (p = tws_sync_call_begin; p < tws_sync_call_end; p++) {
        if (p->uuid == uuid) {
            func = (void (*)(void *))p->func;
            break;
        }
    }
    if (func == 0)
        return;

    void *priv;
    if (tranid != 0 || uuid != 68) {
        priv = (void *)(unsigned long)(u32)sc->priv;
    } else {
        sc->priv &= ~128;
        priv = (void *)(unsigned long)(u32)sc->priv;
    }

    int mul = (((u32)offset - 4u) > 69119996u)
                  ? 2500 : (int)((u32)offset * 625u);
    (void)usr_timeout_add(priv, func, (u32)(mul / 1000), 1);
    /* The SDK call site passes three arguments although sdk.h declares four.
     * Preserve that ABI call shape and its NULL third argument. */
    u32 (*slot_time)(u8, u32, int *) =
        (u32 (*)(u8, u32, int *))get_bt_slot_time;
    sync_rec_instant_time = slot_time(1, 0, 0) + (u32)mul;
}

u32 get_sync_rec_instant_us_time(void) TWS_CODE;
u32 get_sync_rec_instant_us_time(void)
{
    return sync_rec_instant_time;
}

int tws_api_sync_call_func_internal(int uuid, int argc, int *argv) TWS_CODE;
int tws_api_sync_call_func_internal(int uuid, int argc, int *argv)
{
    (void)argc;
    int priv = argv[0];
    int delay = argv[1];
    if (tws_link.link == 0)
        return -22;
    if (sync_call_num > 8)
        return -12;

    tx_seqn++;
    sync_call_list[rx_index].seqn = tx_seqn;
    sync_call_list[rx_index].uuid = uuid;
    sync_call_list[rx_index].priv = priv;
    int clock = (int)tws_link_read_slot_clk();
    int offset = (delay * 1000) / 625;
    int instant = bredr_offset2clkn(clock, offset);
    sync_call_list[rx_index].instant = instant;
    local_irq_disable();
    sync_call_num++;
    local_irq_enable();
    tws_cmd_sync_call(&sync_call_list[rx_index], 1);
    rx_index++;
    if (rx_index == 8)
        rx_index = 0;
    return 0;
}

static int tws_sync_call_tx_events(struct lmp_connection *conn, u8 *buffer,
                                   int err) TWS_CODE;
static int tws_sync_call_tx_events(struct lmp_connection *conn, u8 *buffer,
                                   int err)
{
    (void)conn;
    (void)err;
    u8 count = sync_call_num;
    if (count == 0)
        return 0;

    struct tws_sync_call_data *out = (struct tws_sync_call_data *)buffer;
    int index = tx_index;
    for (int i = 0; i != count; i++) {
        /* The IR performs one 16-byte copy, including padding after seqn. */
        memcpy(&out[i].seqn, &sync_call_list[index].seqn, 16);
        index++;
        if (index == 8)
            index = 0;
    }
    tx_num = count;
    return (int)count << 4;
}

static int tws_sync_call_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
    TWS_CODE;
static int tws_sync_call_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    if (len != 0)
        tws_sync_call_rx_events(conn, rx, len);
    if (tx == 0)
        return 0;
    return tws_sync_call_tx_events(conn, tx, 0);
}

static void tws_sync_call_rx_events(struct lmp_connection *conn, u8 *data,
                                    int len) TWS_CODE;
static void tws_sync_call_rx_events(struct lmp_connection *conn, u8 *data,
                                    int len)
{
    (void)conn;
    struct tws_sync_call_data *d = (struct tws_sync_call_data *)data;
    for (int i = 0; i < len; i += 16, d++) {
        u8 seqn = d->seqn;
        if (rx_seqn >= seqn && (int)rx_seqn - (int)seqn < 8)
            continue;
        rx_seqn = seqn;
        tws_cmd_sync_call(d, 0);
    }
}

static void key_sync_call_event_hander(struct lmp_connection *conn,
                                       int event, int *arg) TWS_CODE;
static void key_sync_call_event_hander(struct lmp_connection *conn,
                                       int event, int *arg)
{
    (void)conn;
    (void)arg;
    if (event == 22) {
        rx_seqn = 0;
        tx_seqn = 0;
        rx_index = 0;
        tx_index = 0;
        tx_num = 0;
        sync_call_num = 0;
    }
}

const struct tws_sync_channel tws_sync_call
    __attribute__((section(".tws_sync_channel.9"))) = {
        .channel = 9,
        .attribute = 1,
        .tx_sync_req = tws_sync_call_tx_events,
        .tx_sync_req_suss = tws_sync_call_tx_events_suss,
        .rx_sync_req_and_res = tws_sync_call_rx_events_and_tx_events,
        .tx_sync_res_suss = tws_sync_call_tx_events_suss,
        .rx_sync_res = tws_sync_call_rx_events,
        /* The SDK stores a void handler in this int-returning slot by cast. */
        .event_handler = (int (*)(struct lmp_connection *, int, int *))
            key_sync_call_event_hander,
    };

void tws_sync_call_init(void) TWS_CODE;
void tws_sync_call_init(void)
{
}

static void tws_sync_call_tx_events_suss(void) TWS_CODE;
static void tws_sync_call_tx_events_suss(void)
{
    u8 count = tx_num;
    if (count == 0)
        return;
    for (u8 i = 0; i != count; i++) {
        tx_index++;
        if (tx_index == 8)
            tx_index = 0;
    }
    sync_call_num -= count;
    tx_num = 0;
}

#ifdef UNITTEST
#include "test.h"

/* Back the SDK's zero-length linker range with one actual fixture entry. */
static void test_sync_call(int a, int b);
struct tws_link_connection tws_link;
const struct tws_sync_call tws_sync_call_begin[1] = {
    { 68, test_sync_call, 0 },
};
__asm__(".globl tws_sync_call_end\n.set tws_sync_call_end, tws_sync_call_begin + 24");

static int mock_clock;
static u32 mock_slot_time;
static void *mock_priv;
static void (*mock_func)(void *);
static u32 mock_timeout;
static int callback_value;
static int mock_timeout_calls;

static void test_sync_call(int a, int b)
{
    callback_value = a + b;
}

u32 tws_link_read_slot_clk(void) { return (u32)mock_clock; }
void local_irq_disable(void) { }
void local_irq_enable(void) { }
int bredr_offset2clkn(int clock, int offset) { return clock + offset; }
int bredr_clkn2offset(int clock, int instant) { return instant - clock; }
u16 usr_timeout_add(void *priv, void (*func)(void *), u32 timeout, u8 reload)
{
    (void)reload;
    mock_priv = priv;
    mock_func = func;
    mock_timeout = timeout;
    mock_timeout_calls++;
    return 1;
}
u32 get_bt_slot_time(u8 type, u32 time, int *ret_time,
                     int (*local_us_time)(void))
{
    (void)type; (void)time; (void)ret_time; (void)local_us_time;
    return mock_slot_time;
}

int main(void)
{
    struct link_connection link = { 0 };
    CHECK(tws_sync_call.channel == 9 && tws_sync_call.attribute == 1);
    CHECK(tws_sync_call.tx_sync_req == tws_sync_call_tx_events &&
          tws_sync_call.rx_sync_res == tws_sync_call_rx_events);
    tws_link.link = &link;
    mock_clock = 100;
    mock_slot_time = 77;
    int argv[2] = { 12, 1000 };

    tws_link.link = 0;
    CHECK(tws_api_sync_call_func_internal(68, 2, argv) == -22);
    tws_link.link = &link;
    CHECK(tws_api_sync_call_func_internal(68, 2, argv) == 0);
    CHECK(sync_call_list[0].seqn == 1 && sync_call_list[0].uuid == 68);
    CHECK(sync_call_list[0].priv == 12 && sync_call_list[0].instant == 1700);
    CHECK(mock_priv == (void *)(unsigned long)12 && mock_func != 0);
    CHECK(mock_timeout == 1000 && get_sync_rec_instant_us_time() == 1000077);

    u8 payload[16] = { 0 };
    CHECK(tws_sync_call_tx_events(0, payload, 0) == 16);
    CHECK(memcmp(payload, &sync_call_list[0], 16) == 0);
    tws_sync_call_tx_events_suss();
    CHECK(sync_call_num == 0 && tx_index == 1 && tx_num == 0);
    CHECK(tws_sync_call_tx_events(0, payload, 0) == 0);

    /* The SDK's > 8 guard accepts 8 and increments the byte count to 9. */
    sync_call_num = 8;
    rx_index = 0;
    CHECK(tws_api_sync_call_func_internal(68, 2, argv) == 0);
    CHECK(sync_call_num == 9);
    CHECK(tws_api_sync_call_func_internal(68, 2, argv) == -12);
    key_sync_call_event_hander(0, 22, 0);
    CHECK(sync_call_num == 0 && tx_seqn == 0 && rx_index == 0 &&
          tx_index == 0 && tx_num == 0 && rx_seqn == 0);
    sync_call_num = 2;
    tx_num = 2;
    tx_index = 7;
    tws_sync_call_tx_events_suss();
    CHECK(sync_call_num == 0 && tx_index == 1 && tx_num == 0);
    CHECK(callback_value == 0);

    /* SDK zexts both sequence bytes before subtracting in i32. Differences
     * above 127 must not become negative and suppress the received call. */
    struct tws_sync_call_data incoming = { .uuid = 68, .priv = 0x180,
                                          .instant = 105 };
    const u8 previous[] = { 9, 10, 200, 255, 0 };
    const u8 next[] = { 2, 2, 1, 1, 255 };
    const int accepted[] = { 0, 1, 1, 1, 1 };
    for (int i = 0; i < 5; ++i) {
        incoming.seqn = next[i];
        incoming.priv = 0x180;
        rx_seqn = previous[i];
        int before = mock_timeout_calls;
        tws_sync_call_rx_events(0, (u8 *)&incoming, 16);
        CHECK(mock_timeout_calls == before + accepted[i]);
        CHECK(rx_seqn == (accepted[i] ? next[i] : previous[i]));
        CHECK(incoming.priv == (accepted[i] ? 0x100 : 0x180));
    }
    return TEST_DONE("btctrler_tws_sync_call");
}
#endif
