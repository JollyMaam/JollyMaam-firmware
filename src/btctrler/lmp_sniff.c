/* Owned lmp_sniff.c: Classic sniff mode.
 *
 * Negotiation: the host (lmp_hci_sniff_mode) or the peer (lmp_rx_sniff_req)
 * proposes; with a standard HCI host the peer's parameters are accepted as
 * they are, unless a second sniffing link needs another offset.
 *
 * Anchor points, on the connection's slot timer: lmp_sniff_anchor_point_preset
 * finds the first one; lmp_sniff_anchor_point_first enters sniff; then each
 * anchor point runs lmp_sniff_pre_anchor_point (a central first wakes the
 * frame in lmp_sniff_wakeup), lmp_sniff_anchor_point (open the attempt
 * window, send a pending unsniff), and lmp_sniff_anchor_timeout (close it,
 * plan the next one with lmp_sniff_anchor_point_set). */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_TWS, "written for a standard HCI host without TWS");
#ifndef UNITTEST
_Static_assert(sizeof(struct lmp_sniff) == 40, "the SDK allocated 40 bytes");
#endif

/* lmp_sniff.state */
#define SNIFF_ACTIVE      (1u << 0)     /* counted in sniff_num */
#define SNIFF_LOCAL_REQ   (1u << 2)     /* we proposed (transaction is ours) */
#define SNIFF_RX_UNSNIFF  (1u << 3)
#define SNIFF_TX_UNSNIFF  (1u << 4)
#define SNIFF_UNSNIFF_DUE (1u << 7)     /* send LMP_unsniff_req at the next anchor */
#define SNIFF_SUBRATING   (1u << 9)

#define NO_TIMER 0xff
#define SLOT_MASK 0x7ffffff             /* the 27-bit slot clock */
#define CLOCK_NEAR_WRAP 134212928

/* Defaults for sniff we ask for (lmp_sniff_t_slot_attemp_reset). */
static u16 sniff_t_slot = 800, sniff_attemp = 4;
static u8 sniff_num;                    /* links in sniff */
static bool sniff_anchor_point_start_tx_empty;

static void link_conn_run(struct link_connection *link)
{
    link_task_run(link ? &link->task : 0);
}

void tws_ble_sniff_slot_reset(u8 type)
{ (void)type; sniff_t_slot = 800; }

void lmp_sniff_t_slot_attemp_reset(u16 slot, u16 attemp)
{
    sniff_t_slot = slot;
    sniff_attemp = attemp;
}

bool lmp_sniff_is_the_main_sniff(struct lmp_connection *conn)
{
    return main_conn && main_conn == conn;
}

struct lmp_sniff *lmp_sniff_misc_alloc(struct lmp_connection *conn)
{
    if (conn->sniff) return conn->sniff;
    conn->sniff = malloc(sizeof(*conn->sniff));
    if (!conn->sniff) btctrler_assert_failed("conn->sniff", 0);
    memset(conn->sniff, 0, sizeof(*conn->sniff));
    conn->sniff->timer = slot_timer_get();
    conn->link->unsniff_ack = 0;
    if (!main_conn) main_conn = conn;
    else other_conn = conn;
    return conn->sniff;
}

void lmp_sniff_misc_free(struct lmp_connection *conn, u8 run)
{
    struct lmp_sniff *s = conn->sniff;
    if (!s) return;
    link_task_idle_disable();
    local_irq_disable();
    if (s->state & SNIFF_ACTIVE) {
        s->state &= ~SNIFF_ACTIVE;
        sniff_num--;
    }
    lc_sniff_ctrl(conn->link, SNIFF_CTRL_EXIT, 0);
    if (s->timer != NO_TIMER) slot_timer_put(s->timer);
    free(s);
    conn->sniff = 0;
    if (main_conn == conn) {
        main_conn = other_conn;
        other_conn = 0;
    } else if (other_conn == conn) {
        other_conn = 0;
    }
    link_task_set_period(0);
    link_task_run_set(&conn->link->task, 1);
    if (run) link_task_run_slots(&conn->link->task, 64);
    local_irq_enable();
}

void lmp_sniff_detach(struct lmp_connection *conn)
{
    if (conn->core.operation_mode == SNIFF_MODE) {
        conn->core.operation_mode = ACTIVE_MODE;
        lmp_send_event_mode_change(conn, 0);
    }
    lmp_sniff_misc_free(conn, 1);
}

/* Sniff allowed by the link policy? */
bool lmp_rx_sniff_standby(struct lmp_connection *conn)
{
    return conn->core.link_policy & 4;
}

void lmp_sniff_subrating_cnt(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    u32 master_slot, local_slot;
    if (!s || !(s->state & SNIFF_SUBRATING)) return;
    bredr_get_link_slot_clk(conn->link, &master_slot, &local_slot);
    s->local_timeout = s->minimum_local_timeout + local_slot;
}

/* How far the link layer moves an AFH instant on a sniffing link. */
s32 lmp_sniff_and_afh_offset_ali(void *lc, u32 instant)
{
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    (void)instant;
    if (!conn || lmp_get_esco_conn_statu() || conn->core.operation_mode != SNIFF_MODE) return 0;
    return conn->sniff ? conn->sniff->attempt * 2 : 0;
}

/* Slots from now to this link's next anchor point (D_sniff, mod T_sniff). */
static u16 lmp_sniff_cal_offset(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    u32 master_slot, local_slot;
    if (!s) btctrler_assert_failed("sniff", 0);
    bredr_get_link_slot_clk(conn->link, &master_slot, &local_slot);
    if (s->timing_control_flags & 2) master_slot ^= 0x4000000;
    int sub = s->D_sniff - (int)(master_slot % s->T_sniff);
    return (u16)(sub + (sub < 0 ? s->T_sniff : 0));
}

/* A D_sniff for a second link with the same interval that keeps its anchor
 * points about half an interval away from the first link's.  0: keep. */
u16 lmp_sniff_cal_other_D_sniff(struct lmp_connection *_main_conn, struct lmp_connection *_other_conn)
{
    struct lmp_sniff *main_sniff = _main_conn->sniff, *other_sniff = _other_conn->sniff;
    if (!main_sniff) btctrler_assert_failed("_main_sniff", 0);
    if (!other_sniff) btctrler_assert_failed("_other_sniff", 0);
    if (main_sniff->T_sniff != other_sniff->T_sniff) return 0;
    u16 main_offset = lmp_sniff_cal_offset(_main_conn), other_offset = lmp_sniff_cal_offset(_other_conn);
    int delta = (int)main_offset - (int)other_offset, ret;
    if (delta < 0) delta = -delta;
    u16 t = main_sniff->T_sniff;
    if (delta < t >> 1) ret = main_offset > other_offset ? (t >> 1) + delta : (t >> 1) - delta;
    else ret = main_offset > other_offset ? t + delta - (t >> 1) : t - delta + (t >> 1);
    return (u16)ret == t ? 0 : (u16)ret;
}

static void lmp_sniff_anchor_point(void *priv);

/* Slot timer, a few slots before an anchor point: open the frame. */
void lmp_sniff_pre_anchor_point(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *s = conn->sniff;
    struct link_connection *link = conn->link;
    sniff_anchor_point_start_tx_empty = link_conn_tx_empty(link) != 0;
    link->sniff_state = SNIFF_POINT;
    link->sniff_Communicate = 0;
    lc_sniff_ctrl(link, SNIFF_CTRL_FRAME_ENABLE, 0);
    if (lmp_sniff_is_the_main_sniff(conn)) {
        link_task_schedule_reset();
        link_task_set_period(1);
    }
    link_conn_run(link);
    if (s->wakeup_offset > 1) slot_timer_set(s->timer, lmp_sniff_anchor_point, conn, s->wakeup_offset);
    else lmp_sniff_anchor_point(conn);
}

/* A central disables its frame until shortly before the anchor point. */
void lmp_sniff_wakeup(void *priv)
{
    struct lmp_connection *conn = priv;
    lc_sniff_ctrl(conn->link, SNIFF_CTRL_FRAME_DISABLE, 0);
    slot_timer_set(conn->sniff->timer, lmp_sniff_pre_anchor_point, conn, 2);
}

/* Plan the next anchor point.  With low_power_enable the frame sleeps until
 * then; short intervals on a quiet link stretch to sniff_long_interval. */
static void lmp_sniff_anchor_point_set(struct lmp_connection *conn, u8 low_power_enable)
{
    struct lmp_sniff *s = conn->sniff;
    struct link_connection *link = conn->link;
    u32 master_slot, local_slot, parm;
    int tx_empty = link_conn_tx_empty(link);
    int reset = sniff_support_reset_anchor_point, interval = 0, result = 2, slot;

    if (reset && s->T_sniff < 100) {
        if (tx_empty && sniff_anchor_point_start_tx_empty) {
            if (link->sniff_Communicate) {
                if (link->sniff_long_interval_filter) {
                    link->sniff_long_interval_filter--;
                } else {
                    link->sniff_pre_point_reset_flag = 1;
                    interval = sniff_long_interval;
                }
            } else {
                link->sniff_pre_point_reset_flag = 0;
                interval = s->T_sniff;
                if (link->sniff_long_interval_filter && --link->sniff_long_interval_filter == 0) {
                    link->sniff_pre_point_reset_flag = 1;
                    interval = sniff_long_interval;
                }
            }
        } else {
            link->sniff_pre_point_reset_flag = 0;
            link->sniff_long_interval_filter = 2;
            interval = s->T_sniff;
        }
    }

    master_slot = bredr_get_master_slot_clk(link);
    bredr_get_link_slot_clk(link, &master_slot, &local_slot);
    if (s->timing_control_flags & 2) master_slot ^= 0x4000000;
    int sub = s->D_sniff - (int)(master_slot % s->T_sniff);
    int offset = sub + (sub < 0 ? s->T_sniff : 0);

    if (low_power_enable) {
        lc_sniff_ctrl(link, SNIFF_CTRL_FRAME_DISABLE, 0);
        parm = (u32)interval;
        if (!reset || interval == sniff_long_interval ||
            (link->sniff_rx_flag && link->sniff_long_interval_filter == 0))
            result = lc_sniff_ctrl(link, SNIFF_CTRL_INTERVAL_OFFSET, &parm);
    }

    int t = s->T_sniff;
    if (interval == sniff_long_interval && reset && t < 100) {
        int at = (interval / t - 1) * t + offset;
        slot = at + (at > result ? 0 : t) - result;
    } else {
        slot = offset + (offset > result ? 0 : t) - result;
        if (t > 100) while (slot < t >> 1) slot += t;
    }
    s->wakeup_offset = (s8)result;
    s->wake_instant = (local_slot + slot) & SLOT_MASK;
    if (conn->core.role == 0)
        slot_timer_set_ext(s->timer, lmp_sniff_wakeup, conn, slot, local_slot);
    else
        slot_timer_set_ext(s->timer, lmp_sniff_pre_anchor_point, conn, slot, local_slot);
    link->sniff_state = SNIFF_PRE_POINT;
}

/* The attempt window is over: unsniff, or plan the next anchor point. */
void lmp_sniff_anchor_timeout(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *s = conn->sniff;
    if (conn->link->unsniff_ack) {
        if (!(s->state & SNIFF_RX_UNSNIFF)) btctrler_assert_failed("sniff->state & LMP_STA_RX_UNSNIFF", 0);
        lmp_sniff_misc_free(conn, 1);
        conn->core.operation_mode = ACTIVE_MODE;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    if (conn->core.operation_mode == ACTIVE_MODE) {
        lmp_sniff_misc_free(conn, 1);
        conn->core.operation_mode = ACTIVE_MODE;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    if (sniff_num == 1) {
        if (s->state & (SNIFF_RX_UNSNIFF | SNIFF_TX_UNSNIFF)) {
            lmp_sniff_anchor_point_set(conn, 0);
        } else {
            lmp_sniff_anchor_point_set(conn, 1);
            link_task_set_period((int)s->wake_instant - 12);
        }
    } else if (sniff_num == 2) {
        if (!main_conn) btctrler_assert_failed("main_conn", 0);
        if (!other_conn) btctrler_assert_failed("other_conn", 0);
        struct lmp_sniff *ms = main_conn->sniff, *os = other_conn->sniff;
        if ((ms->state | os->state) & (SNIFF_RX_UNSNIFF | SNIFF_TX_UNSNIFF)) {
            lmp_sniff_anchor_point_set(conn, 0);
        } else {
            lmp_sniff_anchor_point_set(conn, 1);
            ms = main_conn->sniff;
            os = other_conn->sniff;
            link_task_set_period((int)(ms->wake_instant > os->wake_instant ? os : ms)->wake_instant - 12);
            if (ms->T_sniff == os->T_sniff) {
                int dt = (int)(ms->wake_instant - os->wake_instant);
                if (dt < 0) dt = -dt;
                /* Anchor points too close together: the second link leaves sniff. */
                if (dt < 6) lmp_hci_exit_sniff_mode(other_conn);
            }
        }
    }
    link_conn_run(0);
}

/* Anchor point: open the attempt window; send a pending unsniff. */
static void lmp_sniff_anchor_point(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *s = conn->sniff;
    struct link_connection *link = conn->link;
    u32 master_slot, local_slot;
    int extra = (s->state & SNIFF_UNSNIFF_DUE) ? 4 : 0;
    link_task_try_run_slots(&link->task, s->attempt * 2 + 4 + extra);
    link->sniff_state = SNIFF_TIMEOUT;
    slot_timer_set(s->timer, lmp_sniff_anchor_timeout, conn, s->attempt * 2 + extra);
    bredr_get_link_slot_clk(link, &master_slot, &local_slot);
    /* Leave sniff before a clock wraps. */
    if (master_slot > CLOCK_NEAR_WRAP) lmp_tx_unsniff_req(conn);
    if (local_slot > CLOCK_NEAR_WRAP) lmp_tx_unsniff_req(conn);
    if (s->state & SNIFF_UNSNIFF_DUE) {
        s->state = (u16)((s->state & ~SNIFF_UNSNIFF_DUE) | SNIFF_TX_UNSNIFF);
        lmp_request(conn, "1", PDU(UNSNIFF_REQ));
    }
    sniff_achor_point_hook(master_slot);
    sniff_hook(master_slot, (u8)link_conn_num_more_than_one(), conn->core_s.handle == 0x30,
               s->T_sniff * 625 / 1000);
}

/* Sniff entered at this link: count it and wait for the first window. */
static void lmp_sniff_anchor_point_first(void *priv)
{
    struct lmp_connection *conn = priv;
    struct lmp_sniff *s = conn->sniff;
    struct link_connection *link = conn->link;
    if (lmp_sniff_is_the_main_sniff(conn)) {
        link_task_schedule_reset();
        link_task_set_period(1);
    }
    lc_sniff_ctrl(link, SNIFF_CTRL_ENTER, 0);
    local_irq_disable();
    s->state |= SNIFF_ACTIVE;
    sniff_num++;
    local_irq_enable();
    s->wake_instant = (s->T_sniff + bredr_read_slot_clk()) & SLOT_MASK;
    link_idle_task_enable_detect();
    lc_sniff_ctrl(link, SNIFF_CTRL_FRAME_ENABLE, 0);
    link_task_run_set(&link->task, 0);
    link_conn_run(link);
    slot_timer_set(s->timer, lmp_sniff_anchor_timeout, conn, s->attempt * 2);
}

/* Sniff agreed: time the first anchor point (2 slots early). */
void lmp_sniff_anchor_point_preset(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    u32 master_slot, local_slot;
    if (lmp_get_esco_conn_statu() == 1) {
        local_irq_disable();
        if (s->timer != NO_TIMER) {
            slot_timer_put(s->timer);
            s->timer = NO_TIMER;
        }
        local_irq_enable();
        return;
    }
    bredr_get_link_slot_clk(conn->link, &master_slot, &local_slot);
    if (s->timing_control_flags & 2) master_slot ^= 0x4000000;
    int t = s->T_sniff, slot = s->D_sniff - (int)(master_slot % t);
    while (slot < t >> 1) slot += t;
    slot -= 2;
    s->wakeup_offset = 0;
    s->wake_instant = (local_slot + slot) & SLOT_MASK;
    slot_timer_set_ext(s->timer, lmp_sniff_anchor_point_first, conn, slot, local_slot);
}

/* HCI Sniff Mode.  The interval asked for is at most sniff_t_slot; the
 * minimum is not used. */
void lmp_hci_sniff_mode(struct lmp_connection *conn, u16 sniff_max_interval, u16 sniff_min_interval,
                        u16 attempt, u16 timeout)
{
    struct lmp_sniff *s;
    (void)sniff_min_interval;
    if (!conn || !conn->link || !(hci_param.features[0] & 0x80)) return;
    if (!lmp_rx_sniff_standby(conn)) {     /* Mode Change: Command Disallowed */
        hci_send_event(0x14, "1212", 0x0c, conn->core_s.handle, conn->core.operation_mode, 0);
        return;
    }
    if (conn->core.operation_mode == SNIFF_MODE || (conn->core.state & 8)) {
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    if (conn->sniff) return;                /* already negotiating */
    s = lmp_sniff_misc_alloc(conn);
    s->negotiation_cnt = 0;
    s->timing_control_flags = (bredr_get_master_slot_clk(conn->link) & 0x4000000) ? 2 : 0;
    s->T_sniff = sniff_t_slot < sniff_max_interval ? sniff_t_slot : sniff_max_interval;
    s->D_sniff = 0;
    s->attempt = attempt;
    s->timeout = timeout;
    if (!lmp_sniff_is_the_main_sniff(conn) && main_conn->sniff->T_sniff == s->T_sniff) {
        u16 d = lmp_sniff_cal_other_D_sniff(main_conn, conn);
        if (d) {
            u16 v = (u16)((s->D_sniff + d) % s->T_sniff);
            s->D_sniff = v + (v & 1);
        }
    }
    s->state |= SNIFF_LOCAL_REQ;
    lmp_request(conn, "112222", PDU(SNIFF_REQ), s->timing_control_flags, s->D_sniff, s->T_sniff,
                s->attempt, s->timeout);
}

void lmp_tx_sniff_req(struct lmp_connection *conn)
{
    lmp_hci_sniff_mode(conn, 800, 0, 4, 1);
}

void lmp_tx_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    if (conn->link->tws_link || conn->core.operation_mode == ACTIVE_MODE || !s) return;
    if (s->state & (SNIFF_RX_UNSNIFF | SNIFF_TX_UNSNIFF)) return;     /* already leaving */
    u16 state = s->state;
    s->state = state | SNIFF_UNSNIFF_DUE;
    if (!(state & SNIFF_ACTIVE)) lmp_request(conn, "1", PDU(UNSNIFF_REQ));
}

void lmp_hci_exit_sniff_mode(struct lmp_connection *conn)
{
    if (conn->core.operation_mode == ACTIVE_MODE) {
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    if (!conn->sniff) {
        conn->core.operation_mode = ACTIVE_MODE;
        lmp_send_event_mode_change(conn, 0);
        return;
    }
    lmp_tx_unsniff_req(conn);
}

static void accept_sniff(struct lmp_connection *conn)
{
    lmp_response(conn, "11", PDU(ACCEPTED), SNIFF_REQ);
    conn->link->sniff_pre_point_reset_flag = 1;
    lmp_sniff_anchor_point_preset(conn);
    conn->core.operation_mode = SNIFF_MODE;
    lmp_send_event_mode_change(conn, conn->sniff->T_sniff);
}

void lmp_rx_sniff_req(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_sniff *s;
    if (lmp_rx_sniff_req_check()) return;
    if (!(hci_param.features[0] & 0x80)) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x11);
        return;
    }
    s = conn->sniff;
    if (s) {        /* both sides proposing */
        bool ours = s->state & SNIFF_LOCAL_REQ, same = conn->tran_id == conn->core.role;
        if (ours != same) {
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x0c);
            return;
        }
    }
    if (conn->core.operation_mode == SNIFF_MODE) return;
    s = lmp_sniff_misc_alloc(conn);
    u8 flags = rx[0];
    u16 d = (u16)(rx[1] | rx[2] << 8), t = (u16)(rx[3] | rx[4] << 8);
    u16 attempt = (u16)(rx[5] | rx[6] << 8), timeout = (u16)(rx[7] | rx[8] << 8);
    u16 new_d = d;
    int negotiate = 0;
    if (!lmp_sniff_is_the_main_sniff(conn) && main_conn->sniff->T_sniff == t) {
        u16 other = lmp_sniff_cal_other_D_sniff(main_conn, conn);
        if (other) {
            new_d = (u16)((other + d) % t);
            negotiate = 1;
        }
    }
    s->timing_control_flags = flags;
    s->D_sniff = new_d;
    s->T_sniff = t;
    s->attempt = attempt;
    s->timeout = timeout;
    if (!negotiate) {
        s->negotiation_cnt = 0;
        accept_sniff(conn);
        return;
    }
    u8 cnt = ++s->negotiation_cnt;
    if (cnt > 3) {
        if (t < 64) {
            lmp_sniff_misc_free(conn, 1);
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x20);
            return;
        }
        s->negotiation_cnt = 0;     /* give in: take the peer's parameters */
        s->D_sniff = d;
        accept_sniff(conn);
        return;
    }
    sniff_negotiation_hook(&s->T_sniff, &s->attempt, &s->timeout, cnt);
    lmp_response(conn, "112222", PDU(SNIFF_REQ), s->timing_control_flags, s->D_sniff, s->T_sniff,
                 s->attempt, s->timeout);
}

void lmp_rx_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    if (!s) return;
    if (conn->core.role == 0) {
        s->state |= SNIFF_RX_UNSNIFF;
        lmp_response(conn, "S11", PDU(ACCEPTED), UNSNIFF_REQ);
        return;
    }
    lmp_response(conn, "11", PDU(ACCEPTED), UNSNIFF_REQ);
    conn->core.operation_mode = ACTIVE_MODE;
    if (!(s->state & SNIFF_ACTIVE)) {
        lmp_sniff_misc_free(conn, 0);
        lmp_send_event_mode_change(conn, 0);
    }
}

void lmp_rx_accepted_sniff_req(struct lmp_connection *conn)
{
    local_irq_disable();
    struct lmp_sniff *s = conn->sniff;
    if (!s) {
        local_irq_enable();
        return;
    }
    s->negotiation_cnt = 0;
    lmp_sniff_anchor_point_preset(conn);
    conn->core.operation_mode = SNIFF_MODE;
    lmp_send_event_mode_change(conn, s->T_sniff);
    local_irq_enable();
}

void lmp_rx_accepted_unsniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    if (!s) return;
    u16 state = s->state;
    s->state = state | SNIFF_RX_UNSNIFF;
    lmp_sniff_misc_free(conn, (state & SNIFF_ACTIVE) ? 1 : 0);
    conn->core.operation_mode = ACTIVE_MODE;
    lmp_send_event_mode_change(conn, 0);
}

void lmp_rx_not_accepted_sniff_req(struct lmp_connection *conn)
{
    struct lmp_sniff *s = conn->sniff;
    conn->core.operation_mode = ACTIVE_MODE;
    if (s) {
        bool ours = s->state & SNIFF_LOCAL_REQ, same = conn->tran_id == conn->core.role;
        /* Free only if the refused transaction is the one we hold. */
        if (ours == same) lmp_sniff_misc_free(conn, 0);
    }
    lmp_send_event_mode_change(conn, 0);
}

void lmp_rx_not_accepted_unsniff_req(struct lmp_connection *conn)
{
    conn->core.operation_mode = ACTIVE_MODE;
}

#ifdef UNITTEST
#include <assert.h>
#include <stdio.h>
struct hci_parameter hci_param;
struct lmp_connection *main_conn, *other_conn;
static struct link_connection test_link;
static int requests, requested_interval, requested_attempt, requested_timeout;
u8 slot_timer_get(void) { return 7; }
u32 bredr_get_master_slot_clk(struct link_connection *lc)
{ assert(lc == &test_link); return 0x4000000; }
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; assert(0); }
int lmp_request(struct lmp_connection *conn, const char *format, ...)
{
    assert(conn->link == &test_link && !strcmp(format, "112222"));
    va_list args; va_start(args, format);
    assert(va_arg(args, int) == 46); /* LMP_sniff_req */
    assert(va_arg(args, int) == 2 && va_arg(args, int) == 0);
    requested_interval = va_arg(args, int);
    requested_attempt = va_arg(args, int); requested_timeout = va_arg(args, int);
    va_end(args); ++requests; return 0;
}
static void negotiate_default(int expected)
{
    struct lmp_connection conn = {0}; conn.link = &test_link; conn.core.link_policy = 4;
    memset(&test_link, 0, sizeof(test_link)); main_conn = other_conn = NULL;
    hci_param.features[0] = 0x80; requests = 0;
    lmp_hci_sniff_mode(&conn, 900, 2, 7, 3);
    assert(requests == 1 && requested_interval == expected && requested_attempt == 7 && requested_timeout == 3);
    assert(conn.sniff && conn.sniff->timer == 7);
    free(conn.sniff); main_conn = other_conn = NULL;
}
int main(void)
{
    /* A BLE/TWS slot reset must affect the next owned Classic negotiation,
     * after the application has changed its default interval. */
    for (unsigned type = 0; type < 256; ++type) {
        lmp_sniff_t_slot_attemp_reset(120, 4);
        negotiate_default(120);
        tws_ble_sniff_slot_reset(type);
        negotiate_default(800);
    }
    puts("btctrler sniff default negotiation tests passed");
}
#endif
