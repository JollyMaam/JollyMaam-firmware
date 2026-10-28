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
    static const u32 transformed[4] = { 0, 1, 3, 0 };
    __builtin_memset(out, 0, 10);
    for (u32 i = 0; i < 10; ++i) {
        for (u32 shift = 0; shift < 8; shift += 2)
            out[i] |= (u8)(transformed[((map[i] >> shift) & 3) ^ 2] << shift);
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

void tws_afh_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len);
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
const u8 numbits_table[256] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8 };
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static int rx_wifi_calls, rx_afh_calls, slave_calls, timer_calls, map_calls;
static int sniff, after, classified, mutate_clock;
static u32 clock_value, rx_afh_instant;
static u16 timer_result;
static void *timer_priv;
static void (*timer_cb)(void *);
static struct lmp_connection *active;
static struct link_connection replacement;
static int replace_link;
static u8 desired[10], classification_seen[10];
void wifi_det_tx_slave_channel_map(u8 *map) { ++rx_wifi_calls; (void)map; }
void wifi_det_rx_slave_channel_map(u8 *map) { ++slave_calls; CHECK(map == desired); }
u8 wifi_det_master_slave_channel_map(u8 *map)
{ ++map_calls; memcpy(map, desired, 10); return 0; }
int tws_sniff_state_check(void) { return sniff; }
u32 tws_link_read_slot_clk(void)
{
    CHECK(tws_conn.update_afh != 0);
    if (mutate_clock) active->esco_conn = (void *)&replacement;
    return clock_value;
}
int bredr_clkn_after(int a, int b) { (void)a; (void)b; return after; }
u16 sys_timer_add(void *priv, void (*cb)(void *), u32 ms)
{ CHECK(ms == 100); timer_calls++; timer_priv=priv; timer_cb=cb; return timer_result; }
void lmp_tx_channel_classification(u8 *map)
{ classified++; memcpy(classification_seen, map, 10); CHECK(active->wfdet_counter == 0); }
void bredr_link_set_afh(struct link_connection *link, u32 instant, u8 en, u8 *map)
{
    ++rx_afh_calls; rx_afh_instant=instant; CHECK(en == 1);
    link->afh_slot_end=instant; memcpy(link->channel_map,map,10);
    if (replace_link) tws_link.link=&replacement;
}
int main(void)
{
    struct lmp_connection conn={0}, phone={0};
    struct link_connection link={0}, phone_link={0};
    _Alignas(4) u8 packet[16]={0}, output[16];
    u8 input[10], converted[10], a[10]={0}, b[10]={0};
    active=&conn; tws_link.link=&link; phone.link=&phone_link; tws_conn.lmp_conn=&phone;
    for (unsigned value=0; value<256; value++) {
        memset(input,value,10); wifi_det_change_channel_classification(input,converted);
        unsigned expected=0;
        for (unsigned bit=0; bit<8; bit+=2) {
            unsigned pair=(value>>bit)&3;
            expected |= (pair==0 ? 3 : pair==3 ? 1 : 0)<<bit;
        }
        for (unsigned i=0;i<9;i++) CHECK(converted[i]==expected);
        CHECK(converted[9] == ((value&64) ? ((expected&63)|64) : (expected|192)));
        b[0]=value;
        CHECK(wf_det_compare_map_diff(a,b)==(__builtin_popcount(value)>1));
    }
    wf_det_counter(NULL); conn.wfdet_counter=0xffffffff; wf_det_counter(&conn);
    CHECK(conn.wfdet_counter==0);
    CHECK(tws_afh_sync.tx_sync_req(NULL,NULL,0)==0);
    CHECK(tws_afh_sync_rx(NULL,NULL,-1,NULL)==0);
    CONFIG_TWS_AFH_ENABLE=2; CONFIG_WIFI_DETECT_ENABLE=1;
    memset(b,0,10); b[0]=3;
    CHECK(tws_afh_sync_rx(NULL,NULL,0,b)==10);
    CHECK(tws_afh_sync_rx(NULL,NULL,0,b)==0 && rx_wifi_calls==2);
    *(u32 *)(void *)packet=123; link.afh_slot_end=123;
    CHECK(tws_afh_sync_rx(NULL,packet,1,b)==0 && rx_wifi_calls==2);
    *(u32 *)(void *)packet=124; packet[4]=0xa5;
    CHECK(tws_afh_sync_rx(NULL,packet,-1,NULL)==0 && rx_afh_calls==1 && link.channel_map[0]==0xa5);
    CONFIG_WIFI_DETECT_ENABLE=2; tws_afh_sync_master_rx(NULL,desired,0); CHECK(slave_calls==0);
    CONFIG_WIFI_DETECT_ENABLE=1; tws_afh_sync_master_rx(NULL,desired,-1); CHECK(slave_calls==1);
    sniff=1; CHECK(tws_afh_sync.tx_sync_req(&conn,NULL,0)==0 && timer_calls==0); sniff=0;
    CHECK(tws_afh_sync.tx_sync_req(NULL,NULL,0)==0);
    conn.core.state=8; CHECK(tws_afh_sync.tx_sync_req(&conn,NULL,0)==0); conn.core.state=0;
    memset(link.channel_map,0,10); desired[0]=3; conn.wfdet_counter=5; timer_result=0xffff;
    CHECK(tws_afh_sync.tx_sync_req(&conn,NULL,0)==0);
    CHECK(timer_calls==1 && conn.wf_det_time==0xffff && timer_priv==&conn && timer_cb==wf_det_counter);
    conn.wfdet_counter=6; clock_value=0xfffffff0; mutate_clock=1; replace_link=1;
    memset(replacement.channel_map,0x69,10); memset(output,0xa5,sizeof(output));
    CHECK(tws_afh_sync.tx_sync_req(&conn,output,0)==14);
    CHECK(rx_afh_instant==384 && *(u32 *)(void *)output==384);
    CHECK(classified==1 && classification_seen[0]==0xfd && classification_seen[9]==0xff);
    CHECK(output[4]==0x69 && output[13]==0x69 && output[14]==0xa5);
    CHECK(timer_calls==1 && conn.wfdet_counter==0);
    mutate_clock=replace_link=0; after=0; memset(output,0xa5,16);
    CHECK(tws_afh_sync.tx_sync_req(&conn,output,0)==0 && tws_conn.update_afh==1 && output[0]==0xa5);
    after=1; CHECK(tws_afh_sync.tx_sync_req(&conn,output,0)==0 && tws_conn.update_afh==0);
    CONFIG_WIFI_DETECT_ENABLE=2; conn.esco_conn=NULL; tws_link.link=&link;
    memset(phone_link.channel_map,0x55,10); memset(link.channel_map,0,10); clock_value=10;
    CHECK(tws_afh_sync.tx_sync_req(&conn,output,0)==14 && rx_afh_instant==210 && output[4]==0x55);
    tws_conn.update_afh=0; tws_conn.pure_monitor=1;
    CHECK(tws_afh_sync.tx_sync_req(&conn,output,0)==14 && output[4]==0xff);
    tws_conn.update_afh=0;
    CHECK(tws_afh_sync.tx_sync_req(&conn,NULL,0)==0);
    tws_afh_sync.tx_sync_req_suss(); CHECK(tws_conn.update_afh==0);
    return TEST_DONE("btctrler_bd19_tws_afh");
}
#endif
