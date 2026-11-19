/* Classic/lmp_encryption.c (included by lmp.c): legacy pairing, link keys,
 * secure simple pairing stages and encryption start/stop/pause/resume. */
#undef LMP_FILE
#define LMP_FILE "lmp_encryption.c"

int LMP_CODE lmp_simple_pair_enbale(struct lmp_connection *conn)
{
    if ((hci_param.features[6] & 8) && (hci_param.ext_features[0] & 1) &&
        (conn->core_s.remote_features[6] & 8) && (conn->core_s.remote_ext_features[0] & 1))
        return 1;
    return 0;
}

int LMP_CODE lmp_secure_connections_enbale(struct lmp_connection *conn)
{
    LOG_I("lmp_secure_connections_enbale: %d, %d, %d, %d", hci_param.ext_features[0] & 8,
          hci_param.ext_features[9] & 1, conn->core_s.remote_ext_features[0] & 8,
          conn->core_s.remote_ext_features[9] & 1);
    if ((hci_param.ext_features[0] & 8) && (hci_param.ext_features[9] & 1) &&
        (conn->core_s.remote_ext_features[0] & 8) && (conn->core_s.remote_ext_features[9] & 1))
        return 1;
    return 0;
}

void LMP_CODE lmp_free_encrypt(struct lmp_connection *conn)
{
    if (conn->encrypt) {
        lmp_free(conn->encrypt->rand_num);
        conn->encrypt = NULL;
    }
}

void LMP_CODE lmp_rx_in_rand(struct lmp_connection *conn, u8 *data)
{
    if (conn->encrypt == NULL) {
        conn->encrypt = lmp_malloc(69);
        SDK_ASSERT(conn->encrypt != NULL, 53, "conn->encrypt != NULL ");
    }
    memcpy(conn->encrypt->rand_num, data, 16);
}

void LMP_CODE lmp_in_rand_res(struct lmp_connection *conn, u8 *pin_code, u8 pin_code_len)
{
    conn->tran_id = conn->core.role == 0 ? conn->tran_id | 1 : conn->tran_id & ~1;
    lmp_response(conn, "11", PDU(ACCEPTED), IN_RAND);
    function_E22_api(pin_code, conn->encrypt->rand_num, pin_code_len, hci_param.addr, conn->encrypt->init_key);
}

void LMP_CODE lmp_rx_comb_key(struct lmp_connection *conn, u8 *rx)
{
    if (conn->encrypt == NULL) {
        conn->encrypt = lmp_malloc(69);
        SDK_ASSERT(conn->encrypt != NULL, 77, "conn->encrypt != NULL ");
    }
}

void LMP_CODE lmp_response_comb_key(struct lmp_connection *conn, u8 *rx)
{
    u8 a[16], b[16], k[16], rand[16];

    make_rand_num(rand);
    make_xor(rand, conn->encrypt->init_key, conn->encrypt->c);
    lmp_response(conn, "1C016", PDU(COMB_KEY), conn->encrypt->c);
    make_xor(rx, conn->encrypt->init_key, k);
    function_E21_api(conn->core_s.remote_addr, k, a);
    function_E21_api(hci_param.addr, rand, b);
    make_xor(a, b, conn->core.link_key);
    conn->core.enc_state |= 1;
}

void LMP_CODE lmp_tx_encryption_mode_req_dly(struct lmp_connection *conn)
{
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    if (conn->core.enc_state & 0x24)
        return;
    lmp_tx_encryption_mode_req(conn);
    conn->core.step = LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ;
    if (conn->core.role == 0)
        conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
}

void LMP_CODE lmp_rx_au_rand(struct lmp_connection *conn, u8 *data)
{
    u16 enc_state;

    if (conn->encrypt == NULL) {
        conn->encrypt = lmp_malloc(69);
        SDK_ASSERT(conn->encrypt != NULL, 125, "conn->encrypt != NULL ");
    }
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    conn->enc_timer = sys_timeout_add(conn, (void (*)(void *))lmp_tx_encryption_mode_req_dly, 200);
    memcpy(conn->encrypt->rand_num, data, 16);
    enc_state = conn->core.enc_state;
    conn->core.enc_state = enc_state | 0x80;
    if (!(enc_state & 5)) {
        LOG_I("send_sres_after_get_link_key");
        return;
    }
    __calc_and_send_sres(conn, conn->encrypt->rand_num);
    LOG_I("send_sres_now : %d, %d", conn->core.stage, conn->core.step);
    if ((conn->core.enc_state & 4) && conn->encrypt) {
        make_rand_num(conn->encrypt->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        conn->core.stage = LMP_STAGE_CALCULATE_LINK_KEY;
    } else {
        LOG_E("no need send au rand");
    }
}

void LMP_CODE __calc_and_send_sres(struct lmp_connection *conn, u8 *rand)
{
    u8 out[16];

    function_E1_api(hci_param.addr, rand, conn->core.link_key, out);
    memcpy(conn->encrypt->sres, out, 4);
    memcpy(conn->core.cof, &out[4], 12);
    lmp_response(conn, "1C004", PDU(SRES), conn->encrypt->sres);
}

void LMP_CODE lmp_rx_sres(struct lmp_connection *conn, u8 *data)
{
    u8 out[16];
    struct lmp_encrypt *enc;

    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    conn->enc_timer = sys_timeout_add(conn, (void (*)(void *))lmp_tx_encryption_mode_req_dly, 200);
    function_E1_api(conn->core_s.remote_addr, conn->encrypt->rand_num, conn->core.link_key, out);
    memcpy(conn->encrypt->sres, out, 4);
    memcpy(conn->core.cof, &out[4], 12);
    enc = conn->encrypt;
    if (memcmp(enc->sres, data, 4) == 0) {
        enc->sres_mach = 1;
    } else {
        enc->sres_mach = 2;
        conn->err_code = 5;
        lmp_send_event_auth_complete(conn, 5);
    }
}

void LMP_CODE lmp_tx_in_rand(struct lmp_connection *conn, u8 *pin_code, u8 pin_code_len)
{
    u8 rand[16];

    make_rand_num(rand);
    lmp_request(conn, "1C016", PDU(IN_RAND), rand);
    function_E22_api(pin_code, rand, pin_code_len, conn->core_s.remote_addr, conn->encrypt->init_key);
}

void LMP_CODE lmp_rx_not_accepted_in_rand(struct lmp_connection *conn)
{
    lmp_tx_detch(conn, 0x13);
}

void LMP_CODE lmp_tx_comb_key(struct lmp_connection *conn)
{
    u8 rand[16];

    if (conn->core.enc_state & 1)
        memcpy(conn->encrypt->init_key, conn->core.link_key, 16);
    make_rand_num(rand);
    function_E21_api(hci_param.addr, rand, conn->encrypt->lk_k);
    make_xor(rand, conn->encrypt->init_key, conn->encrypt->c);
    lmp_request(conn, "1C016", PDU(COMB_KEY), conn->encrypt->c);
}

void LMP_CODE edr_vendor_keep_connect_for_keymiss(u8 en)
{
    key_special_bind = en;
}

void LMP_CODE lmp_rx_not_accept_au_rand(struct lmp_connection *conn, u8 *rx)
{
    u8 reason = rx[1];

    switch (reason) {
    case 0x23:
        conn->encrypt->sres_mach = 1;
        lmp_send_event_auth_complete(conn, 0);
        return;
    case 0x06:
        if (key_special_bind) {
            conn->core.step = LMP_TX_IO_CAPABILITY_REQ;
            conn->core.stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL;
            lmp_stage_auth_with_simple_pair_by_local(conn, 0, 0, 0, NULL, (int *)12);
            return;
        }
        break;
    }
    lmp_send_event_auth_complete(conn, reason);
}

void LMP_CODE lmp_tx_au_rand(struct lmp_connection *conn)
{
    u8 rand[16];

    make_rand_num(rand);
    lmp_request(conn, "1C016", PDU(AU_RAND), rand);
}

static void lmp_rx_encryption_mode_req(struct lmp_connection *conn)
{
    conn->core.enc_state |= 0x20;
    lmp_response(conn, "11", PDU(ACCEPTED), ENCRYPTION_MODE_REQ);
}

static void lmp_rx_encryption_key_size_req(struct lmp_connection *conn, u8 *data)
{
    conn->core.key_size = data[0];
    switch (data[0]) {
    case 16:
    case 7:
        lmp_response(conn, "11", PDU(ACCEPTED), ENCRYPTION_KEY_SIZE_REQ);
        break;
    default:
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), ENCRYPTION_KEY_SIZE_REQ, 0x29);
        LOG_I("LMP_NOT_ACCEPTED key_size %d\n", data[0]);
        break;
    }
}

static void lmp_rx_encryption_key_size_mask_req(struct lmp_connection *conn)
{
    if (conn->core.role == 0)
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), ENCRYPTION_KEY_SIZE_MASK_REQ, 0x1a);
    else
        lmp_response(conn, "12", PDU(ENCRYPTION_KEY_SIZE_MASK_RES), 0xffff);
}

static void lmp_rx_start_encryption_req(struct lmp_connection *conn, u8 *data)
{
    u8 key[16];

    LOG_I("---- %d \n", conn->core.key_size);
    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    function_E3_api(conn->core.link_key, data, conn->core.cof, conn->core.key_size, key);
    link_conn_set_encrypt_key(conn->link, key);
    link_conn_set_encrypt(conn->link, 1, 1);
    lmp_response(conn, "11", PDU(ACCEPTED), START_ENCRYPTION_REQ);
    hci_send_event(0xf7, "1", 0);
    lmp_acl_u_tx_enable(conn, 1);
    conn->core.enc_state = 4;
    if (CONFIG_BTCTLER_TWS_ENABLE && conn->core.role == 1)
        tws_phone_link_connection(conn);
    LOG_I("lmp_rx_start_encryption_req");
}

void LMP_CODE lmp_rx_stop_encryption_req(struct lmp_connection *conn)
{
    conn->core.enc_state |= 0x1000;
    lmp_acl_u_tx_enable(conn, 0);
    link_conn_set_encrypt(conn->link, 0, 0);
    lmp_response(conn, "11", PDU(ACCEPTED), STOP_ENCRYPTION_REQ);
}

void LMP_CODE lmp_rx_pause_encryption_req(struct lmp_connection *conn)
{
    if (conn->esco_conn) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), AU_RAND, 0x29);   /* sic: 11 */
        return;
    }
    if (conn->core.role == 0) {
        LOG_I("step %d", conn->core.step);
        LOG_D("lmp_tx_stop_encryption_req");
        lmp_tx_stop_encryption_req(conn);
    } else {
        LOG_I("pause_enc_step_1");
        lmp_response(conn, "G11", 0xfe, PAUSE_ENCRYPTION_REQ);
    }
}

void LMP_CODE lmp_rx_resume_encryption_req(struct lmp_connection *conn)
{
    u8 key[16];
    u8 rand[16];

    if (!(conn->core.enc_state & 0x1000))
        return;
    conn->core.enc_state &= ~0x1000;
    make_rand_num(rand);
    lmp_response(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand);
    function_E3_api(conn->core.link_key, rand, conn->core.cof, conn->core.key_size, key);
    link_conn_set_encrypt_key(conn->link, key);
    lmp_acl_u_tx_enable(conn, 1);
}

void LMP_CODE lmp_tx_encryption_mode_req(struct lmp_connection *conn)
{
    lmp_acl_u_tx_enable(conn, 0);
    lmp_request(conn, "11", PDU(ENCRYPTION_MODE_REQ), 1);
}

void LMP_CODE lmp_rx_not_accepted_encryption_mode_req(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_rx_accepted_encryption_mode_req(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_tx_encryption_key_size_req(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_rx_accepted_encryption_key_size_req(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_tx_start_encryption_req(struct lmp_connection *conn)
{
    u8 key[16];
    u8 rand[16];

    if (conn->enc_timer) {
        sys_timer_del(conn->enc_timer);
        conn->enc_timer = 0;
    }
    make_rand_num(rand);
    function_E3_api(conn->core.link_key, rand, conn->core.cof, conn->core.key_size, key);
    link_conn_set_encrypt_key(conn->link, key);
    if (conn->core.enc_state & 0x20)
        lmp_response(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand);
    else
        lmp_request(conn, "e1C016", 2, PDU(START_ENCRYPTION_REQ), rand);
}

void LMP_CODE lmp_rx_accepted_start_encryption_req(struct lmp_connection *conn)
{
    link_conn_set_encrypt(conn->link, 1, 1);
    lmp_acl_u_tx_enable(conn, 1);
    if (!(conn->core.enc_state & 4))
        conn->core.enc_state = 4;
    else
        hci_send_event(48, "1H", 0, conn->core_s.handle);
    hci_send_event(0xf7, "1", 0);
}

void LMP_CODE lmp_tx_stop_encryption_req(struct lmp_connection *conn)
{
    lmp_response(conn, "ev1", 1, 8, PDU(STOP_ENCRYPTION_REQ));
}

void LMP_CODE lmp_rx_accepted_stop_encryption_req(struct lmp_connection *conn)
{
    conn->core.enc_state |= 0x1000;
    lmp_acl_u_tx_enable(conn, 0);
    link_conn_set_encrypt(conn->link, 0, 0);
}

void LMP_CODE lmp_tx_pause_encryption_req(struct lmp_connection *conn)
{
    if (conn->core.enc_state & 4) {
        LOG_I("pause_enc_step_2");
        lmp_request(conn, "11", 0xfe, PAUSE_ENCRYPTION_REQ);
    }
}

void LMP_CODE lmp_tx_keypress_notification(struct lmp_connection *conn, u8 type)
{
    lmp_response(conn, "111", 0xfe, KEYPRESS_NOTIFICATION, type);
}

void LMP_CODE lmp_tx_user_passkey_request_reply(struct lmp_connection *conn, u8 *data)
{
    struct lmp_simple_pair *simple = conn->simple;
    u8 z[1];

    simple->passkey = data[2] << 16 | data[3] << 24 | data[1] << 8 | data[0];
    if (simple->simple_protocols != 2)
        return;
    make_rand_num(simple->local_random_num);
    z[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
    if (simple->simple_protocols == 2)
        f1_function_api(simple->local_public_key, simple->remote_public_key, simple->local_random_num, z,
                        simple->local_commitment_num);
    lmp_response(conn, "1c16", PDU(SIMPLE_PAIRING_CONFIRM), simple->local_commitment_num);
}

int LMP_CODE lmp_stage_auth_with_link_key_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                   u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;
    u16 enc_state;

    switch (core->step) {
    case LMP_TX_HCI_EVENT_LINK_KEY_REQ:
        lmp_send_event_link_request(conn);
        core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        break;
    case LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY:
        switch (*hci_cmd) {
        case 0x040b:
            LOG_I("HCI_LINK_KEY_REQUEST_REPLY");
            enc_state = conn->core.enc_state | 1;
            conn->core.enc_state = enc_state;
            memcpy(conn->core.link_key, hci_cmd + 1, 16);
            if (enc_state & 0x400) {
                lmp_tx_au_rand(conn);
                core->step = LMP_WAIT_SRES;
                break;
            }
            if (enc_state & 2)
                lmp_send_event_link_key_notification(conn, 0);
            conn->tran_id = conn->core.role == 0;
            __calc_and_send_sres(conn, conn->encrypt->rand_num);
            core->step = LMP_WAIT_ENCRYPTION_MODE_REQ;
            core->stage = LMP_STAGE_ENC_START_BY_REMOTE;
            lmp_free_encrypt(conn);
            return 1;
        case 0x040c:
            LOG_I("HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY PIN_OR_KEY_MISSING");
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), AU_RAND, 0x06);
            core->step = LMP_WAIT_AUTH;
            core->stage = LMP_STAGE_CONN;
            if (conn->enc_timer) {
                sys_timer_del(conn->enc_timer);
                conn->enc_timer = 0;
            }
            break;
        }
        break;
    case LMP_TX_SRES:
        conn->tran_id = conn->core.role == 0;
        __calc_and_send_sres(conn, conn->encrypt->rand_num);
        core->step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        core->stage = LMP_STAGE_ENC_START_BY_REMOTE;
        lmp_free_encrypt(conn);
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES))
            break;
        if (conn->encrypt->sres_mach == 1) {
            core->step = LMP_WAIT_ENCRYPTION_MODE_REQ;
            core->stage = LMP_STAGE_ENC_START_BY_REMOTE;
        }
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

int LMP_CODE lmp_stage_auth_with_simple_pair_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                     u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_simple_pair *simple;
    u8 z[1];
    u8 *step = &conn->core.step;

    switch (*step) {
    case LMP_TX_IO_CAPABILITY_REQ:
        hci_send_event(49, "A", conn->core_s.remote_addr);
        *step = LMP_WAIT_IO_CAPABILITY_REPLY;
        break;
    case LMP_WAIT_IO_CAPABILITY_REPLY:
        if (*hci_cmd != 0x042b)
            break;
        conn->simple = lmp_malloc(208);
        SDK_ASSERT(conn->simple, 677, "conn->simple ");
        conn->simple->local_IO_Capability = hci_cmd[1];
        conn->simple->local_OOB_Data_Present = hci_cmd[2];
        conn->simple->local_Authentication_Req = hci_cmd[3];
        lmp_request(conn, "b05", 0xfe, IO_CAPABILITY_REQ, (u8)hci_cmd[1], (u8)hci_cmd[2], (u8)hci_cmd[3]);
        lmp_ecdh_publickey(conn);
        *step = LMP_WAIT_IO_CAPABILITY_RES;
        break;
    case LMP_WAIT_IO_CAPABILITY_RES:
        if (opcode1 != IO_CAPABILITY_RES)
            break;
        lmp_io_capability_init(conn, &rx->data[2], 0);
        simple = conn->simple;
        lmp_response(conn, "1111", PDU(ENCAPSULATED_HEADER), simple->encapsulated_major_type,
                     simple->encapsulated_minor_type, simple->encapsulated_playload_length);
        *step = LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_HEADER))
            break;
        LOG_I("encapsulated_major_type:%d, encapsulated_minor_type:%d, encapsulated_playload_length:%d",
              rx->data[1], rx->data[2], rx->data[3]);
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), conn->simple->local_public_key);
        conn->simple->local_public_key_len = 16;
        *step = LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_PAYLOAD))
            break;
        simple = conn->simple;
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), &simple->local_public_key[simple->local_public_key_len]);
        simple = conn->simple;
        simple->local_public_key_len += 16;
        if (simple->local_public_key_len == simple->encapsulated_playload_length)
            *step = LMP_WAIT_ENCAPSULATEED_HEADER;
        break;
    case LMP_WAIT_ENCAPSULATEED_HEADER:
        if (opcode0 != PDU(ENCAPSULATED_HEADER))
            break;
        lmp_response(conn, "11", PDU(ACCEPTED), ENCAPSULATED_HEADER);
        *step = LMP_WAIT_ENCAPSULATEED_PAYLOAD;
        break;
    case LMP_WAIT_ENCAPSULATEED_PAYLOAD:
        if (opcode0 != PDU(ENCAPSULATED_PAYLOAD))
            break;
        simple = conn->simple;
        if (simple->remote_public_key_len != simple->encapsulated_playload_length)
            break;
        if (simple->simple_protocols == 0) {
            *step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
            break;
        }
        if (!CONFIG_LMP_PASSKEY_ENABLE)
            break;
        switch (simple->simple_protocols) {
        case 1:
            SDK_ASSERT(0, 746, "0 UNSUPPORTED_OOB");
            break;
        case 2:
            get_random_number((u8 *)&simple->passkey, 4);
            conn->simple->passkey %= 999999;
            LOG_I(" * PASSKEY : %d * ", conn->simple->passkey);
            hci_send_event(59, "A4", conn->core_s.remote_addr, conn->simple->passkey);
            *step = LMP_WAIT_KEY_PRESS_NOTIFICATION;
            break;
        default:
            SDK_ASSERT(0, 758, "0 err simple_protocols : 0x%x", conn->simple->simple_protocols);
            break;
        }
        break;
    case LMP_WAIT_SIMPLE_PAIRING_CONFIRM:
        if (opcode0 != PDU(SIMPLE_PAIRING_CONFIRM))
            break;
        simple = conn->simple;
        switch (simple->simple_protocols) {
        case 0:
            make_rand_num(simple->local_random_num);
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), conn->simple->local_random_num);
            break;
        case 2:
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), simple->local_random_num);
            break;
        }
        *step = LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER;
        break;
    case LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER:
        if (opcode1 == PDU(SIMPLE_PAIRING_NUMBER))
            *step = LMP_WAIT_SIMPLE_PAIRING_NUMBER;
        break;
    case LMP_WAIT_SIMPLE_PAIRING_NUMBER:
        if (opcode0 != PDU(SIMPLE_PAIRING_NUMBER))
            break;
        switch (lmp_master_check_c(conn)) {
        case 1:
            *step = LMP_WAIT_ACCEPTED_DHKEY_CHECK;
            break;
        case 2:
            *step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
            break;
        case 3:
            *step = LMP_WAIT_USER_CONFIRMATION;
            break;
        }
        break;
    case LMP_WAIT_KEY_PRESS_NOTIFICATION:
        if (opcode1 == KEYPRESS_NOTIFICATION && CONFIG_LMP_PASSKEY_ENABLE) {
            hci_send_event(60, "A1", conn->core_s.remote_addr, rx->data[1]);
            if (rx->data[1] == 4) {
                *step = LMP_TX_SIMPLE_PAIRING_CONFIRM;
                return 1;
            }
            goto tx_confirm;
        }
        /* fall through */
    case LMP_TX_SIMPLE_PAIRING_CONFIRM:
        if (!CONFIG_LMP_PASSKEY_ENABLE)
            break;
tx_confirm:
        make_rand_num(conn->simple->local_random_num);
        simple = conn->simple;
        z[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
        f1_function_api(simple->local_public_key, simple->remote_public_key, simple->local_random_num, z,
                        simple->local_commitment_num);
        lmp_response(conn, "1c16", PDU(SIMPLE_PAIRING_CONFIRM), conn->simple->local_commitment_num);
        *step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
        break;
    case LMP_WAIT_USER_CONFIRMATION:
        switch (*hci_cmd) {
        case 0x042c:
            conn->simple->local_confirm = 's';
            master_first_dhkey_check(conn);
            *step = LMP_WAIT_ACCEPTED_DHKEY_CHECK;
            break;
        case 0x042d:
            conn->simple->local_confirm = 'f';
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 5);
            lmp_tx_detch(conn, 5);
            break;
        }
        break;
    case LMP_WAIT_ACCEPTED_DHKEY_CHECK:
        if (opcode0 == PDU(ACCEPTED) && opcode1 == PDU(DHKEY_CHECK))
            *step = LMP_WAIT_DHKEY_CHECK;
        break;
    case LMP_WAIT_DHKEY_CHECK:
        if (opcode0 != PDU(DHKEY_CHECK))
            break;
        if (!lmp_dhkey_check(conn, &rx->data[1])) {
            LOG_I("lmp_dhkey_check not acc\n");
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), DHKEY_CHECK, 5);
            break;
        }
        lmp_response(conn, "11", PDU(ACCEPTED), DHKEY_CHECK);
        hci_send_event(54, "1A", 0, conn->core_s.remote_addr);
        simple = conn->simple;
        if (conn->core.role == 0)
            f2_function_api(simple->local_ecdh_key, simple->local_random_num, simple->remote_random_num,
                            hci_param.addr, conn->core_s.remote_addr, conn->core.link_key);
        else
            f2_function_api(simple->local_ecdh_key, simple->remote_random_num, simple->local_random_num,
                            conn->core_s.remote_addr, hci_param.addr, conn->core.link_key);
        *step = LMP_TX_AU_RAND;
        conn->core.stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL;
        conn->core.enc_state |= 3;
        lmp_free(conn->simple);
        conn->simple = NULL;
        return 1;
    }
    return 0;
}

int LMP_CODE lmp_stage_auth_with_link_key_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                  u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 out[16];
    u8 *step = &conn->core.step;

    switch (*step) {
    case LMP_TX_AU_RAND:
        LOG_I("LMP_TX_AU_RAND");
        if (conn->role_switch_state == 1) {
            LOG_I("LMP_TX_AU_RAND_delay");
            break;
        }
        if (conn->encrypt == NULL) {
            conn->encrypt = lmp_malloc(69);
            SDK_ASSERT(conn->encrypt != NULL, 927, "conn->encrypt != NULL ");
        }
        make_rand_num(conn->encrypt->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), conn->encrypt->rand_num);
        function_E1_api(conn->core_s.remote_addr, conn->encrypt->rand_num, conn->core.link_key, out);
        memcpy(conn->encrypt->sres, out, 4);
        memcpy(conn->core.cof, &out[4], 12);
        *step = LMP_WAIT_SRES;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES))
            break;
        LOG_I("LMP_WAIT_SRES");
        if (conn->encrypt->sres_mach == 2) {
            lmp_send_event_connection_complete(conn, 5, conn->core_s.handle, 1);
            lmp_tx_detch(conn, 5);
            break;
        }
        *step = LMP_WAIT_AU_RAND;
        /* fall through */
    case LMP_WAIT_AU_RAND:
        if (conn->core.enc_state & 0x2000) {
            if (opcode0 != PDU(AU_RAND))
                break;
            lmp_send_event_link_key_notification(conn, 0);
        } else {
            if (opcode0 != PDU(SRES))
                break;
            if (conn->encrypt->sres_mach != 1)
                SDK_ASSERT(0, 963, "0 ");
        }
        lmp_send_event_auth_complete(conn, 0);
        if (!(conn->core.enc_state & 4)) {
            conn->core.state |= 0x10;
            if (conn->core.role == 1) {
                conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
                *step = LMP_WAIT_ENCRYPTION_MODE_REQ;
                if (conn->enc_timer) {
                    sys_timer_del(conn->enc_timer);
                    conn->enc_timer = 0;
                }
                conn->enc_timer = sys_timer_add(conn, (void (*)(void *))lmp_tx_encryption_mode_req_dly, 200);
            } else {
                conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
                *step = LMP_TX_ENCRYPTION_MODE_REQ;
            }
        }
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

int LMP_CODE lmp_master_stage_enc_start_by_local(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;
    struct lmp_role_switch *rs;

    switch (core->step) {
    case LMP_WAIT_ENCRYPTION_MODE_REQ:
        if (*hci_cmd != 0x0413)
            break;
        /* fall through */
    case LMP_TX_ENCRYPTION_MODE_REQ:
        LOG_I("LMP_TX_ENCRYPTION_MODE_REQ");
        lmp_tx_encryption_mode_req(conn);
        core->step = LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ:
        if (opcode1 != PDU(ENCRYPTION_MODE_REQ))
            break;
        if (conn->core.role == 1) {
            core->stage = LMP_STAGE_ENC_START_BY_REMOTE;
            core->step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
            lmp_role_machine_handler_register(conn, lmp_slave_machine);
            return 2;
        }
        /* fall through */
    case LMP_TX_ENCRYPTION_KEY_SIZE_REQ:
        LOG_I("LMP_TX_ENCRYPTION_KEY_SIZE_REQ");
        conn->core.key_size = 16;
        if (conn->core.enc_state & 0x20)
            lmp_response(conn, "11", PDU(ENCRYPTION_KEY_SIZE_REQ), 16);
        else
            lmp_request(conn, "11", PDU(ENCRYPTION_KEY_SIZE_REQ), 16);
        core->step = LMP_WAIT_ACCEPT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_KEY_SIZE_REQ:
        LOG_I("LMP_WAIT_ACCEPT_ENCRYPTION_KEY_SIZE_REQ %x %x %x\n", opcode0, opcode1, opcode2);
        if (opcode0 != PDU(ENCRYPTION_KEY_SIZE_REQ)) {
            if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(ENCRYPTION_KEY_SIZE_REQ))
                break;
        }
        /* fall through */
    case LMP_TX_START_ENCRYPTION_REQ:
        LOG_I("LMP_TX_START_ENCRYPTION_REQ");
        lmp_tx_start_encryption_req(conn);
        core->step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ))
            break;
        lmp_send_event_encryption_change(conn);
        core->step = LMP_TX_NULL;
        core->stage = LMP_STAGE_CONNECTED;
        if (conn->core.role) {
            if (CONFIG_BTCTLER_TWS_ENABLE)
                tws_phone_link_connection(conn);
            lmp_role_machine_handler_register(conn, lmp_slave_machine);
        }
        rs = conn->rs_process;
        if (rs && rs->over_role_switch) {
            rs->over_role_switch = 0;
            if (rs->state == 2)
                lmp_send_event_role_change(conn, 0);
            else
                lmp_send_event_role_change(conn, 0x35);
            lmp_role_switch_misc_free(conn);
        }
        if (!config_btctler_hci_standard && CONFIG_BTCTLER_TWS_ENABLE && conn->core.role == 0 &&
            (hci_param.features[0] & 0x20)) {
            puts("conn->core.role_switch = 1\n");
            conn->core.role_switch = 1;
        }
        break;
    }
    return 0;
}

int LMP_CODE lmp_stage_auth_with_pin_code(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                         struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 a[16], k[16], out[16];
    u8 *step = &conn->core.step;
    u8 *addr;

    switch (*step) {
    case LMP_TX_PIN_CODE_REQ:
        hci_send_event(22, "A", conn->core_s.remote_addr);
        *step = LMP_WAIT_PIN_CODE_REQ_REPLY;
        break;
    case LMP_WAIT_PIN_CODE_REQ_REPLY:
        switch (*hci_cmd) {
        case 0x040d:
            LOG_I("LMP_TX_IN_RAND");
            conn->encrypt = lmp_malloc(69);
            SDK_ASSERT(conn->encrypt, 1127, "conn->encrypt %s\n", "lmp_stage_auth_with_pin_code");
            lmp_tx_in_rand(conn, (u8 *)(hci_cmd + 2), (u8)hci_cmd[1]);
            *step = LMP_WAIT_ACCEPT_IN_RAND;
            break;
        case 0x040e:
            lmp_tx_detch(conn, 6);
            break;
        }
        break;
    case LMP_WAIT_ACCEPT_IN_RAND:
        if (opcode1 != PDU(IN_RAND))
            break;
        if (opcode0 == PDU(NOT_ACCEPTED)) {
            lmp_tx_detch(conn, 0x13);
            break;
        }
        /* fall through */
    case LMP_TX_COMB_KEY:
        LOG_I("LMP_TX_COMB_KEY");
        lmp_tx_comb_key(conn);
        *step = LMP_WAIT_COMB_KEY;
        break;
    case LMP_WAIT_COMB_KEY:
        if (opcode0 != PDU(COMB_KEY))
            break;
        make_rand_num(conn->encrypt->rand_num);
        lmp_request(conn, "1C016", PDU(AU_RAND), conn->encrypt->rand_num);
        make_xor(&rx->data[1], conn->encrypt->init_key, k);
        addr = conn->core_s.remote_addr;
        function_E21_api(addr, k, a);
        make_xor(a, conn->encrypt->lk_k, conn->core.link_key);
        function_E1_api(addr, conn->encrypt->rand_num, conn->core.link_key, out);
        memcpy(conn->encrypt->sres, out, 4);
        memcpy(conn->core.cof, &out[4], 12);
        *step = LMP_WAIT_SRES;
        conn->core.enc_state |= 1;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES))
            break;
        if (conn->encrypt->sres_mach != 2)
            *step = LMP_WAIT_AU_RAND;
        break;
    case LMP_WAIT_AU_RAND:
        if (opcode0 != PDU(AU_RAND))
            break;
        conn->core.state |= 0x10;
        lmp_send_event_auth_complete(conn, 0);
        *step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        conn->core.stage = LMP_STAGE_ENC_START_BY_LOCAL;
        lmp_send_event_link_key_notification(conn, 0);
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

int LMP_CODE lmp_stage_auth_with_simple_pair_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                      u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_simple_pair *simple;
    u8 z[1], zr[1], zc[1];
    u8 c[16];
    u8 *step = &conn->core.step;
    int cmd;

    switch (*step) {
    case LMP_WAIT_IO_CAPABILITY_REPLY:
        LOG_I("hci_cmd: %d\n", *hci_cmd);
        if (*hci_cmd != 0x042b)
            break;
        conn->simple->local_IO_Capability = hci_cmd[1];
        conn->simple->local_OOB_Data_Present = hci_cmd[2];
        conn->simple->local_Authentication_Req = hci_cmd[3];
        if (config_btctler_hci_standard)
            lmp_authenticated_by_using_one_of_three_algorithms(conn);
        conn->tran_id = conn->core.role == 0;
        lmp_response(conn, "b05", 0xfe, IO_CAPABILITY_RES, conn->simple->local_IO_Capability,
                     conn->simple->local_OOB_Data_Present, conn->simple->local_Authentication_Req);
        lmp_ecdh_publickey(conn);
        *step = LMP_WAIT_ENCAPSULATEED_HEADER;
        break;
    case LMP_WAIT_ENCAPSULATEED_HEADER:
        if (opcode0 != PDU(ENCAPSULATED_HEADER))
            break;
        lmp_response(conn, "11", PDU(ACCEPTED), ENCAPSULATED_HEADER);
        *step = LMP_WAIT_ENCAPSULATEED_PAYLOAD;
        break;
    case LMP_WAIT_ENCAPSULATEED_PAYLOAD:
        if (opcode0 != PDU(ENCAPSULATED_PAYLOAD))
            break;
        simple = conn->simple;
        if (simple->remote_public_key_len != simple->encapsulated_playload_length)
            break;
        lmp_response(conn, "1111", PDU(ENCAPSULATED_HEADER), simple->encapsulated_major_type,
                     simple->encapsulated_minor_type, simple->remote_public_key_len);
        *step = LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_HEADER:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_HEADER))
            break;
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), conn->simple->local_public_key);
        conn->simple->local_public_key_len = 16;
        *step = LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD;
        break;
    case LMP_WAIT_ACCEPTED_ENCAPSULATED_PAYLOAD:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(ENCAPSULATED_PAYLOAD))
            break;
        simple = conn->simple;
        lmp_response(conn, "1C016", PDU(ENCAPSULATED_PAYLOAD), &simple->local_public_key[simple->local_public_key_len]);
        simple = conn->simple;
        simple->local_public_key_len += 16;
        if (simple->local_public_key_len != simple->encapsulated_playload_length)
            break;
        z[0] = 0;
        make_rand_num(simple->local_random_num);
        simple = conn->simple;
        f1_function_api(simple->local_public_key, simple->remote_public_key, simple->local_random_num, z,
                        simple->local_commitment_num);
        simple = conn->simple;
        if (simple->simple_protocols == 0) {
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_CONFIRM), simple->local_commitment_num);
            *step = LMP_WAIT_SIMPLE_PAIRING_NUMBER;
            break;
        }
        if (!CONFIG_LMP_PASSKEY_ENABLE)
            break;
        switch (simple->simple_protocols) {
        case 1:
            SDK_ASSERT(0, 1292, "0 UNSUPPORTED_OOB");
            break;
        case 2:
            *step = LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
            break;
        default:
            SDK_ASSERT(0, 1303, "0 err simple_protocols : 0x%x", conn->simple->simple_protocols);
            break;
        }
        break;
    case LMP_WAIT_KEY_PRESS_NOTIFICATION:
        if (!CONFIG_LMP_PASSKEY_ENABLE)
            break;
        cmd = *hci_cmd;
        if (cmd == 0x0c60) {
            lmp_tx_keypress_notification(conn, hci_cmd[1]);
            cmd = *hci_cmd;
        }
        if (cmd != 0x042e)
            break;
        LOG_I(" press over \n");
        lmp_tx_user_passkey_request_reply(conn, (u8 *)(hci_cmd + 1));
        *step = LMP_WAIT_SIMPLE_PAIRING_NUMBER;
        break;
    case LMP_WAIT_SIMPLE_PAIRING_CONFIRM:
        if (opcode0 != PDU(SIMPLE_PAIRING_CONFIRM))
            break;
        simple = conn->simple;
        if (simple->simple_protocols == 2 && CONFIG_LMP_PASSKEY_ENABLE) {
            if (simple->passkey_cnt == 0) {
                *step = LMP_WAIT_KEY_PRESS_NOTIFICATION;
                hci_send_event(52, "A", conn->core_s.remote_addr);
                break;
            }
            make_rand_num(simple->local_random_num);
            simple = conn->simple;
            zc[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
            f1_function_api(simple->local_public_key, simple->remote_public_key, simple->local_random_num, zc,
                            simple->local_commitment_num);
            lmp_response(conn, "1c16", PDU(SIMPLE_PAIRING_CONFIRM), conn->simple->local_commitment_num);
        }
        *step = LMP_WAIT_SIMPLE_PAIRING_NUMBER;
        break;
    case LMP_WAIT_SIMPLE_PAIRING_NUMBER:
        if (opcode0 != PDU(SIMPLE_PAIRING_NUMBER))
            break;
        memcpy(conn->simple->remote_random_num, &rx->data[1], 16);
        simple = conn->simple;
        if (simple->simple_protocols == 0) {
            lmp_response(conn, "11", PDU(ACCEPTED), SIMPLE_PAIRING_NUMBER);
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), conn->simple->local_random_num);
            *step = LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER;
            break;
        }
        if (simple->simple_protocols != 2 || !CONFIG_LMP_PASSKEY_ENABLE)
            break;
        zr[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
        simple->passkey_cnt++;
        f1_function_api(simple->remote_public_key, simple->local_public_key, simple->remote_random_num, zr, c);
        if (memcmp(c, conn->simple->remote_commitment_num, 16) == 0) {
            lmp_response(conn, "11", PDU(ACCEPTED), SIMPLE_PAIRING_NUMBER);
            lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_NUMBER), conn->simple->local_random_num);
        } else {
            LOG_I("simple number err \n");
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 5);
            hci_send_event(54, "1A", 5, conn->core_s.remote_addr);
        }
        *step = conn->simple->passkey_cnt == 20 ? LMP_WAIT_DHKEY_CHECK : LMP_WAIT_SIMPLE_PAIRING_CONFIRM;
        break;
    case LMP_WAIT_ACCEPTED_SIMPLE_PARING_NUMBER:
        if (opcode0 == PDU(ACCEPTED) && opcode1 == PDU(SIMPLE_PAIRING_NUMBER)) {
            u32 numeric;

            simple = conn->simple;
            numeric = g_function_api(simple->remote_public_key, simple->local_public_key, simple->remote_random_num,
                                     simple->local_random_num);
            LOG_I("slave numeric_val = %d", numeric);
            hci_send_event(51, "A4", conn->core_s.remote_addr, numeric);
            *step = LMP_WAIT_USER_CONFIRMATION;
        }
        break;
    case LMP_WAIT_USER_CONFIRMATION:
        simple = conn->simple;
        if (simple == NULL)
            break;
        switch (*hci_cmd) {
        case 0x042c:
            simple->local_confirm = 's';
            break;
        case 0x042d:
            simple->local_confirm = 'f';
            break;
        }
        if (opcode0 == PDU(DHKEY_CHECK))
            simple->remote_confirm = 's';
        else if (opcode1 == NUMERIC_COMPARISON_FAILED)
            simple->remote_confirm = 'f';
        switch (simple->remote_confirm) {
        case 's':
            switch (simple->local_confirm) {
            case 's':
                lmp_dhkey_check_accept(conn);
                *step = LMP_WAIT_ACCEPTED_DHKEY_CHECK;
                conn->core.enc_state |= 0x400;
                hci_send_event(54, "1A", 0, conn->core_s.remote_addr);
                break;
            case 'f':
                lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 5);
                hci_send_event(54, "1A", 5, conn->core_s.remote_addr);
                break;
            }
            break;
        case 'f':
            if (simple->local_confirm)
                hci_send_event(54, "1A", 5, conn->core_s.remote_addr);
            break;
        }
        break;
    case LMP_WAIT_DHKEY_CHECK:
        if (!CONFIG_LMP_PASSKEY_ENABLE)
            goto wait_accepted_dhkey_check;
        if (opcode0 != PDU(DHKEY_CHECK))
            break;
        if (lmp_dhkey_check(conn, &rx->data[1])) {
            lmp_dhkey_check_accept(conn);
            *step = LMP_WAIT_ACCEPTED_DHKEY_CHECK;
            conn->core.enc_state |= 0x400;
            hci_send_event(54, "1A", 0, conn->core_s.remote_addr);
        } else {
            hci_send_event(54, "1A", 5, conn->core_s.remote_addr);
        }
        break;
    case LMP_WAIT_ACCEPTED_DHKEY_CHECK:
wait_accepted_dhkey_check:
        if (opcode1 != PDU(DHKEY_CHECK))
            break;
        if (opcode0 == PDU(NOT_ACCEPTED))
            SDK_ASSERT(0, 1494, "0 ");
        simple = conn->simple;
        if (conn->core.role == 0)
            f2_function_api(simple->local_ecdh_key, simple->local_random_num, simple->remote_random_num,
                            hci_param.addr, conn->core_s.remote_addr, conn->core.link_key);
        else
            f2_function_api(simple->local_ecdh_key, simple->remote_random_num, simple->local_random_num,
                            conn->core_s.remote_addr, hci_param.addr, conn->core.link_key);
        *step = LMP_WAIT_AU_RAND;
        conn->core.stage = LMP_STAGE_CALCULATE_LINK_KEY;
        conn->core.enc_state |= 3;
        lmp_free(conn->simple);
        conn->simple = NULL;
        break;
    }
    return 0;
}

int LMP_CODE lmp_stage_calc_link_key(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                    struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 *step = &conn->core.step;

    switch (*step) {
    case LMP_WAIT_AU_RAND:
        if (opcode0 != PDU(AU_RAND))
            break;
        make_rand_num(conn->encrypt->rand_num);
        lmp_response(conn, "1c16", PDU(AU_RAND), conn->encrypt->rand_num);
        *step = LMP_WAIT_SRES;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES))
            break;
        if (conn->encrypt->sres_mach != 1) {
            lmp_tx_detch(conn, 5);
            break;
        }
        conn->core.state |= 0x10;
        lmp_send_event_link_key_notification(conn, 0);
        *step = LMP_WAIT_ENCRYPTION_MODE_REQ;
        LOG_I("LMP_STAGE_ENC_START_BY_REMOTE");
        conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

int LMP_CODE lmp_slave_stage_enc_start_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;

    switch (core->step) {
    case LMP_WAIT_ENCRYPTION_MODE_REQ:
        if (opcode0 != PDU(ENCRYPTION_MODE_REQ))
            break;
        if (conn->enc_timer) {
            sys_timer_del(conn->enc_timer);
            conn->enc_timer = 0;
        }
        if (conn->core.role == 0) {
            core->stage = LMP_STAGE_ENC_START_BY_LOCAL;
            core->step = LMP_TX_ENCRYPTION_KEY_SIZE_REQ;
            lmp_role_machine_handler_register(conn, lmp_master_machine);
            return 2;
        }
        core->step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ACCEPT_ENCRYPTION_MODE_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(ENCRYPTION_MODE_REQ))
            break;
        core->step = LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ;
        break;
    case LMP_WAIT_ENCRYPTION_KEY_SIZE_REQ:
        if (opcode0 == PDU(ENCRYPTION_KEY_SIZE_REQ))
            core->step = LMP_WAIT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_START_ENCRYPTION_REQ:
        if (opcode0 != PDU(START_ENCRYPTION_REQ))
            break;
        core->step = LMP_TX_NULL;
        core->stage = LMP_STAGE_CONNECTED;
        LOG_I("LMP_WAIT_START_ENCRYPTION_REQ");
        lmp_send_event_encryption_change(conn);
        if (CONFIG_BTCTLER_TWS_ENABLE && !conn->core.role_switch)
            tws_phone_link_connection(conn);
        break;
    }
    return 0;
}

int LMP_CODE lmp_master_stage_refreshing_the_encryption_key(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                           u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 *step = &conn->core.step;

    switch (*step) {
    case LMP_WAIT_PAUSE_ENCRYPTION_REQ:
        if (opcode1 != PAUSE_ENCRYPTION_REQ)
            break;
        /* fall through */
    case LMP_TX_STOP_ENCRYPTION_REQ:
        LOG_D("LMP_TX_STOP_ENCRYPTION_REQ");
        lmp_tx_stop_encryption_req(conn);
        *step = LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ:
        if (opcode1 != PDU(STOP_ENCRYPTION_REQ))
            break;
        if (opcode0 == PDU(NOT_ACCEPTED))
            SDK_ASSERT(0, 1678, "0 ");
        /* fall through */
    case LMP_TX_START_ENCRYPTION_REQ:
        LOG_I("LMP_TX_START_ENCRYPTION_REQ");
        lmp_tx_start_encryption_req(conn);
        *step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        puts("====here111===");
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ))
            break;
        puts("====here===");
        *step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        lmp_tx_version_req(conn);
        break;
    }
    return 0;
}
