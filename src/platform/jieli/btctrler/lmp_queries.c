/* Classic link queries and small controls, recovered from SDK lmp.c. */
#include <string.h>
#include "btctrler/glue.h"

int get_cur_connect_phone_mac_addr(void);

s8 link_conn_get_rssi(struct link_connection *link)
{ return link && link->frame ? (s8)bredr_analog_get_rssi(link->frame) : -127; }
s8 bredr_get_rssi_for_address(u8 *address)
{
    if (!address) return -127;
    local_irq_disable();
    struct lmp_connection *conn = lmp_conn_for_address(address);
    s8 result = conn ? link_conn_get_rssi(conn->link) : -127;
    local_irq_enable();
    return result;
}
s8 get_rssi_api(s8 *phone_rssi, s8 *tws_rssi)
{
    if (tws_rssi) *tws_rssi = -127;
    if (phone_rssi) *phone_rssi = -127;
    if (tws_link.link) {
        if (tws_rssi && tws_link.link->frame)
            *tws_rssi = (s8)bredr_analog_get_rssi(tws_link.link->frame);
        if (phone_rssi && tws_link.plink && tws_link.plink->frame)
            *phone_rssi = (s8)bredr_analog_get_rssi(tws_link.plink->frame);
    } else if (phone_rssi) {
        u8 *address = (u8 *)(uintptr_t)(u32)get_cur_connect_phone_mac_addr();
        *phone_rssi = bredr_get_rssi_for_address(address);
    }
    return 0;
}
u8 *lmp_private_remote_addr_for_handler(int handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    if (conn) return conn->core_s.remote_addr;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn)
        if (conn->core_s.handle && conn->esco_conn && conn->esco_conn->hci_handle == handle)
            return conn->core_s.remote_addr;
    return NULL;
}
u16 lmp_private_handler_for_remote_addr(u8 *addr, int link_type)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    if (!conn) return 0;
    if (link_type == 1)
        return (conn->core.state & 9) == 8 ? 0 : conn->core_s.handle;
    if (addr && conn->esco_conn && !memcmp(conn->core_s.remote_addr, addr, 6))
        return conn->esco_conn->hci_handle;
    return 0;
}
u8 lmp_get_tws_phone_addr(u8 *addr, int link_type)
{
    if (!addr || link_type != 1) return 0;
    struct lmp_connection *conn = lmp_conn_for_address(NULL);
    if (!conn) return 0;
    /* This API writes an eight-byte record: LE handle followed by address.
     * Byte stores also support the SDK's unaligned output buffer. */
    addr[0] = conn->core_s.handle;
    addr[1] = conn->core_s.handle >> 8;
    memcpy(addr + 2, conn->core_s.remote_addr, 6);
    return 1;
}
void link_conn_set_max_slot(struct link_connection *link, u8 max_slot)
{ link->core.max_slot = max_slot; }
u8 link_conn_get_max_slot(struct link_connection *link)
{ return link->core.max_slot; }
void bredr_set_dut_enble(u8 enable, u8 phone)
{
    lmp_dut_en = enable;
    lmp_dut_phone_en = phone;
    bt_dut_reset_mdm();
}
u8 bredr_get_dut_enable(void) { return lmp_dut_en; }
u8 get_page_remote_name(void) { return page_parm.remote_name_req; }
struct lmp_connection *get_updata_conn(void)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn)
        if (conn->core_s.handle) return conn;
    return NULL;
}

#ifdef UNITTEST
#include <assert.h>
#include <limits.h>
#include <stdio.h>
struct lmp_connection lmp_acl_link[1];
struct page_parm_t page_parm;
struct tws_link_connection tws_link;
u8 lmp_dut_en, lmp_dut_phone_en;
static struct lmp_connection lookup_conn;
static struct link_connection link, peer_link;
static struct ctrl_frame frame, peer_frame;
static struct lmp_esco esco;
static u8 address[6] = { 1, 2, 3, 4, 5, 6 };
static int found, irq_depth, locks, unlocks, address_reads, handle_reads, radio_reads, current_reads, dut_resets;
static int lookup_handle, current_missing, signed_rssi, mutate_peer;
static u8 expected_dut, expected_phone;
void local_irq_disable(void) { assert(!irq_depth); irq_depth = 1; ++locks; }
void local_irq_enable(void) { assert(irq_depth == 1); irq_depth = 0; ++unlocks; }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ assert(!addr || addr == address); ++address_reads; return found ? &lookup_conn : NULL; }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ ++handle_reads; return handle == lookup_handle ? &lookup_conn : NULL; }
char bredr_analog_get_rssi(struct ctrl_frame *radio)
{
    assert(radio == &frame || radio == &peer_frame); ++radio_reads;
    if (mutate_peer) tws_link.plink = &peer_link;
    return radio == &frame ? (char)signed_rssi : -42;
}
int get_cur_connect_phone_mac_addr(void)
{ ++current_reads; return current_missing ? 0 : (int)(uintptr_t)address; }
void bt_dut_reset_mdm(void)
{ assert(lmp_dut_en == expected_dut && lmp_dut_phone_en == expected_phone); ++dut_resets; }
static void reset(void)
{
    assert(!irq_depth); memset(lmp_acl_link, 0, sizeof(lmp_acl_link)); memset(&lookup_conn, 0, sizeof(lookup_conn));
    memset(&link, 0, sizeof(link)); memset(&peer_link, 0, sizeof(peer_link)); memset(&tws_link, 0, sizeof(tws_link));
    memset(&esco, 0, sizeof(esco));
    lookup_conn.link = &link; link.frame = &frame; peer_link.frame = &peer_frame;
    memcpy(lookup_conn.core_s.remote_addr, address, 6);
    found = 1; lookup_handle = 0x1234; lookup_conn.core_s.handle = 0x1234;
    locks = unlocks = address_reads = handle_reads = radio_reads = current_reads = dut_resets = 0;
    current_missing = mutate_peer = 0; signed_rssi = -60;
}
int main(void)
{
    for (int value = -128; value < 128; ++value) {
        reset(); signed_rssi = value;
        assert(bredr_get_rssi_for_address(address) == value && radio_reads == 1 && locks == 1 && unlocks == 1);
        reset(); signed_rssi = value; s8 phone = 0, tws = 0;
        assert(!get_rssi_api(&phone, &tws) && phone == value && tws == -127 && current_reads == 1);
    }
    reset(); assert(bredr_get_rssi_for_address(NULL) == -127 && !locks && !address_reads);
    for (int missing = 0; missing < 3; ++missing) {
        reset();
        if (missing == 0) found = 0;
        if (missing == 1) lookup_conn.link = NULL;
        if (missing == 2) link.frame = NULL;
        assert(bredr_get_rssi_for_address(address) == -127 && !radio_reads && locks == 1 && unlocks == 1);
    }
    for (int topology = 0; topology < 16; ++topology)
        for (int outputs = 0; outputs < 4; ++outputs) {
            reset();
            tws_link.link = topology & 1 ? &link : NULL;
            tws_link.plink = topology & 2 ? &peer_link : NULL;
            if (!(topology & 4)) link.frame = NULL;
            if (!(topology & 8)) peer_link.frame = NULL;
            s8 phone = 7, tws = 8;
            assert(!get_rssi_api(outputs & 1 ? &phone : NULL, outputs & 2 ? &tws : NULL));
            int expected_phone = topology & 1 ? ((topology & 10) == 10 ? -42 : -127) : (topology & 4 ? -60 : -127);
            int expected_tws = (topology & 5) == 5 ? -60 : -127;
            assert(phone == (outputs & 1 ? expected_phone : 7));
            assert(tws == (outputs & 2 ? expected_tws : 8));
            assert(current_reads == (!(topology & 1) && (outputs & 1)));
        }
    reset(); current_missing = 1; s8 phone, tws;
    get_rssi_api(&phone, &tws); assert(phone == -127 && tws == -127 && !locks);
    reset(); tws_link.link = &link; tws_link.plink = &peer_link;
    s8 alias; get_rssi_api(&alias, &alias); assert(alias == -42 && radio_reads == 2);
    reset(); tws_link.link = &link; mutate_peer = 1;
    get_rssi_api(&phone, &tws); assert(phone == -42 && tws == -60);
    reset(); assert(lmp_private_remote_addr_for_handler(lookup_handle) == lookup_conn.core_s.remote_addr);
    for (int handle = -1; handle <= 256; ++handle) {
        reset(); lmp_acl_link->core_s.handle = 0x30; lmp_acl_link->esco_conn = &esco; esco.hci_handle = 123;
        assert(lmp_private_remote_addr_for_handler(handle) == (handle == 123 ? lmp_acl_link->core_s.remote_addr : NULL));
    }
    reset(); lmp_acl_link->esco_conn = &esco; esco.hci_handle = 123;
    assert(!lmp_private_remote_addr_for_handler(123));
    for (unsigned state = 0; state < 256; ++state) {
        reset(); lookup_conn.core.state = state;
        assert(lmp_private_handler_for_remote_addr(address, 1) == ((state & 9) == 8 ? 0 : 0x1234));
    }
    for (int type = -2; type < 4; ++type)
        for (int has_esco = 0; has_esco < 2; ++has_esco)
            for (int match = 0; match < 2; ++match) {
                reset(); esco.hci_handle = 241; if (has_esco) lookup_conn.esco_conn = &esco;
                if (!match) lookup_conn.core_s.remote_addr[0] ^= 1;
                assert(lmp_private_handler_for_remote_addr(address, type) == (type == 1 ? 0x1234 : has_esco && match ? 241 : 0));
            }
    reset(); found = 0; assert(!lmp_private_handler_for_remote_addr(address, 1));
    reset(); assert(lmp_private_handler_for_remote_addr(NULL, 1) == 0x1234);
    assert(!lmp_private_handler_for_remote_addr(NULL, 0));
    for (int type = -2; type < 4; ++type)
        for (int exists = 0; exists < 2; ++exists) {
            reset(); found = exists; u8 out[10]; memset(out, 0xa5, sizeof(out));
            int written = type == 1 && exists;
            assert(lmp_get_tws_phone_addr(out + 1, type) == written);
            assert(out[0] == 0xa5 && out[9] == 0xa5);
            if (written) assert(out[1] == 0x34 && out[2] == 0x12 && !memcmp(out + 3, address, 6));
            else for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0xa5);
        }
    reset(); assert(!lmp_get_tws_phone_addr(NULL, 1) && !address_reads);
    for (unsigned value = 0; value < 256; ++value) {
        reset(); link.core.pwr_set = -20; link.core.remote_sr = 2;
        link_conn_set_max_slot(&link, value);
        assert(link_conn_get_max_slot(&link) == value && link.core.pwr_set == -20 && link.core.remote_sr == 2);
        page_parm.remote_name_req = value; assert(get_page_remote_name() == value);
        for (unsigned other = 0; other < 256; ++other) {
            expected_dut = value; expected_phone = other;
            bredr_set_dut_enble(value, other); assert(bredr_get_dut_enable() == value);
        }
        assert(dut_resets == 256);
        lmp_acl_link->core_s.handle = value; assert(get_updata_conn() == (value ? lmp_acl_link : NULL));
    }
    puts("btctrler link query tests passed");
}
#endif
