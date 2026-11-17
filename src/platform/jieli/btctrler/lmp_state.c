/* The rest of the SDK's lmp.c: the link manager's shared state, the length
 * table for vendor-escape PDUs, the weak hooks the SDK let applications
 * override, and the test-box key and user-info requests.  The firmware
 * update (bt_updata_*, uboot), test-box information and A2DP/eSCO API
 * functions of lmp.c have no caller in this firmware and are gone with it. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_TWS, "lmp.c policy for a standard HCI host without TWS");

/* lmp.c's variables, in the SDK's sections. */
#define LMP_DATA __attribute__((section(".classic_lmp_data")))
#define LMP_BSS  __attribute__((section(".classic_lmp_bss")))
LMP_BSS struct lmp_connection lmp_acl_link[1];
LMP_BSS u8 local_private_key[24];                      /* SSP ECDH private key */
LMP_BSS struct inquiry_scan_parm_t inquiry_scan_parm;
LMP_BSS struct page_scan_parm_t page_scan_parm;
LMP_DATA struct page_parm_t page_parm;
LMP_DATA struct bredr_dut *g_dut_hdl;
LMP_DATA u8 g_lmp_update_start_flag;
LMP_DATA u8 key_special_bind;
LMP_DATA u8 lmp_dut_en, lmp_dut_phone_en;
LMP_DATA void (*lmp_update_rx_handler)(int, struct bredr_rx_bulk *);
LMP_DATA u8 sniff_establish_by_remote;
LMP_DATA struct lmp_connection *main_conn, *other_conn;

/* Weak hooks: the SDK's defaults, for applications to replace. */
__attribute__((weak)) void a2dp_rx_notice_to_decode(void) {}
__attribute__((weak)) int acl_u_packet_analyse(u8 *data, u16 size) { (void)data; (void)size; return 0; }
__attribute__((weak)) void bdhw_enable_afh(struct ctrl_frame *frame) { (void)frame; }
__attribute__((weak)) int wifibox_bt_freq_finish_post(void) { return 0; }
__attribute__((weak)) u8 is_call_now(void) { return 0; }

void delay(unsigned int t);
#define RNG_OUT (*(volatile u32 *)0x1e3b00)
__attribute__((weak)) void get_random_number(u8 *ptr, u8 len)
{
    while (len) {
        u32 r = RNG_OUT;
        u8 n = len < 4 ? len : 4;
        memcpy(ptr, &r, n);
        ptr += n;
        len -= n;
        delay(10);              /* let the generator produce a new word */
    }
}

/* Expected PDU lengths, opcode byte included; the first entry for an opcode
 * decides.  (The SDK lists CLKOFFSET_RES twice, the second time as 16.) */
struct pdu_length { u8 opcode, len; };
static const struct pdu_length pdu_lengths[] = {
    { PDU(ACCEPTED), 2 }, { PDU(AU_RAND), 17 }, { PDU(AUTO_RATE), 1 },
    { PDU(CLKOFFSET_REQ), 1 }, { PDU(CLKOFFSET_RES), 3 }, { PDU(DECR_POWER_REQ), 2 },
    { PDU(DETACH), 2 }, { PDU(DHKEY_CHECK), 17 }, { PDU(ENCAPSULATED_HEADER), 4 },
    { PDU(ENCAPSULATED_PAYLOAD), 17 }, { PDU(ENCRYPTION_KEY_SIZE_MASK_REQ), 1 },
    { PDU(ENCRYPTION_KEY_SIZE_MASK_RES), 3 }, { PDU(ENCRYPTION_KEY_SIZE_REQ), 2 },
    { PDU(ENCRYPTION_MODE_REQ), 2 }, { PDU(CLKOFFSET_RES), 16 }, { PDU(FEATURES_REQ), 9 },
    { PDU(FEATURES_RES), 9 }, { PDU(HOST_CONNECTION_REQ), 1 }, { PDU(MAX_POWER), 1 },
    { PDU(MAX_SLOT), 2 }, { PDU(MAX_SLOT_REQ), 2 }, { PDU(NAME_REQ), 2 }, { PDU(NAME_RES), 17 },
    { PDU(NOT_ACCEPTED), 3 }, { PDU(REMOVE_SCO_LINK_REQ), 3 }, { PDU(SCO_LINK_REQ), 7 },
    { PDU(SETUP_COMPLETE), 1 }, { PDU(SLOT_OFFSET), 9 }, { PDU(SNIFF_REQ), 10 },
    { PDU(START_ENCRYPTION_REQ), 17 }, { PDU(STOP_ENCRYPTION_REQ), 1 }, { PDU(SRES), 5 },
    { PDU(SUPERVISION_TIMEOUT), 3 }, { PDU(TIMING_ACCURACY_REQ), 1 },
    { PDU(TIMING_ACCURACY_RES), 3 }, { PDU(UNSNIFF_REQ), 1 }, { PDU(VERSION_REQ), 6 },
    { PDU(VERSION_RES), 6 },
};
static const struct pdu_length escape3_lengths[] = { { ACCEPTED_EXT, 4 }, { NOT_ACCEPTED_EXT, 5 } };
static const struct pdu_length escape4_lengths[] = {
    { ACCEPTED_EXT, 4 }, { FEATURES_REQ_EXT, 12 }, { FEATURES_RES_EXT, 12 },
    { NOT_ACCEPTED_EXT, 5 }, { PACKET_TYPE_TABLE_REQ, 3 }, { ESCO_LINK_REQ, 16 },
    { REMOVE_ESCO_LINK_REQ, 4 },
};

/* Whether a received PDU has its opcode's length; escapes are looked up by
 * their extended opcode.  Unknown opcodes pass.  Checked only with
 * config_bt_security_vulnerability set (platform_jieli_sdk.c: it is not). */
extern BTCTRLER_CONFIG int config_bt_security_vulnerability;
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
u8 lmp_cmd_is_match_size(u8 opcode, u8 *rx_data, u8 rx_len)
{
    if (!config_bt_security_vulnerability) return 1;
    const struct pdu_length *table = pdu_lengths;
    unsigned count = COUNT(pdu_lengths);
    u8 op = opcode;
    if (opcode == PDU(ESCAPE_3)) { table = escape3_lengths; count = COUNT(escape3_lengths); op = rx_data[1]; }
    if (opcode == PDU(ESCAPE_4)) { table = escape4_lengths; count = COUNT(escape4_lengths); op = rx_data[1]; }
    for (unsigned i = 0; i < count; i++)
        if (table[i].opcode == op) return table[i].len == rx_len;
    return 1;
}

/* Re-create the connection after the SDK's firmware-update reboot
 * (bredr_link.c); not part of a normal connection. */
int __bt_updata_lmp_connection_resume(struct link_connection *lc, u8 role)
{
    struct lmp_connection *conn = lmp_create_conn();
    if (!conn) { btctrler_assert_failed("conn != NULL: update resume", 0); return -12; }
    conn->link = lc;
    conn->core.role = role;
    if (role) {
        conn->role_machine_handler = lmp_slave_machine;
        conn->core.stage = LMP_STAGE_CONN;
        conn->core.step = LMP_WAIT_HOST_CONNECTION_REQ;
    }
    conn->core.state = 1;
    return 0;
}

static u16 l2cap_cid(const struct bredr_rx_bulk *bulk) { return bulk->data[2] | bulk->data[3] << 8; }

/* Whether nothing but A2DP media is pending on a connection (TWS role
 * switch, tws_pair.c): received LMP, or L2CAP on any other channel, is not
 * idle. */
int lmp_conn_idle(struct lmp_connection *conn)
{
    if (!conn || !conn->link) return 1;
    if (conn->busy) return 0;
    struct list_head *head = &conn->link->rx_head;
    for (struct list_head *entry = head->next; entry != head; entry = entry->next) {
        struct bredr_rx_bulk *bulk = (void *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry));
        if (bulk->llid == 3) return 0;
        if (bulk->llid == 2 && l2cap_cid(bulk) != conn->sbc_channel) return 0;
    }
    head = &conn->rx_acl_u;
    for (struct list_head *entry = head->next; entry != head; entry = entry->next) {
        struct bredr_rx_bulk *bulk = (void *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry));
        if (!bulk->delete && l2cap_cid(bulk) != conn->sbc_channel) return 0;
    }
    return 1;
}

/* Jieli test-box requests (escape 3, HCI vendor commands 0x0cfc/0x0cfb). */
void lmp_send_test_key(struct lmp_connection *conn, u8 key)
{
    if (!conn) return;
    if ((key & 0xfc) == 0xf0) {
        u8 info[4] = { 0, 2, 1, (u8)(key + 16) };        /* item 0, state 2, one byte */
        lmp_request(conn, "11LB", PDU(ESCAPE_3), 135, 4, info);
    } else if ((u8)(key + 12) < 5) {
        u8 info[4] = { 1, 2, 1, (u8)(key + 13) };        /* item 1 */
        lmp_request(conn, "11LB", PDU(ESCAPE_3), 135, 4, info);
    } else {
        lmp_request(conn, "111", PDU(ESCAPE_3), 103, key);
    }
}

void lmp_send_user_info(struct lmp_connection *conn, u32 info)
{
    if (conn) lmp_request(conn, "114", PDU(ESCAPE_3), 136, info);
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdlib.h>
#include "test.h"
int config_asser, config_bt_security_vulnerability = 1;
static struct lmp_connection created;
static int create_fails, asserts, requests;
static char format_seen[8];
static int args[4];
static u8 info_seen[4];
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
struct lmp_connection *lmp_create_conn(void) { return create_fails ? 0 : &created; }
int lmp_slave_machine(struct lmp_connection *c, struct bredr_rx_bulk *rx, int *msg) { (void)c; (void)rx; (void)msg; return 0; }
void delay(unsigned int t) { (void)t; }
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    (void)c; requests++; strcpy(format_seen, format);
    va_list ap; va_start(ap, format);
    args[0] = va_arg(ap, int); args[1] = va_arg(ap, int); args[2] = va_arg(ap, int);
    if (!strcmp(format, "11LB")) memcpy(info_seen, va_arg(ap, u8 *), 4);
    va_end(ap); return 0;
}
static struct bredr_rx_bulk *bulk(struct list_head *head, int llid, u16 cid, int del)
{
    struct bredr_rx_bulk *b = calloc(1, sizeof(*b) + 8);
    b->llid = llid; b->delete = del; b->data[2] = (u8)cid; b->data[3] = cid >> 8;
    b->entry.prev = head->prev; b->entry.next = head; head->prev->next = &b->entry; head->prev = &b->entry;
    return b;
}
int main(void)
{
    u8 pdu[3] = { 0 };
    CHECK(lmp_cmd_is_match_size(PDU(DETACH), pdu, 2) && !lmp_cmd_is_match_size(PDU(DETACH), pdu, 3));
    CHECK(lmp_cmd_is_match_size(PDU(CLKOFFSET_RES), pdu, 3) && !lmp_cmd_is_match_size(PDU(CLKOFFSET_RES), pdu, 16));
    CHECK(lmp_cmd_is_match_size(0x7a << 1, pdu, 99));                   /* unknown opcode */
    pdu[1] = FEATURES_RES_EXT;
    CHECK(lmp_cmd_is_match_size(PDU(ESCAPE_4), pdu, 12) && !lmp_cmd_is_match_size(PDU(ESCAPE_4), pdu, 11));
    pdu[1] = NOT_ACCEPTED_EXT;
    CHECK(lmp_cmd_is_match_size(PDU(ESCAPE_3), pdu, 5) && !lmp_cmd_is_match_size(PDU(ESCAPE_3), pdu, 4));
    config_bt_security_vulnerability = 0; CHECK(lmp_cmd_is_match_size(PDU(DETACH), pdu, 9));

    struct link_connection link;
    CHECK(__bt_updata_lmp_connection_resume(&link, 1) == 0);
    CHECK(created.link == &link && created.core.role == 1 && created.core.state == 1);
    CHECK(created.role_machine_handler == lmp_slave_machine && created.core.step == LMP_WAIT_HOST_CONNECTION_REQ);
    memset(&created, 0, sizeof(created));
    CHECK(__bt_updata_lmp_connection_resume(&link, 0) == 0 && !created.role_machine_handler && created.core.state == 1);
    create_fails = 1; CHECK(__bt_updata_lmp_connection_resume(&link, 0) == -12 && asserts == 1);

    struct lmp_connection conn = {0};
    CHECK(lmp_conn_idle(0) && lmp_conn_idle(&conn));
    conn.link = &link; conn.sbc_channel = 0x41;
    link.rx_head.next = link.rx_head.prev = &link.rx_head;
    conn.rx_acl_u.next = conn.rx_acl_u.prev = &conn.rx_acl_u;
    CHECK(lmp_conn_idle(&conn));
    conn.busy = 1; CHECK(!lmp_conn_idle(&conn)); conn.busy = 0;
    bulk(&link.rx_head, 2, 0x41, 0); bulk(&link.rx_head, 1, 0x99, 0); CHECK(lmp_conn_idle(&conn));
    bulk(&conn.rx_acl_u, 2, 0x99, 1); CHECK(lmp_conn_idle(&conn));      /* delivered */
    bulk(&conn.rx_acl_u, 2, 0x99, 0); CHECK(!lmp_conn_idle(&conn));
    conn.rx_acl_u.next = conn.rx_acl_u.prev = &conn.rx_acl_u;
    bulk(&link.rx_head, 3, 0, 0); CHECK(!lmp_conn_idle(&conn));

    lmp_send_test_key(&conn, 0xf2);
    CHECK(!strcmp(format_seen, "11LB") && args[0] == 252 && args[1] == 135 && args[2] == 4);
    CHECK(!memcmp(info_seen, (u8[]){ 0, 2, 1, 2 }, 4));
    lmp_send_test_key(&conn, 0xf8); CHECK(!memcmp(info_seen, (u8[]){ 1, 2, 1, 5 }, 4));
    lmp_send_test_key(&conn, 0xf9); CHECK(!strcmp(format_seen, "111") && args[1] == 103 && args[2] == 0xf9);
    lmp_send_user_info(&conn, 0x12345678); CHECK(!strcmp(format_seen, "114") && args[1] == 136 && args[2] == 0x12345678);
    requests = 0; lmp_send_test_key(0, 1); lmp_send_user_info(0, 1); CHECK(!requests);
    return TEST_DONE("btctrler_lmp_state");
}
#endif
