/* LE direct-test mode and its controller callbacks.  Keep the SDK's shared
 * DMA layout, PHY encodings, channel tables, register programming order, and
 * command quirks intact. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef UNITTEST
#include "test.h"
#endif

/* This object has private, no-argument helpers with the same names as public
 * RF_ble APIs in other archive members.  Rename only the declarations pulled
 * in by glue.h; the local definitions below keep the SDK's private names. */
#define __get_ble_anchor_con sdk_rf_get_ble_anchor_con
#define __get_interval sdk_rf_get_interval
#define __set_anchor_cnt sdk_rf_set_anchor_cnt
#define __set_anchor_cnt_disable sdk_rf_set_anchor_cnt_disable
#define __set_ble_anchor_con sdk_rf_set_ble_anchor_con
#define __set_interval sdk_rf_set_interval
#define __set_winsize sdk_rf_set_winsize
#include "btctrler/glue.h"
#undef __get_ble_anchor_con
#undef __get_interval
#undef __set_anchor_cnt
#undef __set_anchor_cnt_disable
#undef __set_ble_anchor_con
#undef __set_interval
#undef __set_winsize

static struct dut_ble_baseband_layer_handle *dut_hdl;
static volatile s32 rx_cnt;
u8 rx_phy_s;
union rx_length_t payload_length __attribute__((common));
static const hci_transport_t *hci_transport;
const u8 le_mac_addr[6] = { 0x20, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const struct ble_dut_ops_t ble_dut_ops;
const struct ble_dut_ops_t *__ble_dut_ops = &ble_dut_ops;
struct dut_rx_param rx_param __attribute__((common));
struct dut_tx_param tx_param __attribute__((common));
extern BTCTRLER_CONFIG int config_btctler_le_hw_nums;
extern BTCTRLER_CONFIG int config_btctler_le_rx_nums;
extern BTCTRLER_CONFIG int config_btctler_le_acl_packet_length;
extern BTCTRLER_CONFIG int config_btctler_le_acl_total_nums;
extern BTCTRLER_CONFIG int config_dut_protocol_mode;
void put_buf(void *data, int len);
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
void log_print(int level, const char *file, const char *format, ...);

#ifdef UNITTEST
#define DUT_IRQ
#else
#define DUT_IRQ __attribute__((interrupt("")))
#endif
static void ble_event_irq_handler(void) DUT_IRQ;
static void ble_rx_irq_handler(void) DUT_IRQ;
static void prestart_cnt_adjust(struct ble_hw *hw);
static u32 __get_ble_anchor_con(u8 index);
static u32 __get_interval(void);
static void __set_ble_anchor_con(u8 index, u32 value);
void rx_test_pre_set(void);
void tx_test_pre_set(u8 data);
static void dut_hci_emit_event_of_cmd_complete(u16 opcode, const char *format, ...);

#ifdef UNITTEST
static volatile u32 dut_test_mmio[8192];
static u32 test_interval;
static u32 test_interval_written;
static u32 test_anchor_con[8];
static u32 test_anchor_written;
static inline volatile u32 *dut_test_reg32(uintptr_t address)
{
    CHECK(address >= 1867776 && address <= 1899712 && (address & 3) == 0);
    return &dut_test_mmio[(address - 1867776) / 4];
}
#define DUT_REG32(address) (*dut_test_reg32(address))
#else
#define DUT_REG32(address) (*(volatile u32 *)(uintptr_t)(address))
#endif
#ifdef UNITTEST
#define DUT_CSYNC() ((void)0)
#else
#define DUT_CSYNC() __asm__ volatile("csync")
#endif

#ifndef UNITTEST
_Static_assert(offsetof(struct ble_hw_base, hw) == 16, "SDK ble_hw_base.hw offset");
_Static_assert(offsetof(struct ble_hw_base, hw_entity) == 20, "SDK ble_hw_base.hw_entity offset");
_Static_assert(offsetof(struct ble_hw_base, rx_dma) == 24, "SDK ble_hw_base.rx_dma offset");
_Static_assert(offsetof(struct ble_hw_base, tx_dma) == 28, "SDK ble_hw_base.tx_dma offset");
_Static_assert(sizeof(struct ble_hw_base) == 32, "SDK ble_hw_base size");
_Static_assert(sizeof(struct ble_hw) == 28, "SDK ble_hw size");
_Static_assert(offsetof(struct _hw_entity, u) == 0, "SDK _hw_entity union offset");
_Static_assert(sizeof(struct _hw_entity) == 628, "SDK _hw_entity size");
_Static_assert(offsetof(struct dut_ble_baseband_layer_handle, hw_base) == 0,
               "SDK DUT base offset");
_Static_assert(offsetof(struct dut_ble_baseband_layer_handle, number) == 32,
               "SDK DUT number offset");
_Static_assert(offsetof(struct dut_ble_baseband_layer_handle, set_le_pwr_level) == 33,
               "SDK DUT power field offset");
#endif

static u8 dut_get_dual_mode_flag(void) { return (u8)config_btctler_modules & 1; }
static u8 dut_get_fcc_mode_flag(void) { return (u8)config_btctler_mode & 4; }

static void __ble_baseband_reset(void)
{
    DUT_REG32(1867776) |= 64;
}

static void __set_anchor_cnt(void)
{
    DUT_REG32(1867808) = 0; DUT_CSYNC(); DUT_REG32(1867804) = 7173;
    DUT_REG32(1867808) = 0; DUT_CSYNC(); DUT_REG32(1867804) = 5;
    DUT_REG32(1867808) = 0; DUT_CSYNC(); DUT_REG32(1867804) = 14341;
    DUT_REG32(1867808) = 0; DUT_CSYNC(); DUT_REG32(1867804) = 5;
    DUT_REG32(1867808) = 32768; DUT_CSYNC(); DUT_REG32(1867804) = 14341;
}

static void __set_anchor_cnt_disable(void)
{
    DUT_REG32(1867808) = 0;
    DUT_CSYNC();
    DUT_REG32(1867804) = 14341;
}

static void __set_fixed_channel_index(u8 index)
{
    u32 channel_index;
    if (index == 0) channel_index = 37;
    else if (index < 12) channel_index = index - 1;
    else if (index == 12) channel_index = 38;
    else channel_index = index < 39 ? index - 2 : index;
    DUT_REG32(1867808) = channel_index | 8192;
    DUT_CSYNC();
    DUT_REG32(1867804) = 6149;
}

static void __set_hw_state(u32 state)
{
    dut_hdl->ble4_dut->ble4_hw->state = (u8)state;
    DUT_REG32(1867808) = state << 12;
    DUT_CSYNC();
    DUT_REG32(1867804) = 2053;
}

static void __set_interval(u32 interval)
{
#ifdef UNITTEST
    test_interval_written = interval;
#endif
    s32 units = (s32)interval / 625;
    DUT_REG32(1867808) = (u32)units & 65535;
    DUT_CSYNC();
    DUT_REG32(1867804) = 1029;
    DUT_REG32(1867808) = (u32)(units >> 16);
    DUT_CSYNC();
    DUT_REG32(1867804) = 15365;
}

static u32 __get_ble_anchor_con(u8 index)
{
#ifdef UNITTEST
    return test_anchor_con[index & 7];
#else
    DUT_REG32(1867804) = ((u32)index << 10) | 2;
    DUT_CSYNC();
    return DUT_REG32(1867812);
#endif
}

static u32 __get_interval(void)
{
#ifdef UNITTEST
    return test_interval;
#else
    DUT_REG32(1867804) = 1026;
    DUT_CSYNC();
    u32 low = DUT_REG32(1867812);
    DUT_REG32(1867804) = 5122;
    DUT_CSYNC();
    u32 high = DUT_REG32(1867812);
    return ((high << 16) & 16711680) | (low & 65535);
#endif
}

static void __set_ble_anchor_con(u8 index, u32 value)
{
#ifdef UNITTEST
    if (index == 7) test_anchor_written = value;
#endif
    DUT_REG32(1867808) = value;
    DUT_CSYNC();
    DUT_REG32(1867804) = ((u32)index << 10) | 5;
}

static void __set_winsize(u32 winsize)
{
    struct ble4_hw *hw = dut_hdl->ble4_dut->ble4_hw;
    hw->ble_fp.WINCNTL0 = (u16)winsize;
    hw->ble_fp.WINCNTL1 = (u16)(winsize >> 16);
    hw->ble_fp.WINCNTL2 = 150;
}

static void __dut_set_data_rx_phy(struct ble4_hw *hw, u8 phy)
{
    if (hw) hw->ble_fp.INTFRAME = (hw->ble_fp.INTFRAME & (u16)-14337) | ((phy & 7) << 11);
}

static void __dut_set_data_tx_phy(struct ble_hw *hw, u8 phy)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    if (hw4) hw4->ble_fp.INTFRAME = (hw4->ble_fp.INTFRAME & (u16)-1793) | ((phy & 7) << 8);
}

static void __dut_set_data_tx_phy_coded_s(struct ble_hw *hw)
{
    struct ble4_hw *hw4 = hw->ble4_hw;
    u16 coded_s = hw->phy.coded_s;
    hw4->ble_fp.INTFRAME = (coded_s << 14) | (hw4->ble_fp.INTFRAME & 16383);
    hw4->ble_fp.TXDHDR0 = ((coded_s << 4) & 16) | (hw4->ble_fp.TXDHDR0 & (u16)-17);
    hw4->ble_fp.TXDHDR1 = ((coded_s << 4) & 16) | (hw4->ble_fp.TXDHDR1 & (u16)-17);
}

static void __dut_set_channel_map(struct ble4_hw *hw, const u8 *channel, u8 mode)
{
    u32 count = 0;
    u32 k = 0;
    u32 mode_bits = mode ? 2 : 0;
    for (u32 i = 0; i < 37; ++i) {
        if (i == 11) k = 2;
        hw->ble_fp.FRQ_IDX0[i] = (u8)i;
        u8 value = (u8)(((i + 1) << 1) + mode_bits + k);
        hw->ble_fp.FRQ_TBL0[i] = value;
        if (channel[i >> 3] & (1 << (i & 7))) {
            hw->ble_fp.FRQ_IDX1[count] = (u8)i;
            hw->ble_fp.FRQ_TBL1[count++] = value;
        }
    }
    static const u8 extra[3] = { 0x00, 0x18, 0x4e };
    for (u32 i = 0; i < 3; ++i) {
        u32 index = i + 37;
        u8 value = extra[i] + mode_bits;
        hw->ble_fp.FRQ_IDX0[index] = (u8)index;
        hw->ble_fp.FRQ_TBL0[index] = value;
        hw->ble_fp.FRQ_IDX1[index] = (u8)index;
        hw->ble_fp.FRQ_TBL1[index] = value;
    }
    hw->ble_fp.CHMAP0 = ((u16)channel[1] << 8) | channel[0];
    hw->ble_fp.CHMAP1 = ((u16)channel[3] << 8) | channel[2];
    hw->ble_fp.CHMAP2 = (channel[4] & 31) | (count << 5);
}

static void cpu_reset(void) { p33_soft_reset(); }

static struct ble_hw *dut_ble4_hw_alloc(void)
{
    struct ble_hw *hw = NULL;
    s32 count = config_btctler_le_hw_nums;
    for (s32 i = 0; i < count; ++i) {
        if (dut_hdl->hw_base.inst[i] == 0) {
            hw = dut_hdl->hw_base.hw + i;
            memset(hw, 0, 28);
            break;
        }
    }
    if (!hw) {
        if (config_asser) btctrler_assert_failed("hw != NULL BLE HW NUM limit!!!", 567);
        else cpu_reset();
    }
    struct _hw_entity *entity = NULL;
    for (s32 i = 0; i < count; ++i) {
        if (!dut_hdl->hw_base.hw_entity[i].used) {
            entity = &dut_hdl->hw_base.hw_entity[i];
            memset(entity, 0, 628);
            entity->used = 1;
            break;
        }
    }
    if (!entity) {
        if (config_asser) btctrler_assert_failed("hw != NULL hw_entiry_alloc error", 494);
        else cpu_reset();
    }
    hw->ble4_hw = entity ? &entity->u.ble4_hw : NULL;
    if (config_asser) {
        if (!hw->ble4_hw) btctrler_assert_failed("hw->ble4_hw ", 601);
    } else if (!hw->ble4_hw) cpu_reset();
    struct ble4_hw *fp_addr = hw->ble4_hw;
    s32 slot_delta = (s32)((u32)(uintptr_t)hw -
                           (u32)(uintptr_t)dut_hdl->hw_base.hw);
    s32 slot_index = slot_delta / 28;
    u32 entity_offset = (u32)(uintptr_t)fp_addr - (u32)(uintptr_t)dut_hdl;
    if (config_asser) {
        if (entity_offset >= 65535) btctrler_assert_failed("(u32)(fp_addr)-(u32)&__this->hw_base < 0xffff ", 551);
    } else if (entity_offset >= 65535) cpu_reset();
    u32 inst_entity_offset = (u32)(uintptr_t)fp_addr - (u32)(uintptr_t)dut_hdl;
    dut_hdl->hw_base.inst[(u32)slot_index & 255] =
        (u16)((inst_entity_offset & 65535) | 0x8000);
    if (hw->ble4_hw) {
        hw->ble4_hw->regs = btctler_nv_memory_malloc(40);
        ++dut_hdl->number;
    }
    return hw;
}

static void dut_le_hw_init(void)
{
    log_print(2, NULL, "[Info]: [LE-DUT]\n--func=%s\n\r\n", "dut_le_hw_init");
    DUT_REG32(1899648) |= 1;
    DUT_REG32(1867776) = 1;
    DUT_REG32(1867828) = 2305;
    s32 hw_count = config_btctler_le_hw_nums;
    s32 hw_bytes = hw_count * 28 + 308;
    s32 entity_bytes = hw_bytes + hw_count * 628;
    s32 rx_bytes = config_btctler_le_rx_nums * 294 + 31;
    s32 dma_bytes = config_btctler_le_acl_packet_length;
    s32 mps27 = (dma_bytes / 27) * 70 + 70;
    s32 mps251 = (dma_bytes / 251) * 294 + 294;
    s32 per_packet = dma_bytes / (mps27 > mps251 ? 27 : 251) + 1;
    s32 acl_bytes = config_btctler_le_acl_total_nums * per_packet * (mps27 > mps251 ? 70 : 294);
    s32 total = rx_bytes + entity_bytes + 31 + acl_bytes;
    if (!dut_hdl) {
        dut_hdl = malloc((u32)total);
        if (!dut_hdl) {
            if (config_asser) btctrler_assert_failed("__this", 1873);
            else cpu_reset();
        }
        memset(dut_hdl, 0, (u32)total);
        dut_hdl->set_le_pwr_level = 10;
        dut_hdl->hw_base.hw = (struct ble_hw *)((u8 *)dut_hdl + 308);
        dut_hdl->hw_base.hw_entity = (struct _hw_entity *)((u8 *)dut_hdl + hw_bytes);
        dut_hdl->hw_base.rx_dma = (u8 *)dut_hdl + entity_bytes;
        dut_hdl->hw_base.tx_dma = (u8 *)dut_hdl + entity_bytes + rx_bytes;
        DUT_REG32(1899652) = (u32)(uintptr_t)dut_hdl;
        DUT_REG32(1899712) = (u32)(uintptr_t)((u8 *)dut_hdl + 32);
        memset(dut_hdl, 0, 8);
        dut_hdl->packet_cnt = 0;
        dut_hdl->pkt_err_cnt = 0;
        dut_hdl->le_test_enable = 0;
    } else log_print(4, NULL, "<Error>: [LE-DUT]dut mode had intialized\n\r\n");
    request_irq(39, 2, ble_event_irq_handler, 0);
    request_irq(38, 2, ble_rx_irq_handler, 0);
}

void dut_le_set_mac(u8 addr_type, const u8 *addr)
{
    struct ble4_hw *hw = dut_hdl->ble4_dut->ble4_hw;
    log_print(2, NULL, "[Info]: [LE-DUT]\n--func=%s\n\r\n", "__dut_set_local_addr");
    hw->ble_fp.FORMAT |= 8;
    hw->local.addr_type = addr_type != 0;
    memcpy(hw->local.addr, addr, 6);
    hw->ble_fp.LOCALADRL = ((u16)addr[1] << 8) | addr[0];
    hw->ble_fp.LOCALADRM = ((u16)addr[3] << 8) | addr[2];
    hw->ble_fp.LOCALADRU = ((u16)addr[5] << 8) | addr[4];
}

static void le_test_frame_init(u8 tx_pwr)
{
    struct ble4_hw *hw = dut_hdl->ble4_dut->ble4_hw;
    u8 max_pwr = dut_hdl->set_le_pwr_level;
    u8 channel[5];
    hw->ble_fp.ANCHOR = 0;
    hw->ble_fp.BITOFF = 0;
    hw->ble_fp.ADVIDX = -11384;
    hw->ble_fp.WIDEN0 = 0;
    hw->ble_fp.WIDEN1 = 20500;
    hw->ble_fp.BDADDR0 = 16681;
    hw->ble_fp.BDADDR1 = 29046;
    hw->ble_fp.CRCWORD0 = 21845;
    hw->ble_fp.CRCWORD1 = 85;
    hw->ble_fp.FORMAT &= (u16)-33;
    hw->ble_fp.OPTCNTL = 62;
    hw->ble_fp.INTFRAME |= 2;
    hw->ble_fp.INTFRAME |= 4;
    DUT_REG32(1867784) = (DUT_REG32(1867784) & (u32)-1024) | 68;
    DUT_REG32(1867784) |= 61440;
    DUT_REG32(1867788) = (DUT_REG32(1867788) & (u32)-1024) | 68;
    DUT_REG32(1867792) = (DUT_REG32(1867792) & (u32)-256) | 100;
    DUT_REG32(1867792) = (DUT_REG32(1867792) & (u32)-65281) | 31744;
    DUT_REG32(1867796) = (DUT_REG32(1867796) & (u32)-256) | 30;
    DUT_REG32(1867800) = (DUT_REG32(1867800) & (u32)-256) | 20;
    DUT_REG32(1867800) = (DUT_REG32(1867800) & (u32)-65281) | 5120;
    (void)ble_agc_normal_set(hw, 0, 25);
    (void)ble_txpwr_normal_set(hw, 0, max_pwr < tx_pwr ? max_pwr : tx_pwr);
    s16 offset = rf.osc_offset_flag ? rf.osc_offset : rf_osc_offset_initial_val_get();
    s32 neg_offset = 0 - (s32)offset;
    s32 scaled_offset = (s32)((u32)neg_offset << 20);
    hw->ble_fp.osc_set = (u32)(scaled_offset / 24000);
    hw->ble_fp.RFPRIOCNTL = 542;
    hw->ble_fp.RFPRIOSTAT = 30;
    hw->ble_fp.TXTOG = 0;
    hw->ble_fp.RXTOG = 0;
    hw->ble_fp.TXAHDR0 = 0;
    hw->ble_fp.TXDHDR0 |= 1;
    hw->ble_fp.TXDHDR0 &= (u16)-5;
    hw->ble_fp.TXDHDR0 &= (u16)-9;
    hw->ble_fp.TXDHDR0 &= 255;
    hw->ble_fp.TXAHDR1 = 0;
    hw->ble_fp.TXDHDR1 = 1;
    hw->ble_fp.TXDHDR1 |= 1;
    hw->ble_fp.TXDHDR1 &= (u16)-5;
    hw->ble_fp.TXDHDR1 &= (u16)-9;
    hw->ble_fp.TXDHDR1 &= 255;
    hw->ble_fp.RXMAXBUF = 255;
    hw->ble_fp.IFSCNT = -32107;
    hw->ble_fp.FILTERCNTL = 0;
    log_print(2, NULL, "[Info]: [LE-DUT]RxMaxBuf= %x\n\r\n", hw->ble_fp.RXMAXBUF);
    hw->ble_fp.TARGETADRL = 0;
    hw->ble_fp.TARGETADRM = 0;
    hw->ble_fp.TARGETADRU = 0;
    memset(channel, 255, sizeof(channel));
    __dut_set_channel_map(hw, channel, 0);
}

static void le_receive_test(const struct dut_rx_param *packet)
{
    struct ble_hw *ble = dut_hdl->ble4_dut;
    struct ble4_hw *hw = ble->ble4_hw;
    DUT_REG32(1867776) = 1;
    if (packet->coded_s == 255) memset(dut_hdl->hw_base.rx_dma, 0, 37);
    else memset(dut_hdl->hw_base.rx_dma, 0, packet->length);
    le_test_frame_init(bt_get_pwr_max_level());
    hw->ble_fp.ANCHOR = (u16)-32768;
    __set_fixed_channel_index(packet->channel);
    u32 dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.rx_dma - (u32)(uintptr_t)dut_hdl;
    if (config_asser) {
        if (dma_offset >= 65535)
            btctrler_assert_failed("(u32)(&(dut_hdl)->hw_base.rx_dma[0])-(u32)&__this->hw_base < 0xffff ", 1101);
    } else if (dma_offset >= 65535) cpu_reset();
    dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.rx_dma - (u32)(uintptr_t)dut_hdl;
    hw->ble_fp.RXPTR0 = (u16)dma_offset;
    if (config_asser) {
        if (dma_offset >= 65535)
            btctrler_assert_failed("(u32)(&(dut_hdl)->hw_base.rx_dma[0])-(u32)&__this->hw_base < 0xffff ", 1102);
    } else if (dma_offset >= 65535) cpu_reset();
    dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.rx_dma - (u32)(uintptr_t)dut_hdl;
    hw->ble_fp.RXPTR1 = (u16)dma_offset;
    hw->ble_fp.FORMAT |= 3072;
    __set_hw_state(7);
    u32 interval = 625;
    ble = dut_hdl->ble4_dut;
    switch (ble->phy.rx_phy) {
    case 0: __set_winsize(3000); break;
    case 1: __set_winsize(3000); interval = 625; break;
    case 2:
        log_print(2, NULL, "[Info]: [LE-DUT]coded phy test\n\r\n");
        if (packet->coded_s == 255) {
            ble->ble4_hw->ble_fp.WIDEN0 = 1;
            __set_winsize(13750);
            interval = 1875;
        } else {
            u32 bits = ((packet->length << 3) + 27) * packet->coded_s;
            u32 rounded = bits + 625;
            u32 rem = rounded % 625;
            interval = rounded - rem + (rem != 0 ? 625 : 0);
            ble->ble4_hw->ble_fp.WIDEN0 = 1;
            u32 winsize = interval + 10000;
            __set_winsize(winsize > 12500 ? winsize : 12500);
        }
        break;
    }
    log_print(2, NULL, "[Info]: [LE-DUT]ch_index:%x phy:%x IL=%d\n\r\n",
              packet->channel, dut_hdl->ble4_dut->phy.rx_phy, interval);
    __set_interval(interval);
    rx_cnt = 0;
    dut_hdl->packet_cnt = 0;
    dut_hdl->pkt_err_cnt = 0;
    dut_hdl->le_test_enable = 1;
    DUT_REG32(1867820) |= 1;
    DUT_REG32(1867820) |= 256;
    DUT_REG32(1867816) |= 1;
    DUT_REG32(1867816) |= 256;
    __set_anchor_cnt();
}

void le_receive_test_handle(u8 *data, int len)
{
    (void)len;
    log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_RECEIVER_TEST_1M\r\n");
    rx_test_pre_set();
    le_receive_test((const struct dut_rx_param *)data);
}

static void le_transmitter_test(const struct dut_tx_param *packet)
{
    struct ble4_hw *hw = dut_hdl->ble4_dut->ble4_hw;
    u8 channel[5];
    __ble_baseband_reset();
    __set_anchor_cnt_disable();
    le_test_frame_init(bt_get_pwr_max_level());
    u8 pattern = packet->patten;
    if (pattern == 0xf0) {
        close_zeroif();
        memset(channel, 255, sizeof(channel));
        __dut_set_channel_map(dut_hdl->ble4_dut->ble4_hw, channel, 1);
        pattern = packet->patten;
    }
    u16 format = 19456;
    switch (pattern) {
    case 0: break;
    case 1: memset(dut_hdl->le_buf, 15, packet->length); format = 3072; break;
    case 2: memset(dut_hdl->le_buf, 85, packet->length); format = 3072; break;
    case 3: format = 52224; break;
    case 4: memset(dut_hdl->le_buf, 255, packet->length); format = 3072; break;
    case 5: memset(dut_hdl->le_buf, 0, packet->length); format = 3072; break;
    case 6: memset(dut_hdl->le_buf, 0xf0, packet->length); format = 3072; break;
    case 7: memset(dut_hdl->le_buf, 0xaa, packet->length); format = 3072; break;
    case 0xf0: format = 11264; break;
    default: return;
    }
    memcpy(dut_hdl->hw_base.tx_dma, dut_hdl->le_buf, packet->length);
    hw->ble_fp.FORMAT |= format;
    log_print(2, NULL, "[Info]: [LE-DUT]TX_TEST:index:%x data_type = 0x%x len:%x\n\r\n",
              packet->channel, packet->patten, packet->length);
    hw->ble_fp.FORMAT &= (u16)-8193;
    __set_fixed_channel_index(packet->channel);
    hw->ble_fp.FORMAT = format;
    u32 dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.tx_dma - (u32)(uintptr_t)dut_hdl;
    if (config_asser) {
        if (dma_offset >= 65535)
            btctrler_assert_failed("(u32)(&(dut_hdl)->hw_base.tx_dma[0])-(u32)&__this->hw_base < 0xffff ", 1222);
    } else if (dma_offset >= 65535) cpu_reset();
    dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.tx_dma - (u32)(uintptr_t)dut_hdl;
    hw->ble_fp.TXPTR0 = (u16)dma_offset;
    if (config_asser) {
        if (dma_offset >= 65535)
            btctrler_assert_failed("(u32)(&(dut_hdl)->hw_base.tx_dma[0])-(u32)&__this->hw_base < 0xffff ", 1224);
    } else if (dma_offset >= 65535) cpu_reset();
    dma_offset = (u32)(uintptr_t)dut_hdl->hw_base.tx_dma - (u32)(uintptr_t)dut_hdl;
    hw->ble_fp.TXPTR1 = (u16)dma_offset;
    __set_hw_state(6);
    u32 bits = ((u32)packet->length << 3) + 80;
    log_print(2, NULL, "[Info]: [LE-DUT]bits = %d\n\r\n", bits);
    u32 interval = 625;
    if (bits >= 377) interval = bits < 1001 ? 1250 : (bits < 1625 ? 1875 : 2500);
    __set_interval(interval);
    u8 header_pattern = packet->patten;
    hw->ble_fp.TXAHDR0 = header_pattern;
    hw->ble_fp.TXDHDR0 = ((u16)packet->length << 8) | (hw->ble_fp.TXDHDR0 & 255);
    hw->ble_fp.TXAHDR1 = header_pattern;
    hw->ble_fp.TXDHDR1 = ((u16)packet->length << 8) | (hw->ble_fp.TXDHDR1 & 255);
    dut_hdl->le_test_enable = 1;
    DUT_REG32(1867820) |= 1;
    DUT_REG32(1867820) |= 256;
    DUT_REG32(1867816) |= 1;
    DUT_REG32(1867816) |= 256;
    __set_anchor_cnt();
}

void le_transmitter_test_handle(struct dut_tx_param *data, u32 len)
{
    (void)len;
    log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_TRANSMITTER_TEST\r\n");
    tx_test_pre_set(data->patten);
    le_transmitter_test(data);
}

static void prestart_cnt_adjust(struct ble_hw *hw __attribute__((unused)))
    __attribute__((section(".volatile_ram_code")));
static void prestart_cnt_adjust(struct ble_hw *hw __attribute__((unused)))
{
    u32 interval = __get_interval();
    if (interval > 1) return;
    s32 delta = 625 - (DUT_REG32(1867788) & 1023) - (DUT_REG32(1867796) & 255);
    u32 anchor = __get_ble_anchor_con(7);
    s32 anchor_signed;
    if (((s16)anchor) & 2048) anchor_signed = (s16)anchor | 63488;
    else anchor_signed = (s32)anchor;
    s32 sum = (s32)((u32)anchor_signed + (u32)delta);
    s32 scaled = (s32)((u32)sum << 16);
    s32 wrapped = (s16)sum;
    s32 powerup_offset = scaled > 40894464 ? wrapped - 625
        : (scaled < 0 ? wrapped + 625 : sum);
    if ((s16)powerup_offset <= 615) return;
    s32 add = (s16)anchor_signed + (delta & 65535);
    s32 adjust;
    if (add < 15 || ((add > 624) && (add - 15 < 625))) adjust = -15;
    else adjust = (add < 0 && add - 15 < -625) ? 610 : -15;
    __set_ble_anchor_con(7, (s16)((u32)adjust + (u32)anchor_signed));
}

static void DUT_IRQ ble_event_irq_handler(void)
{
    for (s32 i = 0; i < config_btctler_le_hw_nums; ++i) {
        u32 mask = 1u << i;
        if ((DUT_REG32(1867816) & mask) & DUT_REG32(1867824))
            DUT_REG32(1867820) |= mask;
    }
}

static void DUT_IRQ ble_rx_irq_handler(void)
{
    for (s32 i = 0; i < config_btctler_le_hw_nums; ++i) {
        u32 mask = 1u << (i + 8);
        if (((DUT_REG32(1867816) & mask) & DUT_REG32(1867824)) == 0) continue;
        DUT_REG32(1867820) |= mask;
        struct ble_hw *ble = dut_hdl->ble4_dut;
        struct ble4_hw *hw = ble->ble4_hw;
        u32 toggle = hw->ble_fp.RXTOG & 1;
        u16 rx_status = toggle ? hw->ble_fp.RXSTAT0 : hw->ble_fp.RXSTAT1;

        /* Preserve the SDK's odd baseband cast: rx_valid is updated through
         * a 16-bit access at ble_hw + 326, not through hw->ble4_hw. */
        struct ble4_hw *baseband_view = (struct ble4_hw *)(void *)ble;
        u16 *rx_flags = (u16 *)(void *)((u8 *)(void *)baseband_view +
                                        offsetof(struct ble4_hw, init) + 1);
        *rx_flags |= 1;
        prestart_cnt_adjust(ble);

        if ((rx_status & 7) != 1) {
            ++dut_hdl->pkt_err_cnt;
            (void)putchar('e');
            continue;
        }

        ++dut_hdl->packet_cnt;
        ++rx_cnt;
        if (rx_cnt != 1 && rx_cnt != 2) continue;

        u16 hdr = toggle ? hw->ble_fp.RXDHDR1 : hw->ble_fp.RXDHDR0;
        u16 length = (u16)(hdr >> 8);
        u16 length_plus_two = (u16)(length + 2);
        u32 interval = 0;
        u8 phy = dut_hdl->ble4_dut->phy.rx_phy;
        if (phy == 0) {
            u16 clocks = (u16)((length_plus_two << 3) + 313);
            interval = ((u32)(clocks / 625) + ((clocks % 625) != 0)) * 625;
            if ((interval == 625 && rx_cnt == 1) ||
                (interval > 625 && rx_cnt == 2)) {
                hw->ble_fp.WINCNTL0 = 50;
                hw->ble_fp.WINCNTL1 = 0;
                hw->ble_fp.WINCNTL2 = 150;
                __set_widen(baseband_view, 20);
                baseband_view->ble_fp.RXMAXBUF = length;
                * (volatile u16 *)(void *)ble &= 32767;
            }
        } else if (phy == 1) {
            u16 clocks = (u16)((length_plus_two << 2) + 285);
            interval = ((u32)(clocks / 625) + ((clocks % 625) != 0)) * 625;
            if (rx_cnt == 2 && interval >= 625) {
                hw->ble_fp.WINCNTL0 = 50;
                hw->ble_fp.WINCNTL1 = 0;
                hw->ble_fp.WINCNTL2 = 150;
                __set_widen(baseband_view, 10);
                baseband_view->ble_fp.RXMAXBUF = length;
                * (volatile u16 *)(void *)ble &= 32767;
            }
        } else if (phy == 2) {
            u32 code = (rx_status & 16) ? 2 : 8;
            u32 clocks = ((u32)length_plus_two << 3) + 27;
            clocks = clocks * code + 625;
            interval = (clocks / 625 + ((clocks % 625) != 0)) * 625;
            if (rx_cnt == 1) {
                __set_widen(baseband_view, 480);
                struct ble_hw *current = dut_hdl->ble4_dut;
                struct ble4_hw *current_hw = current->ble4_hw;
                current_hw->ble_fp.WINCNTL0 = 1000;
                current_hw->ble_fp.WINCNTL1 = 0;
                current_hw->ble_fp.WINCNTL2 = 150;
                baseband_view->ble_fp.RXMAXBUF = length;
                *(volatile u16 *)(void *)ble &= 32767;
            }
        }
        __set_interval(interval);
        (void)putchar('I');
        (void)putchar((int)(interval / 625) + 48);
    }
}

static void le_test_end(void)
{
    rx_cnt = 0;
    dut_hdl->packet_cnt = 0;
    dut_hdl->pkt_err_cnt = 0;
    dut_hdl->le_test_enable = 0;
    DUT_REG32(1867816) &= (u32)-2;
    DUT_REG32(1867816) &= (u32)-257;
    __set_anchor_cnt_disable();
    bsb_normal_setting();
}

static void dut_hci_emit_event_of_cmd_complete(u16 opcode, const char *format, ...)
{
    struct hci_event event;
    va_list args;
    va_start(args, format);
    int body_len = __vsprintf_len(format, args);
#ifdef UNITTEST
    va_end(args);
#endif
    event.len = (u8)(body_len + 3);
    event.event = 14;
    event.data[0] = 1;
    event.data[1] = (u8)opcode;
    event.data[2] = (u8)(opcode >> 8);
    va_start(args, format);
    (void)__vsprintf(&event.data[3], format, args);
    va_end(args);
    (void)hci_transport->send_packet(4, (const u8 *)&event, event.len + 2);
}

void le_dut_hci_reset_handle(void)
{
    log_print(2, NULL, "[Info]: [LE-DUT]HCI_RESET\r\n");
    if (dut_get_dual_mode_flag()) {
        dut_bredr_close_recall();
        log_print(2, NULL, "[Info]: [LE-DUT]edr close\n\r\n");
    }
    bt_reload_trim();
    le_test_end();
}

u16 le_test_end_handle(void)
{
    log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_TEST_END\r\n");
    u16 packet_count = dut_hdl->packet_cnt;
    log_print(2, NULL, "[Info]: [LE-DUT]test_end: total_cnt:%d valid_cnt:%d\r\n\r\n",
              rx_cnt, dut_hdl->packet_cnt);
    log_print(2, NULL, "[Info]: [LE-DUT]pkt_err_cnt :%d\n\r\n", dut_hdl->pkt_err_cnt);
    le_test_end();
    __ble_baseband_reset();
    bt_reload_trim();
    if (dut_get_fcc_mode_flag()) bt_pll_reset(1);
    if (dut_get_dual_mode_flag()) {
        dut_bredr_init_recall();
        log_print(2, NULL, "[Info]: [LE-DUT]edr init\n\r\n");
    }
    return packet_count;
}

void rx_test_pre_set(void)
{
    if (dut_get_dual_mode_flag()) {
        dut_bredr_close_recall();
        log_print(2, NULL, "[Info]: [LE-DUT]edr close\n\r\n");
    }
    bt_reload_trim();
    if (dut_get_fcc_mode_flag()) bt_pll_reset(0);
}

void tx_test_pre_set(u8 data)
{
    if (dut_get_dual_mode_flag()) {
        log_print(2, NULL, "[Info]: [LE-DUT]edr close\n\r\n");
        dut_bredr_close_recall();
    }
    if (data == 0xf0) {
        bt_set_single_carrier();
        log_print(2, NULL, "[Info]: [LE-DUT]single carrier test\n\r\n");
    }
    else bt_reload_trim();
    if (dut_get_fcc_mode_flag()) {
        certification(110);
        bt_pll_reset(1);
    }
}

void le_enhanced_transmitter_test_handle(struct dut_tx_param *data, u32 len)
{
    (void)len;
    log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_ENHANCED_TRANSMITTER_TEST\r\n");
    tx_test_pre_set(data->patten);
    struct ble_hw *ble = dut_hdl->ble4_dut;
    switch (data->coded_s) {
    case 1:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_1M_UNCODED_PHY\n\r\n");
        __dut_set_data_tx_phy(ble, 0); break;
    case 2:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_2M_UNCODED_PHY\n\r\n");
        __dut_set_data_tx_phy(ble, 1); break;
    case 3:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_1M_CODED_PHY_S8\n\r\n");
        __dut_set_data_tx_phy(ble, 2);
        ble->phy.coded_s = 0;
        __dut_set_data_tx_phy_coded_s(ble);
        break;
    case 4:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_1M_CODED_PHY_S2\n\r\n");
        __dut_set_data_tx_phy(ble, 2);
        ble->phy.coded_s = 1;
        __dut_set_data_tx_phy_coded_s(ble);
        break;
    }
    le_transmitter_test(data);
}

void le_enhanced_receiver_test_handle(struct dut_rx_param *data, int len)
{
    (void)len;
    rx_test_pre_set();
    struct ble_hw *ble = dut_hdl->ble4_dut;
    switch (data->phy) {
    case 1:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_1M_UNCODED_PHY\n\r\n");
        __dut_set_data_rx_phy(ble->ble4_hw, 0);
        ble->phy.rx_phy = 0;
        break;
    case 2:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_2M_UNCODED_PHY\n\r\n");
        __dut_set_data_rx_phy(ble->ble4_hw, 1);
        ble->phy.rx_phy = 1;
        bqb_2m_setting();
        break;
    case 3:
        log_print(2, NULL, "[Info]: [LE-DUT]HCI_LE_1M_CODED_PHY\n\r\n");
        __dut_set_data_rx_phy(ble->ble4_hw, 2);
        ble->phy.rx_phy = 2;
        break;
    }
    le_receive_test(data);
}

int ogf_vendor_le_controller(u16 opcode, const u8 *data, int len)
{
    (void)len;
    if ((opcode & 1023) != 133) return 0;
    log_print(2, NULL, "[Info]: [LE-DUT]power_set: %d\n\r\n", *data);
    if (dut_hdl) {
        dut_hdl->set_le_pwr_level = *data;
        log_print(2, NULL, "[Info]: [LE-DUT]pwr level=%d\n\r\n", (s8)*data);
    }
    if (config_btctler_mode & 2) dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
    else (void)hci_send_event_cmd_complete(opcode, "1", 0);
    return 1;
}

void le_test_event_process(void)
{
}

static void dut_hci_le_process(int packet_type, const u8 *packet, int size)
{
    (void)size;
    if (packet_type != 1) return;

    u8 ogf = packet[1] >> 2;
    u16 opcode = (u16)(((u16)packet[1] << 8) | packet[0]);
    u16 command = opcode & 1023;
    if (ogf == 3) {
        log_print(2, NULL, "[Info]: [LE-DUT]OGF_CONTROLLER_BASEBAND:%x\n\r\n", opcode);
        if (opcode == 3075) {
            le_dut_hci_reset_handle();
            dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
        }
    } else if (ogf == 8) {
        const u8 *parameters = packet + 3;
        switch (command) {
        case 29: {
            struct ble_hw *ble = dut_hdl->ble4_dut;
            if (ble->ble4_hw) ble->ble4_hw->ble_fp.INTFRAME &= (u16)-14337;
            ble->phy.rx_phy = 0;
            rx_param.channel = parameters[0];
            le_receive_test_handle(&rx_param.channel, 0);
            dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
            break;
        }
        case 30:
            log_print(2, NULL, "[Info]: [LE-DUT]HCI_1M_UNCODED_PHY\n\r\n");
            tx_param.channel = parameters[0];
            tx_param.length = parameters[1];
            tx_param.patten = parameters[2];
            if (dut_hdl->ble4_dut->ble4_hw)
                dut_hdl->ble4_dut->ble4_hw->ble_fp.INTFRAME &= (u16)-1793;
            le_transmitter_test_handle(&tx_param, 0);
            dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
            break;
        case 31: {
            tx_param.channel = (u8)-1;
            tx_param.length = 0;
            tx_param.patten = (u8)-1;
            tx_param.coded_s = (u8)-1;
            rx_param.channel = (u8)-1;
            rx_param.phy = (u8)-1;
            rx_param.length = 0;
            rx_param.coded_s = (u8)-1;
            u16 packet_count = le_test_end_handle();
            dut_hci_emit_event_of_cmd_complete(opcode, "12", 0, packet_count);
            break;
        }
        case 51:
            rx_param.channel = parameters[0];
            rx_param.phy = parameters[1];
            rx_param.coded_s = (u8)-1;
            le_enhanced_receiver_test_handle(&rx_param, 0);
            dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
            break;
        case 52:
            le_enhanced_transmitter_test_handle(
                (struct dut_tx_param *)(void *)parameters, 0);
            dut_hci_emit_event_of_cmd_complete(opcode, "1", 0);
            break;
        default:
            break;
        }
    } else if (ogf == 62) {
        log_print(2, NULL, "[Info]: [LE-DUT]OGF_VENDOR\r\n");
        (void)ogf_vendor_le_controller(opcode, packet + 3, 0);
    }
    log_print(2, NULL, "[Info]: [LE-DUT]BLE_CON0=0x%x\r\n", DUT_REG32(1867776));
}

void two_wire_setup_process(u8 control, u8 parameter)
{
    log_print(2, NULL, "[Info]: [LE-DUT]control:%x,packet[1]:%x\n\r\n",
              control, parameter);
    switch (control) {
    case 0:
        le_dut_hci_reset_handle();
        break;
    case 1:
        payload_length.length = (payload_length.length & 63) |
                                ((parameter >> 2) << 6);
        log_print(2, NULL, "[Info]: [LE-DUT]h_data_length set:%x \n\r\n",
                  (parameter >> 2) & 3);
        break;
    case 2: {
        u8 selector = (u8)(((u8)(parameter - 4) >> 2) & 63);
        switch (selector) {
        case 0:
        case 1:
            log_print(2, NULL, selector == 0
                ? "[Info]: [LE-DUT]set LE_1M_PHY\n\r\n"
                : "[Info]: [LE-DUT]set LE_2M_PHY\n\r\n");
            struct ble_hw *ble = dut_hdl->ble4_dut;
            struct ble4_hw *hw = ble->ble4_hw;
            __dut_set_data_tx_phy(ble, selector);
            if (hw) __dut_set_data_rx_phy(hw, selector);
            ble->phy.rx_phy = selector;
            break;
        case 2:
        case 3:
            log_print(2, NULL, selector == 2
                ? "[Info]: [LE-DUT]set LE_CODED_S8_PHY\n\r\n"
                : "[Info]: [LE-DUT]set LE_CODED_S2_PHY\n\r\n");
            struct ble_hw *ble_coded = dut_hdl->ble4_dut;
            struct ble4_hw *hw_coded = ble_coded->ble4_hw;
            __dut_set_data_tx_phy(ble_coded, 2);
            ble_coded->phy.coded_s = selector == 2 ? 0 : 1;
            __dut_set_data_tx_phy_coded_s(ble_coded);
            ble_coded = dut_hdl->ble4_dut;
            hw_coded = ble_coded->ble4_hw;
            if (hw_coded) __dut_set_data_rx_phy(hw_coded, 2);
            ble_coded->phy.rx_phy = 2;
            rx_phy_s = selector == 2 ? 8 : 2;
            break;
        }
        break;
    }
    case 3:
        log_print(2, NULL, "[Info]: [LE-DUT]standard index\n\r\n");
        break;
    }
}

static void dut_two_wire_le_process(int packet_type, const u8 *packet, int size)
{
    (void)packet_type;
    (void)size;
    u8 response[2] = { 0, 0 };
    u8 command = packet[0];
    u8 channel = command & 63;
    u8 data_type = packet[1] & 3;
    payload_length.length = (payload_length.length & 0xc0) | (packet[1] >> 2);

    switch (command >> 6) {
    case 0:
        two_wire_setup_process(command & 15, packet[1]);
        response[1] = 30;
        (void)hci_transport->send_packet(0, response, 2);
        break;
    case 1:
        rx_param.channel = channel;
        rx_param.coded_s = rx_phy_s;
        rx_param.length = payload_length.length;
        response[1] = 30;
        le_receive_test_handle(&rx_param.channel, 0);
        (void)hci_transport->send_packet(0, response, 2);
        break;
    case 2:
        tx_param.channel = channel;
        tx_param.length = payload_length.length;
        dut_hdl->ble4_dut->phy.coded_s = 1;
        tx_param.patten = data_type + (data_type == 3);
        le_transmitter_test_handle(&tx_param, 0);
        (void)hci_transport->send_packet(0, response, 2);
        break;
    case 3: {
        u16 packet_count = le_test_end_handle();
        response[1] = (u8)packet_count;
        response[0] = (u8)((packet_count >> 8) | 128);
        rx_phy_s = 0;
        payload_length.length = 0;
        put_buf(response, 2);
        (void)hci_transport->send_packet(0, response, 2);
        break;
    }
    }
}

void dut_hci_controller_init(const hci_transport_t *transport, const void *config)
{
    log_print(2, NULL, "[Info]: [LE-DUT]\n--func=%s\n\r\n",
              "dut_hci_controller_init");
    hci_transport = transport;
    if (transport->init) transport->init(config);
    (void)transport->open();
    if (config_dut_protocol_mode == 0)
        transport->register_packet_handler(dut_hci_le_process);
    else
        transport->register_packet_handler(dut_two_wire_le_process);
}

void dut_le_hw_open(void)
{
    log_print(2, NULL, "[Info]: [LE-DUT]\n--func=%s\n\r\n", "dut_le_hw_open");
    btctler_nv_memory_apply();
    dut_le_hw_init();
    dut_hdl->ble4_dut = dut_ble4_hw_alloc();
    dut_le_set_mac(0, le_mac_addr);
    log_print(2, NULL, "[Info]: [LE-DUT]dut_le_hw_open succ\n\r\n");
}

static void *ble_dut_mode_init(void)
{
    dut_le_hw_init();
    if (dut_hdl) {
        dut_hdl->ble4_dut = dut_ble4_hw_alloc();
        dut_le_set_mac(0, le_mac_addr);
    }
    return dut_hdl;
}

static int ble_dut_mode_ioctrl(int ctrl, ...)
{
    va_list args;
    va_start(args, ctrl);
    if (!dut_hdl || !dut_hdl->ble4_dut) {
        log_print(2, NULL, "[Info]: [LE-DUT]DUT MODE NOT INITED\n\r\n");
        va_end(args);
        return -1;
    }
    switch (ctrl) {
    case 0: le_enhanced_receiver_test_handle(va_arg(args, struct dut_rx_param *), 0); break;
    case 1: le_enhanced_transmitter_test_handle(va_arg(args, struct dut_tx_param *), 0); break;
    case 2: {
        u16 *packet_count = va_arg(args, u16 *);
        u16 *error_count = va_arg(args, u16 *);
        if (packet_count) *packet_count = dut_hdl->packet_cnt;
        if (error_count) *error_count = dut_hdl->pkt_err_cnt;
        le_test_end();
        if (dut_get_fcc_mode_flag()) bt_pll_reset(1);
        if (dut_get_dual_mode_flag()) {
            dut_bredr_init_recall();
            log_print(2, NULL, "[Info]: [LE-DUT]edr init\n\r\n");
        }
        break;
    }
    }
    va_end(args);
    return 0;
}

static void ble_dut_mode_exit(void *priv __attribute__((unused)))
{
    if (dut_hdl) {
        struct dut_ble_baseband_layer_handle *handle = dut_hdl;
        if (handle->ble4_dut) {
            __set_anchor_cnt_disable();
            while (DUT_REG32(1867832) & 2) { }
            handle = dut_hdl;
        }
        DUT_REG32(1867816) &= (u32)-2;
        DUT_REG32(1867816) &= (u32)-257;
        free(handle);
        dut_hdl = NULL;
    } else log_print(4, NULL, "<Error>: [LE-DUT]no dut module to exit\n\r\n");
    DUT_REG32(1867776) = 0;
}

static const struct ble_dut_ops_t ble_dut_ops = {
    .init = ble_dut_mode_init,
    .ioctrl = ble_dut_mode_ioctrl,
    .exit = ble_dut_mode_exit,
};

#ifdef UNITTEST
int config_btctler_modules;
int config_btctler_mode;
int config_asser;
int config_btctler_le_hw_nums;
int config_btctler_le_rx_nums;
int config_btctler_le_acl_packet_length;
int config_btctler_le_acl_total_nums;
int config_dut_protocol_mode;
struct common_rf_param rf;
static int completed_opcode;
static int completion_count;
static unsigned int malloc_count;
static int malloc_size;
static u8 test_regs[40];
static int frame_log_count;
static int irq_calls;
static u8 irq_indices[2], irq_priorities[2], irq_cpus[2];
static void (*irq_handlers[2])(void);
static u32 dma_rx_size, dma_tx_size;
static u8 *dma_rx_ptr, *dma_tx_ptr;
static unsigned int close_calls, close_log_order;
static unsigned int widen_calls;
static int widen_values[4];
static struct ble4_hw *widen_targets[4];
static struct dut_ble_baseband_layer_handle *test_handle;
static struct ble_hw *test_ble;
static struct ble4_hw *test_ble4;
static u8 test_rx_dma[512], test_tx_dma[512];
static union {
    max_align_t align;
    u8 bytes[8192];
} test_arena __attribute__((aligned(65536)));

void log_print(int level, const char *file, const char *format, ...)
{
    (void)level; (void)file; (void)format;
    if (strstr(format, "RxMaxBuf")) {
        ++frame_log_count;
        CHECK(test_ble4->ble_fp.RXMAXBUF == 255);
        CHECK(test_ble4->ble_fp.IFSCNT == (u16)-32107);
        CHECK(test_ble4->ble_fp.FILTERCNTL == 0);
    }
    if (strstr(format, "edr close")) close_log_order = close_calls + 1;
}

void *btctler_nv_memory_malloc(int size)
{
    ++malloc_count;
    malloc_size = size;
    return test_regs;
}

void hw_dma_init(void *rx, u32 rx_size, void *tx, u32 tx_size)
{
    dma_rx_ptr = rx; dma_rx_size = rx_size;
    dma_tx_ptr = tx; dma_tx_size = tx_size;
}
void hw_dma_destory(void) { }
void put_buf(void *data, int len) { (void)data; (void)len; }
void __set_widen(struct ble4_hw *hw, int widen)
{
    if (widen_calls < 4) {
        widen_targets[widen_calls] = hw;
        widen_values[widen_calls] = widen;
    }
    ++widen_calls;
}
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id)
{
    if (irq_calls < 2) {
        irq_indices[irq_calls] = index;
        irq_priorities[irq_calls] = priority;
        irq_handlers[irq_calls] = handler;
        irq_cpus[irq_calls] = cpu_id;
    }
    ++irq_calls;
}
char ble_agc_normal_set(void *fp, char sel, char inc)
{
    CHECK(fp == &test_ble4->ble_fp); CHECK(sel == 0); CHECK(inc == 25);
    return 0;
}
char ble_txpwr_normal_set(void *fp, char sel, char inc)
{
    CHECK(fp == &test_ble4->ble_fp); CHECK(sel == 0);
    (void)inc;
    return 0;
}
u8 bt_get_pwr_max_level(void) { return 10; }
s16 rf_osc_offset_initial_val_get(void) { return 0; }
void bsb_normal_setting(void) { }
void p33_soft_reset(void) { }
void close_zeroif(void) { }
void bt_reload_trim(void) { }
void bt_set_single_carrier(void) { }
void bt_pll_reset(u8 mode) { (void)mode; }
void certification(u8 level) { (void)level; }
void bqb_2m_setting(void) { }
void dut_bredr_close_recall(void) { ++close_calls; }
void dut_bredr_init_recall(void) { }

int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    CHECK(format && strcmp(format, "1") == 0);
    completed_opcode = opcode;
    ++completion_count;
    return 0;
}

static void reset_arena(void)
{
    memset(test_arena.bytes, 0, sizeof(test_arena.bytes));
    memset((void *)dut_test_mmio, 0, sizeof(dut_test_mmio));
    memset(test_anchor_con, 0, sizeof(test_anchor_con));
    memset(test_rx_dma, 0, sizeof(test_rx_dma));
    memset(test_tx_dma, 0, sizeof(test_tx_dma));
    widen_calls = 0;
    test_handle = (struct dut_ble_baseband_layer_handle *)test_arena.bytes;
    test_ble = (struct ble_hw *)(test_arena.bytes + 1024);
    struct _hw_entity *entity = (struct _hw_entity *)(test_arena.bytes + 2048);
    test_ble4 = &entity->u.ble4_hw;
    test_handle->hw_base.hw = test_ble;
    test_handle->hw_base.hw_entity = entity;
    test_handle->hw_base.rx_dma = test_rx_dma;
    test_handle->hw_base.tx_dma = test_tx_dma;
    test_handle->ble4_dut = test_ble;
    test_ble->ble4_hw = test_ble4;
    test_ble4->regs = (u32 *)0x1234;
    test_handle->set_le_pwr_level = 7;
    dut_hdl = test_handle;
    config_btctler_le_hw_nums = 1;
    config_btctler_modules = 0;
    config_btctler_mode = 0;
    config_asser = 1;
    test_interval = 0;
    test_interval_written = 0;
    test_anchor_written = 0;
}

static void test_opcode_and_power(void)
{
    struct dut_ble_baseband_layer_handle handle = { 0 };
    const u8 level0[] = { 0xa5 };
    const u8 level1[] = { 0xff };
    dut_hdl = &handle;
    config_btctler_mode = 0;

    /* The IR masks to ten opcode bits and stores the byte for opcode 0x085. */
    CHECK(ogf_vendor_le_controller(0x085, level0, 1) == 1);
    CHECK(handle.set_le_pwr_level == 0xa5);
    CHECK(completion_count == 1 && completed_opcode == 0x085);
    CHECK(ogf_vendor_le_controller(0x485, level1, 1) == 1);
    CHECK(handle.set_le_pwr_level == 0xff);
    CHECK(completion_count == 2 && completed_opcode == 0x485);
    CHECK(ogf_vendor_le_controller(0x084, NULL, 0) == 0);
    CHECK(completion_count == 2 && handle.set_le_pwr_level == 0xff);
    dut_hdl = NULL;
}

static void test_hw_init_and_allocation(void)
{
    free(dut_hdl);
    dut_hdl = NULL;
    config_btctler_le_hw_nums = 1;
    config_btctler_le_rx_nums = 2;
    config_btctler_le_acl_packet_length = 251;
    config_btctler_le_acl_total_nums = 3;
    irq_calls = 0;
    dut_le_hw_init();
    CHECK(dut_hdl != NULL);
    CHECK(DUT_REG32(1899652) == (u32)(uintptr_t)dut_hdl);
    CHECK(DUT_REG32(1899712) == (u32)(uintptr_t)((u8 *)dut_hdl + 32));
    CHECK(DUT_REG32(1867776) == 1 && DUT_REG32(1867828) == 2305);
    CHECK(dma_rx_size == 0 && dma_tx_size == 0);
    CHECK(dma_rx_ptr == NULL && dma_tx_ptr == NULL);
    CHECK(irq_calls == 2 && irq_indices[0] == 39 && irq_indices[1] == 38);
    CHECK(irq_priorities[0] == 2 && irq_priorities[1] == 2);
    CHECK(irq_cpus[0] == 0 && irq_cpus[1] == 0);
    CHECK(irq_handlers[0] == ble_event_irq_handler && irq_handlers[1] == ble_rx_irq_handler);
    free(dut_hdl);
    dut_hdl = NULL;

    reset_arena();
    test_ble->ble4_hw = NULL;
    malloc_count = 0;
    struct ble_hw *allocated = dut_ble4_hw_alloc();
    CHECK(allocated == test_ble);
    CHECK(test_ble->ble4_hw == test_ble4 && ((struct _hw_entity *)test_ble4)->used == 1);
    CHECK(test_ble4->regs == (u32 *)test_regs && malloc_count == 1 && malloc_size == 40);
    CHECK(test_handle->number == 1);
    CHECK(test_handle->hw_base.inst[0] == 0x8800);
}

static void test_channels(void)
{
    reset_arena();
    u8 all[5] = { 0xff, 0xff, 0xff, 0xff, 0x1f };
    __dut_set_channel_map(test_ble4, all, 0);
    CHECK(test_ble4->ble_fp.CHMAP0 == 0xffff);
    CHECK(test_ble4->ble_fp.CHMAP1 == 0xffff);
    CHECK(test_ble4->ble_fp.CHMAP2 == (u16)(31 | (37 << 5)));
    for (u8 i = 0; i < 37; ++i) {
        u8 expected = (u8)((i + 1) * 2 + (i >= 11 ? 2 : 0));
        CHECK(test_ble4->ble_fp.FRQ_IDX0[i] == i);
        CHECK(test_ble4->ble_fp.FRQ_IDX1[i] == i);
        CHECK(test_ble4->ble_fp.FRQ_TBL0[i] == expected);
        CHECK(test_ble4->ble_fp.FRQ_TBL1[i] == expected);
    }
    CHECK(test_ble4->ble_fp.FRQ_IDX0[37] == 37 && test_ble4->ble_fp.FRQ_IDX0[39] == 39);
    CHECK(test_ble4->ble_fp.FRQ_TBL0[37] == 0 && test_ble4->ble_fp.FRQ_TBL0[38] == 0x18);
    CHECK(test_ble4->ble_fp.FRQ_TBL0[39] == 0x4e);
    u8 sparse[5] = { 1, 0, 0, 0, 0x10 };
    __dut_set_channel_map(test_ble4, sparse, 1);
    CHECK(test_ble4->ble_fp.FRQ_IDX1[0] == 0);
    CHECK(test_ble4->ble_fp.FRQ_IDX1[1] == 36);
    CHECK(test_ble4->ble_fp.FRQ_TBL1[0] == 4);
    CHECK(test_ble4->ble_fp.FRQ_TBL1[1] == 78);
    CHECK(test_ble4->ble_fp.CHMAP2 == (u16)(16 | (2 << 5)));
    static const u8 channels[] = { 0, 1, 11, 12, 13, 38, 39, 255 };
    static const u32 mapped[] = { 37, 0, 10, 38, 11, 36, 39, 255 };
    for (u32 i = 0; i < sizeof(channels); ++i) {
        __set_fixed_channel_index(channels[i]);
        CHECK(DUT_REG32(1867808) == (mapped[i] | 8192));
        CHECK(DUT_REG32(1867804) == 6149);
    }
}

static void test_frame_and_phy_boundaries(void)
{
    reset_arena();
    frame_log_count = 0;
    le_test_frame_init(9);
    CHECK(frame_log_count == 1);
    CHECK(test_ble4->ble_fp.CHMAP2 == (u16)(31 | (37 << 5)));

    /* IR interval formula: (8*length + 80) crosses at 377, 1001, 1625. */
    static const u8 lengths[] = { 37, 38, 115, 116, 193, 194 };
    static const u32 intervals[] = { 625, 1250, 1250, 1875, 1875, 2500 };
    for (u32 i = 0; i < sizeof(lengths); ++i) {
        struct dut_tx_param packet = { 0, lengths[i], 5, 0 };
        le_transmitter_test(&packet);
        CHECK(test_ble4->ble_fp.TXAHDR0 == 5);
        CHECK(test_ble4->ble_fp.TXDHDR0 >> 8 == lengths[i]);
        CHECK(test_interval_written == intervals[i]);
    }

    u8 rx_phy[] = { 0, 2, 20, 0 };
    le_enhanced_receiver_test_handle((struct dut_rx_param *)rx_phy, sizeof(rx_phy));
    CHECK(test_ble4->ble_fp.INTFRAME & 0x0800);
    struct dut_rx_param rx = { 0, 1, 20, 0 };
    le_receive_test(&rx);
    CHECK(test_ble4->ble_fp.WINCNTL0 == 3000 && test_ble4->ble_fp.WINCNTL2 == 150);
    rx.phy = 2; rx.length = 0; rx.coded_s = 255;
    test_ble->phy.rx_phy = 2;
    le_receive_test(&rx);
    CHECK(test_ble4->ble_fp.WINCNTL0 == 13750 && test_ble4->ble_fp.WIDEN0 == 1);
    rx.length = 0; rx.coded_s = 1;
    le_receive_test(&rx);
    CHECK(test_ble4->ble_fp.WINCNTL0 == 12500);
    rx.length = 1; rx.coded_s = 125;
    le_receive_test(&rx);
    CHECK(test_ble4->ble_fp.WINCNTL0 == 15000);
    CHECK(test_interval_written == 5000);
    rx.length = 255; rx.coded_s = 1;
    le_receive_test(&rx);
    CHECK(test_ble4->ble_fp.WINCNTL0 == 13125);
    CHECK(test_interval_written == 3125);
}

static void test_two_wire_setup(void)
{
    reset_arena();
    payload_length.length = 0x2a;
    two_wire_setup_process(1, 0x0c);
    CHECK(payload_length.length == 0xea);

    test_ble4->ble_fp.INTFRAME = 0xffff;
    two_wire_setup_process(2, 8);
    CHECK(test_ble->phy.rx_phy == 1);
    CHECK((test_ble4->ble_fp.INTFRAME & 0x0700) == 0x0100);
    CHECK((test_ble4->ble_fp.INTFRAME & 0x3800) == 0x0800);

    two_wire_setup_process(2, 12);
    CHECK(test_ble->phy.rx_phy == 2 && test_ble->phy.coded_s == 0);
    CHECK(rx_phy_s == 8);
    two_wire_setup_process(2, 16);
    CHECK(test_ble->phy.rx_phy == 2 && test_ble->phy.coded_s == 1);
    CHECK(rx_phy_s == 2);
}

static void test_prestart_and_callback(void)
{
    reset_arena();
    test_interval = 2;
    test_anchor_con[7] = 0;
    DUT_REG32(1867788) = 0;
    DUT_REG32(1867796) = 634;
    prestart_cnt_adjust(test_ble);
    CHECK(test_anchor_written == 0);

    /* A high-word anchor plus a negative delta wraps as raw i32 IR arithmetic. */
    test_interval = 0;
    test_anchor_con[7] = 0x80000200;
    DUT_REG32(1867788) = 1023;
    DUT_REG32(1867796) = 255;
    prestart_cnt_adjust(test_ble);
    CHECK(test_anchor_written == 0);

    /* A -9 delta plus the SDK's masked-u16 add reaches the 610/625 boundary. */
    test_anchor_con[7] = 0;
    DUT_REG32(1867788) = 600;
    DUT_REG32(1867796) = 34;
    prestart_cnt_adjust(test_ble);
    CHECK(test_anchor_written == (u32)-15);
    CHECK(DUT_REG32(1867808) == (u32)-15 && DUT_REG32(1867804) == ((7u << 10) | 5));

    test_handle->packet_cnt = 19;
    test_handle->pkt_err_cnt = 4;
    test_handle->le_test_enable = 1;
    DUT_REG32(1867816) = 0x101;
    u16 packet_count = 0, error_count = 0;
    CHECK(__ble_dut_ops->ioctrl(2, &packet_count, &error_count) == 0);
    CHECK(packet_count == 19 && error_count == 4);
    CHECK(test_handle->packet_cnt == 0 && test_handle->pkt_err_cnt == 0);
    CHECK(test_handle->le_test_enable == 0 && DUT_REG32(1867816) == 0);

    /* Enhanced handlers log before invoking the classic close callback. */
    config_btctler_modules = 1;
    close_calls = 0; close_log_order = 0;
    widen_calls = 0;
    u8 enhanced_rx[] = { 0, 1, 20, 0 };
    le_enhanced_receiver_test_handle((struct dut_rx_param *)enhanced_rx, sizeof(enhanced_rx));
    CHECK(close_calls == 1 && close_log_order == 2);
    CHECK(test_ble->phy.rx_phy == 0 && ((test_ble4->ble_fp.INTFRAME >> 11) & 7) == 0);
    static const u8 expected_tx_phy[] = { 0, 1, 2, 2 };
    static const u8 expected_coded_s[] = { 0, 0, 0, 1 };
    for (u8 phy = 1; phy <= 4; ++phy) {
        u8 enhanced_tx[] = { 0, 20, 5, phy };
        le_enhanced_transmitter_test_handle((struct dut_tx_param *)enhanced_tx, sizeof(enhanced_tx));
        CHECK(((test_ble4->ble_fp.INTFRAME >> 8) & 7) == expected_tx_phy[phy - 1]);
        CHECK(test_ble->phy.coded_s == expected_coded_s[phy - 1]);
        CHECK(test_ble4->ble_fp.TXDHDR0 >> 8 == 20);
    }
    CHECK(close_calls == 5 && close_log_order == 5);
}

static void test_irq_dispatch(void)
{
    reset_arena();
    config_btctler_le_hw_nums = 1;
    test_interval = 2; /* Keep IRQ vectors away from anchor drift correction. */
    test_ble->phy.rx_phy = 0;
    test_ble4->ble_fp.RXTOG = 0;
    test_ble4->ble_fp.RXSTAT1 = 1;
    DUT_REG32(1867816) = 0x101;
    DUT_REG32(1867824) = 0x101;
    DUT_REG32(1867820) = 0;
    ble_event_irq_handler();
    CHECK(DUT_REG32(1867820) == 1);

    ble_rx_irq_handler();
    CHECK(DUT_REG32(1867820) == 0x101);
    CHECK(test_handle->packet_cnt == 1 && test_handle->pkt_err_cnt == 0);
    CHECK(rx_cnt == 1 && test_ble4->ble_fp.WINCNTL0 == 50);
    CHECK(widen_calls == 1 && widen_targets[0] == (struct ble4_hw *)(void *)test_ble &&
          widen_values[0] == 20);
    test_ble4->ble_fp.RXSTAT1 = 2;
    ble_rx_irq_handler();
    CHECK(test_handle->packet_cnt == 1 && test_handle->pkt_err_cnt == 1);
    CHECK(rx_cnt == 1 && test_ble4->ble_fp.WINCNTL0 == 50);

    rx_cnt = 0;
    test_ble->phy.rx_phy = 2;
    test_ble4->ble_fp.RXSTAT1 = 1;
    ble_rx_irq_handler();
    CHECK(test_handle->packet_cnt == 2 && test_handle->pkt_err_cnt == 1);
    CHECK(rx_cnt == 1 && test_ble4->ble_fp.WINCNTL0 == 1000);
    CHECK(widen_calls == 2 && widen_targets[1] == (struct ble4_hw *)(void *)test_ble &&
          widen_values[1] == 480);
}

static void test_exit(void)
{
    struct dut_ble_baseband_layer_handle *owned = calloc(1, sizeof(*owned));
    CHECK(owned != NULL);
    if (!owned) return;
    owned->ble4_dut = test_ble;
    dut_hdl = owned;
    DUT_REG32(1867816) = 0x101;
    __ble_dut_ops->exit(NULL);
    dut_hdl = NULL;
    CHECK(DUT_REG32(1867804) == 14341);
    CHECK(DUT_REG32(1867832) == 0);
    CHECK(DUT_REG32(1867816) == 0);
    CHECK(DUT_REG32(1867776) == 0);
}

int main(void)
{
    test_opcode_and_power();
    test_hw_init_and_allocation();
    test_channels();
    test_frame_and_phy_boundaries();
    test_two_wire_setup();
    test_prestart_and_callback();
    test_irq_dispatch();
    test_exit();
    return TEST_DONE("btctrler_direct_test_dut");
}
#endif
