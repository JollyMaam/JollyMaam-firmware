/* Recovered SDK connection lookup and HCI address-routing policy.
 * The app currently has exactly one SDK ACL slot. Preserve the existing
 * key-reply fallback policy; the owned parser supplies an unframed BD_ADDR. */
#include <string.h>
#include "btctrler_sdk_abi.h"

static int reversed(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[5 - i]) return 0;
    return 1;
}
struct lmp_connection *lmp_conn_for_address(const unsigned char *address)
{
    struct lmp_connection *slot, *result = 0;
    /* Preserve the SDK's IRQ exclusion and unconditional enable on return.
     * This is its established lookup contract, not a new nested lock. */
    local_irq_disable();
    slot = BTCTRLER_ABI_acl_slot();
    if (BTCTRLER_ABI_connection_handle(slot) &&
        (BTCTRLER_ABI_connection_has_link(slot) || !address)) {
        const unsigned char *stored = BTCTRLER_ABI_connection_address(slot);
        if (!address || !memcmp(stored, address, 6) ||
            reversed(stored, address)) result = slot;
    }
    local_irq_enable();
    return result;
}
void lmp_hci_cmd_addr_to_conn(unsigned char *cmd, int *message)
{
    struct lmp_connection *conn;
    int opcode = message[0];
    cyclone2_bringup_record(32018);
    cyclone2_bringup_record(30720u | (opcode & 255));
    conn = lmp_conn_for_address(cmd);
    /* Every caller is in the owned link-control parser, which checks the
     * command length and passes parameters with BD_ADDR at offset zero. */
    /* Preserve the old patch's unconditional single-slot key-reply
     * fallback, even before a live handle/link is attached. */
    if (!conn && opcode == 0x040b) conn = BTCTRLER_ABI_acl_slot();
    const unsigned char *stored = BTCTRLER_ABI_connection_address(BTCTRLER_ABI_acl_slot());
    cyclone2_bringup_record(!memcmp(stored, cmd, 6) ? 32021 :
                           reversed(stored, cmd) ? 32022 : 32023);
    cyclone2_bringup_record(conn ? 32019 : 32020);
    /* SDK retains its assertion/reset behavior and role-machine dispatch. */
    BTCTRLER_BIN_route_selected(conn, message);
}

#ifdef UNITTEST
#include "test.h"
struct lmp_connection { unsigned short handle; int link; unsigned char address[6]; };
static struct lmp_connection slot = { 256, 1, {1, 2, 3, 4, 5, 6} };
static int locked, enables, calls, seen_opcode;
static struct lmp_connection *selected;
void local_irq_disable(void) { CHECK(!locked); locked = 1; }
void local_irq_enable(void) { CHECK(locked); locked = 0; enables++; }
struct lmp_connection *BTCTRLER_ABI_acl_slot(void) { return &slot; }
unsigned short BTCTRLER_ABI_connection_handle(struct lmp_connection *c)
{ CHECK(c == &slot && locked); return c->handle; }
int BTCTRLER_ABI_connection_has_link(struct lmp_connection *c)
{ CHECK(c == &slot && locked); return c->link; }
unsigned char *BTCTRLER_ABI_connection_address(struct lmp_connection *c)
{ CHECK(c == &slot); return c->address; }
void cyclone2_bringup_record(unsigned int code) { (void)code; }
void BTCTRLER_BIN_route_selected(struct lmp_connection *c, int *msg)
{ CHECK(!locked); selected = c; seen_opcode = msg[0]; calls++; }
int main(void)
{
    unsigned char reverse[6] = {6,5,4,3,2,1}, miss[6] = {7,7,7,7,7,7}, cmd[20];
    int message[] = { 0x040b, 0 };
    CHECK(lmp_conn_for_address(slot.address) == &slot);
    CHECK(lmp_conn_for_address(0) == &slot);
    CHECK(lmp_conn_for_address(miss) == 0);
    CHECK(lmp_conn_for_address(reverse) == &slot);
    slot.link = 0;
    CHECK(lmp_conn_for_address(slot.address) == 0);
    CHECK(lmp_conn_for_address(0) == &slot);
    slot.handle = 0; CHECK(lmp_conn_for_address(0) == 0);
    slot.handle = 256; slot.link = 1;
    for (int offset = 0; offset <= 9; offset++) {
        memset(cmd, 0xaa, sizeof(cmd)); memcpy(cmd + offset, slot.address, 6);
        message[0] = 0x040d; lmp_hci_cmd_addr_to_conn(cmd, message);
        CHECK(selected == (offset ? 0 : &slot));
        CHECK(seen_opcode == 0x040d);
    }
    /* Six readable bytes suffice, in either supported address order. */
    lmp_hci_cmd_addr_to_conn(reverse, message); CHECK(selected == &slot);
    memset(cmd, 0xaa, sizeof(cmd));
    message[0] = 0x040b; lmp_hci_cmd_addr_to_conn(cmd, message);
    CHECK(selected == &slot);
    slot.handle = 0; lmp_hci_cmd_addr_to_conn(cmd, message);
    CHECK(selected == &slot);
    message[0] = 0x040c; lmp_hci_cmd_addr_to_conn(cmd, message); CHECK(selected == 0);
    CHECK(calls == 14 && !locked && enables > 14);
    return TEST_DONE("btctrler_address");
}
#endif
