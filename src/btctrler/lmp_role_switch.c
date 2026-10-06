/* Owned lmp_role_switch.c: LMP role switch, either side starting it.
 * The switch instant is announced in LMP_switch_req; slot timers run
 * __role_switch_probe (4 slots after the exchange), which schedules the
 * baseband switch (__role_switch_post) at the instant and frees the link's
 * slots shortly before it (__role_switch_schdule).  The link layer reports
 * the outcome to lmp_role_switch_completed, then lmp_role_switch_machine. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_BREDR_MASTER && !BTCFG_TWS,
               "written for a standard HCI host, no forced central, no TWS");
/* The SDK's timer callbacks used these offsets into the connection. */
#ifndef UNITTEST
_Static_assert(offsetof(struct lmp_connection, link) == 220 &&
               offsetof(struct lmp_connection, rs_process) == 236 &&
               offsetof(struct lmp_connection, core.role) == 138 &&
               offsetof(struct lmp_connection, core.state) == 176, "connection layout");
#endif

#define NO_TIMER 0xff
#define CLOCK_WRAP 0x8000000u       /* the 27-bit slot clock */
#define STATE_ROLE_SWITCHING (1u << 5)
#define STATE_DETACHING      (1u << 3)
#define STATE_SWITCHED       (1u << 13)
#define STATE_HOST_ACCEPTED  (1u << 14)

void lmp_role_switch_misc_alloc(struct lmp_connection *conn)
{
    if (conn->rs_process) return;
    conn->rs_process = zalloc(sizeof(*conn->rs_process));
    if (!conn->rs_process) btctrler_assert_failed("conn->rs_process", 0);
    memset(conn->rs_process, 0, sizeof(*conn->rs_process));
    conn->rs_process->timer0 = NO_TIMER;
    conn->rs_process->timer1 = NO_TIMER;
}

void lmp_role_switch_misc_free(struct lmp_connection *conn)
{
    local_irq_disable();
    struct lmp_role_switch *rs = conn->rs_process;
    if (rs) {
        if (rs->timer0 != NO_TIMER) { slot_timer_put(rs->timer0); rs->timer0 = NO_TIMER; }
        if (rs->timer1 != NO_TIMER) { slot_timer_put(rs->timer1); rs->timer1 = NO_TIMER; }
        free(rs);
        conn->rs_process = 0;
    }
    local_irq_enable();
}

void lmp_role_switch_detach(struct lmp_connection *conn)
{
    lmp_role_switch_misc_free(conn);
}

/* Role switch allowed by the link policy? */
bool lmp_rx_role_switch_standby(struct lmp_connection *conn)
{
    return conn->core.link_policy & 1;
}

/* An instant 350 slots ahead, even and within the clock's range. */
static u32 instant_after(u32 clk)
{
    u32 instant = clk + 350;
    if (instant >= CLOCK_WRAP) instant -= CLOCK_WRAP;
    return instant + (instant & 1);
}

static void role_switch_req_timeout(void *priv)
{
    struct lmp_connection *conn = priv;
    lmp_role_switch_completed(conn->link, 1);
    bredr_link_event(conn->link, 2, 4, 1);
}

void lmp_master_tx_role_switch_req(struct lmp_connection *conn)
{
    lmp_role_switch_misc_alloc(conn);
    struct lmp_role_switch *rs = conn->rs_process;
    rs->init_by_local = 1;
    conn->role_switch_state = 1;
    rs->instant = instant_after(bredr_read_slot_clk());
    lmp_request(conn, "14", PDU(SWITCH_REQ), rs->instant);
    rs->state = ROLE_SWITCH_WAIT_COMPLETED;
    lc_qos_ctrl(conn->link, 0);
    conn->core.role_switch = 0;
    if (rs->timer0 == NO_TIMER) rs->timer0 = slot_timer_get();
    slot_timer_set(rs->timer0, role_switch_req_timeout, conn, 356);
}

void lmp_slave_tx_role_switch_req(struct lmp_connection *conn)
{
    struct link_connection *link = conn->link;
    u32 offset = lc_local_slot_offset(link);
    lmp_role_switch_misc_alloc(conn);
    struct lmp_role_switch *rs = conn->rs_process;
    rs->init_by_local = 1;
    conn->role_switch_state = 1;
    lmp_request(conn, "12A", PDU(SLOT_OFFSET), offset, hci_param.addr);
    rs->instant = instant_after(bredr_get_master_slot_clk(link));
    lmp_request(conn, "14", PDU(SWITCH_REQ), rs->instant);
    rs->state = ROLE_SWITCH_WAIT_COMPLETED;
    lc_qos_ctrl(link, 0);
    conn->core.step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
}

void lmp_tx_role_switch_req(struct lmp_connection *conn)
{
    link_task_run_slots(&conn->link->task, 200);
    if (conn->core.role == 0) lmp_master_tx_role_switch_req(conn);
    else lmp_slave_tx_role_switch_req(conn);
}

static void __role_switch_schdule(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_role_switch *rs = conn->rs_process;
    local_irq_disable();
    if (rs->timer1 != NO_TIMER) { slot_timer_put(rs->timer1); rs->timer1 = NO_TIMER; }
    if (conn->link) link_task_run_slots(&conn->link->task, 128);
    local_irq_enable();
}

static void __role_switch_post(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_role_switch *rs = conn->rs_process;
    local_irq_disable();
    if (rs->timer0 != NO_TIMER) { slot_timer_put(rs->timer0); rs->timer0 = NO_TIMER; }
    local_irq_enable();
    rs->priv = conn;
    if (conn->core.role == 0) bredr_switch_role_to_slave(conn->link, rs->slot_offset);
    else bredr_switch_role_to_master(conn->link, rs->slot_offset);
}

/* The switch is agreed: time the baseband switch for the instant. */
void __role_switch_probe(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_role_switch *rs = conn->rs_process;
    u32 offset;
    if (!rs->instant) {
        lmp_role_switch_misc_free(conn);
        return;
    }
    rs->over_role_switch = 0;
    if (rs->timer1 != NO_TIMER) slot_timer_put(rs->timer1);
    rs->timer1 = slot_timer_get();
    if (conn->core.role == 0) {
        u32 now = bredr_read_slot_clk(), at = rs->instant - 2;
        if (at > now) offset = at - now;
        else offset = now - at > 69120000 ? CLOCK_WRAP - (now - at) : 2;
    } else {
        u32 now = bredr_get_master_slot_clk(conn->link), at = rs->instant;
        if (at > now) offset = at - now;
        else offset = now - at > 69120000 ? CLOCK_WRAP - now + at : 2;
        offset += 1;
    }
    lc_qos_ctrl(conn->link, 0);
    conn->core.state |= STATE_ROLE_SWITCHING;
    if (offset > 8) {
        slot_timer_set(rs->timer0, __role_switch_post, conn, (int)offset);
        slot_timer_set(rs->timer1, __role_switch_schdule, conn, (int)offset - 2);
    } else {
        __role_switch_schdule(conn);
        slot_timer_set(rs->timer0, __role_switch_post, conn, (int)offset);
    }
}

/* Called by the link layer with the baseband's result (0: switched). */
void lmp_role_switch_completed(struct link_connection *link, int result)
{
    struct lmp_connection *conn = lmp_conn_for_link(link);
    if (!conn) btctrler_assert_failed("role switch conn", 0);
    conn->core.state &= ~STATE_ROLE_SWITCHING;
    if (result == 0) {
        struct link_connection *l = conn->link;
        if (l->afh_timer != NO_TIMER) { slot_timer_put(l->afh_timer); l->afh_timer = NO_TIMER; }
        bdhw_disable_afh(l->frame);
        lmp_channel_classification_close(conn);
        if (conn->core.role == 0) {
            conn->tran_id = 1;
            conn->core.role = 1;
            lc_qos_ctrl(l, 0);
        } else {
            conn->tran_id = 0;
            conn->core.role = 0;
        }
        conn->core.state |= STATE_SWITCHED;
        conn->rs_process->state = ROLE_SWITCH_SUSS;
        conn->core.role_switch = 0;
    } else {
        conn->rs_process->state = ROLE_SWITCH_FAILD;
        conn->core.role_switch = 1;
    }
    conn->rs_process->over_role_switch = 1;
    conn->role_switch_state = 0;
}

static void enable_afh_again(struct lmp_connection *conn)
{
    if (conn->link->afh.en == 1) bdhw_enable_afh(conn->link->frame);
}

/* After lmp_role_switch_completed: carry on where the switch interrupted. */
void lmp_role_switch_machine(struct lmp_connection *conn, int result)
{
    struct lmp_role_switch *rs = conn->rs_process;
    (void)result;
    if (conn->core.stage < LMP_STAGE_ROLE_SWITCH_ON_ENC) {     /* during setup */
        if (rs->state == ROLE_SWITCH_SUSS) {
            lmp_send_event_role_change(conn, 0);
            if (conn->core.role == 0) {
                lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
                lmp_tx_supervision_timeout(conn);
                conn->core.step = LMP_WAIT_AUTH;
                if (link_conn_get_ptt(conn->link) != 1) lmp_tx_packet_type_table_req(conn);
            } else if (conn->core.state & STATE_HOST_ACCEPTED) {
                lmp_send_event_link_request(conn);
                conn->core.step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
            }
        } else {
            enable_afh_again(conn);
            if (conn->core.role == 0) {
                /* Retry: up to three times when we asked, always when the
                 * peer did. */
                if (!rs->init_by_local || ++rs->try_cnt < 3) {
                    lmp_master_tx_role_switch_req(conn);
                    return;
                }
                if (conn->core.state & STATE_HOST_ACCEPTED) {
                    lmp_send_event_link_request(conn);
                    conn->core.step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
                }
            } else {
                lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
                conn->core.step = LMP_WAIT_AUTH;
            }
            lmp_send_event_role_change(conn, 0x35);
        }
    } else if (conn->core.stage == LMP_STAGE_ROLE_SWITCH_ON_ENC) {
        if (rs->state == ROLE_SWITCH_SUSS) {
            bool paused = conn->core.enc_state & 0x40;
            conn->core.enc_state &= ~0x40;
            if (conn->core.role == 0) {
                if (paused) {
                    lmp_tx_start_encryption_req(conn);
                    conn->core.step = LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ;
                }
                conn->role_machine_handler = lmp_master_machine;
            } else {
                if (paused) {
                    lmp_tx_resume_encryption_req(conn);
                    conn->core.step = LMP_WAIT_START_ENCRYPTION_REQ;
                }
                conn->role_machine_handler = lmp_slave_machine;
            }
            if (!paused) {
                conn->core.step = LMP_TX_NULL;
                conn->core.stage = LMP_STAGE_CONNECTED;
            }
            lmp_send_event_role_change(conn, 0);
        } else {
            enable_afh_again(conn);
            lmp_send_event_role_change(conn, 0x35);
            conn->err_code = 0x08;
            conn->core.state |= STATE_DETACHING;
        }
    } else {
        if (rs->state == ROLE_SWITCH_SUSS) {
            lmp_send_event_role_change(conn, 0);
        } else {
            enable_afh_again(conn);
            lmp_send_event_role_change(conn, 0x35);
            conn->err_code = 0x08;
            conn->core.state |= STATE_DETACHING;
        }
        conn->role_machine_handler = conn->core.role == 0 ? lmp_master_machine : lmp_slave_machine;
    }
    lmp_role_switch_misc_free(conn);
}

static void no_accepted_switch_req(struct lmp_connection *conn)
{
    lmp_role_switch_misc_free(conn);
    lmp_response(conn, "111", PDU(NOT_ACCEPTED), SWITCH_REQ, 0x21);
    lmp_send_event_role_change(conn, 0x21);
}

void lmp_rx_switch_req(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_role_switch *rs;
    if (!(hci_param.features[0] & 0x20)) {     /* we do not support role switch */
        no_accepted_switch_req(conn);
        return;
    }
    lmp_role_switch_misc_alloc(conn);
    conn->core.role_switch = 0;
    rs = conn->rs_process;
    rs->init_by_local = 0;
    conn->role_switch_state = 1;
    if (rs->timer0 == NO_TIMER) rs->timer0 = slot_timer_get();
    rs->instant = rx[0] | rx[1] << 8 | (u32)rx[2] << 16 | (u32)rx[3] << 24;
    if (conn->core.role == 0) {
        lmp_response(conn, "11", PDU(ACCEPTED), SWITCH_REQ);
    } else {
        lmp_response(conn, "12A", PDU(SLOT_OFFSET), lc_local_slot_offset(conn->link), hci_param.addr);
        lmp_response(conn, "11", PDU(ACCEPTED), SWITCH_REQ);
    }
    slot_timer_set(rs->timer0, __role_switch_probe, conn, 4);
    if (conn->core.stage != LMP_STAGE_CONN) conn->core.stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
}

void lmp_rx_accepted_switch_req(struct lmp_connection *conn)
{
    struct lmp_role_switch *rs = conn->rs_process;
    if (!rs) return;
    if (!conn->role_switch_state) {     /* accepted a request we did not send */
        lmp_role_switch_misc_free(conn);
        return;
    }
    if (rs->timer0 == NO_TIMER) rs->timer0 = slot_timer_get();
    slot_timer_set(rs->timer0, __role_switch_probe, conn, 4);
}

void lmp_rx_not_accepted_switch_req(struct lmp_connection *conn)
{
    struct lmp_role_switch *rs = conn->rs_process;
    if (!rs) return;
    if (rs->timer0 != NO_TIMER) { slot_timer_put(rs->timer0); rs->timer0 = NO_TIMER; }
    if (rs->timer1 != NO_TIMER) { slot_timer_put(rs->timer1); rs->timer1 = NO_TIMER; }
    rs->over_role_switch = 1;
    rs->state = ROLE_SWITCH_FAILD;
    if (link_conn_num_more_than_one()) {
        conn->tran_id = 0;
        lmp_tx_detch(conn, 0x13);
        return;
    }
    if (!(conn->core.state & 2)) {      /* not yet connected: accept anyway */
        conn->tran_id = 0;
        lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
        lmp_setup_complete(conn);
        conn->core.step = LMP_WAIT_AUTH;
    }
    lmp_send_event_role_change(conn, 0x21);
    conn->rs_process->state = ROLE_SWITCH_GIVEUP;
}

void lmp_rx_slot_offset(struct lmp_connection *conn, u8 *rx)
{
    lmp_role_switch_misc_alloc(conn);
    if (conn->rs_process) conn->rs_process->slot_offset = (u16)(rx[0] | rx[1] << 8);
}

/* A role switch we start on an encrypted central link: pause encryption,
 * switch, start it again (lmp_role_switch_machine). */
int lmp_master_stage_role_switch_on_enc(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                        u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_role_switch *rs;
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_TX_PAUSE_ENCRYPTION_REQ:
        conn->core.enc_state |= 0x40;
        lmp_tx_pause_encryption_req(conn);
        conn->core.step = LMP_WAIT_PAUSE_ENCRYPTION_REQ;
        break;
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
    case LMP_TX_ROLE_SWITCH:
        lmp_tx_role_switch_req(conn);
        conn->core.step = LMP_WAIT_SLOT_OFFSET;
        break;
    case LMP_WAIT_SLOT_OFFSET:
        if (opcode0 == PDU(SLOT_OFFSET)) conn->core.step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
        break;
    case LMP_WAIT_ACCEPT_START_ENCRYPTION_REQ:
        if (opcode0 == PDU(NOT_ACCEPTED) || opcode1 != PDU(START_ENCRYPTION_REQ)) break;
        lmp_send_event_encryption_change(conn);
        conn->core.step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
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
