/* Classic/lmp_sniff.c (included by lmp.c): sniff negotiation, anchor
 * points for up to two sniffing links, unsniff. */
#undef LMP_FILE
#define LMP_FILE "lmp_sniff.c"

void free(void *p);
void *malloc(unsigned long size);
static void lmp_sniff_anchor_point(void *priv);
void lmp_sniff_pre_anchor_point(void *priv);
void lmp_sniff_wakeup(void *priv);

__attribute__((weak)) void LMP_CODE sniff_hook(u32 slot, u8 num, u8 first_conn, int t_sniff)
{
}

__attribute__((weak)) void LMP_CODE sniff_negotiation_hook(u16 *T_sniff, u16 *attemp, u16 *timeout, u8 negotiation)
{
}

void LMP_CODE lmp_sniff_t_slot_attemp_reset(u16 t_slot, u16 attemp)
{
    sniff_t_slot = t_slot;
    sniff_attemp = attemp;
}

void LMP_CODE tws_ble_sniff_slot_reset(u8 type)
{
    sniff_t_slot = 800;
}

static u16 LMP_CODE lmp_sniff_cal_offset(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u32 clk, fine;
    int off;

    SDK_ASSERT(sniff, 111, "sniff ");
    bredr_get_link_slot_clk(conn->link, &clk, &fine);
    if (sniff->timing_control_flags & 2)
        clk ^= 0x4000000;
    off = sniff->D_sniff - (int)(clk % sniff->T_sniff);
    if (off < 0)
        off += sniff->T_sniff;
    return off;
}

u16 LMP_CODE lmp_sniff_cal_other_D_sniff(struct lmp_connection *_main_conn, struct lmp_connection *_other_conn)
{
    struct lmp_sniff *_main_sniff, *_other_sniff;
    u16 o1, o2, ret = 0;
    int d, r;

    LOG_D("%s: ms:0x%x, os:0x%x", "lmp_sniff_cal_other_D_sniff", _main_conn, _other_conn);
    _main_sniff = _main_conn->sniff;
    _other_sniff = _other_conn->sniff;
    SDK_ASSERT(_main_sniff, 138, "_main_sniff ");
    SDK_ASSERT(_other_sniff, 139, "_other_sniff ");
    if (_main_sniff->T_sniff == _other_sniff->T_sniff) {
        o1 = lmp_sniff_cal_offset(_main_conn);
        o2 = lmp_sniff_cal_offset(_other_conn);
        LOG_D("offset:%d, %d", o1, o2);
        d = o1 - o2;
        d = d > -1 ? d : -d;
        LOG_D("delta_offset:%d\n", d);
        if (d < (_main_sniff->T_sniff >> 1)) {
            if (o1 > o2) {
                LOG_D(">>>111---");
                r = (_main_sniff->T_sniff >> 1) + d;
            } else {
                LOG_D(">>>222---");
                r = (_main_sniff->T_sniff >> 1) - d;
            }
        } else {
            if (o1 > o2) {
                LOG_D(">>>333---");
                r = _main_sniff->T_sniff + d - (_main_sniff->T_sniff >> 1);
            } else {
                LOG_D(">>>444---");
                r = _main_sniff->T_sniff - d + (_main_sniff->T_sniff >> 1);
            }
        }
        ret = (u16)r == _main_sniff->T_sniff ? 0 : (u16)r;
    }
    LOG_D("ret :%d", ret);
    return ret;
}

u8 LMP_CODE lmp_sniff_is_the_main_sniff(struct lmp_connection *conn)
{
    return main_conn && main_conn == conn;
}

struct lmp_sniff *LMP_CODE lmp_sniff_misc_alloc(struct lmp_connection *conn)
{
    if (conn->sniff)
        return conn->sniff;
    conn->sniff = malloc(40);
    LOG_I("s_alloc");
    SDK_ASSERT(conn->sniff, 213, "conn->sniff ");
    memset(conn->sniff, 0, 40);
    conn->sniff->timer = slot_timer_get();
    conn->link->unsniff_ack = 0;
    if (main_conn == NULL)
        main_conn = conn;
    else
        other_conn = conn;
    return conn->sniff;
}

void LMP_CODE lmp_sniff_misc_free(struct lmp_connection *conn, u8 run)
{
    struct lmp_sniff *sniff = conn->sniff;
    struct link_connection *tws;

    if (sniff == NULL)
        return;
    link_task_idle_disable();
    local_irq_disable();
    if (sniff->state & 1) {
        sniff->state &= ~1;
        sniff_num--;
    }
    lc_sniff_ctrl(conn->link, 6, NULL);
    if (sniff->timer != 0xff)
        slot_timer_put(sniff->timer);
    free(sniff);
    conn->sniff = NULL;
    if (main_conn == conn) {
        main_conn = other_conn;
        other_conn = NULL;
    } else if (other_conn == conn) {
        other_conn = NULL;
    }
    link_task_set_period(0);
    tws = tws_link.link;
    if (CONFIG_BTCTLER_TWS_ENABLE && tws && !tws_sniff.enable)
        link_conn_run_enable(tws, 1);
    link_conn_run_enable(conn->link, 1);
    if (run)
        link_conn_run_slots(conn->link, 64);
    local_irq_enable();
    LOG_I("s_free");
}

void LMP_CODE lmp_sniff_detach(struct lmp_connection *conn)
{
    if (conn->core.operation_mode == 2) {
        conn->core.operation_mode = 0;
        lmp_send_event_mode_change(conn, 0);
    }
    lmp_sniff_misc_free(conn, 1);
}

void LMP_CODE lmp_set_sniff_establish_by_remote(u8 en)
{
    LOG_I("%s:%d", "lmp_set_sniff_establish_by_remote", en);
    sniff_establish_by_remote = en;
}

void LMP_CODE lmp_set_sniff_disable(void)
{
    LOG_I("%s", "lmp_set_sniff_disable");
    hci_param.default_link_policy &= ~4;
}

u8 LMP_CODE lmp_rx_sniff_standby(struct lmp_connection *conn)
{
    u8 lp = conn->core.link_policy;

    if (lp & 4)
        return 1;
    LOG_I("sniff_disable lp:0x%x", lp);
    return 0;
}

void LMP_CODE lmp_rx_accepted_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u16 st;

    if (sniff == NULL) {
        LOG_I("sniff_free_already:0x%x", conn);
        return;
    }
    st = sniff->state;
    sniff->state = st | 8;
    if (st & 1)
        lmp_sniff_misc_free(conn, 1);
    else
        lmp_sniff_misc_free(conn, 0);
    conn->core.operation_mode = 0;
    lmp_send_event_mode_change(conn, 0);
}

void LMP_CODE lmp_rx_not_accepted_unsniff_req(struct lmp_connection *conn)
{
    LOG_I("%s:0x%x", "lmp_rx_not_accepted_unsniff_req", conn);
    conn->core.operation_mode = 0;
}

void LMP_CODE lmp_rx_not_accepted_sniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff;
    u8 local;

    LOG_I("%s:0x%x", "lmp_rx_not_accepted_sniff_req", conn);
    conn->core.operation_mode = 0;
    sniff = conn->sniff;
    if (sniff) {
        local = conn->tran_id == conn->core.role;
        if (sniff->state & 4) {
            if (local)
                lmp_sniff_misc_free(conn, 0);
        } else if (!local) {
            lmp_sniff_misc_free(conn, 0);
        }
    }
    lmp_send_event_mode_change(conn, 0);
}

void LMP_CODE lmp_rx_accepted_sniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;

    LOG_I("%s:0x%x", "lmp_rx_accepted_sniff_req", conn);
    local_irq_disable();
    if (conn->sniff == NULL) {
        local_irq_enable();
        LOG_I("sniff_free_already:0x%x", conn);
        return;
    }
    sniff->negotiation_cnt = 0;
    lmp_sniff_anchor_point_preset(conn);
    conn->core.operation_mode = 2;
    lmp_send_event_mode_change(conn, sniff->T_sniff);
    local_irq_enable();
}

void LMP_CODE lmp_rx_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;

    if (sniff == NULL) {
        LOG_D("sniff_free_already:0x%x", conn);
        return;
    }
    if (conn->core.role == 0) {
        sniff->state |= 8;
        lmp_response(conn, "S11", PDU(ACCEPTED), UNSNIFF_REQ);
        return;
    }
    lmp_response(conn, "11", PDU(ACCEPTED), UNSNIFF_REQ);
    conn->core.operation_mode = 0;
    if (!(sniff->state & 1)) {
        lmp_sniff_misc_free(conn, 0);
        lmp_send_event_mode_change(conn, 0);
    }
}

__attribute__((weak)) u8 LMP_CODE lmp_rx_sniff_req_check(void)
{
    return 0;
}

/* lmp_sniff.c ~287: a sniff request crossing our own. */
INLINE int lmp_sniff_transaction_collision(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u16 tran;

    if (sniff == NULL)
        return 0;
    tran = sniff->state & 4;
    if (tran ? conn->tran_id == conn->core.role : conn->tran_id != conn->core.role)
        return 0;
    LOG_E("sniff transaction collision : 0x%x, 0x%x, 0x%x", tran, conn->tran_id, conn->core.role);
    return 1;
}

void LMP_CODE lmp_rx_sniff_req(struct lmp_connection *conn, u8 *data)
{
    struct lmp_sniff *sniff;
    u8 flags, cnt, neg = 0;
    u16 D, T, A, TO, nD, nT, nA, d;
    u8 esco;

    if (lmp_rx_sniff_req_check())
        return;
    if (!(hci_param.features[0] & 0x80)) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x11);
        return;
    }
    if (lmp_sniff_transaction_collision(conn)) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x0c);
        return;
    }
    if (conn->core.operation_mode == 2) {
        LOG_D("operation_mode is sniff");
        return;
    }
    sniff = lmp_sniff_misc_alloc(conn);
    flags = data[0];
    D = data[2] << 8 | data[1];
    T = data[4] << 8 | data[3];
    A = data[6] << 8 | data[5];
    TO = data[8] << 8 | data[7];
    sniff->timing_control_flags = flags;
    sniff->D_sniff = D;
    sniff->T_sniff = T;
    sniff->attempt = A;
    sniff->timeout = TO;
    LOG_I("rx conn:0x%x, timing_control_flags:0x%x, D:0x%x, T:0x%x, A:0x%x, TO:0x%x", conn, flags, D, T, A, TO);
    nD = D;
    nT = T;
    nA = A;
    if (!sniff_establish_by_remote && !config_btctler_hci_standard) {
        nT = sniff_t_slot;
        esco = D > 6 && lmp_get_esco_conn_statu();
        nD = esco ? 6 : D;
        neg = T != nT || esco;
        nA = sniff_attemp;
        if (A != nA)
            neg |= 2;
    }
    if (!lmp_sniff_is_the_main_sniff(conn) && main_conn->sniff->T_sniff == nT &&
        (d = lmp_sniff_cal_other_D_sniff(main_conn, conn)) != 0) {
        nD = (d + nD) % nT;
        neg |= 4;
        LOG_D("new_D : 0x%x", sniff->D_sniff);      /* sic: the value before the update */
        sniff->timing_control_flags = flags;
        sniff->D_sniff = nD;
        sniff->T_sniff = nT;
        sniff->attempt = nA;
        sniff->timeout = TO;
    } else {
        sniff->timing_control_flags = flags;
        sniff->D_sniff = nD;
        sniff->T_sniff = nT;
        sniff->attempt = nA;
        sniff->timeout = TO;
        if (neg == 0) {
            LOG_I("sniff_accept");
            sniff->negotiation_cnt = 0;
            lmp_response(conn, "11", PDU(ACCEPTED), SNIFF_REQ);
            conn->link->sniff_pre_point_reset_flag = 1;
            lmp_sniff_anchor_point_preset(conn);
            conn->core.operation_mode = 2;
            lmp_send_event_mode_change(conn, sniff->T_sniff);
            return;
        }
    }
    cnt = ++sniff->negotiation_cnt;
    LOG_I("negotiation : 0x%x, %d", neg, cnt);
    LOG_I("tx conn:0x%x, timing_control_flags:0x%x, D:0x%x, T:0x%x, A:0x%x, TO:0x%x", conn, flags, nD, nT, nA, TO);
    cnt = sniff->negotiation_cnt;
    if (cnt > 3) {
        if (T < 64) {
            lmp_sniff_misc_free(conn, 1);
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x20);
            return;
        }
        LOG_I("negotiation over, sniff accept");
        sniff->negotiation_cnt = 0;
        sniff->timing_control_flags = flags;
        sniff->D_sniff = D;
        sniff->T_sniff = T;
        sniff->attempt = A;
        sniff->timeout = TO;
        lmp_response(conn, "11", PDU(ACCEPTED), SNIFF_REQ);
        conn->link->sniff_pre_point_reset_flag = 1;
        lmp_sniff_anchor_point_preset(conn);
        conn->core.operation_mode = 2;
        lmp_send_event_mode_change(conn, sniff->T_sniff);
        return;
    }
    sniff_negotiation_hook(&sniff->T_sniff, &sniff->attempt, &sniff->timeout, cnt);
    LOG_I("form hook T:0x%x, A:0x%x, TO:0x%x", sniff->T_sniff, sniff->attempt, sniff->timeout);
    lmp_response(conn, "112222", PDU(SNIFF_REQ), sniff->timing_control_flags, sniff->D_sniff, sniff->T_sniff,
                 sniff->attempt, sniff->timeout);
}

static void LMP_CODE lmp_sniff_anchor_point_first(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *sniff = conn->sniff;

    if (CONFIG_BTCTLER_TWS_ENABLE && tws_link.link && tws_conn.role == 0)
        tws_sniff_with_phone();
    if (lmp_sniff_is_the_main_sniff(conn)) {
        link_task_schedule_reset();
        link_task_set_period(1);
    }
    lc_sniff_ctrl(conn->link, 5, NULL);
    local_irq_disable();
    sniff->state |= 1;
    sniff_num++;
    local_irq_enable();
    sniff->wake_instant = (sniff->T_sniff + bredr_read_slot_clk()) & 0x7ffffff;
    LOG_I("fap : mk:0x%x", bredr_get_master_slot_clk(conn->link));
    link_idle_task_enable_detect();
    if (CONFIG_BTCTLER_TWS_ENABLE && tws_link.link && tws_conn.role == 1) {
        link_conn_run_enable(conn->link, 0);
        return;
    }
    lc_sniff_ctrl(conn->link, 1, NULL);
    link_conn_run_enable(conn->link, 0);
    link_conn_run(conn->link);
    link_conn_set_first_anchor_point_timer(conn->link, sniff->timer, conn, sniff->attempt << 1);
}

void LMP_CODE lmp_sniff_anchor_point_preset(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u32 clk, base;
    int T, off;

    LOG_I("%s\n", "lmp_sniff_anchor_point_preset");
    if (lmp_get_esco_conn_statu()) {
        local_irq_disable();
        if (sniff->timer != 0xff) {
            slot_timer_put(sniff->timer);
            sniff->timer = 0xff;
        }
        local_irq_enable();
        return;
    }
    bredr_get_link_slot_clk(conn->link, &clk, &base);
    if (sniff->timing_control_flags & 2)
        clk ^= 0x4000000;
    T = sniff->T_sniff;
    off = sniff->D_sniff - (int)(clk % T) +
          ((CONFIG_BTCTLER_TWS_ENABLE && tws_link.link && tws_conn.role == 0) ? T : 0);
    while (off < (sniff->T_sniff >> 1))
        off += T;
    off -= 2;
    sniff->wakeup_offset = 0;
    sniff->wake_instant = (base + off) & 0x7ffffff;
    slot_timer_set_ext(sniff->timer, lmp_sniff_anchor_point_first, conn, off, base);
    LOG_I("fap_set:0x%x, s_num:%d\t", off, sniff_num);
}

void LMP_CODE lmp_hci_exit_sniff_mode(struct lmp_connection *conn)
{
    if (conn->core.operation_mode == 0) {
        lmp_send_event_mode_change(conn, 0);
        LOG_D("active_mode already:0x%x", conn);
        return;
    }
    if (CONFIG_BTCTLER_TWS_ENABLE && !tws_conn_switch_is_master()) {
        lmp_send_event_mode_change(conn, 0);
        LOG_D("tws_role_switching:0x%x", conn);
        return;
    }
    if (conn->sniff == NULL) {
        conn->core.operation_mode = 0;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    LOG_D("lmp_hci_exit_sniff_mode:0x%x", conn);
    lmp_tx_unsniff_req(conn);
}

void LMP_CODE lmp_hci_sniff_mode(struct lmp_connection *conn, u16 sniff_max_interval, u16 sniff_min_interval,
                                 u16 sniff_attemp, u16 sniff_timeout)
{
    struct lmp_sniff *sniff;
    u16 d, D;

    LOG_D("lmp_hci_sniff_mode:0x%x", conn);
    if (conn == NULL || conn->link == NULL || !(hci_param.features[0] & 0x80))
        return;
    if (tws_link.link && tws_conn.monitor_start != 2) {
        hci_send_event(20, "1212", 12, conn->core_s.handle, conn->core.operation_mode, 0);
        return;
    }
    if (!lmp_rx_sniff_standby(conn)) {
        hci_send_event(20, "1212", 12, conn->core_s.handle, conn->core.operation_mode, 0);
        return;
    }
    if (conn->core.operation_mode == 2 || (conn->core.state & 8)) {
        lmp_send_event_mode_change(conn, 0);
        LOG_D("sniff_mode already:0x%x", conn);
        return;
    }
    if (CONFIG_BTCTLER_TWS_ENABLE && !tws_conn_switch_is_master()) {
        lmp_send_event_mode_change(conn, 0);
        LOG_D("tws_role_switching:0x%x", conn);
        return;
    }
    if (conn->sniff) {
        LOG_I("sniff negotiating , tran_id : %x", conn->sniff->state & 4);
        return;
    }
    sniff = lmp_sniff_misc_alloc(conn);
    sniff->negotiation_cnt = 0;
    sniff->timing_control_flags = (bredr_get_master_slot_clk(conn->link) & 0x4000000) ? 2 : 0;
    sniff->T_sniff = sniff_t_slot < sniff_max_interval ? sniff_t_slot : sniff_max_interval;
    sniff->D_sniff = 0;
    sniff->attempt = sniff_attemp;
    sniff->timeout = sniff_timeout;
    if (!lmp_sniff_is_the_main_sniff(conn) && main_conn->sniff->T_sniff == conn->sniff->T_sniff &&
        (d = lmp_sniff_cal_other_D_sniff(main_conn, conn)) != 0) {
        D = (sniff->D_sniff + d) % sniff->T_sniff;
        D += D & 1;
        sniff->D_sniff = D;
        LOG_D("new_D : 0x%x", D);
    }
    LOG_I("hci : conn:0x%x, timing_control_flags:0x%x, D:0x%x, T:0x%x, A:0x%x, TO:0x%x", conn,
          sniff->timing_control_flags, sniff->D_sniff, sniff->T_sniff, sniff->attempt, sniff->timeout);
    sniff->state |= 4;
    lmp_request(conn, "112222", PDU(SNIFF_REQ), sniff->timing_control_flags, sniff->D_sniff, sniff->T_sniff,
                sniff->attempt, sniff->timeout);
}

void LMP_CODE lmp_tx_sniff_req(struct lmp_connection *conn)
{
    LOG_D("lmp_tx_sniff_mode: 0x%x", conn);
    lmp_hci_sniff_mode(conn, 800, 0, 4, 1);
}

void LMP_CODE lmp_tx_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u16 st;

    if (conn->link->tws_link)
        return;
    if (conn->core.operation_mode == 0)
        return;
    if (sniff == NULL || (CONFIG_BTCTLER_TWS_ENABLE && tws_link.link && tws_conn.role == 1))
        return;
    st = sniff->state;
    if (st & 8) {
        LOG_I("LMP_STA_RX_UNSNIFF:0x%x", conn);
        return;
    }
    if (st & 0x10) {
        LOG_I("LMP_STA_TX_UNSNIFF:0x%x", conn);
        return;
    }
    sniff->state = st | 0x80;
    if (!(st & 1))
        lmp_request(conn, "1", PDU(UNSNIFF_REQ));
}

void LMP_CODE lmp_sniff_pre_anchor_point_reset(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    struct link_connection *link = conn->link;
    u32 clk, wi, T, lim;

    if (!link->sniff_pre_point_reset_flag)
        return;
    if (conn->core.operation_mode != 2)
        return;
    if (sniff->T_sniff > 100)
        return;
    if (link->sniff_state != 1)
        return;
    link->sniff_long_interval_filter = 2;
    lc_sniff_ctrl(link, 2, NULL);
    clk = bredr_read_slot_clk();
    if (clk > sniff->wake_instant)
        return;
    conn->link->sniff_state = 4;
    local_irq_disable();
    wi = sniff->wake_instant;
    T = sniff->T_sniff;
    lim = T + clk;
    if (wi > lim) {
        do
            wi -= T;
        while (wi > lim);
        sniff->wake_instant = wi;
    }
    if (wi - clk < 3) {
        wi += T;
        sniff->wake_instant = wi;
    }
    slot_timer_reset(sniff->timer, wi - clk);
    link_task_set_period(clk);
    idle_reset(0);
    conn->link->sniff_pre_point_reset_flag = 0;
    local_irq_enable();
}

static void LMP_CODE lmp_sniff_anchor_point_set(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    struct link_connection *link;
    u32 clk, mclk, parm[2];
    int empty, rst, interval = 0, T, off, x;
    u8 f;
    s16 bitoff;

    empty = link_conn_tx_empty(conn->link);
    rst = sniff_support_reset_anchor_point;
    if (rst && sniff->T_sniff < 100) {
        T = sniff->T_sniff;
        link = conn->link;
        if (!empty || !sniff_anchor_point_start_tx_empty) {
            link->sniff_pre_point_reset_flag = 0;
            conn->link->sniff_long_interval_filter = 2;
            interval = sniff->T_sniff;
        } else if (link->sniff_Communicate) {
            f = link->sniff_long_interval_filter;
            if (f) {
                link->sniff_long_interval_filter = f - 1;
            } else {
                link->sniff_pre_point_reset_flag = 1;
                interval = sniff_long_interval;
            }
        } else {
            link->sniff_pre_point_reset_flag = 0;
            link = conn->link;
            f = link->sniff_long_interval_filter;
            interval = T;
            if (f) {
                link->sniff_long_interval_filter = --f;
                if (f == 0) {
                    link->sniff_pre_point_reset_flag = 1;
                    interval = sniff_long_interval;
                }
            }
        }
    }
    lc_sniff_ctrl(conn->link, 2, NULL);
    parm[0] = interval;
    parm[1] = sniff->T_sniff;
    lc_sniff_ctrl(conn->link, 3, parm);
    bitoff = parm[1];
    mclk = get_mclkn(parm[0], bitoff, &clk);
    T = sniff->T_sniff;
    off = sniff->D_sniff - (int)(((sniff->timing_control_flags & 2) ? mclk ^ 0x4000000 : mclk) % T);
    off += off < 0 ? T : 0;
    if (sniff->T_sniff < 100 && interval == sniff_long_interval && rst) {
        x = ((interval / T) - 1) * T + off;
        x += x < 1 ? T : 0;
    } else {
        x = off + (off < 1 ? T : 0);
        if (sniff->T_sniff > 100) {
            while (x < (sniff->T_sniff >> 1))
                x += T;
        }
    }
    sniff->wakeup_offset = 0;
    sniff->wake_instant = (clk + x) & 0x7ffffff;
    if (conn->core.role == 0) {
        slot_timer_set_ext(sniff->timer, lmp_sniff_wakeup, conn, x, clk);
    } else {
        sniff->wakeup_bitoff = bitoff;
        slot_timer_set_finecnt(sniff->timer, lmp_sniff_pre_anchor_point, conn, clk + x, bitoff);
    }
    conn->link->sniff_state = 1;
}

static void LMP_CODE lmp_sniff_anchor_timeout(void *priv)
{
    static u8 last_dt_1 LMP_DATA, last_dt_2 LMP_DATA;
    struct lmp_connection *conn = priv;
    struct lmp_sniff *sniff = conn->sniff, *ms, *os;
    u32 wi, tw;
    int dt;

    if (conn->link->unsniff_ack) {
        SDK_ASSERT(sniff->state & 8, 1215, "sniff->state & LMP_STA_RX_UNSNIFF ");
        LOG_D("unsniff_ack_2\n");
        lmp_sniff_misc_free(conn, 1);
        conn->core.operation_mode = 0;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    if (conn->core.operation_mode == 0) {
        lmp_sniff_misc_free(conn, 1);
        conn->core.operation_mode = 0;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    switch (sniff_num) {
    case 1:
        lmp_sniff_anchor_point_set(conn);
        if (sniff->state & 0x18)
            break;
        wi = sniff->wake_instant;
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            if (tws_sniff.enable) {
                tw = tws_sniff.wake_instant;
                if (tw)
                    link_task_set_period((wi > tw ? tw : wi) - 4);
            } else {
                link_task_set_period(wi - 4);
            }
        } else {
            link_task_set_period(wi - 4);
        }
        break;
    case 2:
        SDK_ASSERT(main_conn, 1258, "main_conn ");
        SDK_ASSERT(other_conn, 1259, "other_conn ");
        if ((main_conn->sniff->state & 0x18) || (other_conn->sniff->state & 0x18)) {
            lmp_sniff_anchor_point_set(conn);
            break;
        }
        lmp_sniff_anchor_point_set(conn);
        link_task_set_period((main_conn->sniff->wake_instant > other_conn->sniff->wake_instant ? other_conn : main_conn)
                             ->sniff->wake_instant - 4);
        ms = main_conn->sniff;
        os = other_conn->sniff;
        if (ms->T_sniff != os->T_sniff)
            break;
        dt = ms->wake_instant - os->wake_instant;
        dt = dt > -1 ? dt : -dt;
        if (lmp_sniff_is_the_main_sniff(conn)) {
            if (last_dt_1 != dt)
                LOG_D("dt main:%d", dt);
            last_dt_1 = dt;
        } else {
            if (last_dt_2 != dt)
                LOG_D("dt other:%d", dt);
            last_dt_2 = dt;
        }
        if (dt < 6) {
            LOG_D("m_ins:0x%x, o_ins:0x%x, delta_offset:%d", main_conn->sniff->wake_instant,
                  other_conn->sniff->wake_instant, dt);
            lmp_hci_exit_sniff_mode(other_conn);
        }
        break;
    }
    link_conn_run(NULL);
}

static void LMP_CODE lmp_sniff_anchor_point(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *sniff = conn->sniff;
    struct link_connection *link;
    u32 clk, fine;
    u16 st;

    link_task_try_run_slots(&conn->link->task, (sniff->attempt << 1) + 4 + (((s8)sniff->state >> 7) & 4));
    link = conn->link;
    link->sniff_state = 3;
    link_conn_set_first_anchor_point_timer(conn->link, sniff->timer, conn,
                                           (sniff->attempt << 1) + (link_conn_tx_empty(link) ? 1 : 3));
    bredr_get_link_slot_clk(conn->link, &clk, &fine);
    if (sniff->T_sniff > 400)
        LOG_D("(%x,%x)", clk << 1, fine);
    if (clk > 134212928) {
        LOG_I("tx-unsniff1:%d", 1342);
        lmp_tx_unsniff_req(conn);
    }
    if (fine > 134212928) {
        LOG_I("tx-unsniff2:%d", 1347);
        lmp_tx_unsniff_req(conn);
    }
    st = sniff->state;
    if ((s8)st < 0) {
        LOG_I("send-unsniff:%d,%s", 1352, "16:40:18");
        sniff->state = (sniff->state & ~0x90) | 0x10;
        lmp_request(conn, "1", PDU(UNSNIFF_REQ));
    }
    sniff_hook(clk, link_conn_num_more_than_one(), conn->core_s.handle == 0x30, sniff->T_sniff * 625 / 1000);
}

void LMP_CODE lmp_sniff_pre_anchor_point(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *sniff = conn->sniff;
    struct link_connection *link;

    sniff_anchor_point_start_tx_empty = link_conn_tx_empty(conn->link) != 0;
    link = conn->link;
    link->sniff_state = 2;
    link->sniff_Communicate = 0;
    lc_sniff_ctrl(conn->link, 1, NULL);
    if (lmp_sniff_is_the_main_sniff(conn)) {
        link_task_schedule_reset();
        link_task_set_period(1);
    }
    link_conn_run(conn->link);
    if (sniff->wakeup_offset > 1)
        slot_timer_set(sniff->timer, lmp_sniff_anchor_point, conn, sniff->wakeup_offset);
    else
        lmp_sniff_anchor_point(conn);
}

void LMP_CODE lmp_sniff_wakeup(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *sniff = conn->sniff;

    lc_sniff_ctrl(conn->link, 2, NULL);
    slot_timer_set(sniff->timer, lmp_sniff_pre_anchor_point, conn, 2);
}

/* lmp_sniff.c ~1417 */
INLINE int lmp_sniff_offset(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;

    if (sniff == NULL)
        return 0;
    return sniff->attempt << 1;
}

s32 LMP_CODE lmp_sniff_and_afh_offset_ali(void *lc, u32 instant)
{
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    int off;

    if (lmp_get_esco_conn_statu())
        return 0;
    if (conn == NULL || conn->core.operation_mode != 2)
        return 0;
    off = lmp_sniff_offset(conn);
    LOG_I("offset:%d\t", off);
    return off;
}

void LMP_CODE lmp_sniff_subrating_cnt(struct lmp_connection *conn)
{
    struct lmp_sniff *sniff = conn->sniff;
    u32 clk, fine;

    if (sniff == NULL || !(sniff->state & 0x200))
        return;
    bredr_get_link_slot_clk(conn->link, &clk, &fine);
    sniff->local_timeout = sniff->minimum_local_timeout + fine;
}

u8 LMP_CODE get_1to2_conn_exit_sniff(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->link && conn->link->sniff_sta)
            return 1;
    }
    return 0;
}

int LMP_CODE lmp_in_sniff_mode(struct lmp_connection *conn)
{
    int ret = 0;

    local_irq_disable();
    if (conn && conn->link)
        ret = conn->core.operation_mode == 2;
    local_irq_enable();
    return ret;
}
