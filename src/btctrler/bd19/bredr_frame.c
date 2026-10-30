/* BD19 bredr_frame.c recovery, partial checkpoint. */
#include "btctrler/glue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static unsigned bit_count_range(u32 value, unsigned first, unsigned end)
{
    unsigned count = 0;
    for (unsigned bit = first; bit < end; ++bit)
        count += (value >> bit) & 1u;
    return count;
}

static unsigned transition_count(u32 value, unsigned first, unsigned end)
{
    unsigned count = 0;
    for (unsigned bit = first; bit < end; ++bit)
        count += (((value >> bit) ^ (value >> (bit + 1))) & 1u);
    return count;
}

/* The equality against unshifted byte fields is the SDK's operation. It
 * makes this flag true only for value == 0; do not normalize the bytes. */
__attribute__((section(".classic_rf_code")))
unsigned char sync_generation(unsigned long value)
{
    unsigned sparse = 0;
    for (unsigned bit = 0; bit < 25; ++bit) {
        u32 lane = (value >> bit) & 0x7fu;
        if (lane == 0 || lane == 0x7fu)
            ++sparse;
    }

    u8 result = sparse != 0;
    if (value == (u32)-1903575338)
        result |= 2;

    u32 mixed = value ^ (u32)-1903575338;
    if (bit_count_range(mixed, 0, 32) == 1)
        result |= 4;

    u32 shifted = (value << 8) & 0xff00u;
    if (shifted == (value & 0xff000000u) &&
        shifted == (value & 0x0000ff00u) &&
        shifted == (value & 0x00ff0000u))
        result |= 8;

    if (transition_count(value, 0, 31) > 24)
        result |= 16;
    if (transition_count(value, 26, 31) < 2)
        result |= 32;
    if (bit_count_range(value, 0, 8) < 3)
        result |= 64;
    if (transition_count(value, 0, 15) > 11)
        result |= 0x80;
    return result;
}

__attribute__((section(".classic_rf_code")))
unsigned long acc_rand_1(unsigned long seed)
{
    u32 next = (u32)seed << 1;
    return (unsigned long)((((next == 0) | ((u32)seed & 0x80000000u)) == 0)
                     ? next
                     : (next ^ 79764919u));
}

__attribute__((section(".classic_rf_code")))
unsigned long tws_acc_generation(u16 seed)
{
    u32 value = seed;
    do {
        value = (u32)acc_rand_1(value);
    } while (sync_generation(value) != 0);
    return value;
}


#define RF_CODE __attribute__((section(".classic_rf_code")))
#define REG(a) (*(volatile u32 *)(uintptr_t)(a))
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
void unrequest_irq(u8 index);
void delay(u32 count);
void bredr_rx_irq_handler(void);

struct bredr_table bd_base __attribute__((section(".bd_base"), used));
u32 ESCOPORTCNTL0_save __attribute__((section(".classic_rf_bss"), aligned(4)));
u32 ESCOPORTCNTL1_save __attribute__((section(".classic_rf_bss"), aligned(4)));

RF_CODE void bredr_frame_set_irq_handler(struct ctrl_frame *frame, void *priv,
                                           void (*handler)(void *))
{
    frame->private_data = priv;
    frame->irq_handler = handler;
}

RF_CODE u16 __read_reg_packet_seqn(struct ctrl_frame *frame, int index)
{
    return ((&frame->linkcntl0)[index] >> 4) & 1;
}

RF_CODE void __write_reg_seqn(struct ctrl_frame *frame, int index, u8 seqn)
{
    volatile u16 *reg = &frame->linkcntl0 + index;
    u16 value = *reg;
    *reg = seqn ? (u16)(value | 16) : (u16)(value & (u16)-17);
}

RF_CODE void __write_reg_rxint(struct ctrl_frame *frame, int index, u8 enable)
{
    (void)frame; (void)index; (void)enable;
}

RF_CODE void __write_reg_packet_type(struct ctrl_frame *frame, int index, int packet)
{
    volatile u16 *reg = &frame->linkcntl0 + index;
    *reg = (u16)(((*reg & (u16)-32592) | packet) | 0x4000);
}

RF_CODE void __write_reg_txinfo(struct ctrl_frame *frame, int index, void *data,
                                 int len, int llid, int flow, int packet)
{
    uintptr_t ptr = (uintptr_t)data;
    u16 header = (u16)(((u32)len << 3 | (u32)llid) | (u32)flow << 2);
    if (index == 0) {
        frame->txptrl0 = (u16)ptr;
        frame->txptrh0 = (u16)(ptr >> 16);
        frame->txphdr0 = header;
        frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | packet);
    } else {
        frame->txptrl1 = (u16)ptr;
        frame->txptrh1 = (u16)(ptr >> 16);
        frame->txphdr1 = header;
        frame->linkcntl1 = (u16)(((frame->linkcntl1 & (u16)-32) | 16) | packet);
    }
}

RF_CODE void __write_reg_txinfo_sync_train(struct ctrl_frame *frame, int index,
                                            void *data, int len, int llid, int seqn,
                                            int flow, int packet)
{
    (void)frame; (void)index; (void)data; (void)len; (void)llid;
    (void)seqn; (void)flow; (void)packet;
}

RF_CODE u32 READ_SLOT_CLK(void)
{
    u32 low, high;
    do {
        low = REG(0x1c00b4);
        high = REG(0x1c00b8);
    } while (low != REG(0x1c00b4));
    return ((high << 16) | low) >> 1;
}

RF_CODE u32 READ_HALF_SLOT_CLK(void)
{
    u32 low = REG(0x1c00b4);
    u32 high = REG(0x1c00b8);
    return (high << 16) | low;
}

RF_CODE u32 GET_FINETIMECNT(void)
{
    return 0;
}

RF_CODE u32 READ_SLOT_CLK2(void)
{
    return 0;
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

RF_CODE void __write_reg_clkoffset(struct ctrl_frame *frame, u32 offset)
{
    frame->clk_offsetl = (u16)offset;
    frame->clk_offseth = (u16)((frame->clk_offseth & (u16)-4096) | ((offset >> 16) & 4095));
    frame->clk_offset = offset;
}

RF_CODE s32 __read_reg_clkoffset(struct ctrl_frame *frame)
{
    return frame->clk_offset;
}

RF_CODE void __write_reg_txphdr(struct ctrl_frame *frame, int index, u16 value)
{
    (&frame->txphdr0)[index] = value;
}

RF_CODE void __write_reg_fullsta_packet(struct ctrl_frame *frame, int val)
{
    u16 bits = (u16)(val << 9);
    frame->linkcntl0 |= bits;
    frame->linkcntl1 |= bits;
}

RF_CODE void __write_reg_linkcntl(struct ctrl_frame *frame, int index, int value)
{
    (&frame->linkcntl0)[index] = (u16)value;
}

RF_CODE int __read_reg_linkcntl(struct ctrl_frame *frame, int index)
{
    return (&frame->linkcntl0)[index];
}

RF_CODE void bredr_hw_write_lt_addr(struct ctrl_frame *frame, u8 lt_addr)
{
    frame->miscntl = (u16)((frame->miscntl & (u16)-113) | (lt_addr << 4));
}

RF_CODE void __write_reg_format(struct ctrl_frame *frame, u8 train, u8 format)
{
    frame->format = (u16)((format & 31) | train << 7 | (frame->format & (u16)-160));
}

RF_CODE void _write_reg_esco_clkalign(struct ctrl_frame *frame, u8 enable)
{
    u16 value = frame->format;
    frame->format = enable ? (u16)(value | 32) : (u16)(value & (u16)-33);
}

RF_CODE u32 __read_reg_clkn(struct ctrl_frame *frame)
{
    u32 low = frame->rx_clknl;
    u32 high = frame->rx_clknh & 4095;
    return high << 16 | low;
}

RF_CODE void __write_reg_lmprxptr(struct ctrl_frame *frame, int index, u8 *addr)
{
    (void)frame; (void)index; (void)addr;
}

RF_CODE void __write_reg_rxptr(struct ctrl_frame *frame, int index, u8 *addr)
{
    uintptr_t ptr = (uintptr_t)addr;
    if (index == 0) {
        frame->rxptrl0 = (u16)ptr;
        frame->rxptrh0 = (u16)(ptr >> 16);
    } else {
        frame->rxptrl1 = (u16)ptr;
        frame->rxptrh1 = (u16)(ptr >> 16);
    }
}

RF_CODE void __write_reg_txericntl(int len, int type, void *addr)
{
    (void)len; (void)type; (void)addr;
}


RF_CODE void bdhw_set_frq_table(u8 *freq_table, u8 *map, u8 *packed)
{
    memset(packed, 0, 10);
    for (int i = 0; i < 10; ++i) {
        int half = i >> 1;
        for (int bit = 0; bit < 8; ++bit) {
            int parity = bit % 2;
            u8 value = (u8)(((1 << bit) & map[i]) >> ((bit >> 1) + parity));
            u8 *dst = packed + half + (parity ? 5 : 0);
            *dst |= (u8)(((i & 1) ? value << 4 : value));
        }
    }
    memset(freq_table, 0, 80);
    u8 *out = freq_table;
    int channel = 0;
    for (int group = 0; group < 10; ++group) {
        if (group == 5)
            channel = 1;
        for (int bit = 0; bit < 8; ++bit) {
            if (packed[group] & (1 << bit))
                *out++ = (u8)channel;
            channel += 2;
        }
    }
}

RF_CODE void bdhw_disable_afh(struct ctrl_frame *frame)
{
    frame->chused = 0;
}

RF_CODE void bdhw_set_afh(struct ctrl_frame *frame, u8 index, u8 *map)
{
    u8 packed[10];
    bdhw_set_frq_table((u8 *)frame->freq_table[index], map, packed);
    int count = 0;
    for (int i = 0; i < 10; ++i)
        for (int bit = 0; bit < 8; ++bit)
            count += (map[i] & (1 << bit)) != 0;
    u16 used = (u16)count | 0x80;
    volatile u16 *used_reg = index < 2 ? &frame->chused : &frame->twschused;
    volatile u16 *map_regs = index < 2 ? &frame->chmap0 : &frame->twschmap0;
    *used_reg = used;
    const u8 *bytes = (const u8 *)packed;
    for (int i = 0; i < 5; ++i)
        map_regs[i] = (u16)((u16)bytes[2 * i + 1] << 8 | bytes[2 * i]);
}

RF_CODE unsigned char radio_get_exchg_table_impl(void) __asm__("radio_get_exchg_table");
RF_CODE unsigned char radio_get_exchg_table_impl(void)
{
    return 0;
}

RF_CODE void exchg_table_prf(void)
{
}


RF_CODE u8 get_bredr_is_init(void)
{
    return bd_base.bredr_running;
}

RF_CODE void bredr_bd_init(void)
{
    puts("bredr_bd_init\n");
    if (bd_base.bredr_running == 1)
        return;
    memset(&bd_base, 0, 348);
    REG(1899588) = (u32)(uintptr_t)&bd_base;
    REG(1835200) = (u32)(uintptr_t)&bd_base;
    REG(1835204) = (u32)((uintptr_t)&bd_base >> 16);
    REG(1835044) = (REG(1835044) & (u32)-128) | 30;
    REG(1835044) = (REG(1835044) & (u32)-16257) | 6016;
    REG(1835020) = (REG(1835020) & (u32)-256) | 70;
    REG(1835020) = (REG(1835020) & (u32)-65281) | 3840;
    REG(1835024) &= (u32)-256;
    REG(1835024) = (REG(1835024) & (u32)-65281) | 1536;
    REG(1835028) = (REG(1835028) & (u32)-256) | 19;
    REG(1835028) = (REG(1835028) & (u32)-65281) | 6400;
    REG(1835016) = (REG(1835016) & (u32)-4096) | 188;
    REG(1835040) |= 255;
    REG(1835036) &= (u32)-256;
    REG(1835008) |= 2;
    REG(1835264) |= 64;
    REG(1835264) |= 1;
    request_irq(27, 2, bredr_rx_irq_handler, 0);
    slot_timer_init();
    bredr_power_get();
    bd_base.bredr_running = 1;
    puts("bredr_bd_init: exit\n");
}

RF_CODE void bredr_bd_close(void)
{
    if (bd_base.bredr_running == 0)
        return;
    unrequest_irq(27);
    slot_timer_uninit();
    delay(1000);
    REG(1899584) &= (u32)-1025;
    bredr_power_put();
    bd_base.bredr_running = 0;
}

RF_CODE void bredr_bd_open_frame(void)
{
    REG(1835012) |= 1;
}

RF_CODE void bredr_bd_close_frame(void)
{
    REG(1835012) &= (u32)-2;
    REG(1835008) |= 132;
    while ((REG(1835008) & 256) == 0) {}
}

RF_CODE void bredr_bd_frame_enable(struct ctrl_frame *frame, u8 reserved0, u8 reserved1)
{
    (void)reserved0;
    (void)reserved1;
    local_irq_disable();
    REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
}

RF_CODE void bredr_bd_frame_disable(struct ctrl_frame *frame)
{
    local_irq_disable();
    if (((REG(1835012) >> 12) & 15) == frame->id)
        bredr_bd_close_frame();
    local_irq_enable();
}


RF_CODE void __write_reg_escoportcntl(u8 channel, u8 T_sco)
{
    (void)channel; (void)T_sco;
}

RF_CODE void __write_reg_escoltcntl(u8 channel, u8 edr, u8 type, u8 lt_addr)
{
    (void)channel; (void)edr; (void)type; (void)lt_addr;
}

RF_CODE void __write_reg_escotrcntl(u8 channel, u8 type_m2s, u16 len_m2s,
                                     u8 type_s2m, u16 len_s2m)
{
    (void)channel; (void)type_m2s; (void)len_m2s; (void)type_s2m; (void)len_s2m;
}

RF_CODE void __pcm_out_disable(u8 channel)
{
    u32 mask = 256u << channel;
    REG(1835040) |= mask;
    REG(1835036) &= ~mask;
}

RF_CODE void __write_reg_encry_disable(struct ctrl_frame *frame)
{
    frame->linkcntl0 &= (u16)-97;
    frame->linkcntl1 &= (u16)-97;
}

RF_CODE void __write_reg_encry(struct ctrl_frame *frame, u8 rx_enc, u8 tx_enc)
{
    frame->linkcntl0 &= (u16)-97;
    frame->linkcntl1 &= (u16)-97;
    u16 value = (u16)((tx_enc << 5) | (rx_enc << 6));
    frame->linkcntl0 |= value;
    frame->linkcntl1 |= value;
}

RF_CODE void __clean_reg_rxfull(struct ctrl_frame *frame, int index)
{
    if (index == 0) frame->rxbuf0cntl &= (u16)-2;
    else frame->rxbuf1cntl &= (u16)-2;
}

RF_CODE void __clean_reg_txempty(struct ctrl_frame *frame, int index)
{
    if (index == 0) frame->txbuf0cntl &= (u16)-2;
    else frame->txbuf1cntl &= (u16)-2;
}

RF_CODE void __set_reg_flow(struct ctrl_frame *frame, u8 enable)
{
    (void)frame; (void)enable;
}

RF_CODE void __set_default_sco_rx_buf(void)
{
}

RF_CODE void bredr_radiopwrupdn_reset(void)
{
}

RF_CODE void bredr_freq_table_init(u8 *frequency_table, u8 mode)
{
    u8 offset = mode == 0 ? 2 : 0;
    for (u8 value = 0; value < 80; value += 2)
        *frequency_table++ = (u8)(value + offset);
    for (u8 value = 1; value < 79; value += 2)
        *frequency_table++ = (u8)(value + offset);
}


RF_CODE struct ctrl_frame *bredr_bd_get_frame(void)
{
    local_irq_disable();
    int id;
    for (id = 0; id < 8; ++id)
        if (bd_base.frame_ptr[id] == 0)
            break;
    if (id == 8) {
        bredr_link_check_frame_used();
        local_irq_enable();
        return 0;
    }

    struct ctrl_frame *frame = zalloc(584);
    frame->id = (u8)id;
    bd_base.frame_ptr[id] = frame;
    if (frame == 0) {
        bredr_link_check_frame_used();
        local_irq_enable();
        return 0;
    }
    frame->rxmaxbuf = 768;
    frame->bitoff = 1061;
    frame->miscntl_of_audsync = 0;
    frame->osc_set = (u32)(((0 - (s32)rf.osc_offset) << 20) / 24000);
    frame->agc_dat_wr = 55;
    frame->tws_agc_dat_wr = 55;
    bredr_normal_pwr_set(frame, 15);
    frame->rfpriostat = 20;
    frame->chmap0 = 0xffff;
    frame->chmap1 = 0xffff;
    frame->chmap2 = 0xffff;
    frame->chmap3 = 0xffff;
    bredr_freq_table_init(&frame->freq_table[0][0], 1);
    u32 bit = 1u << frame->id;
    REG(1835040) |= bit;
    REG(1835036) = (REG(1835036) | bit) | 4096;
    local_irq_enable();
    return frame;
}

RF_CODE void bredr_bd_put_frame(struct ctrl_frame *frame)
{
    local_irq_disable();
    bredr_bd_frame_disable(frame);
    u8 id = frame->id;
    u32 bit = 1u << id;
    REG(1835036) &= ~bit;
    REG(1835040) |= bit;
    bd_base.frame_ptr[id] = 0;
    local_irq_enable();
    free(frame);
}
