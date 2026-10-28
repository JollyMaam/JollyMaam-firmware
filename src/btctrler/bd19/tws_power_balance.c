/* BD19 battery-level sync and power-based TWS role switching. */
#include "btctrler/glue.h"

#define CLASSIC_TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_IRQ_CODE __attribute__((section(".tws_irq_code")))

static u16 power_update;
static u8 switch_role_cnt;

extern BTCTRLER_CONFIG int CONFIG_TWS_POWER_BALANCE_ENABLE;

#ifdef UNITTEST
struct tws_connection tws_conn;
int CONFIG_TWS_POWER_BALANCE_ENABLE;
static u16 test_battery_voltage;
static u8 test_predicate_result;
static u8 test_override_predicate;
static u8 test_mutate_counter;
static u8 test_mutated_counter;
static u16 test_predicate_m_voltage, test_predicate_s_voltage;
static u8 *test_remote_packet;
static u8 test_mutate_remote_on_voltage;
#endif

u16 tws_host_get_battery_voltage(void) __attribute__((weak)) CLASSIC_TWS_CODE;
u16 tws_host_get_battery_voltage(void)
{
#ifdef UNITTEST
    if (test_mutate_remote_on_voltage && test_remote_packet != NULL) {
        test_remote_packet[0] = 0;
        test_remote_packet[1] = 0;
    }
    return test_battery_voltage;
#else
    return 0;
#endif
}

/* The SDK symbol has zeroext i8 ABI. Use a distinct C identifier to avoid
 * the generated public bool prototype while retaining the actual symbol. */
u8 role_switch_predicate(u16 m_voltage, u16 s_voltage)
    __asm__("tws_host_role_switch_by_power_balance");
u8 role_switch_predicate(u16 m_voltage, u16 s_voltage)
    __attribute__((weak)) CLASSIC_TWS_CODE;
u8 role_switch_predicate(u16 m_voltage, u16 s_voltage)
{
#ifdef UNITTEST
    test_predicate_m_voltage = m_voltage;
    test_predicate_s_voltage = s_voltage;
    if (test_mutate_counter)
        switch_role_cnt = test_mutated_counter;
    if (test_override_predicate)
        return test_predicate_result;
    return (u32)m_voltage + 10u <= (u32)s_voltage;
#else
    return (u32)m_voltage + 10u <= (u32)s_voltage;
#endif
}

static int tws_power_balance_master_tx(struct lmp_connection *conn,
                                       u8 *tx, int len) TWS_IRQ_CODE;
static int tws_power_balance_master_tx(struct lmp_connection *conn,
                                       u8 *tx, int len)
{
    (void)conn;
    (void)tx;
    (void)len;
    return switch_role_cnt != 0;
}

static int tws_power_balance_slave_tx(struct lmp_connection *conn,
                                      u8 *rx, int len, u8 *tx) TWS_IRQ_CODE;
static int tws_power_balance_slave_tx(struct lmp_connection *conn,
                                      u8 *rx, int len, u8 *tx)
{
    (void)conn;
    (void)rx;
    if (CONFIG_TWS_POWER_BALANCE_ENABLE == 0)
        return 0;

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
                                        u8 *data, int len) CLASSIC_TWS_CODE;
static void tws_power_balance_master_rx(struct lmp_connection *conn,
                                        u8 *data, int len)
{
    (void)conn;
    (void)len;
    /* Capture the packet before either weak hook can mutate its backing data. */
    u16 remote_voltage = (u16)data[0] | ((u16)data[1] << 8);
    u16 local_voltage = tws_host_get_battery_voltage();
    u8 switch_role = role_switch_predicate(local_voltage, remote_voltage);
    if (switch_role == 0) {
        switch_role_cnt = 0;
        tws_conn.role_switch_by_power = 0;
    } else {
        /* Load only after the predicate hook: it may mutate SDK state. */
        switch_role_cnt = (u8)(switch_role_cnt + 1);
        if (switch_role_cnt > 4) {
            switch_role_cnt = 0;
            tws_conn.role_switch_by_power = 1;
        }
    }
}

const struct tws_sync_channel tws_power_balance
    __attribute__((section(".tws_sync_channel.10"), aligned(4))) = {
        .channel = 10,
        .attribute = 1,
        .tx_sync_req = tws_power_balance_master_tx,
        .rx_sync_req_and_res = tws_power_balance_slave_tx,
        .rx_sync_res = tws_power_balance_master_rx,
    };

void tws_power_banance_init(void) CLASSIC_TWS_CODE;
void tws_power_banance_init(void)
{
    power_update = 800;
}

#ifdef UNITTEST
#include "test.h"

/* Hooks in production are weak SDK extension points. Test-only override state
 * keeps the real function ABI while making callback side effects observable. */
int main(void)
{
    u8 packet[2] = {0xa5, 0x5a};
    u8 remote[2] = {110, 0};

    CHECK(tws_power_balance.channel == 10);
    CHECK(tws_power_balance.attribute == 1);
    CHECK(tws_power_balance.tx_sync_req != NULL);
    CHECK(tws_power_balance.rx_sync_req_and_res != NULL);
    CHECK(tws_power_balance.rx_sync_res != NULL);
    CHECK(role_switch_predicate(100, 109) == 0);
    CHECK(role_switch_predicate(100, 110) == 1);
    CHECK(role_switch_predicate(100, 111) == 1);
    CHECK(role_switch_predicate(65525, 65535) == 1);
    CHECK(role_switch_predicate(65526, 65535) == 0); /* widened sum */

    /* Disabled gate must not count, reset, call voltage, or touch tx. */
    power_update = 123;
    CONFIG_TWS_POWER_BALANCE_ENABLE = 0;
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 0);
    CHECK(power_update == 123 && packet[0] == 0xa5 && packet[1] == 0x5a);
    CONFIG_TWS_POWER_BALANCE_ENABLE = 1;

    test_battery_voltage = 0x1234;
    tws_power_banance_init();
    for (int i = 0; i < 199; ++i)
        CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 0);
    CHECK(packet[0] == 0xa5 && packet[1] == 0x5a);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, NULL) == 0);
    CHECK(power_update == 1000);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 2);
    CHECK(packet[0] == 0x34 && packet[1] == 0x12 && power_update == 0);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 7, packet) == 2);
    CHECK(packet[0] == 0x34 && packet[1] == 0x12);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, -1, packet) == 2);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 1, NULL) == 0);

    /* u16 wrap plus init's exact 800 value; init leaves role state alone. */
    power_update = 65535;
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 0);
    CHECK(power_update == 0);
    switch_role_cnt = 3;
    tws_power_banance_init();
    CHECK(power_update == 800 && switch_role_cnt == 3);
    for (int i = 0; i < 199; ++i)
        CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 0);
    CHECK(tws_power_balance.rx_sync_req_and_res(NULL, NULL, 0, packet) == 2);

    memset(&tws_conn, 0, sizeof(tws_conn));
    switch_role_cnt = 0;
    test_battery_voltage = 100;
    remote[0] = 110;
    test_override_predicate = 1;
    test_predicate_result = 0;
    tws_power_balance.rx_sync_res(NULL, remote, 2);
    CHECK(switch_role_cnt == 0 && tws_conn.role_switch_by_power == 0);
    CHECK(test_predicate_m_voltage == 100 && test_predicate_s_voltage == 110);
    test_predicate_result = 0x80; /* Any nonzero SDK i8 means true. */
    for (int i = 0; i < 4; ++i) {
        tws_power_balance.rx_sync_res(NULL, remote, 2);
        CHECK(switch_role_cnt == i + 1 && tws_conn.role_switch_by_power == 0);
    }
    tws_power_balance.rx_sync_res(NULL, remote, 2);
    CHECK(switch_role_cnt == 0 && tws_conn.role_switch_by_power == 1);

    /* Weak predicate sees captured packet bytes; its counter mutation is
     * observed because the SDK loads switch_role_cnt after calling the hook. */
    test_mutate_counter = 1;
    test_mutated_counter = 9;
    switch_role_cnt = 0;
    remote[0] = 7;
    remote[1] = 1;
    test_remote_packet = remote;
    test_mutate_remote_on_voltage = 1;
    tws_power_balance.rx_sync_res(NULL, remote, 0);
    CHECK(switch_role_cnt == 0 && tws_conn.role_switch_by_power == 1);
    CHECK(test_predicate_s_voltage == 263);
    CHECK(remote[0] == 0 && remote[1] == 0);
    test_mutate_counter = 0;
    test_mutate_remote_on_voltage = 0;
    test_remote_packet = NULL;
    test_override_predicate = 0;

    /* SDK callback reads two bytes regardless of len; record this unsafe
     * short-packet behavior as a follow-up rather than adding a new guard. */
    return TEST_DONE("btctrler_tws_power_balance_bd19");
}
#endif
