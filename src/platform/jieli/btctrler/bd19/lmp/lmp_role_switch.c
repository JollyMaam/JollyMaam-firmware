/* Classic/lmp_role_switch.c (included by lmp.c): master/slave switch. */
#undef LMP_FILE
#define LMP_FILE "lmp_role_switch.c"

void free(void *p);

void LMP_CODE lmp_role_switch_misc_alloc(struct lmp_connection *conn)
{
    if (conn->rs_process)
        return;
    LOG_D("%s", "lmp_role_switch_misc_alloc");
    conn->rs_process = zalloc(20);
    SDK_ASSERT(conn->rs_process, 13, "conn->rs_process ");
    memset(conn->rs_process, 0, 20);
    conn->rs_process->timer0 = 0xff;
    conn->rs_process->timer1 = 0xff;
}

void LMP_CODE lmp_role_switch_misc_free(struct lmp_connection *conn)
{
    local_irq_disable();
    if (conn->rs_process) {
        LOG_D("%s", "lmp_role_switch_misc_free");
        if (conn->rs_process->timer0 != 0xff) {
            slot_timer_put(conn->rs_process->timer0);
            conn->rs_process->timer0 = 0xff;
        }
        if (conn->rs_process->timer1 != 0xff) {
            slot_timer_put(conn->rs_process->timer1);
            conn->rs_process->timer1 = 0xff;
        }
        free(conn->rs_process);
        conn->rs_process = NULL;
    }
    local_irq_enable();
}

void LMP_CODE lmp_role_switch_detach(struct lmp_connection *conn)
{
    lmp_role_switch_misc_free(conn);
}

void LMP_CODE lmp_role_switch_completed(struct link_connection *link, int result)
{
    struct lmp_connection *conn = lmp_conn_for_link(link);

    SDK_ASSERT(conn, 55, "conn ");
    conn->core.state &= ~0x20;
    if (result == 0) {
        link_conn_disable_afh(conn->link);
        lmp_channel_classification_close(conn);
        if (conn->core.role == 0) {
            conn->tran_id = 1;
            conn->core.role = 1;
            lc_qos_ctrl(conn->link, 0);
        } else {
            conn->tran_id = 0;
            conn->core.role = 0;
        }
        conn->core.state |= 0x2000;
        conn->rs_process->state = 2;
        conn->core.role_switch = 0;
        LOG_D("rs_succ:0x%x", conn->core.role);
    } else {
        conn->rs_process->state = 3;
        conn->core.role_switch = 1;
        LOG_D("rs_fail");
    }
    conn->rs_process->over_role_switch = 1;
    conn->role_switch_state = 0;
}

__attribute__((weak)) void LMP_CODE bdhw_enable_afh(struct ctrl_frame *frame)
{
}

void LMP_CODE lmp_role_switch_machine(struct lmp_connection *conn, int result)
{
    struct lmp_role_switch *rs;
    u8 ok;

    LOG_D("stage:%d, step:%d, lt_addr:%d", conn->core.stage, conn->core.step, link_conn_get_ltaddr(conn->link));
    if (conn->core.stage < LMP_STAGE_ROLE_SWITCH_ON_ENC) {
        if (conn->rs_process->state == 2) {
            lmp_send_event_role_change(conn, 0);
            if (conn->core.role == 0) {
                lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
                lmp_tx_supervision_timeout(conn);
                conn->core.step = LMP_WAIT_AUTH;
                if (!config_btctler_hci_standard)
                    lmp_setup_complete(conn);
                if (link_conn_get_ptt(conn->link) != 1)
                    lmp_tx_packet_type_table_req(conn);
            } else if (conn->core.state & 0x4000) {
                lmp_send_event_link_request(conn);
                conn->core.step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
            }
        } else {
            if (conn->link->afh.en == 1)
                bdhw_enable_afh(conn->link->frame);
            if (conn->core.role == 0) {
                rs = conn->rs_process;
                if (!rs->init_by_local) {
                    puts("rs_try_again");
                    lmp_master_tx_role_switch_req(conn);
                    return;
                }
                if (++rs->try_cnt < 3) {
                    printf("rs_try:%d\n", rs->try_cnt);
                    lmp_master_tx_role_switch_req(conn);
                    return;
                }
                if (conn->core.state & 0x4000) {
                    lmp_send_event_link_request(conn);
                    conn->core.step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
                }
            } else {
                lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
                conn->core.step = LMP_WAIT_AUTH;
                if (!config_btctler_hci_standard)
                    lmp_setup_complete(conn);
            }
            lmp_send_event_role_change(conn, 0x35);
        }
    } else {
        ok = conn->rs_process->state == 2;
        if (conn->core.stage == LMP_STAGE_ROLE_SWITCH_ON_ENC) {
            if (ok) {
                if (conn->core.role == 0) {
                    if (conn->core.enc_state & 0x40) {
                        conn->core.enc_state &= ~0x40;
                        lmp_tx_start_encryption_req(conn);
                        conn->core.step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
                    } else {
                        conn->core.step = LMP_TX_NULL;
                        conn->core.stage = LMP_STAGE_CONNECTED;
                    }
                    lmp_role_machine_handler_register(conn, lmp_master_machine);
                } else {
                    if (conn->core.enc_state & 0x40) {
                        conn->core.enc_state &= ~0x40;
                        lmp_tx_resume_encryption_req(conn);
                        conn->core.step = LMP_WAIT_START_ENCRYPTION_REQ;
                    } else {
                        conn->core.step = LMP_TX_NULL;
                        conn->core.stage = LMP_STAGE_CONNECTED;
                    }
                    lmp_role_machine_handler_register(conn, lmp_slave_machine);
                }
                lmp_send_event_role_change(conn, 0);
            } else {
                if (conn->link->afh.en == 1)
                    bdhw_enable_afh(conn->link->frame);
                lmp_send_event_role_change(conn, 0x35);
                conn->err_code = 8;
                conn->core.state |= 8;
            }
        } else {
            if (ok) {
                lmp_send_event_role_change(conn, 0);
            } else {
                if (conn->link->afh.en == 1)
                    bdhw_enable_afh(conn->link->frame);
                lmp_send_event_role_change(conn, 0x35);
                conn->err_code = 8;
                conn->core.state |= 8;
            }
            if (conn->core.role == 0)
                lmp_role_machine_handler_register(conn, lmp_master_machine);
            else
                lmp_role_machine_handler_register(conn, lmp_slave_machine);
        }
    }
    lmp_role_switch_misc_free(conn);
}

static void LMP_CODE __role_switch_post(void *_conn)
{
    struct lmp_connection *conn = _conn;
    struct lmp_role_switch *rs;

    local_irq_disable();
    if (conn->rs_process->timer0 != 0xff) {
        slot_timer_put(conn->rs_process->timer0);
        conn->rs_process->timer0 = 0xff;
    }
    local_irq_enable();
    rs = conn->rs_process;
    rs->priv = conn;
    if (conn->core.role == 0)
        bredr_switch_role_to_slave(conn->link, rs->slot_offset);
    else if (!CONFIG_BTCTLER_TWS_ENABLE)
        bredr_switch_role_to_master(conn->link, rs->slot_offset);
}

static void LMP_CODE __role_switch_schdule(void *_conn)
{
    struct lmp_connection *conn = _conn;

    local_irq_disable();
    if (conn->rs_process->timer1 != 0xff) {
        slot_timer_put(conn->rs_process->timer1);
        conn->rs_process->timer1 = 0xff;
    }
    if (conn->link)
        link_conn_run_slots(conn->link, 128);
    if (conn->core.role == 0 && conn->link)
        link_conn_enter_role_switch_probe(conn->link);
    local_irq_enable();
}

void LMP_CODE __role_switch_probe(void *_conn)
{
    struct lmp_connection *conn = _conn;
    struct lmp_role_switch *rs = conn->rs_process;
    u32 clk, t, d, delay;

    if (rs->instant == 0) {
        LOG_E("conn->rs_process instant=0");
        lmp_role_switch_misc_free(conn);
        return;
    }
    rs->over_role_switch = 0;
    if (rs->timer1 == 0xff) {
        conn->rs_process->timer1 = slot_timer_get();
    } else {
        slot_timer_put(rs->timer1);
        conn->rs_process->timer1 = slot_timer_get();
        printf("why_have_timer1!");
    }
    LOG_D("conn->core.role 0x%x\n", conn->core.role);
    if (conn->core.role == 0) {
        clk = bredr_read_slot_clk();
        t = conn->rs_process->instant - 2;
        if (t > clk) {
            delay = t - clk;
        } else {
            d = clk - t;
            delay = d > 69120000 ? 134217728 - d : 2;
        }
    } else {
        clk = bredr_get_master_slot_clk(conn->link);
        t = conn->rs_process->instant;
        if (t > clk)
            delay = t - clk;
        else if (clk - t > 69120000)
            delay = 134217728 - clk + t;
        else
            delay = 2;
        delay += 1;
    }
    lc_qos_ctrl(conn->link, 0);
    conn->core.state |= 0x20;
    if (delay > 8) {
        slot_timer_set(conn->rs_process->timer0, __role_switch_post, conn, delay);
        slot_timer_set(conn->rs_process->timer1, __role_switch_schdule, conn, delay - 2);
    } else {
        __role_switch_schdule(conn);
        slot_timer_set(conn->rs_process->timer0, __role_switch_post, conn, delay);
    }
    LOG_D("rs_after: 0x%x, 0x%x, 0x%x", delay, clk, conn->rs_process->instant);
}

static void LMP_CODE lmp_no_accepted_switch_req(struct lmp_connection *conn)
{
    lmp_role_switch_misc_free(conn);
    lmp_response(conn, "111", PDU(NOT_ACCEPTED), SWITCH_REQ, 0x21);
    lmp_send_event_role_change(conn, 0x21);
}

void LMP_CODE lmp_rx_slot_offset(struct lmp_connection *conn, u8 *data)
{
    lmp_role_switch_misc_alloc(conn);
    if (conn->rs_process)
        conn->rs_process->slot_offset = data[1] << 8 | data[0];
}

void LMP_CODE lmp_rx_switch_req(struct lmp_connection *conn, u8 *data)
{
    u32 clk = bredr_get_master_slot_clk(conn->link);
    struct lmp_role_switch *rs;
    u32 instant;

    if (config_btctler_bredr_master && conn->core.role == 0) {
        lmp_no_accepted_switch_req(conn);
        return;
    }
    if (!(hci_param.features[0] & 0x20)) {
        lmp_no_accepted_switch_req(conn);
        return;
    }
    if (CONFIG_BTCTLER_TWS_ENABLE && tws_conn.role == 1) {
        lmp_no_accepted_switch_req(conn);
        return;
    }
    lmp_role_switch_misc_alloc(conn);
    conn->core.role_switch = 0;
    rs = conn->rs_process;
    rs->init_by_local = 0;
    conn->role_switch_state = 1;
    if (rs->timer0 == 0xff) {
        conn->rs_process->timer0 = slot_timer_get();
        rs = conn->rs_process;
    }
    instant = data[1] << 8 | data[0] | data[2] << 16 | data[3] << 24;
    rs->instant = instant;
    LOG_D("rs_rx_instant: 0x%x, 0x%x", clk, instant);
    if (conn->core.role == 0) {
        lmp_response(conn, "11", PDU(ACCEPTED), SWITCH_REQ);
    } else {
        lmp_response(conn, "12A", PDU(SLOT_OFFSET), lc_local_slot_offset(conn->link), hci_param.addr);
        lmp_response(conn, "11", PDU(ACCEPTED), SWITCH_REQ);
    }
    __role_switch_probe(conn);
    if (conn->core.stage != LMP_STAGE_CONN)
        conn->core.stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
}

void LMP_CODE lmp_slave_tx_role_switch_req(struct lmp_connection *conn)
{
    u32 offset = lc_local_slot_offset(conn->link);
    u32 clk;

    lmp_role_switch_misc_alloc(conn);
    conn->rs_process->init_by_local = 1;
    conn->role_switch_state = 1;
    lmp_request(conn, "12A", PDU(SLOT_OFFSET), offset, hci_param.addr);
    clk = bredr_get_master_slot_clk(conn->link);
    conn->rs_process->instant = clk + 350;
    LOG_D("rs_instant: 0x%x, 0x%x", clk, clk + 350);
    if (conn->rs_process->instant > 0x7ffffff)
        conn->rs_process->instant -= 0x8000000;
    if (conn->rs_process->instant & 1)
        conn->rs_process->instant++;
    lmp_request(conn, "14", PDU(SWITCH_REQ), conn->rs_process->instant);
    conn->rs_process->state = 1;
    lc_qos_ctrl(conn->link, 0);
    conn->core.step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
}

static void LMP_CODE role_switch_req_timeout(void *_conn)
{
    struct lmp_connection *conn = _conn;

    lmp_role_switch_completed(conn->link, 1);
    bredr_link_event(conn->link, 2, 4, 1);
}

void LMP_CODE lmp_master_tx_role_switch_req(struct lmp_connection *conn)
{
    u32 clk;

    lmp_role_switch_misc_alloc(conn);
    conn->rs_process->init_by_local = 1;
    conn->role_switch_state = 1;
    clk = bredr_read_slot_clk();
    conn->rs_process->instant = clk + 350;
    LOG_D("RS_instant: 0x%x, 0x%x", clk, clk + 350);
    if (conn->rs_process->instant > 0x7ffffff)
        conn->rs_process->instant -= 0x8000000;
    if (conn->rs_process->instant & 1)
        conn->rs_process->instant++;
    lmp_request(conn, "14", PDU(SWITCH_REQ), conn->rs_process->instant);
    conn->rs_process->state = 1;
    lc_qos_ctrl(conn->link, 0);
    conn->core.role_switch = 0;
    if (conn->rs_process->timer0 == 0xff)
        conn->rs_process->timer0 = slot_timer_get();
    slot_timer_set(conn->rs_process->timer0, role_switch_req_timeout, conn, 356);
}

void LMP_CODE lmp_tx_role_switch_req(struct lmp_connection *conn)
{
    if (!CONFIG_BTCTLER_TWS_ENABLE)
        link_conn_run_slots(conn->link, 200);
    if (conn->core.role == 0)
        lmp_master_tx_role_switch_req(conn);
    else if (!CONFIG_BTCTLER_TWS_ENABLE)
        lmp_slave_tx_role_switch_req(conn);
}

void LMP_CODE lmp_rx_accepted_switch_req(struct lmp_connection *conn)
{
    struct lmp_role_switch *rs = conn->rs_process;

    if (rs == NULL) {
        LOG_E("conn->rs_process NULL");
        return;
    }
    if (conn->role_switch_state == 0) {
        lmp_role_switch_misc_free(conn);
        puts("no tx request but tx accept");
        return;
    }
    if (rs->timer0 == 0xff)
        conn->rs_process->timer0 = slot_timer_get();
    slot_timer_set(conn->rs_process->timer0, __role_switch_probe, conn, 4);
}

void LMP_CODE lmp_rx_not_accepted_switch_req(struct lmp_connection *conn)
{
    if (conn->rs_process == NULL)
        return;
    if (conn->rs_process->timer0 != 0xff) {
        slot_timer_put(conn->rs_process->timer0);
        conn->rs_process->timer0 = 0xff;
    }
    if (conn->rs_process->timer1 != 0xff) {
        slot_timer_put(conn->rs_process->timer1);
        conn->rs_process->timer1 = 0xff;
    }
    conn->rs_process->over_role_switch = 1;
    conn->rs_process->state = 3;
    if (link_conn_num_more_than_one()) {
        conn->tran_id = 0;
        lmp_tx_detch(conn, 0x13);
        return;
    }
    if (!(conn->core.state & 2)) {
        conn->tran_id = 0;
        lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
        lmp_setup_complete(conn);
        conn->core.step = LMP_WAIT_AUTH;
    }
    lmp_send_event_role_change(conn, 0x21);
    conn->rs_process->state = 4;
}

u8 LMP_CODE lmp_rx_role_switch_standby(struct lmp_connection *conn)
{
    u8 lp = conn->core.link_policy;

    if (lp & 1)
        return 1;
    LOG_D("rs_disable lp:0x%x", lp);
    return 0;
}

int LMP_CODE lmp_master_stage_role_switch_on_enc(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                                u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 *step = &conn->core.step;
    struct lmp_role_switch *rs;

    switch (*step) {
    case LMP_TX_PAUSE_ENCRYPTION_REQ:
        LOG_D("LMP_TX_PAUSE_ENCRYPTION_REQ");
        conn->core.enc_state |= 0x40;
        lmp_tx_pause_encryption_req(conn);
        *step = LMP_WAIT_PAUSE_ENCRYPTION_REQ;
        break;
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
            SDK_ASSERT(0, 630, "0 ");
        /* fall through */
    case LMP_TX_ROLE_SWITCH:
        LOG_D("LMP_TX_ROLE_SWITCH");
        lmp_tx_role_switch_req(conn);
        *step = LMP_WAIT_SLOT_OFFSET;
        break;
    case LMP_WAIT_SLOT_OFFSET:
        if (opcode0 == PDU(SLOT_OFFSET))
            *step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ))
            break;
        lmp_send_event_encryption_change(conn);
        *step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        rs = conn->rs_process;
        if (rs && rs->over_role_switch) {
            rs->over_role_switch = 0;
            if (rs->state == 2) {
                lmp_send_event_role_change(conn, 0);
                if (CONFIG_BTCTLER_TWS_ENABLE && conn->core.role == 1)
                    tws_phone_link_connection(conn);
            } else {
                lmp_send_event_role_change(conn, 0x35);
            }
            lmp_role_switch_misc_free(conn);
        }
        break;
    }
    return 0;
}
