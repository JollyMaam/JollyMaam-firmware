/* BD19 classic TWS slot scheduler, recovered from tws_schedule.c.o. */
#include "btctrler/glue.h"

#define TWS_SCH_CODE __attribute__((section(".tws_irq_code")))

/* These configuration values are external constants in the SDK object. */
extern BTCTRLER_CONFIG int CONFIG_PHONE_RUN_SLOT;
extern BTCTRLER_CONFIG int CONFIG_TWS_RUN_SLOT;

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
        lmp->esco_conn || tws_sniff.enable || link->state != 2)
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
        if ((1u << head->index) & link->rx_pend)
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
        void *mode = (head->check == 127 || head->check == 255) ? (void *)2 : (void *)1;
        if (tws_master_try_switch_tlink(mode))
            return;
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
    int count = 0;
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
    node = tws_conn.rx_pending.next;
    if (node == &tws_conn.rx_pending)
        return 0;
    do {
        ++count;
        node = node->next;
    } while (node != &tws_conn.rx_pending);

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

#ifdef UNITTEST
#include "test.h"
#include <string.h>
struct tws_connection tws_conn;
struct tws_link_connection tws_link;
struct lmp_tws_sniff tws_sniff;
int CONFIG_PHONE_RUN_SLOT=200, CONFIG_TWS_RUN_SLOT=12;
static struct lmp_connection lmp;
static struct link_connection phone, peer, input, replacement;
static struct bredr_payload_head payload;
static struct list_head nodes[6];
static int gets, puts_count, disables, sets, runs, tries, sbc_calls, clock_calls;
static int offset_value, remaining, last_slots, last_try, slave_calls;
static int mutate_disable, mutate_clock, mutate_sbc, mutate_try;
static int offset_start, offset_end;
static u8 timer_value, last_timer;
static struct link_task *last_task;
static void (*timer_callback)(void *);
u8 slot_timer_get(void) { gets++; return timer_value; }
void slot_timer_put(u8 id) { puts_count++; last_timer=id; }
void slot_timer_disable(u8 id)
{
    disables++; last_timer=id;
    if (mutate_disable) { input.role=1; tws_link.link=&replacement; }
}
void slot_timer_set(u8 id,void(*cb)(void*),void *priv,int slots)
{ sets++; last_timer=id; timer_callback=cb; CHECK(priv==NULL && slots==10); }
u32 READ_SLOT_CLK(void)
{
    clock_calls++;
    if (mutate_clock) { phone.resume_clkn=999; tws_link.link=&replacement; }
    return 123;
}
int bredr_clkn2offset(int start,int end)
{ offset_start=start; offset_end=end; return offset_value; }
void link_task_run_slots(struct link_task *task,int slots)
{ runs++; last_task=task; last_slots=slots; }
void link_task_try_run_slots(struct link_task *task,int slots)
{ tries++; last_task=task; last_try=slots; if(mutate_try) sch_timer=42; }
int lmp_private_get_sbc_remain_time(void *priv,u8 flag)
{
    CHECK(priv==&lmp && flag==0); sbc_calls++;
    if(mutate_sbc) tws_link.link=&replacement;
    return remaining;
}
void tws_slave_plink_monitor_enable(void *priv) { CHECK(priv==NULL); slave_calls++; }
static void setup(unsigned count)
{
    memset(&lmp,0,sizeof(lmp)); memset(&phone,0,sizeof(phone));
    memset(&peer,0,sizeof(peer)); memset(&input,0,sizeof(input));
    memset(&replacement,0,sizeof(replacement)); memset(&tws_conn,0,sizeof(tws_conn));
    memset(&tws_link,0,sizeof(tws_link)); memset(&tws_sniff,0,sizeof(tws_sniff));
    memset(&payload,0,sizeof(payload));
    tws_conn.lmp_conn=&lmp; lmp.link=&phone;
    tws_link.link=&peer; tws_link.plink=&phone; tws_conn.monitor_start=2;
    phone.state=input.state=2;
    peer.tx_acl_u.next=peer.tx_acl_u.prev=&peer.tx_acl_u;
    struct list_head *head=&tws_conn.rx_pending;
    head->next=count ? &nodes[0] : head; head->prev=count ? &nodes[count-1] : head;
    for(unsigned i=0;i<count;i++) {
        nodes[i].next=i+1==count ? head : &nodes[i+1];
        nodes[i].prev=i==0 ? head : &nodes[i-1];
    }
    runs=tries=sets=disables=sbc_calls=clock_calls=0;
    mutate_disable=mutate_clock=mutate_sbc=mutate_try=0;
    offset_value=remaining=50; seqn_err_cnt=0;
}
int main(void)
{
    setup(0);
    tws_sch_tws_link_disconnection(); CHECK(last_timer==0);
    timer_value=255; tws_sch_tlink_connection(NULL,123);
    CHECK(gets==1 && sch_timer==255);
    tws_sch_tws_link_disconnection(); tws_sch_plink_disconnection();
    CHECK(puts_count==2 && sch_timer==255 && last_timer==255);
    tws_sch_plink_connection_start(NULL); tws_sch_monitor_plink_init(NULL,NULL);
    CHECK(sch_timer==255);
    /* Modes 0/1/2 have different queue gates; empty always returns zero. */
    const int times[]={-1,10,11,40,41};
    for(unsigned mode=0;mode<4;mode++) for(unsigned count=0;count<=6;count++)
    for(unsigned opened=0;opened<2;opened++) for(unsigned t=0;t<5;t++) {
        setup(count); lmp.sbc_open=opened; remaining=times[t];
        int deferred=(mode==0 && count<2)||(mode==1 && count<3)||(mode==2 && count<5);
        int expected=count!=0 && (!deferred || (opened && (remaining<11 || (count>1 && remaining<41))));
        CHECK(tws_master_try_switch_tlink((void *)(uintptr_t)mode)==expected);
        CHECK(runs==expected && sbc_calls==(count!=0 && deferred && opened));
    }
    setup(0); lmp.clear_a2dp_packet_num=1; phone.resume_clkn=77;
    offset_value=31; CHECK(tws_master_try_switch_tlink(NULL)==0);
    offset_value=32; mutate_clock=1; CHECK(tws_master_try_switch_tlink(NULL)==1);
    CHECK(last_task==&replacement.task && offset_start==77 && offset_end==123);
    setup(0); tws_conn.tws_wait_tx=1;
    CHECK(tws_master_try_switch_tlink(NULL)==1 && tws_conn.tws_wait_tx==0);
    setup(0); peer.tx_acl_u.next=&nodes[0];
    CHECK(tws_master_try_switch_tlink(NULL)==1 && last_task==&peer.task);
    setup(1); lmp.sbc_open=1; remaining=10; mutate_sbc=1;
    CHECK(tws_master_try_switch_tlink(NULL)==1 && last_task==&replacement.task);
    /* Sniff enable alone blocks; the other two bits do not gate this path. */
    setup(0); tws_sniff.enable=1; tws_sch_rx_irq_handler(&input,&payload);
    CHECK(disables==0 && sets==0);
    tws_sniff.enable=0; tws_sniff.sync=1; tws_sniff.witchout_phone=1;
    offset_value=180; mutate_try=1; tws_sch_rx_irq_handler(&input,&payload);
    CHECK(tries==1 && last_try==16 && sets==1 && last_timer==42);
    CHECK(timer_callback==(void(*)(void*))tws_master_try_switch_tlink);
    setup(0); offset_value=181; phone.resume_clkn=71; mutate_clock=1;
    tws_sch_rx_irq_handler(&input,&payload);
    CHECK(runs==1 && last_task==&replacement.task && offset_start==71);
    /* Checks 127 and 255 choose mode2; any other check selects mode1. */
    for(unsigned k=0;k<3;k++) {
        setup(3); payload.type=2; payload.check=k==0 ? 127 : k==1 ? 255 : 9;
        offset_value=0; tws_sch_rx_irq_handler(&input,&payload);
        CHECK(runs==(k==2) && tries==(k!=2));
    }
    setup(0); input.monitor=1; payload.type=10; payload.check=255;
    for(unsigned i=0;i<4;i++) tws_sch_rx_irq_handler(&input,&payload);
    CHECK(runs==0 && seqn_err_cnt==4);
    tws_sch_rx_irq_handler(&input,&payload);
    CHECK(runs==1 && seqn_err_cnt==0 && sets==5 && last_task==&peer.task);
    CHECK(timer_callback==tws_sch_slave_plink_monitor_enable);
    timer_callback((void*)1); CHECK(slave_calls==1);
    payload.check=127; payload.type=2; tws_sch_rx_irq_handler(&input,&payload);
    CHECK(last_try==12 && seqn_err_cnt==0);
    input.rx_pend=1; int before=sets;
    tws_sch_rx_irq_handler(&input,&payload); CHECK(sets==before);
    setup(0); input.tws_link=1; mutate_disable=1;
    tws_sch_rx_irq_handler(&input,&payload);
    CHECK(tries==1 && last_task==&replacement.task && last_try==16);
    return TEST_DONE("btctrler_bd19_tws_schedule");
}
#endif
