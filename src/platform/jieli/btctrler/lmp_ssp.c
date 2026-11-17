/* Secure Simple Pairing key exchange and confirmations, from SDK lmp.c.
 * P-192, f1, f3, numeric comparison and entropy remain SDK implementations. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

uECC_Curve uECC_secp192r1(void);

void lmp_ecdh_publickey(struct lmp_connection *conn)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return;
    sp->encapsulated_major_type = sp->encapsulated_minor_type = 1;
    sp->encapsulated_playload_length = sizeof(sp->local_public_key);
    link_conn_follow_ctrl_enable(conn->link);
    get_random_number(local_private_key, sizeof(local_private_key));
    uECC_compute_public_key_api(local_private_key, conn->simple->local_public_key, uECC_secp192r1());
    endian_change(conn->simple->local_public_key, 24);
    endian_change(conn->simple->local_public_key + 24, 24);
    conn->simple->local_public_key_len = 0;
    link_conn_follow_ctrl_disable(conn->link);
}
void lmp_rx_encapsulated_payload(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return;
    unsigned offset = sp->remote_public_key_len;
    /* Fix: the SDK appended without checking its 48-byte public-key buffer,
     * then used the advertised length for a variable-sized stack allocation. */
    if (sp->encapsulated_playload_length != sizeof(sp->remote_public_key) || offset > 32 || offset % 16) {
        lmp_tx_detch(conn, 0x05);
        return;
    }
    memcpy(sp->remote_public_key + offset, rx, 16);
    sp->remote_public_key_len = offset + 16;
    lmp_response(conn, "11", PDU(ACCEPTED), ENCAPSULATED_PAYLOAD);
    sp = conn->simple;
    if (sp->remote_public_key_len != sp->encapsulated_playload_length) return;
    u8 public_key[48], result = 0;
    btctler_reverse_bytes(sp->remote_public_key, public_key, 24);
    btctler_reverse_bytes(sp->remote_public_key + 24, public_key + 24, 24);
    uECC_shared_secret_api(public_key, local_private_key, conn->simple->local_ecdh_key, uECC_secp192r1(), &result);
    if (!result) { btctrler_assert_failed("shared secret", 0); return; }
    endian_change(conn->simple->local_ecdh_key, 24);
}
void lmp_f3_function(struct lmp_connection *conn, u8 *result)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return;
    u8 io_cap[3] = {sp->local_IO_Capability, sp->local_OOB_Data_Present, sp->local_Authentication_Req};
    u8 r[16] = {0};
    if (sp->simple_protocols == 2) {
        for (unsigned i = 0; i < 4; i++) r[i] = sp->passkey >> (8 * i);
    }
    f3_function_api(sp->local_ecdh_key, sp->local_random_num, sp->remote_random_num, r, io_cap,
                    hci_param.addr, conn->core_s.remote_addr, result);
}
void master_first_dhkey_check(struct lmp_connection *conn)
{
    if (!conn->simple) return;
    u8 result[16]; lmp_f3_function(conn, result);
    lmp_request(conn, "1C016", PDU(DHKEY_CHECK), result);
}
void lmp_dhkey_check_accept(struct lmp_connection *conn)
{
    if (!conn->simple) return;
    u8 result[16]; lmp_f3_function(conn, result);
    lmp_response(conn, "11", PDU(ACCEPTED), DHKEY_CHECK);
    lmp_response(conn, "1C016", PDU(DHKEY_CHECK), result);
}
int lmp_master_check_c(struct lmp_connection *conn)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return 0;
    u8 z = 0, commitment[16];
    if (sp->simple_protocols == 2) {
        /* Fix: SSP passkey exchange has 20 rounds; unchecked shifts were UB. */
        if (sp->passkey_cnt >= 20) { lmp_tx_detch(conn, 0x05); return 0; }
        z = 0x80 | ((sp->passkey >> sp->passkey_cnt) & 1);
        sp->passkey_cnt++;
    }
    f1_function_api(sp->remote_public_key, sp->local_public_key, sp->remote_random_num, &z, commitment);
    if (memcmp(commitment, conn->simple->remote_commitment_num, 16)) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 0x05);
        return 0;
    }
    lmp_response(conn, "11", PDU(ACCEPTED), SIMPLE_PAIRING_NUMBER);
    sp = conn->simple;
    if (sp->simple_protocols != 2) {
        u32 number = g_function_api(sp->local_public_key, sp->remote_public_key,
                                    sp->local_random_num, sp->remote_random_num);
        hci_send_event(0x33, "A4", conn->core_s.remote_addr, number);
        return 3;
    }
    if (sp->passkey_cnt > 19) {
        sp->passkey_cnt = 0;
        master_first_dhkey_check(conn);
        return 1;
    }
    z = 0x80 | ((sp->passkey >> sp->passkey_cnt) & 1);
    make_rand_num(sp->local_random_num);
    sp = conn->simple;
    f1_function_api(sp->local_public_key, sp->remote_public_key, sp->local_random_num, &z, sp->local_commitment_num);
    lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_CONFIRM), conn->simple->local_commitment_num);
    return 2;
}
void lmp_tx_user_passkey_request_reply(struct lmp_connection *conn, u8 *cmd)
{
    struct lmp_simple_pair *sp = conn->simple;
    if (!sp) return;
    sp->passkey = cmd[0] | (u32)cmd[1] << 8 | (u32)cmd[2] << 16 | (u32)cmd[3] << 24;
    if (sp->simple_protocols != 2) return;
    if (sp->passkey_cnt >= 20) { lmp_tx_detch(conn, 0x05); return; }
    make_rand_num(sp->local_random_num);
    u8 z = 0x80 | ((sp->passkey >> sp->passkey_cnt) & 1);
    if (sp->simple_protocols == 2)
        f1_function_api(sp->local_public_key, sp->remote_public_key, sp->local_random_num, &z, sp->local_commitment_num);
    lmp_response(conn, "1c16", PDU(SIMPLE_PAIRING_CONFIRM), sp->local_commitment_num);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct hci_parameter hci_param;
u8 local_private_key[24];
static struct lmp_connection conn;
static struct lmp_simple_pair simple;
static struct link_connection link;
static int curve_storage, follows, randoms, public_calls, secret_calls, reversals, endians;
static int crypto_success = 1, asserts, detaches, f1s, f3s, numbers, outputs, reqs;
static int ops[4], scalars[4][2], zs[2];
static u8 packets[4][16];
static const char *formats[4];
uECC_Curve uECC_secp192r1(void) { return (uECC_Curve)&curve_storage; }
void link_conn_follow_ctrl_enable(struct link_connection *lc) { CHECK(lc == &link && !follows); follows = 1; }
void link_conn_follow_ctrl_disable(struct link_connection *lc) { CHECK(lc == &link && follows); follows = 0; }
void get_random_number(u8 *p, u8 len)
{ CHECK(follows && p == local_private_key && len == 24); randoms++; memset(p, 0xab, len); }
void make_rand_num(u8 *p)
{ CHECK(p == simple.local_random_num); randoms++; memset(p, 0xcd, 16); }
int uECC_compute_public_key_api(const uint8_t *priv, uint8_t *pub, uECC_Curve curve)
{
    CHECK(follows && priv == local_private_key && pub == simple.local_public_key && curve == uECC_secp192r1());
    for (int i = 0; i < 24; i++) CHECK(priv[i] == 0xab);
    for (int i = 0; i < 48; i++) pub[i] = i;
    public_calls++; return 0; /* This SDK API reports success via its output, not return value. */
}
void btctler_reverse_bytes(const uint8_t *src, uint8_t *dst, int len)
{ CHECK(len == 24); for (int i = 0; i < len; i++) dst[i] = src[len - i - 1]; reversals++; }
void endian_change(uint8_t *p, uint16_t len)
{
    CHECK(len == 24);
    for (int i = 0; i < len / 2; i++) { u8 tmp = p[i]; p[i] = p[len - 1 - i]; p[len - 1 - i] = tmp; }
    endians++;
}
int uECC_shared_secret_api(const uint8_t *pub, const uint8_t *priv, uint8_t *secret, uECC_Curve curve, u8 *result)
{
    CHECK(priv == local_private_key && secret == simple.local_ecdh_key && curve == uECC_secp192r1());
    for (int half = 0; half < 2; half++) for (int i = 0; i < 24; i++)
        CHECK(pub[half * 24 + i] == simple.remote_public_key[half * 24 + 23 - i]);
    for (int i = 0; i < 24; i++) secret[i] = 0x60 + i;
    *result = crypto_success; secret_calls++; return 0;
}
void btctrler_assert_failed(const char *what, int value) { CHECK(what && !value); asserts++; }
void lmp_tx_detch(struct lmp_connection *c, u8 reason) { CHECK(c == &conn && reason == 5); detaches++; }
void f1_function_api(u8 *u, u8 *v, u8 *x, u8 *z, u8 *result)
{
    CHECK(f1s < 2);
    if (u == simple.remote_public_key) CHECK(v == simple.local_public_key && x == simple.remote_random_num);
    else CHECK(u == simple.local_public_key && v == simple.remote_public_key && x == simple.local_random_num);
    zs[f1s++] = *z; for (int i = 0; i < 16; i++) result[i] = 0x11 + i;
}
void f3_function_api(u8 *w, u8 *n1, u8 *n2, u8 *r, u8 *io, u8 *a1, u8 *a2, u8 *result)
{
    CHECK(w == simple.local_ecdh_key && n1 == simple.local_random_num && n2 == simple.remote_random_num);
    CHECK(a1 == hci_param.addr && a2 == conn.core_s.remote_addr);
    CHECK(io[0] == simple.local_IO_Capability && io[1] == simple.local_OOB_Data_Present && io[2] == simple.local_Authentication_Req);
    for (int i = 0; i < 16; i++) {
        u8 expected = simple.simple_protocols == 2 && i < 4 ? simple.passkey >> (8 * i) : 0;
        CHECK(r[i] == expected); result[i] = 0xa0 + i;
    }
    f3s++;
}
u32 g_function_api(u8 *u, u8 *v, u8 *x, u8 *y)
{
    CHECK(u == simple.local_public_key && v == simple.remote_public_key && x == simple.local_random_num && y == simple.remote_random_num);
    numbers++; return 0xfedcba98;
}
int hci_send_event(int code, const char *format, ...)
{
    CHECK(code == 0x33 && !strcmp(format, "A4")); va_list ap; va_start(ap, format);
    CHECK(va_arg(ap, u8 *) == conn.core_s.remote_addr && va_arg(ap, u32) == 0xfedcba98);
    va_end(ap); return 0;
}
static void capture(const char *fmt, va_list ap)
{
    CHECK(outputs < 4); formats[outputs] = fmt; ops[outputs] = va_arg(ap, int);
    if (!strcmp(fmt, "1C016") || !strcmp(fmt, "1c16")) memcpy(packets[outputs], va_arg(ap, u8 *), 16);
    else {
        scalars[outputs][0] = va_arg(ap, int);
        if (!strcmp(fmt, "111")) scalars[outputs][1] = va_arg(ap, int);
        else CHECK(!strcmp(fmt, "11"));
    }
    outputs++;
}
int lmp_request(struct lmp_connection *c, const char *fmt, ...)
{ CHECK(c == &conn); va_list ap; va_start(ap, fmt); capture(fmt, ap); va_end(ap); reqs++; return 0; }
int lmp_response(struct lmp_connection *c, const char *fmt, ...)
{ CHECK(c == &conn); va_list ap; va_start(ap, fmt); capture(fmt, ap); va_end(ap); return 0; }
static void reset(void)
{
    memset(&simple, 0, sizeof(simple)); conn.simple = &simple; conn.link = &link;
    follows = randoms = public_calls = secret_calls = reversals = endians = asserts = detaches = 0;
    f1s = f3s = numbers = outputs = reqs = 0; crypto_success = 1;
}
int main(void)
{
    reset(); lmp_ecdh_publickey(&conn);
    CHECK(!follows && randoms == 1 && public_calls == 1 && endians == 2 && !simple.local_public_key_len);
    CHECK(simple.encapsulated_major_type == 1 && simple.encapsulated_minor_type == 1 && simple.encapsulated_playload_length == 48);
    for (int half = 0; half < 2; half++) for (int i = 0; i < 24; i++) CHECK(simple.local_public_key[half * 24 + i] == half * 24 + 23 - i);
    reset(); simple.encapsulated_playload_length = 48;
    for (unsigned fragment = 0; fragment < 3; fragment++) {
        u8 rx[16]; for (int i = 0; i < 16; i++) rx[i] = fragment * 16 + i;
        lmp_rx_encapsulated_payload(&conn, rx);
        CHECK(simple.remote_public_key_len == (fragment + 1) * 16 && outputs == (int)fragment + 1);
        CHECK(ops[fragment] == PDU(ACCEPTED) && scalars[fragment][0] == ENCAPSULATED_PAYLOAD);
        CHECK(secret_calls == (fragment == 2) && reversals == (fragment == 2 ? 2 : 0));
    }
    for (int i = 0; i < 48; i++) CHECK(simple.remote_public_key[i] == i);
    for (int i = 0; i < 24; i++) CHECK(simple.local_ecdh_key[i] == 0x60 + 23 - i);
    /* Every byte-valued length/offset pair, including the fourth fragment. */
    for (int length = 0; length < 256; length++) for (int offset = 0; offset < 256; offset++) {
        reset(); simple.encapsulated_playload_length = length; simple.remote_public_key_len = offset;
        struct lmp_simple_pair before = simple; u8 rx[16] = {0};
        lmp_rx_encapsulated_payload(&conn, rx);
        int valid = length == 48 && (offset == 0 || offset == 16 || offset == 32);
        CHECK(detaches == !valid && outputs == valid && secret_calls == (valid && offset == 32));
        if (!valid) CHECK(!memcmp(&before, &simple, sizeof(simple)));
    }
    reset(); simple.encapsulated_playload_length = 48; simple.remote_public_key_len = 32; crypto_success = 0;
    u8 rx[16] = {0}; lmp_rx_encapsulated_payload(&conn, rx);
    CHECK(secret_calls == 1 && asserts == 1 && !endians);
    for (int protocol = 0; protocol < 256; protocol++) {
        reset(); simple.simple_protocols = protocol; simple.passkey = 0xfedcba98;
        simple.local_IO_Capability = 0xaa; simple.local_OOB_Data_Present = 0xbb; simple.local_Authentication_Req = 0xcc;
        u8 result[16]; lmp_f3_function(&conn, result); CHECK(f3s == 1);
        for (int i = 0; i < 16; i++) CHECK(result[i] == 0xa0 + i);
    }
    reset(); master_first_dhkey_check(&conn);
    CHECK(f3s == 1 && reqs == 1 && outputs == 1 && ops[0] == PDU(DHKEY_CHECK) && !strcmp(formats[0], "1C016"));
    reset(); lmp_dhkey_check_accept(&conn);
    CHECK(f3s == 1 && !reqs && outputs == 2 && ops[0] == PDU(ACCEPTED) && scalars[0][0] == DHKEY_CHECK && ops[1] == PDU(DHKEY_CHECK));
    for (int i = 0; i < 16; i++) CHECK(packets[1][i] == 0xa0 + i);
    for (int protocol = 0; protocol < 3; protocol++) for (int matches = 0; matches < 2; matches++)
        for (int round = 0; round < 256; round++) {
            reset(); simple.simple_protocols = protocol; simple.passkey_cnt = round; simple.passkey = 0xabcde;
            for (int i = 0; i < 16; i++) simple.remote_commitment_num[i] = matches ? 0x11 + i : 0;
            int result = lmp_master_check_c(&conn);
            if (protocol == 2 && round >= 20) { CHECK(!result && detaches == 1 && !f1s && !outputs); continue; }
            CHECK(zs[0] == (protocol == 2 ? 0x80 | ((simple.passkey >> round) & 1) : 0));
            if (!matches) {
                CHECK(!result && outputs == 1 && ops[0] == PDU(NOT_ACCEPTED) && scalars[0][0] == SIMPLE_PAIRING_NUMBER && scalars[0][1] == 5);
                continue;
            }
            CHECK(ops[0] == PDU(ACCEPTED) && scalars[0][0] == SIMPLE_PAIRING_NUMBER);
            if (protocol != 2) CHECK(result == 3 && numbers == 1 && f1s == 1 && outputs == 1);
            else if (round == 19) CHECK(result == 1 && !simple.passkey_cnt && f3s == 1 && reqs == 1 && ops[1] == PDU(DHKEY_CHECK));
            else CHECK(result == 2 && simple.passkey_cnt == round + 1 && randoms == 1 && f1s == 2 && zs[1] == (0x80 | ((simple.passkey >> (round + 1)) & 1)) && ops[1] == PDU(SIMPLE_PAIRING_CONFIRM));
        }
    u8 cmd[4] = {0xde, 0xbc, 0x0a, 0};
    for (int protocol = 0; protocol < 3; protocol++) for (int round = 0; round < 256; round++) {
        reset(); simple.simple_protocols = protocol; simple.passkey_cnt = round;
        lmp_tx_user_passkey_request_reply(&conn, cmd); CHECK(simple.passkey == 0xabcde);
        if (protocol != 2) CHECK(!f1s && !outputs && !randoms && !detaches);
        else if (round >= 20) CHECK(detaches == 1 && !f1s && !outputs && !randoms);
        else CHECK(f1s == 1 && randoms == 1 && outputs == 1 && zs[0] == (0x80 | ((simple.passkey >> round) & 1)) && ops[0] == PDU(SIMPLE_PAIRING_CONFIRM) && !strcmp(formats[0], "1c16"));
    }
    reset(); conn.simple = 0;
    lmp_ecdh_publickey(&conn); lmp_rx_encapsulated_payload(&conn, rx); lmp_f3_function(&conn, rx);
    master_first_dhkey_check(&conn); lmp_dhkey_check_accept(&conn); lmp_tx_user_passkey_request_reply(&conn, cmd);
    CHECK(lmp_master_check_c(&conn) == 0 && !randoms && !public_calls && !secret_calls && !f1s && !f3s && !outputs);
    return TEST_DONE("btctrler_lmp_ssp");
}
#endif
