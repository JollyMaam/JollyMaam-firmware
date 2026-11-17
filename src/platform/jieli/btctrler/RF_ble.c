/* BLE radio/baseband controller operations and hardware frame management.
 * Keep the SDK's register programming order, scheduler state, and diagnostics. */
#include "btctrler/glue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define RF_BLE_CODE __attribute__((section(".ble_rf_code")))
#ifdef UNITTEST
#define RF_BLE_CSYNC() ((void)0)
#define RF_BLE_IRQ
#else
#define RF_BLE_CSYNC() __asm__ volatile("csync;")
#define RF_BLE_IRQ __attribute__((interrupt("")))
#endif

extern u8 log_tag_const_i_LE_BB, log_tag_const_d_LE_BB, log_tag_const_e_LE_BB;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
extern void printf_buf(const void *buffer, int size);
extern void cpu_assert_debug(void);
extern void __set_extended_conn_dead_times(struct ble_hw *hw, u8 phy);
extern BTCTRLER_CONFIG int config_btctler_hci_standard;
extern BTCTRLER_CONFIG int config_btctler_le_hw_nums, config_btctler_le_slave_multilink;
extern BTCTRLER_CONFIG int config_btctler_le_master_multilink, ble_disable_wait_enable;
extern u32 config_vendor_le_bb;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG int config_btctler_le_rx_nums, config_btctler_le_acl_packet_length, config_btctler_le_acl_total_nums;
static void RF_BLE_CODE __set_hw_irq(u8 id, u8 en);
static void RF_BLE_CODE __set_hw_frame_init_4_2(struct ble_hw *hw);
static void RF_BLE_CODE __set_channel_map(struct ble4_hw *hw, const u8 *channel);
static void RF_BLE_CODE __set_scan_req_enable(struct ble_param *fp);
static void RF_BLE_CODE __set_scan_req_disable(struct ble_param *fp);
static void RF_BLE_CODE __set_init_end(struct ble_hw *hw);
static void RF_BLE_CODE ble_slot_schedule(void);

static u8 rf_adv_fixed_channel __attribute__((section(".ble_rf_data"))) = 0xff;
static u8 rf_adv_fixed_pktcnt __attribute__((section(".ble_rf_data"))) = 3;
static u8 rf_scan_fixed_channel __attribute__((section(".ble_rf_data"))) = 0xff;
static u8 scan_change_prio_cycle __attribute__((section(".ble_rf_data"))) = 6;
static u8 scan_set_prio_max_enable __attribute__((section(".ble_rf_data"))) = 0;
static u8 set_hold_prio_enable __attribute__((section(".ble_rf_data"))) = 0;
static void (*rx_update_handle)(void *, u8 *, u32) __attribute__((section(".ble_rf_data"))) = 0;
static u32 rf_vendor_config __attribute__((section(".ble_rf_data"))) = 1;
static volatile u32 sche_lock __attribute__((section(".ble_rf_bss"))) = 0;
static u8 ble_slot_schedule_credits __attribute__((section(".ble_rf_data"))) = 0;
static struct ble_hw *ble_hw_dut __attribute__((section(".ble_rf_data"))) = 0;
static const u8 rf_ble_le_mac_addr[6] __attribute__((section(".ble_rf_const"))) = {0x20,0x22,0x33,0x44,0x55,0x66};
static u32 coded_set_adv __attribute__((section(".ble_rf_data"))) = 0;
static u32 coded_set_scan __attribute__((section(".ble_rf_data"))) = 0;

struct ble_baseband_layer_handle *bb_hdl __attribute__((section(".ble_rf_bss")));
struct ble_param *g_ble_fp __attribute__((section(".ble_rf_data")));
struct ble_hw *g_hw __attribute__((section(".ble_rf_bss")));
struct ble_tx *dut_tx __attribute__((section(".ble_rf_data")));

static void RF_BLE_CODE le_hw_init(void);
static void RF_BLE_CODE le_hw_destroy(void);
static struct ble_hw *RF_BLE_CODE le_hw_open(void);
static void INIT_LIST_HEAD(struct list_head *list);
static void RF_BLE_CODE __hw_tx_process(struct ble_hw *hw);
static void RF_BLE_CODE ble_tx_drop_staged(struct ble_hw *ble_hw);
static void RF_BLE_CODE ble_latency_manualhold(struct ble_hw *hw);
static void RF_BLE_CODE ble_hw_decrypt(struct ble4_hw *hw, struct ble_rx *rx);
static void cpu_reset(void) { p33_soft_reset(); }

extern const struct ble_operation *__ble_ops;
/* SDK boolean results use the byte ABI. */
extern u8 rf_bt_bredr_is_sniff(void) __asm__("bt_bredr_is_sniff");
extern u8 rf_master_setup_connection_update(struct ble_hw *)
    __asm__("master_setup_connection_updatea_first_data_location");
extern u8 rf_get_if_exist_ble5(struct ble_hw *) __asm__("get_if_exist_ble5");
extern u8 ble_ext_primary_scan_preprocess_ir(struct ble_hw *, struct ble_rx *)
    __asm__("ble_ext_primary_scan_preprocess");

u32 RF_BLE_CODE PHY_TO_BLE(u8 *a)
{
    u32 address = (u32)a;
    u32 base = (u32)&bb_hdl->hw_base;
    if (address - base >= 0xffffu) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 143);
            printf("ASSERT-FAILD: (u32)(a) - (u32)&__this->hw_base < 0xffff 0x%x, 0x%x", address, (u32)bb_hdl);
            cpu_assert_debug();
        } else cpu_reset();
    }
    return address - (u32)bb_hdl;
}

u32 RF_BLE_CODE BLE_TO_PHY(u32 a) { return (u32)bb_hdl + a; }
u8 RF_BLE_CODE HW_ID(struct ble_hw *x) { return (u8)((s32)((u32)x - (u32)bb_hdl->hw_base.hw) / 28); }

void RF_BLE_CODE ble_rf_reset_hw(void) { *(volatile u32 *)1867776 |= 64; }

struct ble_rx *RF_BLE_CODE hw_alloc_rx(int len)
{
    struct ble_rx *rx = hw_dma_alloc(0, len + 20);
    if (rx && (((u32)rx & 3u) != 0)) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 187);
            printf("ASSERT-FAILD: ((u32)rx & 0x03) == 0 %s\n", "rx not align\n");
            cpu_assert_debug();
        } else cpu_reset();
    }
    return rx;
}

void RF_BLE_CODE ble_rx_init(struct ble4_hw *hw) { hw->rx[0] = 0; hw->rx[1] = 0; }

void RF_BLE_CODE ble_hw_rx(struct ble_hw *ble_hw, struct ble_rx *rx, int index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    s16 ptr = (s16)PHY_TO_BLE(rx->data);
    if (index == 0) hw->ble_fp.RXPTR0 = ptr; else hw->ble_fp.RXPTR1 = ptr;
}

void RF_BLE_CODE ble_tx_init(struct ble4_hw *hw) { hw->tx[0] = 0; hw->tx[1] = 0; }

u32 RF_BLE_CODE __get_ble_anchor_con(struct ble_hw *hw, u8 index)
{
    local_irq_disable();
    RF_BLE_CSYNC();
    *(volatile u32 *)1867804 = ((u32)index << 10) | ((u32)HW_ID(hw) << 4) | 2;
    RF_BLE_CSYNC();
    u32 value = *(volatile u32 *)1867812;
    local_irq_enable();
    return value;
}

void RF_BLE_CODE __set_ble_anchor_con(struct ble_hw *hw, u8 index, u32 value)
{
    local_irq_disable();
    RF_BLE_CSYNC();
    *(volatile u32 *)1867808 = value;
    RF_BLE_CSYNC();
    *(volatile u32 *)1867804 = ((u32)index << 10) | ((u32)HW_ID(hw) << 4) | 5;
    local_irq_enable();
}

void RF_BLE_CODE __set_anchor_cnt(struct ble_hw *hw, u32 slot)
{
    __set_ble_anchor_con(hw, 7, 0); __set_ble_anchor_con(hw, 0, 0); __set_ble_anchor_con(hw, 14, 0);
    u32 value = slot ? slot - 1 : 0;
    __set_ble_anchor_con(hw, 0, value & 0xffffu);
    __set_ble_anchor_con(hw, 14, (value >> 16) | 0x8000u);
}

void RF_BLE_CODE __set_interval(struct ble_hw *hw, int interval, int evt_en)
{
    if (hw->ble4_hw) hw->ble4_hw->interval = interval;
    s32 div = (s32)interval / 625;
    __set_ble_anchor_con(hw, 1, (u32)div & 0xffffu);
    __set_ble_anchor_con(hw, 15, (evt_en << 15) | ((u32)(div >> 16)));
}

void RF_BLE_CODE __set_latency_disable(struct ble_hw *hw)
{
    hw->ble4_hw->latency_substate = 1;
    u32 value = __get_ble_anchor_con(hw, 2);
    __set_ble_anchor_con(hw, 2, value & 63487u);
}

static void RF_BLE_CODE __set_le_prio(struct ble4_hw *hw, u8 prio)
{
    u16 *word = (u16 *)((u8 *)hw + offsetof(struct ble4_hw, widen) - 2);
    if (prio) *word = (*word & (u16)-7937) | ((u16)(prio & 31u) << 8);
    if ((*word & 8192u) == 0) { hw->ble_fp.RFPRIOCNTL = prio; hw->ble_fp.RFPRIOSTAT = prio; }
}

void RF_BLE_CODE __set_latency_enable(struct ble_hw *hw)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    hw4->latency_substate = 0;
    __set_le_prio(hw4, rf_bt_bredr_is_sniff() ? 19 : 30);
    u32 value = __get_ble_anchor_con(hw, 2);
    __set_ble_anchor_con(hw, 2, (value & 63487u) | 2048u);
    value = __get_ble_anchor_con(hw, 9);
    __set_ble_anchor_con(hw, 9, (value & 65471u) | 64u);
}

void RF_BLE_CODE __set_latency_hold_cnt(u8 reset_flag, struct ble4_hw *hw, u16 set_cnt)
{ if (reset_flag || hw->hold_cnt < set_cnt) hw->hold_cnt = set_cnt; }

u8 ble_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt) __asm__("ble_rf_vendor_fixed_channel");
u8 RF_BLE_CODE ble_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt)
{ rf_adv_fixed_channel = channel_index; rf_adv_fixed_pktcnt = pktcnt; rf_scan_fixed_channel = channel_index; return 1; }
u8 ble_adv_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt) __asm__("ble_adv_rf_vendor_fixed_channel");
u8 RF_BLE_CODE ble_adv_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt)
{ rf_adv_fixed_channel = channel_index; rf_adv_fixed_pktcnt = pktcnt; return 1; }
u8 ble_scan_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt) __asm__("ble_scan_rf_vendor_fixed_channel");
u8 RF_BLE_CODE ble_scan_rf_vendor_fixed_channel_ir(u8 channel_index, u8 pktcnt)
{ (void)pktcnt; rf_scan_fixed_channel = channel_index; return 1; }

void RF_BLE_CODE __set_adv_channel_index(struct ble_hw *hw, u8 channel_map)
{
    u32 first = 0, hop = 0;
    for (u32 i = 0; i < 3; i++) if ((1u << i) & channel_map) {
        u32 ch = i + 37;
        if (first) { hop = (ch - first) & 255u; break; }
        first = ch;
    }
    u32 min = ((channel_map & 1u) ^ 1u) + 37u;
    u32 max = ((channel_map >> 2) & 1u) | 38u;
    if (log_tag_const_i_LE_BB) {
        log_print(2, NULL, "[Info]: [LE_BB]ch_min channel=%d\r\n", min);
        log_print(2, NULL, "[Info]: [LE_BB]ch_max channel=%d\r\n", max);
        log_print(2, NULL, "[Info]: [LE_BB]hop_inc=0x%x\r\n", hop);
        log_print(2, NULL, "[Info]: [LE_BB]ch_1st=%d\r\n", first);
    }
    if (rf_adv_fixed_channel < 40) {
        volatile u16 *advidx = &hw->ble4_hw->ble_fp.ADVIDX;
        *advidx = ((u16)rf_adv_fixed_pktcnt << 14) | (*advidx & 16383u);
        __set_ble_anchor_con(hw, 6, rf_adv_fixed_channel | 8192u);
    } else {
        __set_ble_anchor_con(hw, 6, (u8)(((channel_map >> 2) << 7) | (((channel_map & 1u) ^ 1u) << 6) | first) | (hop << 8) | 16384u);
    }
}

void RF_BLE_CODE __set_bitoffset(struct ble_hw *hw, int bitoffset) { __set_ble_anchor_con(hw, 7, bitoffset); }
void RF_BLE_CODE __disable_adv_random(struct ble_hw *hw) { __set_ble_anchor_con(hw, 8, 0); }
void RF_BLE_CODE __set_anchor_cnt_disable(struct ble_hw *hw) { __set_ble_anchor_con(hw, 14, 0); }
void RF_BLE_CODE __set_ble_hw_irq_init(struct ble_hw *hw) { __set_hw_irq(HW_ID(hw), 1); }

static void RF_BLE_CODE __set_hw_irq(u8 id, u8 en)
{
    local_irq_disable();
    RF_BLE_CSYNC();
    u32 mask_low = (1u << id) ^ 0xffffffffu;
    u32 value_low = ((u32)en & 1u) << id;
    u32 mask_high = (256u << id) ^ 0xffffffffu;
    u32 value_high = ((u32)en & 1u) << (id + 8);
    if (en) {
        *(volatile u32 *)1867820 = (*(volatile u32 *)1867820 & mask_low) | value_low;
        *(volatile u32 *)1867820 = (*(volatile u32 *)1867820 & mask_high) | value_high;
        *(volatile u32 *)1867852 = (*(volatile u32 *)1867852 & mask_high) | value_high;
    }
    *(volatile u32 *)1867816 = (*(volatile u32 *)1867816 & mask_low) | value_low;
    *(volatile u32 *)1867816 = (*(volatile u32 *)1867816 & mask_high) | value_high;
    *(volatile u32 *)1867852 = (*(volatile u32 *)1867852 & mask_low) | value_low;
    local_irq_enable();
}

void RF_BLE_CODE ble_hw_enable(struct ble_hw *hw, int slot)
{ __set_ble_hw_irq_init(hw); __set_anchor_cnt(hw, slot); }

void RF_BLE_CODE ble_hw_disable(struct ble_hw *hw)
{
    __set_anchor_cnt_disable(hw);
    if (ble_disable_wait_enable) while ((*(volatile u32 *)1867832 & 2u) != 0) { }
    __set_hw_irq(HW_ID(hw), 0);
}

void RF_BLE_CODE __debug_interval(void) { }

void RF_BLE_CODE __set_winsize(struct ble4_hw *hw, int winsize, int nwinsize)
{
    hw->ble_fp.WINCNTL0 = (u16)nwinsize;
    hw->ble_fp.WINCNTL1 = (u16)(nwinsize >> 16);
    hw->ble_fp.WINCNTL2 = (u16)winsize;
    hw->window = nwinsize;
}

void RF_BLE_CODE __set_widen(struct ble4_hw *hw, int widen)
{
    hw->ble_fp.WIDEN0 = (u16)((s32)widen / 625);
    hw->ble_fp.WIDEN1 = (u16)(((s32)widen % 625) | 20480);
    hw->widen = widen;
}

static u32 RF_BLE_CODE conn_update_small_winsize(u8 coded_s, u8 *phy_error)
{
    *phy_error = 0;
    if (coded_s == 0) return 150;
    if (coded_s == 1) return 80;
    *phy_error = 1;
    return 50;
}

void RF_BLE_CODE trace_le_prio(void)
{
    struct ble_param *fp = g_ble_fp;
    if (fp && log_tag_const_e_LE_BB)
        log_print(4, NULL, "<Error>: [LE_BB]LE Prio : 0x%x - 0x%x\r\n", fp->RFPRIOCNTL, fp->RFPRIOSTAT & 31);
}

void RF_BLE_CODE ble_vendor_set_scan_change_prio(u8 change_cycle, u8 set_max_enable)
{
    if (change_cycle) { scan_change_prio_cycle = change_cycle; scan_set_prio_max_enable = set_max_enable; }
}

void RF_BLE_CODE ble_vendor_set_hold_prio(u8 role, u8 enable)
{
    u8 bit = enable & 1u;
    u8 old = set_hold_prio_enable;
    if (role) { set_hold_prio_enable = old & 2u; set_hold_prio_enable |= bit << 1; }
    else { set_hold_prio_enable = old & 1u; set_hold_prio_enable |= bit; }
}

void RF_BLE_CODE __set_conn_dead_times(struct ble_hw *hw)
{
    if ((config_vendor_le_bb & 8) == 0) {
        u8 phy = hw->phy.tx_phy;
        if (!phy) {
            volatile u16 *anchor = &hw->ble4_hw->ble_fp.ANCHOR;
            *anchor = (*anchor & 0x8000u) | 4u;
        } else __set_extended_conn_dead_times(hw, phy);
    }
}

void RF_BLE_CODE ble_pll_comp_reset(void *ble_hw)
{
    struct ble4_hw *hw = *(struct ble4_hw **)ble_hw;
    hw->ble_fp.PLL_COMP = (s32)((0 - (s32)rf.osc_offset) << 20) / 24000;
}

void RF_BLE_CODE ble_rx_update_handle_register(void (*handle)(void *, u8 *, u32))
{
    u8 enabled = log_tag_const_i_LE_BB;
    if (handle) { if (enabled) log_print(2, NULL, "[Info]: [LE_BB]\nLL:reg rx hdl\n\r\n"); }
    else if (enabled) log_print(2, NULL, "[Info]: [LE_BB]\nLL:unreg rx hdl\n\r\n");
    rx_update_handle = handle;
}

void *RF_BLE_CODE hw_entity_alloc(void)
{
    struct _hw_entity *pool = bb_hdl->hw_base.hw_entity;
    struct _hw_entity *hw = NULL;
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        if (!pool[i].used) { hw = &pool[i]; memset(hw, 0, 628); hw->used = 1; break; }
    }
    if (!hw) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 3202); puts("ASSERT-FAILD: hw != NULL hw_entiry_alloc error"); cpu_assert_debug(); }
        else cpu_reset();
    }
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]hw entity alloc : 0x%x / 0x%x\r\n", hw, hw ? &hw->u : NULL);
    return hw;
}

void RF_BLE_CODE hw_entity_free(void *ptr)
{
    if (!ptr) return;
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]hw entity free : 0x%x\r\n", ptr);
    ((struct _hw_entity *)ptr)->used = 0;
}

void RF_BLE_CODE ble_hw_inst_set(u8 hw_id, void *fp_addr)
{ bb_hdl->hw_base.inst[hw_id] = (u16)PHY_TO_BLE(fp_addr) | 0x8000u; }

static void RF_BLE_CODE clear_all_anchor_cnt(struct ble_hw *hw)
{ for (u32 i = 0; i != 17; i++) __set_ble_anchor_con(hw, (u8)i, 0); }

struct ble_hw *RF_BLE_CODE ble_hw_alloc(void)
{
    struct ble_hw *hw = NULL;
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        if (bb_hdl->hw_base.inst[i] == 0) { hw = &bb_hdl->hw_base.hw[i]; memset(hw, 0, 28); break; }
    }
    if (!hw) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 3247); puts("ASSERT-FAILD: hw != NULL BLE HW NUM limit!!!"); cpu_assert_debug(); }
        else cpu_reset();
    }
    clear_all_anchor_cnt(hw);
    return hw;
}

void RF_BLE_CODE ble4_hw_only_alloc_dma_ram(struct ble_hw *hw)
{
    if (!hw->ble4_hw) {
        hw->ble4_hw = hw_entity_alloc();
        __set_hw_frame_init_4_2(hw);
    }
    if (hw->ble4_hw) {
        hw->ble4_hw->regs = btctler_nv_memory_malloc(40);
        bb_hdl->number++;
    }
}

static void RF_BLE_CODE __set_hw_frame_init_4_2(struct ble_hw *ble_hw)
{
    u8 channel[5];
    struct ble4_hw *hw = ble_hw->ble4_hw;
    hw->ble_fp.ANCHOR = 0;
    hw->ble_fp.BITOFF = 0;
    hw->ble_fp.ADVIDX = -11384;
    hw->ble_fp.WIDEN0 = 0;
    hw->ble_fp.WIDEN1 = 20967;
    hw->ble_fp.BDADDR0 = -16682;
    hw->ble_fp.BDADDR1 = -29047;
    hw->ble_fp.CRCWORD0 = 21845;
    hw->ble_fp.CRCWORD1 = 85;
    hw->ble_fp.OPTCNTL = 40;
    hw->ble_fp.TXPTR0 = 1;
    hw->ble_fp.TXPTR1 = 1;
    hw->ble_fp.OPTCNTL |= 512;
    hw->ble_fp.INTFRAME |= 2;
    hw->ble_fp.INTFRAME |= 4;
    hw->ble_fp.RXBUF0CNTL &= (u16)-2;
    hw->ble_fp.RXBUF1CNTL &= (u16)-2;
    ble_agc_normal_set(hw, 0, 25);
    ble_txpwr_normal_set(hw, 0, bt_get_pwr_max_level());
    s32 offset = rf.osc_offset_flag ? rf.osc_offset : rf_osc_offset_initial_val_get();
    hw->ble_fp.PLL_COMP = (s32)((0 - offset) << 20) / 24000;
    hw->ble_fp.RFPRIOCNTL = 257;
    hw->ble_fp.RFPRIOSTAT = 1;
    hw->ble_fp.TXTOG = 0;
    hw->ble_fp.RXTOG = 0;
    hw->ble_fp.TXAHDR0 = 0;
    hw->ble_fp.TXDHDR0 = 1;
    hw->ble_fp.TXAHDR1 = 0;
    hw->ble_fp.TXDHDR1 = 1;
    hw->ble_fp.RXMAXBUF = 255;
    hw->ble_fp.IFSCNT = -32107;
    hw->ble_fp.FILTERCNTL = 0;
    hw->ble_fp.TARGETADRL = 0;
    hw->ble_fp.TARGETADRM = 0;
    hw->ble_fp.TARGETADRU = 0;
    memset(channel, 0xff, sizeof(channel));
    __set_channel_map(hw, channel);
}

void RF_BLE_CODE le_hw_standby(struct ble_hw *hw)
{
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]-> STANDBY state\r\n");
    hw->ble4_hw->state = 0;
    ble_hw_disable(hw);
}

u8 RF_BLE_CODE rf_set_adv_24g_hackable_coded(u32 coded) { coded_set_adv = coded; return 0; }
u8 RF_BLE_CODE rf_set_scan_24g_hackable_coded(u32 coded) { coded_set_scan = coded; return 0; }
u8 RF_BLE_CODE rf_set_24g_hackable_coded(u32 coded)
{ coded_set_adv = coded; coded_set_scan = coded; return 0; }

void RF_BLE_CODE __set_scan_window(struct ble_hw *hw, int winsize, int interval)
{ __set_winsize(hw->ble4_hw, 50, winsize * 625); __set_interval(hw, interval * 625, 0); }

static void RF_BLE_CODE rf_set_bdaddr(struct ble_param *fp, u32 coded)
{
    if (coded) {
        u32 value = coded < 65536 ? coded + 54789 : coded;
        fp->BDADDR0 = (u16)(value >> 16);
        fp->BDADDR1 = (u16)value;
    } else { fp->BDADDR0 = -16682; fp->BDADDR1 = -29047; }
}

static void RF_BLE_CODE scan_window_adjust(struct ble4_hw *hw, u32 winsize, u32 interval)
{
    s32 slack = (s32)interval - (s32)winsize;
    hw->scan_idle = (u16)(slack > 0 ? slack : 0);
    if ((s32)winsize + 4 > (s32)interval) {
        if ((s32)interval > 4) winsize = interval - 4;
        else { interval = 5; winsize = 1; }
    }
    hw->scan_window = (u16)winsize;
    hw->scan_interval = (u16)interval;
}

static void RF_BLE_CODE __set_scan_device_filter_param(struct ble4_hw *hw, u8 policy)
{
    u8 enable = (policy == 1 || policy == 3) ? 8 : 0;
    hw->ble_fp.FILTERCNTL = (hw->ble_fp.FILTERCNTL & 247u) | enable;
}

static void RF_BLE_CODE __set_scan_active(struct ble4_hw *hw, u8 active)
{
    if (active && !hw->privacy_enable) __set_scan_req_enable(&hw->ble_fp);
    else __set_scan_req_disable(&hw->ble_fp);
}

static void RF_BLE_CODE __set_fixed_channel_index(struct ble_hw *hw, u8 index)
{ __set_ble_anchor_con(hw, 6, index | 8192u); }
static void RF_BLE_CODE __set_scan_channel_index(struct ble_hw *hw, u8 index)
{ __set_ble_anchor_con(hw, 6, index | 8448u); }

static struct ble_tx *RF_BLE_CODE hw_alloc_tx(u32 len)
{
    struct ble_tx *tx = hw_dma_alloc(1, len + 24);
    if (tx) memset(tx, 0, 20);
    return tx;
}

static void RF_BLE_CODE __scan_req_pdu(struct ble4_hw *hw, struct ble_tx *tx)
{
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    *bits = (*bits & (u8)-64) | 3;
    tx->len = 12;
    *bits = ((hw->local.addr_type << 6) & 64) | (*bits & (u8)-125);
    memcpy(tx->data, hw->local.addr, 6);
    memset(&tx->data[6], 0, 6);
}

static void RF_BLE_CODE ble_hw_tx(struct ble_hw *ble_hw, struct ble_tx *tx, u32 index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    u8 len = tx->len;
    volatile u16 *txap = index == 0 ? &hw->ble_fp.TXAHDR0 : &hw->ble_fp.TXAHDR1;
    volatile u16 *txdp = index == 0 ? &hw->ble_fp.TXDHDR0 : &hw->ble_fp.TXDHDR1;
    if (index == 0 && len) hw->ble_fp.TXPTR0 = (u16)PHY_TO_BLE(tx->data);
    else if (index != 0 && len) hw->ble_fp.TXPTR1 = (u16)PHY_TO_BLE(tx->data);
    u32 lenbits = len ? (u32)tx->len << 8 : 0;
    u16 header_bits;
    memcpy(&header_bits, bits, 2);
    *txap = ((header_bits >> 2) & 48u) | (header_bits & 15u);
    memcpy(&header_bits, bits, 2);
    *txdp = lenbits | (*txdp & 4u) | ((header_bits >> 6) & 8u) | ((header_bits >> 4) & 3u);
    __set_extended_tx_data(ble_hw, tx->chsel, index);
    if (ble_hw->tws) *txdp = ((ble_hw->phy.coded_s << 4) & 16u) | (*txdp & (u16)-17);
}

static void RF_BLE_CODE __set_hw_state(struct ble_hw *hw, u32 state, u32 latency_en, u32 latency)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    hw4->state = (u8)state;
    hw4->latency = (u16)latency;
    hw4->latency_en = (u8)latency_en;
    hw4->latency_substate = 0;
    __set_ble_anchor_con(hw, 2, (latency_en << 11) | (state << 12) | latency);
}

int RF_BLE_CODE le_hw_scanning_common(struct ble_hw *hw, struct ble_scan *scan)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    u8 info = log_tag_const_i_LE_BB;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]-> SCAN state\r\n");
    rf_set_bdaddr(&hw4->ble_fp, coded_set_scan);
    __set_le_prio(hw4, 17);
    __disable_adv_random(hw);
    u32 scan_window = scan->window;
    u32 scan_interval = scan->interval;
    scan_window_adjust(hw4, scan_window, scan_interval);
    if (config_btctler_le_hw_nums == 1 && (rf_vendor_config & 1) && scan->type == 0 && scan_interval == scan_window)
        hw4->scan_interval = 2;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]scan: %d,%d\n\r\n", hw4->scan_window, hw4->scan_interval);
    __set_scan_window(hw, hw4->scan_window, hw4->scan_interval);
    if (config_btctler_le_slave_multilink) mult_slave_or_master_set_scan_param(HW_ID(hw), hw4->scan_interval, hw4->scan_window);
    __set_scan_device_filter_param(hw4, scan->filter_policy);
    __set_scan_active(hw4, scan->type);
    u8 channel = rf_scan_fixed_channel;
    if (channel < 40) { hw4->adv_channel = channel; __set_fixed_channel_index(hw, channel); }
    else { hw4->adv_channel = 37; __set_scan_channel_index(hw, 37); }

    hw4->rx[0] = hw_alloc_rx(255);
    hw4->rx[1] = hw_alloc_rx(255);
    struct ble_rx *rx0 = hw4->rx[0];
    struct ble_rx *rx1 = hw4->rx[1];
    if (!rx1 || !rx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]RX DMA NULL\r\n");
        return -2;
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----rx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->rx[0] : %x / hw->rx[1] : %x\r\n", hw4->rx[0], hw4->rx[1]);
    }
    hw4->tx[0] = hw_alloc_tx(12);
    hw4->tx[1] = hw_alloc_tx(12);
    struct ble_tx *tx0 = hw4->tx[0];
    struct ble_tx *tx1 = hw4->tx[1];
    if (!tx1 || !tx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]TX DMA NULL\r\n");
        return -2;
    }
    __scan_req_pdu(hw4, tx0);
    __scan_req_pdu(hw4, hw4->tx[1]);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----tx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->tx[0] : %x / hw->tx[1] : %x\r\n", hw4->tx[0], hw4->tx[1]);
    }
    ble_hw_tx(hw, hw4->tx[0], 0);
    ble_hw_tx(hw, hw4->tx[1], 1);
    __set_hw_state(hw, 1, 0, 0);
    return 0;
}

static void RF_BLE_CODE __set_local_addr_match_enable(struct ble_param *fp)
{ fp->OPTCNTL &= (u16)-17; }

static void RF_BLE_CODE __connect_req_pdu(struct ble4_hw *hw, struct ble_conn *conn, struct ble_tx *tx)
{
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    *bits = (*bits & (u8)-64) | 5;
    tx->len = 34;
    *bits = ((hw->local.addr_type << 6) & 64) | (*bits & (u8)-123);
    memcpy(tx->data, hw->local.addr, 6);
    *bits = (*bits & (u8)-187) | (hw->peer.addr_type << 7);
    memcpy(&tx->data[6], hw->peer.addr, 6);
    memcpy(&tx->data[12], conn->ll_data.aa, 22);
    if (log_tag_const_i_LE_BB) printf_buf(tx->data, 34);
    set_tx_pdu_channel_selection_algorithm(tx);
}

int RF_BLE_CODE le_hw_initiating_common(struct ble_hw *hw, struct ble_conn *conn)
{
    u8 info = log_tag_const_i_LE_BB;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]-> INIT state\r\n");
    struct ble4_hw *hw4 = hw->ble4_hw;
    struct ble_param *fp = &hw4->ble_fp;
    __set_le_prio(hw4, 26);
    rf_set_bdaddr(fp, coded_set_scan);
    u32 scan_window = conn->scan_windows;
    u32 scan_interval = conn->scan_interval;
    if (config_vendor_le_bb && config_btctler_le_master_multilink) {
        conn->scan_windows = 1;
        conn->scan_interval = 1;
        scan_window = 1;
        scan_interval = 1;
    }
    scan_window_adjust(hw4, scan_window, scan_interval);
    __set_scan_window(hw, hw4->scan_window, hw4->scan_interval);
    if (config_btctler_le_slave_multilink)
        mult_slave_or_master_set_init_param(HW_ID(hw), hw4->interval, hw4->scan_window);
    u8 channel = rf_scan_fixed_channel;
    if (channel < 40) { hw4->adv_channel = channel; __set_fixed_channel_index(hw, channel); }
    else { hw4->adv_channel = 37; __set_scan_channel_index(hw, 37); }
    if (hw4->privacy_enable != 0) {
        fp->OPTCNTL |= 16;
        hw4->is_init_enter_conn_pass = 0;
    } else __set_local_addr_match_enable(fp);

    hw4->rx[0] = hw_alloc_rx(255);
    hw4->rx[1] = hw_alloc_rx(255);
    struct ble_rx *rx0 = hw4->rx[0], *rx1 = hw4->rx[1];
    if (!rx1 || !rx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]RX DMA NULL\r\n");
        return -2;
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----rx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->rx[0] : %x / hw->rx[1] : %x\r\n", hw4->rx[0], hw4->rx[1]);
    }
    hw4->tx[0] = hw_alloc_tx(34);
    hw4->tx[1] = hw_alloc_tx(34);
    struct ble_tx *tx0 = hw4->tx[0], *tx1 = hw4->tx[1];
    if (!tx1 || !tx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]TX DMA NULL\r\n");
        return -2;
    }
    __connect_req_pdu(hw4, conn, tx0);
    __connect_req_pdu(hw4, conn, hw4->tx[1]);
    ble_hw_tx(hw, hw4->tx[0], 0);
    ble_hw_tx(hw, hw4->tx[1], 1);
    __set_hw_state(hw, 3, 0, 0);
    find_interspace_between_connected(hw4);
    return 0;
}

void RF_BLE_CODE le_hw_suspend(struct ble_hw *hw)
{
    u8 *state = &hw->ble4_hw->sub_state;
    if (*state == 1 || *state == 3) return;
    *state = 1;
    ble_hw_disable(hw);
}

void RF_BLE_CODE le_hw_resume(struct ble_hw *hw, int slot)
{
    u8 *state = &hw->ble4_hw->sub_state;
    if ((*state & (u8)-2) == 2) return;
    *state = 2;
    ble_hw_enable(hw, slot);
}

u8 RF_BLE_CODE check_event_pending(void)
{
    for (u32 i = 0; i < 8; i++) {
        u32 mask = 1u << i;
        if (((*(volatile u32 *)1867816 & mask) & *(volatile u32 *)1867824) != 0) return 1;
        if (((*(volatile u32 *)1867852 & mask) & *(volatile u32 *)1867856) != 0) return 1;
    }
    return 0;
}

u32 RF_BLE_CODE ble_next_run_slot(struct ble_hw *hw) { return __get_clkn(hw); }

void RF_BLE_CODE bt_le_prio_settings(u8 setting)
{
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        if (bb_hdl->hw_base.inst[i] == 0) continue;
        struct ble4_hw *hw = bb_hdl->hw_base.hw[i].ble4_hw;
        u16 *bits = (u16 *)((u8 *)hw + offsetof(struct ble4_hw, widen) - 2);
        if (setting != 0) {
            __set_le_prio(hw, 0);
            hw = bb_hdl->hw_base.hw[i].ble4_hw;
            bits = (u16 *)((u8 *)hw + offsetof(struct ble4_hw, widen) - 2);
            *bits |= 8192u;
        } else {
            *bits &= (u16)-8193;
            hw = bb_hdl->hw_base.hw[i].ble4_hw;
            bits = (u16 *)((u8 *)hw + offsetof(struct ble4_hw, widen) - 2);
            __set_le_prio(hw, (u8)((*bits >> 8) & 31u));
        }
    }
}

u32 RF_BLE_CODE bt_le_next_event_time(int type) { (void)type; return -1; }

void RF_BLE_CODE le_hw_reset_power(void)
{
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        struct _hw_entity *entity = bb_hdl->hw_base.hw_entity + i;
        if (entity->used) ble_txpwr_normal_set(&entity->u.ble4_hw, 0, bt_get_pwr_max_level());
    }
}

void RF_BLE_CODE ble_rf_vendor_set_config(int config) { rf_vendor_config = config; }
int RF_BLE_CODE ble_rf_vendor_get_config(void) { return rf_vendor_config; }

static void INIT_LIST_HEAD(struct list_head *list)
{ list->next = list; list->prev = list; }

void RF_BLE_CODE le_hw_close(struct ble_hw *hw)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    if (!hw4) return;
    suspend_init(hw4);
    ble_power_put(hw);
    ble_hw_disable(hw);
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]HW close id %d\r\n", HW_ID(hw));
    hw_dma_free(hw4->tx[1]);
    hw_dma_free(hw4->tx[0]);
    hw4->tx[0] = 0;
    hw4->tx[1] = 0;
    ble_tx_drop_staged(hw);
    hw_dma_free(hw4->rx[1]);
    hw_dma_free(hw4->rx[0]);
    hw4->rx[0] = 0;
    hw4->rx[1] = 0;
    hw_dma_clear(&hw4->tx_head);
    hw_dma_clear(&hw4->tx_head_h);
    hw_dma_clear(&hw4->rx_head);
    if (!hw->ble4_hw) {
        if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]ble4_hw_frame_free NULL\r\n");
    } else {
        clear_all_anchor_cnt(hw);
        btctler_nv_memory_free(hw->ble4_hw->regs);
        struct ble_baseband_layer_handle *baseband = bb_hdl;
        u8 id = HW_ID(hw);
        baseband->hw_base.inst[id] = 0;
        baseband->number--;
        if (config_btctler_le_slave_multilink)
            mult_slave_or_master_del_conn_id(hw, HW_ID(hw));
        hw_entity_free(hw->ble4_hw);
        hw->ble4_hw = 0;
    }
    refind_interspace_between_connected();
    local_irq_disable();
    RF_BLE_CSYNC();
    struct ble_baseband_layer_handle *resume_base = bb_hdl;
    for (int i = 0; i < 8; i++) {
        if (resume_base->hw_base.inst[i] == 0) continue;
        struct ble_hw *other = &resume_base->hw_base.hw[i];
        struct ble4_hw *other4 = other->ble4_hw;
        if (!other4) continue;
        if ((other4->state == 1 || other4->state == 3) && other4->sub_state == 1) {
            u16 scan_interval = other4->scan_interval;
            int interval = other4->is_scan_multilink_state ? scan_interval + 48 : scan_interval;
            __set_scan_window(other, other4->scan_window, interval);
            le_hw_resume(other, other->ble4_hw->scan_idle);
            break;
        }
    }
    local_irq_enable();
}

static void RF_BLE_CODE le_hw_destroy(void)
{
    if (!bb_hdl) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]no hw to destroy\n\r\n");
        return;
    }
    for (int i = 0; i < 8; i++)
        if (bb_hdl->hw_base.inst[i]) le_hw_close(&bb_hdl->hw_base.hw[i]);
    hw_dma_destory();
    free(bb_hdl);
    bb_hdl = 0;
    *(volatile u32 *)1867776 = 0;
}

static struct ble_hw *RF_BLE_CODE le_hw_open(void)
{
    struct ble_hw *hw = ble_hw_alloc();
    struct ble4_hw *entity = hw_entity_alloc();
    hw->ble4_hw = entity;
    u8 id = HW_ID(hw);
    ble_hw_inst_set(id, entity);
    __set_hw_frame_init_4_2(hw);
    if (hw->ble4_hw) {
        hw->ble4_hw->regs = btctler_nv_memory_malloc(40);
        bb_hdl->number++;
    }
    if (!hw) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4230); printf("ASSERT-FAILD: hw hw alloc NULL"); cpu_assert_debug(); }
        else cpu_reset();
        return 0;
    }
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]HW open id %d\r\n", HW_ID(hw));
    struct ble4_hw *hw4 = hw->ble4_hw;
    INIT_LIST_HEAD(&hw4->rx_head.head);
    INIT_LIST_HEAD(&hw4->tx_head.head);
    INIT_LIST_HEAD(&hw4->tx_head_h.head);
    memset(hw4->rx, 0, 16);
    le_hw_standby(hw);
    return hw;
}

static void RF_BLE_CODE __set_channel_map(struct ble4_hw *hw, const u8 *channel)
{
    int frq_mid = 81, k = 0, num = 0;
    for (int i = 0; i < 37; i++) {
        if (i == 0) { frq_mid = 81; k = 0; }
        else if (i == 11) { frq_mid = 83; k = 2; }
        (void)get_bta_pll_bank((u8)(frq_mid + (i << 1)));
        u8 frq_idx = (u8)i;
        u8 frq_tbl = (u8)(k + ((i + 1) << 1));
        hw->ble_fp.FRQ_IDX0[i] = frq_idx;
        hw->ble_fp.FRQ_TBL0[i] = frq_tbl;
        if ((channel[i >> 3] & (1 << (i & 7))) != 0) {
            hw->ble_fp.FRQ_IDX1[num] = frq_idx;
            hw->ble_fp.FRQ_TBL1[num] = frq_tbl;
            num++;
        }
    }
    static const u8 special[3] = {0,24,78};
    for (int i = 0; i < 3; i++) {
        u8 v = special[i];
        (void)get_bta_pll_bank((u8)((v << 1) + 79));
        u8 index = (u8)(i + 37);
        hw->ble_fp.FRQ_IDX0[index] = index;
        hw->ble_fp.FRQ_TBL0[index] = v;
        hw->ble_fp.FRQ_IDX1[index] = index;
        hw->ble_fp.FRQ_TBL1[index] = v;
    }
    hw->ble_fp.CHMAP0 = (u16)(((u16)channel[1] << 8) | channel[0]);
    hw->ble_fp.CHMAP1 = (u16)(((u16)channel[3] << 8) | channel[2]);
    hw->ble_fp.CHMAP2 = (u16)((channel[4] & 31u) | ((u32)num << 5));
    hw->CHMAP[0] = hw->ble_fp.CHMAP0;
    hw->CHMAP[1] = hw->ble_fp.CHMAP1;
    hw->CHMAP[2] = hw->ble_fp.CHMAP2;
}

static void RF_BLE_CODE __set_scan_req_enable(struct ble_param *fp) { fp->FORMAT &= (u16)-257; }
static void RF_BLE_CODE __set_scan_req_disable(struct ble_param *fp) { fp->FORMAT |= 256; }

static void RF_BLE_CODE __tws_scan_req_pdu(struct ble_tx *tx)
{
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    *bits = (*bits & (u8)-64) | 3;
    tx->len = 0;
}

static void RF_BLE_CODE __set_init_end(struct ble_hw *hw) { __set_ble_anchor_con(hw, 9, 1); }

static void RF_BLE_CODE __set_adv_device_filter_param(struct ble_hw *hw, u8 policy)
{
    u8 scan = (policy == 1 || policy == 3) ? 8 : 0;
    u8 conn = (policy == 2 || policy == 3) ? 16 : 0;
    hw->ble4_hw->ble_fp.FILTERCNTL = (hw->ble4_hw->ble_fp.FILTERCNTL & 247u) | scan;
    hw->ble4_hw->ble_fp.FILTERCNTL = (hw->ble4_hw->ble_fp.FILTERCNTL & 239u) | conn;
}

static void RF_BLE_CODE __set_adv_random_slot(struct ble_hw *hw)
{ __set_ble_anchor_con(hw, 8, 49152); }

static void RF_BLE_CODE __adv_pdu(struct ble4_hw *hw, struct ble_tx *tx, u8 adv_type, u8 adv_len, const u8 *adv_data)
{
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    if (adv_type == 0) set_tx_pdu_channel_selection_algorithm(tx);
    if (adv_type == 0 || adv_type == 2 || adv_type == 6) {
        *bits = (*bits & (u8)-64) | (adv_type & 15u);
        tx->len = adv_len + 6;
        *bits = ((hw->local.addr_type << 6) & 64) | (*bits & (u8)-113);
        memcpy(tx->data, hw->local.addr, 6);
        memcpy(&tx->data[6], adv_data, adv_len);
    } else if (adv_type == 1) {
        set_tx_pdu_channel_selection_algorithm(tx);
        *bits = (*bits & (u8)-64) | 1;
        tx->len = 12;
        *bits = ((hw->local.addr_type << 6) & 64) | (*bits & (u8)-127);
        memcpy(tx->data, hw->local.addr, 6);
        *bits = (*bits & (u8)-191) | (hw->peer.addr_type << 7);
        memcpy(&tx->data[6], hw->peer.addr, 6);
    }
}

static int RF_BLE_CODE le_hw_advertising(struct ble_hw *hw, struct ble_adv *adv)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    if ((config_btctler_le_roles & 4) == 0) return -1;
    u8 info = log_tag_const_i_LE_BB;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]-> ADV state\r\n");
    __set_le_prio(hw4, rf_bt_bredr_is_sniff() ? 19 : 26);
    hw4->ble_fp.CRCWORD0 = 21845;
    hw4->ble_fp.CRCWORD1 = 85;
    rf_set_bdaddr(&hw4->ble_fp, coded_set_adv);
    hw4->ble_fp.WINCNTL0 = 50;
    hw4->ble_fp.WINCNTL1 = 0;
    hw4->ble_fp.WINCNTL2 = 50;
    hw4->window = 50;
    u16 interval = adv->interval;
    __set_interval(hw, interval * 625, 1);
    __set_adv_device_filter_param(hw, adv->filter_policy);
    u32 vendor = rf_vendor_config;
    int pdu_interval = (vendor & 4) ? 938 : adv->pdu_interval * 625;
    hw4->ble_fp.ADVIDX = pdu_interval | ((u32)adv->pkt_cnt << 14);
    if ((vendor & 2) == 0 && adv->adv_type != 2 && adv->interval > 10) {
        if (config_btctler_le_slave_multilink) {
            if (mult_slave_or_master_get_con_num() == 0) __set_adv_random_slot(hw);
            else __disable_adv_random(hw);
        } else __set_adv_random_slot(hw);
    }
    __set_adv_channel_index(hw, adv->channel_map);
    hw4->rx[0] = hw_alloc_rx(255);
    hw4->rx[1] = hw_alloc_rx(255);
    struct ble_rx *rx0 = hw4->rx[0], *rx1 = hw4->rx[1];
    if (!rx1 || !rx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]RX DMA NULL\r\n");
        return -2;
    }
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----rx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->rx[0] : %x / hw->rx[1] : %x\r\n", hw4->rx[0], hw4->rx[1]);
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
    hw4->tx[0] = hw_alloc_tx(37);
    hw4->tx[1] = hw_alloc_tx(37);
    struct ble_tx *tx0 = hw4->tx[0], *tx1 = hw4->tx[1];
    if (!tx1 || !tx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]TX DMA NULL\r\n");
        return -2;
    }
    __adv_pdu(hw4, tx0, adv->adv_type, hw4->adv_len, hw4->adv_data);
    tx1 = hw4->tx[1];
    if (adv->adv_type == 1) __adv_pdu(hw4, tx1, 1, hw4->adv_len, hw4->adv_data);
    else {
        u8 *bits = (u8 *)tx1 + offsetof(struct ble_tx, len) - 2;
        u8 len = hw4->scan_rsp_len;
        *bits = (*bits & (u8)-64) | 4;
        tx1->len = len + 6;
        *bits = ((hw4->local.addr_type << 6) & 64) | (*bits & (u8)-124);
        memcpy(tx1->data, hw4->local.addr, 6);
        memcpy(&tx1->data[6], hw4->scan_rsp_data, len);
    }
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----tx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->tx[0] : %x / hw->tx[1] : %x\r\n", hw4->tx[0], hw4->tx[1]);
    }
    ble_hw_tx(hw, hw4->tx[0], 0);
    ble_hw_tx(hw, hw4->tx[1], 1);
    hw4->data_flush = 0;
    __set_local_addr_match_enable(&hw4->ble_fp);
    if (config_btctler_le_slave_multilink) {
        if (mult_slave_or_master_get_con_num() == 0) ble_power_get(hw);
        __set_hw_state(hw, 2, 0, 0);
        u8 id = HW_ID(hw);
        mult_slave_or_master_set_adv_interval(id, adv->interval);
        if (mult_slave_or_master_get_con_num() == 0) ble_hw_enable(hw, 1);
    } else {
        ble_power_get(hw);
        __set_hw_state(hw, 2, 0, 0);
        ble_hw_enable(hw, 1);
    }
    hw4->ble_fp.FORMAT |= 4;
    return 0;
}

static int RF_BLE_CODE tws_le_hw_advertising(struct ble_hw *hw, struct ble_tws_adv *adv)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    u8 info = log_tag_const_i_LE_BB;
    u32 aa = adv->aa;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]-> TWS ADV state : 0x%x\r\n", aa);
    hw->tws = 1;
    __set_le_prio(hw4, 26);
    aa = adv->aa;
    hw4->ble_fp.BDADDR0 = (u16)(aa >> 16);
    hw4->ble_fp.BDADDR1 = (u16)aa;
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]TWS BDADDR0 : 0x%x\r\n", hw4->ble_fp.BDADDR0);
        log_print(2, NULL, "[Info]: [LE_BB]TWS BDADDR1 : 0x%x\r\n", hw4->ble_fp.BDADDR1);
    }
    hw4->ble_fp.CRCWORD0 = 21845;
    hw4->ble_fp.CRCWORD1 = 85;
    hw4->ble_fp.WINCNTL0 = 60;
    hw4->ble_fp.WINCNTL1 = 0;
    hw4->ble_fp.WINCNTL2 = 60;
    hw4->window = 60;
    __set_interval(hw, 1310719375, 1);
    __set_hw_state(hw, 2, 0, 0);
    __set_adv_device_filter_param(hw, 0);
    hw4->ble_fp.ADVIDX = ((u32)adv->pkt_cnt << 14) | 1250u;
    __set_adv_random_slot(hw);
    __set_adv_channel_index(hw, adv->channel_map);
    hw4->rx[0] = hw_alloc_rx(64);
    hw4->rx[1] = hw_alloc_rx(64);
    struct ble_rx *rx0 = hw4->rx[0], *rx1 = hw4->rx[1];
    if (!rx1 || !rx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]RX DMA NULL\r\n");
        return -2;
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----rx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->rx[0] : %x / hw->rx[1] : %x\r\n", hw4->rx[0], hw4->rx[1]);
    }
    hw4->tx[0] = hw_alloc_tx(251);
    hw4->tx[1] = hw_alloc_tx(251);
    struct ble_tx *tx0 = hw4->tx[0], *tx1 = hw4->tx[1];
    if (!tx1 || !tx0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]TX DMA NULL\r\n");
        return -2;
    }
    u8 *bits0 = (u8 *)tx0 + offsetof(struct ble_tx, len) - 2;
    *bits0 &= (u8)-64;
    tx0->len = 0;
    tx1 = hw4->tx[1];
    u8 *bits1 = (u8 *)tx1 + offsetof(struct ble_tx, len) - 2;
    u8 rsp_len = hw4->scan_rsp_len;
    *bits1 = (*bits1 & (u8)-64) | 4;
    tx1->len = rsp_len;
    *bits1 = ((hw4->local.addr_type << 6) & 64) | (*bits1 & (u8)-124);
    memcpy(tx1->data, hw4->scan_rsp_data, rsp_len);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----tx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->tx[0] : %x / hw->tx[1] : %x\r\n", hw4->tx[0], hw4->tx[1]);
    }
    ble_hw_tx(hw, hw4->tx[0], 0);
    ble_hw_tx(hw, hw4->tx[1], 1);
    hw->phy.coded_s = 1;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)-1793) | 512;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)-14337) | 4096;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & 16383) | 16384;
    hw4->ble_fp.TXDHDR0 |= 16;
    hw4->ble_fp.TXDHDR1 |= 16;
    hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u16)-128) | 20;
    hw4->ble_fp.EXTIFSDRF1 = (hw4->ble_fp.EXTIFSDRF1 & (u16)-30721) | 16384;
    hw4->ble_fp.FORMAT &= (u16)-9;
    hw4->ble_fp.FORMAT |= 1;
    hw4->ble_fp.LOCALADRL = 0;
    hw4->ble_fp.LOCALADRM = 0;
    hw4->ble_fp.LOCALADRU = 0;
    ble_hw_enable(hw, 1);
    hw4->ble_fp.FORMAT |= 4;
    return 0;
}

static int RF_BLE_CODE le_hw_scanning(struct ble_hw *hw, struct ble_scan *scan)
{
    if ((config_btctler_le_roles & 8) == 0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]ROLE not suppport\r\n");
        return -1;
    }
    struct ble4_hw *hw4 = hw->ble4_hw;
    int result = le_hw_scanning_common(hw, scan);
    if (result != 0) return result;
    u8 type = scan->type;
    hw4->backoff.active = type;
    u8 active = type == 0;
    if (!active) {
        hw4->backoff.send = 0;
        hw4->backoff.upper_limit = 1;
        hw4->backoff.backoff_count = 1;
        hw4->backoff.backoff_pseudo_cnt = 1;
        hw4->backoff.consecutive_fail = 0;
        hw4->backoff.consecutive_succ = 0;
    }
    if ((rf_vendor_config & 1) && active && hw4->scan_interval == 2) {
        *(volatile u32 *)1867788 = (*(volatile u32 *)1867788 & (u32)-1024) | 62;
        hw4->ble_fp.OPTCNTL |= 63;
    }
    if (config_btctler_le_slave_multilink) {
        if (mult_slave_or_master_get_con_num() == 0) ble_power_get(hw);
        if (mult_slave_or_master_get_con_num() != 0) return 0;
    } else ble_power_get(hw);
    if (bb_hdl->number == 1) { hw4->sub_state = 0; ble_hw_enable(hw, 1); }
    else { hw4->sub_state = 1; ble_slot_schedule(); }
    return 0;
}

static int RF_BLE_CODE le_hw_initiating(struct ble_hw *hw, struct ble_conn *conn)
{
    if ((config_btctler_le_roles & 16) == 0) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]ROLE not suppport\r\n");
        return -1;
    }
    struct ble4_hw *hw4 = hw->ble4_hw;
    int result = le_hw_initiating_common(hw, conn);
    if (result != 0) return result;
    if (config_btctler_le_slave_multilink) {
        if (mult_slave_or_master_get_con_num() == 0) ble_power_get(hw);
        if (mult_slave_or_master_get_con_num() != 0) { __set_init_end(hw); return 0; }
    } else ble_power_get(hw);
    if (bb_hdl->number == 1) { hw4->sub_state = 0; ble_hw_enable(hw, 1); }
    else { hw4->sub_state = 1; ble_slot_schedule(); }
    __set_init_end(hw);
    return 0;
}

static void RF_BLE_CODE le_hw_handler_register(struct ble_hw *hw, u8 *priv, struct ble_handler *handler)
{ hw->ble4_hw->priv = priv; hw->ble4_hw->handler = handler; }

static void RF_BLE_CODE le_hw_send_packet(struct ble_hw *ble_hw, u8 llid, const u8 *packet, int len, int tx_octets)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    if (config_asser) {
        if (hw->tx_octets < len) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4321);
            printf("ASSERT-FAILD: len <= hw->tx_octets %s\n", "le_hw_send_packet");
            cpu_assert_debug();
        }
    } else if (hw->tx_octets < len) cpu_reset();
    struct ble_tx *tx = hw_alloc_tx(tx_octets);
    if (!tx) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4324); printf("ASSERT-FAILD: tx != NULL "); cpu_assert_debug(); }
        else cpu_reset();
    }
    memset(tx, 0, 20);
    u8 *bits = (u8 *)tx + offsetof(struct ble_tx, len) - 2;
    *bits = (llid << 4) & 48;
    tx->len = (u8)len;
    if (len >= 256) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4334); printf("ASSERT-FAILD: len <= HW_DATA_PDU_SIZE "); cpu_assert_debug(); }
        else cpu_reset();
    }
    memcpy(tx->data, packet, len);
    hw_dma_push(&hw->tx_head, (struct _dma *)tx);
    __set_le_prio(hw, 28);
    __hw_tx_process(ble_hw);
    __set_latency_hold_cnt(0, hw, 2);
    if (config_btctler_le_slave_multilink) {
        if (mult_slave_or_master_check_up_point() == 0xff && hw->mult_latency_temp == 0)
            ble_latency_manualhold(ble_hw);
    } else ble_latency_manualhold(ble_hw);
}

static void RF_BLE_CODE le_hw_send_packet_high(struct ble_hw *ble_hw, void (*callback)(void *),
                                                void (*ack_callback)(void *), u8 llid,
                                                const u8 *packet, int len)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_rx *buf;
    if (config_vendor_le_bb == 0) {
        if (hw->tx_octets < len) {
            if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4369); printf("ASSERT-FAILD: len <= hw->tx_octets %s\n", "le_hw_send_packet_high"); cpu_assert_debug(); }
            else cpu_reset();
        }
        buf = hw_alloc_rx(27);
    } else buf = hw_alloc_rx(len);
    if (!buf) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4373); printf("ASSERT-FAILD: tx != NULL "); cpu_assert_debug(); }
        else cpu_reset();
    }
    memset(buf, 0, 20);
    memcpy((u8 *)buf + 8, &callback, sizeof(callback));
    memcpy((u8 *)buf + 12, &ack_callback, sizeof(ack_callback));
    u16 bits = (u16)((llid << 4) & 48u);
    memcpy(buf->head, &bits, sizeof(bits));
    buf->head[2] = (u8)len;
    memcpy(buf + 1, packet, len);
    hw_dma_push(&hw->tx_head_h, (struct _dma *)buf);
    __set_le_prio(hw, 30);
    __hw_tx_process(ble_hw);
    __set_latency_hold_cnt(0, hw, 2);
    if (config_btctler_le_slave_multilink) {
        if (mult_slave_or_master_check_up_point() == 0xff && hw->mult_latency_temp == 0)
            ble_latency_manualhold(ble_hw);
    } else ble_latency_manualhold(ble_hw);
}

/* Ours: links whose oldest received data waits for room in the host
 * (btctrlermod_host_acl_room), a bit per HW_ID.  It stays queued, still
 * encrypted (decrypting is done once, as it is taken), and the link reports
 * nothing to take (ioctrl 19), so the link layer's thread sleeps until the
 * host wakes it. */
static u32 rx_held;

static u8 RF_BLE_CODE __ble_rx_buf_process_conn(struct ble4_hw *hw, int index, int ok);

/* Ours: on a connection, a receive buffer the radio went without (the pool
 * was empty in the receive interrupt) as soon as one is free.  Without it
 * the radio receives nothing more, so no interrupt comes to try again, and
 * the link timed out (host flow control holding received data). */
static void RF_BLE_CODE rx_rearm(struct ble4_hw *hw)
{
    if ((hw->state & (u8)~1u) != 6) return;
    local_irq_disable();
    for (int i = 0; i < 2; i++)
        if (!hw->rx[i]) __ble_rx_buf_process_conn(hw, i, 1);
    local_irq_enable();
}

static void RF_BLE_CODE le_hw_upload_data(struct ble_hw *ble_hw, void (*rx_handler)(void *, struct ble_rx *))
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_rx *rx = btctrlermod_dma_first(&hw->rx_head);
    u32 bit = 1u << HW_ID(ble_hw);
    if (rx && config_btctler_hci_standard && (rx->llid == 1 || rx->llid == 2) && !btctrlermod_host_acl_room()) {
        rx_held |= bit;
        return;
    }
    rx_held &= ~bit;
    rx = (struct ble_rx *)hw_dma_pop(&hw->rx_head);
    if (!rx) return;
    ble_hw_decrypt(hw, rx);
    if ((config_btctler_le_roles & 8) && hw->state == 1 && hw->scan_rx_packets)
        hw->scan_rx_packets--;
    if (config_btctler_hci_standard || (rx->llid != 1 && rx->llid != 2)) {
        rx_handler(hw->priv, rx);
        hw_dma_free(rx);
        rx_rearm(hw);
    } else rx_handler(hw->priv, rx);
    /* Ours: one packet a pass, and the link layer's thread runs a pass per
     * message to the controller's task.  Each received packet normally
     * brings one; a starved radio (rx_rearm) brings none, so ask for the
     * next pass while more wait. */
    if (!hw_dma_empty(&hw->rx_head)) ll_thread_resume();
}

static int RF_BLE_CODE le_hw_is_init_enter_conn(struct ble_hw *hw)
{ return hw->ble4_hw->is_init_enter_conn_pass; }

static void RF_BLE_CODE __set_event_instant(struct ble_hw *hw, int instant, int type)
{
    struct ble4_hw *frame = hw->ble4_hw;
    __set_ble_anchor_con(hw, 5, (u32)instant);
    frame->instant = (u16)instant;
    frame->instant_valid = (u8)(type + 1);
}

static void RF_BLE_CODE __set_hw_tx_enable(struct ble_param *fp)
{ fp->INTFRAME &= (u16)~8u; }

static void RF_BLE_CODE __set_local_addr(struct ble4_hw *hw, u8 type, const u8 *addr)
{
    hw->ble_fp.FORMAT |= 8;
    hw->local.addr_type = type != 0;
    memcpy(hw->local.addr, addr, 6);
    hw->ble_fp.LOCALADRL = (u16)(addr[1] << 8 | addr[0]);
    hw->ble_fp.LOCALADRM = (u16)(addr[3] << 8 | addr[2]);
    hw->ble_fp.LOCALADRU = (u16)(addr[5] << 8 | addr[4]);
}

static void RF_BLE_CODE __set_peer_addr(struct ble4_hw *hw, u8 type, const u8 *addr)
{
    hw->peer.addr_type = type != 0;
    memcpy(hw->peer.addr, addr, 6);
    hw->ble_fp.TARGETADRL = (u16)(addr[1] << 8 | addr[0]);
    hw->ble_fp.TARGETADRM = (u16)(addr[3] << 8 | addr[2]);
    hw->ble_fp.TARGETADRU = (u16)(addr[5] << 8 | addr[4]);
}

static void RF_BLE_CODE __set_remote_addr_match_enable(struct ble_param *fp)
{ fp->OPTCNTL &= (u16)~8u; }

static void RF_BLE_CODE __set_winoffset(struct ble_hw *hw, int offset)
{ __set_ble_anchor_con(hw, 4, offset ? (u32)offset | 0x8000u : 0); }

static void RF_BLE_CODE __tx_data_pdu(struct ble_tx *tx, const u8 *data, u8 length)
{
    if (tx->type == 0 || tx->type == 2 || tx->type == 6) {
        tx->len = (u8)(length + 6);
        memcpy(&tx->data[6], data, length);
    }
}

static void atomic_set(atomic_t *v, int i)
{
    local_irq_disable();
    RF_BLE_CSYNC();
    v->counter = i;
    local_irq_enable();
}

static void RF_BLE_CODE backoff_pseudo(struct ble4_hw *hw)
{
    u16 random;
    pseudo_random_genrate((u8 *)&random, 2);
    u32 value = random % hw->backoff.upper_limit;
    hw->backoff.backoff_pseudo_cnt = value ? value : 1;
}

static u8 RF_BLE_CODE __ble_rx_buf_process_conn(struct ble4_hw *hw, int index, int ok)
{
    if (ok) {
        struct ble_rx *rx = hw_alloc_rx(255);
        hw->rx[index] = rx;
        if (!rx) return 1;
        u32 ptr = PHY_TO_BLE(rx->data);
        if (index) hw->ble_fp.RXPTR1 = (s16)ptr;
        else hw->ble_fp.RXPTR0 = (s16)ptr;
        if (index) hw->ble_fp.RXBUF1CNTL &= (u16)~1u;
        else hw->ble_fp.RXBUF0CNTL &= (u16)~1u;
    }
    return (u8)ok;
}

u8 RF_BLE_CODE __hw_set_widen_drift(struct ble_hw *hw, u32 widen, u32 window)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    u32 div = widen / 625;
    u32 rem = widen % 625;
    u32 clkn = __get_clkn(hw);
    u32 phase_delta = (u32)(625 - (*(volatile u32 *)1867788 & 1023) - (*(volatile u32 *)1867796 & 255)) & 65535;
    u32 raw_anchor = __get_ble_anchor_con(hw, 7);
    int anchor = (s16)raw_anchor;
    if (anchor & 2048) anchor |= 63488;
    anchor = (s16)anchor;
    int phase = anchor + (int)phase_delta;
    int drift_slot;
    int bitoff;
    int sub = phase - (int)rem;
    if (phase > 624 && sub < 625) {
        drift_slot = (div & 65535) + 1;
        bitoff = anchor - (int)rem;
    } else if ((u32)phase < rem) {
        drift_slot = (div & 65535) + 1;
        bitoff = anchor - (int)rem;
    } else if (phase < 0 && sub < -625) {
        drift_slot = (div & 65535) + 1;
        bitoff = 625 - (int)rem + anchor;
    } else {
        drift_slot = (int)div;
        bitoff = anchor - (int)rem;
    }
    u32 target = (u32)drift_slot & 65535u;
    if (target >= clkn) { putchar('X'); return 0; }
    __set_ble_anchor_con(hw, 16, target);
    __set_ble_anchor_con(hw, 7, (u32)(s32)(s16)bitoff | 0x8000u);
    hw4->ble_fp.WINCNTL0 = (u16)window;
    hw4->ble_fp.WINCNTL1 = (u16)(window >> 16);
    return 1;
}

static void RF_BLE_CODE ble_latency_manualhold(struct ble_hw *hw)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    local_irq_disable();
    RF_BLE_CSYNC();
    if (!hw4->latency_en) { local_irq_enable(); return; }
    __set_latency_disable(hw);
    local_irq_enable();
}

static void RF_BLE_CODE __attribute__((noinline)) ble_hw_encrpty(struct ble4_hw *hw, struct ble_tx *tx)
{
    u8 tag[16], nonce[13], pt[275], head;
    if ((*(const u64 *)&config_btctler_le_features & 1u) == 0) return;
    memset(tag, 0, sizeof(tag));
    memset(pt, 0, sizeof(pt));
    head = tx->llid & 3;
    if (!hw->encrypt.tx_enable) return;
    u32 counter = hw->encrypt.tx_counter_l;
    nonce[0] = (u8)counter;
    nonce[1] = (u8)(counter >> 8);
    nonce[2] = (u8)(counter >> 16);
    nonce[3] = (u8)(counter >> 24);
    nonce[4] = hw->encrypt.tx_counter_h;
    if (hw->state == 6) nonce[4] |= 0x80;
    memcpy(&nonce[5], hw->encrypt.iv, 8);
    ccm_memory_v2(hw->encrypt.skd, nonce, &head, 1, tx->data, tx->len, pt, tag, 4, 0);
    counter = hw->encrypt.tx_counter_l + 1;
    hw->encrypt.tx_counter_l = counter;
    if (config_asser) {
        if (counter == 0xffffffffu) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 1940);
            printf("ASSERT-FAILD: encrypt->tx_counter_l != 0xffffffff %s\n", "ble_hw_encrpty");
            cpu_assert_debug();
        }
    } else if (counter == 0xffffffffu) cpu_reset();
    u8 len = tx->len;
    memcpy(tx->data, pt, len);
    memcpy(&tx->data[len], tag, 4);
    tx->len = (u8)(len + 4);
}

static void RF_BLE_CODE __attribute__((noinline)) ble_hw_decrypt(struct ble4_hw *hw, struct ble_rx *rx)
{
    u8 tag[16], nonce[13], pt[275], head;
    if ((*(const u64 *)&config_btctler_le_features & 1u) == 0) return;
    if (!hw->encrypt.rx_enable || rx->len == 0) return;
    memset(tag, 0, sizeof(tag));
    memset(pt, 0, sizeof(pt));
    head = rx->llid & 3;
    u32 counter = hw->encrypt.rx_counter_l;
    nonce[0] = (u8)counter;
    nonce[1] = (u8)(counter >> 8);
    nonce[2] = (u8)(counter >> 16);
    nonce[3] = (u8)(counter >> 24);
    nonce[4] = hw->encrypt.rx_counter_h;
    if (hw->state == 7) nonce[4] |= 0x80;
    memcpy(&nonce[5], hw->encrypt.iv, 8);
    counter++;
    hw->encrypt.rx_counter_l = counter;
    if (config_asser) {
        if (counter == 0xffffffffu) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 1973);
            printf("ASSERT-FAILD: encrypt->rx_counter_l != 0xffffffff %s\n", "ble_hw_decrypt");
            cpu_assert_debug();
        }
    } else if (counter == 0xffffffffu) cpu_reset();
    rx->len -= 4;
    ccm_memory_v2(hw->encrypt.skd, nonce, &head, 1, pt, rx->len, rx->data, tag, 4, 1);
    memcpy(rx->data, pt, rx->len);
    if (memcmp(tag, &rx->data[rx->len], 4) != 0 && log_tag_const_e_LE_BB) {
        log_print(4, NULL, "<Error>: [LE_BB]*******NO Match :%d / %d / %d / %d*****: \r\n",
                  rx->llid & 3, rx->len, hw->encrypt.rx_counter_l, hw->encrypt.rx_counter_h);
        printf_buf(&rx->data[rx->len], 4);
        printf_buf(tag, 4);
        log_print(4, NULL, "<Error>: [LE_BB]rx->len=0x%x\r\n", rx->len);
        printf_buf(rx->data, rx->len + 4);
    }
}

/* TX slot ownership.  TXTOG is the slot the baseband sends next; it moves
 * on when the peer acknowledges data from it.  A completed current slot sends
 * an empty PDU, whose acknowledgement does not move TXTOG, so data waiting in
 * the other slot would wait for ever.  Data written into the current slot
 * while such an empty PDU waits for its acknowledgement is lost when that
 * comes.  And an empty PDU that is not acknowledged is sent again from the
 * slot's descriptor.  So:
 *   - a retired descriptor is reset to an empty PDU before its buffer is freed;
 *   - while the current slot is busy, the next packet goes into the other one;
 *   - a completed current slot is refilled only between connection events.
 * A packet that loses the race against a toggle is already encrypted; it
 * waits in tx_staged for the next slot it may use. */
static struct ble_tx *tx_staged[8] __attribute__((section(".ble_rf_bss")));

static struct ble_tx **RF_BLE_CODE ble_tx_staged(struct ble_hw *ble_hw)
{
    u8 id = HW_ID(ble_hw);
    return id < 8 ? &tx_staged[id] : 0;
}

static void RF_BLE_CODE ble_tx_drop_staged(struct ble_hw *ble_hw)
{
    struct ble_tx **staged = ble_tx_staged(ble_hw);
    if (staged && *staged) { hw_dma_free(*staged); *staged = 0; }
}

static void RF_BLE_CODE ble_tx_empty_slot(struct ble4_hw *hw, u8 index)
{
    volatile u16 *header = index ? &hw->ble_fp.TXDHDR1 : &hw->ble_fp.TXDHDR0;
    *header = (*header & 0x14u) | 1u;
    if (index) hw->ble_fp.TXAHDR1 = 0;
    else hw->ble_fp.TXAHDR0 = 0;
}

static void RF_BLE_CODE ble_tx_retire(struct ble_hw *ble_hw, u8 index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    if (!hw->tx[index]) return;
    ble_tx_empty_slot(hw, index);
    const struct ble_handler *handler = hw->handler;
    if (handler && handler->tx_probe_handler) handler->tx_probe_handler(hw->priv, hw->tx[index]);
    struct ble_tx *tx = hw->tx[index];
    if (tx->ack_callback) tx->ack_callback(hw->priv);
    hw_dma_free(hw->tx[index]);
    hw->tx[index] = 0;
}

/* The next packet to send, encrypted: a staged one first, then control, then
 * data. */
static struct ble_tx *RF_BLE_CODE ble_tx_next(struct ble_hw *ble_hw)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_tx **staged = ble_tx_staged(ble_hw);
    struct ble_tx *tx = staged ? *staged : 0;
    if (tx) *staged = 0;
    else {
        if (!hw->tx_acl_c_block) tx = (struct ble_tx *)hw_dma_pop(&hw->tx_head_h);
        if (!tx && !hw->tx_acl_block) tx = (struct ble_tx *)hw_dma_pop(&hw->tx_head);
        if (!tx) return 0;
        ble_hw_encrpty(hw, tx);
    }
    tx->md = (config_vendor_le_bb & 8) == 0 && hw_dma_empty(&hw->tx_head) == 0;
    return tx;
}

/* Between connection events, with at least a slot (two on a central, which
 * sends first) before the next anchor: the margin is not measured. */
static int RF_BLE_CODE ble_tx_idle(struct ble_hw *ble_hw)
{
    if (*(volatile u32 *)1867832 & 2u) return 0;
    u32 low = __get_ble_anchor_con(ble_hw, 0);
    u32 high = __get_ble_anchor_con(ble_hw, 14);
    u32 slots = ((high << 16) & 0x00ff0000u) | (low & 0xffffu);
    return slots >= (ble_hw->ble4_hw->state == 6 ? 2u : 1u) && (*(volatile u32 *)1867832 & 2u) == 0;
}

/* Hand slot index to the baseband: the current slot only between events,
 * the other only while it is still the other. */
static int RF_BLE_CODE ble_tx_commit(struct ble_hw *ble_hw, struct ble_tx *tx, u8 index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_tx **staged = ble_tx_staged(ble_hw);
    ble_hw_tx(ble_hw, tx, index);
    local_irq_disable();
    RF_BLE_CSYNC();
    int ok = !staged || (hw->ble_fp.TXTOG & 1) != index || ble_tx_idle(ble_hw);
    if (ok) {
        if (index) hw->ble_fp.TXBUF1CNTL &= (u16)~1u;
        else hw->ble_fp.TXBUF0CNTL &= (u16)~1u;
    }
    local_irq_enable();
    if (!ok) {
        ble_tx_empty_slot(hw, index);
        *staged = tx;
        return 0;
    }
    hw->tx[index] = tx;
    u16 intframe = hw->ble_fp.INTFRAME;
    hw->ble_fp.INTFRAME = (u16)((tx->md ? 64u : 0u) | (intframe & (u16)~64u));
    if (tx->callback) tx->callback(hw->priv);
    return 1;
}

/* A busy slot whose packet says no more data gets MD when more is queued. */
static void RF_BLE_CODE ble_tx_update_md(struct ble_hw *ble_hw, u8 index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_tx *tx = hw->tx[index];
    if (!tx || tx->md) return;
    int empty = hw_dma_empty(&hw->tx_head);
    struct ble_tx **staged = ble_tx_staged(ble_hw);
    if (staged && *staged) empty = 0;
    tx->md = (config_vendor_le_bb & 8) == 0 && empty == 0;
    ble_hw_tx(ble_hw, tx, index);
}

/* The raw three-bit switch selects the second sample only for 1, 3, 7. */
static u8 tx_soft_snapshot(u8 first, u8 second)
{
    u8 diff = first ^ second;
    return diff == 1 || diff == 3 || diff == 7 ? second : first;
}

/* Fill a completed slot with the next packet; 0 when there is none. */
static int RF_BLE_CODE ble_tx_fill(struct ble_hw *ble_hw, u8 index)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    struct ble_tx *tx = ble_tx_next(ble_hw);
    if (!tx) {
        hw->ble_fp.INTFRAME &= (u16)~64u;
        __set_extended_empty_tx_data(ble_hw);
        return 0;
    }
    return ble_tx_commit(ble_hw, tx, index);
}

static void RF_BLE_CODE ble_tx_service(struct ble_hw *ble_hw)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    u8 first = (u8)((hw->ble_fp.TXTOG & 1) | ((hw->ble_fp.TXBUF0CNTL << 1) & 2) |
                    ((hw->ble_fp.TXBUF1CNTL << 2) & 4));
    u8 second = (u8)((hw->ble_fp.TXTOG & 1) | ((hw->ble_fp.TXBUF0CNTL << 1) & 2) |
                     ((hw->ble_fp.TXBUF1CNTL << 2) & 4));
    u8 snapshot = tx_soft_snapshot(first, second);
    u8 current = snapshot & 1, next = current ^ 1;
    u8 current_done = (snapshot >> (current + 1)) & 1;
    u8 next_done = (snapshot >> (next + 1)) & 1;
    if (current_done) ble_tx_retire(ble_hw, current);
    else ble_tx_update_md(ble_hw, current);
    if (next_done) ble_tx_retire(ble_hw, next);
    else ble_tx_update_md(ble_hw, next);
    if (!current_done) {
        if (next_done) ble_tx_fill(ble_hw, next);
        return;
    }
    /* Only empty PDUs go out now; without MD the event can end. */
    if (!ble_tx_idle(ble_hw)) {
        hw->ble_fp.INTFRAME &= (u16)~64u;
        return;
    }
    if (next_done && ble_tx_fill(ble_hw, current)) ble_tx_fill(ble_hw, next);
}

/* Interrupts that find the lock taken leave a request (2); the holder runs
 * again so no completion is left unprocessed. */
static void RF_BLE_CODE __hw_tx_process(struct ble_hw *ble_hw)
{
    struct ble4_hw *hw = ble_hw->ble4_hw;
    if (!hw || (config_btctler_le_roles & 3) == 0) return;
    if ((hw->state & (u8)~1u) != 6) return;
    local_irq_disable();
    RF_BLE_CSYNC();
    RF_BLE_CSYNC();
    if (*(volatile int *)&hw->tx_lock.counter != 0) {
        atomic_set(&hw->tx_lock, 2);
        local_irq_enable();
        return;
    }
    atomic_set(&hw->tx_lock, 1);
    local_irq_enable();
    for (;;) {
        ble_tx_service(ble_hw);
        local_irq_disable();
        RF_BLE_CSYNC();
        int again = *(volatile int *)&hw->tx_lock.counter == 2;
        atomic_set(&hw->tx_lock, again ? 1 : 0);
        local_irq_enable();
        if (!again) break;
    }
}

static void RF_BLE_CODE rx_mark_invalid(struct ble_rx *rx)
{
    rx->len = 0;
    rx->llid = 1;
    rx->invalid = 1;
}

static void RF_BLE_CODE __hw_rx_buf_process(struct ble_hw *hw, struct ble_rx *rx, u8 index)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    struct ble_param *fp = &hw4->ble_fp;
    u16 ahdr = *(u16 *)(index ? &fp->RXAHDR1 : &fp->RXAHDR0);
    u16 dhdr = *(u16 *)(index ? &fp->RXDHDR1 : &fp->RXDHDR0);
    u16 status = *(u16 *)(index ? &fp->RXSTAT1 : &fp->RXSTAT0);
    u16 bits;
    u8 *header = (u8 *)rx + offsetof(struct ble_rx, len) - 2;
    memcpy(&bits, header, 2);
    bits = (u16)(((ahdr >> 1) & 64) | (ahdr & 15) | ((ahdr << 1) & 128) |
                 ((dhdr << 6) & 1024) | (bits & (u16)-1232));
    memcpy(header, &bits, 2);
    record_rx_chsel_algorithm(rx, (ahdr >> 5) & 1);
    rx->len = (u8)(dhdr >> 8);
    memcpy(&bits, header, 2);
    bits = (u16)(((dhdr << 6) & 512) | ((dhdr << 4) & 48) | ((dhdr << 6) & 256) |
                 (bits & (u16)-817));
    memcpy(header, &bits, 2);
    u8 status_nibble = status & 15;
    hw4->rx_ack = (status >> 8) & 1;
    if (hw4->rx_ack) hw4->rx_packet_valid |= 1;
    rx->event_count = fp->EVTCOUNT;
    rx->channel = (u8)fp->LASTCHMAP;
    rx->rssi = ble_analog_get_rssi(fp);
    rx->invalid = 0;
    if ((rf_vendor_config & 1) && hw4->state == 1 && !hw4->backoff.active && hw4->scan_interval == 2)
        ble_rf_reset_hw();
    if (status_nibble != 1) {
        if (status & 4) {
            rx_mark_invalid(rx);
            hw4->rx_packet_valid = 0;
        } else if (status & 8) {
            rx_mark_invalid(rx);
        } else { hw_dma_free(rx); return; }
    } else if (ble_ext_primary_scan_preprocess_ir(hw, rx)) {
        hw_dma_free(rx); return;
    }

    if (status_nibble == 1) {
    int drop = 0;
    if (config_btctler_le_slave_multilink) {
        if (rx->type == 5) {
            mult_slave_or_master_save_conn_id(HW_ID(hw));
            mult_slave_or_master_fist_con_event_set(1);
        }
        if (hw4->state == 3 && (rx->type & 14) < 2)
            mult_slave_or_master_set_init_param(255, 0, 0);
    }
    if (hw4->state == 2) {
        if (rx->type == 0 || rx->type == 6 || rx->type == 2 || rx->type == 1 || rx->type == 4) drop = 1;
        else if (rx->type == 5 && !hw4->privacy_enable && hw4->tx[0]->type == 1 &&
                 (hw4->peer.addr_type != rx->txadd || memcmp(hw4->peer.addr, rx->data, 6))) drop = 1;
    } else if (hw4->state == 1) {
        if (rx->type == 5 || rx->type == 3) drop = 1;
        else if (hw4->scan_rx_packets > 2) drop = 1;
        else hw4->scan_rx_packets++;
    } else if (hw4->state == 3) {
        if (rx->type == 3 || rx->type == 5) drop = 1;
        else if (rx->type == 1 && !hw4->privacy_enable && (hw4->local.addr_type != rx->rxadd ||
                 memcmp(hw4->local.addr, &rx->data[6], 6))) drop = 1;
        else if (rx->type == 0 || rx->type == 6 || rx->type == 2 || rx->type == 4) {
            if (hw4->peer.addr_type != rx->txadd || memcmp(hw4->peer.addr, rx->data, 6)) drop = 1;
        }
    }
    if (drop) { putchar('~'); hw_dma_free(rx); return; }

    struct ble4_hw *original = hw4;
    hw4 = hw->ble4_hw;
    fp = &hw4->ble_fp;
    if (hw4->state == 2 && (config_btctler_le_roles & 4)) {
        if (rx->type == 3) __set_peer_addr(hw4, rx->txadd, rx->data);
        else if (rx->type == 5) {
            u16 toggle = fp->TXTOG;
            fp->TXAHDR0 = rx->rxadd ? 16 : 0;
            fp->TXDHDR0 = ((toggle << 2) & 4) | 1;
            fp->TXAHDR1 = rx->rxadd ? 16 : 0;
            fp->TXDHDR1 = (((toggle << 2) & 4) ^ 5);
            hw4->tx_seqn = 0;
            hw_dma_free(hw4->tx[0]); hw_dma_free(hw4->tx[1]);
            hw4->tx[0] = 0; hw4->tx[1] = 0;
            ble_tx_drop_staged(hw);
            set_hw_channel_selection_algorithm(hw, rx);
        } else if (log_tag_const_e_LE_BB)
            log_print(4, NULL, "<Error>: [LE_BB]%s / 0x%x\n\r\n", "ble_rx_adv_process", rx->type & 15);
    } else if (hw4->state == 1 && (config_btctler_le_roles & 8) && hw4->backoff.active) {
        if (rx->type == 0 || rx->type == 6) {
            u8 count = --hw4->backoff.backoff_count;
            if (count == 0) {
                if (hw4->backoff.send == 1) {
                    u8 fails = ++hw4->backoff.consecutive_fail;
                    hw4->backoff.consecutive_succ = 0;
                    if (fails == 2 && hw4->backoff.upper_limit != 256) {
                        hw4->backoff.upper_limit <<= 1;
                        backoff_pseudo(hw4);
                        hw4->backoff.consecutive_fail = 0;
                    }
                }
                hw4->backoff.send = 1;
                hw4->backoff.backoff_count = hw4->backoff.backoff_pseudo_cnt;
                count = hw4->backoff.backoff_count;
            }
            __set_scan_active(hw4, count == 1);
        } else if (rx->type == 4) {
            hw4->backoff.send = 0;
            u8 succ = ++hw4->backoff.consecutive_succ;
            hw4->backoff.consecutive_fail = 0;
            if (succ == 2 && hw4->backoff.upper_limit != 1) {
                hw4->backoff.upper_limit >>= 1;
                backoff_pseudo(hw4);
                hw4->backoff.backoff_count = hw4->backoff.backoff_pseudo_cnt;
                hw4->backoff.consecutive_succ = 0;
            }
        }
    } else if (hw4->state == 3 && (config_btctler_le_roles & 16) &&
               (!hw4->privacy_enable || hw4->is_init_enter_conn_pass) && (rx->type & 14) < 2) {
        set_hw_channel_selection_algorithm(hw, rx);
    }

    hw4 = original;
    }
    const struct ble_handler *handler = hw4->handler;
    if (handler && handler->tws_rx_probe_handler) handler->tws_rx_probe_handler(hw4->priv, rx);
    if ((rx->llid << 4) != 16 || rx->len != 0) {
        hw_dma_push(&hw4->rx_head, (struct _dma *)rx);
        if ((rx->llid << 4) != 48) __set_latency_hold_cnt(0, hw4, 3);
    }
    handler = hw4->handler;
    if (handler && handler->rx_probe_handler) handler->rx_probe_handler(hw4->priv, rx);
    if ((rx->llid << 4) == 16 && rx->len == 0) hw_dma_free(rx);
}

static void RF_BLE_CODE RF_BLE_IRQ ble_rx_irq_handler(void)
{
    local_irq_disable();
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        struct ble_hw *hw = &bb_hdl->hw_base.hw[i];
        u32 bit = 1u << (i + 8);
        if (((*(volatile u32 *)1867816 & bit) & *(volatile u32 *)1867824) == 0) continue;
        *(volatile u32 *)1867820 |= bit;
        struct ble4_hw *hw4 = hw->ble4_hw;
        if (rf_get_if_exist_ble5(hw) != 1) {
            struct ble_rx *rx[2] = {hw4->rx[0], hw4->rx[1]};
            int conn = (hw4->state & (u8)~1u) == 6;
            u8 index, ok2 = 0;
            int valid;
            if (conn) {
                u16 toggle;
                u16 this_cntl, other_cntl;
                do {
                    toggle = hw4->ble_fp.RXTOG & 1;
                    this_cntl = toggle ? hw4->ble_fp.RXBUF1CNTL : hw4->ble_fp.RXBUF0CNTL;
                    other_cntl = toggle ? hw4->ble_fp.RXBUF0CNTL : hw4->ble_fp.RXBUF1CNTL;
                } while ((hw4->ble_fp.RXTOG & 1) != toggle);
                index = toggle;
                ok2 = other_cntl & 1;
                valid = __ble_rx_buf_process_conn(hw4, index, this_cntl & 1) == 1;
                hw4->rx_valid |= 1;
            } else {
                index = (hw4->ble_fp.RXTOG & 1) ^ 1;
                struct ble_rx *allocated = hw_alloc_rx(255);
                hw4->rx[index] = allocated;
                u16 ptr = allocated ? (u16)PHY_TO_BLE(allocated->data) : 0;
                if (index) hw4->ble_fp.RXPTR1 = ptr;
                else hw4->ble_fp.RXPTR0 = ptr;
                valid = 1;
            }
            if (valid && rx[index]) __hw_rx_buf_process(hw, rx[index], index);
            if (conn) {
                u8 other = !index;
                int ok = __ble_rx_buf_process_conn(hw4, other, ok2);
                if (rx[other] && ok == 1)
                    __hw_rx_buf_process(hw, rx[other], other);
            }
            __hw_tx_process(hw);
        }
        __ble5_hw_rx_process(hw);
        if (config_btctler_le_slave_multilink) {
            hw4 = hw->ble4_hw;
            if (hw4 && (hw4->state & (u8)~1u) == 6)
                mult_slave_or_master_get_rx_irq_hw(HW_ID(hw));
        }
    }
    local_irq_enable();
}

static void RF_BLE_CODE ble_slot_schedule(void)
{
    local_irq_disable();
    RF_BLE_CSYNC();
    if (sche_lock) { local_irq_enable(); return; }
    sche_lock = 1;
    local_irq_enable();
    int master_multi = config_btctler_le_master_multilink;
    if (!master_multi && bb_hdl->number < 2) goto done;
    u32 min_slots = (u32)-1, slots = (u32)-1;
    u8 period = 0;
    u32 diff = 0;
    for (int i = 0; i < 8; i++) {
        if (!bb_hdl->hw_base.inst[i]) continue;
        struct ble_hw *hw = &bb_hdl->hw_base.hw[i];
        u32 free_slots = __hw5_free_slots(hw);
        if (min_slots > free_slots) min_slots = free_slots;
        struct ble4_hw *hw4 = hw->ble4_hw;
        if (!hw4) continue;
        u8 state = hw4->state;
        period |= 1u << state;
        if (state == 6) diff |= hw4->multilink_diff_us;
        if (state == 6 || state == 2 || state == 7) {
            slots = 0;
            if ((*(volatile u32 *)1867832 & 2) == 0 && !check_event_pending()) {
                u32 low = __get_ble_anchor_con(hw, 0);
                u32 high = __get_ble_anchor_con(hw, 14);
                u32 anchor = ((high << 16) & 0x00ff0000u) | (low & 0xffffu);
                u32 next = anchor + 1;
                slots = next;
                if (!(config_vendor_le_bb && master_multi))
                    slots = next < 4 ? 0 : anchor - 3;
            }
        } else if (state == 1 || state == 3) {
            slots = 0;
            if ((*(volatile u32 *)1867832 & 2) == 0) {
                le_hw_suspend(hw);
                slots = hw->ble4_hw->scan_interval;
            }
        } else continue;
        if (min_slots > slots) min_slots = slots;
    }
    if (diff && master_multi) { putchar('h'); goto done; }
    if (min_slots < 4) goto done;
    switch (period & 10) {
    case 2: ble_slot_schedule_credits = 1; break;
    case 8: ble_slot_schedule_credits = 3; break;
    case 10: ble_slot_schedule_credits = ble_slot_schedule_credits == 1 ? 3 : 1; break;
    default: break;
    }
    for (int i = 0; i < 8; i++) {
        struct ble_hw *hw = &bb_hdl->hw_base.hw[i];
        if (!bb_hdl->hw_base.inst[i]) continue;
        struct ble4_hw *hw4 = hw->ble4_hw;
        if (!hw4 || hw4->state != ble_slot_schedule_credits) continue;
        u8 state = hw4->state;
        if (ble_master_multilink_schedule(hw, min_slots, state, bb_hdl->number) != 0) continue;
        hw4 = hw->ble4_hw;
        if (min_slots < hw4->scan_interval) continue;
        if (hw4->state == 1 && ble_slot_schedule_credits == 1) {
            le_hw_resume(hw, hw4->scan_idle);
            goto done;
        }
        if (hw4->state == 3 && ble_slot_schedule_credits == 3) {
            le_hw_resume(hw, hw4->scan_idle);
            goto done;
        }
    }
done:
    sche_lock = 0;
}

static void RF_BLE_CODE RF_BLE_IRQ ble_event_irq_handler(void)
{
    local_irq_disable();
    int no_multi_slave = config_btctler_le_slave_multilink == 0;
    int no_le_role = (config_btctler_le_roles & 3) == 0;
    for (int i = 0; i < config_btctler_le_hw_nums; i++) {
        struct ble_hw *hw = &bb_hdl->hw_base.hw[i];
        u32 bit = 1u << i;
        if (((*(volatile u32 *)1867816 & bit) & *(volatile u32 *)1867824) != 0) {
            *(volatile u32 *)1867820 |= bit;
            struct ble4_hw *hw4 = hw->ble4_hw;
            if (rf_get_if_exist_ble5(hw) != 1 && hw->ble4_hw) {
                if (hw4->consecutive) hw4->consecutive--;
                switch (hw4->state) {
                case 2:
                    if (config_btctler_le_roles & 4) {
                        u8 tog = hw4->ble_fp.TXTOG & 1;
                        struct ble4_hw *flush_frame = hw->ble4_hw;
                        if (flush_frame->data_flush & 1) {
                            __tx_data_pdu(flush_frame->tx[0], flush_frame->adv_data, flush_frame->adv_len);
                            flush_frame->data_flush &= (u8)~1u;
                        }
                        if (flush_frame->data_flush & 2) {
                            __tx_data_pdu(flush_frame->tx[1], flush_frame->scan_rsp_data, flush_frame->scan_rsp_len);
                            flush_frame->data_flush &= (u8)~2u;
                        }
                        ble_hw_tx(hw, hw4->tx[0], tog);
                        ble_hw_tx(hw, hw4->tx[1], tog ^ 1);
                        __set_le_prio(hw4, rf_bt_bredr_is_sniff() ? 19 : 26);
                    }
                    break;
                case 1:
                case 3:
                    if ((config_btctler_le_roles & 24) == 24 ||
                        (hw4->state == 3 && (config_btctler_le_roles & 16))) {
                        __set_scan_channel_index(hw, hw4->adv_channel);
                        if (hw4->adv_channel != rf_scan_fixed_channel) {
                            u8 channel = hw4->adv_channel + 1;
                            hw4->adv_channel = channel > 39 ? 37 : channel;
                        }
                    }
                    break;
                case 6:
                    if (hw4->handler && hw4->handler->event_probe_handler)
                        hw4->handler->event_probe_handler(hw4->priv, 1, hw4->ble_fp.LASTCHMAP);
                    /* fall through to event-count processing for state 7 */
                    /* fall through */
                case 7:
                    if (!no_le_role) {
                        u32 event_count = __get_conn_event_count(hw);
                        if (hw4->instant_valid && (hw4->instant + (event_count ^ (u32)-1)) > 32765) {
                            putchar('#');
                            hw4->instant_valid = 0;
                            hw4->instant = 0;
                            __set_le_prio(hw4, 28);
                        }
                        if (hw4->handler && hw4->handler->event_probe_handler)
                            hw4->handler->event_probe_handler(hw4->priv, 0, event_count);
                        if (!no_multi_slave)
                            mult_slave_or_master_event_enter(HW_ID(hw));
                        repair_algorithm2_hop(hw);
                    }
                    break;
                default: break;
                }
            }
            __ble5_hw_event_process(hw);
            if (!no_multi_slave) {
                if (mult_slave_or_master_get_con_num() == 0) ble_slot_schedule();
            } else ble_slot_schedule();

            if (!no_le_role) {
                hw4 = hw->ble4_hw;
                if (hw4) {
                    u8 state = hw4->state;
                    u8 prio_state = ++hw4->prio_state_cnt;
                    if ((state & (u8)~1u) == 6) {
                        if (state == 6 && (set_hold_prio_enable & 1)) __set_le_prio(hw4, 28);
                        else {
                            u16 *flags = (u16 *)((u8 *)hw4 + offsetof(struct ble4_hw, init) + 1);
                            u16 f = *flags;
                            u8 saved_prio = (f >> 8) & 31;
                            if (f & 2) { f &= (u16)~252u; *flags = f; }
                            else {
                                f = (u16)(((f + 4) & 252) | (f & (u16)~252u));
                                *flags = f;
                                if (saved_prio == 30) goto event_priority_done;
                            }
                            if (hw4->latency_en && hw4->latency_substate == 0) {
                                if ((*flags & 252) == 0) goto event_priority_done;
                                __set_le_prio(hw4, 30);
                                goto event_priority_done;
                            }
                            if (hw_dma_empty(&hw4->tx_head_h) == 0) { __set_le_prio(hw4, 30); goto event_priority_done; }
                            if ((*flags & 240) != 0) { __set_le_prio(hw4, 30); goto event_priority_done; }
                            if (rf_bt_bredr_is_sniff()) { __set_le_prio(hw4, 19); goto event_priority_done; }
                            if (ble_vendor_is_slow_state()) {
                                __set_le_prio(hw4, 19);
                                f = *flags;
                                u16 shift = f >> 2;
                                if ((shift & 63) != 0) *flags = (u16)((((shift << 2) + 4) & 252) | (f & (u16)~252u));
                                goto event_priority_done;
                            }
                            if (hw_dma_empty(&hw4->tx_head) == 0) {
                                __set_le_prio(hw4, 28);
                                f = *flags;
                                *flags = (u16)(((f + 4) & 252) | (f & (u16)~252u));
                                goto event_priority_done;
                            }
                            if (state == 7 && (set_hold_prio_enable & 2)) { __set_le_prio(hw4, 28); goto event_priority_done; }
                            u8 next_prio = saved_prio - 1;
                            if (next_prio > 27) next_prio = 27;
                            else if (next_prio == 25) next_prio = 19;
                            else if (next_prio < 20) next_prio = ((*flags & 252) == 0) ? 19 : 28;
                            __set_le_prio(hw4, (u8)next_prio);
                        }
                    } else if (state == 3 || state == 1) {
                        int adv = state == 3;
                        if (prio_state % scan_change_prio_cycle) __set_le_prio(hw4, adv ? 26 : 17);
                        else if (adv) __set_le_prio(hw4, 26);
                        else __set_le_prio(hw4, scan_set_prio_max_enable ? 30 : 26);
                    }
                }
            }
event_priority_done:
            if (!no_le_role) {
            hw4 = hw->ble4_hw;
            if (hw4 && (hw4->state & (u8)~1u) == 6 && hw4->latency_en) {
                if (hw4->tx[0] || hw4->tx[1]) __set_latency_hold_cnt(0, hw4, 1);
                else if (!hw4->rx_packet_valid) { __set_latency_hold_cnt(0, hw4, 1); putchar('$'); }
                if (hw4->hold_cnt) { hw4->hold_cnt--; __set_latency_disable(hw); }
                else __set_latency_enable(hw);
            }
            hw4 = hw->ble4_hw;
            if (hw4) {
                if (!hw4->init && hw4->ble_fp.EVTCOUNT != 0) hw4->init = 1;
                else {
                    u32 limit = config_vendor_le_bb == 0 ? 100 : 5;
                    if (hw4->state == 7) {
                        u16 *flags = (u16 *)((u8 *)hw4 + offsetof(struct ble4_hw, init) + 1);
                        if (*flags & 1) {
                            if (hw4->widen_auto_inc >= limit) {
                                hw4->widen_auto_inc = 0;
                                hw4->ble_fp.WINCNTL0 = (u16)hw4->window;
                                hw4->ble_fp.WINCNTL1 = (u16)(hw4->window >> 16);
                            }
                        } else if (hw4->widen + limit + hw4->widen_auto_inc < (config_vendor_le_bb == 0 ? 2000u : 200u)) {
                            if (__hw_set_widen_drift(hw, limit, hw4->window + ((hw4->widen_auto_inc + limit) << 1)))
                                hw4->widen_auto_inc += limit;
                            u16 w1 = hw4->ble_fp.WINCNTL1;
                            u16 w0 = hw4->ble_fp.WINCNTL0;
                            if (log_tag_const_d_LE_BB)
                                log_print(3, NULL, "[Debug]: [LE_BB]w : %d(us) / %d(us)\r\n", hw4->widen_auto_inc + hw4->widen, (w1 << 16) | w0);
                        }
                    }
                    u16 *flags = (u16 *)((u8 *)hw4 + offsetof(struct ble4_hw, init) + 1);
                    *flags &= (u16)~3u;
                }
            }
            }
            /* The event is over: a completed current slot may be refilled. */
            __hw_tx_process(hw);
            ble_power_off(hw);
        }
        if (((*(volatile u32 *)1867856 & bit) & *(volatile u32 *)1867852) != 0) {
            *(volatile u32 *)1867852 |= 1u << (i + 8);
            ble5_master_primary_event_end_handle(hw);
            __ble5_hw_anchor_cnt_timeout(hw);
        }
    }
    if ((s8)*(volatile u32 *)1867832 < 0) {
        *(volatile u32 *)1867832 |= 64;
        for (int i = 0; i < 8; i++)
            if (bb_hdl->hw_base.inst[i]) ble_power_on(&bb_hdl->hw_base.hw[i]);
    }
    local_irq_enable();
}

static int RF_BLE_CODE tws_le_hw_scanning(struct ble_hw *hw, const struct ble_tws_scan *scan)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    u8 info = log_tag_const_i_LE_BB;
    if (info) log_print(2, NULL, "[Info]: [LE_BB]-> TWS SCAN state : 0x%x\r\n", scan->aa);
    hw->tws = 1;
    __set_le_prio(hw4, 17);
    __disable_adv_random(hw);
    u32 aa = scan->aa;
    hw4->ble_fp.BDADDR0 = (u16)(aa >> 16);
    hw4->ble_fp.BDADDR1 = (u16)aa;
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]BDADDR0 : 0x%x\r\n", hw4->ble_fp.BDADDR0);
        log_print(2, NULL, "[Info]: [LE_BB]BDADDR1 : 0x%x\r\n", hw4->ble_fp.BDADDR1);
    }
    __set_scan_window(hw, 11, 11);
    __set_hw_state(hw, 1, 0, 0);
    __set_scan_device_filter_param(hw4, 0);
    __set_scan_active(hw4, 1);
    hw4->adv_channel = 37;
    __set_scan_channel_index(hw, 37);
    hw4->rx[0] = hw_alloc_rx(64);
    hw4->rx[1] = hw_alloc_rx(64);
    if (!hw4->rx[0] || !hw4->rx[1]) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]RX DMA NULL\r\n");
        return -2;
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----rx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->rx[0] : %x / hw->rx[1] : %x\r\n", hw4->rx[0], hw4->rx[1]);
    }
    hw4->tx[0] = hw_alloc_tx(251);
    hw4->tx[1] = hw_alloc_tx(251);
    if (!hw4->tx[0] || !hw4->tx[1]) {
        if (log_tag_const_e_LE_BB) log_print(4, NULL, "<Error>: [LE_BB]TX DMA NULL\r\n");
        return -2;
    }
    __tws_scan_req_pdu(hw4->tx[0]);
    __tws_scan_req_pdu(hw4->tx[1]);
    if (info) {
        log_print(2, NULL, "[Info]: [LE_BB]----tx init-----\r\n");
        log_print(2, NULL, "[Info]: [LE_BB]hw->tx[0] : %x / hw->tx[1] : %x\r\n", hw4->tx[0], hw4->tx[1]);
    }
    ble_hw_tx(hw, hw4->tx[0], 0);
    ble_hw_tx(hw, hw4->tx[1], 1);
    hw->phy.coded_s = 1;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)~1792u) | 512;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)~14336u) | 4096;
    hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & 16383u) | 16384;
    hw4->ble_fp.TXDHDR0 |= 16;
    hw4->ble_fp.TXDHDR1 |= 16;
    hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u16)~127u) | 20;
    hw4->ble_fp.EXTIFSDRF1 = (hw4->ble_fp.EXTIFSDRF1 & (u16)~30720u) | 16384;
    hw4->ble_fp.FORMAT &= (u16)~8u;
    hw4->ble_fp.FORMAT |= 1;
    hw4->ble_fp.LOCALADRL = 0;
    hw4->ble_fp.LOCALADRM = 0;
    hw4->ble_fp.LOCALADRU = 0;
    hw4->backoff.active = 0;
    if (bb_hdl->number == 1) { hw4->sub_state = 0; ble_hw_enable(hw, 1); }
    else { hw4->sub_state = 1; ble_slot_schedule(); }
    return 0;
}

void RF_BLE_CODE le_hw_debug(void)
{
    int i = 0;
    for (;;) {
        struct ble_tx *tx = hw_alloc_tx(251);
        if (log_tag_const_e_LE_BB)
            log_print(4, NULL, "<Error>: [LE_BB]Tx : %d - 0x%x\r\n", i, tx);
        if (config_asser) {
            if (!tx) {
                printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4302);
                printf("ASSERT-FAILD: tx != NULL ");
                cpu_assert_debug();
            }
        } else if (!tx) cpu_reset();
        i++;
    }
}

static void RF_BLE_CODE le_hw_init(void)
{
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]le_hw_init\r\n");
    *(volatile u32 *)1899648 |= 1;
    *(volatile u32 *)1867776 = 1;
    *(volatile u32 *)1867828 = 2561;
    u8 debug = log_tag_const_d_LE_BB;
    if (debug) log_print(3, NULL, "[Debug]: [LE_BB]--func=%s\r\n", "alloc_baseband_hw_ram");
    u32 hw_count = config_btctler_le_hw_nums;
    u32 hw_off = hw_count * 28 + 36;
    u32 hw_entity_off = hw_off + hw_count * 628;
    u32 rx_size = config_btctler_le_rx_nums * 294 + 31;
    u32 tx_off = rx_size + hw_entity_off;
    s32 acl_size = config_btctler_le_acl_packet_length;
    s32 p27 = ((acl_size / 27) * 70) + 70;
    s32 p251 = ((acl_size / 251) * 294) + 294;
    s32 unit = (u32)p27 > (u32)p251 ? 27 : 251;
    u32 overhead = (u32)p27 > (u32)p251 ? 70 : 294;
    u32 tx_size = config_btctler_le_acl_total_nums * ((acl_size / unit) + 1) * overhead;
    u32 total = tx_off + 31 + tx_size;
    bb_hdl = malloc(total);
    if (config_asser) {
        if (!bb_hdl) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/ble/RF_ble.c", 4750);
            printf("ASSERT-FAILD: __this ");
            cpu_assert_debug();
        }
    } else if (!bb_hdl) cpu_reset();
    memset(bb_hdl, 0, total);
    bb_hdl->hw_base.hw = (struct ble_hw *)((u8 *)bb_hdl + 36);
    bb_hdl->hw_base.hw_entity = (struct _hw_entity *)((u8 *)bb_hdl + hw_off);
    bb_hdl->hw_base.rx_dma = (u8 *)bb_hdl + hw_entity_off;
    bb_hdl->hw_base.tx_dma = (u8 *)bb_hdl + tx_off;
    if (debug) {
        log_print(3, NULL, "[Debug]: [LE_BB]total size:0x%x, __this addr:0x%x\r\n", total, bb_hdl);
        log_print(3, NULL, "[Debug]: [LE_BB]hw addr:0x%x\r\n", bb_hdl->hw_base.hw);
        log_print(3, NULL, "[Debug]: [LE_BB]hw_entity addr:0x%x\r\n", bb_hdl->hw_base.hw_entity);
        log_print(3, NULL, "[Debug]: [LE_BB]rx_dma addr:0x%x\r\n", bb_hdl->hw_base.rx_dma);
        log_print(3, NULL, "[Debug]: [LE_BB]tx_dma addr:0x%x\r\n", bb_hdl->hw_base.tx_dma);
    }
    *(volatile u32 *)1899652 = (u32)bb_hdl;
    *(volatile u32 *)1899708 = (u32)bb_hdl + total;
    hw_dma_init(bb_hdl->hw_base.rx_dma, rx_size, bb_hdl->hw_base.tx_dma, tx_size + 31);
    request_irq(39, 2, ble_event_irq_handler, 0);
    request_irq(38, 2, ble_rx_irq_handler, 0);
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]\r\n--func=%s\r\n\r\n", "__set_hw_frame_init_common");
    *(volatile u32 *)1867776 |= 8192;
    *(volatile u32 *)1867776 |= 16384;
    *(volatile u32 *)1867776 &= (u32)~16u;
    *(volatile u32 *)1867784 = (*(volatile u32 *)1867784 & (u32)~1023u) | 70;
    *(volatile u32 *)1867784 |= 61440;
    *(volatile u32 *)1867788 = (*(volatile u32 *)1867788 & (u32)~1023u) | 70;
    *(volatile u32 *)1867792 = (*(volatile u32 *)1867792 & (u32)~255u) | 62;
    *(volatile u32 *)1867792 = (*(volatile u32 *)1867792 & (u32)~65280u) | 15872;
    *(volatile u32 *)1867796 = (*(volatile u32 *)1867796 & (u32)~255u) | 30;
    *(volatile u32 *)1867800 = (*(volatile u32 *)1867800 & (u32)~255u) | 20;
    *(volatile u32 *)1867800 = (*(volatile u32 *)1867800 & (u32)~65280u) | 5120;
}

static void *RF_BLE_CODE le_hw_send_single_carrier(u8 *dut_param)
{
    u8 channel[5];
    if (!dut_param) return 0;
    if (*dut_param > 39) *dut_param = 39;
    *(volatile u32 *)1867776 |= 64;
    struct ble_hw *hw = ble_hw_dut;
    if (hw) { le_hw_close(hw); ble_hw_dut = 0; }
    if (dut_tx) {
        hw_dma_free(dut_tx);
        if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]free dut tx\n\r\n");
    }
    hw = le_hw_open();
    struct ble4_hw *hw4 = hw->ble4_hw;
    __set_local_addr(hw4, 0, rf_ble_le_mac_addr);
    if (config_btctler_modules & 1) {
        if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]edr close\n\r\n");
        dut_bredr_close_recall();
    }
    u8 *typep = dut_param + 1;
    if (*typep == 0xf0) {
        if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]set sig carrier analog\n\r\n");
        bt_set_single_carrier();
    } else bt_reload_trim();
    bt_pll_reset(1);
    memset(channel, 0xff, sizeof(channel));
    u32 count = 0;
    for (u32 i = 0; i < 40; i++) {
        u8 rf_channel = (u8)i;
        u8 table = (u8)((i << 1) + (*typep == 0xf0 ? 2 : 0));
        hw4->ble_fp.FRQ_IDX0[i] = rf_channel;
        hw4->ble_fp.FRQ_TBL0[i] = table;
        if ((channel[i >> 3] & (1u << (i & 7))) != 0) {
            hw4->ble_fp.FRQ_IDX1[count] = rf_channel;
            hw4->ble_fp.FRQ_TBL1[count] = table;
            count++;
        }
    }
    hw4->ble_fp.CHMAP0 = -1;
    hw4->ble_fp.CHMAP1 = -1;
    hw4->ble_fp.CHMAP2 = (u16)((count << 5) | 31);
    u8 len = dut_param[2];
    dut_tx = hw_alloc_tx(len);
    len = dut_param[2];
    dut_tx->len = len;
    switch (*typep) {
    case 0: break;
    case 1: memset(dut_tx->data, 15, len); break;
    case 2: memset(dut_tx->data, 85, len); break;
    case 3: break;
    case 4: memset(dut_tx->data, 0xff, len); break;
    case 5: memset(dut_tx->data, 0, len); break;
    case 6: memset(dut_tx->data, 0xf0, len); break;
    default: break;
    }
    u16 format = *typep == 0 ? 16384 : (*typep == 3 ? 49152 : 0);
    hw4->ble_fp.FORMAT &= (u16)~8192u;
    __set_fixed_channel_index(hw, *dut_param);
    if (dut_param[3]) {
        if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]-continuous_tx\n\r\n");
        format |= 8192;
    }
    hw4->ble_fp.FORMAT = format | 3072;
    u32 bits = (dut_tx->len << 3) + 80;
    if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]bits = %d\n\r\n", bits);
    int interval = bits < 377 ? 625 : bits < 1001 ? 1250 : bits < 1625 ? 1875 : 2500;
    __set_interval(hw, interval, 0);
    ble_hw_tx(hw, dut_tx, 0);
    ble_hw_tx(hw, dut_tx, 1);
    __set_hw_state(hw, 6, 0, 0);
    ble_hw_enable(hw, 1);
    ble_hw_dut = hw;
    return hw;
}

static void RF_BLE_CODE le_hw_ioctrl(struct ble_hw *hw, int ctrl, ...)
{
    va_list ap;
    va_start(ap, ctrl);
    struct ble4_hw *hw4 = hw->ble4_hw;
    switch (ctrl) {
    case 0: /* set IV */
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 1)) break;
        memcpy(hw4->encrypt.iv, va_arg(ap, const void *), 8);
        break;
    case 1: /* set SKD */
        if (!(config_btctler_le_roles & 3) || !(config_btctler_le_features & 1)) break;
        memcpy(hw4->encrypt.skd, va_arg(ap, const void *), 16);
        break;
    case 2: {
        int type = va_arg(ap, int);
        __set_local_addr(hw4, (u8)type, va_arg(ap, const u8 *));
        break;
    }
    case 3: {
        int type = va_arg(ap, int);
        __set_peer_addr(hw4, (u8)type, va_arg(ap, const u8 *));
        break;
    }
    case 4:
        if (!(config_btctler_le_roles & 3)) break;
        { int instant = va_arg(ap, int);
          int type = va_arg(ap, int);
          __set_event_instant(hw, instant, type); }
        __set_le_prio(hw4, 30);
        break;
    case 5:
        { int window = va_arg(ap, int);
          int new_window = va_arg(ap, int);
          __set_winsize(hw4, window, new_window); }
        break;
    case 8:
        if (!(config_btctler_le_roles & 3)) break;
        __set_channel_map(hw4, va_arg(ap, const u8 *));
        break;
    case 6: { /* connection parameters */
        if (!(config_btctler_le_roles & 3)) break;
        struct ble_conn_param *conn = va_arg(ap, struct ble_conn_param *);
        g_ble_fp = &hw4->ble_fp;
        __set_le_prio(hw4, 28);
        hw4->ble_fp.ANCHOR = (u16)0x8000;
        hw4->ble_fp.TXTOG &= (u16)~2u;
        hw4->ble_fp.RXTOG = 0;
        hw4->ble_fp.BDADDR0 = btctler_little_endian_read_16(conn->aa, 0);
        hw4->ble_fp.BDADDR1 = btctler_little_endian_read_16(conn->aa, 2);
        hw4->ble_fp.CRCWORD0 = btctler_little_endian_read_16(conn->crcinit, 0);
        hw4->ble_fp.CRCWORD1 = conn->crcinit[2];
        hw4->ble_fp.OPTCNTL |= 4;
        hw4->ble_fp.OPTCNTL &= (u16)~512u;
        hw4->ble_fp.OPTCNTL |= 3072;
        hw4->ble_fp.OPTCNTL |= 4096;
        hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)~48u) | 16;
        hw4->ble_fp.TXBUF0CNTL |= 1;
        hw4->ble_fp.TXBUF1CNTL |= 1;
        __set_conn_dead_times(hw);
        __set_event_instant(hw, 0, 0);
        hw4->instant_valid = 0;
        if (config_vendor_le_bb) {
            hw4->ble_fp.FORMAT |= 2;
            hw4->ble_fp.INTFRAME |= 8;
        }
        if (hw4->state == 3) {
            __set_ble_anchor_con(hw, 4, 0);
            __set_anchor_cnt(hw, ((u32)conn->winoffset << 1) + 4);
            __master_reset_transmitWindowDelay(hw, conn->winoffset);
            master_setup_first_data_location(hw, conn);
            __set_hw_state(hw, 6, 0, conn->latency);
            if (config_vendor_le_bb & 8) hw4->ble_fp.INTFRAME |= 1;
            if (config_btctler_le_slave_multilink) {
                mult_slave_or_master_save_conn_id(HW_ID(hw));
                ble_power_put(hw); ble_power_get(hw);
            }
        } else {
            int offset = conn->winoffset ? ((int)conn->winoffset << 1) - 1 : 0;
            __set_winoffset(hw, offset);
            int latency = bb_hdl->number > 1 ? 0 : conn->latency;
            __set_hw_state(hw, 7, latency != 0, latency);
            __set_widen(hw4, conn->widening);
            __slave_reset_widen(hw, &conn->widening);
            hw4->hold_cnt = 6;
            if (config_btctler_le_slave_multilink) { ble_power_put(hw); ble_power_get(hw); }
        }
        __disable_adv_random(hw);
        struct ble4_hw *event_frame = hw->ble4_hw;
        __set_ble_anchor_con(hw, 3, 0);
        event_frame->ble_fp.EVTCOUNT = 0;
        __set_winsize(hw4, 50, (conn->winsize * 1250) + 1250);
        __reset_connection_winsize(hw, conn->winsize);
        if (config_vendor_le_bb & 16) __set_interval(hw, conn->interval * 625, 1);
        else __set_interval(hw, conn->interval * 1250, 1);
        hw4->ll_timeout = conn->timeout * 10;
        __set_channel_map(hw4, conn->channel);
        u8 hop = conn->hop & 31;
        __set_ble_anchor_con(hw, 6, hop | ((u32)hop << 8) | 0x8000u);
        hw_dma_free(hw4->tx[0]);
        hw_dma_free(hw4->tx[1]);
        hw4->tx[0] = 0;
        hw4->tx[1] = 0;
        ble_tx_drop_staged(hw);
        hw4->tx_seqn = 0;
        u16 toggle = hw4->ble_fp.TXTOG;
        hw4->ble_fp.TXAHDR0 = 0;
        hw4->ble_fp.TXDHDR0 = ((toggle << 2) & 4) | 1;
        hw4->ble_fp.TXAHDR1 = 0;
        hw4->ble_fp.TXDHDR1 = ((toggle << 2) & 4) ^ 5;
        break;
    }
    case 7: { /* connection update */
        if (!(config_btctler_le_roles & 3)) break;
        struct ble_conn_param *conn = va_arg(ap, struct ble_conn_param *);
        u32 interval_unit = 1250;
        u32 first_winsize = conn->winsize * 1250u + 625u;
        if (config_vendor_le_bb & 16) {
            interval_unit = 625;
            if ((conn->interval & (u16)~1u) == 2) first_winsize = conn->winsize * 625u;
        }
        int offset = conn->winoffset;
        if (hw4->state == 6) {
            int scaled_offset = offset << 1;
            u32 small = 50;
            if (config_btctler_le_features & 2048) {
                u8 phy_error;
                small = conn_update_small_winsize(hw->phy.coded_s, &phy_error);
                if (phy_error && log_tag_const_e_LE_BB)
                    log_print(4, NULL, "<Error>: [LE_BB]coded phy err\r\n");
            }
            __set_winsize(hw4, small, first_winsize);
            __set_hw_state(hw, 6, 0, conn->latency);
            if (rf_master_setup_connection_update(hw)) {
                __set_interval(hw, conn->interval * 1250, 1);
                __set_le_prio(hw4, 30);
                break;
            }
            offset = scaled_offset;
        } else {
            int scaled_offset = offset ? (offset << 1) - 1 : 0;
            if ((config_vendor_le_bb & 16) && (conn->interval & (u16)~1u) == 2)
                scaled_offset = offset ? (offset << 1) : 0;
            __set_winsize(hw4, 50, conn->widening_offset + first_winsize);
            int latency = bb_hdl->number > 1 ? 0 : conn->latency;
            __set_hw_state(hw, 7, latency != 0, latency);
            hw->phy.widen = conn->widening;
            u32 widen = conn->widening;
            if ((config_btctler_le_features & 2048) && hw->phy.tx_phy == 2) widen += 476;
            __set_widen(hw4, widen);
            offset = scaled_offset;
        }
        __set_winoffset(hw, offset);
        __set_interval(hw, conn->interval * interval_unit, 1);
        __set_le_prio(hw4, 30);
        break;
    }
    case 9: {
        int type = va_arg(ap, int);
        const u8 *addr = va_arg(ap, const u8 *);
        hw4->ble_fp.FILTERCNTL = (hw4->ble_fp.FILTERCNTL & 126u) | (((u8)type != 0) << 8) | 1;
        hw4->ble_fp.FILTERCNTL |= 16;
        hw4->ble_fp.WHITELIST0L = (u16)(addr[1] << 8 | addr[0]);
        hw4->ble_fp.WHITELIST0M = (u16)(addr[3] << 8 | addr[2]);
        hw4->ble_fp.WHITELIST0U = (u16)(addr[5] << 8 | addr[4]);
        __set_remote_addr_match_enable(&hw4->ble_fp);
        break;
    }
    case 10: {
        u8 enable = (u8)va_arg(ap, int);
        hw4->encrypt.tx_enable = enable;
        if (!enable) { hw4->encrypt.tx_counter_l = 0; hw4->encrypt.tx_counter_h = 0; }
        break;
    }
    case 11: {
        u8 enable = (u8)va_arg(ap, int);
        hw4->encrypt.rx_enable = enable;
        if (!enable) { hw4->encrypt.rx_counter_l = 0; hw4->encrypt.rx_counter_h = 0; }
        break;
    }
    case 13: {
        u8 type = (u8)va_arg(ap, int);
        const u8 *addr = va_arg(ap, const u8 *);
        hw4->ble_fp.FORMAT &= (u16)~8u;
        hw4->local.addr_type = type != 0;
        memcpy(hw4->local.addr, addr, 6);
        memcpy((u8 *)bb_hdl + hw4->ble_fp.TXPTR0, addr, 6);
        memcpy((u8 *)bb_hdl + hw4->ble_fp.TXPTR1, addr, 6);
        break;
    }
    case 12:
        if (!(config_btctler_le_features & 64)) break;
        hw4->privacy_enable = (u8)va_arg(ap, int);
        hw4->ble_fp.INTFRAME |= 8;
        __set_scan_req_disable(&hw4->ble_fp);
        break;
    case 14: {
        if (!(config_btctler_le_features & 64)) break;
        struct ble_rx *rx = va_arg(ap, struct ble_rx *);
        int failed = va_arg(ap, int);
        if ((u8)failed != 0 || hw4->privacy_enable == 0) break;
        u8 type = rx->type;
        if (hw4->state == 2 && type == 3) {
            hw4->ble_fp.WHITELIST0L = (u16)(rx->data[1] << 8 | rx->data[0]);
            hw4->ble_fp.WHITELIST0M = (u16)(rx->data[3] << 8 | rx->data[2]);
            hw4->ble_fp.WHITELIST0U = (u16)(rx->data[5] << 8 | rx->data[4]);
            hw4->ble_fp.FILTERCNTL |= 256;
            hw4->ble_fp.FILTERCNTL |= 8;
            __set_remote_addr_match_enable(&hw4->ble_fp);
            __set_hw_tx_enable(&hw4->ble_fp);
        } else if (hw4->state == 1 && (type == 0 || type == 6) && hw4->backoff.active) {
            hw4->ble_fp.WHITELIST0L = (u16)(rx->data[1] << 8 | rx->data[0]);
            hw4->ble_fp.WHITELIST0M = (u16)(rx->data[3] << 8 | rx->data[2]);
            hw4->ble_fp.WHITELIST0U = (u16)(rx->data[5] << 8 | rx->data[4]);
            hw4->ble_fp.FILTERCNTL |= 256;
            hw4->ble_fp.FILTERCNTL |= 8;
            __set_remote_addr_match_enable(&hw4->ble_fp);
            __set_scan_req_enable(&hw4->ble_fp);
        } else if (hw4->state == 3 && (type & 14) < 2) {
            hw4->is_init_enter_conn_pass = 1;
            hw4->ble_fp.WHITELIST0L = (u16)(rx->data[1] << 8 | rx->data[0]);
            hw4->ble_fp.WHITELIST0M = (u16)(rx->data[3] << 8 | rx->data[2]);
            hw4->ble_fp.WHITELIST0U = (u16)(rx->data[5] << 8 | rx->data[4]);
            hw4->ble_fp.FILTERCNTL |= 256;
            hw4->ble_fp.FILTERCNTL |= 16;
            __set_remote_addr_match_enable(&hw4->ble_fp);
            __set_hw_tx_enable(&hw4->ble_fp);
        } else if (hw4->state == 7 && type == 5) {
            __set_hw_tx_enable(&hw4->ble_fp);
        }
        break;
    }
    case 15: hw4->tx_octets = (u16)va_arg(ap, int); break;
    case 17: hw4->tx_acl_block = (u8)va_arg(ap, int); break;
    case 18: hw4->tx_acl_c_block = (u8)va_arg(ap, int); break;
    case 19: *va_arg(ap, int *) = hw_dma_empty(&hw4->rx_head) || (rx_held & (1u << HW_ID(hw))); break;
    case 20: {
        u16 index = (u16)va_arg(ap, int);
        if (*(volatile u32 *)1867776 != 0) {
            if (log_tag_const_i_LE_BB) log_print(2, NULL, "[Info]: [LE_BB]--RF Debug_index=%d\n\r\n", index);
            *(volatile u32 *)1867828 = ((u32)index << 8) | 1;
        }
        break;
    }
    case 21: {
        const u8 *input = va_arg(ap, const u8 *);
        u32 len = *input;
        memcpy(hw4->adv_data, input + 1, len);
        hw4->adv_len = (u8)len;
        hw4->data_flush |= 1;
        if (log_tag_const_i_LE_BB) {
            log_print(2, NULL, "[Info]: [LE_BB]Adv data :\r\n");
            printf_buf(hw4->adv_data, len);
        }
        break;
    }
    case 22: {
        const u8 *input = va_arg(ap, const u8 *);
        u32 len = *input;
        memcpy(hw4->scan_rsp_data, input + 1, len);
        hw4->scan_rsp_len = (u8)len;
        hw4->data_flush |= 2;
        if (log_tag_const_i_LE_BB) {
            log_print(2, NULL, "[Info]: [LE_BB]Scan rsp data : %d\r\n", len);
            printf_buf(hw4->scan_rsp_data, hw4->scan_rsp_len);
        }
        break;
    }
    case 23: hw4->hold_cnt = (u16)va_arg(ap, int); ble_latency_manualhold(hw); break;
    default: break;
    }
    va_end(ap);
}

static const struct ble_operation ble_ops __attribute__((section(".ble_rf_const"))) = {
    (void (*)(void))le_hw_init,
    (void (*)(void))le_hw_destroy,
    (void *(*)(void))le_hw_open,
    (void (*)(void *))le_hw_close,
    (void (*)(void *))le_hw_standby,
    (int (*)(void *, struct ble_adv *))le_hw_advertising,
    (int (*)(void *, struct ble_tws_adv *))tws_le_hw_advertising,
    (int (*)(void *, struct ble_scan *))le_hw_scanning,
    (int (*)(void *, struct ble_tws_scan *))tws_le_hw_scanning,
    (int (*)(void *, struct ble_conn *))le_hw_initiating,
    (void (*)(void *, void *, const struct ble_handler *))le_hw_handler_register,
    (void (*)(void *, u8, const u8 *, int, int))le_hw_send_packet,
    (void (*)(void *, int, ...))le_hw_ioctrl,
    (void (*)(void *, void (*)(void *, struct ble_rx *)))le_hw_upload_data,
    (int (*)(void *))le_hw_is_init_enter_conn,
    (void (*)(void *, void (*)(void *), void (*)(void *), u8, const u8 *, int))le_hw_send_packet_high,
    (void *(*)(void *))le_hw_send_single_carrier,
};

const struct ble_operation *__ble_ops __attribute__((section(".ble_rf_data"))) = &ble_ops;

#ifdef UNITTEST
#include "test.h"
#include <sys/mman.h>

u8 log_tag_const_i_LE_BB, log_tag_const_d_LE_BB, log_tag_const_e_LE_BB;
u64 config_btctler_le_features;
int config_asser, config_btctler_le_roles;
int config_btctler_le_hw_nums = 8;
int config_btctler_le_slave_multilink = 1, config_btctler_le_master_multilink;
int ble_disable_wait_enable;
u32 config_vendor_le_bb;
static unsigned queues, frees, empty_calls, tws_probes, rx_probes, schedules;
static u32 scheduled_slots;
static int mutate_schedule_state;
static const struct ble_handler replacement_handler;
void local_irq_disable(void) { }
void local_irq_enable(void) { }
u32 __hw5_free_slots(struct ble_hw *hw) { (void)hw; return (u32)-1; }
int ble_master_multilink_schedule(struct ble_hw *hw, u32 slots, u8 type, u8 number)
{
    (void)type; (void)number; ++schedules; scheduled_slots = slots;
    if (mutate_schedule_state) hw->ble4_hw->state = 0;
    return 0;
}
void hw_dma_free(void *p) { (void)p; ++frees; }
void hw_dma_push(struct _dma_head *head, struct _dma *p)
{ (void)head; (void)p; ++queues; }
/* A data queue for the TX slot tests; other queues are empty there. */
static struct _dma_head *test_tx_head;
static struct ble_tx *test_queue[4];
static unsigned test_queued, test_popped;
int hw_dma_empty(struct _dma_head *head)
{
    ++empty_calls;
    if (!test_tx_head) return 0;
    return head != test_tx_head || test_popped == test_queued;
}
void *hw_dma_pop(struct _dma_head *head)
{
    if (head != test_tx_head || test_popped == test_queued) return 0;
    return test_queue[test_popped++];
}
s8 ble_analog_get_rssi(struct ble_param *fp) { (void)fp; return -42; }
int config_btctler_hci_standard = 1;
static int host_room = 1, host_asks, uploads;
int btctrlermod_host_acl_room(void) { ++host_asks; return host_room; }
void *btctrlermod_dma_first(struct _dma_head *head)
{ return head == test_tx_head && test_popped < test_queued ? test_queue[test_popped] : NULL; }
static void test_upload(void *priv, struct ble_rx *rx) { (void)priv; (void)rx; ++uploads; }
static void *test_rx_buffer;
static unsigned ll_resumes;
void ll_thread_resume(void) { ++ll_resumes; }
void *hw_dma_alloc(u8 type, u32 size) { (void)type; (void)size; return test_rx_buffer; }
void record_rx_chsel_algorithm(struct ble_rx *rx, u8 algorithm)
{
    (void)algorithm;
    /* Preserve a spare header bit changed by this callback. */
    u8 *header = (u8 *)rx + offsetof(struct ble_rx, len) - 2;
    u16 bits; memcpy(&bits, header, 2); bits |= 0x8000;
    memcpy(header, &bits, 2);
}
u8 ble_ext_primary_scan_preprocess_ir(struct ble_hw *, struct ble_rx *)
    __asm__("ble_ext_primary_scan_preprocess");
u8 ble_ext_primary_scan_preprocess_ir(struct ble_hw *hw, struct ble_rx *rx)
{ (void)hw; (void)rx; return 0; }
void __set_extended_tx_data(struct ble_hw *hw, u8 chsel, int index)
{ (void)hw; (void)chsel; (void)index; }
void __set_extended_empty_tx_data(struct ble_hw *hw) { (void)hw; }
static void test_rx_probe(void *priv, struct ble_rx *rx)
{ (void)priv; (void)rx; ++rx_probes; }
static void test_tws_probe(void *priv, struct ble_rx *rx)
{
    (void)rx; ++tws_probes;
    ((struct ble4_hw *)priv)->handler = &replacement_handler;
}
static const struct ble_handler replacement_handler = {
    .rx_probe_handler = test_rx_probe,
};
static const struct ble_handler initial_handler = {
    .tws_rx_probe_handler = test_tws_probe,
};
static struct ble_hw *test_hw;
static int test_toggle_on_ack = -1, test_nested;
static unsigned acks;
static void test_ack(void *priv)
{
    struct ble4_hw *hw = priv;
    ++acks;
    if (test_toggle_on_ack >= 0) { hw->ble_fp.TXTOG = (u16)test_toggle_on_ack; test_toggle_on_ack = -1; }
    if (test_nested) { test_nested = 0; __hw_tx_process(test_hw); }
}
/* Packet buffers must lie just above the baseband handle (PHY_TO_BLE). */
static struct {
    struct ble_baseband_layer_handle baseband;
    u8 storage[5][sizeof(struct ble_tx) + 40];
} test_bb;
static void test_queue_reset(struct ble4_hw *hw, unsigned n)
{
    memset(test_bb.storage, 0, 4 * sizeof(test_bb.storage[0]));
    for (unsigned i = 0; i < n; i++) {
        test_queue[i] = (struct ble_tx *)test_bb.storage[i];
        test_queue[i]->llid = 2; test_queue[i]->len = (u8)(10 + i);
        test_queue[i]->ack_callback = test_ack;
    }
    test_tx_head = &hw->tx_head; test_queued = n; test_popped = 0;
}
#define TX_LEN(hw, i) ((i) ? (hw)->ble_fp.TXDHDR1 >> 8 : (hw)->ble_fp.TXDHDR0 >> 8)
/* Radio idle with this many slots to the next anchor, or busy. */
static void test_radio(int idle, u32 slots)
{
    *(volatile u32 *)1867832 = idle ? 0 : 2;
    *(volatile u32 *)1867812 = slots;
}
static void test_tx_slots(struct ble_hw *ble_hw, struct ble4_hw *hw)
{
    struct ble_baseband_layer_handle *saved_bb = bb_hdl;
    test_bb.baseband = *bb_hdl; bb_hdl = &test_bb.baseband;
    test_hw = ble_hw;
    hw->state = 7; config_btctler_le_roles = 3; hw->priv = hw; hw->handler = 0;
    /* Both slots complete during an event: slot 0 sends empty PDUs, no MD,
     * and nothing is written into either slot. */
    test_queue_reset(hw, 3);
    test_radio(0, 5);
    hw->ble_fp.TXTOG = 0; hw->ble_fp.TXBUF0CNTL = 1; hw->ble_fp.TXBUF1CNTL = 1;
    hw->ble_fp.TXDHDR0 = 0x0005; hw->ble_fp.TXDHDR1 = 0x0001; hw->ble_fp.INTFRAME = 64;
    __hw_tx_process(ble_hw);
    CHECK(test_popped == 0 && hw->tx[0] == 0 && hw->tx[1] == 0);
    CHECK(hw->ble_fp.TXBUF0CNTL == 1 && hw->ble_fp.TXBUF1CNTL == 1 && !(hw->ble_fp.INTFRAME & 64));
    CHECK(hw->tx_lock.counter == 0);
    /* Too close to the anchor: still nothing. */
    test_radio(1, 0);
    __hw_tx_process(ble_hw);
    CHECK(test_popped == 0);
    /* Between events: the current slot first, then the other, in order. */
    test_radio(1, 5);
    __hw_tx_process(ble_hw);
    CHECK(hw->tx[0] == test_queue[0] && hw->tx[1] == test_queue[1] && test_popped == 2);
    CHECK(hw->ble_fp.TXBUF0CNTL == 0 && hw->ble_fp.TXBUF1CNTL == 0);
    CHECK(TX_LEN(hw, 0) == 10 && TX_LEN(hw, 1) == 11);
    CHECK((hw->ble_fp.TXDHDR0 & 4) == 4 && (hw->ble_fp.TXDHDR1 & 4) == 0);
    /* Slot 0 acknowledged during an event: retired, its descriptor reset to
     * an empty PDU (SN kept), refilled because slot 1 is now current. */
    unsigned freed = frees;
    test_radio(0, 5);
    hw->ble_fp.TXTOG = 1; hw->ble_fp.TXBUF0CNTL = 1;
    __hw_tx_process(ble_hw);
    CHECK(frees == freed + 1 && hw->tx[0] == test_queue[2] && TX_LEN(hw, 0) == 12);
    CHECK(hw->ble_fp.TXBUF0CNTL == 0 && test_popped == 3);
    /* Both acknowledged, nothing queued: both retired and reset. */
    freed = frees;
    hw->ble_fp.TXTOG = 1; hw->ble_fp.TXBUF0CNTL = 1; hw->ble_fp.TXBUF1CNTL = 1;
    __hw_tx_process(ble_hw);
    CHECK(frees == freed + 2 && hw->tx[0] == 0 && hw->tx[1] == 0);
    CHECK(hw->ble_fp.TXDHDR0 == 0x0005 && hw->ble_fp.TXDHDR1 == 0x0001);
    CHECK(hw->ble_fp.TXAHDR0 == 0 && hw->ble_fp.TXAHDR1 == 0);
    CHECK((hw->ble_fp.INTFRAME & 64) == 0);
    /* A toggle between the snapshot and the commit, during an event: the
     * encrypted packet is staged, the slot left an empty PDU; between events
     * it goes into the current slot without being encrypted again. */
    test_queue_reset(hw, 2);
    struct ble_tx *old = (struct ble_tx *)test_bb.storage[4];
    memset(old, 0, sizeof(test_bb.storage[4]));
    old->len = 30; old->ack_callback = test_ack;
    hw->tx[0] = old; hw->ble_fp.TXTOG = 1;
    hw->ble_fp.TXBUF0CNTL = 1; hw->ble_fp.TXBUF1CNTL = 0;
    hw->ble_fp.TXDHDR0 = 0x1e06;
    static u8 busy_storage[sizeof(struct ble_tx) + 8];
    struct ble_tx *busy = (struct ble_tx *)busy_storage;
    hw->tx[1] = busy;
    test_toggle_on_ack = 0;
    __hw_tx_process(ble_hw);
    CHECK(test_popped == 1 && hw->tx[0] == 0 && hw->ble_fp.TXBUF0CNTL == 1);
    CHECK(hw->ble_fp.TXDHDR0 == 0x0005 && *ble_tx_staged(ble_hw) == test_queue[0]);
    hw->tx[1] = 0; hw->ble_fp.TXBUF1CNTL = 1;
    __hw_tx_process(ble_hw);
    CHECK(test_popped == 1 && *ble_tx_staged(ble_hw) == test_queue[0]);
    test_radio(1, 5);
    __hw_tx_process(ble_hw);
    CHECK(hw->tx[0] == test_queue[0] && *ble_tx_staged(ble_hw) == 0);
    CHECK(hw->tx[1] == test_queue[1] && test_popped == 2);
    /* An interrupt finding the lock taken makes the holder run again. */
    test_queue_reset(hw, 1);
    test_radio(0, 5);
    hw->tx[1]->ack_callback = test_ack;
    hw->ble_fp.TXTOG = 0; hw->ble_fp.TXBUF0CNTL = 0; hw->ble_fp.TXBUF1CNTL = 1;
    test_nested = 1;
    unsigned before = acks;
    __hw_tx_process(ble_hw);
    CHECK(acks == before + 1 && hw->tx_lock.counter == 0 && hw->tx[1] == test_queue[0]);
    /* A closed link drops a staged packet. */
    *ble_tx_staged(ble_hw) = old;
    freed = frees;
    ble_tx_drop_staged(ble_hw);
    CHECK(*ble_tx_staged(ble_hw) == 0 && frees == freed + 1);
    hw->tx[0] = hw->tx[1] = 0; test_tx_head = 0;
    test_radio(0, 0);
    bb_hdl = saved_bb;
}

int main(void)
{
    struct ble4_hw hw = {0};
    scan_window_adjust(&hw, 10, 20);
    CHECK(hw.scan_idle == 10 && hw.scan_window == 10 && hw.scan_interval == 20);
    scan_window_adjust(&hw, 12, 14);
    CHECK(hw.scan_idle == 2 && hw.scan_window == 10 && hw.scan_interval == 14);
    scan_window_adjust(&hw, 9, 4);
    CHECK(hw.scan_idle == 0 && hw.scan_window == 1 && hw.scan_interval == 5);
    scan_window_adjust(&hw, 11, 11);
    CHECK(hw.scan_idle == 0 && hw.scan_window == 7 && hw.scan_interval == 11);

    __set_winsize(&hw, 0x4321, 0x12345678);
    CHECK(hw.ble_fp.WINCNTL0 == 0x5678 && hw.ble_fp.WINCNTL1 == 0x1234);
    CHECK(hw.ble_fp.WINCNTL2 == 0x4321 && hw.window == 0x12345678);
    __set_widen(&hw, 624);
    CHECK(hw.ble_fp.WIDEN0 == 0 && hw.ble_fp.WIDEN1 == (u16)(624 | 0x5000) && hw.widen == 624);
    __set_widen(&hw, 625);
    CHECK(hw.ble_fp.WIDEN0 == 1 && hw.ble_fp.WIDEN1 == 0x5000 && hw.widen == 625);
    __set_widen(&hw, -626);
    CHECK(hw.ble_fp.WIDEN0 == 0xffff && hw.ble_fp.WIDEN1 == 0xffff && hw.widen == (u32)-626);
    struct ble_hw base = {0};
    u8 phy_error;
    CHECK(conn_update_small_winsize(base.phy.coded_s, &phy_error) == 150 && !phy_error);
    base.phy.coded_s = 1;
    CHECK(conn_update_small_winsize(base.phy.coded_s, &phy_error) == 80 && !phy_error);
    base.phy.coded_s = 2;
    CHECK(conn_update_small_winsize(base.phy.coded_s, &phy_error) == 50 && phy_error);
    struct ble_rx rx = {0};
    rx.type = 3; rx.llid = 2; rx.len = 37; rx.invalid = 0;
    rx_mark_invalid(&rx);
    CHECK(rx.type == 3 && rx.llid == 1 && rx.len == 0 && rx.invalid == 1);
    static const u8 raw_mac[6] = {0x20, 0x22, 0x33, 0x44, 0x55, 0x66};
    CHECK(memcmp(rf_ble_le_mac_addr, raw_mac, 6) == 0);
    /* Raw switch cases 1/3/7 choose sample two; 0/2/4/5/6 choose one. */
    for (u8 diff = 0; diff < 8; ++diff)
        CHECK(tx_soft_snapshot(5, 5 ^ diff) ==
              ((diff == 1 || diff == 3 || diff == 7) ? (5 ^ diff) : 5));
    void *regs = mmap((void *)0x1c8000, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(regs == (void *)0x1c8000);
    static struct ble_baseband_layer_handle baseband;
    static struct ble_hw instances[8];
    static struct ble4_hw frames[8];
    bb_hdl = &baseband; baseband.hw_base.hw = instances;
    baseband.hw_base.inst[0] = 1; baseband.number = 2;
    instances[0].ble4_hw = &frames[0];
    frames[0].state = 1; frames[0].scan_interval = 16;
    frames[0].scan_idle = 5;
    ble_slot_schedule();
    CHECK(schedules == 1 && scheduled_slots == 16);
    CHECK(frames[0].sub_state == 2 && sche_lock == 0);
    frames[0].sub_state = 0; mutate_schedule_state = 1;
    ble_slot_schedule();
    CHECK(schedules == 2 && frames[0].sub_state == 1);
    /* The RX invalid path skips normal state filters, preserves type bit zero,
     * and reloads its handler after the TWS callback. */
    struct ble_rx packet = {0};
    frames[0].state = 2; frames[0].priv = &frames[0];
    frames[0].handler = &initial_handler;
    frames[0].ble_fp.RXAHDR0 = 3;
    frames[0].ble_fp.RXDHDR0 = 0x1200;
    frames[0].ble_fp.RXSTAT0 = 4;
    __hw_rx_buf_process(&instances[0], &packet, 0);
    CHECK(packet.type == 3 && packet.llid == 1 && packet.len == 0 && packet.invalid);
    u16 packed_header;
    memcpy(&packed_header, (u8 *)&packet + offsetof(struct ble_rx, len) - 2, 2);
    CHECK(packed_header & 0x8000);
    CHECK(tws_probes == 1 && rx_probes == 1 && queues == 0 && frees == 1);
    frames[0].handler = &initial_handler;
    frames[0].ble_fp.RXAHDR0 = 0;
    frames[0].ble_fp.RXSTAT0 = 4;
    __hw_rx_buf_process(&instances[0], &packet, 0);
    CHECK(tws_probes == 2 && rx_probes == 2 && frees == 2);
    /* A vendor-blocked MD update still asks whether the queue is empty. */
    struct ble_tx tx = {0};
    frames[0].tx[0] = &tx;
    config_vendor_le_bb = 8;
    unsigned before_empty = empty_calls;
    ble_tx_update_md(&instances[0], 0);
    CHECK(empty_calls == before_empty + 1 && tx.md == 0);
    frames[0].tx[0] = 0;
    config_vendor_le_bb = 0;
    /* The raw TX header is a word at len-2: type/LLID/address flags in
     * byte zero, SN/MD in byte one. Check builders against typed fields. */
    tx.type = 9; tx.llid = 2; tx.txadd = 0; tx.rxadd = 1;
    tx.sn = 1; tx.md = 1;
    __tws_scan_req_pdu(&tx);
    CHECK(tx.type == 3 && tx.llid == 0 && tx.rxadd == 1);
    CHECK(tx.sn == 1 && tx.md == 1 && tx.len == 0);
    tx.type = 5; tx.llid = 3; tx.txadd = 1; tx.rxadd = 1;
    tx.sn = 1; tx.md = 1;
    frames[0].ble_fp.TXDHDR0 = 4;
    frames[0].ble_fp.TXDHDR1 = 0;
    ble_hw_tx(&instances[0], &tx, 0);
    ble_hw_tx(&instances[0], &tx, 1);
    CHECK(frames[0].ble_fp.TXAHDR0 == 0x35);
    CHECK(frames[0].ble_fp.TXAHDR1 == 0x35);
    CHECK(frames[0].ble_fp.TXDHDR0 == 15);
    CHECK(frames[0].ble_fp.TXDHDR1 == 11);
    tx.md = 0; ble_hw_tx(&instances[0], &tx, 0);
    CHECK(frames[0].ble_fp.TXDHDR0 == 7);
    /* SDK controls 21/22 have a one-byte length prefix. */
    const u8 adv_input[] = {3, 0x91, 0x72, 0x53};
    const u8 scan_input[] = {2, 0x36, 0x18};
    le_hw_ioctrl(&instances[0], 21, adv_input);
    le_hw_ioctrl(&instances[0], 22, scan_input);
    CHECK(frames[0].adv_len == 3 && frames[0].scan_rsp_len == 2);
    CHECK(memcmp(frames[0].adv_data, adv_input + 1, 3) == 0);
    CHECK(memcmp(frames[0].scan_rsp_data, scan_input + 1, 2) == 0);
    CHECK(frames[0].data_flush == 3);
    le_hw_ioctrl(&instances[0], 5, 0x4321, 0x12345678);
    CHECK(frames[0].ble_fp.WINCNTL0 == 0x5678);
    CHECK(frames[0].ble_fp.WINCNTL1 == 0x1234);
    CHECK(frames[0].ble_fp.WINCNTL2 == 0x4321);
    bt_le_prio_settings(1);
    u16 flags; memcpy(&flags, (u8 *)&frames[0] + offsetof(struct ble4_hw, widen) - 2, 2);
    CHECK(flags & 8192);
    bt_le_prio_settings(0);
    memcpy(&flags, (u8 *)&frames[0] + offsetof(struct ble4_hw, widen) - 2, 2);
    CHECK((flags & 8192) == 0);
    CHECK(HW_ID((struct ble_hw *)((uintptr_t)instances - 28)) == 255);
    /* SDK shifts the original channel booleans as bytes. */
    __set_adv_channel_index(&instances[0], 7);
    CHECK(*(volatile u32 *)1867808 == 0x41a5);
    __set_adv_channel_index(&instances[0], 0);
    CHECK(*(volatile u32 *)1867808 == 0x4040);
    test_tx_slots(&instances[0], &frames[0]);
    munmap(regs, 4096);
    /* Host flow control: data the host has no room for stays queued, and
     * the link reports nothing to take until there is room. */
    {
        static struct ble_rx held_rx;
        unsigned freed = frees;
        int empty = 0;

        held_rx.llid = 2; held_rx.len = 5;
        test_tx_head = &frames[0].rx_head; test_queue[0] = (struct ble_tx *)&held_rx;
        test_queued = 1; test_popped = 0; host_room = 0; host_asks = uploads = 0;
        le_hw_upload_data(&instances[0], test_upload);
        le_hw_ioctrl(&instances[0], 19, &empty);
        CHECK(!uploads && test_popped == 0 && empty && host_asks == 1 && frees == freed);
        host_room = 1;
        le_hw_upload_data(&instances[0], test_upload);
        empty = 0;
        le_hw_ioctrl(&instances[0], 19, &empty);
        CHECK(uploads == 1 && test_popped == 1 && empty && frees == freed + 1 && !ll_resumes);
        /* LL control PDUs do not ask */
        held_rx.llid = 3; test_queued = 2; test_queue[1] = (struct ble_tx *)&held_rx; host_room = 0;
        le_hw_upload_data(&instances[0], test_upload);
        CHECK(uploads == 2 && host_asks == 2 && !ll_resumes);
        /* On a connection, a buffer the radio went without is given back */
        struct ble_baseband_layer_handle *was = bb_hdl;
        bb_hdl = &test_bb.baseband; test_bb.baseband.hw_base.hw = instances;
        test_rx_buffer = test_bb.storage[4];
        frames[0].state = 6; frames[0].rx[0] = NULL; frames[0].rx[1] = &held_rx;
        frames[0].ble_fp.RXBUF0CNTL = 1;
        test_queued = 4; test_queue[2] = test_queue[3] = (struct ble_tx *)&held_rx;
        le_hw_upload_data(&instances[0], test_upload);
        CHECK(uploads == 3 && frames[0].rx[0] == test_rx_buffer && frames[0].rx[1] == &held_rx);
        CHECK(ll_resumes == 1);   /* one more waits: the next pass is asked for */
        CHECK(!(frames[0].ble_fp.RXBUF0CNTL & 1) &&
              frames[0].ble_fp.RXPTR0 == (s16)PHY_TO_BLE(((struct ble_rx *)test_rx_buffer)->data));
        bb_hdl = was; test_tx_head = NULL; frames[0].state = 0;
    }
    return TEST_DONE("btctrler_RF_ble");
}
#endif
