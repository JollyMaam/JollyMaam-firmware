/* BD19 classic TWS slot scheduler, recovered from tws_schedule.c.o. */
#include "btctrler/glue.h"

#define TWS_SCH_CODE __attribute__((section(".tws_irq_code")))

/* These configuration values are external constants in the SDK object. */
extern const int CONFIG_PHONE_RUN_SLOT;
extern const int CONFIG_TWS_RUN_SLOT;

static u8 sch_timer;
static u8 seqn_err_cnt;

static int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static int tws_master_try_switch_tlink(void *mode) TWS_SCH_CODE;
static void tws_sch_slave_plink_monitor_enable(void *unused) TWS_SCH_CODE;

void tws_sch_tlink_connection(struct link_connection *link, int role) TWS_SCH_CODE;
void tws_sch_tws_link_disconnection(void) TWS_SCH_CODE;
void tws_sch_plink_connection_start(struct link_connection *plink) TWS_SCH_CODE;
void tws_sch_plink_disconnection(void) TWS_SCH_CODE;
void tws_sch_monitor_plink_init(struct link_connection *tlink,
                                struct link_connection *plink) TWS_SCH_CODE;
void tws_sch_rx_irq_handler(struct link_connection *link,
                            struct bredr_payload_head *head) TWS_SCH_CODE;

void tws_sch_tlink_connection(struct link_connection *link, int role)
{
    (void)link;
    (void)role;
    sch_timer = slot_timer_get();
}

void tws_sch_tws_link_disconnection(void)
{
    slot_timer_put(sch_timer);
}

void tws_sch_plink_connection_start(struct link_connection *plink)
{
    (void)plink;
}

void tws_sch_plink_disconnection(void)
{
    slot_timer_disable(sch_timer);
}

void tws_sch_monitor_plink_init(struct link_connection *tlink,
                                struct link_connection *plink)
{
    (void)tlink;
    (void)plink;
}

void tws_sch_rx_irq_handler(struct link_connection *link,
                            struct bredr_payload_head *head)
{
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    struct link_connection *tlink = tws_link.link;

    if (!tlink || tws_conn.monitor_start != 2 || !lmp->link ||
        lmp->esco_conn || (tws_sniff.enable && tws_sniff.sync &&
                           tws_sniff.witchout_phone) || link->state != 2)
        return;

    if (link->tws_link) {
        slot_timer_disable(sch_timer);
        if (link->role) {
            seqn_err_cnt = 0;
            link_task_try_run_slots(&tws_link.link->task, 16);
        }
        return;
    }

    if (link->monitor) {
        if ((1 << head->index) & link->rx_pend)
            return;

        switch (head->check) {
        case 127:
            seqn_err_cnt = 0;
            if (head->type > 1)
                link_task_try_run_slots(&link->task, 12);
            break;
        case 255:
            if (head->type > 9) {
                ++seqn_err_cnt;
                if (seqn_err_cnt > 4) {
                    seqn_err_cnt = 0;
                    link_task_run_slots(&tlink->task, CONFIG_TWS_RUN_SLOT);
                }
            } else {
                seqn_err_cnt = 0;
            }
            break;
        default:
            seqn_err_cnt = 0;
            break;
        }
        slot_timer_set(sch_timer, tws_sch_slave_plink_monitor_enable, 0, 10);
        return;
    }

    slot_timer_disable(sch_timer);
    if (head->type > 1) {
        if (head->check == 127) {
            if (tws_master_try_switch_tlink((void *)1))
                return;
        } else if (head->check == 255) {
            if (tws_master_try_switch_tlink((void *)2))
                return;
        }
    }

    {
        u32 resume_clkn = tws_link.plink->resume_clkn;
        u32 now = READ_SLOT_CLK();
        if (bredr_clkn2offset(resume_clkn, now) > CONFIG_PHONE_RUN_SLOT - 20) {
            link_task_run_slots(&tws_link.link->task, CONFIG_TWS_RUN_SLOT);
        } else {
            link_task_try_run_slots(&link->task, 16);
            slot_timer_set(sch_timer,
                           (void (*)(void *))tws_master_try_switch_tlink,
                           0, 10);
        }
    }
}

static void tws_sch_slave_plink_monitor_enable(void *unused)
{
    (void)unused;
    tws_slave_plink_monitor_enable(0);
}

static int tws_master_try_switch_tlink(void *mode)
{
    struct link_connection *plink = tws_link.plink;
    struct link_connection *tlink;
    struct lmp_connection *lmp;
    struct list_head *node;
    unsigned count = 0;
    int remain;

    if (!plink || plink->state != 2)
        return 0;
    tlink = tws_link.link;
    if (tlink->role)
        return 0;
    lmp = tws_conn.lmp_conn;

    if (lmp->clear_a2dp_packet_num != 0) {
        u32 resume_clkn = plink->resume_clkn;
        u32 now = READ_SLOT_CLK();
        if (bredr_clkn2offset(resume_clkn, now) <= 31)
            return 0;
        tlink = tws_link.link;
        link_task_run_slots(&tlink->task, CONFIG_TWS_RUN_SLOT);
        return 1;
    }

    if (tws_conn.tws_wait_tx) {
        tws_conn.tws_wait_tx = 0;
        link_task_run_slots(&tlink->task, CONFIG_TWS_RUN_SLOT);
        return 1;
    }
    if (!list_empty(&tlink->tx_acl_u))
        goto run_tlink;
    for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending;
         node = node->next)
        ++count;

    if ((mode == 0 && count < 2) || (mode == (void *)1 && count < 3) ||
        (mode == (void *)2 && count < 5)) {
        if (!lmp->sbc_open)
            return 0;
        remain = lmp_private_get_sbc_remain_time(lmp, 0);
        if (remain >= 11 && !(count > 1 && remain < 41))
            return 0;
        tlink = tws_link.link;
        link_task_run_slots(&tlink->task, CONFIG_TWS_RUN_SLOT);
        return 1;
    }

run_tlink:
    link_task_run_slots(&tlink->task, CONFIG_TWS_RUN_SLOT);
    return 1;
}
