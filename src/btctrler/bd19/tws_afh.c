/* BD19 recovery of tws_afh.c.o occurrence 1. */
#include <string.h>
#include "btctrler/glue.h"

#define TWS_IRQ __attribute__((section(".tws_irq_code")))

extern BTCTRLER_CONFIG int CONFIG_TWS_AFH_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_WIFI_DETECT_ENABLE;
extern const u8 numbits_table[256];
extern int tws_sniff_state_check(void);
extern u32 tws_link_read_slot_clk(void);
extern u16 sys_timer_add(void *priv, void (*func)(void *), u32 msec);
extern u8 wifi_det_master_slave_channel_map(u8 *map);
extern void wifi_det_rx_slave_channel_map(u8 *map);
extern void wifi_det_tx_slave_channel_map(u8 *map);

static u8 last_master_map[10];

static u8 wf_det_compare_map_diff(const u8 *a, const u8 *b)
{
    u32 diff = 0;
    for (u32 i = 0; i < 10; ++i)
        diff += numbits_table[a[i] ^ b[i]];
    return diff > 1;
}

static void wifi_det_change_channel_classification(const u8 *map, u8 *out)
{
    static const u8 transformed[4] = { 0, 1, 3, 0 };
    __builtin_memset(out, 0, 10);
    for (u32 i = 0; i < 10; ++i) {
        u8 v = 0;
        for (u32 shift = 0; shift < 8; shift += 2)
            v |= (u8)(transformed[((map[i] >> shift) & 3) ^ 2] << shift);
        out[i] = v;
    }
    if (map[9] & 0x40) {
        out[9] &= 0x3f;
        out[9] |= 0x40;
    } else {
        out[9] |= 0xc0;
    }
}

void wf_det_counter(void *priv) TWS_IRQ;
void wf_det_counter(void *priv)
{
    if (priv) {
        struct lmp_connection *conn = priv;
        ++conn->wfdet_counter;
    }
}

int tws_afh_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_IRQ;
int tws_afh_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    (void)conn;
    if (CONFIG_TWS_AFH_ENABLE == 0)
        return 0;
    /* SDK's zero-length branch must not load the (possibly absent) packet. */
    if (len != 0) {
        u32 instant = *(u32 *)rx;
        struct link_connection *link = tws_link.link;
        if (instant == link->afh_slot_end)
            return 0;
        bredr_link_set_afh(link, instant, 1, rx + 4);
    }
    if (tx && CONFIG_WIFI_DETECT_ENABLE == 1) {
        wifi_det_tx_slave_channel_map(tx);
        if (__builtin_memcmp(last_master_map, tx, 10) != 0) {
            __builtin_memcpy(last_master_map, tx, 10);
            return 10;
        }
    }
    return 0;
}

void tws_afh_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len) TWS_IRQ;
void tws_afh_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len)
{
    (void)conn;
    (void)len;
    if (CONFIG_TWS_AFH_ENABLE != 0 && CONFIG_WIFI_DETECT_ENABLE == 1)
        wifi_det_rx_slave_channel_map(rx);
}

static int tws_afh_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err) TWS_IRQ;
static int tws_afh_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err)
{
    u8 map[10];
    u8 classification[10];
    (void)prev_req_err;
    if (CONFIG_TWS_AFH_ENABLE == 0)
        return 0;
    __builtin_memset(map, 0, sizeof(map));

    if (tws_conn.update_afh == 0) {
        if (tws_sniff_state_check() != 0)
            return 0;
        if (CONFIG_WIFI_DETECT_ENABLE != 1) {
            if (tws_conn.pure_monitor == 0 && conn->esco_conn == 0) {
                struct lmp_connection *phone = tws_conn.lmp_conn;
                __builtin_memcpy(map, phone->link->channel_map, 10);
                if (__builtin_memcmp(tws_link.link->channel_map, map, 10) == 0)
                    return 0;
            } else {
                __builtin_memset(map, 0xff, 10);
                if (__builtin_memcmp(tws_link.link->channel_map, map, 10) == 0)
                    return 0;
            }
        } else {
            if (conn == 0 || (conn->core.state & 8) != 0)
                return 0;
            if (conn->wf_det_time == 0)
                conn->wf_det_time = sys_timer_add(&conn->rejust_esco_req,
                                                  wf_det_counter, 100);
            (void)wifi_det_master_slave_channel_map(map);
            if (conn->wfdet_counter < 6 ||
                wf_det_compare_map_diff(tws_link.link->channel_map, map) == 0)
                return 0;
            conn->wfdet_counter = 0;
            wifi_det_change_channel_classification(map, classification);
            lmp_tx_channel_classification(classification);
        }
        tws_conn.update_afh = 1;
        u32 now = tws_link_read_slot_clk();
        /* Deliberately reload after the clock callback; it can change eSCO state. */
        u32 instant = now + (conn->esco_conn == 0 ? 200 : 400);
        struct link_connection *link = tws_link.link;
        bredr_link_set_afh(link, instant, 1, map);
        *(u32 *)tx = instant;
        /* The SDK copies from the refreshed link map after set_afh. */
        link = tws_link.link;
        __builtin_memcpy(tx + 4, link->channel_map, 10);
        return 14;
    }

    u32 now = tws_link_read_slot_clk();
    struct link_connection *link = tws_link.link;
    if (bredr_clkn_after(now, link->afh_slot_end))
        tws_conn.update_afh = 0;
    return 0;
}

static void tws_afh_sync_tx_suss(void) { }

const struct tws_sync_channel tws_afh_sync
    __attribute__((section(".tws_sync_channel.3"), aligned(4))) = {
        .channel = 3,
        .attribute = 5,
        .tx_sync_req = tws_afh_master_tx,
        .tx_sync_req_suss = tws_afh_sync_tx_suss,
        .rx_sync_req_and_res = tws_afh_sync_rx,
        .tx_sync_res_suss = 0,
        .rx_sync_res = tws_afh_sync_master_rx,
        .event_handler = 0,
    };

#ifdef UNITTEST
#include "test.h"

int CONFIG_TWS_AFH_ENABLE, CONFIG_WIFI_DETECT_ENABLE;
const u8 numbits_table[256] = {
    [0] = 0, [1] = 1, [2] = 1, [3] = 2, [4] = 1, [5] = 2, [6] = 2,
    [7] = 3, [8] = 1, [9] = 2, [10] = 2, [11] = 3, [12] = 2,
    [13] = 3, [14] = 3, [15] = 4,
};
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static int rx_wifi_calls, rx_afh_calls;
static u32 rx_afh_instant;

void wifi_det_tx_slave_channel_map(u8 *map) { ++rx_wifi_calls; (void)map; }
void wifi_det_rx_slave_channel_map(u8 *map) { (void)map; }
void bredr_link_set_afh(struct link_connection *link, u32 instant, u8 en, u8 *map)
{
    ++rx_afh_calls;
    rx_afh_instant = instant;
    link->afh_slot_end = instant;
    link->channel_map_update = en;
    __builtin_memcpy(link->channel_map, map, 10);
}

int main(void)
{
    u8 input[10] = { 0 }, output[10];
    input[9] = 0x40;
    wifi_det_change_channel_classification(input, output);
    CHECK(output[0] == 0xff);
    CHECK(output[9] == 0x7f);

    u8 a[10] = { 0 }, b[10] = { 0 };
    CHECK(wf_det_compare_map_diff(a, b) == 0);
    b[0] = 1;
    CHECK(wf_det_compare_map_diff(a, b) == 0); /* SDK strict > 1 threshold */
    b[1] = 1;
    CHECK(wf_det_compare_map_diff(a, b) == 1);

    struct link_connection link = { 0 };
    tws_link.link = &link;
    CONFIG_TWS_AFH_ENABLE = 1;
    CONFIG_WIFI_DETECT_ENABLE = 1;
    rx_wifi_calls = rx_afh_calls = 0;
    CHECK(tws_afh_sync_rx(0, 0, 0, 0) == 0); /* zero length does not load rx */
    CHECK(rx_afh_calls == 0);
    CHECK(tws_afh_sync_rx(0, 0, 0, b) == 10); /* WiFi map still processed */
    CHECK(rx_wifi_calls == 1);

    u8 packet[14] __attribute__((aligned(4))) = { 0 };
    u32 instant = 0x12345678;
    __builtin_memcpy(packet, &instant, sizeof(instant));
    packet[4] = 0xa5;
    link.afh_slot_end = instant;
    int before = rx_wifi_calls;
    CHECK(tws_afh_sync_rx(0, packet, sizeof(packet), b) == 0);
    CHECK(rx_wifi_calls == before); /* duplicate instant exits before WiFi */
    instant++;
    __builtin_memcpy(packet, &instant, sizeof(instant));
    CHECK(tws_afh_sync_rx(0, packet, sizeof(packet), b) == 0);
    CHECK(rx_afh_calls == 1 && rx_afh_instant == instant);
    CHECK(link.channel_map[0] == packet[4]);
    puts("btctrler_bd19_tws_afh: ok");
    return 0;
}
#endif
