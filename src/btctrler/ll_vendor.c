/* The SDK's BLE vendor helpers (ll_vendor.c): per-connection queries and
 * tweaks looked up by connection handle, the LE coded-PHY S2/S8 selection,
 * and thin wrappers over the HCI-level operations.  SDK quirks kept: a zero
 * handle or an unknown handle returns 0 (the RSSI query returns -127); the
 * supervision timeout is converted as timeout * 4 / 5 with an unsigned
 * divide, masked to 16 bits, and only applied when it exceeds twice the
 * connection interval, with interrupts off around the check; any code type
 * other than 1 is stored as code_type - 2, so 2 gives 0 (S8 log text and
 * all) and 0 wraps to 254; ble_vendor_is_slow_state is a weak hook that
 * always says no.  ll_set_code_type lives here and is read by the extended
 * advertising, initiating and PHY code. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL;
extern void log_print(int level, const char *tag, const char *format, ...);

u8 ll_set_code_type;

/* RSSI of the link, or -127 for no such link. */
s8 ble_vendor_get_peer_rssi(u16 conn_handle)
{
    if (conn_handle == 0)
        return -127;
    struct le_link *link = ll_link_for_handle(conn_handle);
    if (link == NULL)
        return -127;
    return link->rssi;
}

__attribute__((weak)) int ble_vendor_is_slow_state(void)
{
    return 0;
}

void *ble_enter_dut_tx_mode(void *param)
{
    return __ble_ops->send_single_carrier(param);
}

/* Only the low four bits of enable are kept. */
bool ble_vendor_interval_event_enable(u16 conn_handle, int enable)
{
    if (conn_handle == 0)
        return 0;
    struct le_link *link = ll_link_for_handle(conn_handle);
    if (link == NULL)
        return 0;
    link->interval_event_enable = enable & 15;
    return 1;
}

/* 1 selects S2; everything else is stored minus two (S8 is 2 -> 0). */
void ll_vendor_set_code_type(u8 code_type)
{
    if (code_type == 1) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]ll_vendor_set_code_type: S2\r\n");
        ll_set_code_type = 1;
    } else {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]ll_vendor_set_code_type: S8\r\n");
        ll_set_code_type = (u8)(code_type - 2);
    }
}

void ll_vendor_access_addr_generate(u8 *out_address)
{
    access_addr_generate(out_address);
}

/* 1 if the timeout was applied. */
bool ll_vendor_change_local_supervision_timeout(u16 conn_handle, u32 timeout)
{
    if (conn_handle == 0)
        return 0;
    struct le_link *link = ll_link_for_handle(conn_handle);
    if (link == NULL)
        return 0;
    u8 ret = 0;
    local_irq_disable();
    if (((timeout << 2) / 5 & 65535) > (u32)(link->ll_data.interval << 1)) {
        ll_supervision_timeout_changeperiod(link, timeout);
        ret = 1;
    }
    local_irq_enable();
    return ret;
}

bool ll_vendor_check_update_procedure_is_exist(u16 conn_handle)
{
    if (conn_handle == 0)
        return 0;
    struct le_link *link = ll_link_for_handle(conn_handle);
    if (link == NULL)
        return 0;
    return ll_control_procedure_is_exist(link, 32771);
}

/* Copies the 7-byte address (type and address) of the peer when role is
 * non-zero, of the local side when it is 0; returns 1 on success. */
bool ll_vendor_get_link_address_info(u16 conn_handle, u8 role, u8 *addr_info)
{
    if (conn_handle == 0)
        return 0;
    struct le_link *link = ll_link_for_handle(conn_handle);
    if (link == NULL)
        return 0;
    if (role == 0)
        memcpy(addr_info, &link->local, 7);
    else
        memcpy(addr_info, &link->peer, 7);
    return 1;
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

const struct ble_operation *__ble_ops;
u8 log_tag_const_i_LL;
static struct le_link the_link;
static int logs, irq_off, irq_on, change_calls, aa_calls, proc_calls;
static u32 change_timeout;
static void *carrier_arg;
static u32 proc_bits;

struct le_link *ll_link_for_handle(int handle) { return handle == 7 ? &the_link : NULL; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; logs++; }
void local_irq_disable(void) { irq_off++; }
void local_irq_enable(void) { irq_on++; }
void ll_supervision_timeout_changeperiod(struct le_link *link, u32 timeout) { (void)link; change_calls++; change_timeout = timeout; }
void access_addr_generate(u8 *aa) { aa_calls++; aa[0] = 0x5a; }
bool ll_control_procedure_is_exist(struct le_link *link, u32 bits) { (void)link; proc_calls++; proc_bits = bits; return 1; }
static void *carrier(void *p) { carrier_arg = p; return (void *)0x1234; }

int main(void)
{
    /* Unknown handles. */
    CHECK(ble_vendor_get_peer_rssi(0) == -127);
    CHECK(ble_vendor_get_peer_rssi(8) == -127);
    the_link.rssi = -42;
    CHECK(ble_vendor_get_peer_rssi(7) == -42);
    CHECK(ble_vendor_is_slow_state() == 0);

    struct ble_operation ops;
    memset(&ops, 0, sizeof ops);
    ops.send_single_carrier = carrier;
    __ble_ops = &ops;
    CHECK(ble_enter_dut_tx_mode((void *)0x99) == (void *)0x1234);
    CHECK(carrier_arg == (void *)0x99);

    /* Only the low 4 bits of enable are stored; the send nibble is kept. */
    the_link.interval_event_send = 9;
    CHECK(!ble_vendor_interval_event_enable(0, 1));
    CHECK(!ble_vendor_interval_event_enable(8, 1));
    CHECK(ble_vendor_interval_event_enable(7, 0x35));
    CHECK(the_link.interval_event_enable == 5);
    CHECK(the_link.interval_event_send == 9);

    /* Code type: 1 -> 1, 2 -> 0, 3 -> 1, 0 -> 254 (wraps). */
    log_tag_const_i_LL = 1;
    ll_vendor_set_code_type(1);
    CHECK(ll_set_code_type == 1 && logs == 1);
    ll_vendor_set_code_type(2);
    CHECK(ll_set_code_type == 0 && logs == 2);
    ll_vendor_set_code_type(3);
    CHECK(ll_set_code_type == 1);
    ll_vendor_set_code_type(0);
    CHECK(ll_set_code_type == 254);
    log_tag_const_i_LL = 0;
    logs = 0;
    ll_vendor_set_code_type(1);
    ll_vendor_set_code_type(2);
    CHECK(logs == 0 && ll_set_code_type == 0);

    u8 aa[4] = { 0 };
    ll_vendor_access_addr_generate(aa);
    CHECK(aa_calls == 1 && aa[0] == 0x5a);

    /* Supervision timeout: (timeout*4/5) & 0xffff > 2*interval.  With
     * interval 10 the threshold is 20: timeout 25 -> 20 (no), 26 -> 20 (no:
     * 104/5 = 20), 27 -> 21 (yes). */
    the_link.ll_data.interval = 10;
    CHECK(!ll_vendor_change_local_supervision_timeout(0, 27));
    CHECK(!ll_vendor_change_local_supervision_timeout(8, 27));
    CHECK(irq_off == 0 && change_calls == 0);
    CHECK(!ll_vendor_change_local_supervision_timeout(7, 26));
    CHECK(irq_off == 1 && irq_on == 1 && change_calls == 0);
    CHECK(ll_vendor_change_local_supervision_timeout(7, 27));
    CHECK(irq_off == 2 && irq_on == 2 && change_calls == 1 && change_timeout == 27);
    /* The 16-bit mask: 0x14000 * 4 / 5 = 0x10000 * 1.0 = 0x10000 -> 0. */
    CHECK(!ll_vendor_change_local_supervision_timeout(7, 0x14000));
    CHECK(change_calls == 1);
    /* Unsigned divide: 0x80000000 << 2 wraps to 0, so 0 > 20 is false. */
    CHECK(!ll_vendor_change_local_supervision_timeout(7, 0x80000000u));

    CHECK(!ll_vendor_check_update_procedure_is_exist(0));
    CHECK(!ll_vendor_check_update_procedure_is_exist(8));
    CHECK(proc_calls == 0);
    CHECK(ll_vendor_check_update_procedure_is_exist(7));
    CHECK(proc_calls == 1 && proc_bits == 32771);

    /* Address info: role 0 copies local (7 bytes), else peer. */
    for (int i = 0; i < 7; i++) {
        ((u8 *)&the_link.local)[i] = 0x10 + i;
        ((u8 *)&the_link.peer)[i] = 0x20 + i;
    }
    u8 out[8];
    memset(out, 0xee, sizeof out);
    CHECK(!ll_vendor_get_link_address_info(0, 0, out));
    CHECK(!ll_vendor_get_link_address_info(8, 0, out));
    CHECK(out[0] == 0xee);
    CHECK(ll_vendor_get_link_address_info(7, 0, out));
    CHECK(out[0] == 0x10 && out[6] == 0x16 && out[7] == 0xee);
    memset(out, 0xee, sizeof out);
    CHECK(ll_vendor_get_link_address_info(7, 1, out));
    CHECK(out[0] == 0x20 && out[6] == 0x26 && out[7] == 0xee);
    return TEST_DONE("btctrler_ll_vendor");
}
#endif
