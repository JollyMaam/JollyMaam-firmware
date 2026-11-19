/* BD19 Classic baseband frames, radio register access and paging IRQ paths.
 * The SDK allocates584-byte frames in an eight-slot pointer table. Preserve
 * its stubbed APIs, signed16-bit phase arithmetic, callback snapshots and
 * unguarded allocation/IRQ assumptions; follow-ups are in revisit-bd19.md.
 */
#include "btctrler/glue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static __attribute__((always_inline)) inline unsigned bit_count_range(u32 value, unsigned first, unsigned end)
{
    unsigned count = 0;
    for (unsigned bit = first; bit < end; ++bit)
        count += (value >> bit) & 1u;
    return count;
}

static __attribute__((always_inline)) inline unsigned transition_count(u32 value, unsigned first, unsigned end)
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
static void bredr_rx_irq_handler(void);

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
    __builtin_memset(packed, 0, 10);
    for (int i = 0; i < 10; ++i) {
        int half = i >> 1;
        for (int bit = 0; bit < 8; ++bit) {
            int parity = bit % 2;
            u8 value = (u8)(((1 << bit) & map[i]) >> ((bit >> 1) + parity));
            u8 *dst = packed + half + (parity ? 5 : 0);
            *dst |= (u8)(((i & 1) ? value << 4 : value));
        }
    }
    __builtin_memset(freq_table, 0, 80);
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
    __builtin_memset(&bd_base, 0, 348);
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
    while ((REG(1835008) & 256) != 0) {}
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


__attribute__((section(".bt_updata_ram_code")))
void delay_2slot_rise(u32 count)
{
    (void)count;
}

RF_CODE u8 check_update_param_len(void)
{
    printf("=================update_param_len:%d\n", 900);
    return 1;
}

RF_CODE void bredr_frame_suspend_control(u8 suspend)
{
    if (bd_base.bredr_running == 0) {
        puts("bredr not running\n");
    } else if (suspend) {
        bredr_bd_close_frame();
    } else {
        REG(1835012) |= 1;
    }
}

RF_CODE int bredr_frame_agc_set(struct ctrl_frame *frame, s8 rx, s8 tx)
{
    (void)frame; (void)rx; (void)tx;
    return 0;
}

static __attribute__((always_inline)) inline s8 agc_to_rssi(u32 agc)
{
    return (s8)((((agc >> 8) | 192) ^ 63) - (agc >> 16));
}

RF_CODE int bredr_frame_get_agc(struct ctrl_frame *frame)
{
    s8 rssi = agc_to_rssi(frame->agc_dat_wr);
    return rssi < -49 ? 2 : rssi > -16;
}

RF_CODE s8 bredr_analog_get_rssi(struct ctrl_frame *frame)
{
    return agc_to_rssi(frame->agc_dat_wr);
}

RF_CODE void bredr_pll_comp_reset(struct ctrl_frame *frame)
{
    bredr_bd_close_frame();
    frame->osc_set = (u32)(((0 - (s32)rf.osc_offset) << 20) / 24000);
    REG(1835012) |= 1;
}

RF_CODE void bd_frame_odd_even(struct ctrl_frame *frame, u8 flag)
{
    (void)frame; (void)flag;
}

RF_CODE void bd_set_prio(struct ctrl_frame *frame, u8 priority, u8 flag)
{
    (void)flag;
    frame->rfpriostat = priority;
}

RF_CODE void bredr_set_frame_bitoff(struct ctrl_frame *frame, u16 bitoff)
{
    if (((REG(1835012) >> 12) & 15) == frame->id)
        REG(1835012) = (REG(1835012) & (u32)-4095) | (bitoff << 1);
}

RF_CODE void wind_align_set(int wind)
{
    u32 value = (u32)wind + 159;
    bool overflow = (value & 65528) > 311;
    REG(1835264) |= 2;
    REG(1835260) = ((overflow ? ((u32)wind + 65383) : value) & 65535) |
                   (overflow ? 32768 : 0);
}

RF_CODE void windset(struct ctrl_frame *frame, u16 win_normal, u16 win_wide,
                     u8 wide)
{
    frame->winsz1 = win_normal << 6;
    frame->winsz0 = win_wide;
    frame->miscntl &= (u16)-5;
    frame->miscntl |= wide << 2;
    u32 value = (u32)win_normal + 159;
    bool overflow = (value & 65528) > 311;
    REG(1835264) |= 2;
    REG(1835260) = ((overflow ? ((u32)win_normal + 65383) : value) & 65535) |
                   (overflow ? 32768 : 0);
}


RF_CODE void tws_monitor_link_txpwr_cpy(struct ctrl_frame *dst,
                                          struct ctrl_frame *src)
{
    (void)dst; (void)src;
}

RF_CODE void tws_link_monitor_slave_enable(struct ctrl_frame *frame)
{
    (void)frame;
}

RF_CODE void bredr_audio_sync_lat_con_config(struct ctrl_frame *frame)
{
    (void)frame;
}

RF_CODE int audio_sync_lat_hopclkn(struct ctrl_frame *frame)
{
    (void)frame;
    return 0;
}

RF_CODE void bredr_bd_frame_dump(void)
{
    printf("bd_frame_con: %x\n", REG(1835012));
    for (int i = 0; i < 8; ++i) {
        struct ctrl_frame *frame = bd_base.frame_ptr[i];
        if (frame != 0)
            printf("frame: %d, %x, rx=%x, %x, tx=%x, %x\n", frame->id,
                   frame->format,
                   (frame->rxptrh0 << 16) | frame->rxptrl0,
                   (frame->rxptrh1 << 16) | frame->rxptrl1,
                   (frame->txptrh0 << 16) | frame->txptrl0,
                   (frame->txptrh1 << 16) | frame->txptrl1);
    }
}

RF_CODE void bredr_frame_get_empty_txbuf_index(struct ctrl_frame *frame,
                                                u8 *update_index)
{
    local_irq_disable();
    u16 toggle, tx0, tx1, link0, link1;
    do {
        REG(1835008) &= (u32)-113;
        REG(1835008) = (frame->id << 4) | REG(1835008);
        REG(1835008) |= 8;
        toggle = frame->txtog;
        tx0 = frame->txbuf0cntl;
        tx1 = frame->txbuf1cntl;
        link0 = frame->linkcntl0;
        link1 = frame->linkcntl1;
        (void)link0; (void)link1;
    } while ((REG(1835008) & 8) == 0);
    u8 selected = (u8)(toggle & 1);
    local_irq_enable();
    for (int i = 0; i < 2; ++i) {
        if (selected != 0) {
            if (tx1 & 1)
                update_index[i] = selected;
        } else if (tx0 & 1) {
            update_index[i] = 0;
        }
        selected = selected == 0;
    }
}


RF_CODE void bredr_frame_get_rx_payload_head(struct ctrl_frame *frame,
                                             struct bredr_payload_head *head)
{
    u16 status[2], phdr[2], packet[2], rxbuf[2];
    u8 selected_toggle;
    local_irq_disable();
    do {
        REG(1835008) &= (u32)-113;
        REG(1835008) = (frame->id << 4) | REG(1835008);
        REG(1835008) |= 8;
        selected_toggle = (u8)(frame->rxtog & 1);
        rxbuf[0] = frame->rxbuf0cntl;
        rxbuf[1] = frame->rxbuf1cntl;
        status[0] = frame->rxstat0;
        status[1] = frame->rxstat1;
        phdr[0] = frame->rxphdr0;
        phdr[1] = frame->rxphdr1;
        packet[0] = frame->rxheader0;
        packet[1] = frame->rxheader1;
    } while ((REG(1835008) & 8) == 0);
    local_irq_enable();

    u8 selected_index[2] = {0xff, 0xff};
    unsigned selected_count = 0;
    for (u8 i = 0; i < 2; ++i) {
        head[i].check = 0;
        head[i].index = 0xff;
        if (selected_toggle != 0) {
            if (rxbuf[1] & 1)
                selected_index[selected_count++] = 1;
        } else if (rxbuf[0] & 1) {
            selected_index[selected_count++] = 0;
        }
        selected_toggle = selected_toggle == 0;
    }
    for (u8 i = 0; i < selected_count; ++i) {
        u8 index = selected_index[i];
        u16 state = status[index];
        head[i].index = index;
        head[i].check = state >> 7;
        head[i].type = (u8)((packet[index] >> 3) & 15);
        head[i].llid = (u8)(phdr[index] & 3);
        head[i].len = (phdr[index] >> 3) & 1023;
        if (state & 8)
            putchar(35);
        if ((frame->miscntl & 1024) && head[i].check == 255) {
            if (frame->prev_rxcrc == frame->rxcrc[index])
                continue;
            head[i].check = 127;
        }
        if (head[i].check == 127)
            frame->prev_rxcrc = frame->rxcrc[index];
    }

}

RF_CODE u8 clkn0_phase(s16 phase)
{
    u16 value = (u16)phase;
    if (value < 312) return 1;
    if ((u16)(value - 312) < 313) return 2;
    if ((u16)(value - 625) < 312) return 3;
    return (u16)(value - 937) < 313 ? 4 : 0;
}

static __attribute__((always_inline)) inline u32 read_local_clock(void)
{
    u32 low, high;
    do {
        low = REG(1835188);
        high = REG(1835192);
    } while (low != REG(1835188));
    return (high << 16) | low;
}

static __attribute__((always_inline)) inline u32 adjust_master_clock(int offset, u16 bitoff, u32 clock)
{
    s32 value = (offset - 1) + (clock & (u32)-4) + clkn0_phase((s16)bitoff);
    if (value < 0)
        value += 268435456;
    else if (value > 268435455)
        value -= 268435456;
    return (u32)(value >> 1);
}

RF_CODE unsigned long get_mclkn(unsigned long soft_clknoff, u16 soft_bitoff, u32 *local_clkn)
{
    u32 clock = read_local_clock();
    *local_clkn = clock >> 1;
    return adjust_master_clock(soft_clknoff, soft_bitoff, clock);
}

RF_CODE unsigned long get_mclkn_to_local_clkn(unsigned long soft_clknoff, u16 soft_bitoff,
                                     u32 *local_clkn, u8 read_local)
{
    u32 clock = read_local ? read_local_clock() : *local_clkn;
    *local_clkn = clock;
    return adjust_master_clock((int)soft_clknoff, soft_bitoff, clock);
}

RF_CODE u32 bredr_get_anchor_point_usec(u16 D_offset, u16 interval)
{
    u32 low, high, fine;
    do {
        low = REG(1835188);
        high = REG(1835192);
        fine = REG(1835196);
    } while (low != REG(1835188));
    u32 local_clock = (high << 16) | low;
    u32 master = get_mclkn_to_local_clkn(1, 1061, &local_clock, 0);
    s32 offset = (s32)D_offset - (s32)(master % interval);
    return (1250 - (s32)fine) +
           (((offset < 0 ? interval : 0) + offset) * 625);
}

RF_CODE void m2s_offset_cal(struct ctrl_frame *frame)
{
    s32 old_offset = frame->clk_offset;
    u16 old_bitoff = frame->bitoff;
    s16 adjusted = (s16)(old_bitoff - 6);
    u16 next_bitoff = (u16)(adjusted < 0 ? (s16)(old_bitoff + 1244) : adjusted);
    s32 phase_delta = (s32)clkn0_phase((s16)old_bitoff) -
                      (s32)clkn0_phase((s16)next_bitoff);
    s32 next_offset = old_offset + (phase_delta < 0 ? phase_delta + 4 : phase_delta);
    frame->bitoff = next_bitoff;
    __write_reg_clkoffset(frame, (u32)next_offset);
    bredr_set_frame_bitoff(frame, next_bitoff);
}

RF_CODE void s2m_offset_cal(struct ctrl_frame *frame, long target_clkn,
                            long rx_clkn, s16 rxbit)
{
    s16 adjusted = (s16)(rxbit - 275);
    u16 next_bitoff = (u16)(adjusted < 0 ? (s16)(rxbit + 975) : adjusted);
    s32 phase_delta = (s32)clkn0_phase((s16)rxbit) -
                      (s32)clkn0_phase((s16)next_bitoff);
    s32 next_offset = (phase_delta < 0 ? phase_delta + 4 : phase_delta) +
                      ((s32)target_clkn - (s32)rx_clkn);
    frame->bitoff = next_bitoff;
    __write_reg_clkoffset(frame, (u32)next_offset);
    bredr_set_frame_bitoff(frame, next_bitoff);
}

RF_CODE void bredr_frame_role_switch_2_slave(struct ctrl_frame *frame)
{
    frame->linkcntl0 = 0;
    frame->linkcntl1 = 16;
    frame->format = (u16)((frame->format & (u16)-32) | 3);
    frame->bufcntl = 8195;
    m2s_offset_cal(frame);
}

RF_CODE void bredr_frame_role_switch_2_master(struct ctrl_frame *frame)
{
    frame->linkcntl0 = 1;
    frame->linkcntl1 = 17;
    frame->format = (u16)((frame->format & (u16)-32) | 2);
    frame->bufcntl = 8451;
    u32 id = (u32)frame->id << 4;
    u16 hop_low, hop_high, rx_low, rx_high, rxbit;
    do {
        REG(1835008) &= (u32)-113;
        REG(1835008) = id | REG(1835008);
        REG(1835008) |= 8;
        hop_high = frame->hopclknh;
        hop_low = frame->hopclknl;
        rx_low = frame->rx_clknl;
        rx_high = frame->rx_clknh;
        rxbit = frame->rxbit;
    } while ((REG(1835008) & 8) == 0);
    s2m_offset_cal(frame, (hop_low & (u16)-4) | ((u32)hop_high << 16),
                   ((rx_high & 4095) << 16) | rx_low, (s16)rxbit);
}

RF_CODE void bredr_frame_page_scan_rx_config(struct link_page_scan *scan)
{
    struct ctrl_frame *frame = scan->frame;
    u8 configured = 0;
    if (scan->quick_mode == 0) {
        REG(1835008) |= 4;
        configured = 1;
    }
    __write_reg_bdaddr(frame, scan->remote.addr);
    __write_reg_bch(frame, scan->remote.access_addr);
    frame->miscntl = (u16)((scan->remote.lt_addr << 4) | frame->miscntl_of_audsync);
    frame->rxtog = 0;
    frame->txtog = 1;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    frame->rxbuf0cntl &= (u16)-2;
    frame->rxbuf1cntl &= (u16)-2;
    frame->winsz1 = 6592;
    frame->winsz0 = 0;
    frame->miscntl &= (u16)-5;
    frame->miscntl = frame->miscntl;
    REG(1835264) |= 2;
    REG(1835260) = 262;
    if (configured) {
        frame->linkcntl0 = 0;
        frame->linkcntl1 = 16;
        frame->format = 99;
        frame->bufcntl = 8195;
    } else {
        bredr_frame_role_switch_2_master(frame);
    }
}

RF_CODE void bredr_frame_inquiry_init(struct link_inquiry *inquiry)
{
    struct ctrl_frame *frame = inquiry->frame;
    uintptr_t fhs = (uintptr_t)inquiry->fhs;
    frame->rxptrl0 = (u16)fhs;
    frame->rxptrh0 = (u16)(fhs >> 16);
    frame->rxptrl1 = (u16)fhs;
    frame->rxptrh1 = (u16)(fhs >> 16);
    frame->clk_offsetl = 1;
    frame->clk_offseth &= (u16)-4096;
    frame->clk_offset = 1;
    frame->format = 102;
    frame->miscntl = (u16)(frame->miscntl_of_audsync | 3);
    frame->winsz1 = 6592;
    frame->winsz0 = 0;
    frame->miscntl &= (u16)-5;
    frame->miscntl = frame->miscntl;
    REG(1835264) |= 2;
    REG(1835260) = 262;
    frame->private_data = &inquiry->agc;
    frame->irq_handler = inquiry_rx_handler;
}

extern u32 UPDATA_BREDR_BASE_BEG[];
extern void __BT_UPDATA_JUMP(u32 command);
extern void p33_soft_reset(void);
static void cpu_reset(void) { p33_soft_reset(); }

/* SDK permits a byte-aligned update record even though the public prototype
 * uses the generated struct type. Keep the intrinsic's minimum alignment1. */
static __attribute__((always_inline)) inline void copy_update_record(void *dest, const void *src)
{
    __builtin_memcpy(dest, src, 20);
}

RF_CODE void __bt_updata_save_curr_used_frame(struct ctrl_frame *frame,
                                               struct bd_bt_updata *updata)
{
    for (u8 index = 0; index < 8; ++index) {
        if (bd_base.frame_ptr[index] == frame) {
            copy_update_record(&bd_base.bt_updata.bt_updata_role, updata);
            bd_base.bt_updata.curr_frame_used_num = index;
            printf("save frame_used_num=%d\n", index);
            return;
        }
    }
}

RF_CODE struct ctrl_frame *__bt_updata_bd_get_frame_rusume(void)
{
    if (config_asser != 0) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/bredr_frame.c", 2899);
        printf("ASSERT-FAILD: frame != NULL ");
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
    return 0;
}

__attribute__((section(".bt_updata_ram_code")))
void __bt_updata_reset_bt_bredrexm_addr(void)
{
    u8 id = bd_base.bt_updata.curr_frame_used_num;
    struct ctrl_frame **frame_ptr = &bd_base.frame_ptr[id];
    bredr_bd_frame_disable(*frame_ptr);
    __builtin_memset(UPDATA_BREDR_BASE_BEG, 0, 900);
    __builtin_memcpy(UPDATA_BREDR_BASE_BEG + 8, &bd_base.bt_updata.bt_updata_role, 20);
    __builtin_memcpy(UPDATA_BREDR_BASE_BEG + 14, *frame_ptr, 584);
    struct ctrl_frame **replacement = (struct ctrl_frame **)UPDATA_BREDR_BASE_BEG;
    replacement[id] = (struct ctrl_frame *)(UPDATA_BREDR_BASE_BEG + 14);
    REG(1899588) = (u32)(uintptr_t)UPDATA_BREDR_BASE_BEG;
    REG(1835200) = (u32)(uintptr_t)UPDATA_BREDR_BASE_BEG;
    REG(1835204) = (u32)((uintptr_t)UPDATA_BREDR_BASE_BEG >> 16);
    printf("new_frame_ptr:%x\n", replacement[id]);
    bredr_bd_frame_enable(replacement[id], 0, 0);
    REG(1982724) = 65535;
    __BT_UPDATA_JUMP(1);
}

RF_CODE void __bt_updata_resume_bredr_rx_irq_handler(void)
{
    request_irq(27, 2, bredr_rx_irq_handler, 0);
    slot_timer_init();
}

RF_CODE void preserve_bitoff_cal(struct ctrl_frame *frame, u32 bitoff_reg,
                                  u32 *preserve_clknoff, u16 *preserve_bitoff,
                                  u16 preserve_time)
{
    s32 base_clk = frame->clk_offset + (((s32)preserve_time / 1250) << 2);
    s16 rem = (s16)(preserve_time % 1250);
    s16 phase_bit;
    if (rem > 625) {
        u32 candidate = bitoff_reg + 64911;
        rem = (s16)(rem - 625);
        phase_bit = (s16)candidate < 0 ? (s16)(bitoff_reg + 625) : (s16)candidate;
        s32 phase_delta = clkn0_phase((s16)bitoff_reg) - clkn0_phase(phase_bit);
        base_clk += phase_delta < 0 ? phase_delta + 4 : phase_delta;
    } else {
        phase_bit = (s16)bitoff_reg;
    }
    s16 next_bitoff = (s16)(phase_bit - rem);
    if (next_bitoff < 0)
        next_bitoff = (s16)(next_bitoff + 1250);
    *preserve_bitoff = (u16)next_bitoff;
    s32 phase_delta = clkn0_phase(phase_bit) - clkn0_phase((s16)next_bitoff);
    *preserve_clknoff = (u32)(base_clk + (phase_delta < 0 ? phase_delta + 4 : phase_delta));
}

RF_CODE void m_tws_offset_cal(struct ctrl_frame *frame, u16 bitoff, u8 mode)
{
    s32 old_offset = frame->clk_offset;
    s16 next = (s16)((mode == 0 ? 631 : mode == 1 ? 6 : 625) + bitoff);
    if (next > 1249)
        next = (s16)(next - 1250);
    s32 phase_delta = clkn0_phase((s16)frame->bitoff) - clkn0_phase(next);
    s32 adjusted = phase_delta + 1;
    if (adjusted < 0)
        adjusted = phase_delta + 5;
    s32 next_offset = adjusted + old_offset + (adjusted > 2 ? -5 : -1);
    frame->bitoff = (u16)next;
    __write_reg_clkoffset(frame, (u32)next_offset);
}

RF_CODE void m_tws_offset_cal_start(struct ctrl_frame *frame, u16 bitoff, u8 mode)
{
    s32 old_offset = frame->clk_offset;
    bool positive = mode != 0;
    s16 next = (s16)((positive ? 6 : 631) + bitoff);
    if (next > 1249)
        next = (s16)(next - 1250);
    s32 phase_delta = clkn0_phase(next) - clkn0_phase((s16)frame->bitoff);
    s32 adjusted = phase_delta < 0 ? phase_delta + 4 : phase_delta;
    if (adjusted == 0 && (int64_t)next < (int64_t)(u32)frame->bitoff)
        adjusted = 4;
    s32 next_offset = (positive ? adjusted : -adjusted) + old_offset;
    frame->bitoff = (u16)next;
    __write_reg_clkoffset(frame, (u32)next_offset);
}

RF_CODE void esco_tws_offset_cal_start(struct ctrl_frame *frame, u16 bitoff)
{
    u32 offset;
    u16 next_bitoff;
    preserve_bitoff_cal(frame, bitoff, &offset, &next_bitoff, 625);
    frame->bitoff = next_bitoff;
    __write_reg_clkoffset(frame, offset);
}

RF_CODE void s_forward_offset_cal(struct ctrl_frame *frame, u32 *forward_clkoff,
                                  u16 *forward_bitoff, u16 sniffer_bitoff,
                                  u8 cal_mode)
{
    (void)frame;
    u32 old_offset = *forward_clkoff;
    bool forward = cal_mode != 0;
    u16 shifted = forward ? sniffer_bitoff : (u16)(sniffer_bitoff + 625);
    s32 next = (s16)shifted % 1250;
    u16 next_bitoff = (u16)next;
    s32 phase_delta = clkn0_phase((s16)next_bitoff) - clkn0_phase((s16)*forward_bitoff);
    s32 adjusted = phase_delta < 0 ? phase_delta + 4 : phase_delta;
    if (adjusted == 0 && (int64_t)next < (int64_t)(u32)*forward_bitoff)
        adjusted = 4;
    *forward_clkoff = (u32)(old_offset + (forward ? adjusted : -adjusted));
    *forward_bitoff = next_bitoff;
}

RF_CODE void s_sniffer_offset_cal(struct ctrl_frame *frame, u16 bitoff)
{
    s32 old_offset = frame->clk_offset;
    u16 next_bitoff = (u16)((bitoff + 625) % 1250);
    s32 phase_delta = clkn0_phase((s16)frame->bitoff) - clkn0_phase((s16)next_bitoff);
    s32 adjusted = phase_delta + 1;
    if (adjusted < 0)
        adjusted = phase_delta + 5;
    s32 next_offset = adjusted + old_offset + (adjusted > 2 ? -5 : -1);
    frame->bitoff = next_bitoff;
    __write_reg_clkoffset(frame, (u32)next_offset);
}

RF_CODE void s_sniffer_offset_cal_start(struct ctrl_frame *forward_frame,
                                         struct ctrl_frame *sniffer_frame,
                                         u32 *sniffer_clkoff, u16 *sniffer_bitoff,
                                         u32 forward_clkoff, u16 forward_bitoff)
{
    u32 offset = forward_clkoff;
    u16 bitoff = forward_bitoff;
    preserve_bitoff_cal(forward_frame, forward_frame->bitoff, &offset, &bitoff,
                        (u16)(1250 - *sniffer_bitoff));
    s32 next_offset = (s32)(offset - 4) + (s32)*sniffer_clkoff;
    sniffer_frame->clk_offsetl = (u16)next_offset;
    sniffer_frame->clk_offseth = (u16)((sniffer_frame->clk_offseth & (u16)-4096) |
                                      ((next_offset >> 16) & 4095));
    sniffer_frame->clk_offset = (u32)next_offset;
    sniffer_frame->bitoff = bitoff;
    *sniffer_bitoff = bitoff;
    *sniffer_clkoff = (u32)next_offset;
}

RF_CODE void tws_forward_offset_cal(struct ctrl_frame *frame, u32 *forward_clkoff,
                                    u16 *forward_bitoff, u16 sniffer_bitoff,
                                    u8 cal_mode)
{
    u16 time = cal_mode == 0 ? sniffer_bitoff :
               (u16)((s16)(sniffer_bitoff + 625) % 1250);
    preserve_bitoff_cal(frame, frame->bitoff, forward_clkoff, forward_bitoff, time);
}

extern void bit_clr_ie(u8 index);
extern void bit_set_ie(u8 index);
static RF_CODE void page_rx_handler(void *priv);

RF_CODE void bredr_frame_page_init(struct link_page *page)
{
    page->frame->private_data = &page->agc;
    page->frame->irq_handler = page_rx_handler;
}

RF_CODE void dut_bredr_close_recall(void)
{
    local_irq_disable();
    bd_base.bredr_running = 0;
    bit_clr_ie(27);
    bit_clr_ie(28);
    bredr_bd_close_frame();
    local_irq_enable();
}

RF_CODE void dut_bredr_init_recall(void)
{
    local_irq_disable();
    bd_base.bredr_running = 1;
    bit_set_ie(27);
    bit_set_ie(28);
    REG(1835012) |= 1;
    local_irq_enable();
}

RF_CODE void __rxoffset_cal(struct ctrl_frame *frame, int target_clkn,
                            int rx_clkn, s16 rxbit, int enable_bitoff)
{
    s16 adjusted = (s16)(rxbit - 281);
    u16 next_bitoff = (u16)(adjusted < 0 ? (s16)(rxbit + 969) : adjusted);
    s32 phase_delta = (s32)clkn0_phase(rxbit) - clkn0_phase((s16)next_bitoff);
    s32 next_offset = (phase_delta < 0 ? phase_delta + 4 : phase_delta) +
                      (target_clkn - rx_clkn);
    frame->bitoff = next_bitoff;
    frame->clk_offset = (u32)next_offset;
    if (enable_bitoff) {
        frame->clk_offsetl = (u16)next_offset;
        frame->clk_offseth = (u16)((frame->clk_offseth & (u16)-4096) |
                                   ((next_offset >> 16) & 4095));
        frame->clk_offset = (u32)next_offset;
        bredr_set_frame_bitoff(frame, next_bitoff);
    }
}

RF_CODE void rxoffset_cal(struct ctrl_frame *frame, int enable_bitoff)
{
    u16 hop_high, hop_low, rx_low, rx_high, rxbit;
    do {
        REG(1835008) &= (u32)-113;
        REG(1835008) = (frame->id << 4) | REG(1835008);
        REG(1835008) |= 8;
        hop_high = frame->hopclknh;
        hop_low = frame->hopclknl;
        rx_low = frame->rx_clknl;
        rx_high = frame->rx_clknh;
        rxbit = frame->rxbit;
    } while ((REG(1835008) & 8) == 0);
    __rxoffset_cal(frame, (hop_low & (u16)-4) | ((u32)hop_high << 16),
                   ((rx_high & 4095) << 16) | rx_low, (s16)rxbit,
                   enable_bitoff);
}

RF_CODE void bredr_frame_inquiry_scan_init(struct link_inquiry_scan *scan,
                                            struct bredr_local_eir *eir)
{
    struct ctrl_frame *frame = scan->frame;
    uintptr_t fhs = (uintptr_t)scan->fhs;
    frame->txptrl0 = (u16)fhs;
    frame->txptrh0 = (u16)(fhs >> 16);
    frame->txphdr0 = 147;
    frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | 2);
    u8 enabled = eir->enable;
    if (enabled) {
        uintptr_t data = (uintptr_t)scan->eir_data;
        frame->txptrl1 = (u16)data;
        frame->txptrh1 = (u16)(data >> 16);
        frame->txphdr1 = (u16)((eir->len << 3) | 3);
        frame->linkcntl1 = (u16)((eir->type | 16) | (frame->linkcntl1 & (u16)-32));
    }
    frame->format = 103;
    frame->miscntl = (u16)((enabled ? 3 : 0) | frame->miscntl_of_audsync);
    frame->bitoff -= 10;
    frame->winsz1 = 6592;
    frame->winsz0 = 10000;
    frame->miscntl &= (u16)-5;
    frame->miscntl |= 4;
    REG(1835264) |= 2;
    REG(1835260) = 262;
}

RF_CODE void bredr_frame_page_scan_init(struct link_page_scan *scan)
{
    struct ctrl_frame *frame = scan->frame;
    if (scan->quick_mode == 0) {
        uintptr_t fhs = (uintptr_t)scan->fhs;
        frame->rxptrl0 = (u16)fhs;
        frame->rxptrh0 = (u16)(fhs >> 16);
        frame->rxptrl1 = (u16)fhs;
        frame->rxptrh1 = (u16)(fhs >> 16);
    } else {
        uintptr_t rx = (uintptr_t)&scan->tws_rx_info.crc;
        uintptr_t tx = (uintptr_t)&scan->tws_tx_info.crc;
        frame->rxptrl0 = (u16)rx;
        frame->rxptrh0 = (u16)(rx >> 16);
        frame->rxptrl1 = (u16)rx;
        frame->rxptrh1 = (u16)(rx >> 16);
        frame->txptrl0 = (u16)tx;
        frame->txptrh0 = (u16)(tx >> 16);
        frame->txphdr0 = 131;
        frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | 3);
        frame->txptrl1 = (u16)tx;
        frame->txptrh1 = (u16)(tx >> 16);
        frame->txphdr1 = 131;
        frame->linkcntl1 = (u16)((frame->linkcntl1 & (u16)-32) | 19);
        scan->remote_clkn = 19088752;
    }
    frame->private_data = &scan->state;
    frame->irq_handler = page_scan_rx_handler;
}

static RF_CODE void finecnt_timer_set(u8 timer, struct ctrl_frame *frame,
                              void (*handler)(void *), void *priv,
                              int slot, u16 finecnt)
{
    u32 offset;
    u16 bitoff;
    preserve_bitoff_cal(frame, frame->bitoff, &offset, &bitoff, finecnt);
    slot_timer_set_finecnt(timer, handler, priv, slot, bitoff);
}

RF_CODE void bredr_frame_page_scan_resume(struct link_page_scan *scan)
{
    struct ctrl_frame *frame = scan->frame;
    bredr_bd_frame_disable(frame);
    u16 *enable_bitoff = &frame->bitoff;
    if (scan->quick_mode == 0) {
        frame->bufcntl = 0;
        frame->rxtog = 0;
        frame->txtog = 0;
        frame->rxbuf0cntl = 0;
        frame->rxbuf1cntl = 0;
        frame->txbuf0cntl = 0;
        frame->txbuf1cntl = 0;
        frame->format = 101;
        frame->bitoff = 1051;
        frame->miscntl = (u16)(frame->miscntl_of_audsync | 3);
        frame->winsz1 = (u16)-25536;
        frame->winsz0 = 10000;
        frame->miscntl &= (u16)-5;
        frame->miscntl |= 4;
        REG(1835264) |= 2;
        REG(1835260) = 33240;
        frame->clk_offsetl = 0;
        frame->clk_offseth &= (u16)-4096;
        frame->clk_offset = 0;
        __write_reg_bdaddr(frame, scan->local_addr);
        __write_reg_bch(frame, scan->local_acc);
        slot_timer_set(scan->timer, link_page_scan_timeout, &scan->state, 16);
    } else {
        u32 low = REG(1835188) & (u32)-4;
        u32 high = REG(1835192);
        u32 clock = (high << 16) | low;
        u16 esco_bitoff = 0;
        int has_esco = get_esco_conn_bitoff((s16 *)&esco_bitoff);
        u32 target = clock;
        if (has_esco) {
            s16 adjusted = (s16)(esco_bitoff + 275);
            esco_bitoff = (u16)(adjusted > 1249 ? (s16)(esco_bitoff - 975) : adjusted);
            target = (clock - 1) + clkn0_phase((s16)esco_bitoff);
            s2m_offset_cal(frame, scan->remote_clkn, target, (s16)esco_bitoff);
        } else {
            s2m_offset_cal(frame, scan->remote_clkn, clock, 83);
        }
        frame->format = 99;
        frame->bufcntl = 0;
        frame->rxtog = 0;
        frame->txtog = 1;
        frame->miscntl = (u16)(frame->miscntl_of_audsync | 115);
        frame->txbuf0cntl &= (u16)-2;
        frame->txbuf1cntl &= (u16)-2;
        frame->rxbuf0cntl &= (u16)-2;
        frame->rxbuf1cntl &= (u16)-2;
        frame->winsz1 = (u16)-25536;
        frame->winsz0 = 5000;
        frame->miscntl &= (u16)-5;
        frame->miscntl |= 4;
        REG(1835264) |= 2;
        REG(1835260) = 33240;
        __write_reg_bdaddr(frame, scan->tws_addr);
        __write_reg_bch(frame, scan->tws_acc);
        uintptr_t tx = (uintptr_t)&scan->tws_tx_info.crc;
        frame->txptrl0 = (u16)tx;
        frame->txptrh0 = (u16)(tx >> 16);
        frame->txphdr0 = 131;
        frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | 3);
        frame->txptrl1 = (u16)tx;
        frame->txptrh1 = (u16)(tx >> 16);
        frame->txphdr1 = 131;
        frame->linkcntl1 = (u16)((frame->linkcntl1 & (u16)-32) | 19);
        finecnt_timer_set(scan->timer, frame, link_page_scan_timeout, &scan->state,
                          frame->winsz0 / 625 + (s32)target / 2, 200);
    }
    local_irq_disable();
    REG(1835012) = (*enable_bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
}

extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;
extern u8 crc8(const u8 *data, u16 length);
static RF_CODE void role_switch_page_scan_timeout(void *priv);
static RF_CODE void page_timeout(void *priv);

RF_CODE int __check_tws_info(struct tws_pair_info *info)
{
    if (CONFIG_BTCTLER_TWS_ENABLE == 0)
        return 0;
    if (crc8((const u8 *)&info->crc + 1, 15) != info->crc)
        return -1;
    return tws_host_channel_match(info->channel) == 0 ? -2 : 0;
}

RF_CODE void bredr_frame_rs_page_scan_enable(struct link_page_scan *scan)
{
    struct ctrl_frame *frame = scan->frame;
    scan->remote.clk_offset = 0;
    scan->substate = 0;
    __builtin_memcpy(scan->local_addr, hci_param.addr, 6);
    __get_access_addr(scan->local_acc, hci_param.addr);
    m2s_offset_cal(frame);
    uintptr_t fhs = (uintptr_t)scan->fhs;
    frame->rxptrl0 = (u16)fhs;
    frame->rxptrh0 = (u16)(fhs >> 16);
    frame->rxptrl1 = (u16)fhs;
    frame->rxptrh1 = (u16)(fhs >> 16);
    frame->miscntl &= (u16)-241;
    frame->miscntl |= (scan->ab_train << 7) | (scan->backup_lt_addr << 4);
    frame->format = (u16)((frame->format & (u16)-32) | 3);
    frame->rxbuf0cntl &= (u16)-2;
    frame->rxbuf1cntl &= (u16)-2;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    __write_reg_bdaddr(frame, scan->local_addr);
    __write_reg_bch(frame, scan->local_acc);
    frame->winsz1 = 6592;
    frame->winsz0 = 0;
    frame->miscntl &= (u16)-5;
    frame->miscntl = frame->miscntl;
    REG(1835264) |= 2;
    REG(1835260) = 262;
    frame->private_data = &scan->state;
    frame->irq_handler = role_switch_page_scan_rx_handler;
    scan->rs_timeout_step = LMP_TX_FEATURES_REQ;
    scan->timer = slot_timer_get();
    slot_timer_set(scan->timer, role_switch_page_scan_timeout, &scan->state, 32);
    local_irq_disable();
    REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
}


RF_CODE void bredr_frame_page_resume(struct link_page *page)
{
    struct ctrl_frame *frame = page->frame;
    bredr_bd_frame_disable(frame);
    if (page->quick_mode == 0) {
        frame->clk_offsetl = 1;
        frame->clk_offseth &= (u16)-4096;
        frame->clk_offset = 1;
        uintptr_t fhs = (uintptr_t)page->fhs;
        frame->txptrl0 = (u16)fhs;
        frame->txptrh0 = (u16)(fhs >> 16);
        frame->txphdr0 = 147;
        frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | 2);
        frame->txptrl1 = (u16)fhs;
        frame->txptrh1 = (u16)(fhs >> 16);
        frame->txphdr1 = 147;
        frame->linkcntl1 = (u16)((frame->linkcntl1 & (u16)-32) | 18);
        frame->format = 100;
        frame->miscntl = (u16)(frame->miscntl_of_audsync | (page->ab_train << 7) | 3);
        frame->bufcntl = 0;
        frame->rxtog = 0;
        frame->txtog = 0;
        frame->winsz1 = 6592;
        frame->winsz0 = 0;
        frame->miscntl &= (u16)-5;
        frame->miscntl = frame->miscntl;
        REG(1835264) |= 2;
        REG(1835260) = 262;
        __write_reg_bdaddr(frame, page->remote.addr);
        __write_reg_bch(frame, page->remote.access_addr);
        slot_timer_set(page->timer, page_timeout, &page->agc, 18);
    } else {
        while (REG(1835196) > 100) {}
        u32 low = REG(1835188);
        u32 high = REG(1835192);
        u32 clock = (high << 16) | low;
        s2m_offset_cal(frame, 19088752, clock, 83);
        uintptr_t tx = (uintptr_t)&page->tws_tx_info.crc;
        frame->txptrl0 = (u16)tx;
        frame->txptrh0 = (u16)(tx >> 16);
        frame->txphdr0 = 131;
        frame->linkcntl0 = (u16)((frame->linkcntl0 & (u16)-16) | 3);
        frame->txptrl1 = (u16)tx;
        frame->txptrh1 = (u16)(tx >> 16);
        frame->txphdr1 = 131;
        frame->linkcntl1 = (u16)((frame->linkcntl1 & (u16)-32) | 19);
        uintptr_t rx = (uintptr_t)&page->tws_rx_info.crc;
        frame->rxptrl0 = (u16)rx;
        frame->rxptrh0 = (u16)(rx >> 16);
        frame->rxptrl1 = (u16)rx;
        frame->rxptrh1 = (u16)(rx >> 16);
        frame->format = 98;
        frame->miscntl = (u16)(frame->miscntl_of_audsync | 115);
        frame->bufcntl = 0;
        frame->rxtog = 0;
        frame->txtog = 1;
        frame->txbuf0cntl &= (u16)-2;
        frame->txbuf1cntl &= (u16)-2;
        frame->rxbuf0cntl &= (u16)-2;
        frame->rxbuf1cntl &= (u16)-2;
        frame->winsz1 = 6592;
        frame->winsz0 = 0;
        frame->miscntl &= (u16)-5;
        frame->miscntl = frame->miscntl;
        REG(1835264) |= 2;
        REG(1835260) = 262;
        __write_reg_bdaddr(frame, page->tws_addr);
        __write_reg_bch(frame, page->tws_acc);
        finecnt_timer_set(page->timer, frame, page_timeout, &page->agc,
                          (clock >> 1) + 26, 200);
    }
    local_irq_disable();
    REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
}

static RF_CODE void page_timeout(void *priv)
{
    struct link_page *page = priv;
    if (page->quick_mode == 0)
        REG(1835008) |= 4;
    link_page_timeout(priv);
}

static RF_CODE void page_rx_frame_config(struct link_page *page)
{
    struct ctrl_frame *frame = page->frame;
    u8 normal = page->quick_mode == 0;
    if (normal)
        REG(1835008) |= 4;
    __write_reg_bdaddr(frame, page->local_addr);
    __write_reg_bch(frame, page->local_acc);
    frame->miscntl = (u16)((page->lt_addr << 4) | frame->miscntl_of_audsync);
    frame->winsz1 = 6592;
    frame->winsz0 = 0;
    frame->miscntl &= (u16)-5;
    frame->miscntl = frame->miscntl;
    REG(1835264) |= 2;
    REG(1835260) = 262;
    frame->rxtog = 0;
    frame->txtog = 1;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    frame->rxbuf0cntl &= (u16)-2;
    frame->rxbuf1cntl &= (u16)-2;
    if (normal) {
        frame->linkcntl0 = 1;
        frame->linkcntl1 = 17;
        frame->format = 98;
        frame->bufcntl = 8451;
    } else {
        frame->linkcntl0 = 0;
        frame->linkcntl1 = 16;
        frame->format = (u16)((frame->format & (u16)-32) | 3);
        frame->bufcntl = 8195;
        m2s_offset_cal(frame);
    }
}

static RF_CODE void quick_page_step2(void *priv)
{
    struct link_page *page = priv;
    struct ctrl_frame *frame = page->frame;
    bredr_bd_frame_disable(frame);
    page->lt_addr = page->tws_tx_info.lt_addr;
    page->substate = 2;
    page_rx_frame_config(page);
    local_irq_disable();
    REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
    slot_timer_set(page->timer, page_timeout, page, 16);
}

static RF_CODE void quick_page_scan_step2(void *priv)
{
    struct link_page_scan *scan = priv;
    struct ctrl_frame *frame = scan->frame;
    bredr_bd_frame_disable(frame);
    scan->substate = 2;
    bredr_frame_page_scan_rx_config(scan);
    local_irq_disable();
    REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
    REG(1835012) |= 1;
    local_irq_enable();
    slot_timer_set(scan->timer, link_page_scan_timeout, scan, 16);
}

extern BTCTRLER_CONFIG int CONFIG_TEST_DUT_CODE;

RF_CODE void bredr_frame_conn_rx_irq_handler(void *priv)
{
    struct link_connection *link = priv;
    struct ctrl_frame *initial_frame = link->frame;
    struct ctrl_frame *frame = initial_frame;
    if (g_dut_hdl != 0) {
        if (CONFIG_TEST_DUT_CODE != 0)
            dut_rx_handler(link);
        return;
    }
    struct bredr_payload_head heads[2];
    bredr_frame_get_rx_payload_head(frame, heads);
    if (((heads[0].check & 2) != 0 || (heads[1].check & 2) != 0) &&
        link->adjust_clkoffset_enable && (link->role & 1) != 0) {
        bool no_esco = link->esco == 0;
        rxoffset_cal(frame, no_esco);
        frame = link->frame;
        link->sniff_last_bitoff = (s16)frame->bitoff;
        link->sniff_last_soft_clknoff = frame->clk_offset;
        if (no_esco && CONFIG_BTCTLER_TWS_ENABLE != 0)
            tws_slave_plink_premaxval_settings(link);
        if (link->sniff_sta) {
            REG(1835008) |= 4;
            initial_frame->miscntl &= (u16)-261;
        }
    }
    link_conn_rx_handler(link, heads, 1);
}

RF_CODE void inquiry_rx_handler(void *priv)
{
    struct link_inquiry *inquiry = priv;
    struct ctrl_frame *frame = inquiry->frame;
    u8 index = (u8)((frame->rxtog & 1) ^ 1);
    if (inquiry->state != 2)
        return;
    u16 status = (&frame->rxstat0)[index];
    u16 packet_header = (&frame->rxphdr0)[index];
    u16 header = (&frame->rxheader0)[index];
    u8 substate = inquiry->substate;
    if ((status & 0xff80) != 16256) {
        if (substate == 1) {
            inquiry->substate = 2;
            link_inquiry_rx_packet_handler(0);
        }
        return;
    }
    switch (substate) {
    case 0: {
        if (((header >> 3) & 15) != 2) {
            REG(1835008) |= 4;
            inquiry->substate = 2;
            slot_timer_reset(inquiry->timer, 8);
            link_inquiry_rx_packet_handler(0);
            return;
        }
        __read_fhs_packet(&inquiry->result.remote, inquiry->fhs);
        u32 remote_clkn = inquiry->result.remote.clkn;
        u32 rx_clkn = __read_reg_clkn(frame);
        inquiry->result.remote.clk_offset = (s32)(remote_clkn - rx_clkn);
        inquiry->result.remote.rssi = agc_to_rssi(frame->agc_dat_wr);
        if (inquiry->result.remote.eir != 0) {
            ++inquiry->substate;
            slot_timer_reset(inquiry->timer, 8);
            link_inquiry_rx_packet_handler(0);
        } else {
            slot_timer_reset(inquiry->timer, 8);
            link_inquiry_rx_packet_handler(1);
        }
        return;
    }
    case 1: {
        REG(1835008) |= 4;
        u16 type = (header >> 3) & 15;
        if (type != 3 && type != 10 && type != 14 &&
            type != 4 && type != 11 && type != 15) {
            inquiry->substate = 2;
            slot_timer_reset(inquiry->timer, 8);
            link_inquiry_rx_packet_handler(0);
            return;
        }
        inquiry->substate = 2;
        link_inquiry_rx_packet_handler((packet_header >> 3) & 1023);
        return;
    }
    default:
        link_inquiry_rx_packet_handler(0);
        return;
    }
}

RF_CODE void page_scan_rx_handler(void *priv)
{
    struct link_page_scan *scan = priv;
    struct bredr_remote_device *remote = &scan->remote;
    struct ctrl_frame *frame = scan->frame;
    u8 index = (u8)((frame->rxtog & 1) ^ 1);
    if (scan->state != 2)
        return;
    u16 status = (&frame->rxstat0)[index] >> 7;
    u8 type = (u8)(((&frame->rxheader0)[index] >> 3) & 15);
    if (status != 127) {
        printf("e: %x\n", status);
        REG(1835008) |= 4;
        return;
    }
    if (scan->quick_mode != 0) {
        switch (scan->substate) {
        case 0:
            if (type != 3)
                return;
            {
                int check = __check_tws_info(&scan->tws_rx_info);
                if (check != 0) {
                    printf("tws_check_err: %d\n", check);
                    REG(1835008) |= 132;
                    return;
                }
            }
            remote->lt_addr = scan->tws_rx_info.lt_addr;
            __builtin_memcpy(remote->addr, scan->tws_rx_info.addr, 6);
            __builtin_memcpy(remote->mac_addr, scan->tws_rx_info.mac_addr, 6);
            __get_access_addr(remote->access_addr, remote->addr);
            rxoffset_cal(frame, 1);
            {
                u8 timer = scan->timer;
                u32 low, high;
                do {
                    low = REG(1835188);
                    high = REG(1835192);
                } while (low != REG(1835188));
                finecnt_timer_set(timer, frame, quick_page_scan_step2, scan,
                                  (((high << 16) | low) >> 1) + 4, 200);
            }
            link_task_try_run_slots(&scan->task, 20);
            break;
        case 1:
            return;
        default:
            goto advance;
        }
    } else {
        switch (scan->substate) {
        case 0:
            if (type != 0)
                putchar(97);
            slot_timer_reset(scan->timer, 16);
            link_task_try_run_slots(&scan->task, 32);
            break;
        case 1:
            if (type != 2) {
                putchar(98);
                return;
            }
            __read_fhs_packet(remote, scan->fhs);
            {
                u32 target_clkn = remote->clkn;
                u32 rx_clock = __read_reg_clkn(frame);
                __rxoffset_cal(frame, target_clkn, rx_clock, frame->rxbit, 1);
            }
            bredr_frame_page_scan_rx_config(scan);
            slot_timer_reset(scan->timer, 16);
            link_task_try_run_slots(&scan->task, 16);
            break;
        case 2:
            if (type != 1)
                putchar(99);
            rxoffset_cal(frame, 1);
            if (index == 0)
                frame->rxbuf0cntl &= (u16)-2;
            else
                frame->rxbuf1cntl &= (u16)-2;
            break;
        default:
            return;
        }
    }
advance:
    ++scan->substate;
    putchar(scan->substate + 65);
    if (scan->substate > 2) {
        slot_timer_disable(scan->timer);
        frame->txtog |= 1;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        scan->state = 4;
        scan->frame = 0;
        create_link_connection(frame, remote, (frame->format & 31) != 2,
                               scan->quick_mode);
    }
}

static RF_CODE void role_switch_page_scan_timeout(void *priv)
{
    struct link_page_scan *scan = priv;
    struct ctrl_frame *frame = scan->frame;
    switch (scan->rs_timeout_step) {
    case 1: {
        scan->rs_timeout_step = 0;
        u8 timer = scan->timer;
        u32 clock = read_local_clock();
        finecnt_timer_set(timer, frame, role_switch_page_scan_timeout, scan,
                          (clock >> 1) + 8, 700);
        putchar('D');
        return;
    }
    case 2: {
        if (scan->slot_offset != 0)
            scan->remote.clkn += 4;
        s32 remote_offset = scan->remote.clkn;
        s32 rxbit_value = frame->rxbit + scan->slot_offset;
        u32 rx_high = frame->rx_clknh & 4095;
        u32 rx_low = frame->rx_clknl;
        bool wrapped = (rxbit_value & 65534) > 1249;
        s16 bitoff = (s16)(wrapped ? rxbit_value - 1249 : rxbit_value);
        s32 target = ((wrapped ? 3 : -1) + clkn0_phase(bitoff)) +
                     (s32)((rx_high << 16) | rx_low);
        __rxoffset_cal(frame, remote_offset, target, bitoff, 1);
        __write_reg_bdaddr(frame, scan->remote.addr);
        __write_reg_bch(frame, scan->remote.access_addr);
        REG(1835008) |= 4;
        frame->rxtog = 0;
        frame->txtog = 1;
        frame->format = 99;
        frame->miscntl = (u16)(frame->miscntl_of_audsync |
                               (frame->miscntl & (u16)-113) |
                               (scan->remote.lt_addr << 4));
        frame->rxbuf0cntl &= (u16)-2;
        frame->rxbuf1cntl &= (u16)-2;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        frame->linkcntl0 = 0;
        frame->linkcntl1 = 16;
        frame->bufcntl = 8195;
        frame->winsz1 = 6592;
        frame->winsz0 = 0;
        frame->miscntl &= (u16)-5;
        frame->miscntl = frame->miscntl;
        REG(1835264) |= 2;
        REG(1835260) = 262;
        scan->rs_timeout_step = 3;
        ++scan->substate;
        slot_timer_reset(scan->timer, 12);
        putchar('g');
        return;
    }
    case 3:
        bredr_frame_rs_page_scan_enable(scan);
        putchar('k');
        return;
    default:
        bredr_bd_frame_disable(frame);
        frame->clk_offsetl = 1;
        frame->clk_offseth &= (u16)-4096;
        frame->clk_offset = 1;
        frame->bitoff = 1061;
        frame->rxtog = 2;
        frame->txtog = 16;
        frame->format = (u16)((frame->format & (u16)-32) | 2);
        frame->txphdr0 = 6;
        frame->txphdr1 = 6;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        frame->bufcntl = 8451;
        link_role_switch_page_scan_completed(scan, -35);
        local_irq_disable();
        REG(1835012) = (frame->bitoff << 1) | (frame->id << 12);
        REG(1835012) |= 1;
        local_irq_enable();
        return;
    }
}

RF_CODE void role_switch_page_scan_rx_handler(void *priv)
{
    struct link_page_scan *scan = priv;
    struct link_connection *connection = scan->private_data;
    struct ctrl_frame *frame = scan->frame;
    struct bredr_payload_head heads[2];
    bredr_frame_get_rx_payload_head(frame, heads);
    for (u8 i = 0; i < 2; ++i) {
        struct bredr_payload_head *head = &heads[i];
        if (head->index == 0xff)
            break;
        if (head->check != 127) {
            if (head->index == 0)
                frame->rxbuf0cntl &= (u16)-2;
            else
                frame->rxbuf1cntl &= (u16)-2;
            printf("e: %x, %d\n", head->check, scan->substate);
            continue;
        }
        u8 type = head->type;
        putchar(scan->substate + 65);
        switch (scan->substate) {
        case 0:
            if (type != 2) {
                printf("type: %d\n", type);
                if (head->index == 0)
                    frame->rxbuf0cntl &= (u16)-2;
                else
                    frame->rxbuf1cntl &= (u16)-2;
                break;
            }
            __read_fhs_packet(&scan->remote, scan->fhs);
            scan->rs_timeout_step = 2;
            slot_timer_reset(scan->timer, 6);
            link_task_try_run_slots(&connection->task, 64);
            return;
        case 1:
            rxoffset_cal(frame, 1);
            if (head->index == 0)
                frame->rxbuf0cntl &= (u16)-2;
            else
                frame->rxbuf1cntl &= (u16)-2;
            frame->txtog |= 1;
            frame->rxtog = 0;
            link_role_switch_page_scan_completed(scan, 0);
            return;
        default:
            break;
        }
    }
}

static RF_CODE void page_rx_handler(void *priv)
{
    struct link_page *page = priv;
    struct ctrl_frame *frame = page->frame;
    u8 index = (u8)((frame->rxtog & 1) ^ 1);
    void (*completed)(struct bredr_remote_device *, int) = page->completed;
    if (page->state != 2)
        return;
    u16 check = (&frame->rxstat0)[index] >> 7;
    u16 packet_header = (&frame->rxphdr0)[index];
    (void)packet_header;
    u8 type = (u8)(((&frame->rxheader0)[index] >> 3) & 15);
    if (check != 127) {
        printf("check: %x\n", check);
        return;
    }
    if (page->quick_mode != 0) {
        switch (page->substate) {
        case 0:
            if (type != 3)
                return;
            if (__check_tws_info(&page->tws_rx_info) != 0) {
                REG(1835008) |= 132;
                return;
            }
            __builtin_memcpy(page->remote.addr, page->tws_rx_info.addr, 6);
            __builtin_memcpy(page->remote.mac_addr, page->tws_rx_info.mac_addr, 6);
            {
                u8 timer = page->timer;
                u32 low, high;
                do {
                    low = REG(1835188);
                    high = REG(1835192);
                } while (low != REG(1835188));
                finecnt_timer_set(timer, frame, quick_page_step2, page,
                                  (((high << 16) | low) >> 1) + 4, 200);
            }
            link_task_try_run_slots(&page->task, 32);
            page->substate = 1;
            break;
        case 2:
            if (type != 1) {
                printf("type: %d\n", type);
                goto common;
            }
            page->substate = 3;
            rxoffset_cal(frame, 1);
            slot_timer_set(page->timer, page_timeout, page, 8);
            return;
        case 3:
            if (type != 1)
                return;
            page->substate = 4;
            rxoffset_cal(frame, 1);
            break;
        default:
            goto common;
        }
    } else {
        if (type != 0)
            return;
        switch (page->substate) {
        case 0:
            slot_timer_reset(page->timer, 16);
            link_task_try_run_slots(&page->task, 32);
            break;
        case 1:
            page_rx_frame_config(page);
            frame->clk_offsetl = 1;
            frame->clk_offseth &= (u16)-4096;
            frame->clk_offset = 1;
            slot_timer_reset(page->timer, 32);
            link_task_try_run_slots(&page->task, 32);
            break;
        case 2:
            if (index == 0)
                frame->rxbuf0cntl &= (u16)-2;
            else
                frame->rxbuf1cntl &= (u16)-2;
            break;
        default:
            break;
        }
        ++page->substate;
    }
common:
    putchar(page->substate + 48);
    if (page->substate > 2) {
        page->remote.clk_offset = 0;
        page->remote.lt_addr = page->lt_addr;
        page->clk_offset = (u16)gbredr_local_dev.clk_base;
        page->frame = 0;
        page->lt_addr = 0;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        if (completed != 0) {
            page->completed = 0;
            completed((struct bredr_remote_device *)((u8 *)page + 128), 0);
        }
        create_link_connection(frame, &page->remote, (frame->format & 31) != 2,
                               page->quick_mode);
        return;
    }
    if (completed != 0 && page->timeout != 0) {
        u32 low, high;
        do {
            low = REG(1835188);
            high = REG(1835192);
        } while (low != REG(1835188));
        if (bredr_clkn_after((((high << 16) | low) >> 1), page->timeout)) {
            page->completed = 0;
            completed((struct bredr_remote_device *)((u8 *)page + 128), -35);
        }
    }
}

#ifndef UNITTEST
#define BD19_IRQ __attribute__((interrupt("")))
#else
#define BD19_IRQ
#endif
static RF_CODE BD19_IRQ void bredr_rx_irq_handler(void)
{
    if ((REG(1835032) & 4096) != 0 && (REG(1835036) & 4096) != 0) {
        REG(1835040) |= 4096;
        if ((REG(1835012) & 1) != 0)
            bredr_link_head_err_irq_handler(bd_base.frame_ptr[(REG(1835012) >> 12) & 15]);
    }
    for (u8 irq = 8; irq < 11; ++irq) {
        u32 mask = 1u << irq;
        if ((REG(1835032) & mask) != 0 && (REG(1835036) & mask) != 0) {
            REG(1835040) |= mask;
            bredr_link_esco_rx_irq_handler(irq - 8);
        }
    }
    for (u8 irq = 0; irq < 8; ++irq) {
        u32 mask = 1u << irq;
        if ((REG(1835032) & mask) != 0 && (REG(1835036) & mask) != 0) {
            REG(1835040) |= mask;
            struct ctrl_frame *frame = bd_base.frame_ptr[irq];
            if (frame->irq_handler != 0)
                frame->irq_handler(frame->private_data);
        }
    }
    if (CONFIG_BTCTLER_TWS_ENABLE != 0) {
        for (u8 irq = 0; irq < 8; ++irq) {
            u32 mask = 1u << irq;
            if ((REG(1835268) & mask) != 0 && (REG(1835272) & mask) != 0) {
                REG(1835276) |= mask;
                tws_slave_rx_switch_conn_irq_handler(bd_base.frame_ptr[irq]);
            }
        }
    }
}
