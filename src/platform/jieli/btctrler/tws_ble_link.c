/* The SDK's TWS-over-BLE link hooks (tws_ble_link.c): the entry points the
 * LE slave link layer and link_layer.c call when a phone link connects or
 * drops, and the slave establish handlers.  In this SDK build every one of
 * them is an empty stub or returns 0 (no TWS-over-BLE role is compiled in),
 * and that is kept.  The member's other functions (the weak host core data
 * hooks, ll_conn_idle, the monitor start requests) have no callers and go
 * with it. */
#include "btctrler/glue.h"

void tws_phone_ble_link_connection(struct le_link *link)
{
    (void)link;
}

void tws_phone_ble_link_disconnection(struct le_link *link)
{
    (void)link;
}

void tws_link_ble_slave_conn_establish_probe_handler(void)
{
}

void tws_link_ble_slave_conn_establish_handler(void)
{
}

int tws_link_get_ble_role_slave(void)
{
    return 0;
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    /* The IR bodies are "ret void" / "ret i32 0"; the test only pins the
     * return value that callers branch on. */
    tws_phone_ble_link_connection(NULL);
    tws_phone_ble_link_disconnection(NULL);
    tws_link_ble_slave_conn_establish_probe_handler();
    tws_link_ble_slave_conn_establish_handler();
    CHECK(tws_link_get_ble_role_slave() == 0);
    return TEST_DONE("btctrler_tws_ble_link");
}
#endif
