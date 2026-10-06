/* Pairing state and utility handlers, recovered from SDK lmp.c.
 * Random generation and cryptographic operations remain SDK APIs. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

void make_rand_num(u8 *buf) { get_random_number(buf, 16); }
void make_xor(u8 *rand, u8 *key, u8 *c)
{ for (unsigned i = 0; i < 16; i++) c[i] = rand[i] ^ key[i]; }
int lmp_secure_connections_enbale(struct lmp_connection *conn)
{
    return !!((hci_param.ext_features[0] & 8) && (hci_param.ext_features[9] & 1) &&
              (conn->core_s.remote_ext_features[0] & 8) && (conn->core_s.remote_ext_features[9] & 1));
}
void lmp_rx_not_accepted_encryption_mode_req(struct lmp_connection *conn) { (void)conn; }
void lmp_rx_accepted_encryption_mode_req(struct lmp_connection *conn) { (void)conn; }
void lmp_tx_encryption_key_size_req(struct lmp_connection *conn) { (void)conn; }
void lmp_rx_accepted_encryption_key_size_req(struct lmp_connection *conn) { (void)conn; }
void lmp_tx_resume_encryption_req(struct lmp_connection *conn)
{
    if (!(conn->core.enc_state & 0x1000)) return;
    conn->core.enc_state &= ~0x1000;
    lmp_response(conn, "11", PDU(ESCAPE_4), RESUME_ENCRYPTION_REQ);
}
void lmp_rx_temp_key(struct lmp_connection *conn)
{ hci_send_event(0x0a, "121", 0, conn->core_s.handle, 1); }
void lmp_tx_keypress_notification(struct lmp_connection *conn, u8 key)
{ lmp_response(conn, "111", PDU(ESCAPE_4), KEYPRESS_NOTIFICATION, key); }

void lmp_authenticated_by_using_one_of_three_algorithms(struct lmp_connection *conn)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) { btctrler_assert_failed("simple pairing state", 0); return; }
    sp->simple_protocols = 0xff;
    if (!(sp->local_Authentication_Req & 1) && !(sp->remote_Authentication_Req & 1))
        sp->simple_protocols = 0;
    else if ((sp->local_IO_Capability < 2 && sp->remote_IO_Capability < 2) ||
             sp->local_IO_Capability == 3 || sp->remote_IO_Capability == 3)
        sp->simple_protocols = 0;
    else if (sp->local_IO_Capability == 2 || sp->remote_IO_Capability == 2)
        sp->simple_protocols = 2;
    else if (sp->local_OOB_Data_Present && sp->remote_OOB_Data_Present)
        sp->simple_protocols = 1;
    if (!sp->simple_protocols) sp->passkey_cnt = 0;
    if (sp->simple_protocols == 0xff) btctrler_assert_failed("simple pairing protocol", 0xff);
}
void lmp_io_capability_init(struct lmp_connection *conn, u8 *rx, u8 io_cap)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return;
    sp->remote_IO_Capability = rx[0]; sp->remote_OOB_Data_Present = rx[1];
    sp->remote_Authentication_Req = rx[2];
    sp->remote_public_key_len = sp->simple_protocols = sp->passkey_cnt = 0;
    if (io_cap) lmp_authenticated_by_using_one_of_three_algorithms(conn);
    sp = conn->simple;
    hci_send_event(0x32, "A111", conn->core_s.remote_addr, sp->remote_IO_Capability,
                   sp->remote_OOB_Data_Present, sp->remote_Authentication_Req);
}
void lmp_rx_io_capability_req(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_simple_pair *sp = lmp_malloc(sizeof(*sp));
    if (!sp) { btctrler_assert_failed("simple pairing allocation", 0); return; }
    /* Fix: repeated requests replaced the old allocation without releasing it. */
    if (conn->simple) lmp_free(conn->simple);
    conn->simple = sp; sp->state |= 4;
    lmp_io_capability_init(conn, rx, 0);
    conn->core.stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE;
    conn->core.step = LMP_WAIT_IO_CAPABILITY_REPLY;
    hci_send_event(0x31, "A", conn->core_s.remote_addr);
}
void lmp_rx_pairing_confirm(struct lmp_connection *conn, u8 *rx)
{
    /* Fix: unsolicited pairing PDUs must not dereference absent state. */
    if (!conn->simple) return;
    memcpy(conn->simple->remote_commitment_num, rx, 16);
    conn->simple->state |= 0x10;
}
void lmp_rx_pairing_number(struct lmp_connection *conn, u8 *rx)
{
    if (conn->simple) memcpy(conn->simple->remote_random_num, rx, 16);
}

#ifdef UNITTEST
#include <stdlib.h>
#include <stdarg.h>
#include "test.h"
struct hci_parameter hci_param;
static struct lmp_connection conn;
static struct lmp_simple_pair simple;
static int asserts, allocated, released, allocation_fails, responses, opcode, args[3], events, codes[2], randoms;
static const char *format;
void btctrler_assert_failed(const char *what, int value)
{ CHECK(what && (value == 0 || value == 255)); asserts++; }
void *lmp_malloc(int size)
{ CHECK(size == sizeof(simple)); allocated++; return allocation_fails ? 0 : calloc(1, size); }
void lmp_free(void *p) { CHECK(p && p != &simple); released++; free(p); }
void get_random_number(u8 *p, u8 len)
{ CHECK(len == 16); randoms++; for (unsigned i = 0; i < len; i++) p[i] = i; }
int lmp_response(struct lmp_connection *c, const char *fmt, ...)
{
    CHECK(c == &conn); va_list ap; va_start(ap, fmt);
    format = fmt; opcode = va_arg(ap, int); args[0] = va_arg(ap, int);
    if (!strcmp(fmt, "111")) args[1] = va_arg(ap, int);
    va_end(ap); responses++; return 0;
}
int hci_send_event(int code, const char *fmt, ...)
{
    CHECK(events < 2); codes[events++] = code; format = fmt;
    va_list ap; va_start(ap, fmt);
    if (code == 0x0a) {
        CHECK(!strcmp(fmt, "121") && va_arg(ap, int) == 0 && va_arg(ap, int) == 0x123 && va_arg(ap, int) == 1);
    } else {
        CHECK(va_arg(ap, u8 *) == conn.core_s.remote_addr);
        if (code == 0x32) {
            CHECK(!strcmp(fmt, "A111"));
            for (int i = 0; i < 3; i++) args[i] = va_arg(ap, int);
        } else CHECK(code == 0x31 && !strcmp(fmt, "A"));
    }
    va_end(ap); return 0;
}
int main(void)
{
    conn.simple = &simple; conn.core_s.handle = 0x123;
    u8 random[16], key[16], out[16];
    make_rand_num(random); CHECK(randoms == 1);
    for (int i = 0; i < 16; i++) key[i] = 0x91 + i;
    make_xor(random, key, out);
    for (int i = 0; i < 16; i++) CHECK(out[i] == (random[i] ^ key[i]));
    make_xor(random, key, random);
    for (int i = 0; i < 16; i++) CHECK(random[i] == out[i]);
    for (int mask = 0; mask < 16; mask++) {
        hci_param.ext_features[0] = mask & 1 ? 0x88 : 0x80;
        hci_param.ext_features[9] = mask & 2 ? 0x81 : 0x80;
        conn.core_s.remote_ext_features[0] = mask & 4 ? 0x88 : 0x80;
        conn.core_s.remote_ext_features[9] = mask & 8 ? 0x81 : 0x80;
        CHECK(lmp_secure_connections_enbale(&conn) == (mask == 15));
    }
    lmp_rx_not_accepted_encryption_mode_req(0); lmp_rx_accepted_encryption_mode_req(0);
    lmp_tx_encryption_key_size_req(0); lmp_rx_accepted_encryption_key_size_req(0);
    for (int enc = 0; enc < 65536; enc++) {
        conn.core.enc_state = enc; int before = responses;
        lmp_tx_resume_encryption_req(&conn);
        CHECK(conn.core.enc_state == (enc & ~0x1000) && responses == before + !!(enc & 0x1000));
        if (enc & 0x1000) CHECK(opcode == PDU(ESCAPE_4) && !strcmp(format, "11") && args[0] == RESUME_ENCRYPTION_REQ);
    }
    lmp_rx_temp_key(&conn); CHECK(events == 1 && codes[0] == 0x0a);
    for (int keypress = 0; keypress < 256; keypress++) {
        lmp_tx_keypress_notification(&conn, keypress);
        CHECK(opcode == PDU(ESCAPE_4) && !strcmp(format, "111") && args[0] == KEYPRESS_NOTIFICATION && args[1] == keypress);
    }
    /* Exhaustively preserve the SDK selector's byte-valued capability inputs. */
    for (unsigned local = 0; local < 256; local++) for (unsigned remote = 0; remote < 256; remote++)
        for (unsigned auth = 0; auth < 4; auth++) for (unsigned oob = 0; oob < 4; oob++) {
            simple.local_IO_Capability = local; simple.remote_IO_Capability = remote;
            simple.local_Authentication_Req = auth & 1; simple.remote_Authentication_Req = auth >> 1;
            simple.local_OOB_Data_Present = oob & 1; simple.remote_OOB_Data_Present = oob >> 1;
            simple.passkey_cnt = 17; int before = asserts;
            lmp_authenticated_by_using_one_of_three_algorithms(&conn);
            int expected = !auth || (local < 2 && remote < 2) || local == 3 || remote == 3 ? 0 :
                           local == 2 || remote == 2 ? 2 : oob == 3 ? 1 : 255;
            CHECK(simple.simple_protocols == expected && simple.passkey_cnt == (expected ? 17 : 0));
            CHECK(asserts == before + (expected == 255));
        }
    u8 rx[16]; for (int i = 0; i < 16; i++) rx[i] = i + 3;
    simple.state = 4; lmp_rx_pairing_confirm(&conn, rx);
    CHECK(simple.state == 0x14 && !memcmp(simple.remote_commitment_num, rx, 16));
    lmp_rx_pairing_number(&conn, rx); CHECK(!memcmp(simple.remote_random_num, rx, 16));
    conn.simple = 0; lmp_rx_pairing_confirm(&conn, rx); lmp_rx_pairing_number(&conn, rx);
    int before = asserts; lmp_authenticated_by_using_one_of_three_algorithms(&conn); CHECK(asserts == before + 1);
    conn.simple = &simple; events = 0; simple.remote_public_key_len = simple.simple_protocols = simple.passkey_cnt = 0xff;
    lmp_io_capability_init(&conn, rx, 0);
    CHECK(!simple.remote_public_key_len && !simple.simple_protocols && !simple.passkey_cnt && events == 1 && codes[0] == 0x32);
    CHECK(args[0] == rx[0] && args[1] == rx[1] && args[2] == rx[2]);
    events = 0; simple.local_Authentication_Req = 0; rx[2] = 0;
    lmp_io_capability_init(&conn, rx, 1); CHECK(simple.simple_protocols == 0 && events == 1);
    conn.simple = 0; events = 0;
    lmp_rx_io_capability_req(&conn, rx);
    CHECK(allocated == 1 && !released && conn.simple && conn.simple->state == 4 &&
          conn.core.stage == LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE && conn.core.step == LMP_WAIT_IO_CAPABILITY_REPLY);
    CHECK(events == 2 && codes[0] == 0x32 && codes[1] == 0x31);
    struct lmp_simple_pair *old = conn.simple; allocation_fails = 1; events = 0;
    lmp_rx_io_capability_req(&conn, rx); CHECK(conn.simple == old && !released && !events);
    allocation_fails = 0; events = 0;
    lmp_rx_io_capability_req(&conn, rx); CHECK(released == 1 && conn.simple && events == 2);
    lmp_free(conn.simple); conn.simple = 0;
    return TEST_DONE("btctrler_lmp_pairing_support");
}
#endif
