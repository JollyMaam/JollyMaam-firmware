/* Remaining public Classic HCI control APIs, recovered from SDK lmp_hci.c. */
#include <string.h>
#include "btctrler/glue.h"

_Static_assert(BTCFG_NAME_REQ, "remote name requests are enabled");
u16 CRC16(const void *ptr, u32 len);

static u32 read32(const u8 *p)
{ return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 read16(const u8 *p) { return p[0] | (u32)p[1] << 8; }

u8 *get_tws_internal_addr(int channel)
{
    return channel == 'R' ? gbredr_local_dev.internal_addr_remote : gbredr_local_dev.internal_addr_local;
}
void lmp_hci_write_local_priv_version(const char *ic_version, const char *priv_version, u8 *addr)
{
    u8 *data = gbredr_local_dev.priv_version;
    memcpy(data, ic_version, 4); memcpy(data + 4, priv_version, 2); memcpy(data + 6, addr, 6);
    data[12] = CRC16(data, 12); /* SDK stores only the low CRC byte. */
}
void lmp_hci_remote_name_request(u8 *addr, u8 repetition, u16 clk_offset)
{
    (void)clk_offset; /* The SDK ignores the clock hint for name requests. */
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    if (conn) lmp_tx_name_req(conn);
    else link_page_try_start_enable(1, addr, repetition, 0);
}
void lmp_hci_create_connection(const u8 *addr, u16 packet_type, u8 repetition_mode,
                               u8 reserved, u16 clk_offset, u8 allow_role_switch)
{
    (void)reserved;
    /* Fix: a BD_ADDR has only six bytes; the second task word is zero-padded. */
    btctrler_hci_cmd_to_task(0x400002, 7, 0x0405, read32(addr), read16(addr + 4),
                           packet_type, repetition_mode, clk_offset, allow_role_switch);
}
void lmp_hci_cancel_page(void)
{ btctrler_hci_cmd_to_task(0x400002, 2, 0x0cfd, 0); }
void lmp_hci_connection_cancel(u8 *addr)
{ btctrler_hci_cmd_to_task(0x400002, 3, 0x0cfe, read32(addr), read16(addr + 4)); }
void lmp_hci_inquiry(int lap, u8 length, u8 num)
{ btctrler_hci_cmd_to_task(0x400002, 4, 0x0401, lap, length, num); }
void lmp_hci_cancel_inquiry(void)
{ btctrler_hci_cmd_to_task(0x400002, 1, 0x0402); }
void lmp_hci_remote_name_request_cus(u8 *addr)
{ btctrler_hci_cmd_to_task(0x400002, 5, 0x0419, read32(addr), read16(addr + 4), 0, 0); }
void lmp_hci_tx_channel_chassification(u8 *map)
{
    /* Fix: a channel map has ten bytes; the final task word needs only two. */
    btctrler_hci_cmd_to_task(0x400002, 4, 0x0c3f, read32(map), read32(map + 4), read16(map + 8));
}
void lmp_hci_setup_sync_connection(u16 handle, u32 tx_bandwidth, u32 rx_bandwidth,
                                 u16 max_latency, u16 voice_setting, u8 retran_effort, u16 packet_type)
{
    lmp_hci_cmd_to_conn_for_handle(handle, 5, 0x0428, tx_bandwidth, rx_bandwidth,
                                 (u32)max_latency << 16 | voice_setting,
                                 (u32)retran_effort << 16 | packet_type);
}
void lmp_hci_accept_sco_connection_request(u8 *addr, u32 tx_bandwidth, u32 rx_bandwidth,
                                        u16 max_latency, u16 content_format, u8 retransmission, u16 packet_type)
{
    lmp_hci_cmd_to_conn_for_addr(addr, 5, 0x0429, tx_bandwidth, rx_bandwidth,
                               (u32)max_latency << 16 | content_format,
                               (u32)retransmission << 16 | packet_type);
}

#ifdef UNITTEST
#include <stdlib.h>
#include <stdarg.h>
#include "test.h"
struct bredr_local_device gbredr_local_dev;
static struct lmp_connection conn;
static u8 address[6];
static int found, names, pages, page_repetition, argc_seen, queue_kind;
static u32 words[7];
static void capture(int argc, va_list ap)
{
    CHECK(argc > 0 && argc <= 7); argc_seen = argc;
    for (int i = 0; i < argc; i++) words[i] = va_arg(ap, u32);
}
int btctrler_hci_cmd_to_task(int cmd, int argc, ...)
{
    CHECK(cmd == 0x400002); queue_kind = 1;
    va_list ap; va_start(ap, argc); capture(argc, ap); va_end(ap); return -1;
}
int lmp_hci_cmd_to_conn_for_addr(u8 *addr, int argc, ...)
{
    CHECK(addr == address); queue_kind = 2;
    va_list ap; va_start(ap, argc); capture(argc, ap); va_end(ap); return -1;
}
int lmp_hci_cmd_to_conn_for_handle(u16 handle, int argc, ...)
{
    CHECK(handle == 0x123); queue_kind = 3;
    va_list ap; va_start(ap, argc); capture(argc, ap); va_end(ap); return -1;
}
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(addr == address); return found ? &conn : 0; }
int lmp_tx_name_req(struct lmp_connection *c) { CHECK(c == &conn); names++; return 0; }
void link_page_try_start_enable(u8 name, u8 *addr, u8 repetition, int offset)
{ CHECK(name == 1 && addr == address && !offset); pages++; page_repetition = repetition; }
u16 CRC16(const void *ptr, u32 len)
{ CHECK(ptr == gbredr_local_dev.priv_version && len == 12); return 0xabcd; }
static void expect(int kind, int argc, u32 opcode)
{ CHECK(queue_kind == kind && argc_seen == argc && words[0] == opcode); }
int main(void)
{
    for (int channel = -256; channel < 256; channel++)
        CHECK(get_tws_internal_addr(channel) == (channel == 'R' ? gbredr_local_dev.internal_addr_remote : gbredr_local_dev.internal_addr_local));
    memset(gbredr_local_dev.priv_version, 0xa5, 16);
    const char ic[4] = {1, 2, 3, 4}, version[2] = {5, 6};
    for (unsigned i = 0; i < 6; i++) address[i] = 0x81 + i;
    lmp_hci_write_local_priv_version(ic, version, address);
    CHECK(!memcmp(gbredr_local_dev.priv_version, ic, 4) && !memcmp(gbredr_local_dev.priv_version + 4, version, 2) &&
          !memcmp(gbredr_local_dev.priv_version + 6, address, 6) && gbredr_local_dev.priv_version[12] == 0xcd);
    for (int i = 13; i < 16; i++) CHECK(gbredr_local_dev.priv_version[i] == 0xa5);
    for (found = 0; found < 2; found++) for (int repetition = 0; repetition < 256; repetition++) {
        int old_names = names, old_pages = pages;
        lmp_hci_remote_name_request(address, repetition, 0xffff);
        CHECK(names == old_names + found && pages == old_pages + !found);
        if (!found) CHECK(page_repetition == repetition);
    }
    /* Exact-size BD_ADDR and AFH buffers expose the SDK's final-word overreads. */
    u8 *addr = malloc(6), *map = malloc(10); CHECK(addr && map);
    memcpy(addr, address, 6); for (int i = 0; i < 10; i++) map[i] = 0x91 + i;
    lmp_hci_create_connection(addr, 0xabcd, 2, 0xff, 0xfedc, 0xaa);
    expect(1, 7, 0x0405);
    CHECK(words[1] == read32(addr) && words[2] == read16(addr + 4) && words[3] == 0xabcd &&
          words[4] == 2 && words[5] == 0xfedc && words[6] == 0xaa);
    lmp_hci_cancel_page(); expect(1, 2, 0x0cfd); CHECK(!words[1]);
    lmp_hci_connection_cancel(addr); expect(1, 3, 0x0cfe);
    CHECK(words[1] == read32(addr) && words[2] == read16(addr + 4));
    lmp_hci_inquiry(0x9e8b33, 0xff, 0x80); expect(1, 4, 0x0401);
    CHECK(words[1] == 0x9e8b33 && words[2] == 0xff && words[3] == 0x80);
    lmp_hci_cancel_inquiry(); expect(1, 1, 0x0402);
    lmp_hci_remote_name_request_cus(addr); expect(1, 5, 0x0419);
    CHECK(words[1] == read32(addr) && words[2] == read16(addr + 4) && !words[3] && !words[4]);
    lmp_hci_tx_channel_chassification(map); expect(1, 4, 0x0c3f);
    CHECK(words[1] == read32(map) && words[2] == read32(map + 4) && words[3] == read16(map + 8));
    free(addr); free(map);
    lmp_hci_setup_sync_connection(0x123, 0xfedcba98, 0xabcdef01, 0xffff, 0xabcd, 0xff, 0x8001);
    expect(3, 5, 0x0428);
    CHECK(words[1] == 0xfedcba98 && words[2] == 0xabcdef01 && words[3] == 0xffffabcd && words[4] == 0x00ff8001);
    lmp_hci_accept_sco_connection_request(address, 0xabcdef01, 0xfedcba98, 0x8000, 0xffff, 0x80, 0xffff);
    expect(2, 5, 0x0429);
    CHECK(words[1] == 0xabcdef01 && words[2] == 0xfedcba98 && words[3] == 0x8000ffff && words[4] == 0x0080ffff);
    return TEST_DONE("btctrler_lmp_hci_control");
}
#endif
