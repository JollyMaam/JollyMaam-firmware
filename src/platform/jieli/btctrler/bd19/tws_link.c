/* BD19 tws_link.c.o, occurrence 1. The 68-byte core record and BD19
 * monitor hardware differ from BR23. Preserve the SDK callback/reload order. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "btctrler/glue.h"
#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_IRQ __attribute__((section(".tws_irq_code")))
#define TWS_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/tws_link.c"
void log_print(int, const char *, const char *, ...);
void p33_soft_reset(void);
extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS,log_tag_const_d_TWS,log_tag_const_e_TWS,log_tag_const_v_TWS;
extern BTCTRLER_CONFIG int CONFIG_PHONE_RUN_SLOT,CONFIG_TWS_RUN_SLOT;
extern BTCTRLER_CONFIG int CONFIG_PHONE_LOW_LATENCY_RUN_SLOT,CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
#define INFO(...) do { if(log_tag_const_i_TWS) log_print(2,0,__VA_ARGS__); } while(0)
#define DEBUG(...) do { if(log_tag_const_d_TWS) log_print(1,0,__VA_ARGS__); } while(0)
#ifdef UNITTEST
static u32 tws_test_regs[128];
#define REG32(a) tws_test_regs[((a)-0x1c0000)/4]
#else
#define REG32(a) (*(volatile u32 *)(uintptr_t)(a))
#endif
#define memcpy __builtin_memcpy
#define memset __builtin_memset
/* The SDK bitfield stores interleave with volatile halfword reads. Permit
 * aliasing here so optimization retains those staged 32-bit wire stores. */
typedef u32 tws_wire_word __attribute__((may_alias));
struct tws_link_connection tws_link __attribute__((common));
int g_soft_agc_enable = 1;
int slave_rx_switch_irq;
static int tws_link_tx_sync_req(struct lmp_connection *, u8 *, int) TWS_IRQ;
static int tws_link_rx_sync_req(struct lmp_connection *, u8 *, int, u8 *) TWS_CODE;
static int tws_link_connection_resume(struct link_task *) TWS_IRQ;
static int tws_link_connection_suspend(struct link_task *) TWS_IRQ;
static void tws_slave_sniffer_cfg(struct ctrl_frame *,struct ctrl_frame *,u32,u16,u32,u16,u8 *,u8 *) TWS_CODE;
static void tws_slave_tlink_monitor_enable(void *) TWS_CODE;
static void phone_link_ack_enable(void *) TWS_CODE;
static void tws_master_plink_monitor_disable(void *) TWS_CODE;
static void bredr_frame_tx_enable(struct ctrl_frame *f) { f->miscntl &= (u16)~8192; }
static void bredr_frame_tx_disable(struct ctrl_frame *f) { f->miscntl |= 8192; }
static void cpu_reset(void) { p33_soft_reset(); }
#define SDK_ASSERT(p,line,text) do { if(config_asser) { if(!(p)) { \
 printf("file:%s, line:%d",TWS_FILE,line); printf("ASSERT-FAILD: " text " "); cpu_assert_debug(); \
 } } else if(!(p)) cpu_reset(); } while(0)
const struct tws_sync_channel tws_link_sync __attribute__((section(".tws_sync_channel.1"),aligned(4))) = {
 1,12,tws_link_tx_sync_req,0,tws_link_rx_sync_req,0,0,0
};
const struct link_task_ops tws_link_task_ops __attribute__((section(".classic_tws_const"),aligned(4))) = {
 tws_link_connection_resume,tws_link_connection_suspend
};

u32 TWS_CODE tws_link_read_slot_clk(void)
{ struct link_connection *p=tws_link.link; return p?bredr_get_master_slot_clk(p):0; }
void TWS_CODE tws_link_monitor_slave_init(struct ctrl_frame *f)
{ struct ctrl_frame *t=tws_link.link->frame; f->miscntl &= (u16)~1536; t->miscntl &= (u16)~512; }
void TWS_CODE set_master_p_link_monitor_parm(struct ctrl_frame *f,u8 call)
{
 f->twsbdaddr0=f->bdaddr0; f->twsbdaddr1=f->bdaddr1; f->twsbdaddr2=f->bdaddr2;
 f->miscntl|=512; f->twscon=6; f->twscnt=2570; f->twsrxcnt=2560; f->twstxcnt=588;
 if(call) { f->twsbchl=f->bchl; f->twsbchm=f->bchm; f->twsbchh=f->bchh; }
}
void TWS_CODE set_slave_p_link_monitor_parm(struct ctrl_frame *p,struct ctrl_frame *t,u32 clk,u16 bit,u8 call)
{
 (void)call; p->miscntl|=512;
 p->twsbdaddr0=t->bdaddr0; p->twsbdaddr1=t->bdaddr1; p->twsbdaddr2=t->bdaddr2;
 p->twsbchl=t->bchl; p->twsbchm=t->bchm; p->twsbchh=t->bchh;
 p->twscon=((clkn0_phase(bit)<<5)-32)|9; p->twscnt=2570; p->twstxcnt=2560; p->twsrxcnt=637;
 p->twsclknoffl=clk; p->twsclknoffh=clk>>16;
}
void TWS_CODE set_slave_tws_link_monitor_parm(struct ctrl_frame *p,struct ctrl_frame *t,u32 clk,u16 bit,u8 call)
{
 t->twsbdaddr0=p->bdaddr0; t->twsbdaddr1=p->bdaddr1; t->twsbdaddr2=p->bdaddr2;
 t->twscon=((clkn0_phase(bit)<<5)-32)|15; t->twscnt=2570; t->twstxcnt=2560; t->twsrxcnt=596;
 if(call) t->miscntl|=512;
 else { t->twsbchl=p->bchl; t->twsbchm=p->bchm; t->twsbchh=p->bchh;
 t->twsclknoffl=clk; t->twsclknoffh=clk>>16; }
}
void TWS_CODE tws_link_monitor_master_init(struct ctrl_frame *f) { set_master_p_link_monitor_parm(f,0); }
void TWS_CODE tws_link_monitor_master_enable(struct ctrl_frame *f) { (void)f; }
void TWS_CODE tws_link_monitor_master_disable(struct ctrl_frame *f) { f->miscntl &= (u16)~512; }
void TWS_CODE tws_esco_link_monitor_master_resume(struct ctrl_frame *f,struct ctrl_frame *t)
{ set_master_p_link_monitor_parm(f,1); if(f!=t) { u32 c=t->clk_offset; f->twsclknoffl=c; f->twsclknoffh=c>>16; } else f->miscntl&=(u16)~512; }
void TWS_CODE tws_esco_link_monitor_master_init(struct ctrl_frame *f,struct ctrl_frame *t)
{
 f->miscntl=f!=t?(f->miscntl|512):(f->miscntl & (u16)~512); f->twscon=18;
 REG32(0x1c0014)&=~255u; REG32(0x1c0014)|=19;
 f->twsbdaddr0=t->bdaddr0; f->twsbdaddr1=t->bdaddr1; f->twsbdaddr2=t->bdaddr2;
 f->twscnt=2570; f->twstxcnt=2710; f->twsrxcnt=2642;
}
void TWS_CODE tws_esco_link_monitor_slave_resume(struct ctrl_frame *f,struct ctrl_frame *t)
{ set_slave_p_link_monitor_parm(f,t,t->clk_offset,t->bitoff,0); set_slave_tws_link_monitor_parm(f,t,0,f->bitoff,1); }
void TWS_CODE tws_esco_link_monitor_slave_init(struct ctrl_frame *f,struct ctrl_frame *t)
{
 if(f!=t) { f->miscntl|=512; f->miscntl&=(u16)~8192; f->miscntl&=(u16)~1024; }
 else f->miscntl&=(u16)~512;
 REG32(0x1c0014)&=~255u; REG32(0x1c0014)|=19;
 f->twsbdaddr0=t->bdaddr0; f->twsbdaddr1=t->bdaddr1; f->twsbdaddr2=t->bdaddr2;
 f->twscon=19; f->twscnt=2570; f->twstxcnt=2640; f->twsrxcnt=2716;
}
static void tws_slave_sniffer_cfg(struct ctrl_frame *t,struct ctrl_frame *p,u32 tc,u16 tb,u32 pc,u16 pb,u8 *tm,u8 *pm)
{ set_slave_p_link_monitor_parm(p,t,tc,tb,0); bdhw_set_afh(p,2,tm);
 set_slave_tws_link_monitor_parm(p,t,pc,pb,0); if(pm) bdhw_set_afh(t,2,pm); }
void TWS_CODE tws_link_master_2_slave(struct link_connection *p)
{
 struct ctrl_frame *pf=p->frame,*tf=tws_link.link->frame;
 DEBUG("[Debug]: [TWS]master2slave=%d, %x, %d\n\r\n",p->esco,tf->clk_offset,tf->bitoff);
 local_irq_disable(); p->monitor=1; tws_link.bk_clk_offset=tf->clk_offset; tws_link.bk_bitoff=tf->bitoff;
 link_conn_qos_disable(p); bredr_frame_role_switch_2_slave(tf); tws_link.link->role=1;
 u32 pc=__read_reg_clkoffset(pf); u16 pb=pf->bitoff; u32 tc=__read_reg_clkoffset(tf);
 tws_slave_sniffer_cfg(tf,pf,tc,tf->bitoff,pc,pb,tws_link.link->channel_map,0);
 tws_link.last_tws_bitoff=tf->bitoff;
 u32 tm=1u<<tf->id,pm=1u<<pf->id;
 REG32(0x1c010c)|=tm|pm; REG32(0x1c0108)|=tm|pm; tf->miscntl|=512;
 if(p->esco) { tws_esco_link_monitor_slave_init(p->frame,tws_link.link->frame);
 struct ctrl_frame *f=tws_link.link->frame; tws_esco_link_monitor_slave_init(f,f); }
 else bredr_bd_frame_enable(tf,0,0);
 bredr_pwr_set(p,0,9); local_irq_enable();
}
void TWS_CODE tws_link_slave_2_master(struct link_connection *p)
{
 struct ctrl_frame *pf=p->frame,*tf=tws_link.link->frame;
 DEBUG("[Debug]: [TWS]slave2master: %d, %x\n\r\n",p->esco,tws_link_read_slot_clk());
 local_irq_disable(); connection_tx_handler(p); if(tws_link.link->state==2) bredr_bd_frame_disable(tf);
 p->monitor=0; p->tx_bulk[0]=0; p->tx_bulk[1]=0;
 pf->txbuf0cntl|=1; pf->txbuf1cntl|=1; pf->miscntl&=(u16)~1024;
 if(p->esco) { tws_esco_link_monitor_master_init(p->frame,tws_link.link->frame);
 struct ctrl_frame *f=tws_link.link->frame; tws_esco_link_monitor_master_init(f,f); }
 else tws_link_monitor_master_init(pf);
 tf->miscntl&=(u16)~512; bredr_frame_role_switch_2_master(tf); tws_link.link->role=0;
 u32 tm=1u<<tf->id,pm=1u<<pf->id;
 REG32(0x1c0108)&=~(tm|pm); REG32(0x1c010c)|=tm|pm;
 if(tws_link.link->state==2) bredr_bd_frame_enable(tf,0,0);
 local_irq_enable();
}
void TWS_CODE tws_monitor_enable(struct link_connection *p,u8 en,u8 role)
{ local_irq_disable(); p->monitor_en=en; if(!en) { INFO("[Info]: [TWS]t-role:%d\n\r\n",role);
 if(role==0) tws_link_monitor_master_init(p->frame); else tws_link_monitor_slave_init(p->frame); } local_irq_enable(); }
void TWS_CODE tws_link_cacl_bitoff(struct ctrl_frame *p,struct ctrl_frame *t,u16 *out)
{
 u16 a=p->bitoff,n=a+631; if((s16)n>1249) n=a-619;
 /* Only n is sign-extended for the forward comparison in the SDK. */
 u16 b=t->bitoff; *out=((int)b<(s16)n?(u16)(b+1250):b)-n;
}
void TWS_CODE tws_link_cacl_clkoff_for_slave(struct ctrl_frame *p,struct ctrl_frame *t,u32 *clk,u16 *bit)
{
 u16 a=p->bitoff,b=t->bitoff,delta=a>b?a-b:a-b+1250; u32 tc=__read_reg_clkoffset(t);
 preserve_bitoff_cal(p,p->bitoff,clk,bit,delta);
 if(log_tag_const_i_TWS) {
 log_print(2,0,"[Info]: [TWS]forward_bitoff=,%d,%x\n\r\n",t->bitoff,tc);
 log_print(2,0,"[Info]: [TWS]sniff_=%d,%x\n\r\n",p->bitoff,*clk);
 *clk-=tc; *bit=delta; log_print(2,0,"[Info]: [TWS]*sniffer_bitoff=%x,%d\n\r\n",*clk,delta);
 } else { *clk-=tc; *bit=delta; }
}
struct ctrl_frame *TWS_CODE tws_link_monitor_frame_init(void)
{
 struct ctrl_frame *f=bredr_bd_get_frame(); f->format=99; windset(f,103,0,0); f->bufcntl=8195;
 f->txbuf0cntl|=1; f->txbuf1cntl|=1; f->rxbuf0cntl&=(u16)~1; f->rxbuf1cntl&=(u16)~1;
 f->linkcntl0=0; f->linkcntl1=16; return f;
}
void TWS_CODE tws_link_monitor_frame_setup(struct ctrl_frame *f,struct bredr_frame_core_data *d)
{
 struct ctrl_frame *t=tws_link.link->frame; u32 pc,tc; u16 pb,tb;
 f->miscntl=d->miscntl; f->format=d->format;
 f->bdaddr0=d->bdaddr0; f->bdaddr1=d->bdaddr1; f->bdaddr2=d->bdaddr2;
 f->bchl=d->bchl; f->bchm=d->bchm; f->bchh=d->bchh;
 for(int i=0;i<8;i++) f->cryptk[i]=d->cryptk[i];
 u16 enc=(d->rx_enc<<6)|(d->tx_enc<<5); f->linkcntl0|=enc; f->linkcntl1|=enc;
 u8 *map=d->channel_map; bdhw_set_afh(f,1,map);
 pc=((u32)d->clk_offseth<<16)|d->clk_offsetl; pb=d->bitoff; tc=__read_reg_clkoffset(t); tb=t->bitoff;
 s_sniffer_offset_cal_start(t,f,&pc,&pb,tc,tb);
 INFO("[Info]: [TWS](s1:%d), %d\r\n",d->tbitoff,t->bitoff);
 tws_link.prev_pbitoff=t->bitoff; tws_forward_offset_cal(t,&tc,&tb,d->tbitoff,0);
 tws_link.frame_bitoff=tb; tws_slave_sniffer_cfg(t,f,tc,tb,pc,pb,tws_link.link->channel_map,map);
 t->miscntl&=(u16)~512;
}
void TWS_CODE tws_link_slave_start_monitor(void)
{
 struct link_connection *p=tws_link.plink; struct ctrl_frame *f=p->frame,*t=tws_link.link->frame;
 u16 old=t->bitoff; u32 tm=1u<<t->id,pm=1u<<f->id;
 REG32(0x1c010c)|=tm|pm; REG32(0x1c0108)|=tm|pm;
 __write_reg_clkoffset(t,((u32)f->twsclknoffh<<16)|f->twsclknoffl); t->miscntl|=512;
 t->bitoff=tws_link.frame_bitoff; tws_link.last_tws_bitoff=tws_link.frame_bitoff; tws_link.last_phone_bitoff=f->bitoff;
 bredr_bd_frame_enable(t,0,0); slot_timer_set(p->super_timer,link_conn_super_timeout,p,800);
 INFO("[Info]: [TWS]---c: %x, %d, %x, %d, %d\n\r\n",f->clk_offset,f->bitoff,t->clk_offset,t->bitoff,old);
 DEBUG("[Debug]: [TWS]slave_start_monitor:%x, %d\n\r\n",tws_link_read_slot_clk(),t->bitoff);
}
void TWS_CODE tws_link_master_start_monitor(void)
{
 struct ctrl_frame *p=tws_link.plink->frame,*t=tws_link.link->frame; u32 c; u16 b;
 tws_forward_offset_cal(t,&c,&b,tws_link.tbitoff,0); t->bitoff=b; __write_reg_clkoffset(t,c); bredr_bd_frame_enable(t,0,0);
 INFO("[Info]: [TWS](m2:%d)\r\n",p->bitoff); tws_link.last_phone_bitoff=p->bitoff;
}
void TWS_CODE tws_link_master_pure_monitor_init(void)
{
 struct link_connection *p=tws_link.plink,*t=tws_link.link; struct ctrl_frame *f=p->frame,*g=t->frame;
 f->miscntl|=512; f->twscon=18;
 f->twsbdaddr0=g->bdaddr0; f->twsbdaddr1=g->bdaddr1; f->twsbdaddr2=g->bdaddr2;
 f->twsbchh=g->bchh; f->twsbchm=g->bchm; f->twsbchl=g->bchl;
 f->twscnt=2580; f->twstxcnt=2710; f->twsrxcnt=3937; g->miscntl&=(u16)~512;
 REG32(0x1c0014)&=~255u; REG32(0x1c0014)|=19;
 p->pure_monitor=1; p->task.run_slot=CONFIG_PHONE_LOW_LATENCY_RUN_SLOT; t->task.run_slot=CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
}
void TWS_CODE tws_link_master_exit_pure_monitor(void)
{
 struct link_connection *p=tws_link.plink,*t=tws_link.link;
 tws_esco_link_monitor_master_resume(p->frame,t->frame); t->frame->miscntl&=(u16)~512;
 p->pure_monitor=0; p->task.run_slot=CONFIG_PHONE_RUN_SLOT; t->task.run_slot=CONFIG_TWS_RUN_SLOT; tws_link.plink_switch_mode=0;
}
void TWS_CODE tws_link_slave_pure_monitor_init(void)
{
 struct link_connection *p=tws_link.plink,*t=tws_link.link; struct ctrl_frame *f=p->frame,*g=t->frame;
 f->miscntl|=512; f->miscntl&=(u16)~8192; f->miscntl&=(u16)~1024;
 f->twsbdaddr0=g->bdaddr0; f->twsbdaddr1=g->bdaddr1; f->twsbdaddr2=g->bdaddr2;
 f->twsbchh=g->bchh; f->twsbchm=g->bchm; f->twsbchl=g->bchl;
 f->twscon=19; f->twscnt=2580; f->twstxcnt=2650; f->twsrxcnt=2716; g->miscntl&=(u16)~512;
 REG32(0x1c0014)&=~255u; REG32(0x1c0014)|=19;
 p->pure_monitor=1; p->task.run_slot=CONFIG_PHONE_LOW_LATENCY_RUN_SLOT; t->task.run_slot=CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
}
void TWS_CODE tws_link_slave_exit_pure_monitor(void)
{
 struct link_connection *p=tws_link.plink,*t=tws_link.link;
 tws_esco_link_monitor_slave_resume(p->frame,t->frame);
 p->pure_monitor=0; p->task.run_slot=CONFIG_PHONE_RUN_SLOT; t->task.run_slot=CONFIG_TWS_RUN_SLOT; tws_link.plink_switch_mode=0;
}
int TWS_CODE tws_link_get_rxtx_state(struct link_connection *p,struct tws_link_rxtx_state *d)
{
 struct ctrl_frame *f=p->frame; d->rxtog=f->rxtog; d->txtog=f->txtog; d->rxstat0=f->rxstat0; d->rxstat1=f->rxstat1; d->seqn=0;
 if(__read_reg_packet_seqn(f,0)) d->seqn|=1;
 if(__read_reg_packet_seqn(f,1)) d->seqn|=2;
 return 8;
}
int TWS_CODE tws_link_set_rxtx_state(struct link_connection *p,struct tws_link_rxtx_state *d)
{
 struct ctrl_frame *f=p->frame; f->rxtog=d->rxtog; f->txtog=d->txtog; f->rxstat0=d->rxstat0; f->rxstat1=d->rxstat1;
 f->txbuf0cntl|=1; f->txbuf1cntl|=1; __write_reg_seqn(f,0,d->seqn&1); __write_reg_seqn(f,1,d->seqn&2); return 8;
}
void TWS_CODE tws_link_disconnection(void) { link_conn_close(tws_link.link); }
void TWS_CODE tws_link_rxtx_state_reset(void) {}
void TWS_CODE tws_slave_plink_monitor_enable(void *unused)
{
 (void)unused; struct link_connection *p=tws_link.plink;
 if(p && p->state==2) { struct ctrl_frame *f=p->frame; if(!(f->miscntl&512)) {
 f->miscntl|=512; f->miscntl&=(u16)~8192; if(!tws_link.plink_switch_mode) f->miscntl|=1024; slot_timer_disable(tws_link.tx_timer); } }
}
void TWS_CODE tws_slave_plink_monitor_disable(void *unused)
{
 (void)unused; struct link_connection *p=tws_link.plink;
 if(p && tws_link.plink_switch_mode==1) { struct ctrl_frame *f=p->frame; f->miscntl|=8192;
 f->miscntl&=(u16)~512; f->miscntl&=(u16)~1024; slot_timer_set(tws_link.tx_timer,tws_slave_plink_monitor_enable,0,64); }
}
void TWS_CODE reset_tws_supertimeout(void) { if(tws_link.link) link_conn_super_timeout_reset(tws_link.link); }
int TWS_IRQ tws_link_conn_rx_packet_handler(struct link_connection *p,struct bredr_payload_head *h)
{
 struct ctrl_frame *f=p->frame; u8 index=h->index,type=h->type,llid=h->llid; u16 len=h->len; u16 check=h->check;
 if(!tws_conn.pure_monitor) tws_sch_rx_irq_handler(p,h);
 if(check!=127 || !p->tws_link) return -22;
 if(!p->role) {
 if(type==0) { if(!tws_link.plink && !link_task_schedule_active()) link_task_set_sleep_slot(16); else tws_communication_ending(); return -22; }
 } else tws_link.rx_clkn=READ_SLOT_CLK();
 switch(type) { case 0:case 1:case 2:case 3:case 6:case 7:case 9: return -22; }
 u8 *data=p->rx_data[index]; struct bredr_rx_bulk *old=p->rx_bulk;
 if(llid==2) {
 if(old) { bredr_rx_bulk_free(old); p->rx_bulk=0; DEBUG("[Debug]: [TWS]tws_bad_packet_s: %d\n\r\n",len); }
 unsigned total=((unsigned)data[1]<<8|data[0])+4;
 if(total>1023) { if(log_tag_const_e_TWS) log_print(4,0,"<Error>: [TWS]l2cap_len = %x\n\r\n",total); goto clean; }
 if(total==len) { int r=tws_core_rx_irq_handler_probe(data,len); if(!r) goto clean;
 if(r==-12) { p->rx_pend|=1u<<index; return -12; } }
 int capacity=tws_core_get_buf_len_for_packet(data); struct bredr_rx_bulk *b=bredr_rx_bulk_alloc(p->index,capacity);
 if(!b) { p->rx_pend|=1u<<index; putchar('f'); tws_core_rx_bulk_full_handler(capacity); return -12; }
 b->len=len; b->llid=2; memcpy(b->data,data,len);
 if(total>len) p->rx_bulk=b; else tws_core_rx_irq_handler_post(p,b);
 } else {
 int logged=log_tag_const_d_TWS;
 if(!old && logged) log_print(1,0,"[Debug]: [TWS]tws_bad_packet: %d, %d\n\r\n",llid,len);
 if(!old) goto clean;
 unsigned total=((unsigned)old->data[1]<<8|old->data[0])+4; u16 n=old->len;
 if((unsigned)n+len>total) {
 if(logged) log_print(1,0,"[Debug]: [TWS]drop_packet: %d, %d, %d\n\r\n",len,n,total);
 bredr_rx_bulk_free(p->rx_bulk); p->rx_bulk=0;
 } else { memcpy(old->data+n,data,len); u16 sum=n+len; old->len=sum; if(sum==total) { p->rx_bulk=0; tws_core_rx_irq_handler_post(p,old); } }
 }
clean: __clean_reg_rxfull(f,index); p->rx_pend&=~(1u<<index); return 0;
}
void TWS_CODE tws_link_conn_start(struct link_connection *p)
{ memset(&tws_link,0,sizeof(tws_link)); tws_link.link=p; tws_link.tx_timer=slot_timer_get(); tws_sch_tlink_connection(p,0); }
void TWS_CODE tws_link_detach(void)
{
 struct link_connection *p=tws_link.plink; slot_timer_put(tws_link.tx_timer); tws_sch_tws_link_disconnection();
 if(p && !p->monitor) { struct ctrl_frame *f=p->frame; f->miscntl&=(u16)~512; bredr_frame_tx_enable(f); }
 link_conn_close(tws_link.link); tws_link.link=0;
}
void TWS_CODE tws_link_set_plink(struct link_connection *p) { tws_link.plink=p; }
void TWS_CODE tws_link_plink_detach(void)
{
 struct link_connection *t=tws_link.link; slot_timer_disable(tws_link.tx_timer); tws_sch_plink_disconnection(); tws_link.plink=0;
 if(t && t->role) { t->frame->miscntl&=(u16)~512; link_task_run(&t->task); }
}
struct link_connection *TWS_CODE tws_link_get_plink(void)
{
 struct link_connection *p=get_link_connection(); SDK_ASSERT(p,1304,"link != NULL");
 tws_link.plink=p; link_connection_init(p); p->monitor=1; p->role=1; p->frame=tws_link_monitor_frame_init();
 p->last_signed_bitoff=0; p->task.run_slot=64; bredr_pwr_set(p,0,15);
 for(int i=0;i<2;i++) link_conn_set_rx_bulk_in_irq(p,i);
 bredr_frame_set_irq_handler(p->frame,p,bredr_frame_conn_rx_irq_handler); return p;
}
int TWS_CODE tws_link_master_get_plink_core_data(void *buf)
{
 struct bredr_frame_core_data *d=(void *)((u8 *)buf+16); struct link_connection *p=tws_link.plink;
 struct ctrl_frame *f=p->frame,*t=tws_link.link->frame; u16 bit; u32 clk;
 memcpy(buf,&p->core,16); tws_link_cacl_bitoff(f,t,&tws_link.tbitoff); tws_link_cacl_clkoff_for_slave(f,t,&clk,&bit);
 d->miscntl=f->miscntl; d->format=f->format;
 d->bitoff=bit; d->clk_offsetl=clk; d->clk_offseth=clk>>16;
 d->bdaddr0=f->bdaddr0; d->bdaddr1=f->bdaddr1; d->bdaddr2=f->bdaddr2;
 d->bchl=f->bchl; d->bchm=f->bchm; d->bchh=f->bchh;
 d->rx_enc=(f->linkcntl0>>6)&1; d->tx_enc=(f->linkcntl0>>5)&1;
 u16 tb=tws_link.tbitoff; d->tbitoff=tb;
 memcpy(d->cryptk,(const void *)f->cryptk,16); memcpy(d->channel_map,tws_link.plink->channel_map,10);
 INFO("[Info]: [TWS](m1:%d), %d\r\n",tb,f->bitoff); tws_link.prev_pbitoff=f->bitoff; return 68;
}
int TWS_CODE tws_link_slave_set_plink_core_data(void *buf)
{
 struct link_connection *p=tws_link.plink; memcpy(&p->core,__builtin_assume_aligned(buf,4),16);
 tws_link_monitor_frame_setup(p->frame,&((struct tws_link_core_data *)buf)->frame_core); return 68;
}
void TWS_CODE tws_slave_rx_switch_conn_irq_handler(struct ctrl_frame *f)
{
 struct link_connection *p=tws_link.plink; if(!p || !tws_conn.monitor_start) return;
 struct link_connection *t=tws_link.link; if(t->sniff_sta) return; slave_rx_switch_irq=1;
 struct ctrl_frame *other=t->frame;
 if(other==f) { if(!tws_conn.role) { tws_conn.role=1; tws_conn.role_switch=0; tws_conn.tx_tick_res=1; t->adjust_clkoffset_enable=1; p=tws_link.plink; }
 other=p->frame; link_task_run_slots(&p->task,CONFIG_PHONE_RUN_SLOT); }
 else link_task_run_slots(&t->task,CONFIG_TWS_RUN_SLOT);
 u32 c=f->clk_offset; other->twsclknoffl=c; other->twsclknoffh=c>>16;
 u16 con=other->twscon; other->twscon=(con & (u16)~96)|((clkn0_phase(f->bitoff)<<5)-32);
}
void TWS_CODE tws_slave_plink_premaxval_settings(struct link_connection *p)
{
 if(!tws_link.plink || tws_conn.monitor_start!=2 || tws_conn.pure_monitor || tws_conn.role!=1) return;
 struct ctrl_frame *f=p->frame; u16 rx=f->rxbit,b=f->bitoff;
 int edge=((unsigned)(b-1212)<9) | ((unsigned)(b-899)<9) | ((unsigned)(b-274)<9) | ((unsigned)(b-587)<9);
 if(!p->tws_link) {
 u32 reg=REG32(0x1c0008)&~4095u;
 if((u16)(rx-253)<9 || (u16)(rx-565)<9 || (u16)(rx-1190)<9 || (u16)(rx-878)<9) {
 REG32(0x1c0008)=reg|178; windset(f,113,0,0); wind_align_set(edge?93:103);
 } else { REG32(0x1c0008)=reg|188; windset(f,103,0,0); wind_align_set(edge?113:103); }
 } else if(edge) wind_align_set(113); else windset(f,103,0,0);
}
static void tws_slave_tlink_monitor_enable(void *unused)
{ (void)unused; struct link_connection *t=tws_link.link; if(t) t->frame->miscntl|=512; }
static void phone_link_ack_enable(void *unused)
{
 (void)unused; if(tws_link.plink) { SDK_ASSERT(tws_link.link,1385,"tws_link.link");
 bredr_frame_tx_enable(tws_link.plink->frame);
 if(!tws_conn.role_switch && tws_link.plink_switch_mode) slot_timer_set(tws_link.tx_timer,tws_master_plink_monitor_disable,0,20); }
}
static void tws_master_plink_monitor_disable(void *unused)
{ (void)unused; struct link_connection *p=tws_link.plink; if(p) p->frame->miscntl&=(u16)~512; }

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

static int tws_link_tx_sync_req(struct lmp_connection *conn, u8 *txbuffer,
                                int prev_req_err) TWS_IRQ;
static int tws_link_tx_sync_req(struct lmp_connection *conn, u8 *txbuffer,
                                int prev_req_err)
{
    struct link_connection *plink = tws_link.plink;
    struct ctrl_frame *frame = plink->frame;
    tws_wire_word *wire = (tws_wire_word *)txbuffer;
    u32 bits = (frame->miscntl >> 3 & 1) | (wire[0] & ~1u);
    wire[0] = bits;
    bits = ((frame->linkcntl0 >> 4) & 4) | (bits & ~4u);
    wire[0] = bits;
    bits = ((frame->linkcntl0 >> 2) & 8) | (bits & ~8u);
    wire[0] = bits;
    bits = ((frame->chused >> 6) & 2) | (bits & ~2u);
    wire[0] = bits;

    *(u32 *)(txbuffer + 32) = plink->afh_slot_end;
    int offset = bredr_clkn2offset(plink->super_timeout_begin,
                                   bredr_read_slot_clk());
    *(u16 *)(txbuffer + 14) = (u16)(offset / 800);
    memcpy(__builtin_assume_aligned(txbuffer + 4,4), __builtin_assume_aligned(plink->channel_map,4), 10);
    memcpy(txbuffer + 16, (const void *)frame->cryptk, 16);

    bits = wire[0] & 15;
    wire[0] = bits;
    u8 switch_mode = tws_link.plink_switch_mode;
    if (plink->rx_state == 71) {
        if (prev_req_err != 0 || switch_mode == 0) {
            wire[0] = bits | 48;
            tws_link.plink_switch_mode = 1;
        }
    } else if (prev_req_err != 0 || switch_mode == 1) {
        wire[0] = bits | 32;
        tws_link.plink_switch_mode = 0;
    }

    if (conn->esco_conn != NULL) {
        if (prev_req_err == 0 && tws_link.esco_tx_sync_req_cnt != 0) {
            u8 count = --tws_link.esco_tx_sync_req_cnt;
            if (count % 5 == 0) putchar(90);
            return count % 5 == 0 ? 36 : 0;
        }
        return 0;
    }

    u32 hash = JBHash(txbuffer, 36);
    if (tws_link.core_hash != (int)hash || tws_link.tx_req_cnt > 29) {
        tws_link.core_hash = hash;
        tws_link.wait_tx_suss = 1;
        tws_link.tx_req_cnt = 0;
        return 36;
    }
    tws_link.tx_req_cnt++;
    if (tws_link.wait_tx_suss != 0) {
        if (prev_req_err != 0) return 36;
        tws_link.wait_tx_suss = 0;
    }
    return 0;
}

static int tws_link_rx_sync_req(struct lmp_connection *conn, u8 *rxbuffer,
                                int len, u8 *txbuffer) TWS_CODE;
static int tws_link_rx_sync_req(struct lmp_connection *conn, u8 *rxbuffer,
                                int len, u8 *txbuffer)
{
    (void)conn;
    (void)len;
    (void)txbuffer;
    struct link_connection *plink = tws_link.plink;
    if (plink == NULL) return 0;
    struct ctrl_frame *frame = plink->frame;
    tws_wire_word *wire = (tws_wire_word *)rxbuffer;
    link_conn_set_ptt(plink, (u8)wire[0] & 1);
    u32 bits = wire[0];
    link_conn_set_encrypt(plink, (bits >> 2) & 1, (bits >> 3) & 1);
    memcpy((void *)frame->cryptk, __builtin_assume_aligned(rxbuffer + 16,2), 16);
    bits = wire[0] >> 4;
    if ((bits & 2) != 0) tws_link.plink_switch_mode = (u8)bits & 1;

    u16 timeout = *(u16 *)(rxbuffer + 14);
    tws_link.master_super_timeout_cnt = (u8)timeout;
    u32 now = bredr_read_slot_clk();
    u32 timeout_ticks = (u32)*(u16 *)(rxbuffer + 14) * 800;
    if ((s32)now >= (s32)timeout_ticks) {
        u32 begin = now - timeout_ticks;
        if (bredr_clkn_after(begin, plink->super_timeout_begin))
            plink->super_timeout_begin = begin;
    }

    u32 afh_end = *(u32 *)(rxbuffer + 32);
    if (bredr_clkn_after(afh_end, plink->afh_slot_end) ||
        plink->afh_slot_end == 0) {
        bits = wire[0];
        bredr_link_set_afh(plink, *(u32 *)(rxbuffer + 32), (bits >> 1) & 1, rxbuffer + 4);
    }
    return 0;
}

static int tws_link_connection_resume(struct link_task *task) TWS_IRQ;
static int tws_link_connection_resume(struct link_task *task)
{
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    struct link_connection *link = tws_link.link;
    if (link == NULL) return 0;

    if (lmp != NULL && tws_conn.monitor_start == 2) {
        struct link_connection *plink = tws_link.plink;
        if (plink->esco == NULL && (tws_sniff.state & 1) == 0) {
            u16 phone_bitoff = plink->frame->bitoff;
            struct ctrl_frame *frame = link->frame;
            if (tws_conn.esco_super_timeout == 0 &&
                tws_link.last_phone_bitoff != phone_bitoff) {
                tws_link.last_phone_bitoff = phone_bitoff;
                if (link->role) {
                    if (tws_link.master_super_timeout_cnt == 0) {
                        u16 old_bitoff = frame->bitoff;
                        m_tws_offset_cal(frame, phone_bitoff, 2);
                        u16 new_bitoff = frame->bitoff;
                        u16 delta = old_bitoff > new_bitoff
                                  ? old_bitoff - new_bitoff
                                  : new_bitoff - old_bitoff;
                        if (delta > 625) delta = (u16)(1250 - delta);
                        if (delta > 7) {
                            if (log_tag_const_d_TWS)
                                log_print(1, NULL,
                                          "[Debug]: [TWS]bitoff: %d, %d\n\r\n",
                                          old_bitoff, new_bitoff);
                            windset(frame, 133, 0, 0);
                            new_bitoff = frame->bitoff;
                        }
                        tws_link.last_tws_bitoff = new_bitoff;
                    } else if (log_tag_const_v_TWS) {
                        log_print(0, NULL, "[Verb]: [TWS]super_tmo: %d\n\r\n",
                                  tws_link.master_super_timeout_cnt);
                    }
                } else {
                    m_tws_offset_cal(frame, phone_bitoff, 0);
                }
            }

            link = tws_link.link;
            if (!link->role) {
                int offset = bredr_clkn2offset(link->super_timeout_begin,
                                               READ_SLOT_CLK());
                if (offset > 499) task->run_slot = 80;
            } else if (tws_link.plink_switch_mode != 0) {
                link->frame->miscntl &= (u16)~512;
                slot_timer_set(tws_link.tx_timer, tws_slave_tlink_monitor_enable,
                               NULL, 8);
            }
        }

        tws_sniff_ap_enter();
        struct link_connection *esco_plink = tws_link.plink;
        if (esco_plink->esco != NULL &&
            (u8)(lmp->core.esco_state - 1) > 2 && tws_conn.esco_status == 0) {
            struct list_head *head = &tws_conn.rx_pending;
            struct list_head *entry = head->next;
            if (entry != head) {
                u8 log_enabled = log_tag_const_i_TWS;
                do {
                    struct list_head *next = entry->next;
                    if (log_enabled)
                        log_print(2, NULL,
                                  "[Info]: [TWS]<<<<<<<<<<<esco_tws_conn.rx_pending_waring\r\n");
                    __list_del_entry(entry);
                    list_add_tail(entry, &tws_link.plink->rx_head);
                    entry = next;
                } while (entry != head);
                bredr_link_event(tws_link.plink, 1, 5);
            }
            tws_conn.esco_status = 1;
            if (tws_conn.role == 0) {
                tws_link.esco_tx_sync_req_cnt = 20;
                tws_monitor_enable(tws_link.plink, 1, 0);
            } else {
                tws_monitor_enable(tws_link.plink, 1, 1);
            }
            tws_data_sync_clear_abandon_num();
        }
    }
    tws_core_tws_link_resume();
    link_conn_task_resume(task);
    return 0;
}

static int tws_link_connection_suspend(struct link_task *task) TWS_IRQ;
static int tws_link_connection_suspend(struct link_task *task)
{
    struct lmp_connection *lmp = tws_conn.lmp_conn;
    link_conn_task_suspend(task);
    tws_core_tws_link_suspend();

    if (lmp != NULL && tws_conn.monitor_start == 2) {
        if (tws_conn.role == 0) {
            struct link_connection *link = tws_link.link;
            if (link->role) {
                struct ctrl_frame *frame = link->frame;
                tws_link_slave_2_master(tws_link.plink);
                frame->bitoff = tws_link.bk_bitoff;
                __write_reg_clkoffset(frame, tws_link.bk_clk_offset);
                tws_conn.role_switch = 1;
                if (log_tag_const_d_TWS)
                    log_print(1, NULL, "[Debug]: [TWS]rs_faild\n\r\n");
            } else if (tws_conn.role_switch == 3 && !is_tws_esco_working()) {
                tws_conn.role_switch = 1;
                if (log_tag_const_d_TWS)
                    log_print(1, NULL, "[Debug]: [TWS]rs_faild\n\r\n");
            }
        } else if (tws_conn.role_switch == 6) {
            tws_conn.role_switch = 0;
            tws_conn.rs_instant = 0;
            if (log_tag_const_d_TWS)
                log_print(1, NULL, "[Debug]: [TWS]rs_faild\n\r\n");
        }

        struct link_connection *link = tws_link.link;
        if (tws_conn.esco_super_timeout == 0 &&
            link->role &&
            link->esco == NULL && lmp->esco_conn == NULL &&
            (tws_sniff.state & 1) == 0) {
            u16 phone_bitoff = link->frame->bitoff;
            if (tws_link.last_tws_bitoff != phone_bitoff) {
                tws_link.last_tws_bitoff = phone_bitoff;
                s_sniffer_offset_cal(tws_link.plink->frame, phone_bitoff);
                tws_link.last_phone_bitoff = tws_link.plink->frame->bitoff;
            }
            if (tws_conn.pure_monitor == 0) {
                if (tws_link.plink->rx_state == 66)
                    tws_link.plink_switch_mode = 0;
                tws_slave_plink_monitor_disable(NULL);
                tws_link.link->frame->miscntl |= 512;
            }
        }

        if (tws_conn.role == 0) {
            if (lmp->esco_conn != NULL || lmp->sniff != NULL) {
                link_conn_qos_disable(tws_link.plink);
                tws_master_start_monitor(NULL);
            } else if (tws_conn.pure_monitor == 0) {
                bredr_frame_tx_disable(tws_link.plink->frame);
                slot_timer_set(tws_link.tx_timer, phone_link_ack_enable, NULL, 8);
                tws_link.plink->frame->miscntl |= 512;
                tws_master_start_monitor(NULL);
            }
        } else {
            tws_conn_slave_start_monitor(NULL);
        }
        tws_link.suspend_clkn = tws_link_read_slot_clk();
    }
    tws_sniff_ap_exit();
    return 0;
}

#ifdef UNITTEST
#include <stdlib.h>
#include <stdarg.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
u8 log_tag_const_i_TWS,log_tag_const_d_TWS,log_tag_const_e_TWS,log_tag_const_v_TWS;
int CONFIG_PHONE_RUN_SLOT=100,CONFIG_TWS_RUN_SLOT=60;
int CONFIG_PHONE_LOW_LATENCY_RUN_SLOT=8,CONFIG_TWS_LOW_LATENCY_RUN_SLOT=12;
int config_asser;
struct tws_connection tws_conn;
struct lmp_tws_sniff tws_sniff;
static struct link_connection phone,peer,replacement;
static struct ctrl_frame phone_frame,peer_frame,other_frame;
static struct lmp_connection lmp;
static int irq,logs,cleaned,posted,freed,probe_result,alloc_fail,full,sleep_slots,ending;
static int timer_ticks,disabled,tx_calls,enable_calls,disable_calls,core_resume,core_suspend,task_resume,task_suspend;
static int scheduled,monitor_calls,events,mutate,align_value,ptt,rx_enc,tx_enc;
static u32 clock_value=2400,hash_value=99;
static void (*timer_fn)(void *);
static struct bredr_rx_bulk *last_post;
static u8 *sync_buffer;
void local_irq_disable(void) { ++irq; }
void local_irq_enable(void) { --irq; }
void cpu_assert_debug(void) { abort(); }
void p33_soft_reset(void) { abort(); }
void log_print(int level,const char *tag,const char *fmt,...)
{
 (void)level;(void)tag;++logs;
 if(mutate==1 && strstr(fmt,"(m2:")) phone_frame.bitoff=919;
 if(mutate==2 && strstr(fmt,"rx_pending_waring")) { tws_link.plink=&replacement; log_tag_const_i_TWS=0; }
}
u32 bredr_get_master_slot_clk(struct link_connection *p) { CHECK(p==&peer); return clock_value; }
u8 clkn0_phase(s16 phase) { return phase>=625?2:1; }
void link_conn_qos_disable(struct link_connection *p) { CHECK(p==&phone); }
void bredr_frame_role_switch_2_slave(struct ctrl_frame *f) { CHECK(f==&peer_frame); }
void bredr_frame_role_switch_2_master(struct ctrl_frame *f) { CHECK(f==&peer_frame); }
s32 __read_reg_clkoffset(struct ctrl_frame *f) { return f->clk_offset; }
void __write_reg_clkoffset(struct ctrl_frame *f,u32 c) { f->clk_offset=c; }
void bredr_bd_frame_enable(struct ctrl_frame *f,u8 a,u8 b) { CHECK(f==&peer_frame && !a && !b); ++enable_calls; }
void bredr_bd_frame_disable(struct ctrl_frame *f) { CHECK(f==&peer_frame); ++disable_calls; }
int bredr_pwr_set(struct link_connection *p,s8 a,s8 b) { CHECK(p==&phone && !a && (b==9||b==15)); return 0; }
void connection_tx_handler(void *p) { CHECK(p==&phone); ++tx_calls; }
void preserve_bitoff_cal(struct ctrl_frame *f,u32 b,u32 *c,u16 *o,u16 delta)
{ CHECK(f==&phone_frame && b==f->bitoff); *c=0x12345678;*o=delta; }
struct ctrl_frame *bredr_bd_get_frame(void) { return &phone_frame; }
void windset(struct ctrl_frame *f,u16 a,u16 b,u8 c) { (void)f; CHECK(!b&&!c); align_value=a; }
void wind_align_set(int n) { align_value=n; }
void bdhw_set_afh(struct ctrl_frame *f,u8 en,u8 *map) { (void)f;CHECK(en==1||en==2); CHECK(map); }
void s_sniffer_offset_cal_start(struct ctrl_frame *t,struct ctrl_frame *p,u32 *c,u16 *b,u32 tc,u16 tb)
{ CHECK(t==&peer_frame && p==&phone_frame); (void)c;(void)b;(void)tc;(void)tb; }
void tws_forward_offset_cal(struct ctrl_frame *f,u32 *c,u16 *b,u16 sn,u8 mode)
{ CHECK(f==&peer_frame && !mode); *c=0xabcdef01; *b=sn+10; }
void slot_timer_set(u8 id,void (*fn)(void *),void *arg,int ticks)
{ (void)id;(void)arg;timer_fn=fn;timer_ticks=ticks; }
void slot_timer_disable(u8 id) { (void)id;++disabled; }
void link_conn_super_timeout(void *p) { (void)p; }
u16 __read_reg_packet_seqn(struct ctrl_frame *f,int index) { CHECK(f==&phone_frame); return index==1; }
static u8 written_seqn[2];
void __write_reg_seqn(struct ctrl_frame *f,int index,u8 n) { CHECK(f==&phone_frame);written_seqn[index]=n; }
void link_conn_close(struct link_connection *p) { CHECK(p==&peer); }
void link_conn_super_timeout_reset(struct link_connection *p) { CHECK(p==&peer); }
void tws_sch_rx_irq_handler(struct link_connection *p,struct bredr_payload_head *h)
{ CHECK(p==&peer);++scheduled; if(mutate==3) { h->type=0; h->len=0; } }
int link_task_schedule_active(void) { return 0; }
void link_task_set_sleep_slot(int n) { sleep_slots=n; }
void tws_communication_ending(void) { ++ending; }
u32 READ_SLOT_CLK(void) { return clock_value; }
u32 bredr_read_slot_clk(void)
{ if(mutate==4) *(u16 *)(sync_buffer+14)=2; return clock_value; }
int bredr_clkn2offset(int a,int b) { return a-b; }
int bredr_clkn_after(int a,int b) { if(mutate==5) { *(u32 *)(sync_buffer+32)=777; mutate=0; } return a>b; }
void bredr_rx_bulk_free(struct bredr_rx_bulk *b) { ++freed; free(b); }
int tws_core_rx_irq_handler_probe(u8 *data,int n) { CHECK(data && n>0);return probe_result; }
int tws_core_get_buf_len_for_packet(u8 *data) { (void)data;return 1024; }
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id,int n) { CHECK(id==7);return alloc_fail?NULL:calloc(1,sizeof(struct bredr_rx_bulk)+n); }
int tws_core_rx_bulk_full_handler(int n) { CHECK(n==1024);++full;return 0; }
void tws_core_rx_irq_handler_post(struct link_connection *p,struct bredr_rx_bulk *b) { CHECK(p==&peer);++posted;last_post=b; }
void __clean_reg_rxfull(struct ctrl_frame *f,int index) { CHECK(f==&peer_frame && index==1);++cleaned; }
u8 slot_timer_get(void) { return 9; }
void slot_timer_put(u8 n) { CHECK(n==9); }
void tws_sch_tlink_connection(struct link_connection *p,int role) { CHECK(p==&peer && !role); }
void tws_sch_tws_link_disconnection(void) {}
void tws_sch_plink_disconnection(void) {}
void link_task_run(struct link_task *task) { CHECK(task==&peer.task); }
struct link_connection *get_link_connection(void) { return &phone; }
void link_connection_init(struct link_connection *p) { CHECK(p==&phone); }
int link_conn_set_rx_bulk_in_irq(struct link_connection *p,int index) { CHECK(p==&phone && index>=0 && index<2);return 0; }
void bredr_frame_set_irq_handler(struct ctrl_frame *f,void *p,void (*fn)(void *)) { CHECK(f==&phone_frame && p==&phone && fn==bredr_frame_conn_rx_irq_handler); }
void bredr_frame_conn_rx_irq_handler(void *p) { (void)p; }
void link_task_run_slots(struct link_task *p,int slots) { p->run_slot=slots; }
u32 JBHash(u8 *p,int n) { CHECK(p && n==36); return hash_value; }
void link_conn_set_ptt(struct link_connection *p,u8 en) { CHECK(p==&phone);ptt=en; }
void link_conn_set_encrypt(struct link_connection *p,u8 rx,u8 tx) { CHECK(p==&phone);rx_enc=rx;tx_enc=tx; }
void bredr_link_set_afh(struct link_connection *p,u32 end,u8 en,u8 *map)
{ CHECK(p==&phone && en==1 && map==sync_buffer+4);p->afh_slot_end=end; }
void m_tws_offset_cal(struct ctrl_frame *f,u16 b,u8 mode) { CHECK(f==&peer_frame && (mode==0||mode==2)); f->bitoff=b; if(mutate==6) tws_link.link=&replacement; }
void s_sniffer_offset_cal(struct ctrl_frame *f,u16 b) { CHECK(f==&phone_frame);f->bitoff=b; }
void tws_sniff_ap_enter(void) {}
void tws_sniff_ap_exit(void) {}
int bredr_link_event(void *p,int argc,...) { CHECK(p==tws_link.plink && argc==1);++events;return 0; }
void tws_data_sync_clear_abandon_num(void) {}
void tws_core_tws_link_resume(void) { ++core_resume; }
void tws_core_tws_link_suspend(void) { ++core_suspend; }
int link_conn_task_resume(struct link_task *p) { CHECK(p==&peer.task);++task_resume;return 0; }
int link_conn_task_suspend(struct link_task *p) { CHECK(p==&peer.task);++task_suspend;return 0; }
int is_tws_esco_working(void) { return 0; }
void tws_master_start_monitor(void *p) { CHECK(!p);++monitor_calls; }
void tws_conn_slave_start_monitor(void *p) { CHECK(!p);++monitor_calls; }
static void fixture(void)
{
 memset(&tws_link,0,sizeof(tws_link));memset(&phone,0,sizeof(phone));memset(&peer,0,sizeof(peer));memset(&replacement,0,sizeof(replacement));
 memset(&phone_frame,0,sizeof(phone_frame));memset(&peer_frame,0,sizeof(peer_frame));memset(&other_frame,0,sizeof(other_frame));
 memset(&lmp,0,sizeof(lmp));memset(&tws_conn,0,sizeof(tws_conn));memset(&tws_sniff,0,sizeof(tws_sniff));memset(tws_test_regs,0,sizeof(tws_test_regs));
 phone.frame=&phone_frame;peer.frame=&peer_frame;replacement.frame=&other_frame;
 tws_link.plink=&phone;tws_link.link=&peer;tws_link.tx_timer=9;
 phone.rx_head.next=phone.rx_head.prev=&phone.rx_head;replacement.rx_head.next=replacement.rx_head.prev=&replacement.rx_head;
 tws_conn.rx_pending.next=tws_conn.rx_pending.prev=&tws_conn.rx_pending;
 phone.state=peer.state=2;peer.index=7;peer.tws_link=1;peer_frame.id=2;phone_frame.id=3;
 irq=logs=cleaned=posted=freed=alloc_fail=full=sleep_slots=ending=timer_ticks=disabled=tx_calls=enable_calls=disable_calls=0;
 core_resume=core_suspend=task_resume=task_suspend=scheduled=monitor_calls=events=mutate=align_value=0; probe_result=1;
 timer_fn=0;last_post=0;clock_value=2400;hash_value=99;
 log_tag_const_i_TWS=log_tag_const_d_TWS=log_tag_const_e_TWS=log_tag_const_v_TWS=0;
}
int main(void)
{
 /* The wrapping addition is signed, but the other bitoff is zero-extended
  * before the SDK's signed comparison. Cover every input halfword. */
 fixture();
 for(unsigned a=0;a<65536;a++) for(unsigned j=0;j<6;j++) {
 static const u16 values[]={0,624,1249,32767,32768,65535};
 phone_frame.bitoff=a;peer_frame.bitoff=values[j];u16 actual;
 tws_link_cacl_bitoff(&phone_frame,&peer_frame,&actual);
 int n=(s16)(u16)(a+631);if(n>1249)n=(s16)(u16)(a-619);
 u16 expected=(values[j]<n?values[j]+1250:values[j])-n;CHECK(actual==expected);
 }
 fixture();phone_frame.bdaddr0=123;phone_frame.bchl=987;
 tws_monitor_enable(&phone,0,0);CHECK(phone.monitor_en==0 && phone_frame.twsbdaddr0==123 && phone_frame.twsrxcnt==2560 && irq==0);
 phone_frame.miscntl=1536;peer_frame.miscntl=512;tws_monitor_enable(&phone,0,1);CHECK(!phone_frame.miscntl && !peer_frame.miscntl);
 tws_monitor_enable(&phone,1,0);CHECK(phone.monitor_en==1 && !phone_frame.miscntl);
 tws_esco_link_monitor_master_init(&phone_frame,&peer_frame);CHECK(phone_frame.twscon==18 && phone_frame.twstxcnt==2710 && REG32(0x1c0014)==19);
 tws_esco_link_monitor_slave_init(&phone_frame,&peer_frame);CHECK(phone_frame.twscon==19 && phone_frame.twsrxcnt==2716);
 tws_link_master_pure_monitor_init();CHECK(phone.pure_monitor && phone.task.run_slot==8 && peer.task.run_slot==12 && phone_frame.twsrxcnt==3937);
 tws_link_master_exit_pure_monitor();CHECK(!phone.pure_monitor && phone.task.run_slot==100);
 tws_link_slave_pure_monitor_init();CHECK(phone_frame.twstxcnt==2650);tws_link_slave_exit_pure_monitor();CHECK(!phone.pure_monitor);
 fixture();tws_link_master_2_slave(&phone);CHECK(phone.monitor && peer.role && enable_calls==1 && REG32(0x1c0108)==12 && irq==0);
 tws_link_slave_2_master(&phone);CHECK(!phone.monitor && !peer.role && tx_calls==1 && disable_calls==1 && enable_calls==2 && !REG32(0x1c0108));
 fixture();struct tws_link_rxtx_state state={0};phone_frame.rxtog=0x123;phone_frame.txtog=0x234;phone_frame.rxstat0=77;
 CHECK(tws_link_get_rxtx_state(&phone,&state)==8 && state.rxtog==0x23 && state.txtog==0x34 && state.seqn==2);
 state.seqn=3;CHECK(tws_link_set_rxtx_state(&phone,&state)==8 && written_seqn[0]==1 && written_seqn[1]==2);
 tws_link.plink_switch_mode=1;tws_slave_plink_monitor_disable(0);CHECK(timer_ticks==64 && timer_fn==tws_slave_plink_monitor_enable && (phone_frame.miscntl&8192));
 timer_fn(0);CHECK((phone_frame.miscntl&512) && !(phone_frame.miscntl&8192) && disabled==1);
 fixture();u32 wire[17];memset(wire,0xa5,sizeof(wire));sync_buffer=(u8 *)wire;
 phone_frame.miscntl=8;phone_frame.linkcntl0=96;phone_frame.chused=128;phone.super_timeout_begin=clock_value+2400;phone.rx_state=71;
 CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,0)==36 && wire[0]==63 && *(u16 *)(sync_buffer+14)==3 && tws_link.wait_tx_suss);
 CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,1)==36 && tws_link.tx_req_cnt==1);
 CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,0)==0 && !tws_link.wait_tx_suss);
 tws_link.tx_req_cnt=30;CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,0)==36);
 lmp.esco_conn=(void *)1;tws_link.esco_tx_sync_req_cnt=6;CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,0)==36 && tws_link.esco_tx_sync_req_cnt==5);
 CHECK(tws_link_sync.tx_sync_req(&lmp,sync_buffer,1)==0 && tws_link.esco_tx_sync_req_cnt==5);
 fixture();memset(wire,0,sizeof(wire));sync_buffer=(u8 *)wire;wire[0]=63;*(u16 *)(sync_buffer+14)=257;wire[8]=20;mutate=4;
 CHECK(tws_link_sync.rx_sync_req_and_res(0,sync_buffer,0,0)==0);CHECK(tws_link.master_super_timeout_cnt==1 && phone.super_timeout_begin==800 && phone.afh_slot_end==20 && ptt && rx_enc && tx_enc);
 wire[8]=30;mutate=5;CHECK(!tws_link_sync.rx_sync_req_and_res(0,sync_buffer,1,0));CHECK(phone.afh_slot_end==777);
 fixture();u8 data[16]={4,0,0,0,1,2,3,4};peer.rx_data[1]=data;peer.role=1;
 struct bredr_payload_head h={0};h.index=1;h.type=4;h.llid=2;h.len=8;h.check=127;
 h.check=383;CHECK(tws_link_conn_rx_packet_handler(&peer,&h)==-22 && !posted && !cleaned); h.check=127;scheduled=0;
 mutate=3;CHECK(!tws_link_conn_rx_packet_handler(&peer,&h) && posted==1 && cleaned==1 && scheduled==1 && last_post->len==8);bredr_rx_bulk_free(last_post);
 h.type=4;h.len=4;probe_result=1;mutate=0;CHECK(!tws_link_conn_rx_packet_handler(&peer,&h) && peer.rx_bulk && peer.rx_bulk->len==4);
 h.llid=1;CHECK(!tws_link_conn_rx_packet_handler(&peer,&h) && !peer.rx_bulk && posted==2);bredr_rx_bulk_free(last_post);
 h.llid=2;h.len=8;probe_result=-12;int count=cleaned;CHECK(tws_link_conn_rx_packet_handler(&peer,&h)==-12 && cleaned==count && (peer.rx_pend&2));
 probe_result=1;alloc_fail=1;CHECK(tws_link_conn_rx_packet_handler(&peer,&h)==-12 && full==1 && cleaned==count);
 alloc_fail=0;data[0]=255;data[1]=255;CHECK(!tws_link_conn_rx_packet_handler(&peer,&h) && cleaned==count+1);
 fixture();CHECK(tws_link_task_ops.resume(&peer.task)==0 && core_resume==1 && task_resume==1);
 tws_link.link=0;CHECK(!tws_link_task_ops.resume(&peer.task) && core_resume==1);
 fixture();tws_conn.lmp_conn=&lmp;tws_conn.monitor_start=2;peer.role=1;phone_frame.bitoff=100;peer_frame.bitoff=0;
 CHECK(!tws_link_task_ops.resume(&peer.task) && tws_link.last_tws_bitoff==100 && align_value==133);
 /* Cached next/list-log flag, reloaded destination after logging. */
 fixture();tws_conn.lmp_conn=&lmp;tws_conn.monitor_start=2;phone.esco=(void *)1;struct list_head nodes[2];
 nodes[0].prev=&tws_conn.rx_pending;nodes[0].next=&nodes[1];nodes[1].prev=&nodes[0];nodes[1].next=&tws_conn.rx_pending;
 tws_conn.rx_pending.next=&nodes[0];tws_conn.rx_pending.prev=&nodes[1];log_tag_const_i_TWS=1;mutate=2;
 CHECK(!tws_link_task_ops.resume(&peer.task));CHECK(logs==2 && replacement.rx_head.next==&nodes[0] && replacement.rx_head.prev==&nodes[1] && events==1 && replacement.monitor_en==1);
 fixture();tws_conn.lmp_conn=&lmp;tws_conn.monitor_start=2;
 CHECK(!tws_link_task_ops.suspend(&peer.task) && core_suspend==1 && task_suspend==1 && monitor_calls==1 && timer_ticks==8 && timer_fn==phone_link_ack_enable);
 timer_fn(0);CHECK(!(phone_frame.miscntl&8192));
 fixture();tws_link.tbitoff=70;log_tag_const_i_TWS=1;mutate=1;tws_link_master_start_monitor();CHECK(tws_link.last_phone_bitoff==919 && peer_frame.bitoff==80);
 fixture();u32 core_words[18];memset(core_words,0xa5,sizeof(core_words));
 phone_frame.bitoff=100;peer_frame.bitoff=200;peer_frame.clk_offset=0x123;
 phone_frame.miscntl=0x1234;phone_frame.format=0x4567;phone_frame.linkcntl0=96;
 phone_frame.bdaddr0=0x55aa;phone_frame.bchl=0x3456;phone_frame.cryptk[0]=0xbeef;
 CHECK(tws_link_master_get_plink_core_data(core_words)==68);
 struct tws_link_core_data *record=(void *)core_words;
 CHECK(record->frame_core.miscntl==0x34 && record->frame_core.format==0x67 && record->frame_core.bdaddr0==0x55aa);
 CHECK(record->frame_core.bitoff==1150 && record->frame_core.clk_offsetl==(u16)(0x12345678-0x123) && record->frame_core.cryptk[0]==0xbeef);
 CHECK(record->frame_core.rx_enc==1 && record->frame_core.tx_enc==1 && tws_link.tbitoff==719 && core_words[17]==0xa5a5a5a5);
 CHECK(tws_link_slave_set_plink_core_data(core_words)==68 && phone_frame.miscntl==0x234 && phone_frame.format==0x67 && phone_frame.cryptk[0]==0xbeef && tws_link.frame_bitoff==729);
 fixture();tws_conn.monitor_start=2;tws_conn.role=1;phone_frame.bitoff=274;phone_frame.rxbit=253;
 tws_slave_plink_premaxval_settings(&phone);CHECK(REG32(0x1c0008)==178 && align_value==93);
 phone_frame.rxbit=0;tws_slave_plink_premaxval_settings(&phone);CHECK(REG32(0x1c0008)==188 && align_value==113);
 fixture();tws_conn.monitor_start=1;peer_frame.clk_offset=0x12345678;peer_frame.bitoff=700;
 tws_slave_rx_switch_conn_irq_handler(&peer_frame);CHECK(tws_conn.role==1 && peer.adjust_clkoffset_enable && phone_frame.twsclknoffl==0x5678 && phone_frame.twsclknoffh==0x1234 && phone.task.run_slot==100);
 fixture();CHECK(tws_link_get_plink()==&phone && phone.monitor && phone.role && phone_frame.format==99 && phone_frame.linkcntl1==16);
 tws_link_conn_start(&peer);CHECK(tws_link.link==&peer && !tws_link.plink && tws_link.tx_timer==9);
 tws_link_detach();CHECK(!tws_link.link);
 puts("bd19/tws_link: ok");return 0;
}
#endif
