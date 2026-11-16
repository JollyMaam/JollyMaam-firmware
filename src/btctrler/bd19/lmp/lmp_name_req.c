/* Classic/lmp_name_req.c (included by lmp.c): remote name request link. */
#undef LMP_FILE
#define LMP_FILE "lmp_name_req.c"

int LMP_CODE lmp_name_stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                 struct bredr_rx_bulk *rx, int *hci_cmd)
{
    if (!CONFIG_LMP_NAME_REQ_ENABLE)
        return 0;
    switch (conn->core.step) {
    case LMP_TX_FEATURES_REQ:
        LOG_I("LMP_TX_FEATURES_REQ_N");
        lmp_tx_features_req(conn);
        conn->core.step = LMP_WAIT_FEATURES_RES;
        break;
    case LMP_WAIT_FEATURES_RES:
        if (opcode0 != PDU(FEATURES_RES))
            break;
        /* fall through */
    case LMP_TX_FEATURES_REQ_EXT:
        LOG_I("LMP_TX_FEATURES_REQ_EXT_N");
        lmp_tx_features_req_ext(conn, 1);
        conn->core.step = LMP_WAIT_FEATURES_RES_EXT;
        break;
    case LMP_WAIT_FEATURES_RES_EXT:
        if (opcode1 != FEATURES_RES_EXT)
            break;
        /* fall through */
    case LMP_TX_NAME_REQ:
        LOG_I("LMP_TX_NAME_REQ");
        lmp_tx_name_req(conn);
        conn->core.step = LMP_WAIT_NAME_RES;
        break;
    case LMP_WAIT_NAME_RES:
        if (opcode0 != PDU(NAME_RES) || conn->remote_name)
            break;
        /* fall through */
    case LMP_TX_DETACH:
        LOG_I("LMP_TX_DETACH_N");
        lmp_tx_detch(conn, 0x13);
        conn->core.step = LMP_TX_NULL;
        break;
    }
    return 0;
}

int LMP_CODE lmp_name_req_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 opcode0, opcode1, opcode2;

    if (!CONFIG_LMP_NAME_REQ_ENABLE)
        return 0;
    opcode0 = 0;
    opcode1 = 0;
    opcode2 = 0;
    if (rx)
        lmp_acl_c_handler(conn, rx, &opcode0, &opcode1, &opcode2);
    switch (conn->core.stage) {
    case LMP_STAGE_CONN:
        lmp_name_stage_conn(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
        break;
    default:
        break;
    }
    return 0;
}

void LMP_CODE lmp_name_req_init(struct lmp_connection *conn)
{
    int hci_cmd;

    if (!CONFIG_LMP_NAME_REQ_ENABLE)
        return;
    conn->core.stage = LMP_STAGE_CONN;
    conn->core.step = LMP_TX_FEATURES_REQ;
    lmp_name_req_machine(conn, NULL, &hci_cmd);
}
