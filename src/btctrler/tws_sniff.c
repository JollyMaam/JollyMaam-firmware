/* The SDK's TWS sniff coordination (tws_sniff.c): the earbud pair follows
 * the phone's sniff schedule.  One side keeps the sniff parameters in
 * tws_sniff, arms a slot timer for the anchor point and wakes the Classic
 * link a few slots early, then lets the link task run until the anchor
 * point timer expires.  Entry points are called by retained TWS code
 * (tws_pair) and tws_tx_unsniff_req by bredr_link.c.  Quirks kept as the
 * SDK has them: tws_sniff_with_phone reads tws_conn.lmp_conn->sniff without
 * checking lmp_conn; tws_rx_unsniff_req dereferences tws_link.link without
 * checking it when the peer's flag is not 0x55; asserts continue after the
 * report (or reset); the "without phone" entry points and the transmit
 * requests are empty; wake instants are masked to 27 bits only where the SDK
 * masks them (tws_sniff_fap_set does not).  cpu_reset has no caller left
 * and stays in the SDK object. */
#include <stddef.h>
#include "btctrler/glue.h"

/* The slot timer callbacks receive the connection, which is the address of
 * its first member. */
_Static_assert(offsetof(struct lmp_connection, rejust_esco_req) == 0, "conn address");

#ifdef UNITTEST
extern u8 log_tag_const_i_TWS, log_tag_const_d_TWS;
#else
extern const u8 log_tag_const_i_TWS, log_tag_const_d_TWS;
#endif
void log_print(int level, const char *file, const char *format, ...);

/* The SDK's file-scope sniff state (no section). */
struct lmp_tws_sniff tws_sniff;

static void tws_reset_2_slot(void *parm);
static void tws_sniff_first_anchor_point(void *_conn);
static void tws_sniff_wakeup(void *_conn);
static void tws_sniff_pre_anchor_point(void *_conn);
static void tws_sniff_anchor_point_timeout(void *_conn);
static void tws_sniff_without_phone_wakeup(void *_conn);
static void tws_sniff_without_phone_pre_ap(void *p);
static void tws_sniff_without_phone_ap_timeout(void *_conn);

/* Slot-timer callback: leave the two-slot restriction after an unsniff. */
static void tws_reset_2_slot(void *parm)
{
    (void)parm;
    local_irq_disable();
    if (tws_sniff.reset_timer) {
        slot_timer_put(tws_sniff.reset_timer);
        tws_sniff.reset_timer = 0;
    }
    local_irq_enable();
    link_task_idle_run(2);
    if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]tws_reset_2_slot\r\n");
}

void tws_rx_sniff_req(u8 *rx)
{
    tws_conn_send_cmd(2, "11", 15, *rx);
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s:%d\r\n", "tws_rx_sniff_req", *rx);
    if (*rx == 0x55) {
        tws_lmp_rx_sniff_req(tws_conn.lmp_conn, rx + 1);
        tws_sniff_with_phone();
    }
}

void tws_rx_unsniff_req(u8 *rx)
{
    tws_conn_send_cmd(2, "11", 17, *rx);
    if (*rx == 0x55) {
        tws_lmp_rx_unsniff_req(tws_conn.lmp_conn);
        tws_unsniff_with_phone();
    } else {
        link_task_run_slots(&tws_link.link->task, 20);
    }
    if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]sync:rx unsniff\r\n");
}

void tws_rx_unsniff_accept(u8 *rx)
{
    if (*rx == 0x55)
        tws_unsniff_with_phone();
}

void tws_rx_sniff_accept(u8 *rx)
{
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s:%d\r\n", "tws_rx_sniff_accept", *rx);
    if (*rx == 0x55)
        tws_sniff_with_phone();
}

void tws_tx_unsniff_req(void)
{
}

void tws_tx_sniff_req(void)
{
}

void tws_sniff_ap_exit(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    local_irq_disable();
    if (tws_sniff.enable && tws_sniff.sync && (tws_sniff.state & 1)) {
        int without_phone = tws_sniff.witchout_phone;
        tws_sniff.sync = 0;
        if (without_phone)
            tws_sniff_without_phone_ap_set();
        else
            tws_sniff_ap_set(conn);
    }
    local_irq_enable();
}

void tws_sniff_ap_enter(void)
{
    tws_conn_sync_tx_operation_mode();
}

/* Slot-timer callbacks of the anchor point sequence: wake early, then wait
 * for the anchor point or give up after the timeout. */
static void tws_sniff_anchor_point_timeout(void *_conn)
{
    (void)_conn;
    link_task_run(0);
}

static void tws_sniff_pre_anchor_point(void *_conn)
{
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_ENABLE, 0);
    tws_sniff.sync = 1;
    link_task_run(&tws_link.link->task);
    slot_timer_set(tws_sniff.timer, tws_sniff_anchor_point_timeout, _conn,
                   (int)tws_sniff.wakeup_offset + 8 + ((tws_sniff.state & 16) ? 20 : 0));
}

static void tws_sniff_wakeup(void *_conn)
{
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_DISABLE, 0);
    slot_timer_set(tws_sniff.timer, tws_sniff_pre_anchor_point, _conn, 2);
}

static void tws_sniff_first_anchor_point(void *_conn)
{
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s\r\n", "tws_sniff_first_anchor_point");
    tws_link.link->core.max_slot = 1;
    if (!((tws_sniff.state & 1) == 0))
        btctrler_assert_failed("(tws_sniff.state & LMP_STA_SNIFF_EN) == 0 ", 0);
    tws_sniff.state = tws_sniff.state | 1;
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_ENTER, 0);
    link_idle_task_enable_detect();
    link_task_run_set(&tws_link.link->task, 0);
    tws_sniff_pre_anchor_point(_conn);
}

void tws_sniff_fap_set(struct lmp_connection *conn)
{
    u32 master_slot, local_slot;
    int pre_ap_slot;

    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s\r\n", "tws_sniff_fap_set");
    bredr_get_link_slot_clk(tws_link.link, &master_slot, &local_slot);
    local_irq_disable();
    if (conn && conn->sniff) {
        struct lmp_sniff *sniff = conn->sniff;

        pre_ap_slot = (int)((sniff->wake_instant + 20 + (u32)(int)sniff->wakeup_offset) - local_slot);
        if (pre_ap_slot < 10)
            pre_ap_slot = (int)(sniff->T_sniff + (u32)pre_ap_slot);
        if (!(pre_ap_slot > 0))
            btctrler_assert_failed("pre_ap_slot > 0 pre_ap_slot:%d", pre_ap_slot);
        tws_sniff.wakeup_offset = 0;
        tws_sniff.wake_instant = local_slot + (u32)pre_ap_slot;
        slot_timer_set_ext(tws_sniff.timer, tws_sniff_first_anchor_point, (void *)conn,
                           pre_ap_slot, local_slot);
        if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]tws_fap :%d\r\n", pre_ap_slot);
    }
    local_irq_enable();
}

void tws_sniff_ap_set(struct lmp_connection *conn)
{
    u32 master_slot, local_slot;
    s32 call1;
    struct link_connection *link;
    u32 slot;
    int pre_ap_slot;

    bredr_get_link_slot_clk(tws_link.link, &master_slot, &local_slot);
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_DISABLE, 0);
    call1 = lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_INTERVAL_OFFSET, 0);
    link = tws_link.link;
    if (!link->sniff_rx_flag || tws_sniff.unsniff_retry > 6) {
        tws_sniff.unsniff_retry = 0;
        if (conn && conn->sniff) {
            struct lmp_sniff *sniff = conn->sniff;
            u32 wake = sniff->wake_instant;

            slot = local_slot;
            pre_ap_slot = (int)(((20 - (u32)call1) - slot + (u32)(int)sniff->wakeup_offset) +
                                (wake < slot ? wake + 134217728 : wake));
            if (pre_ap_slot < 10)
                pre_ap_slot = (int)(sniff->T_sniff + (u32)pre_ap_slot);
        } else {
            if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]phone_unsniff\n\r\n");
            tws_unsniff_with_phone();
            return;
        }
    } else {
        u32 wake;

        tws_sniff.unsniff_retry = tws_sniff.unsniff_retry + 1;
        wake = tws_sniff.wake_instant;
        slot = local_slot;
        pre_ap_slot = (int)(((28 - (u32)call1) - slot + (u32)(int)tws_sniff.wakeup_offset) +
                            (wake < slot ? wake + 134217728 : wake));
    }
    tws_sniff.wakeup_offset = (s8)call1;
    tws_sniff.wake_instant = (slot + (u32)pre_ap_slot) & 134217727;
    if (!link->role)
        slot_timer_set_ext(tws_sniff.timer, tws_sniff_wakeup, (void *)conn, pre_ap_slot, slot);
    else
        slot_timer_set_ext(tws_sniff.timer, tws_sniff_pre_anchor_point, (void *)conn,
                           pre_ap_slot, slot);
    if (conn && conn->sniff && !(conn->sniff->state & 24)) {
        u32 a = conn->sniff->wake_instant, b = tws_sniff.wake_instant;
        link_task_set_period((int)((a > b ? b : a) - 12));
    }
}

void tws_sniff_with_phone(void)
{
    local_irq_disable();
    if (tws_link.link) {
        struct lmp_sniff *sniff = tws_conn.lmp_conn->sniff;

        if (sniff && sniff->wake_instant == 0) {
            local_irq_enable();
            return;
        }
        if (!tws_sniff.enable) {
            if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]%s\r\n", "tws_sniff_with_phone");
            tws_sniff.state = 0;
            tws_sniff.enable = 1;
            tws_sniff.sync = 0;
            tws_sniff.witchout_phone = 0;
            tws_sniff.unsniff_retry = 0;
            tws_sniff.timer = slot_timer_get();
            tws_sniff_fap_set(tws_conn.lmp_conn);
        }
    }
    local_irq_enable();
}

void tws_sniff_without_phone(void)
{
}

void tws_lmp_rx_sniff_req(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_sniff sniff_parm;

    if (conn->core.operation_mode == 2)
        return;
    sniff_parm.timing_control_flags = rx[0];
    sniff_parm.D_sniff = (u16)(rx[2] << 8 | rx[1]);
    sniff_parm.T_sniff = (u16)(rx[4] << 8 | rx[3]);
    sniff_parm.attempt = (u16)(rx[6] << 8 | rx[5]);
    sniff_parm.timeout = (u16)(rx[8] << 8 | rx[7]);
    tws_lmp_rx_sniff_sync(conn, &sniff_parm);
}

void tws_lmp_rx_unsniff_req(struct lmp_connection *conn)
{
    if (conn->sniff == 0)
        return;
    conn->core.operation_mode = 0;
    lmp_send_event_mode_change(conn, 0);
    lmp_sniff_misc_free(conn, 0);
}

void tws_unsniff_with_phone(void)
{
    if (tws_sniff.enable)
        link_task_idle_disable();
    local_irq_disable();
    if (tws_link.link && tws_sniff.enable) {
        if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]%s\r\n", "tws_unsniff_with_phone");
        tws_sniff.state = 0;
        tws_sniff.enable = 0;
        tws_sniff.sync = 0;
        tws_sniff.unsniff_retry = 0;
        tws_sniff.wakeup_offset = 0;
        tws_sniff.wake_instant = 0;
        slot_timer_put(tws_sniff.timer);
        link_task_run_set(&tws_link.link->task, 1);
        lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_EXIT, 0);
        link_task_run_slots(&tws_link.link->task, 20);
        if (tws_link.link->role) {
            if (tws_sniff.reset_timer == 0) {
                tws_sniff.reset_timer = slot_timer_get();
                if (tws_sniff.reset_timer == 0)
                    btctrler_assert_failed("tws_sniff.reset_timer ", 0);
            }
            slot_timer_set(tws_sniff.reset_timer, tws_reset_2_slot, 0, 16);
        }
    }
    local_irq_enable();
}

void tws_unsniff_without_phone(void)
{
}

void tws_conn_sync_tx_operation_mode(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct lmp_sniff *sniff;

    if (conn == 0 || tws_conn.monitor_start != 2 || tws_conn.role == 1)
        return;
    sniff = conn->sniff;
    if (conn->core.operation_mode == conn->core.operation_mode_sync)
        return;
    if (conn->core.operation_mode == 2) {
        if (sniff == 0)
            return;
        if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]sync:tx sniff\r\n");
        conn->core.operation_mode_sync = conn->core.operation_mode;
        tws_conn_send_cmd(15, "112222", 0x55, sniff->timing_control_flags, sniff->D_sniff,
                          sniff->T_sniff, sniff->attempt, sniff->timeout);
    } else {
        if (log_tag_const_i_TWS) log_print(2, 0, "[Info]: [TWS]sync:tx unsniff\r\n");
        conn->core.operation_mode_sync = conn->core.operation_mode;
        tws_sniff.state = tws_sniff.state | 16;
        tws_conn_send_cmd(17, "1", 0x55);
    }
}

void tws_sniff_detach(void)
{
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s\r\n", "tws_sniff_detach");
    if (tws_sniff.enable)
        link_task_idle_disable();
    local_irq_disable();
    if (tws_sniff.enable) {
        tws_sniff.state = 0;
        tws_sniff.enable = 0;
        tws_sniff.sync = 0;
        tws_sniff.unsniff_retry = 0;
        tws_sniff.wakeup_offset = 0;
        tws_sniff.wake_instant = 0;
        slot_timer_put(tws_sniff.timer);
        link_task_run_set(&tws_link.link->task, 1);
        lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_EXIT, 0);
    }
    if (tws_conn.lmp_conn)
        tws_conn.lmp_conn->core.operation_mode_sync = 0;
    if (tws_link.link)
        tws_link.link->core.max_slot = 5;
    local_irq_enable();
}

void tws_lmp_rx_sniff_sync(struct lmp_connection *conn, struct lmp_sniff *sniff_parm)
{
    struct lmp_sniff *sniff = lmp_sniff_misc_alloc(conn);
    u8 flags = sniff_parm->timing_control_flags;
    u16 d = sniff_parm->D_sniff, t = sniff_parm->T_sniff, a = sniff_parm->attempt, to = sniff_parm->timeout;

    sniff->timing_control_flags = flags;
    sniff->D_sniff = d;
    sniff->T_sniff = t;
    sniff->attempt = a;
    sniff->timeout = to;
    if (log_tag_const_i_TWS)
        log_print(2, 0, "[Info]: [TWS]conn:0x%x, timing_control_flags:0x%x, D:0x%x, T:0x%x, A:0x%x, TO:0x%x\r\n",
                  conn, flags, d, t, a, to);
    lmp_sniff_anchor_point_preset(conn);
    conn->core.operation_mode = 2;
    lmp_send_event_mode_change(conn, sniff->T_sniff);
}

/* The "without phone" variants of the anchor point sequence. */
static void tws_sniff_without_phone_ap_timeout(void *_conn)
{
    (void)_conn;
    link_task_run(0);
}

static void tws_sniff_without_phone_pre_ap(void *p)
{
    (void)p;
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_ENABLE, 0);
    tws_sniff.sync = 1;
    link_task_run(&tws_link.link->task);
    slot_timer_set(tws_sniff.timer, tws_sniff_without_phone_ap_timeout, 0,
                   (int)tws_sniff.wakeup_offset + 8 + ((tws_sniff.state & 16) ? 6 : 0));
}

static void tws_sniff_without_phone_wakeup(void *_conn)
{
    (void)_conn;
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_DISABLE, 0);
    slot_timer_set(tws_sniff.timer, tws_sniff_without_phone_pre_ap, 0, 2);
}

void tws_sniff_without_phone_fap(void *p)
{
    struct link_connection *link;

    (void)p;
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s\r\n", "tws_sniff_without_phone_fap");
    link = tws_link.link;
    link->core.max_slot = 1;
    tws_sniff.state = tws_sniff.state | 1;
    lc_sniff_ctrl(link, SNIFF_CTRL_ENTER, 0);
    link_idle_task_enable_detect();
    link_task_run_set(&tws_link.link->task, 0);
    tws_sniff_without_phone_pre_ap(0);
    link_task_schedule_resume();
}

void tws_sniff_without_phone_fap_set(void)
{
    u32 master_slot, local_slot;
    int add, pre_ap_slot;
    u16 T;

    bredr_get_link_slot_clk(tws_link.link, &master_slot, &local_slot);
    T = tws_sniff.T_sniff;
    add = (int)(tws_sniff.D_sniff - (master_slot % T)) + T;
    pre_ap_slot = add - 2 + (add < (T >> 1) ? T : 0);
    if (!(pre_ap_slot > 0))
        btctrler_assert_failed("pre_ap_slot > 0 mk:0x%x", (int)master_slot);
    tws_sniff.wakeup_offset = 0;
    tws_sniff.wake_instant = (local_slot + (u32)pre_ap_slot) & 134217727;
    slot_timer_set_ext(tws_sniff.timer, tws_sniff_without_phone_fap, 0, pre_ap_slot, local_slot);
    link_task_schedule_suspend();
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]fap_slot:%d\r\n", pre_ap_slot);
}

void tws_sniff_without_phone_ap_set(void)
{
    u32 master_slot, local_slot;
    s32 call1;
    int sub, add_sub, pre_ap_slot;
    u16 T;

    bredr_get_link_slot_clk(tws_link.link, &master_slot, &local_slot);
    lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_FRAME_DISABLE, 0);
    call1 = lc_sniff_ctrl(tws_link.link, SNIFF_CTRL_INTERVAL_OFFSET, 0);
    T = tws_sniff.T_sniff;
    sub = (int)(tws_sniff.D_sniff - (master_slot % T));
    add_sub = (sub < 0 ? T : 0) + sub;
    pre_ap_slot = (add_sub - call1) + (add_sub > call1 ? 0 : T);
    tws_sniff.wakeup_offset = (s8)call1;
    tws_sniff.wake_instant = (local_slot + (u32)pre_ap_slot) & 134217727;
    if (!tws_link.link->role)
        slot_timer_set_ext(tws_sniff.timer, tws_sniff_without_phone_wakeup, 0, pre_ap_slot, local_slot);
    else
        slot_timer_set_ext(tws_sniff.timer, tws_sniff_without_phone_pre_ap, 0, pre_ap_slot, local_slot);
    link_task_set_period((int)(tws_sniff.wake_instant - 12));
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

u8 log_tag_const_i_TWS, log_tag_const_d_TWS;
struct tws_connection tws_conn;
struct tws_link_connection tws_link;

static struct link_connection lcon;
static struct lmp_connection lmp;
static struct lmp_sniff lsniff, alloc_sniff;
static char trace[256];
static u32 g_master, g_local;
static s32 g_call1;
static u8 g_timer_get;
static int asserts, assert_value, period, slots, ext_slots, run_slots, idle_run, mode_ev_t, sent_n;
static u8 timer_i;
static void (*handler)(void *);
static void *priv;
static const char *assert_what;
static int sent_op, sent_args[8], sent_fmt_len;
static int put_timer, ctrl_cmds[8], n_ctrl, task_run_set_v, task_run_arg_null;
static void *run_task;

static void tr(const char *s) { strcat(trace, s); }
void local_irq_disable(void) { tr("<"); }
void local_irq_enable(void) { tr(">"); }
void log_print(int level, const char *file, const char *format, ...) { (void)level; (void)file; (void)format; tr("L"); }
u8 slot_timer_get(void) { tr("g"); return g_timer_get; }
void slot_timer_put(u8 i) { tr("p"); put_timer = i; }
void slot_timer_set(u8 i, void (*h)(void *), void *p, int s) { tr("s"); timer_i = i; handler = h; priv = p; slots = s; ext_slots = -1; }
void slot_timer_set_ext(u8 i, void (*h)(void *), void *p, int s, int e) { tr("e"); timer_i = i; handler = h; priv = p; slots = s; ext_slots = e; }
s32 lc_sniff_ctrl(struct link_connection *lc, SNIFF_CTRL_CMD cmd, u32 *parm)
{
    CHECK(lc == &lcon && parm == 0);
    ctrl_cmds[n_ctrl++ & 7] = cmd; tr("c");
    return cmd == SNIFF_CTRL_INTERVAL_OFFSET ? g_call1 : 0;
}
void bredr_get_link_slot_clk(struct link_connection *lc, u32 *m, u32 *l) { CHECK(lc == &lcon); *m = g_master; *l = g_local; tr("k"); }
void link_task_run_set(struct link_task *t, u8 v) { run_task = t; task_run_set_v = v; tr("r"); }
void link_task_run(struct link_task *t) { run_task = t; task_run_arg_null = t == 0; tr("R"); }
void link_task_run_slots(struct link_task *t, int s) { run_task = t; run_slots = s; tr("n"); }
void link_task_set_period(int p) { period = p; tr("P"); }
void link_task_idle_run(int s) { idle_run = s; tr("i"); }
u32 link_task_idle_disable(void) { tr("d"); return 0; }
bool link_idle_task_enable_detect(void) { tr("D"); return 0; }
void link_task_schedule_suspend() { tr("u"); }
void link_task_schedule_resume() { tr("v"); }
void lmp_send_event_mode_change(struct lmp_connection *c, u16 t) { CHECK(c == &lmp); mode_ev_t = t; tr("M"); }
void lmp_sniff_misc_free(struct lmp_connection *c, u8 run) { CHECK(c == &lmp && run == 0); lmp.sniff = 0; tr("F"); }
struct lmp_sniff *lmp_sniff_misc_alloc(struct lmp_connection *c) { CHECK(c == &lmp); tr("A"); return &alloc_sniff; }
void lmp_sniff_anchor_point_preset(struct lmp_connection *c) { CHECK(c == &lmp); tr("Z"); }
int tws_conn_send_cmd(u8 opcode, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    sent_op = opcode; sent_fmt_len = (int)strlen(format);
    for (int i = 0; i < sent_fmt_len; i++) sent_args[i] = va_arg(ap, int);
    va_end(ap);
    sent_n++; tr("T");
    return 0;
}
void btctrler_assert_failed(const char *what, int value) { asserts++; assert_what = what; assert_value = value; tr("!"); }

#define TRACE(s) CHECK(strcmp(trace, s) == 0)

static void reset(void)
{
    memset(&tws_sniff, 0, sizeof tws_sniff); memset(&tws_conn, 0, sizeof tws_conn);
    memset(&tws_link, 0, sizeof tws_link); memset(&lcon, 0, sizeof lcon); memset(&lmp, 0, sizeof lmp);
    memset(&lsniff, 0, sizeof lsniff); memset(&alloc_sniff, 0, sizeof alloc_sniff);
    trace[0] = 0; asserts = 0; n_ctrl = 0; sent_n = 0; g_timer_get = 0x21; g_call1 = 4;
    tws_link.link = &lcon; tws_conn.lmp_conn = &lmp; lmp.sniff = &lsniff;
    log_tag_const_i_TWS = log_tag_const_d_TWS = 0; timer_i = 0xee; handler = 0; priv = (void *)1;
}
static void fire(void) { void (*h)(void *) = handler; void *p = priv; trace[0] = 0; h(p); }

int main(void)
{
    u8 rx[9];

    /* Empty entry points. */
    reset(); tws_tx_sniff_req(); tws_tx_unsniff_req(); tws_sniff_without_phone(); tws_unsniff_without_phone(); TRACE("");

    /* First anchor point: wake_instant + 20 + offset - local; below 10 the sniff interval is added. */
    reset(); lsniff.wake_instant = 1000; lsniff.wakeup_offset = -3; lsniff.T_sniff = 100; g_local = 985; tws_sniff.timer = 0x22;
    tws_sniff_fap_set(&lmp);
    TRACE("k<e>"); CHECK(slots == 32 && ext_slots == 985 && timer_i == 0x22 && priv == &lmp);
    CHECK(handler == tws_sniff_first_anchor_point && tws_sniff.wake_instant == 1017 && tws_sniff.wakeup_offset == 0);
    reset(); lsniff.wake_instant = 1000; lsniff.wakeup_offset = -3; lsniff.T_sniff = 100; g_local = 1030; log_tag_const_d_TWS = 1;
    tws_sniff_fap_set(&lmp);
    TRACE("Lk<eL>"); CHECK(slots == 87 && ext_slots == 1030 && tws_sniff.wake_instant == 1117);
    reset(); lsniff.wake_instant = 1000; lsniff.T_sniff = 0; g_local = 1030;     /* -10 stays, assert, carries on */
    tws_sniff_fap_set(&lmp);
    TRACE("k<!e>"); CHECK(asserts == 1 && assert_value == -10 && slots == -10);
    reset(); tws_sniff_fap_set(0); TRACE("k<>"); lmp.sniff = 0; trace[0] = 0; tws_sniff_fap_set(&lmp); TRACE("k<>");

    /* Anchor point sequence callbacks. */
    reset(); tws_sniff.timer = 0x23; tws_sniff.wakeup_offset = -3;
    tws_sniff_first_anchor_point(&lmp);
    TRACE("cDrcRs"); CHECK(ctrl_cmds[0] == SNIFF_CTRL_ENTER && ctrl_cmds[1] == SNIFF_CTRL_FRAME_ENABLE);
    CHECK(lcon.core.max_slot == 1 && (tws_sniff.state & 1) && tws_sniff.sync && slots == 5 && priv == &lmp);
    CHECK(handler == tws_sniff_anchor_point_timeout && timer_i == 0x23 && task_run_set_v == 0 && run_task == &lcon.task);
    reset(); tws_sniff.state = 1;                                                  /* already in sniff: assert, continues */
    tws_sniff_first_anchor_point(&lmp); TRACE("!cDrcRs"); CHECK(asserts == 1);
    reset(); tws_sniff.state = 16; tws_sniff.wakeup_offset = -3; tws_sniff_pre_anchor_point(&lmp);
    CHECK(slots == 25 && tws_sniff.sync == 1 && tws_sniff.enable == 0);
    fire(); TRACE("R"); CHECK(task_run_arg_null);
    reset(); tws_sniff.timer = 0x24; tws_sniff_wakeup(&lmp);
    TRACE("cs"); CHECK(ctrl_cmds[0] == SNIFF_CTRL_FRAME_DISABLE && handler == tws_sniff_pre_anchor_point && slots == 2 && priv == &lmp && timer_i == 0x24);

    /* ap_set, first branch: wake 1000 offset -3, local 980, call1 4 -> 33 slots. */
    reset(); lsniff.wake_instant = 1000; lsniff.wakeup_offset = -3; lsniff.T_sniff = 100; g_local = 980; tws_sniff.timer = 0x25; tws_sniff.unsniff_retry = 7;
    lcon.sniff_rx_flag = 1;
    tws_sniff_ap_set(&lmp);
    TRACE("kcceP"); CHECK(ctrl_cmds[0] == SNIFF_CTRL_FRAME_DISABLE && ctrl_cmds[1] == SNIFF_CTRL_INTERVAL_OFFSET);
    CHECK(slots == 33 && ext_slots == 980 && handler == tws_sniff_wakeup && priv == &lmp && timer_i == 0x25 && tws_sniff.wakeup_offset == 4);
    CHECK(tws_sniff.wake_instant == 1013 && tws_sniff.unsniff_retry == 0 && period == 988);
    /* role set: the pre anchor point handler; sniff state bits 3/4 suppress the period. */
    reset(); lsniff.wake_instant = 1000; lsniff.wakeup_offset = -3; lsniff.T_sniff = 100; g_local = 980; lcon.role = 1; lsniff.state = 8;
    tws_sniff_ap_set(&lmp);                      /* sniff_rx_flag clear: same first branch */
    TRACE("kcce"); CHECK(handler == tws_sniff_pre_anchor_point && slots == 33);
    /* wrap of the 27-bit slot clock and a short distance (below 10: add T_sniff). */
    reset(); lsniff.wake_instant = 5; lsniff.wakeup_offset = -3; lsniff.T_sniff = 100; g_local = 134217700;
    tws_sniff_ap_set(&lmp);
    CHECK(slots == 46 && tws_sniff.wake_instant == 18 && ext_slots == 134217700 && period == 5 - 12);
    reset(); lsniff.wake_instant = 1000; lsniff.wakeup_offset = -12; lsniff.T_sniff = 100; g_local = 996;   /* 20-4-996-12+1000 = 8 */
    tws_sniff_ap_set(&lmp);
    CHECK(slots == 108 && tws_sniff.wake_instant == 1104 && period == 1000 - 12);
    /* wake instant already behind the local clock: it is taken as wrapped by 2^27 */
    reset(); lsniff.wake_instant = 1000; lsniff.T_sniff = 100; g_local = 1010;
    tws_sniff_ap_set(&lmp);
    CHECK(slots == 134217734 && tws_sniff.wake_instant == 1016);
    /* retry branch: sniff_rx_flag set, retry <= 6; uses tws_sniff's own wake instant, 28 - call1; conn may be null. */
    reset(); lcon.sniff_rx_flag = 1; tws_sniff.unsniff_retry = 2; tws_sniff.wake_instant = 1000; tws_sniff.wakeup_offset = -3; g_local = 980; lcon.role = 1;
    tws_sniff_ap_set(0);
    TRACE("kcce"); CHECK(tws_sniff.unsniff_retry == 3 && slots == 41 && tws_sniff.wake_instant == 1021 && priv == 0 && handler == tws_sniff_pre_anchor_point);
    reset(); lcon.sniff_rx_flag = 1; tws_sniff.unsniff_retry = 6; tws_sniff.wake_instant = 5; tws_sniff.wakeup_offset = 0; g_local = 134217700;
    tws_sniff_ap_set(&lmp);                      /* 28-4-134217700+5+134217728 = 57 */
    TRACE("kcceP"); CHECK(tws_sniff.unsniff_retry == 7 && slots == 57 && tws_sniff.wake_instant == 29);
    reset(); lcon.sniff_rx_flag = 1; tws_sniff.unsniff_retry = 7; g_local = 980;   /* above 6: first branch, no connection: unsniff */
    log_tag_const_i_TWS = 1;
    tws_sniff_ap_set(0);
    TRACE("kccL<>"); CHECK(tws_sniff.unsniff_retry == 0);

    /* ap_exit: only with enable, sync and the sniff-entered state bit. */
    reset(); tws_sniff.enable = 1; tws_sniff.sync = 1; tws_sniff.state = 1; g_local = 980; lsniff.wake_instant = 1000; lsniff.T_sniff = 100;
    tws_sniff_ap_exit();
    TRACE("<kcceP>"); CHECK(tws_sniff.sync == 0 && tws_sniff.enable == 1);
    reset(); tws_sniff.enable = 1; tws_sniff.sync = 1; tws_sniff.state = 1; tws_sniff.witchout_phone = 1; tws_sniff.T_sniff = 100; tws_sniff.D_sniff = 50;
    g_master = 1250; g_local = 700; lcon.role = 1;
    tws_sniff_ap_exit();
    TRACE("<kcceP>"); CHECK(slots == 96 && ext_slots == 700 && tws_sniff.wake_instant == 796 && handler == tws_sniff_without_phone_pre_ap && period == 784);
    CHECK(tws_sniff.sync == 0 && tws_sniff.wakeup_offset == 4);
    reset(); tws_sniff.enable = 1; tws_sniff.sync = 0; tws_sniff.state = 1; tws_sniff_ap_exit(); TRACE("<>");
    reset(); tws_sniff.enable = 1; tws_sniff.sync = 1; tws_sniff.state = 0; tws_sniff_ap_exit(); TRACE("<>");
    reset(); tws_sniff.enable = 0; tws_sniff.sync = 1; tws_sniff.state = 1; tws_sniff_ap_exit(); TRACE("<>");

    /* Without phone: D 30, T 100, master 1234 -> 96 before the call1 (4) correction. */
    reset(); tws_sniff.D_sniff = 30; tws_sniff.T_sniff = 100; g_master = 1234; g_local = 500; tws_sniff.timer = 0x26;
    tws_sniff_without_phone_ap_set();
    TRACE("kcceP"); CHECK(slots == 92 && handler == tws_sniff_without_phone_wakeup && tws_sniff.wake_instant == 592 && tws_sniff.wakeup_offset == 4 && period == 580);
    reset(); tws_sniff.D_sniff = 50; tws_sniff.T_sniff = 100; g_master = 1250; g_local = 134217700;
    tws_sniff_without_phone_ap_set();
    CHECK(slots == 96 && ext_slots == 134217700 && tws_sniff.wake_instant == (134217796u & 134217727u));
    reset(); tws_sniff.D_sniff = 30; tws_sniff.T_sniff = 100; g_master = 1200; g_local = 500; g_call1 = 40;   /* sub 30, call1 > sub: 30 - 40 + 100 */
    tws_sniff_without_phone_ap_set();
    CHECK(slots == 90 && tws_sniff.wakeup_offset == 40);
    reset(); tws_sniff.D_sniff = 30; tws_sniff.T_sniff = 100; g_master = 1260; g_local = 500; g_call1 = 4;      /* sub -30 -> 70 */
    tws_sniff_without_phone_ap_set();
    CHECK(slots == 66);

    reset(); tws_sniff.D_sniff = 30; tws_sniff.T_sniff = 100; g_master = 1234; g_local = 500; tws_sniff.timer = 0x27; log_tag_const_d_TWS = 1;
    tws_sniff_without_phone_fap_set();
    TRACE("keuL"); CHECK(slots == 94 && ext_slots == 500 && tws_sniff.wake_instant == 594 && handler == tws_sniff_without_phone_fap && priv == 0 && timer_i == 0x27);
    reset(); tws_sniff.D_sniff = 0; tws_sniff.T_sniff = 100; g_master = 1260; g_local = 500;   /* add 40 < 50: + T */
    tws_sniff_without_phone_fap_set();
    CHECK(slots == 138 && asserts == 0);
    reset(); tws_sniff.D_sniff = 0; tws_sniff.T_sniff = 1; g_master = 77; g_local = 500;       /* add 1: -1 -> assert, carries on */
    tws_sniff_without_phone_fap_set();
    TRACE("k!eu"); CHECK(asserts == 1 && assert_value == 77 && slots == -1);
    reset(); tws_sniff.timer = 0x28; tws_sniff.wakeup_offset = 2; tws_sniff.state = 16;
    tws_sniff_without_phone_fap(0);
    TRACE("cDrcRsv"); CHECK(lcon.core.max_slot == 1 && (tws_sniff.state & 1) && slots == 16 && handler == tws_sniff_without_phone_ap_timeout);
    CHECK(ctrl_cmds[0] == SNIFF_CTRL_ENTER && ctrl_cmds[1] == SNIFF_CTRL_FRAME_ENABLE);
    reset(); tws_sniff.timer = 0x29; tws_sniff_without_phone_wakeup(0); TRACE("cs"); CHECK(handler == tws_sniff_without_phone_pre_ap && slots == 2 && timer_i == 0x29);
    fire(); TRACE("cRs"); CHECK(slots == 8);
    reset(); tws_sniff_without_phone_ap_timeout(0); TRACE("R");

    /* with_phone / unsniff_with_phone. */
    reset(); tws_link.link = 0; tws_sniff_with_phone(); TRACE("<>");
    reset(); lsniff.wake_instant = 0; tws_sniff_with_phone(); TRACE("<>"); CHECK(!tws_sniff.enable);
    reset(); tws_sniff.enable = 1; lsniff.wake_instant = 7; tws_sniff_with_phone(); TRACE("<>");
    reset(); lsniff.wake_instant = 1000; lsniff.T_sniff = 100; g_local = 985; g_timer_get = 0x31; tws_sniff.state = 9; tws_sniff.sync = 1; tws_sniff.witchout_phone = 1; tws_sniff.unsniff_retry = 3;
    log_tag_const_i_TWS = 1;
    tws_sniff_with_phone();
    TRACE("<Lgk<e>>"); CHECK(tws_sniff.enable == 1 && !tws_sniff.sync && !tws_sniff.witchout_phone && tws_sniff.state == 0 && tws_sniff.unsniff_retry == 0);
    CHECK(tws_sniff.timer == 0x31 && handler == tws_sniff_first_anchor_point);
    reset(); tws_conn.lmp_conn = &lmp; lmp.sniff = 0; g_timer_get = 0x32; g_local = 1; tws_sniff_with_phone();   /* no sniff block: nothing to check, no wake_instant */
    CHECK(tws_sniff.enable == 1 && tws_sniff.timer == 0x32);

    reset(); tws_sniff.enable = 1; tws_sniff.state = 17; tws_sniff.sync = 1; tws_sniff.unsniff_retry = 2; tws_sniff.wakeup_offset = 4; tws_sniff.wake_instant = 9; tws_sniff.timer = 0x33;
    tws_unsniff_with_phone();
    TRACE("d<prcn>"); CHECK(!tws_sniff.enable && !tws_sniff.sync && tws_sniff.state == 0 && tws_sniff.wake_instant == 0 && put_timer == 0x33 && run_slots == 20 && ctrl_cmds[0] == SNIFF_CTRL_EXIT);
    reset(); tws_unsniff_with_phone(); TRACE("<>");
    reset(); tws_sniff.enable = 1; lcon.role = 1; g_timer_get = 0x34; tws_unsniff_with_phone();
    TRACE("d<prcngs>"); CHECK(tws_sniff.reset_timer == 0x34 && handler == tws_reset_2_slot && slots == 16 && timer_i == 0x34 && priv == 0);
    reset(); tws_sniff.enable = 1; lcon.role = 1; g_timer_get = 0; tws_unsniff_with_phone();
    TRACE("d<prcng!s>"); CHECK(asserts == 1 && slots == 16 && timer_i == 0);
    reset(); tws_sniff.enable = 1; lcon.role = 1; tws_sniff.reset_timer = 0x35; tws_unsniff_with_phone();
    CHECK(timer_i == 0x35 && asserts == 0);
    fire(); TRACE("<p>i"); CHECK(tws_sniff.reset_timer == 0 && put_timer == 0x35 && idle_run == 2);

    /* detach */
    reset(); tws_sniff.enable = 1; tws_sniff.state = 3; lmp.core.operation_mode_sync = 2; lcon.core.max_slot = 1; tws_sniff.timer = 0x36;
    tws_sniff_detach();
    TRACE("d<prc>"); CHECK(!tws_sniff.enable && lmp.core.operation_mode_sync == 0 && lcon.core.max_slot == 5 && put_timer == 0x36);
    reset(); tws_conn.lmp_conn = 0; tws_link.link = 0; tws_sniff_detach(); TRACE("<>");

    /* sync: transmit operation mode. */
    reset(); tws_conn.monitor_start = 2; lmp.core.operation_mode = 2; lmp.core.operation_mode_sync = 0;
    lsniff.timing_control_flags = 3; lsniff.D_sniff = 0x102; lsniff.T_sniff = 0x204; lsniff.attempt = 5; lsniff.timeout = 6;
    tws_conn_sync_tx_operation_mode();
    TRACE("T"); CHECK(sent_op == 15 && sent_fmt_len == 6 && sent_args[0] == 0x55 && sent_args[1] == 3 && sent_args[2] == 0x102 && sent_args[3] == 0x204 && sent_args[4] == 5 && sent_args[5] == 6);
    CHECK(lmp.core.operation_mode_sync == 2);
    trace[0] = 0; sent_n = 0; tws_conn_sync_tx_operation_mode(); TRACE(""); CHECK(sent_n == 0);
    lmp.core.operation_mode = 0; tws_conn_sync_tx_operation_mode();
    TRACE("T"); CHECK(sent_op == 17 && sent_fmt_len == 1 && sent_args[0] == 0x55 && lmp.core.operation_mode_sync == 0 && (tws_sniff.state & 16));
    reset(); tws_conn.monitor_start = 2; lmp.core.operation_mode = 2; lmp.sniff = 0; tws_conn_sync_tx_operation_mode(); TRACE(""); CHECK(lmp.core.operation_mode_sync == 0);
    reset(); tws_conn.monitor_start = 1; lmp.core.operation_mode = 2; tws_conn_sync_tx_operation_mode(); TRACE("");
    reset(); tws_conn.monitor_start = 2; tws_conn.role = 1; lmp.core.operation_mode = 2; tws_conn_sync_tx_operation_mode(); TRACE("");
    reset(); tws_conn.lmp_conn = 0; tws_conn.monitor_start = 2; tws_conn_sync_tx_operation_mode(); TRACE("");
    reset(); tws_conn.monitor_start = 2; lmp.core.operation_mode = 2; tws_sniff_ap_enter(); TRACE("T");

    /* LMP side: sniff sync, request parsing, unsniff. */
    reset(); tws_conn.lmp_conn = &lmp; lmp.core.operation_mode = 0; log_tag_const_i_TWS = 1;
    {
        struct lmp_sniff parm = { .timing_control_flags = 1, .D_sniff = 0x1122, .T_sniff = 0x3344, .attempt = 0x5566, .timeout = 0x7788 };
        tws_lmp_rx_sniff_sync(&lmp, &parm);
    }
    TRACE("ALZM"); CHECK(alloc_sniff.timing_control_flags == 1 && alloc_sniff.D_sniff == 0x1122 && alloc_sniff.T_sniff == 0x3344 && alloc_sniff.attempt == 0x5566 && alloc_sniff.timeout == 0x7788);
    CHECK(lmp.core.operation_mode == 2 && mode_ev_t == 0x3344);
    reset(); { u8 b[9] = { 9, 1, 2, 3, 4, 5, 6, 7, 8 }; memcpy(rx, b, 9); }
    tws_lmp_rx_sniff_req(&lmp, rx); TRACE("AZM");
    CHECK(alloc_sniff.timing_control_flags == 9 && alloc_sniff.D_sniff == 0x0201 && alloc_sniff.T_sniff == 0x0403 && alloc_sniff.attempt == 0x0605 && alloc_sniff.timeout == 0x0807);
    reset(); lmp.core.operation_mode = 2; tws_lmp_rx_sniff_req(&lmp, rx); TRACE("");
    reset(); tws_lmp_rx_unsniff_req(&lmp); TRACE("MF"); CHECK(lmp.core.operation_mode == 0 && mode_ev_t == 0);
    reset(); lmp.sniff = 0; lmp.core.operation_mode = 2; tws_lmp_rx_unsniff_req(&lmp); TRACE(""); CHECK(lmp.core.operation_mode == 2);

    /* Peer messages: only flag 0x55 acts. */
    reset(); { u8 m[10] = { 0x55, 7, 1, 2, 3, 4, 5, 6, 7, 8 };
        tws_rx_sniff_req(m);
        TRACE("TAZM<>"); CHECK(sent_op == 2 && sent_fmt_len == 2 && sent_args[0] == 15 && sent_args[1] == 0x55); }
    reset(); { u8 m[10] = { 0x54, 0 }; tws_rx_sniff_req(m); TRACE("T"); CHECK(sent_args[1] == 0x54); }
    reset(); { u8 m = 0x55; log_tag_const_i_TWS = 1; tws_sniff.enable = 1; tws_rx_unsniff_req(&m);
        CHECK(sent_op == 2 && sent_args[0] == 17 && sent_args[1] == 0x55); }
    reset(); { u8 m = 0x00; tws_rx_unsniff_req(&m); TRACE("Tn"); CHECK(run_slots == 20 && run_task == &lcon.task && sent_args[1] == 0); }
    reset(); { u8 m = 0x55; tws_rx_unsniff_accept(&m); TRACE("<>"); m = 0x56; trace[0] = 0; tws_rx_unsniff_accept(&m); TRACE(""); }
    reset(); { u8 m = 0x55; lsniff.wake_instant = 5; lsniff.T_sniff = 100; g_local = 0; tws_rx_sniff_accept(&m); TRACE("<gk<e>>"); }
    reset(); { u8 m = 0x56; tws_rx_sniff_accept(&m); TRACE(""); }

    return TEST_DONE("btctrler_tws_sniff");
}
#endif
