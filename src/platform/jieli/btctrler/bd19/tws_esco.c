/* BD19 tws_esco.c.o occurrence 1; original SDK quirks retained. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_ESCO;
void log_print(int level, const char *tag, const char *format, ...);

#define TWS_ESCO_CODE __attribute__((section(".classic_tws_code.esco")))

/* BD19 IR lines 161–187. Keep the separate 1/2 request cases and state
 * guard: this is not equivalent to treating request as a boolean. */
void tws_esco_tx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t) TWS_ESCO_CODE;
void tws_esco_tx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t)
{
    if (!esco)
        return;
    if (request == 1) {
        if (esco->esco_role_switch_state != 2) {
            esco->esco_acl_resume_t = acl_resume_t;
        }
        esco->esco_role_switch_state = 2;
    } else if (request == 2) {
        if (esco->esco_role_switch_state != 4) {
            esco->esco_acl_resume_t = acl_resume_t;
        }
        esco->esco_role_switch_state = 4;
    }
}

/* BD19 IR lines 191–212. The strict >6 test and byte subtraction are SDK
 * behavior, including wrap for a zero request value in the accepted path. */
void tws_esco_rx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t) TWS_ESCO_CODE;
void tws_esco_rx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t)
{
    if (!esco)
        return;
    if (request == 1) {
        if (acl_resume_t > 6 && esco->esco_role_switch_state != 3) {
            esco->esco_role_switch_state = 3;
            esco->esco_acl_resume_t = (u8)(acl_resume_t - 1);
            if (log_tag_const_i_TWS_ESCO)
                log_print(2, NULL,
                          "[Info]: [TWS_ESCO]slave_role_switch_resume_acl\n\r\n");
        }
    } else if (request == 2 && esco->esco_role_switch_state != 5) {
        esco->esco_role_switch_state = 5;
        esco->esco_acl_resume_t = (u8)(acl_resume_t - 1);
        if (log_tag_const_i_TWS_ESCO)
            log_print(2, NULL,
                      "[Info]: [TWS_ESCO]tws_esco_resume_acl\n\r\n");
    }
}

/* Weak SDK default, BD19 IR line 216. */
u8 get_esco_packet_dump(void) __attribute__((weak, section(".classic_tws_code.esco")));
u8 get_esco_packet_dump(void)
{
    return 120;
}

#ifdef UNITTEST
static int esco_test_putchar(int);
#define putchar esco_test_putchar
#endif
#define CODE TWS_ESCO_CODE
#define FILE_NAME "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/tws_esco.c"
#define INFO(...) do { if (log_tag_const_i_TWS_ESCO) log_print(2, 0, __VA_ARGS__); } while (0)
#define ASSERT_AT(value, text, line) do { if (!(value)) { if (config_asser) { printf("file:%s, line:%d", FILE_NAME, line); printf("ASSERT-FAILD: " text " "); cpu_assert_debug(); } else cpu_reset(); } } while (0)
#ifdef UNITTEST
static volatile u32 test_regs[64];
#define REG32(a) test_regs[((a) & 255) / 4]
#else
#define REG32(a) (*(volatile u32 *)(a))
_Static_assert(sizeof(struct bredr_esco_link) == 116, "SDK relay allocation");
_Static_assert(offsetof(struct link_connection, esco) == 236, "esco ABI");
_Static_assert(offsetof(struct link_connection, frame) == 208, "frame ABI");
#endif
static void cpu_reset(void) { p33_soft_reset(); }
static void INIT_LIST_HEAD(struct list_head *p) { p->next=p; p->prev=p; }
static void __list_del_entry(struct list_head *p) { struct list_head *prev=p->prev,*next=p->next; next->prev=prev; prev->next=next; }
static u8 bredr_hw_read_lt_addr(struct ctrl_frame *p) { return (p->miscntl >> 4) & 7; }
#define RX_ENTRY(p) ((struct bredr_rx_bulk *)((char *)(p)-offsetof(struct bredr_rx_bulk,entry)))
static void CODE tws_get_time_offset(struct link_connection *lc);
static void CODE bredr_tws_esco_get_data(void *arg);

int CODE get_tws_master_clk(struct link_connection *lc)
{
    u32 local, offset; u16 bitoff;
    preserve_bitoff_cal(lc->frame,lc->frame->bitoff,&offset,&bitoff,600);
    return get_mclkn(offset,bitoff,&local);
}
u8 CODE tws_esco_acl_communication(struct link_connection *lc,u8 *status)
{
    struct bredr_esco_link *esco=lc->esco;
    u32 clk=get_tws_master_clk(lc), t=esco->pdu.T_sco;
    struct link_connection *peer=tws_link.link;
    if (!esco || !peer) return 0;
    u32 period=100-100%t;
    if (!peer->esco) period=(100/(t*2))*t;
    ASSERT_AT(lc->esco,"lc->esco",82);
    s32 delta=(s32)(((esco->pdu.timing_flags&2)?clk^0x4000000:clk)%period)-esco->pdu.D_sco;
    if(delta<0) delta+=period;
    s32 threshold=(s32)period-2-esco->pdu.T_sco+esco->slot*2;
    peer=tws_link.link;
    if(peer && peer->esco) {
        if(status) { *status=peer->esco->rx_remove_esco_req_flag; peer=tws_link.link; }
        if(peer->esco->rx_remove_esco_req_flag) --peer->esco->rx_remove_esco_req_flag;
    }
    tws_link.acl_communication=0;
    switch(esco->esco_acl_resume_t) {
    case 16: tws_link.acl_communication=1; return 2;
    case 0:
        if((u32)delta >= (u32)threshold) {
            if(!peer || !peer->esco || (peer->esco->hw.reg->BREDR_ESCO_CON&1)) tws_link.acl_communication=1;
            return 2;
        }
        return peer && peer->esco && peer->esco->rx_remove_esco_req_flag ? 2 : 0;
    default:
        if((u32)delta >= (u32)threshold) tws_link.acl_communication=1;
        return 2;
    }
}
u8 CODE tws_esco_acl_check(struct link_connection *lc)
{
    struct bredr_esco_link *esco=lc->esco; u32 clk=get_tws_master_clk(lc);
    u32 t=esco->pdu.T_sco, period=100-100%t;
    if(!esco || !tws_link.link) return 0;
    ASSERT_AT(lc->esco,"lc->esco",138);
    s32 delta=(s32)(((esco->pdu.timing_flags&2)?clk^0x4000000:clk)%period)-esco->pdu.D_sco;
    if(delta<0) delta+=period;
    if((u32)delta < period-2-esco->pdu.T_sco+esco->slot*2)
        return tws_link.link->esco->rx_remove_esco_req_flag ? 2 : 0;
    return 2;
}
void CODE check_esco_rx_info(struct link_connection *lc)
{
    struct bredr_esco_link *esco=lc->esco; if(!esco) return;
    u32 clk=get_tws_master_clk(lc),t=esco->pdu.T_sco,period=600-600%t;
    s32 delta=(s32)(((esco->pdu.timing_flags&2)?clk^0x4000000:clk)%period)-esco->pdu.D_sco;
    if(delta<0) delta+=period;
    if((u32)delta >= period-2-t+esco->slot*2) {
        if(tws_conn.role==0) { INFO("[Info]: [TWS_ESCO]M=%d,%x\r\n",esco->rx_esco_num,clk); }
        else { INFO("[Info]: [TWS_ESCO]S=%d,%x\r\n",esco->rx_esco_num,clk); }
        esco->rx_esco_num=0;
    }
}
void CODE tws_esco_monitor_init(struct link_connection *lc)
{
    struct link_connection *peer=tws_link.link; if(!peer) return;
    if(!tws_conn.role) tws_esco_link_monitor_master_init(lc->frame,peer->frame);
    else tws_esco_link_monitor_slave_init(lc->frame,peer->frame);
}
static void CODE tws_get_time_offset(struct link_connection *lc)
{
    u32 local, offset; u16 bitoff;
    struct link_connection *peer=tws_link.link;
    struct bredr_esco_link *esco=peer->esco;
    if(!peer || !lc->esco) return;
    local_irq_disable();
    preserve_bitoff_cal(peer->frame,peer->frame->bitoff,&offset,&bitoff,600);
    u32 clk=get_mclkn(offset,bitoff,&local), t=esco->pdu.T_sco;
    s32 delta=esco->pdu.D_sco-(s32)(((esco->pdu.timing_flags&2)?clk^0x4000000:clk)%t);
    if(delta<=0) delta+=t;
    if(!(peer->esco->hw.reg->BREDR_ESCO_CON&1)) delta+=t;
    if(delta<2) delta+=t;
    if(esco->timer0!=255) {
        slot_timer_set_finecnt(esco->timer0,tws_esco_tick_timer_deal,lc,local+delta,bitoff);
        if(!(esco->hw.reg->BREDR_ESCO_CON&1)) {
            esco->esco_next_offset=(local+delta)>>1;
            esco->esco_next_soft_bitoff=(s16)(bitoff+600);
            if(esco->esco_next_soft_bitoff>1249) { esco->esco_next_soft_bitoff=bitoff-650; ++esco->esco_next_offset; }
        }
    }
    local_irq_enable();
}
u8 CODE get_tws_frame_flag(void *unused)
{
    (void)unused; return !tws_link.link || !tws_link.link->esco;
}
u8 CODE tws_dsco_cal(struct link_connection *lc,struct link_connection *peer,struct lmp_esco_pdu *pdu)
{
    u32 local, a, b; u16 ba,bb;
    preserve_bitoff_cal(lc->frame,lc->frame->bitoff,&a,&ba,600);
    u32 m=get_mclkn_to_local_clkn(a,ba,&local,1);
    if(pdu->timing_flags&2) m^=0x4000000;
    u32 t=pdu->T_sco;
    s32 d=pdu->D_sco-(s32)(m%t); if(d<=0) d+=t;
    if(t==6) d+=4;
    else if(t>11) d+=4+pdu->W_sco;
    else { INFO("[Info]: [TWS_ESCO] T_sco<6 value , please check\n\r\n"); }
    d%=pdu->T_sco;
    u8 logged=log_tag_const_i_TWS_ESCO!=0;
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]m_step1=0x%x,%d,%d,%d\n\r\n",local,lc->frame->bitoff,a,ba);
    preserve_bitoff_cal(peer->frame,peer->frame->bitoff,&b,&bb,600);
    u32 s=get_mclkn_to_local_clkn(b,bb,&local,0);
    if(pdu->timing_flags&2) s^=0x4000000;
    t=pdu->T_sco; s32 d2=-(s32)(s%t); if(d2<=0) d2+=t;
    s32 result=d-d2; if(result<=0) result+=t; result%=(s32)t;
    if(logged) {
        log_print(2,0,"[Info]: [TWS_ESCO]t_step2=0x%x,%d,%d,%d\n\r\n",local,peer->frame->bitoff,b,bb);
        log_print(2,0,"[Info]: [TWS_ESCO]%x, %x,%x,%x,%x\n\r\n",m,s,d,d2,(u8)result);
    }
    return result;
}
void CODE tws_esco_slave_cal_bitoff(struct link_connection *lc,struct link_connection *peer,u8 mode)
{
    u16 phone=lc->frame->bitoff;
    u32 clk=__read_reg_clkoffset(peer->frame); u16 bitoff=peer->frame->bitoff;
    u8 logged=log_tag_const_i_TWS_ESCO!=0;
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]---: %d, %d %x \n\r\n",phone,bitoff,clk);
    s_forward_offset_cal(peer->frame,&clk,&bitoff,phone,mode);
    peer->frame->bitoff=bitoff;
    __write_reg_clkoffset(peer->frame,clk);
    struct bredr_esco_link *esco=peer->esco;
    if(esco) { esco->hw.reg->BREDR_ESCO_BITOFF=peer->frame->bitoff; tws_link.last_tws_bitoff=peer->frame->bitoff; }
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]>>>>>: %d, %d, %x %d \n\r\n",phone,peer->frame->bitoff,clk,lc->frame->bitoff);
}
void CODE tws_esco_creat_start(void *arg)
{
    struct link_connection *peer=tws_link.link;
    if(!arg || !peer) return;
    struct bredr_esco_link *esco=peer->esco; if(!esco) return;
    if(esco->timer1!=255) { slot_timer_put(esco->timer1); peer->esco->timer1=255; }
    tws_get_time_offset(arg);
    INFO("[Info]: [TWS_ESCO]tws_esco_creat_start=%d\n\r\n",esco->pdu.D_sco);
}
void CODE tws_slave_rx_start_esco_req_timeout(void *arg)
{
    struct link_connection *peer=tws_link.link; if(!arg || !peer || !peer->esco) return;
    u8 d=tws_dsco_cal(arg,peer,&peer->esco->pdu); peer->esco->pdu.D_sco=d;
    tws_esco_creat_start(arg);
    INFO("[Info]: [TWS_ESCO]tws_slave_rx_start_esco_req_timeout=%d\n\r\n",peer->esco->pdu.D_sco);
}
void CODE tws_master_tx_start_esco_req(void *arg)
{
    struct link_connection *peer=tws_link.link; if(!arg || !peer || !peer->esco) return;
    struct bredr_esco_link *esco=peer->esco;
    u32 now=tws_link_read_slot_clk(), instant=bredr_offset2clkn(now,48);
    tws_conn_send_cmd(21,"41",instant,esco->pdu.D_sco);
    slot_timer_set(peer->esco->timer1,tws_esco_creat_start,arg,48);
    INFO("[Info]: [TWS_ESCO]tws_master_tx_start_esco_req: %x,%x %d,%d\n\r\n",instant,now,48,esco->pdu.D_sco);
}
void CODE tws_slave_rx_start_esco_req(u8 *data,void *arg)
{
    struct link_connection *peer=tws_link.link; if(!peer || !peer->esco || peer->esco->timer1==255) return;
    struct bredr_esco_link *esco=peer->esco;
    u32 instant=(u32)data[0]|((u32)data[1]<<8)|((u32)data[2]<<16)|((u32)data[3]<<24);
    esco->pdu.D_sco=data[4];
    u32 now=tws_link_read_slot_clk(); s32 delay=bredr_clkn2offset(now,instant);
    if(delay>4095) delay=2;
    slot_timer_set(peer->esco->timer1,tws_esco_creat_start,arg,delay>2?delay:2);
    INFO("[Info]: [TWS_ESCO]tws_slave_rx_start_esco_req : %x,%x, %d,%d\n\r\n",instant,now,delay,esco->pdu.D_sco);
}
void CODE bredr_tws_esco_send_data(struct link_connection *phone,void *unused)
{
    (void)unused;
    struct link_connection *peer=tws_link.link;
    struct bredr_esco_link *esco=peer->esco,*ph=phone->esco;
    if(!peer || !esco) return;
    if(!(esco->hw.reg->BREDR_ESCO_CON&1)) { ph->rsvd_state=0; return; }
    u32 index=((esco->hw.reg->BREDR_ESCO_CON>>1)&1)^1;
    local_irq_disable();
    struct bredr_rx_bulk *selected=NULL;
    for(struct list_head *p=ph->rx_esco.next;p!=&ph->rx_esco;p=p->next) {
        struct bredr_rx_bulk *rx=RX_ENTRY(p);
        if((rx->payload_head[2]&3) && !(rx->payload_head[2]&4)) { selected=rx; rx->payload_head[2]|=4; break; }
    }
    local_irq_enable();
    if(esco->transpond_rx_bulk[index]) { INFO("[Info]: [TWS_ESCO]check why tx bulk is not NULL\n\r\n"); }
    u8 *data=esco->tx_bulk[index]->data;
    if(selected) {
        esco->transpond_rx_bulk[index]=selected;
        if(selected->payload_head[2]&1) putchar('!'); else data=selected->data;
    }
    uintptr_t address=(uintptr_t)data; volatile struct bredr_esco_frame *frame=esco->hw.frame;
    if(!index) { frame->txptrl0=address; frame->txptrh0=address>>16; }
    else { frame->txptrl1=address; frame->txptrh1=address>>16; }
}
u8 CODE tws_clear_timeout_tr_esco_packet(struct link_connection *lc,int limit)
{
    struct list_head *head=&lc->esco->rx_esco;
    struct bredr_rx_bulk *rx=RX_ENTRY(head->next),*next=RX_ENTRY(rx->entry.next);
    int count=0;
    if(limit) {
        for(; &rx->entry!=head; rx=next,next=RX_ENTRY(next->entry.next)) ++count;
        if(count>limit && head->next!=head) {
            rx=RX_ENTRY(head->next);
            if(log_tag_const_i_TWS_ESCO) {
                log_print(2,0,"[Info]: [TWS_ESCO]@%d \r\n",count);
                if(rx->payload_head[2]&4) log_print(2,0,"[Info]: [TWS_ESCO]waring_clear_ESCO_FLAG_WAIT_RESEND1\n\r\n");
            }
            rx->payload_head[2]&=~3; --count;
        }
    } else {
        u8 logged=log_tag_const_i_TWS_ESCO;
        for(; &rx->entry!=&lc->esco->rx_esco; rx=next,next=RX_ENTRY(next->entry.next)) {
            if((rx->payload_head[2]&4) && logged) log_print(2,0,"[Info]: [TWS_ESCO]waring_clear_ESCO_FLAG_WAIT_RESEND2\n\r\n");
            rx->payload_head[2]&=~3; --count;
        }
    }
    return count;
}
u8 CODE tws_esco_transmit_deal(u8 step,struct link_connection *lc)
{
    struct link_connection *peer=tws_link.link; if(!peer) return 0;
    switch(step) {
    case 1:return 1;
    case 2:return peer->esco!=NULL;
    case 5:bredr_tws_esco_send_data(lc,NULL);return 0;
    case 6: {
        local_irq_disable(); struct list_head *head=&lc->esco->rx_esco;
        u8 result=head->next!=head && (RX_ENTRY(head->next)->payload_head[2]&3);
        local_irq_enable(); return !!result;
    }
    case 7:link_task_run(&peer->task);return 1;
    case 10:
        if(!peer->esco) return 0;
        return peer->esco->rx_remove_esco_req_flag || !(peer->esco->hw.reg->BREDR_ESCO_CON&1);
    default:return 0;
    }
}
void CODE tws_esco_tick_timer_deal(void *arg)
{
    struct link_connection *phone=arg,*peer=tws_link.link;
    if(!phone || !peer) return;
    u8 disable=0;
    volatile struct bredr_esco_reg *reg=peer->esco->hw.reg;
    if(!(reg->BREDR_ESCO_CON&1)) {
        reg->BREDR_ESCO_BITOFF=peer->frame->bitoff;
        u32 now=READ_SLOT_CLK()>>1;
        struct bredr_esco_link *esco=peer->esco;
        if(now>esco->esco_next_offset || (now==esco->esco_next_offset && (u32)(s32)esco->esco_next_soft_bitoff<REG32(0x1c00bc))) goto schedule;
        u32 mask=256u<<esco->hw.esco_id;
        REG32(0x1c0020)|=mask; REG32(0x1c001c)|=mask;
        esco->hw.reg->BREDR_ESCO_CON|=1;
        tws_esco_monitor_init(peer);
        if(peer->esco->esco_conn_start_monitor) { peer->esco->esco_conn_start_monitor=0; tws_esco_monitor_init(phone); }
        INFO("[Info]: [TWS_ESCO]tws_sco_enable %x\r\n",get_tws_master_clk(peer));
        peer->adjust_clkoffset_enable=1; phone->esco->rsvd_state=0;
    } else {
        u8 acl=tws_esco_acl_check(phone);
        struct bredr_esco_link *esco=peer->esco;
        struct ctrl_frame *frame;
        if(phone->esco->rsvd_state==0xaa && esco->phone_esco_err_cnt<10) {
            frame=peer->frame;
            if((frame->format&96)==96) { frame->format&=~96; frame->format|=esco->hw.esco_id<<5; }
            ++esco->phone_esco_err_cnt;
            if(acl) esco->tws_acl_cnt=5;
        } else {
            frame=peer->frame; frame->format|=96; esco=peer->esco;
            if(acl || esco->phone_esco_err_cnt>9) { esco->tws_acl_cnt=5; }
            else disable=esco->tws_acl_cnt==0;
            if(esco->tws_acl_cnt) --esco->tws_acl_cnt;
            esco->phone_esco_err_cnt=0;
        }
        if(!tws_conn.role) m_tws_offset_cal(frame,phone->frame->bitoff,1);
    }
    peer->esco->hw.reg->BREDR_ESCO_BITOFF=peer->frame->bitoff;
    if(peer->role) { __write_reg_clkoffset(peer->frame,peer->frame->clk_offset); bredr_set_frame_bitoff(peer->frame,peer->frame->bitoff); }
    link_task_run(&tws_link.link->task);
    if(disable && !tws_link.post_instant && !tws_link.remove_esco_timeout && !phone->esco->esco_acl_resume_t && !peer->esco->rx_remove_esco_req_flag) bredr_bd_frame_disable(peer->frame);
    else bredr_bd_frame_enable(peer->frame,0,0);
schedule:
    tws_get_time_offset(phone);
    u32 timeout=tws_link.remove_esco_timeout,t=phone->esco->pdu.T_sco;
    if(timeout>t) { tws_link.remove_esco_timeout=timeout-t; if((u16)(timeout-t)<t) tws_slave_remove_esco_deal(0,1); }
}
static void CODE bredr_tws_esco_get_data(void *arg)
{
    static u8 tws_last_ind=255;
    struct link_connection *lc=arg;
    struct bredr_esco_link *esco=*(struct bredr_esco_link **)((u8 *)arg+offsetof(struct link_connection,esco));
    struct link_connection *phone=esco->phone_lc;
    struct bredr_esco_link *ph=phone->esco;
    if(!tws_link.link || !tws_link.link->esco) return;
    ASSERT_AT(phone,"phone_lc",534);
    (*(struct ctrl_frame **)((u8 *)arg+offsetof(struct link_connection,frame)))->txtog&=~32;
    if(ph->esco_acl_resume_t) --ph->esco_acl_resume_t;
    if(!ph->esco_acl_resume_t) {
        if(tws_esco_check_role_switch(ph,0,0,0)) ph->esco_acl_resume_t=16;
        else if(!ph->esco_acl_resume_t && tws_conn.role_switch==3) {
            tws_conn.role_switch=0; tws_conn_switch_role(); ph->esco_acl_resume_t=16;
            INFO("[Info]: [TWS_ESCO]esco_role fail continue\r\n");
        }
    }
    /* Keep the SDK late null diagnostic despite earlier unchecked dereferences. */
    void *checked_arg=arg;
    __asm__("" : "+r"(checked_arg));
    ASSERT_AT(checked_arg,"_lc",222);
    u32 con=lc->esco->hw.reg->BREDR_ESCO_CON;
    u8 index=((con&2)>>1)^1;
    u8 crc=(((con&2)<<4)^32)+32; crc=(crc&con)!=0;
    u8 logged=log_tag_const_i_TWS_ESCO;
    if(logged && index==tws_last_ind) log_print(2,0,"[Info]: [TWS_ESCO]why esco_idx_err\n\r\n");
    tws_last_ind=index;
    if(!(crc && ph->rsvd_state!=0xaa)) {
        struct bredr_rx_bulk *rx=esco->transpond_rx_bulk[index];
        if(rx) {
            struct list_head *p;
            for(p=ph->rx_esco.next;p!=&ph->rx_esco;p=p->next) if(RX_ENTRY(p)==rx) break;
            if(p==&ph->rx_esco) {
                if(logged) log_print(2,0,"[Info]: [TWS_ESCO]find_transpond_rx_bulk err\n\r\n");
                esco->transpond_rx_bulk[index]=NULL; ph->rsvd_state=0; goto pointers;
            }
            if(crc) putchar('%');
            else if(rx->payload_head[2]&1) {
                u8 *data=esco->rx_bulk[index]->data; int i;
                for(i=0;i<10;i++) if(data[i]!=0xaa) break;
                if(i<10) { putchar('K'); __builtin_memcpy(rx->data,esco->rx_bulk[index]->data,ph->pdu.packet_len_m2s); rx->payload_head[2]=0; }
                else putchar('j');
            }
            rx->payload_head[2]=(rx->payload_head[2]<<5)&32;
            esco->transpond_rx_bulk[index]=NULL;
        }
        if(!esco->transpond_rx_bulk[0] && !esco->transpond_rx_bulk[1]) { ph->rsvd_state=0; os_sem_post(&esco_sem); }
    }
pointers:;
    uintptr_t tx=(uintptr_t)esco->tx_bulk[index]->data;
    volatile struct bredr_esco_frame *frame=esco->hw.frame;
    if(!index) { frame->txptrl0=tx; frame->txptrh0=tx>>16; }
    else { frame->txptrl1=tx; frame->txptrh1=tx>>16; }
    uintptr_t rx=(uintptr_t)esco->rx_bulk[0]->data;
    frame->rxptrl0=rx; frame->rxptrh0=rx>>16;
    rx=(uintptr_t)esco->rx_bulk[1]->data; frame->rxptrl1=rx; frame->rxptrh1=rx>>16;
    if(lc->role) { __write_reg_clkoffset(lc->frame,lc->frame->clk_offset); bredr_set_frame_bitoff(lc->frame,lc->frame->bitoff); }
    lc->esco->hw.reg->BREDR_ESCO_BITOFF=lc->frame->bitoff;
}
u8 CODE bredr_esco_tws_link_open(struct link_connection *lc,u8 monitor)
{
    struct bredr_esco_link *ph=lc->esco;
    struct link_connection *peer=tws_link.link;
    if(!ph || !peer) return 0;
    tws_link.remove_esco_timeout=0;
    tws_link.phone_link_acl_lt_addr=bredr_hw_read_lt_addr(lc->frame);
    local_irq_disable(); peer->adjust_clkoffset_enable=0;
    struct bredr_esco_link *esco=bt_malloc(sizeof(*esco));
    esco->hw.frame=NULL; esco->hw.esco_id=255;
    ASSERT_AT(esco,"tws_esco",955);
    u8 logged=log_tag_const_i_TWS_ESCO!=0;
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]_%s_\n\r\n",__FUNCTION__);
    __builtin_memcpy(&esco->pdu,&ph->pdu,sizeof(esco->pdu));
    esco->slot=ph->slot; esco->link_type=ph->link_type; esco->max_slot_cnt=ph->max_slot_cnt;
    esco->timer0=slot_timer_get(); esco->timer1=slot_timer_get(); esco->phone_lc=lc;
    esco->rx_remove_esco_req_flag=10;
    if(monitor) esco->esco_conn_start_monitor=0;
    else {
        esco->esco_conn_start_monitor=1;
        int count=0; for(struct list_head *p=ph->rx_esco.next;p!=&ph->rx_esco;p=p->next) ++count;
        if(logged) log_print(2,0,"[Info]: [TWS_ESCO]rx_esco_cnt=0x%x\n\r\n",count);
    }
    tws_link.link->esco=esco;
    INIT_LIST_HEAD(&esco->rx_esco); INIT_LIST_HEAD(&esco->tx_esco);
    local_irq_enable();
    u8 edr=ph->pdu.packet_type_s2m>13;
    u8 m2s=bredr_get_esco_packet_type(ph->pdu.packet_type_m2s),s2m=bredr_get_esco_packet_type(ph->pdu.packet_type_s2m);
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]pcket_type: %d, %d, %d, %d\n\r\n",m2s,s2m,ph->pdu.packet_len_m2s,ph->pdu.packet_len_s2m);
    esco->rx_bulk[0]=bredr_rx_bulk_alloc(0,ph->pdu.packet_len_m2s);
    esco->rx_bulk[1]=bredr_rx_bulk_alloc(0,ph->pdu.packet_len_m2s);
    esco->tx_bulk[0]=bredr_tx_bulk_alloc(ph->pdu.packet_len_s2m);
    esco->tx_bulk[1]=bredr_tx_bulk_alloc(ph->pdu.packet_len_s2m);
    esco->transpond_rx_bulk[0]=NULL; esco->transpond_rx_bulk[1]=NULL;
    __builtin_memset(esco->tx_bulk[0]->data,0xaa,ph->pdu.packet_len_s2m);
    __builtin_memset(esco->tx_bulk[1]->data,0xaa,ph->pdu.packet_len_s2m);
    esco->pdu.T_sco=ph->pdu.T_sco; esco->pdu.W_sco=ph->pdu.W_sco; esco->pdu.timing_flags=ph->pdu.timing_flags;
    struct ctrl_frame *peer_frame=tws_link.link->frame;
    bredr_get_esco_hw(peer_frame,&esco->hw);
    if(logged) log_print(2,0,"[Info]: [TWS_ESCO]tws->lt_addr=0x%x,tws_esco_lt_addr%x,role=%x,monitor=%x\n\r\n",peer_frame->miscntl&7,ph->pdu.tws_lt_addr,lc->role,lc->monitor);
    esco->get_data=bredr_tws_esco_get_data;
    volatile struct bredr_esco_frame *frame=esco->hw.frame;
    frame->con=(edr<<13)|(edr<<12)|(m2s<<8)|(s2m<<4)|((ph->link_type==0)<<3)|ph->pdu.tws_lt_addr;
    uintptr_t p=(uintptr_t)esco->rx_bulk[0]->data; frame->rxptrl0=p; frame->rxptrh0=p>>16;
    p=(uintptr_t)esco->rx_bulk[1]->data; frame->rxptrl1=p; frame->rxptrh1=p>>16;
    frame->rxlen=ph->pdu.packet_len_m2s;
    p=(uintptr_t)esco->tx_bulk[0]->data; frame->txptrl0=p; frame->txptrh0=p>>16;
    p=(uintptr_t)esco->tx_bulk[1]->data; frame->txptrl1=p; frame->txptrh1=p>>16;
    frame->txlen=ph->pdu.packet_len_s2m;
    esco->hw.reg->BREDR_ESCO_INTERVAL=ph->pdu.T_sco*2-1;
    esco->hw.reg->BREDR_ESCO_CNT=195; esco->hw.reg->BREDR_ESCO_CON=32768;
    u8 master=!tws_conn.role;
    esco_tws_offset_cal_start(peer->frame,peer->frame->bitoff);
    if(master) peer->esco->hw.reg->BREDR_ESCO_BITOFF=peer->frame->bitoff;
    bd_set_prio(peer_frame,29,0); peer->frame->format|=96;
    if(!tws_conn.role) {
        u8 d=tws_dsco_cal(lc,peer,&esco->pdu); peer->esco->pdu.D_sco=d;
        slot_timer_set(peer->esco->timer1,tws_master_tx_start_esco_req,lc,4);
    } else {
        link_task_run_slots(&peer->task,64);
        slot_timer_set(peer->esco->timer1,tws_slave_rx_start_esco_req_timeout,lc,48);
        INFO("[Info]: [TWS_ESCO]set_slave_start_esco_timeout\n\r\n");
    }
    return 1;
}
u8 CODE tws_esco_disable(struct link_connection *lc)
{
    struct link_connection *peer=lc->tws_link?lc:tws_link.link;
    if(!peer || !peer->esco) return 0;
    struct bredr_esco_link *esco=peer->esco;
    if(esco->timer0!=255) { slot_timer_put(esco->timer0); peer->esco->timer0=255; }
    esco=peer->esco;
    if(esco->timer1!=255) { slot_timer_put(esco->timer1); peer->esco->timer1=255; }
    bredr_put_esco_hw(&peer->esco->hw); return 1;
}
void CODE tws_link_esco_close_adjust_clkoffset(void)
{
    if(tws_link.link && tws_link.link->esco) tws_link.link->adjust_clkoffset_enable=0;
}
u8 CODE bredr_esco_tws_link_close(struct link_connection *lc)
{
    struct link_connection *peer=lc->tws_link?lc:tws_link.link;
    if(!peer || !peer->esco) return 0;
    tws_esco_disable(lc); local_irq_disable(); tws_link.esco_tx_sync_req_cnt=0;
    INFO("[Info]: [TWS_ESCO]clos_tws_lc->esco->pdu.tws_lt_addr=%d\n\r\n",peer->esco->pdu.tws_lt_addr);
    bredr_rx_bulk_free(peer->esco->rx_bulk[0]); bredr_rx_bulk_free(peer->esco->rx_bulk[1]);
    bredr_tx_bulk_free(peer->esco->tx_bulk[0]); bredr_tx_bulk_free(peer->esco->tx_bulk[1]);
    peer->esco->transpond_rx_bulk[0]=NULL; peer->esco->transpond_rx_bulk[1]=NULL;
    struct list_head *p=peer->esco->rx_esco.next;
    while(p!=&peer->esco->rx_esco) { struct list_head *next=p->next; __list_del_entry(p); bredr_rx_bulk_free(RX_ENTRY(p)); p=next; }
    if(tws_link.slot_instant_timer) { slot_timer_put(tws_link.slot_instant_timer); tws_link.slot_instant_timer=0; }
    if(peer->role) { __write_reg_clkoffset(peer->frame,peer->frame->clk_offset); bredr_set_frame_bitoff(peer->frame,peer->frame->bitoff); }
    peer->frame->format|=96; bd_set_prio(peer->frame,20,0);
    bredr_hw_write_lt_addr(lc->frame,tws_link.phone_link_acl_lt_addr);
    bt_free(peer->esco); peer->esco=NULL; local_irq_enable(); peer->adjust_clkoffset_enable=0;
    u8 master=!tws_link.link->role;
    esco_tws_offset_cal_start(peer->frame,peer->frame->bitoff);
    if(master) {
        tws_esco_link_monitor_master_resume(lc->frame,peer->frame);
        tws_esco_link_monitor_master_resume(peer->frame,peer->frame);
        tws_link_monitor_master_disable(peer->frame);
    } else tws_esco_link_monitor_slave_resume(lc->frame,peer->frame);
    link_task_run_slots(&peer->task,64); peer->adjust_clkoffset_enable=1;
    INFO("[Info]: [TWS_ESCO]%s==>\r\n",__FUNCTION__); return 1;
}
void CODE tws_esco_suspend_resume(u8 flag)
{
    local_irq_disable(); struct link_connection *peer=tws_link.link;
    if(!peer || !peer->esco) { local_irq_enable(); return; }
    struct list_head *head=&peer->esco->rx_esco;
    struct bredr_rx_bulk *rx=RX_ENTRY(head->next),*next=RX_ENTRY(rx->entry.next);
    int count=0;
    while(&rx->entry!=head) {
        __list_del_entry(&rx->entry); bredr_rx_bulk_free(rx);
        if(flag!=1 && ++count==flag) break;
        rx=next; next=RX_ENTRY(next->entry.next);
    }
    local_irq_enable();
}

#ifdef UNITTEST
#include <stdlib.h>
#include <stdarg.h>
#include "test.h"
struct tws_link_connection tws_link;
struct tws_connection tws_conn;
OS_SEM esco_sem;
u8 log_tag_const_i_TWS_ESCO;
int config_asser;
static struct link_connection phone,peer;
static struct bredr_esco_link ph,relay;
static struct ctrl_frame phone_frame,peer_frame;
static struct bredr_esco_reg hwreg;
static struct bredr_esco_frame hwframe;
static int irq,max_irq,timer_puts,gets,frees,sem_posts,task_runs,run_slots,enabled,disabled,master_init,slave_init,master_resume,slave_resume,hw_put,priority,logs,resets,asserts,role_checks,role_result,role_switches,last_char;
static u32 master_clock,slave_clock,local_clock,slot_clock,command_instant;
static int command_opcode,command_d,delay,clkn_delay, timer_id, fine,cal_calls,cal_mode;
static void (*timer_handler)(void *);
static void *timer_arg;
static int mutate_log,mutate_timer,mutate_offset;
static int esco_test_putchar(int c) { last_char=c;return c; }
void local_irq_disable(void) { ++irq; if(irq>max_irq) max_irq=irq; }
void local_irq_enable(void) { CHECK(irq>0); --irq; }
void p33_soft_reset(void) { ++resets; }
void cpu_assert_debug(void) { ++asserts; }
void log_print(int level,const char *tag,const char *fmt,...)
{
    CHECK(level==2 && tag==NULL); ++logs;
    if(mutate_log==1 && strstr(fmt,"m_step1")) { log_tag_const_i_TWS_ESCO=0; }
    if(mutate_log==2 && strstr(fmt,"WAIT_RESEND")) { RX_ENTRY(ph.rx_esco.next)->payload_head[2]=0xff; }
}
void preserve_bitoff_cal(struct ctrl_frame *frame,u32 bitoff,u32 *offset,u16 *out,u16 change)
{ CHECK(change==600 && bitoff==frame->bitoff); *offset=frame==&phone_frame?11:22; *out=frame->bitoff; }
unsigned long get_mclkn(unsigned long off,u16 bit,u32 *local)
{ (void)off;(void)bit; *local=local_clock;return master_clock; }
unsigned long get_mclkn_to_local_clkn(unsigned long off,u16 bit,u32 *local,u8 read)
{ (void)off;(void)bit; *local=local_clock;return read?master_clock:slave_clock; }
s32 __read_reg_clkoffset(struct ctrl_frame *frame) { return frame->clk_offset; }
void __write_reg_clkoffset(struct ctrl_frame *frame,u32 value) { frame->clk_offset=value; }
void s_forward_offset_cal(struct ctrl_frame *frame,u32 *clk,u16 *bit,u16 phone_bit,u8 mode)
{ CHECK(frame==&peer_frame && phone_bit==phone_frame.bitoff);*clk=0x12345678;*bit=321;cal_mode=mode; }
void m_tws_offset_cal(struct ctrl_frame *frame,u16 bit,u8 mode) { CHECK(frame==&peer_frame && bit==phone_frame.bitoff && mode==1);++cal_calls; }
u8 slot_timer_get(void) { return ++gets; }
void slot_timer_put(u8 id) { CHECK(id!=255);++timer_puts; }
void slot_timer_set(u8 id,void (*handler)(void *),void *arg,int ticks)
{ timer_id=id;timer_handler=handler;timer_arg=arg;delay=ticks; if(mutate_timer) peer.esco=&relay; }
void slot_timer_set_finecnt(u8 id,void (*handler)(void *),void *arg,int ticks,short bit)
{ slot_timer_set(id,handler,arg,ticks);fine=bit; }
u32 tws_link_read_slot_clk(void) { return slot_clock; }
u32 READ_SLOT_CLK(void) { return slot_clock; }
int bredr_offset2clkn(int clk,int off) { return clk+off; }
int bredr_clkn2offset(int clk,int instant) { (void)clk;(void)instant;return clkn_delay; }
int tws_conn_send_cmd(u8 opcode,const char *fmt,...)
{ CHECK(!strcmp(fmt,"41"));command_opcode=opcode;va_list ap;va_start(ap,fmt);command_instant=va_arg(ap,u32);command_d=va_arg(ap,int);va_end(ap);return 0; }
void link_task_run(struct link_task *task) { CHECK(task==&peer.task);++task_runs; }
void link_task_run_slots(struct link_task *task,int slots) { CHECK(task==&peer.task && slots==64);++run_slots; }
void bredr_set_frame_bitoff(struct ctrl_frame *frame,u16 bit) { frame->bitoff=bit; }
void bredr_bd_frame_disable(struct ctrl_frame *frame) { CHECK(frame==&peer_frame);++disabled; }
void bredr_bd_frame_enable(struct ctrl_frame *frame,u8 a,u8 b) { CHECK(frame==&peer_frame && !a && !b);++enabled; }
u8 tws_slave_remove_esco_deal(int instant,u8 reason) { CHECK(!instant && reason==1);++role_switches;return 0; }
u8 tws_esco_check_role_switch(struct bredr_esco_link *esco,u8 a,u8 b,u16 c) { CHECK(esco==&ph && !a && !b && !c);++role_checks;return role_result; }
void tws_conn_switch_role(void) { ++role_switches; }
int os_sem_post(OS_SEM *sem) { CHECK(sem==&esco_sem);++sem_posts;return 0; }
void *bt_malloc(int size) { void *p=malloc(size);memset(p,0xa5,size);return p; }
void bt_free(void *p) { free(p);++frees; }
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int type,int size) { CHECK(type==0);return calloc(1,sizeof(struct bredr_rx_bulk)+size); }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size) { return calloc(1,sizeof(struct bredr_tx_bulk)+size); }
void bredr_rx_bulk_free(struct bredr_rx_bulk *rx) { free(rx);++frees; }
void bredr_tx_bulk_free(struct bredr_tx_bulk *tx) { free(tx);++frees; }
u8 bredr_get_esco_packet_type(u8 type) { return type&15; }
int bredr_get_esco_hw(struct ctrl_frame *frame,struct bredr_esco_hw *hw) { CHECK(frame==&peer_frame && hw->esco_id==255 && !hw->frame);hw->esco_id=1;hw->frame=&hwframe;hw->reg=&hwreg;return 0; }
void bredr_put_esco_hw(struct bredr_esco_hw *hw) { CHECK(hw->reg==&hwreg);++hw_put; }
void esco_tws_offset_cal_start(struct ctrl_frame *frame,u16 bit) { CHECK(frame==&peer_frame && bit==frame->bitoff);if(mutate_offset) { tws_conn.role=1;tws_link.link->role=1; } }
void bd_set_prio(struct ctrl_frame *frame,u8 prio,u8 add) { CHECK(frame==&peer_frame && !add);priority=prio; }
void bredr_hw_write_lt_addr(struct ctrl_frame *frame,u8 lt) { CHECK(frame==&phone_frame);frame->miscntl=(frame->miscntl&~0x70)|(lt<<4); }
void tws_esco_link_monitor_master_init(struct ctrl_frame *a,struct ctrl_frame *b) { CHECK(a && b==&peer_frame);++master_init; }
void tws_esco_link_monitor_slave_init(struct ctrl_frame *a,struct ctrl_frame *b) { CHECK(a && b==&peer_frame);++slave_init; }
void tws_esco_link_monitor_master_resume(struct ctrl_frame *a,struct ctrl_frame *b) { CHECK(a && b==&peer_frame);++master_resume; }
void tws_esco_link_monitor_slave_resume(struct ctrl_frame *a,struct ctrl_frame *b) { CHECK(a && b==&peer_frame);++slave_resume; }
void tws_link_monitor_master_disable(struct ctrl_frame *frame) { CHECK(frame==&peer_frame);++disabled; }
static void fixture(void)
{
    memset(&phone,0,sizeof(phone));memset(&peer,0,sizeof(peer));memset(&ph,0,sizeof(ph));memset(&relay,0,sizeof(relay));memset(&tws_link,0,sizeof(tws_link));memset(&tws_conn,0,sizeof(tws_conn));memset(&phone_frame,0,sizeof(phone_frame));memset(&peer_frame,0,sizeof(peer_frame));memset(&hwreg,0,sizeof(hwreg));memset(&hwframe,0,sizeof(hwframe));memset((void *)test_regs,0,sizeof(test_regs));
    phone.esco=&ph;phone.frame=&phone_frame;peer.esco=&relay;peer.frame=&peer_frame;tws_link.link=&peer;
    relay.hw.reg=&hwreg;relay.hw.frame=&hwframe;relay.phone_lc=&phone;relay.timer0=1;relay.timer1=2;
    ph.pdu.T_sco=relay.pdu.T_sco=6;ph.pdu.D_sco=relay.pdu.D_sco=2;ph.slot=relay.slot=1;
    INIT_LIST_HEAD(&ph.rx_esco);INIT_LIST_HEAD(&relay.rx_esco);INIT_LIST_HEAD(&ph.tx_esco);INIT_LIST_HEAD(&relay.tx_esco);
    phone_frame.bitoff=100;peer_frame.bitoff=200;
    irq=max_irq=timer_puts=gets=frees=sem_posts=task_runs=run_slots=enabled=disabled=master_init=slave_init=master_resume=slave_resume=hw_put=priority=logs=resets=asserts=role_checks=role_result=role_switches=last_char=0;
    command_opcode=command_d=delay=clkn_delay=timer_id=fine=cal_calls=cal_mode=mutate_log=mutate_timer=mutate_offset=0;
    master_clock=slave_clock=slot_clock=0;local_clock=100;log_tag_const_i_TWS_ESCO=0;config_asser=0;timer_handler=NULL;timer_arg=NULL;
}
static struct bredr_rx_bulk *append(struct bredr_esco_link *esco,u8 flags)
{
    struct bredr_rx_bulk *rx=bredr_rx_bulk_alloc(0,32);rx->payload_head[2]=flags;
    struct list_head *head=&esco->rx_esco,*tail=head->prev;
    rx->entry.next=head;rx->entry.prev=tail;tail->next=&rx->entry;head->prev=&rx->entry;return rx;
}
static void clear_rx(struct bredr_esco_link *esco)
{ while(esco->rx_esco.next!=&esco->rx_esco) { struct list_head *p=esco->rx_esco.next;__list_del_entry(p);bredr_rx_bulk_free(RX_ENTRY(p)); } }
int main(void)
{
    fixture();CHECK(get_esco_packet_dump()==120 && !get_tws_frame_flag(NULL));peer.esco=NULL;CHECK(get_tws_frame_flag(NULL));peer.esco=&relay;
    tws_esco_tx_requset(&ph,1,9);CHECK(ph.esco_role_switch_state==2 && ph.esco_acl_resume_t==9);
    tws_esco_tx_requset(&ph,1,20);CHECK(ph.esco_acl_resume_t==9);
    tws_esco_tx_requset(&ph,2,12);CHECK(ph.esco_role_switch_state==4 && ph.esco_acl_resume_t==12);
    tws_esco_rx_requset(&ph,1,6);CHECK(ph.esco_role_switch_state==4);
    tws_esco_rx_requset(&ph,1,7);CHECK(ph.esco_role_switch_state==3 && ph.esco_acl_resume_t==6);
    tws_esco_rx_requset(&ph,2,0);CHECK(ph.esco_role_switch_state==5 && ph.esco_acl_resume_t==255);
    /* 100 rounded to 96; clock94-D2=92 reaches 96-2-6+2=90. */
    fixture();master_clock=94;relay.rx_remove_esco_req_flag=258;u8 status=0;
    CHECK(tws_esco_acl_communication(&phone,&status)==2 && status==2 && relay.rx_remove_esco_req_flag==257 && !tws_link.acl_communication);
    hwreg.BREDR_ESCO_CON=1;CHECK(tws_esco_acl_communication(&phone,NULL)==2 && tws_link.acl_communication);
    master_clock=10;relay.rx_remove_esco_req_flag=0;CHECK(!tws_esco_acl_check(&phone));
    ph.esco_acl_resume_t=16;CHECK(tws_esco_acl_communication(&phone,NULL)==2 && tws_link.acl_communication);
    master_clock=598;ph.rx_esco_num=9;check_esco_rx_info(&phone);CHECK(!ph.rx_esco_num);
    /* DSCO: m=10 -> remaining4 +4 =>2mod6; s=8 =>4; (2-4+6)%6=4. */
    fixture();master_clock=10;slave_clock=8;CHECK(tws_dsco_cal(&phone,&peer,&ph.pdu)==4);
    log_tag_const_i_TWS_ESCO=1;mutate_log=1;CHECK(tws_dsco_cal(&phone,&peer,&ph.pdu)==4 && logs==3 && !log_tag_const_i_TWS_ESCO);
    fixture();tws_esco_monitor_init(&phone);CHECK(master_init==1);tws_conn.role=1;tws_esco_monitor_init(&phone);CHECK(slave_init==1);
    tws_esco_slave_cal_bitoff(&phone,&peer,3);CHECK(peer_frame.bitoff==321 && peer_frame.clk_offset==0x12345678 && hwreg.BREDR_ESCO_BITOFF==321 && tws_link.last_tws_bitoff==321 && cal_mode==3);
    /* Negative queue return is truncated, and clearing flags does not free. */
    fixture();struct bredr_rx_bulk *a=append(&ph,7),*b=append(&ph,3),*c=append(&ph,1);
    CHECK(tws_clear_timeout_tr_esco_packet(&phone,2)==2 && a->payload_head[2]==4 && b->payload_head[2]==3 && !frees);
    CHECK(tws_clear_timeout_tr_esco_packet(&phone,0)==253 && b->payload_head[2]==0 && c->payload_head[2]==0);clear_rx(&ph);
    fixture();append(&relay,1);append(&relay,1);append(&relay,1);tws_esco_suspend_resume(2);CHECK(frees==2);tws_esco_suspend_resume(0);CHECK(frees==3 && !irq);
    fixture();append(&relay,1);append(&relay,1);tws_esco_suspend_resume(1);CHECK(frees==2 && !irq);
    /* Protocol marshaling and strict signed clock-delay clamps. */
    fixture();slot_clock=1000;relay.pdu.D_sco=5;tws_master_tx_start_esco_req(&phone);
    CHECK(command_opcode==21 && command_instant==1048 && command_d==5 && delay==48 && timer_handler==tws_esco_creat_start && timer_arg==&phone);
    u8 packet[5]={0x78,0x56,0x34,0x12,9};clkn_delay=-1;tws_slave_rx_start_esco_req(packet,&phone);CHECK(delay==2 && relay.pdu.D_sco==9);
    clkn_delay=4095;tws_slave_rx_start_esco_req(packet,&phone);CHECK(delay==4095);clkn_delay=4096;tws_slave_rx_start_esco_req(packet,&phone);CHECK(delay==2);
    fixture();hwreg.BREDR_ESCO_CON=0;master_clock=10;tws_esco_creat_start(&phone);
    CHECK(timer_puts==1 && relay.timer1==255 && timer_id==1 && delay==110 && fine==200 && timer_handler==tws_esco_tick_timer_deal && relay.esco_next_offset==55 && relay.esco_next_soft_bitoff==800 && !irq);
    /* Disabled engine activation, then active idle-frame suppression. */
    fixture();slot_clock=100;relay.esco_next_offset=100;relay.hw.esco_id=1;relay.esco_conn_start_monitor=1;tws_esco_tick_timer_deal(&phone);
    CHECK((hwreg.BREDR_ESCO_CON&1) && REG32(0x1c0020)==512 && REG32(0x1c001c)==512 && master_init==2 && enabled==1 && peer.adjust_clkoffset_enable && !irq);
    fixture();hwreg.BREDR_ESCO_CON=1;master_clock=10;tws_esco_tick_timer_deal(&phone);CHECK(disabled==1 && task_runs==1 && cal_calls==1);
    fixture();hwreg.BREDR_ESCO_CON=1;master_clock=10;tws_link.remove_esco_timeout=10;tws_esco_tick_timer_deal(&phone);CHECK(tws_link.remove_esco_timeout==4 && role_switches==1 && enabled==1);
    /* Signed soft bit offset is compared as unsigned against hardware. */
    fixture();slot_clock=100;relay.esco_next_offset=50;relay.esco_next_soft_bitoff=-1;REG32(0x1c00bc)=100;tws_esco_tick_timer_deal(&phone);CHECK(hwreg.BREDR_ESCO_CON&1);
    fixture();slot_clock=100;relay.esco_next_offset=50;relay.esco_next_soft_bitoff=99;REG32(0x1c00bc)=100;tws_esco_tick_timer_deal(&phone);CHECK(!hwreg.BREDR_ESCO_CON && !enabled && !disabled);
    fixture();peer_frame.bitoff=700;master_clock=10;tws_get_time_offset(&phone);CHECK(relay.esco_next_soft_bitoff==50 && relay.esco_next_offset==56);
    fixture();hwreg.BREDR_ESCO_CON=1;master_clock=10;tws_link.remove_esco_timeout=6;tws_esco_tick_timer_deal(&phone);CHECK(tws_link.remove_esco_timeout==6 && !role_switches);
    /* Full relay open/close: allocation is deliberately uncleared. */
    fixture();peer.esco=NULL;phone_frame.miscntl=0x50;ph.pdu.packet_len_m2s=16;ph.pdu.packet_len_s2m=20;ph.pdu.packet_type_m2s=3;ph.pdu.packet_type_s2m=14;ph.pdu.tws_lt_addr=4;
    CHECK(bredr_esco_tws_link_open(&phone,0)==1);struct bredr_esco_link *opened=peer.esco;
    CHECK(opened && opened->rx_remove_esco_req_flag==10 && opened->esco_conn_start_monitor==1 && opened->esco_acl_resume_t==0xa5 && opened->get_data==bredr_tws_esco_get_data && hwframe.con==0x33ec && hwframe.rxlen==16 && hwframe.txlen==20 && opened->tx_bulk[0]->data[19]==0xaa && hwreg.BREDR_ESCO_INTERVAL==11 && hwreg.BREDR_ESCO_CNT==195 && hwreg.BREDR_ESCO_CON==32768 && priority==29 && delay==4 && !irq);
    mutate_offset=1;
    CHECK(bredr_esco_tws_link_close(&phone)==1 && !peer.esco && frees==5 && timer_puts==2 && hw_put==1 && priority==20 && master_resume==2 && peer.adjust_clkoffset_enable && !irq);
    /* Open caches role before the offset hook; subsequent role selection reloads. */
    fixture();peer.esco=NULL;ph.pdu.packet_len_m2s=16;ph.pdu.packet_len_s2m=20;hwreg.BREDR_ESCO_BITOFF=999;mutate_offset=1;
    CHECK(bredr_esco_tws_link_open(&phone,1)==1 && hwreg.BREDR_ESCO_BITOFF==200 && run_slots==1 && delay==48);
    mutate_offset=0;CHECK(bredr_esco_tws_link_close(&phone)==1 && slave_resume==1 && !irq);
    /* Relay ring selection and receive completion with ten-byte marker probe. */
    fixture();relay.rx_bulk[0]=bredr_rx_bulk_alloc(0,32);relay.rx_bulk[1]=bredr_rx_bulk_alloc(0,32);relay.tx_bulk[0]=bredr_tx_bulk_alloc(32);relay.tx_bulk[1]=bredr_tx_bulk_alloc(32);
    hwreg.BREDR_ESCO_CON=3;a=append(&ph,2);bredr_tws_esco_send_data(&phone,NULL);
    CHECK(relay.transpond_rx_bulk[0]==a && a->payload_head[2]==6 && hwframe.txptrl0==(u16)(uintptr_t)a->data);
    ph.esco_acl_resume_t=1;bredr_tws_esco_get_data(&peer);CHECK(!relay.transpond_rx_bulk[0] && !a->payload_head[2] && sem_posts==1 && role_checks==1);
    a->payload_head[2]=1;relay.transpond_rx_bulk[0]=a;ph.pdu.packet_len_m2s=12;memset(relay.rx_bulk[0]->data,0x42,12);bredr_tws_esco_get_data(&peer);CHECK(!memcmp(a->data,relay.rx_bulk[0]->data,12) && !a->payload_head[2]);
    a->payload_head[2]=1;relay.transpond_rx_bulk[0]=a;memset(relay.rx_bulk[0]->data,0xaa,12);bredr_tws_esco_get_data(&peer);CHECK(a->payload_head[2]==32);
    CHECK(tws_esco_transmit_deal(1,&phone)==1 && tws_esco_transmit_deal(2,&phone)==1 && tws_esco_transmit_deal(6,&phone)==0 && tws_esco_transmit_deal(7,&phone)==1 && tws_esco_transmit_deal(10,&phone)==0);
    clear_rx(&ph);bredr_rx_bulk_free(relay.rx_bulk[0]);bredr_rx_bulk_free(relay.rx_bulk[1]);bredr_tx_bulk_free(relay.tx_bulk[0]);bredr_tx_bulk_free(relay.tx_bulk[1]);
    CHECK(!irq);return TEST_DONE("btctrler_bd19_tws_esco");
}
#endif
