/* Classic/lmp_esco.c (included by lmp.c): SCO/eSCO link setup, the
 * forwarded-eSCO variant and the media (sco packet) private API. */
#undef LMP_FILE
#define LMP_FILE "lmp_esco.c"

void free(void *p);
void *malloc(unsigned long size);
void bredr_esco_standard_tws_link_open(struct link_connection *lc, struct lmp_esco_pdu *pdu, int link_type, int en);

u8 LMP_CODE lmp_create_esco_hci_handle(u8 link_type)
{
    static u8 hci_hdl LMP_DATA = 1;

    hci_hdl = hci_hdl > 10 ? 1 : hci_hdl + 1;
    return hci_hdl;
}

struct lmp_esco *LMP_CODE lmp_esco_conn_malloc(struct lmp_connection *conn)
{
    struct lmp_esco *esco;

    SDK_ASSERT(conn->esco_conn == NULL, 54, "conn->esco_conn == NULL ");
    if (CONFIG_ESCO_MUX_RX_BULK_ENABLE)
        conn->esco_conn = malloc(32);
    else
        conn->esco_conn = lmp_malloc(32);
    SDK_ASSERT(conn->esco_conn, 61, "conn->esco_conn ");
    memset(conn->esco_conn, 0, 32);
    esco = conn->esco_conn;
    esco->esco_conn_timer = 0xff;
    esco->acl_link = conn;
    esco->hci_handle = lmp_create_esco_hci_handle(0);
    return esco;
}

/* lmp_esco.c ~73: answer a remote SCO request, as central with our handle. */
INLINE void lmp_tx_sco_link_res(struct lmp_connection *conn)
{
    struct lmp_esco *esco = conn->esco_conn;
    struct lmp_esco_pdu *pdu = &esco->pdu;

    LOG_I("setup sco --->role %d\n", conn->core.role);
    if (conn->core.role == 0) {
        if (bredr_read_slot_clk() & 0x4000000)
            pdu->timing_flags |= 2;
        conn->esco_conn->hci_handle = lmp_create_esco_hci_handle(0);
        pdu->handle = conn->esco_conn->hci_handle;
        pdu->lt_addr = link_conn_get_ltaddr(conn->link);
    }
    lmp_response(conn, "b07", PDU(SCO_LINK_REQ), pdu->handle, pdu->timing_flags, pdu->D_sco, pdu->T_sco, 2,
                 pdu->air_mode);
}

/* lmp_esco.c ~103: request an HV3 SCO link. */
INLINE void lmp_tx_sco_link_req(struct lmp_connection *conn)
{
    struct lmp_esco *esco = conn->esco_conn;
    struct lmp_esco_pdu *pdu;

    if (esco == NULL) {
        esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
        esco->link_type = 0;
        esco->state = 2;
    } else if (esco->acl_link != conn) {
        lmp_response(conn, "11", PDU(NOT_ACCEPTED), SCO_LINK_REQ);
        return;
    }
    pdu = &esco->pdu;
    pdu->handle = 0;
    pdu->lt_addr = 0;
    pdu->timing_flags = 0;
    pdu->D_sco = 0;
    pdu->T_sco = 6;
    pdu->W_sco = 2;
    pdu->packet_type_m2s = 2;
    pdu->packet_type_s2m = 2;
    pdu->packet_len_m2s = 30;
    pdu->packet_len_s2m = 30;
    pdu->air_mode = 2;
    pdu->negotiation_state = 0;
    if (CONFIG_LMP_MASTER_ESCO_ENABLE && conn->core.role == 0) {
        if (bredr_read_slot_clk() & 0x4000000)
            pdu->timing_flags |= 2;
        conn->esco_conn->hci_handle = lmp_create_esco_hci_handle(0);
        pdu->handle = conn->esco_conn->hci_handle;
        pdu->lt_addr = link_conn_get_ltaddr(conn->link);
        conn->core.stage = LMP_STAGE_SCO_CONN_BY_REMOTE;
        conn->core.step = LMP_WAIT_SCO_ACCEPT;
    }
    lmp_request(conn, "b07", PDU(SCO_LINK_REQ), pdu->handle, pdu->timing_flags, pdu->D_sco, pdu->T_sco, 2,
                pdu->air_mode);
}

void LMP_CODE lmp_setup_esco(struct lmp_connection *conn, u8 *param, u8 unused)
{
    struct lmp_esco *esco;
    struct lmp_esco_pdu *pdu;
    u8 air = 2, ev3_2m = 0;

    if (param)
        air = (param[12] & 3) == 3 ? 3 : 2;
    esco = conn->esco_conn;
    if (esco == NULL) {
        esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
        esco->link_type = 2;
        esco->state = 2;
    } else if (esco->acl_link != conn) {
        lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, 0x0a);
        return;
    }
    if (hci_param.features[5] & 0x20)
        ev3_2m = (conn->core_s.remote_features[5] & 0x20) != 0;
    pdu = &esco->pdu;
    pdu->handle = 0;
    pdu->lt_addr = 0;
    pdu->timing_flags = 0;
    pdu->D_sco = 0;
    if (ev3_2m) {
        pdu->T_sco = 12;
        pdu->W_sco = config_btctler_le_master_multilink ? 2 : 4;
        pdu->packet_type_m2s = 38;
        pdu->packet_type_s2m = 38;
        pdu->packet_len_m2s = 60;
        pdu->packet_len_s2m = 60;
    } else {
        pdu->T_sco = 6;
        pdu->W_sco = 2;
        pdu->packet_type_m2s = 7;
        pdu->packet_type_s2m = 7;
        pdu->packet_len_m2s = 30;
        pdu->packet_len_s2m = 30;
    }
    pdu->air_mode = air;
    pdu->negotiation_state = 0;
    if (CONFIG_LMP_MASTER_ESCO_ENABLE && conn->core.role == 0) {
        if (bredr_read_slot_clk() & 0x4000000)
            pdu->timing_flags |= 2;
        conn->esco_conn->hci_handle = lmp_create_esco_hci_handle(0);
        pdu->handle = conn->esco_conn->hci_handle;
        pdu->lt_addr = esco_creart_lt_addr();
        conn->esco_conn->state = 2;
        conn->core.step = LMP_WAIT_NEW_ESCO_PARAMENTERS;
        conn->core.stage = LMP_STAGE_ESCO_CONN_BY_REMOTE;
    }
    lmp_request(conn, "b102211", 0xfe, ESCO_LINK_REQ, pdu->handle, pdu->lt_addr, pdu->timing_flags, pdu->D_sco,
                pdu->T_sco, pdu->W_sco, pdu->packet_type_m2s, pdu->packet_type_s2m, pdu->packet_len_m2s,
                pdu->packet_len_s2m, pdu->air_mode, 0);
}

int LMP_CODE lmp_hci_disconnect_sync_connection(int handle, u8 reason)
{
    struct lmp_connection *conn;
    struct lmp_esco *esco;

    if (handle == 0)
        return 1;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        esco = conn->esco_conn;
        if (esco == NULL || esco->hci_handle != handle)
            continue;
        if (esco->link_type == 2) {
            lmp_request(conn, "b04", 0xfe, REMOVE_ESCO_LINK_REQ, esco->pdu.handle, 0x16);
            conn->core.step = LMP_WAIT_REMOVE_ESCO_LINK_RES;
        } else {
            lmp_request(conn, "b03", PDU(REMOVE_SCO_LINK_REQ), esco->pdu.handle, 0x16);
            conn->core.step = LMP_WAIT_REMOVE_SCO_LINK_RES;
        }
    }
    return 0;
}

void LMP_CODE lmp_esco_link_removed(struct lmp_connection *conn, u8 reason)
{
    if (conn->esco_conn == NULL)
        return;
    if (reason) {
        LOG_I("lmp_esco_link_removed=%d\n", reason);
        hci_send_event(5, "1H11A", 0, conn->esco_conn->hci_handle, reason, conn->esco_conn->link_type,
                       conn->core_s.remote_addr);
    }
    local_irq_disable();
    if (conn->esco_conn->esco_conn_timer != 0xff) {
        slot_timer_put(conn->esco_conn->esco_conn_timer);
        conn->esco_conn->esco_conn_timer = 0xff;
    }
    if (CONFIG_ESCO_MUX_RX_BULK_ENABLE)
        free(conn->esco_conn);
    else
        lmp_free(conn->esco_conn);
    conn->esco_conn = NULL;
    local_irq_enable();
    conn->core.stage = LMP_STAGE_CONNECTED;
    conn->core.esco_state = 0;
    LOG_I("free_esco_conn\n");
    if (CONFIG_ESCO_MUX_RX_BULK_ENABLE)
        check_exit_esco_rx_bulk_change(conn);
}

void LMP_CODE lmp_rx_sco_link_req(struct lmp_connection *conn, u8 *data)
{
    static const u16 packet_len[3] = { 10, 20, 30 };
    struct lmp_esco *esco;

    conn->core.step = LMP_TX_NULL;
    conn->core.stage = LMP_STAGE_CONNECTED;
    conn->core.esco_state = 0;
    esco = conn->esco_conn;
    if (esco == NULL) {
        esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
        esco->link_type = 0;
        esco->state = 1;
    }
    esco->pdu.handle = data[0];
    esco->pdu.timing_flags = data[1];
    esco->pdu.D_sco = data[2];
    esco->pdu.T_sco = data[3];
    esco->pdu.packet_type_m2s = data[4];
    esco->pdu.packet_type_s2m = data[4];
    esco->pdu.air_mode = data[5];
    esco->pdu.packet_len_m2s = packet_len[data[4]];
    esco->pdu.packet_len_s2m = packet_len[data[4]];
}

void LMP_CODE lmp_event_esco_link_setup(struct lmp_connection *conn)
{
    struct lmp_esco *esco = conn->esco_conn;
    struct lmp_esco_pdu *pdu = &esco->pdu;

    LOG_I("esco_link_setup: %d\n", esco->pdu.air_mode);
    if (conn->link->esco) {
        LOG_I("<<<<<<<<<have link_esco return\n");
        return;
    }
    lmp_hci_exit_sniff_mode(conn);
    if (ESCO_FORWARD_ENABLE && conn->esco_conn->tws_esco_forward)
        bredr_esco_standard_tws_link_open(conn->link, pdu, conn->esco_conn->link_type, 1);
    else
        bredr_esco_link_open(conn->link, pdu, conn->esco_conn->link_type);
    SDK_ASSERT(esco->pdu.air_mode > 1, 391, "pdu->air_mode > 1 ");
    esco = conn->esco_conn;
    if (esco->link_type == 0)
        lmp_send_event_connection_complete(conn, 0, esco->hci_handle, 0);
    else
        hci_send_event(44, "1HA111221", 0, esco->hci_handle, conn->core_s.remote_addr, 2, esco->pdu.T_sco,
                       esco->pdu.W_sco, esco->pdu.packet_len_m2s, esco->pdu.packet_len_s2m, esco->pdu.air_mode);
    if (CONFIG_BTCTLER_TWS_ENABLE)
        tws_lmp_esco_link_setup(1);
}

int LMP_CODE lmp_esco_negotiation_parameters(struct lmp_connection *conn)
{
    struct lmp_esco *esco = conn->esco_conn;
    u8 max, changed = 0;

    if (esco == NULL) {
        LOG_I("\n<<<<<<<<<<<<<<<<<<<<<<<<<waring lmp_esco_negotiation_parameters>>>>>>>>>>>>>>>\n");
        return 0;
    }
    if (esco->pdu.packet_type_s2m == 7) {
        max = 2;
    } else if (CONFIG_BTCTLER_TWS_ENABLE && tws_esco_transmit_deal(1, NULL)) {
        max = 4;
    } else {
        max = CONFIG_BTCTLER_TWS_ENABLE ? 4 : 6;
        if (link_conn_num_more_than_one())
            max = 4;
    }
    LOG_I("esco_negotiation: Wsco %d, Max:%d,lt_addr %d\n", esco->pdu.W_sco, max, esco->pdu.lt_addr);
    if (esco->pdu.W_sco > max) {
        esco->pdu.W_sco = max;
        changed = 1;
    }
    if ((esco->pdu.packet_type_m2s & ~1) == 12 && esco->pdu.T_sco != 16) {
        esco->pdu.T_sco = 16;
        esco->pdu.packet_len_m2s = 80;
        esco->pdu.packet_len_s2m = 80;
    } else if (!changed) {
        return 0;
    }
    lmp_response(conn, "b102211", 0xfe, ESCO_LINK_REQ, esco->pdu.handle, esco->pdu.lt_addr, esco->pdu.timing_flags,
                 esco->pdu.D_sco, esco->pdu.T_sco, esco->pdu.W_sco, esco->pdu.packet_type_m2s,
                 esco->pdu.packet_type_s2m, esco->pdu.packet_len_m2s, esco->pdu.packet_len_s2m, esco->pdu.air_mode, 1);
    return 1;
}

u8 LMP_CODE tws_fast_setup_esco_link(struct lmp_connection *conn, struct lmp_esco_pdu *pdu, u8 setup)
{
    struct lmp_esco *esco = conn->esco_conn;

    if (esco) {
        memcpy(&esco->pdu, pdu, 14);
        if (setup)
            bredr_link_event(conn->link, 1, 9);
        return 0;
    }
    esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
    esco->link_type = 2;
    esco->state = 1;
    memcpy(&esco->pdu, pdu, 14);
    lmp_send_event_connection_request(conn, 2);
    conn->core.stage = LMP_STAGE_ESCO_CONN_BY_REMOTE;
    conn->core.step = LMP_WAIT_HOST_ACCEPT_CONNECTION_REQ;
    return 0;
}

u8 LMP_CODE remove_esco_link(struct lmp_connection *conn, u8 reason)
{
    LOG_I("esco release");
    bredr_esco_link_close(conn->link);
    lmp_connection_ctl_slot(conn, 5);
    lmp_send_max_slot(conn, 5);
    lmp_esco_link_removed(conn, reason);
    return 0;
}

void LMP_CODE lmp_rx_esco_link_req(struct lmp_connection *conn, u8 *data)
{
    struct lmp_esco *esco;
    u8 f, T, ptype;

    if (CONFIG_BTCTLER_TWS_ENABLE) {
        conn->rx_esco_req_after_remove_esco = 0;
        if (tws_link.link && tws_link.post_step) {
            printf("remov esco ing tws_link.post_step=%d\n", tws_link.post_step);
            conn->rx_esco_req_after_remove_esco = 1;
            tws_post_remove_esco_link(tws_conn.lmp_conn);
        }
    }
    esco = conn->esco_conn;
    if (esco == NULL) {
        if (!conn->rejust_esco_req)
            bredr_rx_bulk_resume_wait();
        esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
        esco->link_type = 2;
        esco->state = 1;
    }
    esco->rx_esco_tran_id = conn->tran_id;
    conn->core.stage = LMP_STAGE_CONNECTED;
    conn->core.step = LMP_TX_NULL;
    conn->core.esco_state = 0;
    esco->pdu.handle = data[0];
    esco->pdu.lt_addr = data[1];
    esco->pdu.timing_flags = data[2];
    esco->pdu.D_sco = data[3];
    T = data[4];
    esco->pdu.T_sco = T;
    esco->pdu.W_sco = data[5];
    esco->pdu.packet_type_m2s = data[6];
    ptype = data[7];
    esco->pdu.packet_type_s2m = ptype;
    esco->pdu.packet_len_m2s = data[9] << 8 | data[8];
    esco->pdu.packet_len_s2m = data[11] << 8 | data[10];
    esco->pdu.air_mode = data[12];
    esco->pdu.negotiation_state = data[13];
    if (conn->rejust_esco_req) {
        lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, 0x20);
    } else {
        f = hci_param.features[5];
        if ((ptype == 38 && !(f & 0x20)) || (ptype == 55 && !(f & 0x40)) ||
            (ptype == 44 && !(f & 0x20)) || (ptype == 61 && !(f & 0x40))) {
            LOG_I("not support 2mEV3");
            lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, 0x1a);
        } else if (ptype == 7 && T != 6) {
            LOG_I("esco_pdu.conn->T_sco != 6\n");
            lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, 0x20);
        } else {
            return;
        }
    }
    conn->esco_conn->state = 4;
}

int LMP_CODE lmp_slave_sco_conn_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                         struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 *step = &conn->core.step;

    switch (*step) {
    case LMP_TX_SCO_LINK_REQ:
        LOG_I("setup SCO");
        conn->core.stage = LMP_STAGE_CONNECTED;
        *step = LMP_WAIT_REMOVE_SCO_LINK_REQ;
        lmp_tx_sco_link_req(conn);
        break;
    case LMP_WAIT_SCO_ACCEPT:
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE)
            break;
        LOG_I("LMP_WAIT_SCO_ACCEPT\n");
        conn->esco_conn->state = 3;
        *step = LMP_WAIT_REMOVE_SCO_LINK_REQ;
        bredr_link_event(conn->link, 1, 9);
        break;
    case LMP_TX_HCI_EVENT_CONNECTION_REQ:
        lmp_send_event_connection_request(conn, 0);
        *step = LMP_WAIT_ACCEPT_CONNECTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_CONNECTION_REQ:
        LOG_I(" LMP_WAIT_ACCEPT_CONNECTION_REQ\n");
        if (CONFIG_LMP_MASTER_ESCO_ENABLE && conn->core.role == 0) {
            lmp_tx_sco_link_res(conn);
            break;
        }
        if (conn->rejust_esco_req) {
            hci_cmd[0] = 0x040a;
            hci_cmd[1] = 0x1e;
        } else {
            switch (*hci_cmd) {
            case 0x0409:
                lmp_response(conn, "V11", 9, PDU(ACCEPTED), SCO_LINK_REQ);
                conn->esco_conn->state = 3;
                *step = LMP_WAIT_REMOVE_SCO_LINK_REQ;
                return 0;
            case 0x040a:
                break;
            default:
                return 0;
            }
        }
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SCO_LINK_REQ, hci_cmd[1]);
        lmp_esco_link_removed(conn, 0);
        break;
    case LMP_WAIT_REMOVE_SCO_LINK_REQ:
        if (opcode0 != PDU(REMOVE_SCO_LINK_REQ))
            break;
        lmp_response(conn, "11", PDU(ACCEPTED), REMOVE_SCO_LINK_REQ);
        bredr_esco_link_close(conn->link);
        lmp_connection_ctl_slot(conn, 5);
        lmp_send_max_slot(conn, 5);
        lmp_esco_link_removed(conn, rx->data[1]);
        break;
    case LMP_WAIT_REMOVE_SCO_LINK_RES:
        if (opcode0 != PDU(ACCEPTED) || opcode1 != PDU(REMOVE_SCO_LINK_REQ))
            break;
        LOG_I(" LMP_WAIT_REMOVE_SCO_LINK_RES ");
        bredr_esco_link_close(conn->link);
        lmp_connection_ctl_slot(conn, 5);
        lmp_send_max_slot(conn, 5);
        lmp_esco_link_removed(conn, rx->data[1]);
        break;
    }
    return 0;
}

u8 LMP_CODE creat_esco_forward_standard(struct lmp_connection *conn)
{
    struct lmp_connection *other;
    struct lmp_esco *oesco, *esco;
    struct lmp_esco_pdu *pdu;
    u8 lt;

    if (conn == NULL || !link_conn_num_more_than_one())
        return 0;
    lmp_hci_exit_sniff_mode(conn);
    other = lmp_get_other_conn(conn);
    oesco = other->esco_conn;
    if (conn->esco_conn) {
        printf("tws_conn->esco_conn->tws_esco_forward=0x%x\n", conn->esco_conn->tws_esco_forward);
        return 1;
    }
    esco = conn->esco_conn = lmp_esco_conn_malloc(conn);
    esco->link_type = 2;
    esco->state = 2;
    esco->tws_esco_forward = 1;
    memcpy(&esco->pdu, &other->esco_conn->pdu, 14);
    esco = conn->esco_conn;
    pdu = &esco->pdu;
    if (bredr_read_slot_clk() & 0x4000000)
        pdu->timing_flags |= 2;
    conn->esco_conn->hci_handle = lmp_create_esco_hci_handle(0);
    pdu->handle = conn->esco_conn->hci_handle;
    lt = esco_creart_lt_addr();
    pdu->lt_addr = lt;
    pdu->W_sco = 6 - oesco->pdu.W_sco;
    pdu->D_sco = oesco->pdu.W_sco + 4 + oesco->pdu.D_sco;
    if (lt & 8)
        puts("<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<pdu->tws_lt_addr err>>>>>>>>>>>>>>>>>>>\n");
    conn->esco_conn->state = 2;
    conn->core.step = LMP_WAIT_NEW_ESCO_PARAMENTERS;
    conn->core.stage = LMP_STAGE_ESCO_CONN_BY_REMOTE;
    lmp_request(conn, "b102211", 0xfe, ESCO_LINK_REQ, pdu->handle, pdu->lt_addr, pdu->timing_flags, pdu->D_sco,
                pdu->T_sco, pdu->W_sco, pdu->packet_type_m2s, pdu->packet_type_s2m, pdu->packet_len_m2s,
                pdu->packet_len_s2m, pdu->air_mode, 0);
    return 1;
}

/* lmp_esco.c ~247: accept a remote eSCO request as central. */
INLINE void lmp_tx_esco_link_res(struct lmp_connection *conn)
{
    struct lmp_esco *esco = conn->esco_conn;
    struct lmp_esco_pdu *pdu = &esco->pdu;

    pdu->timing_flags = 0;
    pdu->negotiation_state = 1;
    LOG_I("lmp_setup_esco %d\n", conn->core.role);
    if (conn->core.role == 0) {
        if (bredr_read_slot_clk() & 0x4000000)
            pdu->timing_flags |= 2;
        conn->esco_conn->hci_handle = lmp_create_esco_hci_handle(0);
        pdu->handle = conn->esco_conn->hci_handle;
        pdu->lt_addr = esco_creart_lt_addr();
        if (ESCO_FORWARD_ENABLE) {
            if (pdu->W_sco > 4)
                pdu->W_sco = 4;
            else if (pdu->W_sco < 3)
                pdu->W_sco = 4;
        }
        conn->esco_conn->state = 2;
    }
    lmp_response(conn, "b102211", 0xfe, ESCO_LINK_REQ, pdu->handle, pdu->lt_addr, pdu->timing_flags, pdu->D_sco,
                 pdu->T_sco, pdu->W_sco, pdu->packet_type_m2s, pdu->packet_type_s2m, pdu->packet_len_m2s,
                 pdu->packet_len_s2m, pdu->air_mode, 1);
}

int LMP_CODE lmp_slave_esco_conn_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                          struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 *step = &conn->core.step;
    struct lmp_esco *esco;

    switch (*step) {
    case LMP_TX_ESCO_LINK_REQ:
        LOG_I("setup ESCO");
        if (ESCO_FORWARD_ENABLE) {
            if (creat_esco_forward_standard(conn))
                break;
            printf("setup ESCO1");
        }
        conn->core.stage = LMP_STAGE_CONNECTED;
        *step = LMP_WAIT_REMOVE_ESCO_LINK_REQ;
        lmp_setup_esco(conn, (u8 *)hci_cmd, 0);
        break;
    case LMP_TX_HCI_EVENT_CONNECTION_REQ:
        if (conn->esco_conn->state == 4) {
            lmp_esco_link_removed(conn, 0);
            break;
        }
        lmp_send_event_connection_request(conn, 2);
        *step = LMP_WAIT_HOST_ACCEPT_CONNECTION_REQ;
        break;
    case LMP_WAIT_HOST_ACCEPT_CONNECTION_REQ:
        if (CONFIG_LMP_MASTER_ESCO_ENABLE && conn->core.role == 0) {
            LOG_I("LMP_HOST_ACCEPT_CONN master\n");
            lmp_tx_esco_link_res(conn);
            *step = LMP_WAIT_NEW_ESCO_PARAMENTERS;
            break;
        }
        esco = conn->esco_conn;
        if (esco && esco->rx_esco_tran_id != conn->tran_id)
            conn->tran_id = esco->rx_esco_tran_id;
        switch (*hci_cmd) {
        case 0x0429:
            if (lmp_esco_negotiation_parameters(conn)) {
                *step = LMP_WAIT_NEW_ESCO_PARAMENTERS;
                break;
            }
            LOG_I("accepted_esco_link\n");
            if (conn->core.esco_state) {
                LOG_I("slave_esco_state break\n");
                *step = LMP_WAIT_REMOVE_ESCO_LINK_REQ;
                break;
            }
            lmp_response(conn, "Vb04", 9, 0xfe, ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ);
            *step = LMP_WAIT_REMOVE_ESCO_LINK_REQ;
            break;
        case 0x040a:
            lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, hci_cmd[1]);
            lmp_esco_link_removed(conn, 0);
            break;
        }
        break;
    case LMP_WAIT_NEW_ESCO_PARAMENTERS:
        if (opcode1 == ACCEPTED_EXT && opcode2 == ESCO_LINK_REQ) {
            LOG_I("accepted_esco_link param\n");
            bredr_link_event(conn->link, 1, 9);
        } else if (opcode1 == ESCO_LINK_REQ) {
            LOG_I("reset esco_link param\n");
            lmp_response(conn, "Vb04", 9, 0xfe, ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ);
        } else {
            break;
        }
        conn->esco_conn->state = 3;
        *step = LMP_WAIT_REMOVE_ESCO_LINK_REQ;
        break;
    case LMP_WAIT_REMOVE_ESCO_LINK_REQ:
        if (opcode1 != REMOVE_ESCO_LINK_REQ)
            break;
        LOG_I("LMP_WAIT_REMOVE_ESCO_LINK_REQ\n");
        esco = conn->esco_conn;
        esco->rx_esco_tran_id = conn->tran_id;
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            conn->rx_esco_req_after_remove_esco = 0;
            if (tws_master_notice_slave_step_fun(2, rx->data[3], 0))
                break;
            esco = conn->esco_conn;
        }
        esco->state = 5;
        lmp_response(conn, "b04", 0xfe, ACCEPTED_EXT, ESCAPE_4, REMOVE_ESCO_LINK_REQ);
        remove_esco_link(conn, rx->data[3]);
        break;
    case LMP_WAIT_REMOVE_ESCO_LINK_RES:
        if (opcode1 != ACCEPTED_EXT || opcode2 != REMOVE_ESCO_LINK_REQ)
            break;
        LOG_I("LMP_WAIT_REMOVE_ESCO_LINK_RES %d", rx->data[3]);
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            conn->rx_esco_req_after_remove_esco = 0;
            if (tws_master_notice_slave_step_fun(2, rx->data[3], 0))
                break;
        }
        remove_esco_link(conn, rx->data[3]);
        break;
    }
    return 0;
}

int LMP_CODE lmp_private_send_esco_packet(void *priv, u8 *packet, int len)
{
    struct lmp_connection *conn;
    struct bredr_tx_bulk *tx;
    int n = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    if (ESCO_FORWARD_ENABLE) {
        for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
            if (conn->esco_conn)
                n += conn->esco_conn->tws_esco_forward == 1;
        }
        if (n)
            return 0;
    }
    tx = bredr_tx_bulk_alloc(len);
    if (tx == NULL) {
        LOG_I("why sco tx NULL=====\n");
        return -1;
    }
    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL || conn->link->esco == NULL)
            continue;
        if (conn->esco_conn->codec_ops)
            LOG_I("not support esco tx\n");
        tx->len = len;
        tx->llid = 4;
        memcpy(tx->data, packet, len);
        bredr_tx_bulk_push(&conn->link->esco->tx_esco, tx);
        local_irq_enable();
        return 0;
    }
    local_irq_enable();
    bredr_tx_bulk_free(tx);
    return 0;
}

void *LMP_CODE lmp_private_get_esco_packet(int *len, u32 *hash)
{
    struct lmp_connection *conn;
    struct link_connection *link;
    struct bredr_rx_bulk *rx;
    int err;

    if (!(config_btctler_modules & 1))
        return NULL;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL)
            continue;
        link = conn->link;
        if (link->esco == NULL) {
            LOG_I("W");
            continue;
        }
        if (*hash == 0xffffffff) {
            rx = bredr_find_esco_packet(link);
            if (rx) {
                *hash = rx->hash;
                *len = rx->len;
                return rx->data;
            }
            *len = 0;
            return NULL;
        }
        rx = bredr_get_esco_packet(link, &err);
        if (rx) {
            if (conn->esco_conn) {
                if (conn->esco_conn->codec_ops) {
                    LOG_I("esco codec not support yet, please check\n");
                    continue;
                }
                *len = rx->len;
                if (rx->payload_head[2] & 0x20) {
                    putchar('e');
                    memset(rx->data, 0xaa, rx->len);
                    rx->data[0] = 0x55;
                    rx->data[1] = 0x55;
                }
                *hash = rx->hash;
                return rx->data;
            }
            bredr_rx_bulk_free(rx);
        }
        if (err == -2)
            *len = -2;
        return NULL;
    }
    *len = -22;
    return NULL;
}

void LMP_CODE lmp_private_free_esco_packet(void *packet)
{
    if (!(config_btctler_modules & 1))
        return;
    local_irq_disable();
    bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)packet - offsetof(struct bredr_rx_bulk, data)));
    local_irq_enable();
}

int LMP_CODE lmp_private_get_esco_data_len(void)
{
    struct lmp_connection *conn;
    struct bredr_esco_link *esco;
    struct list_head *node;
    int len = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL || (esco = conn->link->esco) == NULL)
            continue;
        for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next) {
            if (RX_OF(node)->llid == 4)
                len += RX_OF(node)->len;
        }
        local_irq_enable();
        return len;
    }
    local_irq_enable();
    LOG_I("why esco_conn NULL 3333\n");
    return 0;
}

u32 LMP_CODE esco_media_get_packet_num(void)
{
    struct lmp_connection *conn;
    struct bredr_esco_link *esco;
    struct list_head *node;
    int num = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL)
            continue;
        esco = conn->link->esco;
        if (esco == NULL) {
            LOG_I("W");
            continue;
        }
        for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next)
            num += RX_OF(node)->llid == 4;
        local_irq_enable();
        return num;
    }
    local_irq_enable();
    return 0;
}

int LMP_CODE lmp_private_get_esco_remain_buffer_size(void)
{
    struct lmp_connection *conn;
    struct bredr_esco_link *esco;
    int size;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL || (esco = conn->link->esco) == NULL)
            continue;
        size = link_conn_rx_bulk_remain_size(conn->link, esco->pdu.packet_len_m2s + 10);
        local_irq_enable();
        return size;
    }
    local_irq_enable();
    return 0;
}

int LMP_CODE lmp_private_esco_suspend_resume(int flag)
{
    struct lmp_connection *conn;
    struct link_connection *link;
    struct bredr_esco_link *esco;
    struct list_head *node, *next;
    int n;

    if (!(config_btctler_modules & 1))
        return -1;
    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        link = conn->link;
        if (link) {
            switch (flag) {
            case 3:
                link->suspend_rx_esco = 1;
                break;
            case 4:
                link->suspend_rx_esco = 0;
                break;
            }
        }
        if (conn->esco_conn == NULL || (esco = link->esco) == NULL)
            continue;
        switch (flag) {
        case 1:
            esco->suspend_rx_data_store = 1;
            n = 0;
            for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next)
                n++;
            node = esco->rx_esco.next;
            while (n != 1 && node != &conn->link->esco->rx_esco) {
                next = node->next;
                __list_del_entry(node);
                bredr_rx_bulk_free(RX_OF(node));
                n--;
                node = next;
            }
            if (CONFIG_BTCTLER_TWS_ENABLE)
                tws_esco_suspend_resume(1);
            break;
        case 2:
            if (CONFIG_BTCTLER_TWS_ENABLE)
                tws_esco_suspend_resume(2);
            conn->link->esco->suspend_rx_data_store = 0;
            break;
        }
        local_irq_enable();
        return 0;
    }
    local_irq_enable();
    LOG_I("why NULL 44444\n");
    return -1;
}

u32 LMP_CODE lmp_private_fetch_sco_packet(void)
{
    struct lmp_connection *conn;
    struct bredr_esco_link *esco;
    struct list_head *node, *next;
    int cnt = 0, del = 0, half;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL)
            continue;
        local_irq_disable();
        esco = conn->link->esco;
        for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next)
            cnt++;
        LOG_I("esco_del");
        if (cnt < 24) {
            local_irq_enable();
            return 0;
        }
        node = conn->link->esco->rx_esco.next;
        half = cnt >> 1;
        while (del != half && node != &conn->link->esco->rx_esco) {
            next = node->next;
            __list_del_entry(node);
            bredr_rx_bulk_free(RX_OF(node));
            del++;
            node = next;
        }
        if (CONFIG_BTCTLER_TWS_ENABLE)
            tws_esco_suspend_resume(del);
        local_irq_enable();
    }
    return 0;
}

u32 LMP_CODE lmp_private_clear_sco_packet(u8 clear_num)
{
    struct lmp_connection *conn;
    struct bredr_esco_link *esco;
    struct list_head *node, *next, *first;
    int cnt = 0, del = 0;
    u8 num = clear_num;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn == NULL)
            continue;
        local_irq_disable();
        esco = conn->link->esco;
        first = esco->rx_esco.next;
        for (node = first; node != &esco->rx_esco; node = node->next)
            cnt++;
        if (cnt == 1)
            num = 1;
        node = first;
        if (del != num && first != &esco->rx_esco) {
            do {
                next = node->next;
                __list_del_entry(node);
                bredr_rx_bulk_free(RX_OF(node));
                del++;
                node = next;
            } while (del != num && node != &conn->link->esco->rx_esco);
        }
        if (CONFIG_BTCTLER_TWS_ENABLE)
            tws_esco_suspend_resume(del);
        local_irq_enable();
    }
    return 0;
}

u8 *LMP_CODE lmp_private_get_esco_conn_exist(u8 *addr)
{
    struct lmp_connection *conn;

    if (addr == NULL)
        return NULL;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        /* sic: returns the first eSCO link whose address differs */
        if (conn->esco_conn && memcmp(addr, conn->core_s.remote_addr, 6) != 0)
            return conn->core_s.remote_addr;
    }
    return NULL;
}

u8 LMP_CODE lmp_private_get_esco_conn_num(void)
{
    struct lmp_connection *conn;
    u8 num = 0;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        num += conn->esco_conn != NULL;
    return num;
}

u8 LMP_CODE lmp_get_esco_conn_statu(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn)
            return 1;
    }
    return 0;
}

__attribute__((weak)) u8 LMP_CODE is_call_now(void)
{
    return 0;
}

u8 LMP_CODE lmp_esco_sniff_check(struct lmp_connection *conn)
{
    if (conn->tran_id == conn->core.role || config_btctler_hci_standard)
        return 1;
    if (conn->esco_conn || lmp_get_esco_conn_statu())
        return 0;
    if (!sniff_establish_by_remote)
        return 0;
    return !is_call_now();
}
