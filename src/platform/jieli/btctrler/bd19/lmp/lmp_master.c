/* Classic/lmp_master.c (included by lmp.c): the central's link setup. */
#undef LMP_FILE
#define LMP_FILE "lmp_master.c"

void LMP_CODE lmp_tx_set_afh_req(struct lmp_connection *conn, u8 *map)
{
    u32 instant, clk;

    bredr_read_slot_clk();
    clk = bredr_get_master_slot_clk(conn->link);
    if (conn->core.role != 0)
        return;
    if (!(conn->core_s.remote_features[4] & 8))
        return;
    instant = (clk + 200) & 0x0ffffffe;
    if (instant > 0x7ffffff)
        instant = (instant + 0x8000000) & 0x0ffffffe;
    link_conn_set_afh_enable(conn->link, map, instant);
    lmp_request(conn, "A141C010", PDU(SET_AFH), instant, 1, map);
}

void LMP_CODE lmp_tx_set_afh(struct lmp_connection *conn, u8 mode)
{
    u8 map[10];

    if (!(config_bt_function & 2))
        return;
    if (conn->core.role != 0)
        return;
    if (bredr_link_update_afh(conn->link, map, conn->afh_report.tx_channel_classification, mode)) {
        LOG_I("tx afh \n");
        lmp_tx_set_afh_req(conn, map);
    }
}

int LMP_CODE lmp_master_stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                   struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;

    switch (core->step) {
    case LMP_TX_GET_REMOTE_ADDR_REQ:
        if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE) {
            lmp_tx_get_remote_addr_req(conn);
            core->step = LMP_WAIT_GET_REMOTE_ADDR_RES;
            break;
        }
        goto tx_features_req;
    case LMP_WAIT_GET_REMOTE_ADDR_RES:
        if (opcode0 != 0x85 && CONFIG_BTCTLER_FAST_CONNECT_ENABLE)
            break;
        /* fall through */
    case LMP_TX_FEATURES_REQ:
tx_features_req:
        LOG_I("LMP_TX_FEATURES_REQ");
        lmp_tx_features_req(conn);
        core->step = LMP_WAIT_FEATURES_RES;
        break;
    case LMP_WAIT_FEATURES_RES:
        if (opcode0 != PDU(FEATURES_RES))
            break;
        /* fall through */
    case LMP_TX_VERSION_REQ:
        LOG_I("LMP_TX_VERSION_REQ");
        lmp_tx_version_req(conn);
        core->step = LMP_WAIT_VERSION_RES;
        break;
    case LMP_WAIT_VERSION_RES:
        if (opcode0 != PDU(VERSION_RES))
            break;
        if (!(conn->core_s.remote_features[7] & 0x80)) {
            core->step = LMP_TX_CONNECTION_REQ;
            return 1;
        }
        /* fall through */
    case LMP_TX_FEATURES_REQ_EXT:
        LOG_I("LMP_TX_FEATURES_REQ_EXT");
        lmp_tx_features_req_ext(conn, 1);
        core->step = LMP_WAIT_FEATURES_RES_EXT;
        break;
    case LMP_WAIT_FEATURES_RES_EXT:
        if (opcode1 != FEATURES_RES_EXT)
            break;
        /* fall through */
    case LMP_TX_CONNECTION_REQ:
        LOG_I("LMP_TX_CONNECTION_REQ");
        lmp_tx_connection_req(conn);
        core->step = LMP_WAIT_ACCEPT_CONNECTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_CONNECTION_REQ:
        if (opcode1 != PDU(HOST_CONNECTION_REQ))
            break;
        if (opcode0 == PDU(NOT_ACCEPTED)) {
            LOG_E("LMP_HOST_CONNECTION_REQ : LMP_NOT_ACCEPTED");
            break;
        }
        core->state |= 0x4000;
        if (!config_btctler_hci_standard) {
            if (core->role == 0) {
                LOG_I("emitter_or_receiver %x\n", hci_param.emitter_or_receiver);
                if (hci_param.emitter_or_receiver)
                    core->role_switch = 0;
            } else {
                if (core->state & 0x40) {
                    lmp_tx_max_slot(conn);
                    lmp_send_event_link_request(conn);
                    core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
                } else {
                    core->step = LMP_WAIT_SETUP_COMPLETE;
                }
                break;
            }
        }
        /* fall through */
    case LMP_WAIT_SETUP_COMPLETE:
        if (core->step == LMP_WAIT_SETUP_COMPLETE) {
            if (opcode0 != PDU(SETUP_COMPLETE)) {
                puts("not LMP_SETUP_COMPLET \n ");
                break;
            }
            lmp_tx_max_slot(conn);
            lmp_send_event_link_request(conn);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
            break;
        }
        /* fall through */
    case LMP_TX_SUPERVISION_TIMEOUT:
        LOG_I("LMP_TX_SUPERVISION_TIMEOUT");
        lmp_tx_supervision_timeout(conn);
        /* fall through */
    case LMP_TX_TIMING_ACCURACY:
        LOG_I("LMP_TX_TIMING_ACCURACY");
        lmp_tx_timing_accuracy_req(conn);
        core->step = LMP_WAIT_TIMING_ACCURACY_RES;
        break;
    case LMP_WAIT_TIMING_ACCURACY_RES:
    case LMP_TX_MAX_SLOT:
        LOG_I("LMP_TX_MAX_SLOT");
        lmp_tx_max_slot(conn);
        /* fall through */
    case LMP_TX_MAX_SLOT_REQ:
        LOG_I("LMP_TX_MAX_SLOT_REQ");
        lmp_tx_max_slot_req(conn, link_conn_get_max_slot(conn->link));
        core->step = LMP_WAIT_ACCEPT_MAX_SLOT_REQ;
        break;
    case LMP_WAIT_ACCEPT_MAX_SLOT_REQ:
        if (opcode1 != PDU(MAX_SLOT_REQ))
            break;
        /* fall through */
    case LMP_TX_PACKET_TYPE_TABLE_REQ:
        if (link_conn_get_ptt(conn->link) == 1) {
            core->step = LMP_TX_HCI_EVENT_LINK_KEY_REQ;
            return 1;
        }
        lmp_tx_packet_type_table_req(conn);
        core->step = LMP_WAIT_ACCEPT_PACKET_TYPE_TABLE_REQ;
        break;
    case LMP_WAIT_ACCEPT_PACKET_TYPE_TABLE_REQ:
        core->step = LMP_TX_HCI_EVENT_LINK_KEY_REQ;
        /* fall through */
    case LMP_TX_HCI_EVENT_LINK_KEY_REQ:
        if (config_btctler_bredr_master) {
            core->role_switch = 0;
        } else if (core->role_switch) {
            core->role_switch = 0;
            if (!conn->rs_process || !conn->rs_process->instant) {
                lmp_master_tx_role_switch_req(conn);
                core->step = LMP_WAIT_ROLE_SWITCH_COMPLETE;
            }
            break;
        }
        if (!conn->rs_process)
            *hci_cmd = 0x0411;
        if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            LOG_I("LMP_SWITCH_REQ NOT_ACCEPT and LMP_TX_HCI_EVENT_LINK_KEY_REQ");
            core->enc_state |= 0x100;
            hci_send_event(0x17, "A", conn->core_s.remote_addr);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        if (!conn->rs_process && *hci_cmd == 0x0411) {
            LOG_I("LMP_TX_HCI_EVENT_LINK_KEY_REQ");
            lmp_send_event_link_request(conn);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        break;
    case LMP_WAIT_ROLE_SWITCH_COMPLETE:
        LOG_I(" LMP_WAIT_ROLE_SWITCH_COMPLETE ");
        if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            LOG_I("LMP_SWITCH_REQ NOT_ACCEPT and LMP_TX_HCI_EVENT_LINK_KEY_REQ");
            core->enc_state |= 0x100;
            hci_send_event(0x17, "A", conn->core_s.remote_addr);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        break;
    case LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY:
        switch (*hci_cmd) {
        case 0x040b: {
            u16 was = core->enc_state;

            core->enc_state = was | 1;
            if (was & 0x80) {
                memcpy(core->link_key, hci_cmd + 1, 16);
                core->step = LMP_TX_SRES;
                core->stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE;
            } else {
                core->step = LMP_TX_AU_RAND;
                core->stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL;
                memcpy(core->link_key, hci_cmd + 1, 16);
            }
            return 1;
        }
        case 0x040c:
            if (lmp_simple_pair_enbale(conn)) {
                LOG_I("start simple_pair");
                core->step = LMP_TX_IO_CAPABILITY_REQ;
                core->stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL;
            } else {
                LOG_I("start pin code");
                core->step = LMP_TX_PIN_CODE_REQ;
                core->stage = LMP_STAGE_AUTH_WITH_PIN_CODE_BY_LOCAL;
            }
            return 1;
        }
        break;
    case LMP_TX_NULL:
        LOG_I("LMP_WAIT_NULL");
        break;
    }
    return 0;
}

int LMP_CODE lmp_master_stage_connected(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                        struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;

    switch (core->step) {
    case LMP_TX_NULL:
        if (core->role_switch) {
            core->role_switch = 0;
            if (core->enc_state & 4) {
                core->step = LMP_TX_PAUSE_ENCRYPTION_REQ;
                core->stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
                printf("role_switch_on_enc 1");
                return 1;
            }
            lmp_master_tx_role_switch_req(conn);
            core->step = LMP_WAIT_SLOT_OFFSET;
            printf("role_switch_on_enc 2");
        }
        switch (opcode0) {
        case PDU(AU_RAND):       /* sic: 22, not LMP_stop_encryption_req */
            lmp_free_encrypt(conn);
            break;
        case PDU(SCO_LINK_REQ):
            LOG_I("LMP_SCO_LINK_REQ\n");
            if (CONFIG_LMP_MASTER_ESCO_ENABLE) {
                core->step = LMP_TX_HCI_EVENT_CONNECTION_REQ;
                core->stage = LMP_STAGE_SCO_CONN_BY_REMOTE;
            }
            return 1;
        case PDU(ESCAPE_4):
            if (!CONFIG_LMP_MASTER_ESCO_ENABLE)
                break;
            switch (rx->data[1]) {
            case ESCO_LINK_REQ:
                core->step = LMP_TX_HCI_EVENT_CONNECTION_REQ;
                core->stage = LMP_STAGE_ESCO_CONN_BY_REMOTE;
                return 1;
            case NOT_ACCEPTED_EXT:
                if (rx->data[2] == ESCO_LINK_REQ) {
                    LOG_I("LMP_not_ACCEPTED_EXT esco\n");
                    core->step = LMP_WAIT_REMOVE_ESCO_LINK_RES;
                    core->stage = LMP_STAGE_CONNECTED;
                }
                return 0;
            }
            break;
        }
        break;
    case LMP_WAIT_SLOT_OFFSET:
        switch (opcode0) {
        case PDU(SLOT_OFFSET):
            core->step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
            break;
        case PDU(NOT_ACCEPTED):
            if (opcode1 == PDU(SWITCH_REQ)) {
                conn->core.role_switch = 0;
                core->step = LMP_TX_NULL;
            }
            break;
        }
        break;
    case LMP_TX_STOP_ENCRYPTION_REQ:
        LOG_I("LMP_TX_STOP_ENCRYPTION_REQ");
        lmp_response(conn, "ev1", 1, 8, PDU(STOP_ENCRYPTION_REQ));
        core->step = LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ:
        if (opcode1 != PDU(STOP_ENCRYPTION_REQ)) {
            core->step = LMP_TX_NULL;
            break;
        }
        if (opcode0 == PDU(NOT_ACCEPTED))
            SDK_ASSERT(0, 387, "0 ");
        break;
    }
    if (hci_cmd && CONFIG_LMP_MASTER_ESCO_ENABLE && *hci_cmd == 0x0428) {
        LOG_I("lmp setup esco connect\n");
        if ((hci_param.features[3] & 0x80) && (conn->core_s.remote_features[3] & 0x80)) {
            core->step = LMP_TX_ESCO_LINK_REQ;
            core->stage = LMP_STAGE_ESCO_CONN_BY_REMOTE;
        } else {
            core->step = LMP_TX_SCO_LINK_REQ;
            core->stage = LMP_STAGE_SCO_CONN_BY_REMOTE;
        }
        return 1;
    }
    return 0;
}

int LMP_CODE lmp_master_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 opcode0, opcode1, opcode2;
    int ret = 0;

    opcode0 = 0;
    opcode1 = 0;
    opcode2 = 0;
    if (rx)
        lmp_acl_c_handler(conn, rx, &opcode0, &opcode1, &opcode2);
    do {
        switch (conn->core.stage) {
        case LMP_STAGE_CONN:
            ret = lmp_master_stage_conn(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_PIN_CODE_BY_LOCAL:
            lmp_stage_auth_with_pin_code(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL:
            ret = lmp_stage_auth_with_simple_pair_by_local(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE:
            lmp_stage_auth_with_simple_pair_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL:
            lmp_stage_auth_with_link_key_by_local(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_ENC_START_BY_LOCAL:
            ret = lmp_master_stage_enc_start_by_local(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE:
            ret = lmp_stage_auth_with_link_key_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_ENC_START_BY_REMOTE:
            ret = lmp_slave_stage_enc_start_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_CONNECTED:
            ret = lmp_master_stage_connected(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            break;
        case LMP_STAGE_ROLE_SWITCH_ON_ENC:
            lmp_master_stage_role_switch_on_enc(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_REFRESHING_ENCRYPTION_KEY_BY_LOCAL:
            lmp_master_stage_refreshing_the_encryption_key(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_SCO_CONN_BY_REMOTE:
            if (!CONFIG_LMP_MASTER_ESCO_ENABLE)
                break;
            lmp_slave_sco_conn_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_ESCO_CONN_BY_REMOTE:
            if (!CONFIG_LMP_MASTER_ESCO_ENABLE)
                break;
            lmp_slave_esco_conn_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        default:
            break;
        }
    } while (ret == 1);     /* SDK: an unhandled stage keeps ret, so 1 spins */
    return ret;
}

void LMP_CODE lmp_master_init(struct lmp_connection *conn)
{
    int hci_cmd = 0;

    if (!config_btctler_hci_standard && (hci_param.features[0] & 0x20))
        conn->core.role_switch = 1;
    conn->core.stage = LMP_STAGE_CONN;
    conn->core.step = CONFIG_BTCTLER_FAST_CONNECT_ENABLE ? LMP_TX_GET_REMOTE_ADDR_REQ : LMP_TX_FEATURES_REQ;
    lmp_master_machine(conn, NULL, &hci_cmd);
}
