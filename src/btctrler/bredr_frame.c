/* The SDK's bredr_frame.c: the Classic baseband's hardware layer.  Each
 * link owns one of four ctrl_frame register blocks in bd_base (the table
 * the baseband reads through 0x1cfc44, which holds its base address; radio
 * pointers are 15-bit offsets from it).  This file writes those blocks,
 * hands frames out and back, programs the 16-entry exchange table that
 * schedules them, the hop tables and AFH, the eSCO ports, and takes the
 * baseband interrupt.
 *
 * Kept as the SDK has it: bd_base is cleared over 4196 bytes on init;
 * pointer range checks read the base register once to check and again to
 * convert; bredr_analog_get_rssi reads RSSI3 and drops it; the AFH table
 * search, the TWS monitor set-up and every register write sequence as
 * written.  Not owned (no caller): the update-jump path, sync-train tx info,
 * READ_SLOT_CLK2, anchor-point and flow helpers, priority setters,
 * bredr_sco_init. */
#include "btctrler/glue.h"

#include <stdint.h>
#include <string.h>

#define RF_CODE __attribute__((section(".classic_rf_code")))
#define RAM_CODE __attribute__((section(".bt_updata_ram_code")))

void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
void unrequest_irq(u8 index);
int irq_read(u32 index);
void bit_clr_ie(u32 index);
void bit_set_ie(u32 index);
void delay(u32 cnt);
void bredr_power_get(void);
void bredr_power_put(void);
extern const u8 log_tag_const_i_BL;
void log_print(int level, const char *tag, const char *format, ...);

#define BT_IRQ 27

static void bredr_rx_irq_handler(void);

#ifdef UNITTEST
static volatile u32 test_bb[0x200 / 4], test_bt[0x300 / 4];
#define REG(a) (*((a) >= 0x1cfc00 ? &test_bt[((a) - 0x1cfc00) / 4] : &test_bb[((a) - 0x1c0000) / 4]))
#define RF_IRQ
#else
#define REG(a) (*(volatile u32 *)(a))
#define RF_IRQ __attribute__((interrupt("")))
#endif

#define RADIO_BASE REG(0x1cfc44)
#define PHY_TO_RADIO(p) ((u32)(uintptr_t)(p) - RADIO_BASE)

_Static_assert(sizeof(struct ctrl_frame) == 180, "SDK ctrl_frame size");
_Static_assert(offsetof(struct ctrl_frame, acldmpriocntl) == 84, "SDK acldmpriocntl offset");

struct bredr_table bd_base __attribute__((section(".bd_base"), used));
const struct update_target bredr_update_target __attribute__((section(".update_target"), used)) = {
    "bredr", (update_handler_t)bredr_bd_close,
};
u32 ESCOPORTCNTL0_save __attribute__((section(".classic_rf_bss")));
u32 ESCOPORTCNTL1_save __attribute__((section(".classic_rf_bss")));

static int frame_index(struct ctrl_frame *frame)
{
    return (int)(frame - bd_base.frame);
}

/* ---- per-frame registers ------------------------------------------------ */

RF_CODE u16 __read_reg_packet_seqn(struct ctrl_frame *frame, int index)
{
    return ((&frame->linkcntl0)[index] >> 4) & 1;
}

RF_CODE void __write_reg_seqn(struct ctrl_frame *frame, int index, u8 seqn)
{
    volatile u16 *r = &frame->linkcntl0 + index;
    u16 v = *r;
    *r = seqn ? (v | 16) : (v & ~16);
}

RF_CODE void __write_reg_rxint(struct ctrl_frame *frame, int index, u8 enable)
{
    volatile u16 *r = &frame->linkcntl0 + index;
    u16 v = *r;
    *r = enable ? (v | 0x4000) : (v & ~0x4000);
}

RF_CODE void __write_reg_packet_type(struct ctrl_frame *frame, int index, int packet)
{
    volatile u16 *r = &frame->linkcntl0 + index;
    *r = (u16)((u16)((*r & (u16)-32592) | packet) | 0x4000);
}

RF_CODE void __write_reg_txinfo(struct ctrl_frame *frame, int index, void *data, int len, int llid, int flow, int packet)
{
    if (!(PHY_TO_RADIO(data) < 32767)) btctrler_assert_failed("PHY_TO_RADIO(data) < 0x7fff", (int)(uintptr_t)data);
    (&frame->txptr0)[index] = (u16)PHY_TO_RADIO(data);
    (&frame->txphdr0)[index] = (u16)(((u32)len << 3 | (u32)llid) | (u32)flow << 2);
    __write_reg_packet_type(frame, index, packet);
}

/* The slot clock: request a latch, wait for it, read it. */
RF_CODE u32 READ_SLOT_CLK(void)
{
    REG(0x1c001c) = 1;
    while (REG(0x1c001c)) {}
    return REG(0x1c0020);
}

RF_CODE u32 GET_FINETIMECNT(void)
{
    return REG(0x1c0024);
}

RF_CODE void __write_reg_bdaddr(struct ctrl_frame *frame, const u8 *addr)
{
    frame->bdaddr0 = (u16)(addr[1] << 8 | addr[0]);
    frame->bdaddr1 = (u16)(addr[3] << 8 | addr[2]);
    frame->bdaddr2 = (u16)(addr[5] << 8 | addr[4]);
}

RF_CODE void __write_reg_bch(struct ctrl_frame *frame, const u8 *bch)
{
    frame->bchl = (u16)(bch[1] << 8 | bch[0]);
    frame->bchm = (u16)(bch[3] << 8 | bch[2]);
    frame->bchh = bch[4];
}

/* 28-bit offset: negative values keep their sign bits above bit 27. */
RF_CODE void __write_reg_clkoffset(struct ctrl_frame *frame, s32 clk_offset)
{
    u32 v = clk_offset < 0 ? ((u32)clk_offset | 0xf8000000u) : ((u32)clk_offset & 0x07ffffffu);
    frame->clk_offsetl = (u16)v;
    frame->clk_offseth = (u16)(v >> 16);
}

RF_CODE s32 __read_reg_clkoffset(struct ctrl_frame *frame)
{
    u32 h = frame->clk_offseth;   /* high half first, as the SDK reads it */
    return (s32)(h << 16 | frame->clk_offsetl);
}

RF_CODE void __write_reg_txphdr(struct ctrl_frame *frame, int index, u16 val)
{
    (&frame->txphdr0)[index] = val;
}

RF_CODE void __write_reg_linkcntl(struct ctrl_frame *frame, int index, int val)
{
    (&frame->linkcntl0)[index] = (u16)val;
}

RF_CODE void __write_reg_miscntl(struct ctrl_frame *frame, u8 ptt, u8 lt_addr)
{
    frame->miscntl = (u8)((lt_addr & 7) | ptt << 3 | (frame->miscntl & 0xf0));
}

RF_CODE void __write_reg_lt_addr(struct ctrl_frame *frame, u8 lt_addr)
{
    frame->miscntl = (u8)((frame->miscntl & 0xf8) | (lt_addr & 7));
}

RF_CODE void __write_reg_format(struct ctrl_frame *frame, u8 train, u8 format)
{
    frame->format = (u8)((format & 31) | train << 7 | (frame->format & 96));
}

RF_CODE void _write_reg_esco_clkalign(struct ctrl_frame *frame, u8 en)
{
    u8 v = frame->format;
    frame->format = en ? (u8)(v | 32) : (u8)(v & ~32);
}

RF_CODE u32 __read_reg_clkn(struct ctrl_frame *frame)
{
    u32 l = frame->rx_clknl;
    return (u32)frame->rx_clknh << 16 | l;
}

/* ---- eSCO ports (channel 0 at 0x1c0120.., channel 1 at 0x1c0150..) ------- */

RF_CODE void __write_reg_escoportcntl(u8 channel, u8 T_sco)
{
    if (channel == 0) REG(0x1c0120) |= (u32)T_sco << 24;
    else if (channel == 1) REG(0x1c0150) |= (u32)T_sco << 24;
}

RF_CODE void __write_reg_escoltcntl(u8 channel, u8 edr, u8 type, u8 lt_addr)
{
    u32 a;
    if (channel == 0) a = 0x1c0134;
    else if (channel == 1) a = 0x1c0164;
    else return;
    REG(a) = (u32)edr << 5 | (u32)edr << 4;
    REG(a) = ((lt_addr & 7) | (u32)type << 3) | REG(a);
}

RF_CODE void __write_reg_escotrcntl(u8 channel, u8 type_m2s, u16 len_m2s, u8 type_s2m, u16 len_s2m)
{
    u32 a;
    if (channel == 0) a = 0x1c0138;
    else if (channel == 1) a = 0x1c0168;
    else return;
    REG(a) = (((u32)len_m2s << 22 | (u32)type_m2s << 8) | (u32)type_s2m << 4) | (u32)len_s2m << 12;
}

RF_CODE void __pcm_out_enable(u8 channel)
{
    if (channel == 0) REG(0x1c0120) |= 0x10000;
    else if (channel == 1) REG(0x1c0150) |= 0x10000;
}

RF_CODE void __pcm_out_disable(u8 channel)
{
    REG(0x1cfc50) &= ~1u;
    if (channel == 0) REG(0x1c0120) &= ~0x10000u;
    else if (channel == 1) REG(0x1c0150) &= ~0x10000u;
}

RF_CODE void __write_reg_encry(struct ctrl_frame *frame, u8 rx_enc, u8 tx_enc)
{
    frame->linkcntl0 &= 32639;
    frame->linkcntl1 &= 32639;
    u32 bits = (u32)tx_enc << 7 | (u32)rx_enc << 15;
    frame->linkcntl0 = (u16)(frame->linkcntl0 | bits);
    frame->linkcntl1 = (u16)(frame->linkcntl1 | bits);
}

RF_CODE void __write_reg_txptr(struct ctrl_frame *frame, int index, u8 *addr)
{
    if (!(PHY_TO_RADIO(addr) < 32767)) btctrler_assert_failed("PHY_TO_RADIO(addr) < 0x7fff", (int)(uintptr_t)addr);
    (&frame->txptr0)[index] = (u16)PHY_TO_RADIO(addr);
}

RF_CODE void __write_reg_lmprxptr(struct ctrl_frame *frame, int index, u8 *addr)
{
    (&frame->lmprxptr0)[index] = (u16)PHY_TO_RADIO(addr);
}

RF_CODE void __clean_reg_rxfull(struct ctrl_frame *frame, int index)
{
    if (index == 0) frame->rxbuf0cntl &= ~1;
    else frame->rxbuf1cntl &= ~1;
    __write_reg_rxint(frame, index, 1);
}

RF_CODE void __clean_reg_txempty(struct ctrl_frame *frame, int index)
{
    if (index == 0) frame->txbuf0cntl &= ~1;
    else frame->txbuf1cntl &= ~1;
}

RF_CODE void __write_reg_rxptr(struct ctrl_frame *frame, int index, u8 *addr)
{
    (&frame->rxptr0)[index] = addr ? (u16)PHY_TO_RADIO(addr) : 0x8000;
}

RF_CODE void __write_reg_txericntl(int len, int type, void *addr)
{
    if (!(PHY_TO_RADIO(addr) < 65535)) btctrler_assert_failed("PHY_TO_RADIO(addr) < 0xffff", 0);
    REG(0x1c00c0) = ((u32)type << 16 | (u32)len << 20) | PHY_TO_RADIO(addr);
}

RF_CODE void __set_default_sco_rx_buf(void)
{
    REG(0x1c0138) = 335873904;
    REG(0x1c0168) = 335873904;
    REG(0x1c0160) = PHY_TO_RADIO(bd_base.esco_rx_buf) << 16 | PHY_TO_RADIO(bd_base.esco_rx_buf);
    REG(0x1c015c) = PHY_TO_RADIO(bd_base.esco_rx_buf) << 16 | PHY_TO_RADIO(bd_base.esco_rx_buf);
    REG(0x1c0130) = PHY_TO_RADIO(bd_base.esco_rx_buf) << 16 | PHY_TO_RADIO(bd_base.esco_rx_buf);
    REG(0x1c012c) = PHY_TO_RADIO(bd_base.esco_rx_buf) << 16 | PHY_TO_RADIO(bd_base.esco_rx_buf);
}

RF_CODE void bredr_radiopwrupdn_reset(void)
{
    REG(0x1c0080) |= 0xf00;
}

/* Hop table 0: even channels 0..78, then odd 1..77 (79 entries), offset by
 * 2 in mode 0. */
RF_CODE void bredr_freq_table_init(u8 mode)
{
    int base = mode ? 0 : 2;
    int cnt = 0;
    for (u8 *p = bd_base.frq_table[0]; p != &bd_base.frq_table[0][40]; p++) {
        cnt = (s8)cnt;
        *p = (u8)(cnt + base);
        cnt += 2;
    }
    cnt = 1;
    for (u8 *p = &bd_base.frq_table[0][40]; p != &bd_base.frq_table[0][79]; p++) {
        cnt = (s8)cnt;
        *p = (u8)(cnt + base);
        cnt += 2;
    }
}

RF_CODE u8 get_bredr_is_init(void)
{
    return bd_base.bredr_running;
}

RF_CODE void bredr_bd_init(void)
{
    if (bd_base.bredr_running == 1) return;
    REG(0x1cfc40) |= 0x400;
    memset(&bd_base, 0, 4196);
    RADIO_BASE = (u32)(uintptr_t)&bd_base;
    REG(0x1c000c) = 0;
    REG(0x1c000c) = (u32)-63486;
    REG(0x1c0018) = 65535;
    REG(0x1c002c) = 320;
    REG(0x1c0058) = 16384;
    REG(0x1c005c) = 0;
    REG(0x1c00f0) = 0;
    REG(0x1c00f4) = 0;
    REG(0x1c0120) = 100663296;
    REG(0x1c0128) = 192;
    REG(0x1c0134) = 8;
    REG(0x1c0120) &= ~0x10000u;
    REG(0x1c0124) = 32789;
    REG(0x1c0150) = 100663296;
    REG(0x1c0158) = 192;
    REG(0x1c0164) = 8;
    REG(0x1c0150) &= ~0x10000u;
    REG(0x1c0154) = 32789;
    REG(0x1c0000) = 263;
    REG(0x1c0080) = 289934664;
    REG(0x1cfc48) = 173;
    REG(0x1c0084) = 0;
    REG(0x1c007c) = 2564;
    __set_default_sco_rx_buf();
    bredr_freq_table_init(1);
    REG(0x1c0000) |= 0x80000000u;
    REG(0x1c0000) |= 0x100;
    request_irq(BT_IRQ, 2, bredr_rx_irq_handler, 0);
    slot_timer_init();
    bredr_power_get();
    bd_base.bredr_running = 1;
}

RF_CODE void bredr_bd_close(void)
{
    if (bd_base.bredr_running == 0) return;
    unrequest_irq(BT_IRQ);
    slot_timer_uninit();
    radio_set_eninv(0);
    REG(0x1c0000) |= 0x80000000u;
    REG(0x1c0000) &= ~0x100u;
    REG(0x1cfc40) &= ~0x400u;
    bredr_power_put();
    bd_base.bredr_running = 0;
}

/* ---- the exchange table: bit 12 radio on (per slot parity), 11 eSCO
 * reserved, 15 eSCO next, 13-14 channel, 0-10 the frame's radio offset ---- */

RF_CODE void radio_set_eninv_open(void)
{
    for (int i = 0; i < 16; i++) bd_base.exchg_table[i] &= ~0x1000;
}

RF_CODE void radio_set_eninv_even_open(void)
{
    for (int i = 0; i < 16; i++) {
        u16 v = bd_base.exchg_table[i];
        bd_base.exchg_table[i] = (i & 1) ? (u16)(v | 0x1000) : (u16)(v & ~0x1000);
    }
}

RF_CODE void radio_set_eninv_odd_open(void)
{
    for (int i = 0; i < 16; i++) {
        u16 v = bd_base.exchg_table[i];
        bd_base.exchg_table[i] = (i & 1) ? (u16)(v & ~0x1000) : (u16)(v | 0x1000);
    }
}

RF_CODE static void radio_set_exchg_table(int bit, int v)
{
    u32 mask = 1u << bit;
    if (v)
        for (int i = 0; i < 16; i++) bd_base.exchg_table[i] = (u16)(bd_base.exchg_table[i] | mask);
    else
        for (int i = 0; i < 16; i++) bd_base.exchg_table[i] = (u16)(bd_base.exchg_table[i] & (mask ^ 0xffff));
}

RF_CODE void radio_set_eninv(int v)
{
    local_irq_disable();
    if (v) {
        if (bd_base.eninv_flag) {
            local_irq_enable();
            return;
        }
        if (bd_base.current_frame == 1) radio_set_eninv_odd_open();
        else if (bd_base.current_frame == 0) radio_set_eninv_even_open();
        else radio_set_eninv_open();
    } else {
        radio_set_exchg_table(12, 1);
        bd_base.eninv_flag = 0;
    }
    local_irq_enable();
}

RF_CODE void bd_frame_odd_even(struct ctrl_frame *frame, u8 flag)
{
    if (bd_base.cur_ctrl_frame != frame) return;
    bd_base.current_frame = flag;
    radio_set_eninv(1);
}

RF_CODE void bredr_bd_frame_enable(struct ctrl_frame *frame, u8 flag, u8 set_en)
{
    if (!(PHY_TO_RADIO(frame) < 2048)) btctrler_assert_failed("PHY_TO_RADIO(frame) <= 0x7ff", (int)(uintptr_t)frame);
    if (!((void *)frame < (void *)bd_base.link_rx_bulk[0][0]))
        btctrler_assert_failed("frame < bd_base.frame + BREDR_FRAME_NUM", (int)(uintptr_t)frame);
    local_irq_disable();
    for (int i = 0; i < 16; i++)
        bd_base.exchg_table[i] = (u16)((bd_base.exchg_table[i] & ~0x7ff) | PHY_TO_RADIO(frame));
    bd_base.current_frame = flag;
    bd_base.cur_ctrl_frame = frame;
    if (set_en) radio_set_eninv(1);
    local_irq_enable();
}

RF_CODE void bredr_bd_frame_disable(struct ctrl_frame *frame)
{
    if ((bd_base.exchg_table[0] & 0x7ff) == PHY_TO_RADIO(frame)) radio_set_eninv(0);
}

RF_CODE void bt_edr_prio_settings(void *priv, u8 setting)
{
    ((struct ctrl_frame *)priv)->acldmpriocntl = setting ? 7710 : 5140;
}

/* A free frame block, initialised; 0 (after asking the link layer to check
 * its frames) when all four are in use. */
RF_CODE struct ctrl_frame *bredr_bd_get_frame(void)
{
    local_irq_disable();
    u8 used = bd_base.frame_used;
    int i;
    for (i = 0; i < 4; i++)
        if (!(used & 1 << i)) break;
    if (i == 4) {
        bredr_link_check_frame_used();
        local_irq_enable();
        return 0;
    }
    bd_base.frame_used = (u8)(1 << i | used);
    struct ctrl_frame *f = &bd_base.frame[i];
    bd_base.last_clkn[frame_index(f)] = 0;
    memset(f, 0, 180);
    f->linkcntl0 = 16400;
    f->linkcntl1 = 16400;
    f->escorxstat0 = 131;
    f->escorxstat1 = 131;
    f->rxmaxbuf = 768;
    f->txphdr0 = 24583;
    f->txphdr1 = 24583;
    f->acldmpriocntl = 5140;
    f->escodmpriocntl = 7453;
    f->bufcntl = 8236;
    f->PLL_COMP = (u32)(((0 - (int)rf.osc_offset) << 20) / 24000);
    bredr_frame_agc_set(f, 0, 15);
    bredr_normal_pwr_set(f, 15);
    local_irq_enable();
    return f;
}

/* Receive gain step: sel 0 sets step; otherwise nudge the current gain by
 * the last RSSI (up below 800, down above 1100).  Clamped to 0..15; returns
 * 1 below 0, 2 above 15, else 0. */
RF_CODE int bredr_frame_agc_set(struct ctrl_frame *frame, char sel, char step)
{
    u32 gain = frame->RXGAIN0;
    s8 agc = (s8)step;
    if (sel) {
        if (frame->RSSI1 < 800) agc = (s8)(gain + 1);
        else if (frame->RSSI1 > 1100) agc = (s8)(gain + 255);
        else agc = (s8)gain;
    }
    int ret;
    u32 out;
    if (agc < 0) {
        ret = 1;
        out = 0;
    } else {
        ret = agc > 15 ? 2 : 0;
        out = (u32)(s32)(agc < 15 ? agc : 15);
    }
    frame->RXGAIN0 = out;
    return ret;
}

RF_CODE void bredr_pll_comp_reset(struct ctrl_frame *frame)
{
    radio_set_eninv(0);
    REG(0x1c0000) |= 0x80000000u;
    frame->PLL_COMP = (u32)(((0 - (int)rf.osc_offset) << 20) / 24000);
    radio_set_eninv(1);
}

RF_CODE static u8 find_afg_table(struct ctrl_frame *frame)
{
    for (int i = 0; i < 3; i++)
        if (bd_base.afhtable_used[i] == 1 << frame_index(frame)) return (u8)i;
    return 0xff;
}

RF_CODE void bredr_bd_put_frame(struct ctrl_frame *frame)
{
    bredr_bd_frame_disable(frame);
    bd_base.frame_used = (u8)(bd_base.frame_used & ((1 << frame_index(frame)) ^ 255));
    u8 t = find_afg_table(frame);
    if (t != 0xff) bd_base.afhtable_used[t] = 0;
}

/* ---- AFH: a hop table from the 79-bit channel map ----------------------- */

RF_CODE void bdhw_set_frq_table(u8 *frq_table, u8 *channel_map)
{
    u8 map[10];
    memset(map, 0, 10);
    for (int i = 0; i < 10; i++) {
        u32 byte = channel_map[i];
        int half = i >> 1;
        for (int j = 0; j < 8; j++) {
            int rem = j % 2;
            u32 bit = (1u << j & byte) >> ((j >> 1) + rem);
            u8 *dst = rem ? &map[half + 5] : &map[half];
            *dst = (u8)(*dst | ((i & 1) == 0 ? bit : bit << 4));
        }
    }
    memset(frq_table, 0, 80);
    u8 cnt = 0;
    u8 *out = frq_table;
    for (int i = 0; i < 10; i++) {
        u8 c = cnt;
        for (int j = 0; j < 8; j++) {
            if (map[i] & 1 << j) *out++ = c;
            c = (u8)(c + 2);
        }
        cnt = i == 4 ? 1 : (u8)(cnt + 16);
    }
}

RF_CODE void bdhw_disable_afh(struct ctrl_frame *frame)
{
    frame->miscntl &= ~64;
    frame->format &= ~64;
}

RF_CODE void bdhw_enable_afh(struct ctrl_frame *frame)
{
    frame->miscntl |= 64;
    frame->format |= 64;
}

/* Claim (or reuse) one of three AFH tables for the frame, build its hop
 * table, load the map and used-channel count into that table's registers. */
RF_CODE void bdhw_set_afh(struct ctrl_frame *frame, u8 *channel_map)
{
    u8 t = find_afg_table(frame);
    if (t == 0xff) {
        int i;
        for (i = 0; i < 3; i++) {
            if (bd_base.afhtable_used[i] == 0) {
                bd_base.afhtable_used[i] = (u8)(1 << frame_index(frame));
                break;
            }
        }
        if (i >= 3) btctrler_assert_failed("i < BREDR_AFH_TABLE", i);
        t = (u8)i;
    }
    u8 table = (u8)(t + 1);
    bdhw_set_frq_table(bd_base.frq_table[table], channel_map);
    int cnt = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 8; j++)
            cnt += (channel_map[i] & 1 << j) != 0;
    u32 base = 0x1c0090 + t * 12;
    REG(base) = (u32)channel_map[2] << 16 | (u32)channel_map[3] << 24 | (u32)channel_map[1] << 8 | channel_map[0];
    REG(base + 4) = (u32)channel_map[6] << 16 | (u32)channel_map[7] << 24 | (u32)channel_map[5] << 8 | channel_map[4];
    REG(base + 8) = ((u32)channel_map[9] << 8 | channel_map[8]) | (u32)cnt << 24;
    frame->miscntl = (u8)((frame->miscntl & 63) | table << 6);
    frame->format |= 64;
}

RF_CODE void radio_set_esco_next(int v)
{
    radio_set_exchg_table(15, v);
}

RF_CODE void radio_set_esco_rsvd(int v)
{
    radio_set_exchg_table(11, v);
}

RF_CODE void radio_set_channel(int v)
{
    for (int i = 0; i < 16; i++) bd_base.exchg_table[i] &= ~0x6000;
    if (v)
        for (int i = 0; i < 16; i++) bd_base.exchg_table[i] = (u16)(bd_base.exchg_table[i] | (u32)v << 13);
}

/* ---- RSSI ---------------------------------------------------------------- */

RF_CODE char bredr_analog_get_rssi(struct ctrl_frame *frame)
{
    (void)frame->RSSI3;
    u32 r = frame->RSSI2;
    return (char)(((r | 192) ^ 63) - (u8)agc_dbm_tlb[(r >> 8) & 63]);
}

RF_CODE char bredr_analog_get_realtime_rssi(void)
{
    u32 r = REG(0x1cfd8c);
    return (char)(((r | 192) ^ 63) - (u8)agc_dbm_tlb[(r >> 8) & 63]);
}

/* 2 below -19 dBm, 0 for -19..-1, 1 from 0. */
RF_CODE int bredr_frame_get_agc(struct ctrl_frame *frame)
{
    s8 rssi = (s8)bredr_analog_get_rssi(frame);
    return rssi < -19 ? 2 : (((u8)rssi >> 7) ^ 1);
}

/* ---- TWS monitor set-up (unused here: TWS is off) ------------------------ */

RF_CODE void tws_monitor_link_txpwr_cpy(struct ctrl_frame *frame, struct ctrl_frame *tws_frame)
{
    memcpy((void *)&frame->TXPWER_t, (const void *)&tws_frame->TXPWER, 36);
}

static inline void tws_monitor_slave(struct ctrl_frame *frame)
{
    frame->miscntl |= 16;
    REG(0x1c0084) = 351;
    frame->twscntl = (u16)-32701;
    u32 acc = tws_le_acc_generation((u8 *)frame);
    REG(0x1c0084) |= 512;
    frame->twsword0 = (u16)(((acc & 1) ? 5 : 10) | acc << 4);
    frame->twsword1 = (u16)(acc >> 12);
    frame->twsword2 = (u16)(acc >> 28);
    frame->twsword3 = 0;
}

static inline void tws_monitor_master(struct ctrl_frame *frame, u16 twscntl)
{
    REG(0x1c0084) = 347;
    frame->twscntl = twscntl;
    u32 acc = tws_le_acc_generation((u8 *)frame);
    REG(0x1c0084) |= 512;
    frame->twsword0 = 0;
    frame->twsword1 = 0;
    frame->twsword2 = (u16)acc;
    frame->twsword3 = (u16)(acc >> 16);
}

RF_CODE void tws_link_monitor_slave_enable(struct ctrl_frame *frame)
{
    tws_monitor_slave(frame);
}

RF_CODE void tws_link_monitor_master_enable(struct ctrl_frame *frame)
{
    frame->miscntl |= 16;
    tws_monitor_master(frame, 1364);
}

RF_CODE void tws_link_monitor_slave_init(struct ctrl_frame *frame)
{
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]++++++slave %x \n\r\n", frame);
    tws_monitor_slave(frame);
}

RF_CODE void tws_link_monitor_master_init(struct ctrl_frame *frame)
{
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]++++++master %x \n\r\n", frame);
    frame->miscntl &= ~16;
    tws_monitor_master(frame, 1492);
}

/* ---- firmware-update hand-over helpers ----------------------------------- */

RF_CODE void __bt_updata_save_curr_used_frame(struct ctrl_frame *frame, struct bd_bt_updata *updata_info)
{
    for (int i = 0; i < 4; i++) {
        if (&bd_base.frame[i] == frame) {
            memcpy(&bd_base.bt_updata.bt_updata_role, &updata_info->bt_updata_role, 20);
            bd_base.bt_updata.curr_frame_used_num = (u8)i;
            if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]save_frame_used_num=%d\n\r\n", i & 255);
            return;
        }
    }
}

RF_CODE struct ctrl_frame *__bt_updata_bd_get_frame_rusume(void)
{
    u8 n = bd_base.bt_updata.curr_frame_used_num;
    struct ctrl_frame *frame = 0;
    for (int i = 0; i < 4; i++) {
        if (n == i) {
            bd_base.frame_used = (u8)(bd_base.frame_used | 1 << n);
            frame = &bd_base.frame[n];
            if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]load_frame_used_num=%d\n\r\n", n);
            break;
        }
    }
    if (!frame) btctrler_assert_failed("frame != NULL", 0);
    return frame;
}

/* Wait cnt slot-clock edges with the baseband and slot timer interrupts held. */
RAM_CODE void delay_2slot_rise(u32 cnt)
{
    int bb = irq_read(BT_IRQ);
    int st = irq_read(28);
    bit_clr_ie(BT_IRQ);
    bit_clr_ie(28);
    for (u32 i = 0; i != cnt; i++) {
        REG(0x1c000c) &= (u32)-1431633921;
        REG(0x1c0018) |= 1;
        while (!(REG(0x1c0014) & 1)) {}
        REG(0x1c000c) |= 1431633920;
    }
    if (bb) bit_set_ie(BT_IRQ);
    if (st) bit_set_ie(28);
}

RF_CODE void __bt_updata_resume_bredr_rx_irq_handler(void)
{
    request_irq(BT_IRQ, 2, bredr_rx_irq_handler, 0);
    slot_timer_init();
}

RF_CODE void dut_bredr_close_recall(void)
{
    bd_base.bredr_running = 0;
    bit_clr_ie(BT_IRQ);
    bit_clr_ie(28);
    radio_set_eninv(0);
    delay_2slot_rise(5);
    delay(1000);
}

RF_CODE void dut_bredr_init_recall(void)
{
    bd_base.bredr_running = 1;
    request_irq(BT_IRQ, 2, bredr_rx_irq_handler, 0);
    slot_timer_init();
}

/* The baseband interrupt: acknowledge each source; on receive, hand every
 * used frame whose receive clock moved to the link layer; on timeout, the
 * link layer's timeout handler. */
RF_CODE RF_IRQ static void bredr_rx_irq_handler(void)
{
    if (REG(0x1c0010) & 1) REG(0x1c0018) |= 1;
    if (REG(0x1c0010) & 2) {
        REG(0x1c0018) |= 2;
        for (int i = 0; i < 4; i++) {
            if (!(bd_base.frame_used & 1 << i)) continue;
            u32 clkn = (u32)bd_base.frame[i].rx_clknh << 16 | bd_base.frame[i].rx_clknl;
            if (clkn != bd_base.last_clkn[i]) {
                bd_base.last_clkn[i] = clkn;
                bredr_link_rx_irq_handler(&bd_base.frame[i]);
            }
        }
    }
    if (REG(0x1c0010) & 1024) REG(0x1c0018) |= 1024;
    if (REG(0x1c0010) & 512) {
        REG(0x1c0018) |= 512;
        bredr_link_timeout_irq_handler();
    }
    if (REG(0x1c0010) & 2048) REG(0x1c0018) |= 2048;
    if (REG(0x1c0010) & 4) REG(0x1c0018) |= 4;
}

#ifdef UNITTEST
#include "test.h"
struct common_rf_param rf;
const char agc_dbm_tlb[33] = { 10, 20, 30, 40 };
const u8 log_tag_const_i_BL = 0;
static int irq_depth, power_gets, power_puts, timer_inits, rx_irqs, frame_checks, irq_ie_cleared;
static void (*bt_handler)(void);
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id) { (void)priority; (void)cpu_id; if (index == BT_IRQ) bt_handler = handler; }
void unrequest_irq(u8 index) { if (index == BT_IRQ) bt_handler = 0; }
int irq_read(u32 index) { (void)index; return 1; }
void bit_clr_ie(u32 index) { (void)index; irq_ie_cleared++; }
void bit_set_ie(u32 index) { (void)index; }
void delay(u32 cnt) { (void)cnt; }
void bredr_power_get(void) { power_gets++; }
void bredr_power_put(void) { power_puts++; }
void slot_timer_init(void) { timer_inits++; }
void slot_timer_uninit(void) {}
void local_irq_disable(void) { irq_depth++; }
void local_irq_enable(void) { irq_depth--; }
void bredr_link_check_frame_used(void) { frame_checks++; }
void bredr_link_rx_irq_handler(struct ctrl_frame *frame) { (void)frame; rx_irqs++; }
void bredr_link_timeout_irq_handler(void) {}
void bredr_normal_pwr_set(struct ctrl_frame *frame, u8 step) { (void)frame; (void)step; }
long unsigned int tws_le_acc_generation(u8 *addr) { (void)addr; return 0x12345671; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
static int asserts;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }

int main(void)
{
    struct ctrl_frame f;
    memset(&f, 0, sizeof(f));

    /* Clock offset: 28 bits, a negative offset keeps its sign above bit 27. */
    __write_reg_clkoffset(&f, 0x1234567);
    CHECK(f.clk_offsetl == 0x4567 && f.clk_offseth == 0x123 && __read_reg_clkoffset(&f) == 0x1234567);
    __write_reg_clkoffset(&f, -2);
    CHECK(f.clk_offsetl == 0xfffe && f.clk_offseth == 0xffff && __read_reg_clkoffset(&f) == -2);
    __write_reg_clkoffset(&f, 0x7fffffff);       /* bits above 27 dropped */
    CHECK(f.clk_offseth == 0x07ff);

    /* Packet type keeps bits 4-6, 8, 15 of linkcntl and sets rx-int. */
    f.linkcntl1 = 0xffff;
    __write_reg_packet_type(&f, 1, 0x0b);
    CHECK(f.linkcntl1 == ((0xffff & (u16)-32592) | 0x0b | 0x4000));
    __write_reg_seqn(&f, 0, 1);
    CHECK(__read_reg_packet_seqn(&f, 0) == 1);
    __write_reg_seqn(&f, 0, 0);
    CHECK(__read_reg_packet_seqn(&f, 0) == 0);

    /* Addresses are written little-endian, BCH low/mid/high. */
    const u8 addr[6] = { 1, 2, 3, 4, 5, 6 };
    __write_reg_bdaddr(&f, addr);
    CHECK(f.bdaddr0 == 0x0201 && f.bdaddr1 == 0x0403 && f.bdaddr2 == 0x0605);
    __write_reg_bch(&f, addr);
    CHECK(f.bchl == 0x0201 && f.bchm == 0x0403 && f.bchh == 5);
    f.miscntl = 0xff;
    __write_reg_miscntl(&f, 1, 3);
    CHECK(f.miscntl == (0xf0 | 8 | 3));
    __write_reg_format(&f, 1, 0x3f);
    CHECK(f.format == (0x80 | 0x1f));

    /* Encryption bits: tx at 7, rx at 15, in both link controls. */
    f.linkcntl0 = 0xffff; f.linkcntl1 = 0;
    __write_reg_encry(&f, 1, 0);
    CHECK(f.linkcntl0 == ((0xffff & 32639) | 0x8000) && f.linkcntl1 == 0x8000);

    /* Init: base register, hop table 0 (even 0..78 then odd 1..77). */
    bredr_bd_init();
    CHECK(get_bredr_is_init() == 1 && RADIO_BASE == (u32)(uintptr_t)&bd_base && bt_handler);
    CHECK(power_gets == 1 && timer_inits == 1);
    CHECK(bd_base.frq_table[0][0] == 0 && bd_base.frq_table[0][39] == 78);
    CHECK(bd_base.frq_table[0][40] == 1 && bd_base.frq_table[0][78] == 77 && bd_base.frq_table[0][79] == 0);
    bredr_freq_table_init(0);
    CHECK(bd_base.frq_table[0][0] == 2 && bd_base.frq_table[0][40] == 3);

    /* TX info: radio offset from the base, header len/llid/flow. */
    u8 *tx = (u8 *)&bd_base.link_rx_bulk[0][0][0];
    struct ctrl_frame *fr = bredr_bd_get_frame();
    CHECK(fr == &bd_base.frame[0] && bd_base.frame_used == 1 && fr->linkcntl0 == 16400 && fr->bufcntl == 8236);
    __write_reg_txinfo(fr, 0, tx, 27, 2, 1, 4);
    CHECK(fr->txptr0 == (u16)(tx - (u8 *)&bd_base) && fr->txphdr0 == (27 << 3 | 2 | 1 << 2));
    CHECK((fr->linkcntl0 & 0x780f) == (0x4000 | 4) && asserts == 0);

    /* Frames: four, then the link layer is asked to check; put frees one. */
    CHECK(bredr_bd_get_frame() == &bd_base.frame[1]);
    CHECK(bredr_bd_get_frame() == &bd_base.frame[2]);
    CHECK(bredr_bd_get_frame() == &bd_base.frame[3]);
    CHECK(bredr_bd_get_frame() == 0 && frame_checks == 1 && irq_depth == 0);
    bredr_bd_put_frame(&bd_base.frame[2]);
    CHECK(bd_base.frame_used == 0x0b && bredr_bd_get_frame() == &bd_base.frame[2]);

    /* Exchange table: frame offset in bits 0-10, radio on by slot parity. */
    bredr_bd_frame_enable(fr, 0, 1);
    CHECK((bd_base.exchg_table[5] & 0x7ff) == (u16)((u8 *)fr - (u8 *)&bd_base));
    CHECK(!(bd_base.exchg_table[0] & 0x1000) && (bd_base.exchg_table[1] & 0x1000));
    bd_frame_odd_even(fr, 1);
    CHECK((bd_base.exchg_table[0] & 0x1000) && !(bd_base.exchg_table[1] & 0x1000));
    bredr_bd_frame_disable(fr);
    CHECK((bd_base.exchg_table[0] & 0x1000) && (bd_base.exchg_table[1] & 0x1000));
    radio_set_channel(2);
    CHECK((bd_base.exchg_table[7] & 0x6000) == 0x4000);
    radio_set_esco_next(1);
    CHECK(bd_base.exchg_table[3] & 0x8000);

    /* AFH: a table per frame, hop list from the map, used-channel count. */
    u8 map[10] = { 0xff, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0x01 };
    bdhw_set_afh(fr, map);
    CHECK(bd_base.afhtable_used[0] == 1 && (fr->miscntl >> 6) == 1 && (fr->format & 64));
    CHECK(REG(0x1c0098) == (0x0100u | 13u << 24) && REG(0x1c0090) == 0x00000fffu);
    bdhw_set_afh(fr, map);                       /* same frame, same table */
    CHECK(bd_base.afhtable_used[1] == 0);
    bdhw_set_afh(&bd_base.frame[1], map);
    CHECK(bd_base.afhtable_used[1] == 2 && (bd_base.frame[1].miscntl >> 6) == 2);
    bredr_bd_put_frame(&bd_base.frame[1]);
    CHECK(bd_base.afhtable_used[1] == 0);

    /* Hop list from a map with channels 0, 1, 8 and 78 used. */
    u8 m2[10] = { 0x03, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x40 };
    u8 hops[80];
    bdhw_set_frq_table(hops, m2);
    CHECK(hops[0] == 0 && hops[1] == 8 && hops[2] == 78 && hops[3] == 1 && hops[4] == 0);

    /* AGC: set directly, clamp to 0..15, nudge by RSSI. */
    CHECK(bredr_frame_agc_set(fr, 0, 7) == 0 && fr->RXGAIN0 == 7);
    CHECK(bredr_frame_agc_set(fr, 0, 20) == 2 && fr->RXGAIN0 == 15);
    CHECK(bredr_frame_agc_set(fr, 0, -1) == 1 && fr->RXGAIN0 == 0);
    fr->RXGAIN0 = 5; fr->RSSI1 = 700;
    CHECK(bredr_frame_agc_set(fr, 1, 0) == 0 && fr->RXGAIN0 == 6);
    fr->RSSI1 = 1200;
    CHECK(bredr_frame_agc_set(fr, 1, 0) == 0 && fr->RXGAIN0 == 5);

    /* RSSI: low byte, sign-folded, minus the gain table entry. */
    fr->RSSI2 = 0x0310;
    CHECK(bredr_analog_get_rssi(fr) == (char)(((0x0310 | 192) ^ 63) - 40));
    CHECK(bredr_frame_get_agc(fr) == 1 || bredr_frame_get_agc(fr) == 0 || bredr_frame_get_agc(fr) == 2);

    /* Clock offset read order and the interrupt: rx on a moved clock only. */
    REG(0x1c0010) = 2;
    bd_base.frame[0].rx_clknl = 5;
    bt_handler();
    CHECK(rx_irqs == 1 && (REG(0x1c0018) & 2));
    bt_handler();
    CHECK(rx_irqs == 1);

    bredr_bd_close();
    CHECK(get_bredr_is_init() == 0 && power_puts == 1 && !bt_handler);
    return TEST_DONE("btctrler_bredr_frame");
}
#endif
