/* The SDK TWS-over-BLE phone-link hooks and slave establish handlers are
 * empty stubs or return 0 in this build, and stay that way. The original
 * member also exports weak host hooks, queue-idle checks, and monitor control;
 * keep those behaviors for whole-member ownership. */
#include "btctrler/glue.h"

extern const struct ble_operation *__ble_ops;
extern void log_print(int level, const char *tag, const char *format, ...);

/* This object accesses the SDK-owned TWS state, whose declaration is not in
 * either recovered SDK header. Keep its BD19 IR byte layout local here. */
struct tws_ble_frame_dynamic_data { u8 field0; u32 field1; u8 field2; u8 field3; };
struct tws_ble_link_state {
    u8 field0;
    u8 *field1;
    u8 slot_timer;
    u8 state;
    u32 field4;
    struct tws_ble_frame_dynamic_data field5;
    u8 field6;
};
extern struct tws_ble_link_state tws_ble_link;
extern void tws_link_ble_slave_start_monitor(void);

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

/* These weak SDK host hooks are intentionally no-ops in the original
 * BD19 object; they remain weak so a product implementation can override. */
__attribute__((weak)) int tws_host_get_ble_core_data(u8 *packet, int size)
{
    (void)packet; (void)size;
    return 0;
}

__attribute__((weak)) int tws_host_set_ble_core_data(u8 *packet, int size)
{
    (void)packet; (void)size;
    return 0;
}

__attribute__((weak)) int tws_host_ble_state_is_busy(void)
{
    return 0;
}

void tws_link_check_for_ble_link_switch(void) {}

int ll_conn_idle(struct le_link *link)
{
    if ((config_btctler_le_roles & 3) == 0) {
        log_print(4, NULL, "<Error>: [T_BLE]No either LE_MASTER or LE_SLAVE support\r\n");
        return 0;
    }
    if (link->rx_oneshot_head.next != &link->rx_oneshot_head ||
        link->event_oneshot_head.next != &link->event_oneshot_head ||
        link->acl_packet_head.next != &link->acl_packet_head ||
        link->event_oneshot_post_head.next != &link->event_oneshot_post_head)
        return 0;
    int idle;
    __ble_ops->ioctrl(link->hw, 19, &idle);
    return idle != 0;
}

int tws_rx_ble_setup_monitor_link_req(u8 *data, int len)
{
    (void)data; (void)len;
    return 0;
}

int tws_master_tx_ble_start_monitor_req(void)
{
    return 0;
}

void tws_conn_ble_slave_start_monitor(void *p)
{
    (void)p;
    slot_timer_disable(tws_ble_link.slot_timer);
    if (tws_ble_link.state == 1) {
        tws_ble_link.state = 2;
        tws_link_ble_slave_start_monitor();
    }
    log_print(2, NULL, "[Info]: [T_BLE][s] Mo end\n\r\n");
}

void tws_conn_slave_rx_ble_monitor_start_req(u8 *rx)
{
    (void)rx;
}

#ifdef UNITTEST
#include "test.h"

int config_btctler_le_roles;
struct tws_ble_link_state tws_ble_link;
static int ioctrl_calls, ioctrl_idle, disabled_slot, monitor_starts, logged;
static void test_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    (void)hw;
    CHECK(cmd == 19);
    va_start(ap, cmd);
    int *idle = va_arg(ap, int *);
    va_end(ap);
    *idle = ioctrl_idle;
    ioctrl_calls++;
}
static const struct ble_operation test_ops = { .ioctrl = test_ioctrl };
const struct ble_operation *__ble_ops = &test_ops;
void slot_timer_disable(u8 slot) { disabled_slot = slot; }
void tws_link_ble_slave_start_monitor(void) { monitor_starts++; }
void log_print(int level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; logged++; }

int main(void)
{
    /* The IR bodies are "ret void" / "ret i32 0"; the test only pins the
     * return value that callers branch on. */
    tws_phone_ble_link_connection(NULL);
    tws_phone_ble_link_disconnection(NULL);
    tws_link_ble_slave_conn_establish_probe_handler();
    tws_link_ble_slave_conn_establish_handler();
    CHECK(tws_link_get_ble_role_slave() == 0);
    CHECK(tws_host_get_ble_core_data(NULL, 123) == 0);
    CHECK(tws_host_set_ble_core_data(NULL, 123) == 0);
    CHECK(tws_host_ble_state_is_busy() == 0);
    CHECK(tws_rx_ble_setup_monitor_link_req(NULL, 123) == 0);
    CHECK(tws_master_tx_ble_start_monitor_req() == 0);
    tws_link_check_for_ble_link_switch();
    tws_conn_slave_rx_ble_monitor_start_req(NULL);

    /* ll_conn_idle checks four distinct queues before IOCTL 19. */
    struct le_link link = { 0 };
    link.rx_oneshot_head.next = link.rx_oneshot_head.prev = &link.rx_oneshot_head;
    link.event_oneshot_head.next = link.event_oneshot_head.prev = &link.event_oneshot_head;
    link.acl_packet_head.next = link.acl_packet_head.prev = &link.acl_packet_head;
    link.event_oneshot_post_head.next = link.event_oneshot_post_head.prev = &link.event_oneshot_post_head;
    config_btctler_le_roles = 3;
    ioctrl_idle = 1;
    CHECK(ll_conn_idle(&link) == 1 && ioctrl_calls == 1);
    ioctrl_idle = 0;
    CHECK(ll_conn_idle(&link) == 0 && ioctrl_calls == 2);
    link.acl_packet_head.next = (struct list_head *)1; /* next != head is sufficient */
    link.acl_packet_head.prev = &link.acl_packet_head;
    CHECK(ll_conn_idle(&link) == 0 && ioctrl_calls == 2);
    link.acl_packet_head.next = &link.acl_packet_head;
    config_btctler_le_roles = 0;
    CHECK(ll_conn_idle(&link) == 0 && ioctrl_calls == 2 && logged == 1);

    tws_ble_link.slot_timer = 7;
    tws_ble_link.state = 1;
    tws_conn_ble_slave_start_monitor(NULL);
    CHECK(disabled_slot == 7 && tws_ble_link.state == 2 && monitor_starts == 1 && logged == 2);
    tws_conn_ble_slave_start_monitor(NULL);
    CHECK(monitor_starts == 1 && logged == 3);
    return TEST_DONE("btctrler_tws_ble_link");
}
#endif
