/* Owned lmp_encryption.c: authentication (legacy PIN pairing, link keys,
 * Secure Simple Pairing either side starts) and encryption start, stop,
 * pause, resume and refresh.  Numeric comparison only: passkey entry and
 * OOB are disabled in this firmware (config.h). */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_TWS && !BTCFG_PASSKEY,
               "written for a standard HCI host, no TWS, no passkey entry");
#ifndef UNITTEST
_Static_assert(sizeof(struct lmp_simple_pair) == 208 && sizeof(struct lmp_encrypt) == 69,
               "the SDK's allocation sizes");
#endif

/* lmp_core_data.enc_state */
#define ENC_HAVE_LINK_KEY    (1u << 0)
#define ENC_NEW_LINK_KEY     (1u << 1)
#define ENC_ON               (1u << 2)
#define ENC_PEER_MODE_REQ    (1u << 5)      /* the peer sent LMP_encryption_mode_req */
#define ENC_PAUSED           (1u << 6)
#define ENC_PEER_AU_RAND     (1u << 7)      /* the peer authenticates us */
#define ENC_LINK_KEY_REQ     (1u << 8)
#define ENC_AUTH_AFTER_SSP   (1u << 10)
#define ENC_STOPPED          (1u << 12)
#define ENC_SSP_DONE         (1u << 13)
/* lmp_core_data.state */
#define STATE_AUTHENTICATED  (1u << 4)

static struct lmp_encrypt *encrypt_alloc(struct lmp_connection *conn)
{
    if (!conn->encrypt) {
        conn->encrypt = lmp_malloc(sizeof(struct lmp_encrypt));
        if (!conn->encrypt) btctrler_assert_failed("conn->encrypt != NULL", 0);
    }
    return conn->encrypt;
}

/* SRES and the ciphering offset (COF) from E1. */
static void sres_and_cof(struct lmp_connection *conn, u8 *address, u8 *rand_num)
{
    u8 result[16];
    function_E1_api(address, rand_num, conn->core.link_key, result);
    memcpy(conn->encrypt->sres, result, 4);
    memcpy(conn->core.cof, result + 4, 12);
}

void lmp_free_encrypt(struct lmp_connection *conn)
{
    if (conn->encrypt) {
        lmp_free(conn->encrypt);
        conn->encrypt = 0;
    }
}

/* Is Secure Simple Pairing supported (controller and host) on both sides? */
int lmp_simple_pair_enbale(struct lmp_connection *conn)
{
    return (hci_param.features[6] & 8) && (hci_param.ext_features[0] & 1) &&
           (conn->core_s.remote_features[6] & 8) && (conn->core_s.remote_ext_features[0] & 1);
}

void edr_vendor_keep_connect_for_keymiss(u8 enable)
{
    key_special_bind = enable;
}

/* ---- legacy PIN pairing ---------------------------------------------- */

void lmp_rx_in_rand(struct lmp_connection *conn, u8 *rand)
{
    memcpy(encrypt_alloc(conn)->rand_num, rand, 16);
}

void lmp_tx_in_rand(struct lmp_connection *conn, u8 *pin_code, u8 pin_code_len)
{
    u8 rand_num[16];
    make_rand_num(rand_num);
    lmp_request(conn, "1C016", PDU(IN_RAND), rand_num);
    function_E22_api(pin_code, rand_num, pin_code_len, conn->core_s.remote_addr, conn->encrypt->init_key);
}

void lmp_in_rand_res(struct lmp_connection *conn, u8 *pin_code, u8 pin_code_len)
{
    if (conn->core.role == 0) conn->tran_id |= 1;
    else conn->tran_id &= ~1;
    lmp_response(conn, "11", PDU(ACCEPTED), IN_RAND);
    function_E22_api(pin_code, conn->encrypt->rand_num, pin_code_len, hci_param.addr,
                     conn->encrypt->init_key);
}

void lmp_rx_not_accepted_in_rand(struct lmp_connection *conn)
{
    lmp_tx_detch(conn, 0x13);
}

void lmp_rx_comb_key(struct lmp_connection *conn, u8 *rx)
{
    (void)rx;
    encrypt_alloc(conn);
}

void lmp_tx_comb_key(struct lmp_connection *conn)
{
    u8 rand_num[16];
    struct lmp_encrypt *e = conn->encrypt;
    if (conn->core.enc_state & ENC_HAVE_LINK_KEY) memcpy(e->init_key, conn->core.link_key, 16);
    make_rand_num(rand_num);
    function_E21_api(hci_param.addr, rand_num, e->lk_k);
    make_xor(rand_num, e->init_key, e->c);
    lmp_request(conn, "1C016", PDU(COMB_KEY), e->c);
}

/* Answer the peer's LMP_comb_key and derive the combination link key. */
void lmp_response_comb_key(struct lmp_connection *conn, u8 *rx)
{
    u8 lk_k_r[16], lk_k_l[16], lk_rand[16], rand_num[16];
    make_rand_num(rand_num);
    make_xor(rand_num, conn->encrypt->init_key, conn->encrypt->c);
    lmp_response(conn, "1C016", PDU(COMB_KEY), conn->encrypt->c);
    make_xor(rx, conn->encrypt->init_key, lk_rand);
    function_E21_api(conn->core_s.remote_addr, lk_rand, lk_k_r);
    function_E21_api(hci_param.addr, rand_num, lk_k_l);
    make_xor(lk_k_l, lk_k_r, conn->core.link_key);
    conn->core.enc_state |= ENC_HAVE_LINK_KEY;
}

/* Legacy pairing we started (central). */
int lmp_stage_auth_with_pin_code(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                 struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 lk_k[16], lk_rand[16];
    (void)opcode2;
    switch (conn->core.step) {
    case LMP_TX_PIN_CODE_REQ:
        hci_send_event(0x16, "A", conn->core_s.remote_addr);       /* PIN Code Request */
        conn->core.step = LMP_WAIT_PIN_CODE_REQ_REPLY;
        break;
    case LMP_WAIT_PIN_CODE_REQ_REPLY:
        if (hci_cmd[0] == 0x040d) {                                /* PIN Code Request Reply */
            conn->encrypt = lmp_malloc(sizeof(struct lmp_encrypt));
            if (!conn->encrypt) btctrler_assert_failed("conn->encrypt", 0);
            lmp_tx_in_rand(conn, (u8 *)(hci_cmd + 2), (u8)hci_cmd[1]);
            conn->core.step = LMP_WAIT_ACCEPT_IN_RAND;
        } else if (hci_cmd[0] == 0x040e) {                         /* negative reply */
            lmp_tx_detch(conn, 0x06);
        }
        break;
    case LMP_WAIT_ACCEPT_IN_RAND:
        if (opcode1 != PDU(IN_RAND)) break;
        if (opcode0 == PDU(NOT_ACCEPTED)) {
            lmp_tx_detch(conn, 0x13);
            break;
        }
        /* fall through */
    case LMP_TX_COMB_KEY:
        lmp_tx_comb_key(conn);
        conn->core.step = LMP_WAIT_COMB_KEY;
        break;
    case LMP_WAIT_COMB_KEY: {
        if (opcode0 != PDU(COMB_KEY)) break;
        struct lmp_encrypt *e = conn->encrypt;
        make_rand_num(e->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), e->rand_num);
        make_xor(rx->data + 1, e->init_key, lk_rand);
        function_E21_api(conn->core_s.remote_addr, lk_rand, lk_k);
        make_xor(e->lk_k, lk_k, conn->core.link_key);
        sres_and_cof(conn, conn->core_s.remote_addr, e->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        conn->core.enc_state |= ENC_HAVE_LINK_KEY;
        break;
    }
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES) || conn->encrypt->sres_mach == LMP_SRES_NOT_MACH) break;
        conn->core.step = LMP_WAIT_AU_RAND;
        break;
    case LMP_WAIT_AU_RAND:
        if (opcode0 != PDU(AU_RAND)) break;
        conn->core.state |= STATE_AUTHENTICATED;
        lmp_send_event_auth_complete(conn, 0);
        conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
        lmp_send_event_link_key_notification(conn, 0);
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

/* ---- authentication with a link key ----------------------------------- */

void lmp_tx_au_rand(struct lmp_connection *conn)
{
    u8 rand_num[16];
    make_rand_num(rand_num);
    lmp_request(conn, "1C016", PDU(AU_RAND), rand_num);
}

void __calc_and_send_sres(struct lmp_connection *conn, u8 *rand_num)
{
    sres_and_cof(conn, hci_param.addr, rand_num);
    lmp_response(conn, "1C004", PDU(SRES), conn->encrypt->sres);
}

void lmp_tx_encryption_mode_req(struct lmp_connection *conn)
{
    lmp_send_aclu_en(conn, 0);
    lmp_request(conn, "11", PDU(ENCRYPTION_MODE_REQ), 1);
}

/* 200 ms after authentication: ask for encryption unless the peer did. */
void lmp_tx_encryption_mode_req_dly(struct lmp_connection *conn)
{
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    if (conn->core.enc_state & (ENC_ON | ENC_PEER_MODE_REQ)) return;
    lmp_tx_encryption_mode_req(conn);
    conn->core.step = LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ;
    if (conn->core.role == 0) conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
}

static void restart_encryption_timer(struct lmp_connection *conn)
{
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    conn->enc_timer = sys_timeout_add(conn, (void (*)(void *))lmp_tx_encryption_mode_req_dly, 200);
}

/* The peer authenticates us: answer now if we have the key. */
void lmp_rx_au_rand(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_encrypt *e = encrypt_alloc(conn);
    restart_encryption_timer(conn);
    memcpy(e->rand_num, rx, 16);
    u16 was = conn->core.enc_state;
    conn->core.enc_state = was | ENC_PEER_AU_RAND;
    if (!(was & (ENC_HAVE_LINK_KEY | ENC_ON))) return;     /* answered once the host gives the key */
    __calc_and_send_sres(conn, conn->encrypt->rand_num);
    if ((conn->core.enc_state & ENC_ON) && conn->encrypt) {
        /* Re-authentication while encrypted: authenticate the peer too. */
        make_rand_num(conn->encrypt->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        conn->core.stage = LMP_STAGE_CALCULATE_LINK_KEY;
    }
}

void lmp_rx_sres(struct lmp_connection *conn, u8 *rx)
{
    restart_encryption_timer(conn);
    sres_and_cof(conn, conn->core_s.remote_addr, conn->encrypt->rand_num);
    if (!memcmp(conn->encrypt->sres, rx, 4)) {
        conn->encrypt->sres_mach = LMP_SRES_MACH;
    } else {
        conn->encrypt->sres_mach = LMP_SRES_NOT_MACH;
        conn->err_code = 0x05;
        lmp_send_event_auth_complete(conn, 0x05);
    }
}

void lmp_rx_not_accept_au_rand(struct lmp_connection *conn, u8 *rx)
{
    switch (rx[1]) {
    case 0x23:      /* LMP error transaction collision: take it as done */
        conn->encrypt->sres_mach = LMP_SRES_MACH;
        lmp_send_event_auth_complete(conn, 0);
        break;
    case 0x06:      /* key missing: pair again if the app allows */
        if (key_special_bind) {
            int hci_cmd = 12;
            conn->core.step = LMP_TX_IO_CAPABILITY_REQ;
            conn->core.stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL;
            lmp_stage_auth_with_simple_pair_by_local(conn, 0, 0, 0, 0, &hci_cmd);
            break;
        }
        /* fall through */
    default:
        lmp_send_event_auth_complete(conn, rx[1]);
        break;
    }
}

/* We authenticate the peer with the host's link key. */
int lmp_stage_auth_with_link_key_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                          u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode1; (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_TX_AU_RAND:
        if (conn->role_switch_state == 1) break;   /* after the role switch */
        encrypt_alloc(conn);
        make_rand_num(conn->encrypt->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), conn->encrypt->rand_num);
        sres_and_cof(conn, conn->core_s.remote_addr, conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES)) break;
        if (conn->encrypt->sres_mach == LMP_SRES_NOT_MACH) {
            lmp_send_event_connection_complete(conn, 0x05, conn->core_s.handle, 1);
            lmp_tx_detch(conn, 0x05);
            break;
        }
        conn->core.step = LMP_WAIT_AU_RAND;
        /* fall through */
    case LMP_WAIT_AU_RAND:
        if (conn->core.enc_state & ENC_SSP_DONE) {
            if (opcode0 != PDU(AU_RAND)) break;
            lmp_send_event_link_key_notification(conn, 0);
        } else {
            if (opcode0 != PDU(SRES)) break;
            if (conn->encrypt->sres_mach != LMP_SRES_MACH) btctrler_assert_failed("sres", 0);
        }
        lmp_send_event_auth_complete(conn, 0);
        if (!(conn->core.enc_state & ENC_ON)) {
            conn->core.state |= STATE_AUTHENTICATED;
            if (conn->core.role == 1) {
                conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
                conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
                if (conn->enc_timer) {
                    sys_timer_del(conn->enc_timer);
                    conn->enc_timer = 0;
                }
                conn->enc_timer = sys_timer_add(conn, (void (*)(void *))lmp_tx_encryption_mode_req_dly, 200);
            } else {
                conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
                conn->core.step = LMP_TX_ENCRYPTION_MODE_REQ;
            }
        }
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

/* The peer authenticates us with the host's link key. */
int lmp_stage_auth_with_link_key_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                           u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode1; (void)opcode2; (void)rx;
    switch (conn->core.step) {
    case LMP_TX_HCI_EVENT_LINK_KEY_REQ:
        lmp_send_event_link_request(conn);
        conn->core.step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        break;
    case LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY:
        if (hci_cmd[0] == 0x040b) {                     /* Link Key Request Reply */
            u16 enc = conn->core.enc_state |= ENC_HAVE_LINK_KEY;
            memcpy(conn->core.link_key, hci_cmd + 1, 16);
            if (enc & ENC_AUTH_AFTER_SSP) {
                lmp_tx_au_rand(conn);
                conn->core.step = LMP_WAIT_SRES;
                break;
            }
            if (enc & ENC_NEW_LINK_KEY) lmp_send_event_link_key_notification(conn, 0);
            conn->tran_id = conn->core.role == 0;
            __calc_and_send_sres(conn, conn->encrypt->rand_num);
            conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
            conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
            lmp_free_encrypt(conn);
            return 1;
        }
        if (hci_cmd[0] == 0x040c) {                     /* negative reply: key missing */
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), AU_RAND, 0x06);
            conn->core.step = LMP_WAIT_AUTH;
            conn->core.stage = LMP_STAGE_CONN;
            if (conn->enc_timer) {
                sys_timer_del(conn->enc_timer);
                conn->enc_timer = 0;
            }
        }
        break;
    case LMP_TX_SRES:
        conn->tran_id = conn->core.role == 0;
        __calc_and_send_sres(conn, conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
        lmp_free_encrypt(conn);
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES)) break;
        if (conn->encrypt->sres_mach == LMP_SRES_MACH) {
            conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
            conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
        }
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

/* After pairing as peripheral (SSP or PIN): authenticate the peer once more. */
int lmp_stage_calc_link_key(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                            struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode1; (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_WAIT_AU_RAND:
        if (opcode0 != PDU(AU_RAND)) break;
        make_rand_num(conn->encrypt->rand_num);
        lmp_response(conn, "1c16", PDU(AU_RAND), conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES)) break;
        if (conn->encrypt->sres_mach != LMP_SRES_MACH) {
            lmp_tx_detch(conn, 0x05);
            break;
        }
        conn->core.state |= STATE_AUTHENTICATED;
        lmp_send_event_link_key_notification(conn, 0);
        conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

/* ---- Secure Simple Pairing -------------------------------------------- */

/* DHKey check (f3) on the peer's value; marks SSP done. */
int lmp_dhkey_check(struct lmp_connection *conn, u8 *rx)
{
    u8 remote_confirmation[16], r[16] = {0}, io_cap[3], result[16];
    struct lmp_simple_pair *sp = conn->simple;
    memcpy(remote_confirmation, rx, 16);
    io_cap[0] = sp->remote_IO_Capability;
    io_cap[1] = sp->remote_OOB_Data_Present;
    io_cap[2] = sp->remote_Authentication_Req;
    if (sp->simple_protocols == 2) memcpy(r, &sp->passkey, 4);
    f3_function_api(sp->local_ecdh_key, sp->remote_random_num, sp->local_random_num, r, io_cap,
                    conn->core_s.remote_addr, hci_param.addr, result);
    conn->core.enc_state |= ENC_SSP_DONE;
    return !memcmp(remote_confirmation, result, 16);
}

/* Secure Simple Pairing we started (the host had no link key). */
int lmp_stage_auth_with_simple_pair_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                             u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_simple_pair *sp;
    (void)opcode2;
    switch (conn->core.step) {
    case LMP_TX_IO_CAPABILITY_REQ:
        hci_send_event(0x31, "A", conn->core_s.remote_addr);       /* IO Capability Request */
        conn->core.step = LMP_WAIT_IO_CAPABILITY_REPLY;
        break;
    case LMP_WAIT_IO_CAPABILITY_REPLY:
        if (hci_cmd[0] != 0x042b) break;                           /* IO Capability Request Reply */
        conn->simple = sp = lmp_malloc(sizeof(struct lmp_simple_pair));
        if (!sp) btctrler_assert_failed("conn->simple", 0);
        sp->local_IO_Capability = (u8)hci_cmd[1];
        sp->local_OOB_Data_Present = (u8)hci_cmd[2];
        sp->local_Authentication_Req = (u8)hci_cmd[3];
        lmp_request(conn, "b05", PDU(ESCAPE_4), IO_CAPABILITY_REQ, hci_cmd[1] & 0xff,
                    hci_cmd[2] & 0xff, hci_cmd[3] & 0xff);
        lmp_ecdh_publickey(conn);
        conn->core.step = LMP_WAIT_IO_CAPABILITY_RES;
        break;
    case LMP_WAIT_IO_CAPABILITY_RES:
        if (opcode1 != IO_CAPABILITY_RES) break;
        lmp_io_capability_init(conn, rx->data + 2, 0);
        sp = conn->simple;
        lmp_response(conn, "1111", PDU(ENCAPSULATED_HEADER), sp->encapsulated_major_type,
                     sp->encapsulated_minor_type, sp->encapsulated_playload_length);
        conn->core.step = LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER:     /* send our public key, 16 bytes a PDU */
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_HEADER)) break;
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), conn->simple->local_public_key);
        conn->simple->local_public_key_len = 16;
        conn->core.step = LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_PAYLOAD)) break;
        sp = conn->simple;
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), sp->local_public_key + sp->local_public_key_len);
        sp = conn->simple;
        sp->local_public_key_len += 16;
        if (sp->local_public_key_len == sp->encapsulated_playload_length)
            conn->core.step = LMP_WAIT_ENCAPSULATEED_HEADER;
        break;
    case LMP_WAIT_ENCAPSULATEED_HEADER:            /* the peer's public key */
        if (opcode0 != PDU(ENCAPSULATED_HEADER)) break;
        lmp_response(conn, "11", PDU(ACCEPTED), ENCAPSULATED_HEADER);
        conn->core.step = LMP_WAIT_ENCAPSULATEED_PAYLOAD;
        break;
    case LMP_WAIT_ENCAPSULATEED_PAYLOAD:
        if (opcode0 != PDU(ENCAPSULATED_PAYLOAD)) break;
        sp = conn->simple;
        if (sp->remote_public_key_len != sp->encapsulated_playload_length) break;
        if (sp->simple_protocols == 0) conn->core.step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
        break;
    case LMP_WAIT_SIMPLE_PAIRING_CONFIRM:
        if (opcode0 != PDU(SIMPLE_PAIRING_CONFIRM)) break;
        sp = conn->simple;
        if (sp->simple_protocols == 0) {
            make_rand_num(sp->local_random_num);
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), conn->simple->local_random_num);
        } else if (sp->simple_protocols == 2) {
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), sp->local_random_num);
        }
        conn->core.step = LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER;
        break;
    case LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER:
        if (opcode1 == PDU(SIMPLE_PAIRING_NUMBER)) conn->core.step = LMP_WAIT_SIMPLE_PAIRING_NUMBER;
        break;
    case LMP_WAIT_SIMPLE_PAIRING_NUMBER:
        if (opcode0 != PDU(SIMPLE_PAIRING_NUMBER)) break;
        switch (lmp_master_check_c(conn)) {
        case 1: conn->core.step = LMP_WAIT_ACCEPTED_DHKEY_CHECK; break;
        case 2: conn->core.step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM; break;
        case 3: conn->core.step = LMP_WAIT_USER_CONFIRMATION; break;
        }
        break;
    case LMP_WAIT_USER_CONFIRMATION:
        if (hci_cmd[0] == 0x042c) {                /* User Confirmation Request Reply */
            conn->simple->local_confirm = 's';
            master_first_dhkey_check(conn);
            conn->core.step = LMP_WAIT_ACCEPTED_DHKEY_CHECK;
        } else if (hci_cmd[0] == 0x042d) {         /* negative reply */
            conn->simple->local_confirm = 'f';
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 0x05);
            lmp_tx_detch(conn, 0x05);
        }
        break;
    case LMP_WAIT_ACCEPTED_DHKEY_CHECK:
        if (opcode0 == PDU(ACCEPTED) && opcode1 == PDU(DHKEY_CHECK))
            conn->core.step = LMP_WAIT_DHKEY_CHECK;
        break;
    case LMP_WAIT_DHKEY_CHECK:
        if (opcode0 != PDU(DHKEY_CHECK)) break;
        if (!lmp_dhkey_check(conn, rx->data + 1)) {
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), DHKEY_CHECK, 0x05);
            break;
        }
        lmp_response(conn, "11", PDU(ACCEPTED), DHKEY_CHECK);
        hci_send_event(0x36, "1A", 0, conn->core_s.remote_addr);    /* Simple Pairing Complete */
        sp = conn->simple;
        if (conn->core.role == 0)
            f2_function_api(sp->local_ecdh_key, sp->local_random_num, sp->remote_random_num,
                            hci_param.addr, conn->core_s.remote_addr, conn->core.link_key);
        else
            f2_function_api(sp->local_ecdh_key, sp->remote_random_num, sp->local_random_num,
                            conn->core_s.remote_addr, hci_param.addr, conn->core.link_key);
        /* Authenticate with the new key; its notification follows there. */
        conn->core.step = LMP_TX_AU_RAND;
        conn->core.stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL;
        conn->core.enc_state |= ENC_HAVE_LINK_KEY | ENC_NEW_LINK_KEY;
        lmp_free(conn->simple);
        conn->simple = 0;
        return 1;
    }
    return 0;
}

/* ---- encryption --------------------------------------------------------- */

static void link_key_to_encryption_key(struct lmp_connection *conn, u8 *rand_num, u8 *key)
{
    function_E3_api(conn->core.link_key, rand_num, conn->core.cof, conn->core.key_size, key);
    link_conn_set_encrypt_key(conn->link, key);
}

void lmp_tx_start_encryption_req(struct lmp_connection *conn)
{
    u8 key[16], rand_num[16];
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    make_rand_num(rand_num);
    link_key_to_encryption_key(conn, rand_num, key);
    /* An answer to the peer's encryption mode request, or our own request. */
    if (conn->core.enc_state & ENC_PEER_MODE_REQ)
        lmp_response(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand_num);
    else
        lmp_request(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand_num);
}

void lmp_rx_accepted_start_encryption_req(struct lmp_connection *conn)
{
    link_conn_set_encrypt(conn->link, 1, 1);
    lmp_send_aclu_en(conn, 1);
    if (!(conn->core.enc_state & ENC_ON))
        conn->core.enc_state = ENC_ON;
    else
        hci_send_event(0x30, "1H", 0, conn->core_s.handle);     /* Encryption Key Refresh Complete */
    hci_send_event(0xf7, "1", 0);
}

void lmp_tx_stop_encryption_req(struct lmp_connection *conn)
{
    lmp_response(conn, "ev1", 1, 8, 36);      /* the SDK's format and values */
}

void lmp_rx_stop_encryption_req(struct lmp_connection *conn)
{
    conn->core.enc_state |= ENC_STOPPED;
    lmp_send_aclu_en(conn, 0);
    link_conn_set_encrypt(conn->link, 0, 0);
    lmp_response(conn, "11", PDU(ACCEPTED), STOP_ENCRYPTION_REQ);
}

void lmp_rx_accepted_stop_encryption_req(struct lmp_connection *conn)
{
    conn->core.enc_state |= ENC_STOPPED;
    lmp_send_aclu_en(conn, 0);
    link_conn_set_encrypt(conn->link, 0, 0);
}

void lmp_tx_pause_encryption_req(struct lmp_connection *conn)
{
    if (conn->core.enc_state & ENC_ON) lmp_request(conn, "11", PDU(ESCAPE_4), PAUSE_ENCRYPTION_REQ);
}

void lmp_rx_pause_encryption_req(struct lmp_connection *conn)
{
    if (conn->esco_conn) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), 11, 0x29);    /* sic: opcode 11 */
        return;
    }
    if (conn->core.role == 0) lmp_tx_stop_encryption_req(conn);
    else lmp_response(conn, "G11", PDU(ESCAPE_4), PAUSE_ENCRYPTION_REQ);
}

void lmp_rx_resume_encryption_req(struct lmp_connection *conn)
{
    u8 key[16], rand_num[16];
    if (!(conn->core.enc_state & ENC_STOPPED)) return;
    conn->core.enc_state &= ~ENC_STOPPED;
    make_rand_num(rand_num);
    lmp_response(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand_num);
    link_key_to_encryption_key(conn, rand_num, key);
    lmp_send_aclu_en(conn, 1);
}

/* The peer starts encryption (we are peripheral, or let it). */
int lmp_slave_stage_enc_start_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                        u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_WAIT_ENCRYPTION_MODE_REQ:
        if (opcode0 != PDU(ENCRYPTION_MODE_REQ)) break;
        if (conn->enc_timer) {
            sys_timer_del(conn->enc_timer);
            conn->enc_timer = 0;
        }
        if (conn->core.role == 0) {         /* a central sends the key size */
            conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
            conn->core.step = LMP_TX_ENCRYPTION_KEY_SIZE_REQ;
            conn->role_machine_handler = lmp_master_machine;
            return 2;
        }
        conn->core.step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ:
        if (opcode0 != PDU(NOT_ACCEPTED) && opcode1 == PDU(ENCRYPTION_MODE_REQ))
            conn->core.step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ:
        if (opcode0 == PDU(ENCRYPTION_KEY_SIZE_REQ)) conn->core.step = LMP_WAIT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_START_ENCRYPTION_REQ:
        if (opcode0 != PDU(START_ENCRYPTION_REQ)) break;
        conn->core.step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        lmp_send_event_encryption_change(conn);
        break;
    }
    return 0;
}

/* We start encryption (central, or a host's Set Connection Encryption). */
int lmp_master_stage_enc_start_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                        u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_role_switch *rs;
    (void)opcode2; (void)rx;
    switch (conn->core.step) {
    case LMP_WAIT_ENCRYPTION_MODE_REQ:
        if (hci_cmd[0] != 0x0413) break;            /* Set Connection Encryption */
        /* fall through */
    case LMP_TX_ENCRYPTION_MODE_REQ:
        lmp_tx_encryption_mode_req(conn);
        conn->core.step = LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ:
        if (opcode1 != PDU(ENCRYPTION_MODE_REQ)) break;
        if (conn->core.role == 1) {         /* a peripheral waits for the key size */
            conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
            conn->core.step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
            conn->role_machine_handler = lmp_slave_machine;
            return 2;
        }
        /* fall through */
    case LMP_TX_ENCRYPTION_KEY_SIZE_REQ:
        conn->core.key_size = 16;
        if (conn->core.enc_state & ENC_PEER_MODE_REQ)
            lmp_response(conn, "11", PDU(ENCRYPTION_KEY_SIZE_REQ), 16);
        else
            lmp_request(conn, "11", PDU(ENCRYPTION_KEY_SIZE_REQ), 16);
        conn->core.step = LMP_WAIT_ACCEPT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_KEY_SIZE_REQ:
        /* The peer accepts, or proposes the same size back. */
        if (opcode0 != PDU(ENCRYPTION_KEY_SIZE_REQ) &&
            (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(ENCRYPTION_KEY_SIZE_REQ)))
            break;
        /* fall through */
    case LMP_TX_START_ENCRYPTION_REQ:
        lmp_tx_start_encryption_req(conn);
        conn->core.step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ)) break;
        lmp_send_event_encryption_change(conn);
        conn->core.step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        if (conn->core.role != 0) conn->role_machine_handler = lmp_slave_machine;
        rs = conn->rs_process;
        if (rs && rs->over_role_switch) {
            rs->over_role_switch = 0;
            lmp_send_event_role_change(conn, rs->state == ROLE_SWITCH_SUSS ? 0 : 0x35);
            lmp_role_switch_misc_free(conn);
        }
        break;
    }
    return 0;
}

/* Encryption key refresh (central): pause, stop, start again. */
int lmp_master_stage_refreshing_the_encryption_key(struct lmp_connection *conn, u8 opcode0,
                                                   u8 opcode1, u8 opcode2,
                                                   struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_WAIT_PAUSE_ENCRYPTION_REQ:
        if (opcode1 != PAUSE_ENCRYPTION_REQ) break;
        /* fall through */
    case LMP_TX_STOP_ENCRYPTION_REQ:
        lmp_tx_stop_encryption_req(conn);
        conn->core.step = LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ:
        if (opcode1 != PDU(STOP_ENCRYPTION_REQ)) break;
        if (opcode0 == PDU(NOT_ACCEPTED)) btctrler_assert_failed("stop encryption refused", 0);
        /* fall through */
    case LMP_TX_START_ENCRYPTION_REQ:
        lmp_tx_start_encryption_req(conn);
        conn->core.step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ)) break;
        conn->core.step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        lmp_tx_version_req(conn);
        break;
    }
    return 0;
}

/* Secure Simple Pairing that the peer started (a console pairing from
 * Change Grip/Order), one step per call: conn->core.step holds the state,
 * opcode0/opcode1 the received LMP PDU (opcode << 1) and its first
 * parameter, hci_cmd the host command that woke it.  Numeric comparison
 * only: passkey entry is disabled in this firmware (platform_jieli_sdk.c),
 * so the SDK's passkey steps (73, 75, 77) do nothing or fall through. */
int lmp_stage_auth_with_simple_pair_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                              u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_simple_pair *sp;
    u8 z = 0;
    (void)opcode2;
    switch (conn->core.step) {
    case 66: /* waiting for the host's IO Capability Request Reply */
        if (hci_cmd[0] != 0x042b) break;
        sp = conn->simple;
        sp->local_IO_Capability = (u8)hci_cmd[1];
        sp->local_OOB_Data_Present = (u8)hci_cmd[2];
        sp->local_Authentication_Req = (u8)hci_cmd[3];
        lmp_authenticated_by_using_one_of_three_algorithms(conn);   /* HCI standard mode */
        sp = conn->simple;
        conn->tran_id = conn->core.role == 0;
        lmp_response(conn, "b05", 127 << 1, 26, sp->local_IO_Capability,    /* io_capability_res */
                     sp->local_OOB_Data_Present, sp->local_Authentication_Req);
        lmp_ecdh_publickey(conn);
        conn->core.step = 67;
        break;
    case 67: /* peer's encapsulated header (its public key) */
        if (opcode0 != 122) break;
        lmp_response(conn, "11", 3 << 1, 61);                               /* accepted */
        conn->core.step = 68;
        break;
    case 68: /* peer's key received: send our encapsulated header */
        if (opcode0 != 124) break;
        sp = conn->simple;
        if (sp->remote_public_key_len != sp->encapsulated_playload_length) break;
        lmp_response(conn, "1111", 122, sp->encapsulated_major_type,
                     sp->encapsulated_minor_type, sp->remote_public_key_len);
        conn->core.step = 69;
        break;
    case 69: /* header accepted: first 16 bytes of our key */
        if (opcode1 != 122 || opcode0 != 6) break;
        lmp_response(conn, "1C016", 124, conn->simple->local_public_key);
        conn->simple->local_public_key_len = 16;
        conn->core.step = 70;
        break;
    case 70: /* payload accepted: next 16 bytes; after the last, commit */
        if (opcode1 != 124 || opcode0 != 6) break;
        sp = conn->simple;
        lmp_response(conn, "1C016", 124, sp->local_public_key + sp->local_public_key_len);
        sp = conn->simple;
        sp->local_public_key_len += 16;
        if (sp->local_public_key_len != sp->encapsulated_playload_length) break;
        make_rand_num(sp->local_random_num);
        sp = conn->simple;
        f1_function_api(sp->local_public_key, sp->remote_public_key, sp->local_random_num, &z,
                        sp->local_commitment_num);
        sp = conn->simple;
        if (sp->simple_protocols == 0) {
            lmp_response(conn, "1C016", 126, sp->local_commitment_num);    /* simple_pairing_confirm */
            conn->core.step = 71;
        }
        break;
    case 73: /* passkey entry round; without passkey entry, just go on */
        if (opcode0 != 126) break;
        conn->core.step = 71;
        break;
    case 71: /* peer's nonce: answer with ours */
        if (opcode0 != 128) break;
        sp = conn->simple;
        memcpy(sp->remote_random_num, rx->data + 1, 16);
        sp = conn->simple;
        if (sp->simple_protocols == 0) {
            lmp_response(conn, "11", 3 << 1, 64);                           /* accepted */
            lmp_response(conn, "1C016", 128, conn->simple->local_random_num);
            conn->core.step = 72;
        }
        break;
    case 72: /* nonce accepted: ask the host to confirm the number */
        if (opcode1 != 128 || opcode0 != 6) break;
        sp = conn->simple;
        hci_send_event(0x33, "A4", conn->core_s.remote_addr,                /* User Confirmation Request */
                       g_function_api(sp->remote_public_key, sp->local_public_key,
                                      sp->remote_random_num, sp->local_random_num));
        conn->core.step = 74;
        break;
    case 74: /* both sides' confirmation */
        sp = conn->simple;
        if (!sp) break;
        if (hci_cmd[0] == 0x042c) sp->local_confirm = 's';
        else if (hci_cmd[0] == 0x042d) sp->local_confirm = 'f';
        if (opcode0 == 130) sp->remote_confirm = 's';                       /* dhkey_check */
        else if (opcode1 == 27) sp->remote_confirm = 'f';                   /* numeric_comparison_failed */
        if (sp->remote_confirm == 's') {
            if (sp->local_confirm == 's') {
                lmp_dhkey_check_accept(conn);
                conn->core.step = 78;
                conn->core.enc_state |= 0x400;
                hci_send_event(0x36, "1A", 0, conn->core_s.remote_addr);    /* Simple Pairing Complete */
            } else if (sp->local_confirm == 'f') {
                lmp_response(conn, "111", 4 << 1, 64, 5);                   /* not_accepted */
                hci_send_event(0x36, "1A", 5, conn->core_s.remote_addr);
            }
        } else if (sp->remote_confirm == 'f' && sp->local_confirm) {
            hci_send_event(0x36, "1A", 5, conn->core_s.remote_addr);
        }
        break;
    case 77:
    case 78: /* our DHKey check answered: derive the link key */
        if (opcode1 != 130) break;
        if (opcode0 == 8) p33_soft_reset();     /* rejected: the SDK resets the chip */
        sp = conn->simple;
        if (conn->core.role == 0)
            f2_function_api(sp->local_ecdh_key, sp->local_random_num, sp->remote_random_num,
                            hci_param.addr, conn->core_s.remote_addr, conn->core.link_key);
        else
            f2_function_api(sp->local_ecdh_key, sp->remote_random_num, sp->local_random_num,
                            conn->core_s.remote_addr, hci_param.addr, conn->core.link_key);
        /* Fix: the SDK computed the key and told nobody.  The host stores
         * Classic link keys; without this event a reconnect found no key,
         * paired again, and a console holding the old key dropped the link
         * (reason 5).  Every other key path raises this event. */
        lmp_send_event_link_key_notification(conn, 0);
        conn->core.step = 36;
        conn->core.stage = 12;
        conn->core.enc_state |= 3;
        lmp_free(conn->simple);
        conn->simple = 0;
        break;
    }
    return 0;
}


#ifdef UNITTEST
#include <stdarg.h>
#include <stdlib.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
struct hci_parameter hci_param;
u8 key_special_bind;
int config_btctler_modules, config_btctler_mode, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
static char trace[512];
static int sent_events;
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
/* LMP and HCI output. */
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); int op = va_arg(ap, int); va_end(ap);
    char s[16]; snprintf(s, sizeof(s), "r%d", op); note(s); (void)c; return 0;
}
int hci_send_event(int event, const char *format, ...)
{ char s[16]; snprintf(s, sizeof(s), "e%x", event); note(s); sent_events++; (void)format; return 0; }
void lmp_send_event_link_key_notification(struct lmp_connection *c, int ret) { note("key"); (void)c; (void)ret; }
void lmp_send_event_connection_complete(struct lmp_connection *c, u8 e, u16 h, u8 t) { note("cc"); (void)c; (void)e; (void)h; (void)t; }
/* Pairing helpers. */
void lmp_authenticated_by_using_one_of_three_algorithms(struct lmp_connection *c) { (void)c; }
void lmp_ecdh_publickey(struct lmp_connection *c) { (void)c; }
void make_rand_num(u8 *b) { memset(b, 7, 16); }
void f1_function_api(u8 *u, u8 *v, u8 *x, u8 *z, u8 *re) { note("f1"); (void)u; (void)v; (void)x; (void)z; memset(re, 1, 16); }
void f2_function_api(u8 *w, u8 *n1, u8 *n2, u8 *a1, u8 *a2, u8 *re) { note("f2"); (void)w; (void)n1; (void)n2; (void)a1; (void)a2; memset(re, 2, 16); }
u32 g_function_api(u8 *u, u8 *v, u8 *x, u8 *y) { (void)u; (void)v; (void)x; (void)y; return 123456; }
void lmp_dhkey_check_accept(struct lmp_connection *c) { note("accept"); (void)c; }
void p33_soft_reset(void) { note("RESET"); }
void lmp_free(void *p) { free(p); }
static void step(struct lmp_connection *c, u8 op0, u8 op1, int hci)
{
    static u8 buf[sizeof(struct bredr_rx_bulk) + 32];
    struct bredr_rx_bulk *rx = (struct bredr_rx_bulk *)buf;
    int cmd[6] = { hci, 3, 0, 1 };
    rx->data[0] = op0;
    for (int i = 1; i <= 16; i++) rx->data[i] = (u8)(0x40 + i);    /* the PDU's parameters */
    lmp_stage_auth_with_simple_pair_by_remote(c, op0, op1, 0, rx, cmd);
}

int main(void)
{
    struct lmp_connection *c = &lmp_acl_link[0];
    c->simple = calloc(1, sizeof(*c->simple));
    c->simple->encapsulated_playload_length = c->simple->remote_public_key_len = 48;
    c->core.step = 66; c->core.role = 1;
    step(c, 0, 0, 0x042b);  CHECK(c->core.step == 67 && c->simple->local_IO_Capability == 3);
    step(c, 122, 0, 0);     CHECK(c->core.step == 68);
    step(c, 124, 0, 0);     CHECK(c->core.step == 69);
    step(c, 6, 122, 0);     CHECK(c->core.step == 70);
    step(c, 6, 124, 0);     CHECK(c->core.step == 70 && c->simple->local_public_key_len == 32);
    step(c, 6, 124, 0);     CHECK(c->core.step == 71);
    step(c, 128, 0, 0);     CHECK(c->core.step == 72);
    CHECK(c->simple->remote_random_num[0] == 0x41 && c->simple->remote_random_num[15] == 0x50);
    step(c, 6, 128, 0);     CHECK(c->core.step == 74);
    step(c, 130, 0, 0x042c); CHECK(c->core.step == 78 && (c->core.enc_state & 0x400));
    step(c, 6, 130, 0);     CHECK(c->core.step == 36 && c->core.stage == 12 && !c->simple);
    CHECK(c->core.link_key[0] == 2);
    /* The link key reaches the host right after it is derived. */
    CHECK(strstr(trace, "accept e36 f2 key ") != 0 && !strstr(trace, "RESET"));

    return TEST_DONE("btctrler_lmp_encryption");
}
#endif
