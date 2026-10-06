/* TWS battery-level sync and power-based role switching.  Keep the SDK's
 * 16-bit poll counter, strict threshold comparisons, and five-report switch. */
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))

static u16 power_update;
static u8 switch_role_cnt;

#ifdef UNITTEST
struct tws_connection tws_conn;
#endif

#ifdef UNITTEST
static u16 test_battery_voltage;
#endif

u16 tws_host_get_battery_voltage() __attribute__((weak)) TWS_CODE;
u16 tws_host_get_battery_voltage(void)
{
#ifdef UNITTEST
    return test_battery_voltage;
#else
    return 0;
#endif
}

static int tws_power_balance_master_tx(struct lmp_connection *conn,
                                       u8 *tx, int len) TWS_CODE;
static int tws_power_balance_master_tx(struct lmp_connection *conn,
                                       u8 *tx, int len)
{
    (void)conn;
    (void)tx;
    (void)len;
    return switch_role_cnt != 0;
}

static int tws_power_balance_slave_tx(struct lmp_connection *conn,
                                      u8 *rx, int len, u8 *tx) TWS_CODE;
static int tws_power_balance_slave_tx(struct lmp_connection *conn,
                                      u8 *rx, int len, u8 *tx)
{
    (void)conn;
    (void)rx;
    if (len == 0) {
        power_update = (u16)(power_update + 1);
        if (tx == NULL || power_update < 1000)
            return 0;
    } else if (tx == NULL) {
        return 0;
    }

    power_update = 0;
    u16 voltage = tws_host_get_battery_voltage();
    tx[0] = (u8)voltage;
    tx[1] = (u8)(voltage >> 8);
    return 2;
}

static void tws_power_balance_master_rx(struct lmp_connection *conn,
                                        u8 *data, int len) TWS_CODE;
static void tws_power_balance_master_rx(struct lmp_connection *conn,
                                        u8 *data, int len)
{
    (void)conn;
    (void)len;
    u16 remote_voltage = (u16)data[0] | ((u16)data[1] << 8);
    u16 local_voltage = tws_host_get_battery_voltage();
    if ((u32)local_voltage + 10u > remote_voltage) {
        switch_role_cnt = 0;
        tws_conn.role_switch_by_power = 0;
    } else {
        switch_role_cnt = (u8)(switch_role_cnt + 1);
        if (switch_role_cnt > 4) {
            switch_role_cnt = 0;
            tws_conn.role_switch_by_power = 1;
        }
    }
}

const struct tws_sync_channel tws_power_balance
    __attribute__((section(".tws_sync_channel.10"))) = {
        .channel = 10,
        .attribute = 3,
        .tx_sync_req = tws_power_balance_master_tx,
        .rx_sync_req_and_res = tws_power_balance_slave_tx,
        .rx_sync_res = tws_power_balance_master_rx,
    };

void tws_power_banance_init(void) TWS_CODE;
void tws_power_banance_init(void)
{
    power_update = 800;
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    u8 packet[2] = {0xaa, 0xbb};
    u8 remote[2];

    CHECK(tws_power_balance.channel == 10);
    CHECK(tws_power_balance.attribute == 3);
    CHECK(tws_power_balance.tx_sync_req != NULL);
    CHECK(tws_power_balance.rx_sync_req_and_res != NULL);
    CHECK(tws_power_balance.rx_sync_res != NULL);

    test_battery_voltage = 0x1234;
    tws_power_banance_init();
    for (int i = 0; i < 199; ++i)
        CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 0);
    CHECK(packet[0] == 0xaa && packet[1] == 0xbb);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, NULL) == 0);
    /* Null tx still advances the 16-bit poll counter. The next call crosses
     * 1000 and returns the little-endian voltage in two bytes. */
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 2);
    CHECK(packet[0] == 0x34 && packet[1] == 0x12);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 7, packet) == 2);
    CHECK(packet[0] == 0x34 && packet[1] == 0x12);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 7, NULL) == 0);

    memset(&tws_conn, 0, sizeof(tws_conn));
    switch_role_cnt = 0;
    test_battery_voltage = 100;
    remote[0] = 110; remote[1] = 0;
    tws_power_balance.rx_sync_res(NULL, remote, 2);
    CHECK(tws_conn.role_switch_by_power == 0); /* equal to local + 10 counts */
    CHECK(tws_power_balance.tx_sync_req(NULL, NULL, 0) == 1);
    remote[0] = 111;
    for (int i = 0; i < 3; ++i) {
        tws_power_balance.rx_sync_res(NULL, remote, 2);
        CHECK(tws_power_balance.tx_sync_req(NULL, NULL, 0) == 1);
        CHECK(tws_conn.role_switch_by_power == 0);
    }
    tws_power_balance.rx_sync_res(NULL, remote, 2);
    CHECK(tws_power_balance.tx_sync_req(NULL, NULL, 0) == 0);
    CHECK(tws_conn.role_switch_by_power == 1);

    /* A report below local+10 clears the count and the role flag. */
    remote[0] = 109;
    tws_power_balance.rx_sync_res(NULL, remote, 2);
    CHECK(tws_power_balance.tx_sync_req(NULL, NULL, 0) == 0);
    CHECK(tws_conn.role_switch_by_power == 0);

    /* SDK ignores len in the receive callback and reads exactly two bytes. */
    remote[0] = 111; remote[1] = 0;
    for (int i = 0; i < 5; ++i)
        tws_power_balance.rx_sync_res(NULL, remote, 0);
    CHECK(tws_conn.role_switch_by_power == 1);
    return TEST_DONE("btctrler_tws_power_balance");
}
#endif
