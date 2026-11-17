/* Owned lmp.c connection lookup by address and HCI command routing.
 * This controller has a single ACL slot.  The owned link-control parser
 * passes parameters with the BD_ADDR at offset zero. */
#include <string.h>
#include "btctrler/glue.h"

static int reversed(const u8 *a, const u8 *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[5 - i]) return 0;
    return 1;
}

struct lmp_connection *lmp_conn_for_address(u8 *address)
{
    struct lmp_connection *conn = &lmp_acl_link[0], *result = 0;
    local_irq_disable();
    /* The SDK requires a live handle and an attached link.  A NULL address
     * (any connection) may also find one whose link is not attached yet. */
    if (conn->core_s.handle && (conn->link || !address)) {
        const u8 *stored = conn->core_s.remote_addr;
        if (!address || !memcmp(stored, address, 6) || reversed(stored, address))
            result = conn;
    }
    local_irq_enable();
    return result;
}

void lmp_hci_cmd_addr_to_conn(u8 *cmd, int *msg)
{
    struct lmp_connection *conn = lmp_conn_for_address(cmd);
    /* A Link Key Request Reply goes to the only slot whatever its address
     * (the tested builds' fallback). */
    if (!conn && msg[0] == 0x040b) conn = &lmp_acl_link[0];
    if (!conn) btctrler_assert_failed("conn", msg[0]);
    lmp_role_machine(conn, 0, msg);
}

#ifdef UNITTEST
#include "test.h"
struct lmp_connection lmp_acl_link[1];
static struct lmp_connection *const slot = &lmp_acl_link[0];
static int locked, enables, calls, seen_opcode;
static struct lmp_connection *selected;
void local_irq_disable(void) { CHECK(!locked); locked = 1; }
void local_irq_enable(void) { CHECK(locked); locked = 0; enables++; }
static int asserted;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserted++; }
void lmp_role_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg)
{ CHECK(!locked && !rx); selected = c; seen_opcode = msg[0]; calls++; }
int main(void)
{
    u8 address[6] = {1,2,3,4,5,6}, reverse[6] = {6,5,4,3,2,1}, miss[6] = {7,7,7,7,7,7}, cmd[20];
    int message[] = { 0x040b, 0 };
    slot->core_s.handle = 256; slot->link = (struct link_connection *)1;
    memcpy(slot->core_s.remote_addr, address, 6);
    CHECK(lmp_conn_for_address(address) == slot);
    CHECK(lmp_conn_for_address(0) == slot);
    CHECK(lmp_conn_for_address(miss) == 0);
    CHECK(lmp_conn_for_address(reverse) == slot);
    slot->link = 0;
    CHECK(lmp_conn_for_address(address) == 0);
    CHECK(lmp_conn_for_address(0) == slot);
    slot->core_s.handle = 0; CHECK(lmp_conn_for_address(0) == 0);
    slot->core_s.handle = 256; slot->link = (struct link_connection *)1;
    for (int offset = 0; offset <= 9; offset++) {
        memset(cmd, 0xaa, sizeof(cmd)); memcpy(cmd + offset, address, 6);
        message[0] = 0x040d; lmp_hci_cmd_addr_to_conn(cmd, message);
        CHECK(selected == (offset ? 0 : slot));
        CHECK(seen_opcode == 0x040d);
    }
    lmp_hci_cmd_addr_to_conn(reverse, message); CHECK(selected == slot);
    memset(cmd, 0xaa, sizeof(cmd));
    message[0] = 0x040b; lmp_hci_cmd_addr_to_conn(cmd, message);
    CHECK(selected == slot);
    slot->core_s.handle = 0; lmp_hci_cmd_addr_to_conn(cmd, message);
    CHECK(selected == slot);
    message[0] = 0x040c; lmp_hci_cmd_addr_to_conn(cmd, message); CHECK(selected == 0);
    CHECK(calls == 14 && !locked && enables > 14 && asserted == 10);
    return TEST_DONE("btctrler_address");
}
#endif
