/* BLE 5 baseband support. Keep the SDK's event ordering and ext-PDU quirks. */
#include "btctrler/glue.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define memcpy __builtin_memcpy
#define memset __builtin_memset

/* This SDK consumer declares an int-returning, BLE5-frame entry point. */
extern int ble5_read_rssi(struct ble5_param *) __asm__("ble_analog_get_rssi");

typedef u16 ble5_unaligned_u16 __attribute__((aligned(1), may_alias));
typedef struct list_head *ble5_unaligned_link __attribute__((aligned(1), may_alias));

#ifdef UNITTEST
#define BLE5_CODE
#else
#define BLE5_CODE __attribute__((section(".ble_rf_code")))
#endif

static void __set_channel_selection_algorithm(struct ble_hw *hw, u8 chsel_algorithm);
static u8 get_if_aux_enable(struct ble_hw *hw);
void __set_ext_aux_event_hold_ir(struct ble_hw *hw, u8 hold_en)
    __asm__("__set_ext_aux_event_hold");
static void le_hw_legacy_open(struct ble_hw *hw);
static void le_hw_legacy_close(struct ble_hw *hw);
static void *le_ext_hw_open(void);
static void le_ext_hw_close(struct ble_hw *hw);
static void le_hw_ext_ioctrl(struct ble_hw *hw, int ctrl, ...);
static void le_hw_ext_advertising_ctrl(struct ble_hw *hw, int ctrl, ...);
static void le_hw_ext_scanning(struct ble_hw *hw, struct ble_hw *legacy_hw,
                               struct ble_ext_scan *scan);
static void le_hw_ext_initiating(struct ble_hw *hw, struct ble_hw *primary_hw,
                                 struct ble_ext_init *init);
static void le_ext_hw_handler_register(struct ble_hw *hw, void *priv,
                                       const struct ble_ext_handler *handler);
static void le_ext_hw_upload_data(struct ble_hw *hw,
                                  void (*rx_handler)(void *, struct ble_ext_rx *));
static void ble_ext_primary_scan_report_produce(struct ble_hw *hw,
                                                const struct ble_rx *rx,
                                                u8 data_status);
static u8 get_if_last_packet(const struct ble_ext_rx *rx);
static void __ext_header_resolve(u8 flag, const u8 *head, u8 member,
                                 u8 *member_buf);
static void ext_rx_probe_handler(struct ble_hw *hw, struct ble_ext_rx *rx);
static u8 *hw_alloc_ext_rx(u32 len);
static void cpu_reset(void);
static void loop_cbuf_init(struct __loop_cbuf *cbuffer, u8 *buf, u32 size);
static u32 loop_cbuf_write(struct __loop_cbuf *cbuffer, const u8 *buf, u32 len);
static void loop_cbuf_read(struct __loop_cbuf *cbuffer, u8 *buf, u32 len);
static void __set_ext_adv_channel_hop(struct ble_hw *hw, u8 auxidx,
                                      u8 alg2idx, u8 auxhop);
static u8 __common_ext_adv_pdu_build(struct ble_hw *hw, struct ble_ext_adv *adv,
                                     u8 pdu_name, u8 advmode, u8 ext_flag);
static void __aux_chain_pdu(struct ble_hw *hw, struct ble_ext_adv *adv,
                            u8 superior_flag);
static void __aux_sync_pdu(struct ble_hw *hw,
                           const struct ble_ext_period_adv *sync_adv);
static u8 __get_ext_secondary_phy(struct ble_hw *hw);
static void __ext_hw_enable(struct ble_hw *hw, u8 enable, u32 slot);
static void loop_buffer_tx_handle(struct ble5_hw *hw);
static void __clear_ext_filter_mode(const struct ble_hw *hw);
static void ext_master_scan_data_truncated_handle(struct ble_hw *hw);
static void close_ble5(struct ble_hw *hw);
static void check_if_need_update_ext_adv_data(struct ble_hw *hw);
static void ext_aux_adv_process_reset(struct ble_hw *hw, u8 reset_ptr);
static void ble5_hw_frame_free(struct ble_hw *hw);
static void le_hw_ext_primary_adv_switch(struct ble_hw *hw, u8 enable);
static void free_ext_tx_loop_buffer_list(struct ble5_hw *hw,
                                         const struct list_head *free_head);
static void __ext_adv_tx_loop_buffer_alloc_total(struct ble5_hw *hw);
static void __ext_tx_loop_buffer_hw_build(struct ble5_hw *hw);
static void __ext_adv_tx_loop_buffer_data_build(struct ble5_hw *hw);
static void le_hw_ext_secondary_adv_switch(struct ble_hw *hw, u8 enable);
static void le_hw_ext_advertising_switch(struct ble_hw *hw, u8 enable, u32 slot);
static void __ext_adv_hw_tx_ptr_reset(struct ble5_hw *hw);
static void __ext_kickstart_next_hw(struct ble5_hw *hw, u8 next_hw_id, u8 enable);

extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern const u8 log_tag_const_e_LE5_BB;
extern const u8 log_tag_const_i_LE5_BB;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);

static struct __aux_filter aux_filter __attribute__((section(".ble_rf_bss")));
static u8 aux_data_channel __attribute__((section(".ble_rf_data"))) = 0xff;

static const u8 ext_header_table[6][2]
    __attribute__((section(".ble_rf_const"))) = {
        {0x01, 0x06}, {0x02, 0x06}, {0x08, 0x02},
        {0x10, 0x03}, {0x20, 0x12}, {0x40, 0x01},
    };

static const struct ble5_operation ble5_ops __attribute__((section(".ble_rf_const"))) = {
    (void (*)(void *))le_hw_legacy_open,
    (void (*)(void *))le_hw_legacy_close,
    le_ext_hw_open,
    (void (*)(void *))le_ext_hw_close,
    (void (*)(void *, int, ...))le_hw_ext_ioctrl,
    (void (*)(void *, int, ...))le_hw_ext_advertising_ctrl,
    (void (*)(void *, void *, struct ble_ext_scan *))le_hw_ext_scanning,
    (void (*)(void *, void *, struct ble_ext_init *))le_hw_ext_initiating,
    (void (*)(void *, void *, const struct ble_ext_handler *))le_ext_hw_handler_register,
    (void (*)(void *, void (*)(void *, struct ble_ext_rx *)))le_ext_hw_upload_data,
};

const struct ble5_operation *__ble5_ops __attribute__((section(".ble_rf_data"))) = &ble5_ops;


u8 get_if_exist_ble5_ir(struct ble_hw *hw)
    __asm__("get_if_exist_ble5") ;
BLE5_CODE u8 get_if_exist_ble5_ir(struct ble_hw *hw) {
    return hw->ble5_hw != NULL;
}

BLE5_CODE u32 __hw5_free_slots(struct ble_hw *hw) {
    if (!(config_btctler_le_features & (1ULL << 12)))
        return -1;
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5)
        return -1;
    if (*(volatile u32 *)0x1c8038 & 2) {
        return ble5->hw_state == 7 ? 11 : 0;
    }
    if (check_event_pending()) {
        ble5 = hw->ble5_hw;
        return ble5->hw_state == 7 ? 11 : 0;
    }
    ble5 = hw->ble5_hw;
    switch (ble5->hw_state) {
    case 1: case 3: case 5: case 7:
        return -1;
    default:
        break;
    }
    u32 slot = (u32)-1;
    if ((s16)__get_ble_anchor_con(hw, 14) < 0) {
        u32 low = __get_ble_anchor_con(hw, 0);
        u32 high = __get_ble_anchor_con(hw, 14);
        slot = (((high << 16) & 0x00ff0000U) | (low & 0xffffU)) + 1;
    }
    struct ble_hw *aux_hw = hw->ble5_ext->correlated_hw;
    u32 aux_slot = (u32)-1;
    if (aux_hw && get_if_aux_enable(aux_hw))
        aux_slot = __get_ble_anchor_con(aux_hw, 11) & 4095;
    u32 nearest = slot < aux_slot ? slot : aux_slot;
    return nearest < 4 ? 0 : nearest - 4;
}

BLE5_CODE static inline __attribute__((always_inline)) u8 get_if_aux_enable(struct ble_hw *hw) {
    return (u16)__get_ble_anchor_con(hw, 11) >> 15;
}

BLE5_CODE void record_rx_chsel_algorithm(struct ble_rx *rx, u8 chsel_algorithm) {
    if (config_btctler_le_features & (1ULL << 14))
        rx->chsel_algorithm = chsel_algorithm;
}

BLE5_CODE void set_hw_channel_selection_algorithm(struct ble_hw *hw, struct ble_rx *rx) {
    if (config_btctler_le_features & (1ULL << 14)) {
        rx->chsel_algorithm &= 1;
        __set_channel_selection_algorithm(hw, rx->chsel_algorithm);
    }
}

BLE5_CODE static void __set_channel_selection_algorithm(struct ble_hw *hw, u8 chsel_algorithm) {
    u32 value = __get_ble_anchor_con(hw, 10);
    __set_ble_anchor_con(hw, 10, (value & ~64U) | ((chsel_algorithm << 6) & 64));
}

BLE5_CODE void set_tx_pdu_channel_selection_algorithm(struct ble_tx *tx) {
    if (config_btctler_le_features & (1ULL << 14))
        tx->chsel = 1;
}

BLE5_CODE void __set_extended_tx_data(struct ble_hw *hw, u8 chsel, int index) {
    u64 features = config_btctler_le_features;
    if (features & (1ULL << 11)) {
        struct ble4_hw *hw4 = hw->ble4_hw;
        volatile u16 *txdhdr = index == 0 ? &hw4->ble_fp.TXDHDR0 : &hw4->ble_fp.TXDHDR1;
        u16 header = *txdhdr;
        *txdhdr = (hw->phy.coded_s << 4 & 16) | (header & ~16U);
    }
    if (features & (1ULL << 14)) {
        struct ble4_hw *hw4 = hw->ble4_hw;
        volatile u16 *txahdr = index == 0 ? &hw4->ble_fp.TXAHDR0 : &hw4->ble_fp.TXAHDR1;
        *txahdr = (*txahdr & ~512U) | ((chsel & 1) << 9);
    }
}

BLE5_CODE void __set_extended_empty_tx_data(struct ble_hw *hw) {
    if (config_btctler_le_features & (1ULL << 11)) {
        volatile u16 *intframe = &hw->ble4_hw->ble_fp.INTFRAME;
        *intframe = (hw->phy.coded_s << 14) | (*intframe & 16383U);
    }
}

BLE5_CODE void __set_extended_conn_dead_times(struct ble_hw *hw, u8 tx_phy) {
    u64 features = config_btctler_le_features;
    if (!(features & ((1ULL << 8) | (1ULL << 11))))
        return;
    if (tx_phy == 1) {
        if (features & (1ULL << 8)) {
            volatile u16 *anchor = &hw->ble4_hw->ble_fp.ANCHOR;
            *anchor = (*anchor & 0x8000) | 3;
        }
    } else if (tx_phy == 2 && (features & (1ULL << 11))) {
        int uncoded = hw->phy.coded_s == 0;
        volatile u16 *anchor = &hw->ble4_hw->ble_fp.ANCHOR;
        u16 saved = *anchor;
        *anchor = (u16)(((hw->ble4_hw->tx_octets << (uncoded ? 6 : 4)) +
                         (uncoded ? 1516 : 1354)) / 625) | (saved & 0x8000);
    }
}

BLE5_CODE void __master_reset_transmitWindowDelay(struct ble_hw *hw, u16 winoffset) {
    u64 features = config_btctler_le_features;
    if (!(features & (1ULL << 12)) || !hw->ble5_hw)
        return;
    switch (hw->phy.tx_phy) {
    case 1:
        if (!(features & (1ULL << 8))) return;
        /* fall through */
    case 0:
        __set_anchor_cnt(hw, (winoffset << 1) + 6);
        return;
    case 2:
        if (features & (1ULL << 11))
            __set_anchor_cnt(hw, (hw->phy.coded_s == 0 ? 11 : 8) + (winoffset << 1));
        return;
    default:
        return;
    }
}

BLE5_CODE void __slave_reset_widen(struct ble_hw *hw, u16 *widening) {
    hw->phy.widen = *widening;
    u64 features = config_btctler_le_features;
    if ((features & (1ULL << 12)) && (features & (1ULL << 11)) &&
        hw->ble5_hw && hw->phy.tx_phy == 2)
        __set_widen(hw->ble4_hw, *widening + 476);
}

BLE5_CODE void __reset_connection_winsize(struct ble_hw *hw, u8 winsize) {
    if ((config_btctler_le_features & (1ULL << 12)) && hw->ble5_hw)
        __set_winsize(hw->ble4_hw, 520, winsize * 1250 + 775);
}

BLE5_CODE void __set_ble_hw_timeout_irq_init(struct ble_hw *hw) {
    u32 value = __get_ble_anchor_con(hw, 10);
    __set_ble_anchor_con(hw, 10, value | 768);
    u32 reg = *(volatile u32 *)0x1c804c;
    u8 id = HW_ID(hw);
    u32 mask = (u32)256 << id;
    id = HW_ID(hw);
    u32 set = (u32)256 << id;
    *(volatile u32 *)0x1c804c = (reg & ~mask) | set;
    reg = *(volatile u32 *)0x1c804c;
    id = HW_ID(hw);
    mask = (u32)1 << id;
    id = HW_ID(hw);
    set = (u32)1 << id;
    *(volatile u32 *)0x1c804c = (reg & ~mask) | set;
}

BLE5_CODE void __set_ext_aux_event_hold_ir(struct ble_hw *hw, u8 hold_en) {
    u32 value = __get_ble_anchor_con(hw, 2);
    __set_ble_anchor_con(hw, 2, (value & ~32768U) | ((hold_en & 1) << 15));
}


BLE5_CODE void __set_ext_mode_enable(struct ble_hw *hw, u8 ext_mode_en) {
    u32 value = __get_ble_anchor_con(hw, 10);
    __set_ble_anchor_con(hw, 10, (value & ~1U) | (ext_mode_en & 1));
}

BLE5_CODE static inline __attribute__((always_inline)) void free_ble5_ext(struct ble_hw *hw) {
    btctler_nv_memory_free(hw->ble5_ext);
    hw->ble5_ext = NULL;
}

BLE5_CODE static void __set_ext_period_channel_map(const struct ble_hw *hw, u8 *chm) {
    u32 used = 0;
    for (u32 i = 0; i != 37; ++i)
        used += (chm[i >> 3] & (1 << (i & 7))) != 0;
    u16 value = btctler_little_endian_read_16(chm, 0);
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_CHMAP0 = value;
    value = btctler_little_endian_read_16(chm, 2);
    ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_CHMAP1 = value;
    ble5->ble5_fp.EXT_CHMAP2 = (chm[4] & 31) | (used << 5);
}

BLE5_CODE static void __set_ext_period_adv_aa_crcinit(const struct ble_hw *hw,
                                            u8 *aa, u8 *crcinit) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_BDADDR0 = btctler_little_endian_read_16(aa, 0);
    ble5->ble5_fp.EXT_BDADDR1 = btctler_little_endian_read_16(aa, 2);
    ble5->ble5_fp.EXT_CRCWORD0 = btctler_little_endian_read_16(crcinit, 0);
    ble5->ble5_fp.EXT_CRCWORD1 = crcinit[2];
}

BLE5_CODE static void __set_anchor_cnt_disable_with_default(struct ble_hw *hw) {
    __set_ble_anchor_con(hw, 0, 65534);
    __set_ble_anchor_con(hw, 14, 4095);
}

BLE5_CODE static inline __attribute__((always_inline)) void __set_ext_adv_aa_crcinit(const struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_BDADDR0 = (u16)-16682;
    ble5->ble5_fp.EXT_BDADDR1 = (u16)-29047;
    ble5->ble5_fp.EXT_CRCWORD0 = 21845;
    ble5->ble5_fp.EXT_CRCWORD1 = 85;
}

BLE5_CODE static inline __attribute__((always_inline)) void __set_data_tx_phy(const struct ble_hw *hw, u8 phy) {
    struct ble4_hw *ble4 = hw->ble4_hw;
    if (ble4)
        ble4->ble_fp.INTFRAME = (ble4->ble_fp.INTFRAME & ~1792U) | ((phy & 7) << 8);
}

BLE5_CODE static inline __attribute__((always_inline)) void __set_data_rx_phy(const struct ble_hw *hw, u8 phy) {
    struct ble4_hw *ble4 = hw->ble4_hw;
    if (ble4)
        ble4->ble_fp.INTFRAME = (ble4->ble_fp.INTFRAME & ~14336U) | ((phy & 7) << 11);
}

BLE5_CODE static void __set_data_tx_phy_coded_s(const struct ble_hw *hw) {
    struct ble4_hw *ble4 = hw->ble4_hw;
    if (!ble4)
        return;
    u16 intframe = ble4->ble_fp.INTFRAME;
    u8 coded_s = hw->phy.coded_s;
    ble4->ble_fp.INTFRAME = (coded_s << 14) | (intframe & 16383U);
    ble4->ble_fp.TXDHDR0 = ((coded_s << 4) & 16) | (ble4->ble_fp.TXDHDR0 & ~16U);
    ble4->ble_fp.TXDHDR1 = ((coded_s << 4) & 16) | (ble4->ble_fp.TXDHDR1 & ~16U);
}

BLE5_CODE static void __set_ble4_ifs_phy_compens(const struct ble_hw *hw) {
    struct ble4_hw *ble4 = hw->ble4_hw;
    if (!ble4)
        return;
    ble4->ble_fp.EXTIFSDRF0 = (ble4->ble_fp.EXTIFSDRF0 & ~127U) | 20;
    ble4->ble_fp.EXTIFSDRF0 = (ble4->ble_fp.EXTIFSDRF0 & ~16256U) | 2560;
    ble4->ble_fp.EXTIFSDRF1 |= 127;
    ble4->ble_fp.EXTIFSDRF1 |= 1920;
    if (hw->phy.coded_s == 1) {
        ble4->ble_fp.EXTIFSDRF1 &= ~30720U;
    } else if (hw->phy.coded_s == 0) {
        ble4->ble_fp.EXTIFSDRF0 |= (u16)-32768;
        ble4->ble_fp.EXTIFSDRF1 = (ble4->ble_fp.EXTIFSDRF1 & ~30720U) | 14336;
    }
}

BLE5_CODE static inline __attribute__((always_inline)) u8 exchange_AirPhyParam_2_HardwarePhyParam(u8 air_phy_param) {
    switch (air_phy_param) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default: return 4;
    }
}

BLE5_CODE static void __set_ext_adv_channel_map(const struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    for (u32 i = 0; i != 40; ++i) {
        ble5->ble5_fp.EXT_FRQ_IDX0[i] = i;
        ble5->ble5_fp.EXT_FRQ_IDX1[i] = i;
    }
    for (u32 i = 0; i != 11; ++i) {
        u8 value = (i + 1) << 1;
        ble5->ble5_fp.EXT_FRQ_TB0[i] = value;
        ble5->ble5_fp.EXT_FRQ_TB1[i] = value;
    }
    for (u32 i = 11; i != 37; ++i) {
        u8 value = ((i + 1) << 1) + 2;
        ble5->ble5_fp.EXT_FRQ_TB0[i] = value;
        ble5->ble5_fp.EXT_FRQ_TB1[i] = value;
    }
    ble5->ble5_fp.EXT_FRQ_TB0[37] = 0;
    ble5->ble5_fp.EXT_FRQ_TB0[38] = 24;
    ble5->ble5_fp.EXT_FRQ_TB0[39] = 78;
}

BLE5_CODE static void __le_ext_filter_init(const struct ble_hw *hw, u8 local_filter_en) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_AUXCTL0 &= ~128U;
    ble5->ble5_fp.EXT_AUXCTL0 = (ble5->ble5_fp.EXT_AUXCTL0 & ~256U) |
                                ((local_filter_en & 1) << 8);
    ble5->ble5_fp.EXT_AUXCTL0 &= ~512U;
    ble5->ble5_fp.EXT_AUXCTL0 &= ~1024U;
    ble5->ble5_fp.EXT_AUXCTL0 &= ~2048U;
    __clear_ext_filter_mode(hw);
    ble5->ble5_fp.EXT_AUXCTL1 &= ~8U;
    ble5->ble5_fp.EXT_AUXCTL1 &= ~32U;
    ble5->ble5_fp.EXT_WLISTL = 0;
    ble5->ble5_fp.EXT_WLISTM = 0;
    ble5->ble5_fp.EXT_WLISTU = 0;
}

BLE5_CODE static inline __attribute__((always_inline)) void __set_ext_adv_other_hw(const struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_ANCHOR = 0;
    ble5->ble5_fp.EXT_BITOFF = 0;
    ble5->ble5_fp.EXT_WIDEN0 = 0;
    ble5->ble5_fp.EXT_WIDEN1 = 0;
    ble5->ble5_fp.EXT_ANCHOR |= (u16)-32768;
    ble5->ble5_fp.EXT_WIDEN1 |= (u16)-4096;
    ble5->ble5_fp.EXT_AUXCTL0 |= 32;
}

BLE5_CODE static void __set_ext_hw_state(struct ble_hw *hw, int state) {
    hw->ble5_hw->hw_state = (u8)state;
    hw->ble5_hw->soft_state = 0;
    u32 value = __get_ble_anchor_con(hw, 10);
    __set_ble_anchor_con(hw, 10, (value & ~30U) | ((state << 1) & 30));
}

BLE5_CODE static void __set_ext_primary_or_period_adv_phy(struct ble_hw *hw, u8 phy) {
    u32 value = __get_ble_anchor_con(hw, 13);
    __set_ble_anchor_con(hw, 13, (value & ~7U) | (phy & 7));
}

BLE5_CODE static inline __attribute__((always_inline)) void __set_ble5_aux_tx_phy_compens(const struct ble_hw *hw,
                                          u8 source, u8 target) {
    if (source == 0 || source == 1)
        hw->ble5_hw->ble5_fp.EXT_PKTDRIFT = target == 2 ? (u16)-10 : 0;
    else if (source == 2)
        hw->ble5_hw->ble5_fp.EXT_PKTDRIFT = target < 2 ? 10 : 0;
}

BLE5_CODE static void __set_ble5_tx_phy_compens(const struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5)
        return;
    ble5->ble5_fp.EXT_PHYDRIFT0 &= (u16)-256;
    ble5->ble5_fp.EXT_PHYDRIFT0 = (ble5->ble5_fp.EXT_PHYDRIFT0 & 255) | 2048;
    ble5->ble5_fp.EXT_PHYDRIFT1 &= (u16)-256;
    ble5->ble5_fp.EXT_PKTDRIFT = 0;
}

BLE5_CODE static void __set_ble5_ifs_phy_compens(const struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5)
        return;
    ble5->ble5_fp.EXT_IFSDRF0 = (ble5->ble5_fp.EXT_IFSDRF0 & ~127U) | 20;
    ble5->ble5_fp.EXT_IFSDRF0 = (ble5->ble5_fp.EXT_IFSDRF0 & ~16256U) | 2560;
    ble5->ble5_fp.EXT_IFSDRF1 |= 127;
    ble5->ble5_fp.EXT_IFSDRF1 = (ble5->ble5_fp.EXT_IFSDRF1 & ~1920U) | 1152;
    ble5->ble5_fp.EXT_IFSDRF1 = (ble5->ble5_fp.EXT_IFSDRF1 & ~30720U) | 16384;
}

BLE5_CODE static void __set_ble4_rx_phy_compens(const struct ble_hw *hw) {
    struct ble4_hw *ble4 = hw->ble4_hw;
    if (!ble4)
        return;
    ble4->ble_fp.EXTPHYDRF0 = (ble4->ble_fp.EXTPHYDRF0 & ~255U) | 24;
    ble4->ble_fp.EXTPHYDRF0 = (ble4->ble_fp.EXTPHYDRF0 & 255) | 12800;
    ble4->ble_fp.EXTPHYDRF1 = (ble4->ble_fp.EXTPHYDRF1 & ~255U) | 180;
    ble4->ble_fp.EXTPKTDRF = 0;
}

BLE5_CODE static void set_connecting_data_phy(const struct ble_hw *hw) {
    __set_data_tx_phy(hw, hw->phy.tx_phy);
    __set_data_rx_phy(hw, hw->phy.rx_phy);
    __set_data_tx_phy_coded_s(hw);
    __set_ble4_ifs_phy_compens(hw);
}

BLE5_CODE void assign_aux_data_channel(u8 ch) {
    aux_data_channel = ch;
}

BLE5_CODE static void __ext_header_resolve(u8 flag, const u8 *head, u8 member,
                                 u8 *member_buf) {
    if (!(member & flag))
        return;
    for (u32 i = 0; i < 6; ++i) {
        u8 field = ext_header_table[i][0];
        if (field == member) {
            memcpy(member_buf, head, ext_header_table [i][1]);
            return;
        }
        if (field & flag)
            head += ext_header_table[i][1];
    }
}

BLE5_CODE static u8 *hw_alloc_ext_rx(u32 len) {
    u8 *rx = hw_dma_alloc(0, len);
    if (!rx)
        return NULL;
    if (((u32)rx & 3) != 0) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 295);
            printf("ASSERT-FAILD: ((u32)rx & 0x03) == 0 %s\n", "RF ext rx not align\n");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    return rx;
}

static void cpu_reset(void)
{
    p33_soft_reset();
}

BLE5_CODE static u32 loop_cbuf_write(struct __loop_cbuf *cbuffer, const u8 *buf, u32 len) {
    u32 total_len = cbuffer->total_len;
    if (!cbuffer) return 0;
    u8 *begin = cbuffer->begin;
    u32 free_len = total_len - cbuffer->can_read_len;
    u8 *write_ptr = cbuffer->write_ptr;
    u32 to_end = (u32)(begin + total_len - write_ptr);
    if (free_len < len) {
        if (free_len > to_end) {
            memset(write_ptr, 0, to_end);
            memset(cbuffer->begin, 0, free_len - to_end);
        } else {
            memset(write_ptr, 0, free_len);
        }
        return 0;
    }
    u32 second = len;
    if (to_end <= len) {
        second = len - to_end;
        if (buf) {
            memcpy(write_ptr, buf, to_end);
            memcpy(cbuffer->begin, buf + to_end, second);
        }
    } else if (buf) { memcpy(write_ptr, buf, len);
    }
    cbuffer->write_ptr = to_end <= len ? cbuffer->begin + second : cbuffer->write_ptr + len;
    cbuffer->can_read_len += len;
    return len;
}

BLE5_CODE static void loop_cbuf_read(struct __loop_cbuf *cbuffer, u8 *buf, u32 len) {
    if (!cbuffer)
        return;
    u8 *begin = cbuffer->begin;
    u8 *read_ptr = cbuffer->read_ptr;
    u32 to_end = (u32)(begin + cbuffer->total_len - read_ptr);
    if (cbuffer->can_read_len < len)
        return;
    u32 second = len;
    if (to_end <= len) {
        second = len - to_end;
        if (buf) {
            memcpy(buf, read_ptr, to_end);
            memcpy(buf + to_end, cbuffer->begin, second);
        }
    } else if (buf) {
        memcpy(buf, read_ptr, len);
    }
    cbuffer->read_ptr = to_end <= len ? cbuffer->begin + second : cbuffer->read_ptr + len;
    cbuffer->can_read_len -= len;
}

BLE5_CODE static inline __attribute__((always_inline)) void loop_cbuf_init(struct __loop_cbuf *cbuffer, u8 *buf, u32 size) {
    cbuffer->can_read_len = 0;
    cbuffer->begin = buf;
    cbuffer->read_ptr = buf;
    cbuffer->write_ptr = buf;
    cbuffer->total_len = size;
}

BLE5_CODE static u8 *hw_alloc_ext_tx(u32 len) {
    u8 *tx = hw_dma_alloc(1, len);
    if (!tx) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 307);
            printf("ASSERT-FAILD: tx != NULL %s\n", "RF ext tx alloc err\n");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    memset(tx, 0, len);
    return tx;
}

BLE5_CODE static void ble_ext_primary_scan_report_produce(struct ble_hw *hw,
                                                const struct ble_rx *rx,
                                                u8 data_status) {
    struct ble_ext_rx *ext_rx =
        (struct ble_ext_rx *)hw_alloc_ext_rx(rx->len + sizeof(*ext_rx));
    if (!ext_rx) {
        if (log_tag_const_e_LE5_BB)
            log_print(4, NULL, "<Error>: [LE5_BB]ble_ext_primary_scan_report_produce alloc NULL\r\n");
        return;
    }
    memset(ext_rx, 0, sizeof(*ext_rx));
    ext_rx->txadd = rx->txadd;
    ext_rx->rxadd = rx->rxadd;
    ext_rx->type = rx->type;
    ext_rx->len = rx->len;
    ext_rx->data_status = data_status;
    ext_rx->rssi = rx->rssi;
    memcpy(ext_rx->data, rx->data, rx->len);
    ext_rx_probe_handler(hw, ext_rx);
}

BLE5_CODE static void ext_rx_probe_handler(struct ble_hw *hw, struct ble_ext_rx *rx) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (ble5->hw_state == 7) {
        struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
        struct ble4_hw *hw4 = correlated->ble4_hw;
        if (!hw4) {
            hw_dma_free(rx);
            return;
        }
        rx->primary_phy = ((hw4->ble_fp.INTFRAME >> 11) & 7) + 1;
        u8 secondary_phy = __get_ext_secondary_phy(hw);
        rx->secondary_phy = secondary_phy;
        u64 features = config_btctler_le_features;
        if ((secondary_phy == 1 && !(features & (1ULL << 8))) ||
            (secondary_phy == 2 && !(features & (1ULL << 11)))) {
            hw_dma_free(rx);
            return;
        }
        rx->secondary_phy = secondary_phy + 1;
        ble5 = hw->ble5_hw;
    }
    u8 len = rx->len;
    if (len)
        hw_dma_push(&hw->ble5_ext->ext_rx_head, (struct _dma *)rx);
    const struct ble_ext_handler *handler = ble5->handler;
    if (handler && handler->ext_rx_probe_handler)
        handler->ext_rx_probe_handler(hw->ble5_ext->ext_priv, rx);
    if (!rx->len)
        hw_dma_free(rx);
}

BLE5_CODE static void __set_ext_hw_rx(const struct ble_hw *hw, u16 rx_size) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    u8 *rx = hw_alloc_ext_rx(rx_size);
    if (!rx) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 1988);
            printf("ASSERT-FAILD: loop_buf_head_addr != NULL __set_ext_hw_rx alloc NULL");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    ble5->ble5_fp.EXT_RXPTR = PHY_TO_BLE(rx);
    ble5->ble5_fp.EXT_RXBASE = PHY_TO_BLE(rx);
    ble5->ble5_fp.EXT_RXMAXBUF = PHY_TO_BLE(rx + rx_size);
    ble5->ble5_fp.EXT_RXPKTLEN = 255;
    hw->ble5_hw->loop_buf_rx_len = rx_size;
    loop_cbuf_init(&ble5->loop_cbuf_rx, rx, rx_size);
    if (log_tag_const_i_LE5_BB) {
        log_print(2, NULL, "[Info]: [LE5_BB]rx loop_buf_head addr=0x%x\n\r\n", rx);
        log_print(2, NULL, "[Info]: [LE5_BB]rx loop_buf_end addr=0x%x\n\r\n", rx + rx_size);
    }
}

BLE5_CODE static inline __attribute__((always_inline)) u8 __get_ext_secondary_phy(struct ble_hw *hw) {
    return __get_ble_anchor_con(hw, 13) & 7;
}

BLE5_CODE static void __ext_hw_enable(struct ble_hw *hw, u8 enable, u32 slot) {
    if (enable)
        __set_anchor_cnt(hw, slot);
    else
        __set_anchor_cnt_disable(hw);
}

BLE5_CODE static void __set_ext_adv_channel_hop(struct ble_hw *hw, u8 ext_auxidx_en,
                                      u8 ext_alog2idx_en, u8 ext_auxhop_en) {
    hw->ble5_hw->ble5_fp.EXT_AUXCTL0 |= 64;
    u32 value = __get_ble_anchor_con(hw, 10);
    __set_ble_anchor_con(hw, 10,
        ((u8)(ext_auxhop_en << 7) | ((ext_auxidx_en << 5) & 32)) | (value & ~160U));
    __set_channel_selection_algorithm(hw, ext_alog2idx_en);
}

BLE5_CODE static inline __attribute__((always_inline)) void __clear_ext_filter_mode(const struct ble_hw *hw) {
    hw->ble5_hw->ble5_fp.EXT_AUXCTL1 &= ~7U;
}

BLE5_CODE static inline __attribute__((always_inline)) void close_ble5(struct ble_hw *hw) {
    ble_hw_inst_set(HW_ID(hw), hw->ble4_hw);
    __set_ext_mode_enable(hw, 0);
    hw->ble5_ext->correlated_hw = NULL;
    ble5_hw_frame_free(hw);
}

static void __list_del_entry(const struct list_head *entry)
    __attribute__((noinline));
static void __list_del_entry(const struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

static void list_add_tail(struct list_head *new_entry, struct list_head *head)
    __attribute__((section(".text")));
static void list_add_tail(struct list_head *new_entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = new_entry;
    new_entry->next = head;
    new_entry->prev = prev;
    prev->next = new_entry;
}

static void list_del(struct list_head *entry)
    __attribute__((section(".text")));
static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next; next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static void INIT_LIST_HEAD(struct list_head *list)
    __attribute__((noinline));
static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

BLE5_CODE static void free_ext_tx_loop_buffer_list(struct ble5_hw *hw,
                                         const struct list_head *free_head) {
    u8 *loop_buf = hw->loop_buf_head;
    if (loop_buf) {
        hw_dma_free(loop_buf);
        hw->loop_buf_head = NULL;
    }
    struct list_head *entry = free_head->next;
    while (entry != free_head) {
        struct ble_ext_tx_list *node = (struct ble_ext_tx_list *)entry;
        struct list_head *next = entry->next;
        list_del(entry);
        __bt_free(node);
        entry = next;
    }
}

BLE5_CODE static void __ext_adv_tx_loop_buffer_alloc_total(struct ble5_hw *hw) {
    u8 index = hw->ext_tx_head_index;
    struct list_head **p = &hw->ext_tx_head[index].next;
    struct ble_ext_tx_list *node;
    u16 total = 0;
    for (;;) {
        node = (struct ble_ext_tx_list *)*(ble5_unaligned_link *)p;
        if (&node->entry == &hw->ext_tx_head[index])
            break;
        total += *(ble5_unaligned_u16 *)&node->tx.txbuf_packet_length & (u16)~3072U;
        p = &node->entry.next;
    }
    if (!total)
        return;

    u32 alloc_len = total;
    hw->loop_buf_head = hw_alloc_ext_tx(alloc_len);
    if (config_asser && !hw->loop_buf_head) {
        printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 1895);
        printf("ASSERT-FAILD: hw->loop_buf_head != NULL %s\n", "RF loop_buf_head alloc err\n");
        cpu_assert_debug();
    } else if (!hw->loop_buf_head) {
        cpu_reset();
    }
    memset(hw->loop_buf_head, 0, alloc_len);
    hw->loop_buf_tx_len = total;

    index = hw->ext_tx_head_index;
    struct list_head *entry = hw->ext_tx_head[index].next;
    u8 *write = hw->loop_buf_head;
    while (entry != &hw->ext_tx_head[index]) {
        node = (struct ble_ext_tx_list *)entry;
        struct list_head *next = *(ble5_unaligned_link *)&entry->next;
        u16 len = *(ble5_unaligned_u16 *)&node->tx.txbuf_packet_length & (u16)~3072U;
        memcpy(write, &node->tx, len);
        write += len;
        list_del(entry);
        __bt_free(node);
        index = hw->ext_tx_head_index;
        entry = next;
    }
    loop_cbuf_init(&hw->loop_cbuf_tx, hw->loop_buf_head, hw->loop_buf_tx_len);
}

BLE5_CODE static inline __attribute__((always_inline)) void __ext_adv_tx_loop_buffer_data_build(struct ble5_hw *hw) {
    __ext_adv_tx_loop_buffer_alloc_total(hw) ;
}

BLE5_CODE static void loop_buffer_tx_handle(struct ble5_hw *hw) {
    if (!hw->chain_adv_total_nums)
        return;
    u32 txptr = BLE_TO_PHY(hw->ble5_fp.EXT_TXPTR); u8 *read_ptr = hw->loop_cbuf_tx.read_ptr;
    u32 send_len = txptr <= (u32)read_ptr ?
        txptr - (u32)read_ptr + hw->loop_buf_tx_len : txptr - (u32)read_ptr;
    loop_cbuf_read( &hw->loop_cbuf_tx, NULL, send_len);
    u8 index = hw->cur_ext_tx_head_index;
    struct list_head *entry = hw->ext_tx_head[index].next;
    while (entry != &hw->ext_tx_head[index]) {
        struct ble_ext_tx_list *node = (struct ble_ext_tx_list *)entry;
        struct list_head *next = *(ble5_unaligned_link *)&entry->next;
        u32 len = *(ble5_unaligned_u16 *)&node->tx.txbuf_packet_length & (u16)~3072U;
        if (!loop_cbuf_write(&hw->loop_cbuf_tx, (const u8 *)&node->tx, len))
            return;
        __list_del_entry(entry);
        list_add_tail(entry, &hw->ext_tx_head[hw->cur_ext_tx_head_index]);
        index = hw->cur_ext_tx_head_index;
        entry = next;
    }
}

BLE5_CODE static inline __attribute__((always_inline)) void __ext_adv_hw_tx_ptr_reset(struct ble5_hw *hw) {
    hw->ble5_fp.EXT_TXPTR = PHY_TO_BLE(hw->loop_aux_ptr);
}

BLE5_CODE static void __ext_tx_loop_buffer_hw_build(struct ble5_hw *hw) {
    hw->loop_aux_ptr = hw->loop_buf_head;
    __ext_adv_hw_tx_ptr_reset(hw);
    hw->ble5_fp.EXT_TXBASE = PHY_TO_BLE(hw->loop_buf_head);
    hw->ble5_fp.EXT_TXMAXBUF = PHY_TO_BLE(hw->loop_buf_head + hw->loop_buf_tx_len);
}

BLE5_CODE static inline __attribute__((always_inline)) void __ext_kickstart_next_hw(struct ble5_hw *hw, u8 next_hw_id, u8 enable) {
    hw->ble5_fp.EXT_AUXCTL0 = (hw->ble5_fp.EXT_AUXCTL0 & ~15U) | (next_hw_id & 15);
    hw->ble5_fp.EXT_AUXCTL0 = (hw->ble5_fp.EXT_AUXCTL0 & ~16U) | ((enable << 4) & 16);
}

BLE5_CODE static inline __attribute__((always_inline)) void le_hw_ext_advertising_switch(struct ble_hw *hw, u8 enable, u32 slot) {
    __ext_adv_hw_tx_ptr_reset(hw->ble5_hw);
    __ext_hw_enable(hw, enable, slot);
}

BLE5_CODE static void le_hw_ext_secondary_adv_switch(struct ble_hw *hw, u8 enable) {
    le_hw_ext_advertising_switch(hw, enable,
        hw->ble5_hw->secondary_event.anchor_point_gap_slot);
}

BLE5_CODE static void le_hw_ext_primary_adv_switch(struct ble_hw *hw, u8 enable) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    u8 id = HW_ID(hw->ble5_ext->correlated_hw);
    __ext_kickstart_next_hw(ble5, id, enable);
    le_hw_ext_advertising_switch(hw, enable, 10);
}

BLE5_CODE static inline __attribute__((always_inline)) void ext_aux_adv_process_reset(struct ble_hw *hw, u8 reset_ptr) {
    if (!hw)
        return;
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (reset_ptr) {
        u32 ptr = BLE_TO_PHY(ble5->ble5_fp.EXT_TXPTR);
        ble5 = hw->ble5_hw;
        ble5->loop_aux_ptr = (u8 *)ptr;
    }
    struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
    if (correlated)
        le_hw_ext_primary_adv_switch(correlated, 1);
    if (correlated)
        ble5 = hw->ble5_hw;
    u32 gap = ble5->secondary_event.anchor_point_gap_slot;
    u16 ptr = PHY_TO_BLE(ble5->loop_aux_ptr);
    ble5->ble5_fp.EXT_TXPTR = ptr;
    __set_anchor_cnt(hw, gap);
}

BLE5_CODE static void check_if_need_update_ext_adv_data(struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5->update_adv_data)
        return;
    struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
    struct ble5_hw *correlated_ble5 = correlated->ble5_hw;
    ble5->update_adv_data = 0;
    ble5->chain_adv_total_nums = ble5->chain_adv_cnt;
    u8 ext_tx_head_index = ble5->ext_tx_head_index;
    ble5->cur_ext_tx_head_index = ext_tx_head_index;
    __set_anchor_cnt_disable(hw);
    free_ext_tx_loop_buffer_list(correlated_ble5,
        &correlated_ble5->ext_tx_head[correlated_ble5->ext_tx_head_index == 0]);
    free_ext_tx_loop_buffer_list(ble5,
        &ble5->ext_tx_head[ble5->ext_tx_head_index == 0]);
    __ext_adv_tx_loop_buffer_alloc_total(correlated_ble5);
    __ext_tx_loop_buffer_hw_build(correlated_ble5);
    __ext_adv_tx_loop_buffer_data_build(ble5);
    __ext_tx_loop_buffer_hw_build(ble5);
}

BLE5_CODE static void ext_master_scan_data_truncated_handle(struct ble_hw *hw) {
    struct ble_ext_rx *rx = (struct ble_ext_rx *)hw_alloc_ext_rx(sizeof(*rx));
    if (!rx) {
        if (log_tag_const_e_LE5_BB)
            log_print(4, NULL, "<Error>: [LE5_BB]ext_master_scan_data_truncated_handle alloc NULL\r\n");
        return;
    }
    memset(rx, 0, sizeof(*rx));
    rx->len = 1;
    rx->data_status = 4;
    ext_rx_probe_handler(hw, rx);
}

BLE5_CODE static void ble5_hw_frame_free(struct ble_hw *hw) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5) {
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]ble5_hw_frame_free NULL\r\n");
        return;
    }
    free_ext_tx_loop_buffer_list(ble5, &ble5->ext_tx_head[ble5->ext_tx_head_index]);
    ble5 = hw->ble5_hw;
    free_ext_tx_loop_buffer_list(ble5,
        &ble5->ext_tx_head[ble5->ext_tx_head_index == 0]);
    ble5 = hw->ble5_hw;
    u8 *rxbuf = ble5->loop_cbuf_rx.begin;
    if (rxbuf) {
        hw_dma_free(rxbuf);
        ble5 = hw->ble5_hw;
    }
    btctler_nv_memory_free(ble5->regs);
    if (!hw->ble4_hw) {
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]ble5 clear hw_base.inst\n\r\n");
        struct ble_baseband_layer_handle *base = bb_hdl;
        u8 id = HW_ID(hw);
        base->hw_base.inst[id] = 0;
    }
    bb_hdl->number--;
    hw_entity_free(hw->ble5_hw);
    hw->ble5_hw = NULL;
}

BLE5_CODE void __ble5_hw_rx_process(struct ble_hw *hw) {
    u64 features = config_btctler_le_features;
    if (!(features & (1ULL << 12)))
        return;
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5)
        return;

    u32 rxptr = BLE_TO_PHY(ble5->ble5_fp.EXT_RXPTR);
    struct __loop_cbuf *cbuf = &ble5->loop_cbuf_rx;
    u8 *write_ptr = cbuf->write_ptr;
    u32 rx_len = rxptr > (u32)write_ptr ? rxptr - (u32)write_ptr :
        rxptr - (u32)write_ptr + ble5->loop_buf_rx_len;
    loop_cbuf_write(cbuf, NULL, rx_len);
    struct __common_header header;
    u32 roles = config_btctler_le_roles;
    int sync_enabled = (features & (1ULL << 13)) != 0;
    int error_log_disabled = log_tag_const_e_LE5_BB == 0;

    while (ble5->loop_cbuf_rx.can_read_len != 0) {
        loop_cbuf_read(cbuf, (u8 *)&header, 2);
        u32 len = header.length + 1;
        struct ble_ext_rx *rx = (struct ble_ext_rx *)hw_alloc_ext_rx(len + sizeof(*rx));
        if (config_asser) {
            if (!rx) {
                printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 480);
                printf("ASSERT-FAILD: rx != NULL loop_buffer_rx_get alloc NULL");
                cpu_assert_debug();
                goto rx_alloc_failed;
            }
        } else if (!rx) {
            cpu_reset();
rx_alloc_failed:
            if (log_tag_const_i_LE5_BB)
                log_print(2, NULL, "[Info]: [LE5_BB]loop_buffer_rx_get alloc NULL\r\n");
            loop_cbuf_read(cbuf, NULL, len);
            if (!error_log_disabled)
                log_print(4, NULL, "<Error>: [LE5_BB]loop_buffer_rx_get alloc NULL\r\n");
            continue;
        }

        u8 *data = rx->data;
        loop_cbuf_read(cbuf, data, len);
        u16 header_bits;
        memcpy(&header_bits, &header, sizeof(header_bits));
        u8 payload_len = header.length;
        if (data[payload_len] & 6) {
            hw_dma_free(rx);
            if (!error_log_disabled)
                log_print(4, NULL, "<Error>: [LE5_BB]ext rx error\r\n");
            continue;
        }
        rx->len = payload_len;
        rx->type = header_bits & 15;
        rx->txadd = (header_bits >> 6) & 1;
        rx->rxadd = (header_bits >> 7) & 1;
        rx->data_status = 0;
        rx->rssi = (u8)ble5_read_rssi(&hw->ble5_hw->ble5_fp);

        if (roles & 2) {
            struct ble5_hw *slave_ble5 = hw->ble5_hw;
            if (slave_ble5->soft_state == 0) {
                if (slave_ble5->hw_state == 6) {
                    slave_ble5->soft_state = 3;
                    loop_buffer_tx_handle(slave_ble5);
                } else if (slave_ble5->hw_state == 4) {
                    __set_ext_adv_channel_hop(hw, 0, 1, 1);
                    hw->ble5_hw->soft_state = 2;
                }
            }
        }

        if (roles & 1) {
            struct ble5_hw *master_ble5 = hw->ble5_hw;
            u8 soft_state = master_ble5->soft_state;
            int process_soft_state = soft_state != 0;
            if (!soft_state) {
                switch (master_ble5->hw_state) {
                case 5:
                    if ((data[0] & 0xc0) == 0x40) {
                        __set_ext_aux_event_hold_ir(hw, 1);
                        struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
                        __set_anchor_cnt_disable(correlated);
                        u8 phy = __get_ext_secondary_phy(hw);
                        hw->phy.tx_phy = phy;
                        hw->phy.rx_phy = phy;
                        hw->ble5_hw->soft_state = 12;
                    }
                    break;
                case 7: {
                    int aux = rx->type == 7 && ((data[1] & 8) != 0);
                    struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
                    if (!correlated->ble5_ext ||
                        !(correlated->ble5_ext->adi_update.update_adi & aux)) {
                        rx->len = 0;
                        break;
                    }
                    rx->data_status = 8;
                    if (sync_enabled && (data[0] & 63) != 0 && (data[1] & 32)) {
                        rx->data_status = 9;
                        __set_ext_aux_event_hold_ir(correlated, 1);
                        __set_ext_aux_event_hold_ir(hw, 1);
                        hw->ble5_hw->soft_state = 14;
                        break;
                    }
                    __set_ext_aux_event_hold_ir(correlated, 1);
                    u8 aux_mode = data[0] >> 6;
                    if (aux_mode == 0) {
                        hw->ble5_hw->soft_state = 7;
                        process_soft_state = 1;
                    } else if (aux_mode == 1) {
                        rx->data_status |= 1;
                        hw->ble5_hw->soft_state = 8;
                        break;
                    } else if (aux_mode == 2) {
                        rx->data_status |= 1;
                        hw->ble5_hw->soft_state = 9;
                        break;
                    }
                    master_ble5 = hw->ble5_hw;
                    soft_state = master_ble5->soft_state;
                    master_ble5->scan_rx_state = 1;
                    process_soft_state = 1;
                    break;
                }
                default:
                    break;
                }
            } if (process_soft_state) {
            master_ble5 = hw->ble5_hw;
            soft_state = master_ble5->soft_state;
            switch (soft_state) {
            case 9:
                rx->data_status = 16;
                /* fall through */
            case 10:
            case 7: {
                master_ble5->scan_rx_state = 1; struct ble_hw *correlated = hw->ble5_ext->correlated_hw;
                if (correlated->ble5_ext->adi_update.update_adi && get_if_last_packet(rx)) {
                    __clear_ext_filter_mode(hw);
                    rx->data_status |= 1;
                    hw->ble5_hw->soft_state = 11;
                } else {
                    hw->ble5_hw->soft_state = 10;
                }
                break;
            }
            case 12:
                master_ble5->soft_state = 13;
                break;
            case 14:
            case 16:
                if (sync_enabled) {
                    master_ble5->soft_state = 15;
                    rx->data_status = 8;
                    master_ble5->scan_rx_state = 2;
                    if (get_if_last_packet(rx)) {
                        rx->data_status |= 1;
                        hw->ble5_hw->soft_state = 16;
                    }
                }
                break;
            case 15:
                if (!sync_enabled)
                    break;
                master_ble5->scan_rx_state = 2;
                if (get_if_last_packet(rx)) {
                    rx->data_status |= 1;
                    hw->ble5_hw->soft_state = 16;
                }
                break;
            default:
                break;
            }
            }
        }
        ext_rx_probe_handler(hw, rx);
    }
}

BLE5_CODE static void __ble5_hw_master_event_end_handle(struct ble_hw *primary_hw,
                                             const struct ble_hw *secondary_hw) {
    if (!primary_hw || !secondary_hw)
        return;
    struct ble5_hw *ble5 = secondary_hw->ble5_hw;
    if (!ble5 || ble5->soft_state > 13)
        return;
    ble5->soft_state = 0;
    ble5->scan_rx_state = 0;
    primary_hw->ble5_ext->adi_update.update_adi = 0;
    __set_ext_aux_event_hold_ir(primary_hw, 0);
}

BLE5_CODE void ble5_master_primary_event_end_handle(struct ble_hw *hw) {
    if (!(config_btctler_le_features & (1ULL << 12)) ||
        !(config_btctler_le_roles & 1))
        return;
    if (hw->ble5_hw)
        return;
    struct ble5_extended *ext = hw->ble5_ext;
    if (!ext || !hw->ble4_hw || !ext->correlated_hw)
        return;
    struct ble_hw *secondary = ext->correlated_hw;
    if (get_if_aux_enable(secondary))
        return;
    if (!(*(volatile u32 *)0x1c8038 & 2))
        __ble5_hw_master_event_end_handle(hw, secondary);
}

BLE5_CODE void __ble5_hw_event_process(struct ble_hw *hw) {
    u64 features = config_btctler_le_features;
    if (!(features & (1ULL << 12)))
        return;
    struct ble5_hw *ble5 = hw->ble5_hw;
    if (!ble5)
        return;
    if (config_btctler_le_roles & 1) {
        u8 state = ble5->soft_state;
        if (!state) {
            if (ble5->hw_state == 5 || ble5->hw_state == 7) {
                __set_ext_aux_event_hold_ir(hw, 1);
                __set_ext_aux_event_hold_ir(hw->ble5_ext->correlated_hw, 0);
                ble5 = hw->ble5_hw;
                state = ble5->soft_state;
            }
        }
        switch (state) {
        case 10:
            if (ble5->scan_rx_state == 1) {
                ble5->scan_rx_state = 0;
                break;
            }
            ext_master_scan_data_truncated_handle(hw);
            /* fall through */
        case 8:
        case 7:
        case 9:
        case 11:
            __ble5_hw_master_event_end_handle(hw->ble5_ext->correlated_hw, hw);
            __set_ext_aux_event_hold_ir(hw, 1);
            break;
        case 12:
            __set_anchor_cnt_disable(hw);
            ble5 = hw->ble5_hw;
            if (ble5->handler && ble5->handler->ext_event_probe_handler)
                ble5->handler->ext_event_probe_handler(hw->ble5_ext->ext_priv, 12);
            break;
        case 13:
            __set_ext_adv_channel_hop(hw, 0, 1, 1);
            close_ble5(hw);
            break;
        case 15:
            if (features & (1ULL << 13)) {
                if (!(ble5->ble5_fp.EXT_AUXCTL1 & 4096)) {
                    u32 anchor = __get_ble_anchor_con(hw, 10);
                    __set_ble_anchor_con(hw, 10, anchor | 1024);
                    ble5 = hw->ble5_hw;
                }
                if (ble5->scan_rx_state == 2) {
                    ble5->scan_rx_state = 0;
                    break;
                }
                ext_master_scan_data_truncated_handle(hw);
                hw->ble5_hw->soft_state = 16;
            }
            break;
        default:
            break;
        }
    }

    if (!(config_btctler_le_roles & 2))
        return;
    ble5 = hw->ble5_hw;
    if (!ble5->soft_state) {
        switch (ble5->hw_state) {
        case 0:
            if (hw->ble5_ext->correlated_hw) {
                u8 event_count = ble5->primary_event.event_cnt;
                ble5->primary_event.event_cnt = event_count + 1;
                if (event_count == ble5->secondary_event.max_primary_events_skip) {
                    le_hw_ext_primary_adv_switch(hw, 0);
                    ble5->primary_event.event_cnt = 0;
                    ble5 = hw->ble5_hw;
                    struct __AuxPtr_field *pdu = (void *)(ble5->loop_buf_head +
                        ble5->ExtAdvPdu_AuxChIndex_offset);
                    u8 channel = aux_data_channel;
                    u8 index;
                    if ((s8)channel == -1) {
                        u8 random;
                        pseudo_random_genrate(&random, 1);
                        index = btctler_get_rand_from_assign_range(random, 0, 36) & 63;
                    } else {
                        index = channel > 36 ? 23 : (channel & 63);
                    }
                    pdu->channel_index = index;
                }
            }
            break;
        case 2:
            ble5->soft_state = 1;
            break;
        case 6:
            check_if_need_update_ext_adv_data(hw);
            /* fall through */
        case 4:
            ext_aux_adv_process_reset(hw, 0); break;
        default:
            break;
        }
    }

    ble5 = hw->ble5_hw;
    switch (ble5->soft_state) {
    case 2:
        close_ble5(hw);
        break;
    case 1:
    case 3:
        ble5->remain_chain_nums = ble5->chain_adv_total_nums;
        /* fall through */
    case 4:
        loop_buffer_tx_handle(ble5);
        ble5 = hw->ble5_hw;
        if (ble5->remain_chain_nums) {
            --ble5->remain_chain_nums;
            ble5->soft_state = 4;
            __set_anchor_cnt_disable(hw);
            return;
        }
        if (!hw->ble5_ext->correlated_period_hw) {
            check_if_need_update_ext_adv_data(hw);
            ext_aux_adv_process_reset(hw, 1);
        }
        hw->ble5_hw->soft_state = 0;
        break;
    case 5:
        if (!(features & (1ULL << 13)))
            return;
        ble5->remain_chain_nums = ble5->chain_adv_total_nums;
        /* fall through */
    case 6:
        if (!(features & (1ULL << 13)))
            return;
        loop_buffer_tx_handle(ble5);
        ble5 = hw->ble5_hw;
        if (ble5->remain_chain_nums) {
            --ble5->remain_chain_nums;
            ble5->soft_state = 6;
            break;
        }
        struct ble_hw *period_hw = hw->ble5_ext->correlated_hw;
        if (period_hw->ble5_hw->update_adv_data && ble5->update_adv_data) {
            ble5->update_adv_data = 0;
            ble5->chain_adv_total_nums = ble5->chain_adv_cnt;
            u8 index = ble5->ext_tx_head_index;
            ble5->cur_ext_tx_head_index = index;
            free_ext_tx_loop_buffer_list(ble5, &ble5->ext_tx_head[index == 0]);
            __ext_adv_tx_loop_buffer_data_build(ble5);
            __ext_tx_loop_buffer_hw_build(ble5);
            check_if_need_update_ext_adv_data(period_hw);
            period_hw = hw->ble5_ext->correlated_hw;
        }
        ext_aux_adv_process_reset(period_hw, 1);
        hw->ble5_hw->soft_state = 5;
        break;
    default:
        break;
    }
}

BLE5_CODE static inline __attribute__((always_inline)) u8 get_if_last_packet(const struct ble_ext_rx *rx) {
    if (!(rx->data[0] & 63))
        return 1;
    struct __AuxPtr_field auxptr = {0};
    __ext_header_resolve(rx->data[1], rx->data + 2, 16, (u8 *)&auxptr);
    return auxptr.aux_offset == 0;
}

u8 ble_ext_primary_scan_preprocess_ir(struct ble_hw *hw, struct ble_rx *rx)
    __asm__("ble_ext_primary_scan_preprocess") ;
BLE5_CODE u8 ble_ext_primary_scan_preprocess_ir(struct ble_hw *hw, struct ble_rx *rx) {
    if (!(config_btctler_le_features & (1ULL << 12)) ||
        !(config_btctler_le_roles & 1))
        return 0;
    struct ble5_extended *ext = hw->ble5_ext;
    if (!ext || !ext->correlated_hw)
        return 0;
    if (!rx->len)
        return 1;
    struct ble_hw *correlated = ext->correlated_hw;
    if (rx->type != 7) {
        ble_ext_primary_scan_report_produce(correlated, rx, 33);
        return 1;
    }

    u8 adva[6] = {0};
    struct __AuxPtr_field auxptr = {0};
    struct __AdvDataInfo_field adi = {0};
    if (!(rx->data[0] & 63))
        return 1;
    u8 flags = rx->data[1];
    if (!(flags & 16)) {
        ble_ext_primary_scan_report_produce(correlated, rx, 33);
        return 1;
    }
    if (!(flags & 8))
        return 1;
    const u8 *header = rx->data + 2;
    __ext_header_resolve(flags, header, 16, (u8 *)&auxptr);
    if (!auxptr.aux_offset) {
        ble_ext_primary_scan_report_produce(correlated, rx, 33) ;
        return 1;
    }
    __set_ext_aux_event_hold_ir(hw, 1);
    __ext_header_resolve(flags, header, 8, (u8 *)&adi);
    __ext_header_resolve(flags, header, 1, adva);

    u8 count = aux_filter.save_nums;
    u16 received_adi = adi.did | ((u16)adi.sid << 12);
    u32 i;
    for (i = 0; i < count; ++i) {
        if (received_adi == aux_filter.aux_entry[i].adi &&
            memcmp(adva, aux_filter.aux_entry[i].adva, 6) == 0) {
            __set_ext_aux_event_hold_ir(hw, 0);
            count = aux_filter.save_nums;
            break;
        }
    }
    struct ble5_extended *live_ext = hw->ble5_ext;
    if (i == count) {
        live_ext->adi_update.update_adi = 1;
        received_adi = adi.did | ((u16)adi.sid << 12);
        live_ext->adi_update.adi = received_adi;
        memcpy(live_ext->adi_update.adva, adva, 6);
        struct ble5_hw *correlated_ble5 = correlated->ble5_hw;
        correlated_ble5->ble5_fp.EXT_ADI = (received_adi & 4095) |
            (correlated_ble5->ble5_fp.EXT_ADI & ~4095U);
        correlated_ble5->ble5_fp.EXT_ADI = (received_adi & ~4095U) |
            (correlated_ble5->ble5_fp.EXT_ADI & 4095U);
        correlated_ble5->ble5_fp.EXT_AUXCTL0 |= 128;
        __set_ext_aux_event_hold_ir(correlated, 0);
    } else {
        live_ext->adi_update.update_adi = 0;
    }
    ble_ext_primary_scan_report_produce(correlated, rx, 64);
    return 1;
}

BLE5_CODE void __ble5_hw_anchor_cnt_timeout(struct ble_hw *ble_hw) {
    (void)ble_hw;
}


BLE5_CODE void repair_algorithm2_hop(struct ble_hw *ble_hw) {
    if (!(config_btctler_le_features & (1ULL << 14)))
        return;
    if (!(__get_ble_anchor_con(ble_hw, 10) & 64))
        return;
    struct ble4_hw *hw4 = ble_hw->ble4_hw;
    u16 event = hw4->ble_fp.EVTCOUNT + 1;
    u16 chmap2 = hw4->ble_fp.CHMAP2;
    u16 addr0 = hw4->ble_fp.BDADDR0;
    u16 addr1 = hw4->ble_fp.BDADDR1;
    u16 xor1 = addr1 ^ addr0;
    u16 value = xor1 ^ event;
    for (u8 i = 0; i < 3; ++i) {
        u16 reverse = 0;
        for (u8 bit = 0; bit < 16; ++bit)
            reverse |= (u16)((value >> bit) & 1) << ((bit & 8) + 7 - (bit & 7));
        value = (u16)((u32)reverse * 17 + xor1);
    }
    u16 channel = (value ^ xor1) % 37;
    u16 shadow2 = hw4->CHMAP[2];
    u16 shadow1 = hw4->CHMAP[1];
    u16 shadow0 = hw4->CHMAP[0];
    u64 channel_map = (u64)shadow0 | ((u64)shadow1 << 16) |
                      ((u64)(shadow2 & 31) << 32);
    if (!(channel_map & (1ULL << channel))) {
        u32 remap = ((u32)(value ^ xor1) * ((chmap2 >> 5) & 63)) >> 16;
        u64 remap_bit = 1ULL << remap;
        if ((channel_map & remap_bit) == remap_bit) {
            if (remap < 16)
                hw4->ble_fp.CHMAP0 = shadow0 & (u16)~(1U << remap);
            else if (remap < 32)
                hw4->ble_fp.CHMAP1 = shadow1 & (u16)~(1U << (remap - 16));
            else
                hw4->ble_fp.CHMAP2 = shadow2 & (u16)~(1U << (remap - 32));
            return;
        }
    }
    hw4->ble_fp.CHMAP0 = shadow0;
    hw4->ble_fp.CHMAP1 = shadow1;
    hw4->ble_fp.CHMAP2 = shadow2;
}

BLE5_CODE static void le_hw_legacy_open(struct ble_hw *hw) {
    ble4_hw_only_alloc_dma_ram(hw);
    struct ble4_hw *hw4 = hw->ble4_hw;
    INIT_LIST_HEAD(&hw4->rx_head.head);
    INIT_LIST_HEAD(&hw4->tx_head.head);
    INIT_LIST_HEAD(&hw4->tx_head_h.head);
    ble_rx_init(hw4);
    ble_tx_init(hw4);
    hw4->rx[0] = hw_alloc_rx(255);
    hw4->rx[1] = hw_alloc_rx(255);
    if (!hw4->rx[0] || !hw4->rx[1]) {
        if (log_tag_const_e_LE5_BB)
            log_print(4, NULL, "<Error>: [LE5_BB]Legacy RX DMA NULL\r\n");
        return;
    }
    ble_hw_rx(hw, hw4->rx[0], 0);
    ble_hw_rx(hw, hw4->rx[1], 1);
}

BLE5_CODE static void le_hw_legacy_close(struct ble_hw *hw) {
    le_hw_close(hw);
    free_ble5_ext(hw);
}

BLE5_CODE static void *le_ext_hw_open(void) {
    struct ble_hw *hw = ble_hw_alloc();
    struct ble5_hw *ble5 = hw_entity_alloc();
    hw->ble5_hw = ble5;
    u8 id = HW_ID(hw);
    ble_hw_inst_set(id, hw->ble5_hw);
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "__set_hw_frame_init_5_0");
    ble5 = hw->ble5_hw;
    /* BD19 names this SDK register slot ana_index; the SDK writes 15 only. */
    ble5->ble5_fp.ana_index = 15;
    ble5->ble5_fp.EXT_IFSCNT = (ble5->ble5_fp.EXT_IFSCNT & ~255U) | 140U;
    ble5->ble5_fp.EXT_IFSCNT = (ble5->ble5_fp.EXT_IFSCNT & 255U) | (u32)-32256; ble5->ble5_fp.EXT_RXSHIFT = 1;
    ble5_agc_normal_set(ble5, 0, 25);
    ble5 = hw->ble5_hw;
    u8 power = bt_get_pwr_max_level();
    ble5_txpwr_normal_set(ble5, 0, power);
    if (hw->ble5_hw) {
        void *regs = btctler_nv_memory_malloc(40);
        hw->ble5_hw->regs = regs;
        ++bb_hdl->number;
        void *ext = btctler_nv_memory_malloc(32);
        hw->ble5_ext = ext;
        if (!ext) {
            if (config_asser) {
                printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 257);
                printf("ASSERT-FAILD: __ext(hw) != NULL nv alloc __ext(hw) err");
                cpu_assert_debug();
            } else {
                cpu_reset();
            }
        }
    }
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]ext HW open id : %d \r\n", HW_ID(hw));
    INIT_LIST_HEAD(&hw->ble5_hw->ext_tx_head[hw->ble5_hw->ext_tx_head_index]);
    INIT_LIST_HEAD(&hw->ble5_hw->ext_tx_head[hw->ble5_hw->ext_tx_head_index == 0]);
    INIT_LIST_HEAD(&hw->ble5_ext->ext_rx_head.head);
    ble_power_get(hw);
    return hw;
}

BLE5_CODE static void le_ext_hw_close(struct ble_hw *hw) {
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]ext HW close id : %d \r\n", HW_ID(hw));
    if (!hw->ble4_hw) {
        ble_hw_disable(hw);
        ble_power_put(hw);
    }
    hw_dma_clear(&hw->ble5_ext->ext_rx_head);
    free_ble5_ext(hw);
    ble5_hw_frame_free(hw);
}

BLE5_CODE static void le_ext_hw_handler_register(struct ble_hw *hw, void *priv,
                                       const struct ble_ext_handler *handler) {
    hw->ble5_ext->ext_priv = priv;
    hw->ble5_hw->handler = handler;
}

BLE5_CODE static void le_ext_hw_upload_data(struct ble_hw *hw,
                                  void (*rx_handler)(void *, struct ble_ext_rx *)) {
    struct ble_ext_rx *rx = hw_dma_pop(&hw->ble5_ext->ext_rx_head); if (!rx)
        return;
    rx_handler(hw->ble5_ext->ext_priv, rx);
    hw_dma_free(rx);
}

BLE5_CODE static u32 le_get_ext_adv_packet_len_us(u32 pdu_len, u8 phy, u8 coded_s) {
    switch (phy) {
    case 0:
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_UNCODED_PHY\r\n");
        return (pdu_len << 3) + 64;
    case 1:
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]LE_2M_UNCODED_PHY\r\n");
        return ((pdu_len << 2) & 0x7ffffffcU) + 36;
    case 2:
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_CODED_PHY\r\n");
        return (((pdu_len << 3) + 27) * (coded_s == 0 ? 8 : 2)) + 376;
    default:
        return 0;
    }
}

BLE5_CODE static inline __attribute__((always_inline)) u8 if_need_sync_info(struct ble_hw *hw) {
    return hw->ble5_ext->correlated_period_hw != NULL;
}

BLE5_CODE static u8 if_need_aux_ptr(struct ble_ext_adv *adv, u8 flag) {
    if (!adv->adv_data_len)
        return 0;
    u8 payload_len = (flag & 111) == 0 ? 1 : 2;
    for (u32 i = 0; i < 6; ++i) {
        if (((flag & (u8)-17) & ext_header_table[i][0]) != 0)
            payload_len += ext_header_table[i][1];
    }
    if ((s8)flag < 0) {
        if (payload_len == 0xff) {
            if (config_asser) {
                printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/ble/RF_ble5.c", 2936);
                puts("ASSERT-FAILD: EXT_PAYLOAD_MAX_SIZE > payload_len EXT_PAYLOAD_MAX_SIZE should more");
                cpu_assert_debug();
            } else {
                cpu_reset();
            }
        }
        return adv->adv_data_len > (u8)~payload_len;
    }
    return 1;
}

BLE5_CODE static void __aux_connection_rsp_pdu(struct ble_hw *hw, struct ble_ext_adv *adv) {
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_CONNECT_RSP <----\n\r\n");
    (void)__common_ext_adv_pdu_build(hw, adv, 5, 0, 3);
}

BLE5_CODE static void __aux_scan_rsp_pdu(struct ble_hw *hw, struct ble_ext_adv *adv) {
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_SCAN_RSP <----\n\r\n");
    u8 flag = if_need_aux_ptr(adv, (u8)-111) ? (u8)-111 : (u8)-127;
    (void)__common_ext_adv_pdu_build(hw, adv, 2, 0, flag);
    __aux_chain_pdu(hw, adv, flag);
}

BLE5_CODE static void __aux_chain_pdu(struct ble_hw *hw, struct ble_ext_adv *adv,
                            u8 superior_flag) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->chain_adv_cnt = 0;
    u8 aux_flag = (superior_flag & 8) | (u8)-112;
    while (adv->adv_data_len) {
        ++ble5->chain_adv_cnt;
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_CHAIN_IND <----\n\r\n");
        u8 flag = if_need_aux_ptr(adv, aux_flag) ? aux_flag :
                  ((superior_flag & 8) | (u8)-128);
        (void)__common_ext_adv_pdu_build(hw, adv, 4, 0, flag);
        if (adv->adv_data_len)
            ble5 = hw->ble5_hw;
    }
}

BLE5_CODE static void __aux_sync_pdu(struct ble_hw *hw,
                           const struct ble_ext_period_adv *sync_adv) {
    struct ble_ext_adv adv;
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_SYNC_IND <----\n\r\n");
    memset(&adv, 0, sizeof(adv));
    adv.adv_data_len = sync_adv->adv_data_len;
    adv.adv_data = sync_adv->adv_data;
    adv.channel_index = sync_adv->chain_channel_index;
    adv.secondary_phy = sync_adv->chain_phy;
    adv.coded_s = sync_adv->chain_coded_s;
    u8 flag = if_need_aux_ptr(&adv, (u8)-112) ? (u8)-112 : (u8)-128;
    (void)__common_ext_adv_pdu_build(hw, &adv, 3, 0, flag);
    __aux_chain_pdu(hw, &adv, flag);
}

BLE5_CODE static u8 __common_ext_adv_pdu_build(struct ble_hw *hw, struct ble_ext_adv *adv,
                                     u8 pdu_name, u8 advmode, u8 ext_flag) {
    struct ble5_hw *ble5 = hw->ble5_hw;
    u8 payload_len = (ext_flag & 127) == 0 ? 1 : 2;
    for (u32 i = 0; i < 6; ++i) {
        if (ext_header_table[i][0] & ext_flag)
            payload_len += ext_header_table[i][1];
    }
    u8 length = payload_len;
    if ((s8)ext_flag < 0) {
        if (adv->adv_data_len > (u8)~payload_len)
            length = 0xff;
        else
            length = (u8)(adv->adv_data_len + payload_len);
    }
    u32 alloc_payload_size = (length + 7U) & 508U;
    u8 *allocation = __bt_malloc(alloc_payload_size + 8);
    u8 *tx = allocation + 8;
    u8 *add_ptr = allocation + 13;
    u8 v5 = ext_flag & 123;
    u8 *ptr = add_ptr;
    if (v5) {
        *ptr = v5;
        ++ptr;
    }
    for (u32 i = 0; i < 6; ++i) {
        switch ((u8)((1U << i) & v5)) {
        case 1:
            memcpy(ptr, adv->addr.addr, 6);
            ptr += 6;
            break;
        case 2:
            memcpy(ptr, adv->peer_addr.addr, 6);
            ptr += 6;
            break;
        case 8: {
            u16 adi = (u16)((adv->sid << 12) | (adv->did & 4095));
            memcpy(ptr, &adi, sizeof(adi));
            ptr += 2;
            break;
        }
        case 16: {
            u8 phy = adv->secondary_phy;
            u32 flags = ((u32)phy << 21) | (adv->channel_index & 63);
            u32 packet_len = le_get_ext_adv_packet_len_us((u32)length + 2,
                                                           phy & 7, adv->coded_s);
            u32 offset_us = packet_len + 530;
            if (offset_us >= 245700)
                flags |= 128;
            u32 divisor = (flags & 128) ? 300 : 30;
            u32 offset = offset_us / divisor;
            if (offset > 8191)
                offset = 8191;
            u32 auxptr = ((offset & 8191) << 8) | flags;
            if (log_tag_const_i_LE5_BB) {
                log_print(2, NULL, "[Info]: [LE5_BB]adv_packet_len_us=%u\r\n", packet_len);
                log_print(2, NULL, "[Info]: [LE5_BB]offset_units=0x%x\r\n", (flags >> 7) & 1);
                log_print(2, NULL, "[Info]: [LE5_BB]aux_offset=0x%x\r\n", (auxptr >> 8) & 8191);
                log_print(2, NULL, "[Info]: [LE5_BB]expect aux_offset us=%u\r\n", packet_len + 500);
                log_print(2, NULL, "[Info]: [LE5_BB]real aux_offset us=%u\r\n", ((auxptr >> 8) & 8191) * divisor);
                log_print(2, NULL, "[Info]: [LE5_BB]expect free gap us=%u\r\n", 500);
                log_print(2, NULL, "[Info]: [LE5_BB]real free gap us=%u\r\n", (((auxptr >> 8) & 8191) * divisor) - packet_len);
            }
            memcpy(ptr, &auxptr, 3);
            hw->ble5_hw->ExtAdvPdu_AuxChIndex_offset = (u8)(5 + (ptr - add_ptr));
            ptr += 3;
            break;
        }
        case 32: {
            u16 interval = adv->sync_info.interval;
            u32 aa = adv->sync_info.aa;
            u8 crc[3], chm[5];
            memcpy(crc, adv->sync_info.crcinit, 3);
            memcpy(chm, adv->sync_info.chm, 5);
            u16 zero = 0;
            memcpy(ptr, &zero, 2);
            memcpy(ptr + 2, &interval, 2);
            memcpy(ptr + 4, chm, 5);
            memcpy(ptr + 9, &aa, 4);
            memcpy(ptr + 13, crc, 3);
            memcpy(ptr + 16, &zero, 2);
            ptr += 18;
            break;
        }
        default:
            break;
        }
    }
    u8 header_len = (u8)(ptr - add_ptr) & 63;
    allocation[12] = (u8)(header_len | (advmode << 6));
    if ((s8)ext_flag < 0) {
        u8 cap = (u8)(-2 - header_len);
        u8 copy_len = adv->adv_data_len > cap ? cap : (u8)adv->adv_data_len;
        memcpy(allocation + 13 + header_len, adv->adv_data, copy_len);
        adv->adv_data_len -= copy_len;
        adv->adv_data += copy_len;
    }
    u8 *header = allocation + 10;
    u16 old_header;
    memcpy(&old_header, header, 2);
    u16 value = (u16)(((u16)length << 8) | ((pdu_name > 4) ? 8 : 7) | (old_header & 240));
    memcpy(header, &value, 2);
    value = (u16)(((adv->addr.addr_type << 6) & 64) | (value & (u16)-65));
    memcpy(header, &value, 2);
    u16 tx_packet_length = (u16)(((adv->coded_s & 3) << 10) | alloc_payload_size);
    memcpy(tx, &tx_packet_length, 2);
    list_add_tail((struct list_head *)allocation,
                  &ble5->ext_tx_head[ble5->ext_tx_head_index]);
    if (log_tag_const_i_LE5_BB)
        printf_buf(tx, *(ble5_unaligned_u16 *)tx & (u16)-3073);
    return length;
}

BLE5_CODE static u8 __adv_ext_pdu(struct ble_hw *hw, struct ble_ext_adv *adv) {
    bool log_info = log_tag_const_i_LE5_BB != 0;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]----> ADV_EXT_IND <----\n\r\n");
    switch (adv->event_type) {
    case 1:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN_NONAUX\r\n");
        return __common_ext_adv_pdu_build(hw, adv, 0, 0, 1);
    case 2: {
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN_AUX\r\n");
        u8 len = __common_ext_adv_pdu_build(hw, adv, 0, 0,
                                            adv->primary_phy == 2 ? 24 : 25);
        adv->event_type = 2;
        return len;
    }
    case 3:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN_DIRECT_NONAUX\r\n");
        return __common_ext_adv_pdu_build(hw, adv, 0, 0, 3);
    case 4: {
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN_DIRECT_AUX\r\n");
        u8 len = __common_ext_adv_pdu_build(hw, adv, 0, 0,
                                            adv->primary_phy == 2 ? 26 : 27);
        adv->event_type = 4;
        return len;
    }
    case 5: case 6:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_CON_UNDIRECT\r\n");
        return __common_ext_adv_pdu_build(hw, adv, 0, 1, 24);
    case 7: case 8:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_SCAN_UNDIRECT\r\n");
        return __common_ext_adv_pdu_build(hw, adv, 0, 2, 24);
    default:
        return 0;
    }
}

BLE5_CODE static void __aux_adv_pdu(struct ble_hw *hw, struct ble_ext_adv *adv) {
    bool log_info = log_tag_const_i_LE5_BB != 0;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_ADV_IND <----\n\r\n");
    switch (adv->event_type) {
    case 2: {
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN\r\n");
        u8 base = if_need_sync_info(hw) ? (u8)-72 : (u8)-104;
        u8 flag = ((if_need_aux_ptr(adv, base) ? base : (base & (u8)-88)) |
                   (adv->primary_phy == 2));
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 0, flag);
        __aux_chain_pdu(hw, adv, flag);
        break;
    }
    case 4: {
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_NONCON_NONSCAN_DIRECT\r\n");
        u8 base = if_need_sync_info(hw) ? 56 : 24;
        u8 flag = ((if_need_aux_ptr(adv, base) ? base : (base & 40)) |
                   (adv->primary_phy == 2));
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 0, flag);
        __aux_chain_pdu(hw, adv, flag);
        break;
    }
    case 5:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_CON_UNDIRECT\r\n");
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 1, (u8)-119);
        __aux_connection_rsp_pdu(hw, adv);
        break;
    case 6:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_CON_DIRECT\r\n");
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 1, (u8)-117);
        __aux_connection_rsp_pdu(hw, adv);
        break;
    case 7:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_SCAN_UNDIRECT\r\n");
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 2, 9);
        __aux_scan_rsp_pdu(hw, adv);
        break;
    case 8:
        if (log_info) log_print(2, NULL, "[Info]: [LE5_BB]EVENT_SCAN_DIRECT\r\n");
        (void)__common_ext_adv_pdu_build(hw, adv, 1, 2, 11);
        __aux_scan_rsp_pdu(hw, adv);
        break;
    default:
        break;
    }
}

BLE5_CODE static void __le_ext_master_build_hw(struct ble_hw *hw, u32 hw_state) {
    __set_ext_adv_channel_map(hw);
    __set_ext_adv_channel_hop(hw, 1, 0, 1);
    __disable_adv_random(hw);
    hw->ble5_hw->ble5_fp.EXT_WINCNTL0 = 150;
    hw->ble5_hw->ble5_fp.EXT_WINCNTL1 = 0;
    hw->ble5_hw->ble5_fp.EXT_WINCNTL2 = 1000;
    __set_ext_adv_other_hw(hw);
    __set_ext_adv_aa_crcinit(hw);
    struct ble5_hw *ble5 = hw->ble5_hw;
    ble5->ble5_fp.EXT_WIDEN1 = (ble5->ble5_fp.EXT_WIDEN1 & (u32)-1024) | 30;
    if (ble5) {
        ble5->ble5_fp.EXT_PHYDRIFT0 = (ble5->ble5_fp.EXT_PHYDRIFT0 & (u32)-256) | 14;
        ble5->ble5_fp.EXT_PHYDRIFT0 = (ble5->ble5_fp.EXT_PHYDRIFT0 & 255) | 12800;
        ble5->ble5_fp.EXT_PHYDRIFT1 = (ble5->ble5_fp.EXT_PHYDRIFT1 & (u32)-256) | 180;
        ble5->ble5_fp.EXT_PKTDRIFT = 0;
    }
    __set_ble5_ifs_phy_compens(hw);
    __set_ext_hw_rx(hw, hw_state == 7 ? 774 : 258);
    __set_ext_hw_state(hw, hw_state);
    __set_ext_mode_enable(hw, 1);
    __set_ble_hw_irq_init(hw);
    __set_ble_hw_timeout_irq_init(hw);
    __set_interval(hw, 16777215, 0);
    __set_anchor_cnt_disable_with_default(hw);
}

BLE5_CODE static void __ext_master_pdu_data_build(struct ble5_hw *hw,
                                         const struct __common_header *pdu_header, const u8 *payload_buffer, u8 coded_s) {
    u16 header_bits;
    memcpy(&header_bits, pdu_header, sizeof(header_bits));
    u32 size = (((header_bits >> 8) + 7U) & 508U);
    bool log_info = log_tag_const_i_LE5_BB != 0;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "__ext_master_pdu_data_build");
    u8 *buffer = hw_alloc_ext_tx(size);
    memcpy(buffer, &size, 2);
    memcpy(&header_bits, pdu_header, sizeof(header_bits));
    memcpy(buffer + 2, &header_bits, 2);
    memcpy(&header_bits, pdu_header, sizeof(header_bits));
    memcpy(buffer + 4, payload_buffer, header_bits >> 8);
    hw->loop_buf_head = buffer;
    hw->loop_buf_tx_len = (u16)size;
    u16 packet_length = (u16)(size | ((coded_s & 3) << 10));
    memcpy(buffer, &packet_length, 2);
    if (log_info)
        printf_buf(buffer, size);
}

BLE5_CODE static inline __attribute__((always_inline)) void __le_ext_legacy_scan_build_hw(struct ble_hw *hw) {
    struct ble4_hw *hw4 = hw->ble4_hw;
    hw4->ble_fp.EXTCTL0 = 0;
    hw4->ble_fp.EXTCTL1 = 1;
    hw4->ble_fp.EXTCTL0 |= 1;
    u16 control = hw4->ble_fp.EXTCTL0;
    u8 id = HW_ID(hw->ble5_ext->correlated_hw);
    hw4->ble_fp.EXTCTL0 = ((id << 1) & 14) | (control & (u32)-15);
    hw4->ble_fp.EXTCTL0 &= (u32)-17;
    __set_ext_aux_event_hold_ir(hw->ble5_ext->correlated_hw, 1);
    hw4->ble_fp.WIDEN1 = (hw4->ble_fp.WIDEN1 & (u32)-1024) | 30;
}

static void le_ext_primary_init_build(struct ble_hw *hw,
                                      const struct ble_ext_init *init)
    __attribute__((always_inline));
BLE5_CODE static inline __attribute__((always_inline)) void le_ext_primary_init_build(struct ble_hw *hw,
                                      const struct ble_ext_init *init) {
    struct ble_conn conn;
    u8 phy_index = 0;
    u8 primary_scan_phy = 0;
    bool log_info = log_tag_const_i_LE5_BB != 0;
    for (u32 i = 0; i < 3; ++i) {
        switch (init->ext_init->Initiating_PHYs & (1U << i)) {
        case 1:
            if (log_info) {
                log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_UNCODED_PHY\r\n");
                printf_buf(&init->ext_init->init_phy_param[phy_index], 16);
            }
            ++phy_index;
            primary_scan_phy = 0;
            break;
        case 2:
            if (log_info) {
                log_print(2, NULL, "[Info]: [LE5_BB]LE_2M_UNCODED_PHY\r\n");
                printf_buf(&init->ext_init->init_phy_param[phy_index], 16);
            }
            ++phy_index;
            primary_scan_phy = 1;
            break;
        case 4:
            if (log_info) {
                log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_CODED_PHY\r\n");
                printf_buf(&init->ext_init->init_phy_param[phy_index], 16);
            }
            ++phy_index;
            primary_scan_phy = 2;
            break;
        default:
            break;
        }
    }
    memcpy(conn.ll_data.aa, init->conn_param.aa, 22);
    le_hw_initiating_common(hw, &conn);
    __set_ble_hw_timeout_irq_init(hw);
    __le_ext_legacy_scan_build_hw(hw);
    __set_ble4_rx_phy_compens(hw);
    __set_data_rx_phy(hw, primary_scan_phy);
    ble_hw_enable(hw, 1);
}

static void __aux_connect_req(struct ble_hw *hw, const struct ble_ext_init *init)
    __attribute__((always_inline));
BLE5_CODE static inline __attribute__((always_inline)) void __aux_connect_req(struct ble_hw *hw, const struct ble_ext_init *init) {
    struct __common_header header = {0};
    struct __connect_req_payload payload;
    bool log_info = log_tag_const_i_LE5_BB != 0;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_CONNECT_REQ <----\n\r\n");
    u16 bits = (u16)((((init->local.addr_type << 6) & 64) |
                      (init->ext_init->Peer_Address_Type << 7)) | 8709);
    memcpy(&header, &bits, sizeof(bits));
    memcpy(payload.InitA, init->local.addr, 6);
    memcpy(payload.AdvA, init->ext_init->Peer_Address, 6);
    memcpy(&payload.LLData, init->conn_param.aa, 22);
    __ext_master_pdu_data_build(hw->ble5_hw, &header, (const u8 *)&payload,
                                init->coded_s);
}

BLE5_CODE static void le_hw_ext_scanning(struct ble_hw *hw, struct ble_hw *legacy_hw,
                               struct ble_ext_scan *scan) {
    if (!(config_btctler_le_roles & 1))
        return;
    bool log_info = log_tag_const_i_LE5_BB != 0;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_scanning");
    struct __common_header pdu_header;
    struct __scan_req_payload payload;
    if (log_info)
        log_print(2, NULL, "[Info]: [LE5_BB]----> AUX_SCAN_REQ <----\n\r\n");
    u16 header = (u16)((((scan->addr.addr_type << 6) & 64) | 3075));
    memcpy(&pdu_header, &header, sizeof(header));
    memset(&payload.ScanA[6], 0, 6);
    memcpy(payload.ScanA, scan->addr.addr, 6);
    struct ble5_hw *ble5 = hw->ble5_hw;
    __ext_master_pdu_data_build(ble5, &pdu_header, payload.ScanA, scan->coded_s);
    __ext_tx_loop_buffer_hw_build(hw->ble5_hw);
    __le_ext_master_build_hw(hw, 7);

    struct __ext_scan *ext = scan->ext_scan;
    struct ble_scan legacy_scan;
    const struct __scan_phy_param *first_phy = NULL;
    u8 phy_type = 0;
    u8 phy_index = 0;
    for (u32 i = 0; i < 3; ++i) {
        ext = scan->ext_scan;
        switch (ext->ext_scan_param.Scanning_PHYs & (1U << i)) {
        case 1:
            if (log_info)
                log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_UNCODED_PHY\r\n");
            first_phy = &scan->ext_scan->ext_scan_param.scan_phy_param[phy_index++];
            phy_type = 0;
            if (log_info)
                printf_buf(&first_phy->Scan_Type, 5);
            break;
        case 4:
            if (log_info)
                log_print(2, NULL, "[Info]: [LE5_BB]LE_1M_CODED_PHY\r\n");
            first_phy = &scan->ext_scan->ext_scan_param.scan_phy_param[phy_index++];
            phy_type = 2;
            if (log_info)
                printf_buf(&first_phy->Scan_Type, 5);
            break;
        default:
            break;
        }
    }
    ext = scan->ext_scan;
    legacy_scan.filter_duplicates = ext->ext_scan_enable.Filter_Duplicates;
    legacy_scan.filter_policy = ext->ext_scan_param.Scanning_Filter_Policy;
    legacy_scan.type = first_phy->Scan_Type;
    memcpy(&legacy_scan.interval, &first_phy->Scan_Interval, sizeof(u16));
    memcpy(&legacy_scan.window, &first_phy->Scan_Window, sizeof(u16));
    if (log_info) {
        log_print(2, NULL, "[Info]: [LE5_BB]filter_duplicates=0x%x\n\r\n", legacy_scan.filter_duplicates);
        log_print(2, NULL, "[Info]: [LE5_BB]filter_policy=0x%x\n\r\n", legacy_scan.filter_policy);
        log_print(2, NULL, "[Info]: [LE5_BB]legacy_scan.type=0x%x\n\r\n", legacy_scan.type);
        log_print(2, NULL, "[Info]: [LE5_BB]legacy_scan.interval=0x%x\n\r\n", legacy_scan.interval);
        log_print(2, NULL, "[Info]: [LE5_BB]legacy_scan.window=0x%x\n\r\n", legacy_scan.window);
    }
    __set_ble_hw_timeout_irq_init(legacy_hw);
    __set_ble4_rx_phy_compens(legacy_hw);
    __set_data_rx_phy(legacy_hw, phy_type);
    le_hw_scanning_common(legacy_hw, &legacy_scan);
    __le_ext_legacy_scan_build_hw(legacy_hw);
    ble_hw_enable(legacy_hw, 1);
}

BLE5_CODE static void le_hw_ext_initiating(struct ble_hw *hw, struct ble_hw *primary_hw,
                                 struct ble_ext_init *init) {
    if (!(config_btctler_le_roles & 1))
        return;
    if (log_tag_const_i_LE5_BB)
        log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_initiating");
    __aux_connect_req(hw, init);
    __ext_tx_loop_buffer_hw_build(hw->ble5_hw);
    hw->phy.coded_s = init->coded_s;
    __le_ext_master_build_hw(hw, 5);
    le_ext_primary_init_build(primary_hw, init);
}

BLE5_CODE static void le_hw_ext_advertising_ctrl(struct ble_hw *hw, int ctrl, ...) {
    va_list ap;
    va_start(ap, ctrl);
    switch (ctrl) {
    case 0: {
        struct ble_ext_adv *adv = va_arg(ap, struct ble_ext_adv *);
        if (!(config_btctler_le_roles & 2)) break;
        bool log_info = log_tag_const_i_LE5_BB != 0;
        if (log_info)
            log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_primary_adv_state");
        struct ble5_hw *ble5 = hw->ble5_hw;
        ble5->ble5_fp.EXT_ADVIDX = (adv->pkt_cnt << 14) | adv->pdu_interval;
        __set_interval(hw, adv->interval * 625, 1);
        ble5->secondary_event.max_primary_events_skip = adv->secondary_max_skip;
        ble5->ext_event.max_ext_events = adv->max_ext_adv_events;
        ble5->ext_event.duration = adv->duration;
        __set_ext_adv_channel_map(hw);
        __set_adv_channel_index(hw, adv->channel_map);
        __set_ext_adv_channel_hop(hw, 0, 0, 0);
        __le_ext_filter_init(hw, 0);
        __set_ext_adv_other_hw(hw);
        __set_ext_adv_aa_crcinit(hw);
        hw->ble5_hw->ble5_fp.EXT_WIDEN1 &= (u32)-1024;
        __set_ext_primary_or_period_adv_phy(hw, adv->primary_phy);
        __set_ble5_aux_tx_phy_compens(hw, adv->primary_phy, adv->secondary_phy);
        __set_ext_hw_state(hw, 0);
        __set_ext_mode_enable(hw, 1);
        __set_ble_hw_irq_init(hw);
        u8 primary_payload_len = __adv_ext_pdu(hw, adv);
        ble5 = hw->ble5_hw;
        ble5->chain_adv_total_nums = ble5->chain_adv_cnt;
        __ext_adv_tx_loop_buffer_alloc_total(ble5);
        __ext_tx_loop_buffer_hw_build(hw->ble5_hw);
        u32 skip_gap = (u32)(adv->interval * 625) * hw->ble5_hw->secondary_event.max_primary_events_skip;
        u32 packet_len = le_get_ext_adv_packet_len_us((u32)primary_payload_len + 2,
                                                       adv->primary_phy, adv->coded_s);
        u32 primary_packets_gap = ((u32)adv->pkt_cnt - 1U) * adv->pdu_interval + packet_len;
        u32 gap_us = skip_gap + 6750 + primary_packets_gap;
        adv->secondary_anchor_gap_slot = gap_us / 625 + 1;
        if (log_info) {
            log_print(2, NULL, "[Info]: [LE5_BB]adv->interval=%u\r\n", adv->interval);
            log_print(2, NULL, "[Info]: [LE5_BB]primary_adv_packet_len_us=%u\r\n", packet_len);
            log_print(2, NULL, "[Info]: [LE5_BB]1 :EXT_PRIMARY_ADV_FIRST_ANCHOR_SLOT=%d slots\r\n", 10);
            log_print(2, NULL, "[Info]: [LE5_BB]2 :skip_gap_us=%u us\r\n", skip_gap);
            log_print(2, NULL, "[Info]: [LE5_BB]3 :primary_adv_packets_gap_us=%u us\r\n", primary_packets_gap);
            log_print(2, NULL, "[Info]: [LE5_BB]4 :T_PRIMARY_TO_SECONDARY_AFS_MARGIN_US=%u us\r\n", 500);
            log_print(2, NULL, "[Info]: [LE5_BB]1+2+3+4 :secondary_adv_anchor_gap_us=%u us\r\n", gap_us);
            log_print(2, NULL, "[Info]: [LE5_BB]1+2+3+4 :secondary_anchor_gap_slot=%d slots\r\n", adv->secondary_anchor_gap_slot);
        }
        break;
    }
    case 1: {
        struct ble_ext_adv *adv = va_arg(ap, struct ble_ext_adv *);
        if (!(config_btctler_le_roles & 2)) break;
        bool log_info = log_tag_const_i_LE5_BB != 0;
        if (log_info)
            log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_secondary_adv_state");
        struct ble5_hw *ble5 = hw->ble5_hw;
        struct ble_hw *period_hw = hw->ble5_ext->correlated_period_hw;
        __set_ble_anchor_con(hw, 1, 32767);
        __set_ext_adv_channel_map(hw);
        __set_ext_adv_channel_hop(hw, 1, 0, 1);
        __le_ext_filter_init(hw, 1);
        __disable_adv_random(hw);
        struct ble5_hw *regs_hw = hw->ble5_hw;
        regs_hw->ble5_fp.EXT_WINCNTL0 = 550;
        regs_hw->ble5_fp.EXT_WINCNTL1 = 0;
        regs_hw->ble5_fp.EXT_WINCNTL2 = 550;
        __set_ext_adv_other_hw(hw);
        __set_ext_adv_aa_crcinit(hw);
        hw->ble5_hw->ble5_fp.EXT_WIDEN1 &= (u32)-1024;
        if (period_hw) {
            if (config_btctler_le_features & (1ULL << 13)) {
                __set_ext_period_channel_map(period_hw, adv->sync_info.chm);
                __set_ext_period_adv_aa_crcinit(period_hw, (u8 *)&adv->sync_info.aa,
                                                adv->sync_info.crcinit);
                __set_interval(period_hw, adv->sync_info.interval * 1250, 0);
                __ext_kickstart_next_hw(ble5, HW_ID(period_hw), 1);
                __set_ble5_aux_tx_phy_compens(hw, adv->secondary_phy,
                                              adv->secondary_phy);
            }
        } else {
            __ext_kickstart_next_hw(ble5, HW_ID(hw), 0);
            __set_ble5_tx_phy_compens(hw);
            __set_ble5_ifs_phy_compens(hw);
            hw->phy.tx_phy = adv->secondary_phy;
            hw->phy.rx_phy = adv->secondary_phy;
            hw->phy.coded_s = adv->coded_s;
        }
        u32 state;
        switch (adv->event_type) {
        case 2: case 4: state = 2; break;
        case 5: case 6: state = 4; break;
        case 7: case 8: state = 6; break;
        default: state = 0; break;
        }
        if (log_info)
            log_print(2, NULL, "[Info]: [LE5_BB]format state=0x%x\n\r\n", state);
        if (state == 4)
            __set_ext_hw_rx(hw, 37);
        else if (state == 6)
            __set_ext_hw_rx(hw, 15);
        __set_ext_hw_state(hw, (u8)state);
        __set_ext_mode_enable(hw, 1);
        __set_ble_hw_irq_init(hw);
        __aux_adv_pdu(hw, adv);
        ble5 = hw->ble5_hw;
        ble5->chain_adv_total_nums = ble5->chain_adv_cnt;
        __ext_adv_tx_loop_buffer_data_build(ble5);
        __ext_tx_loop_buffer_hw_build(hw->ble5_hw);
        hw->ble5_hw->secondary_event.anchor_point_gap_slot = adv->secondary_anchor_gap_slot;
        break;
    }
    case 2: {
        struct ble_ext_period_adv *period = va_arg(ap, struct ble_ext_period_adv *);
        if (!(config_btctler_le_roles & 2)) break;
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_periodic_adv_state");
        struct ble5_hw *ble5 = hw->ble5_hw;
        __set_ext_adv_channel_map(hw);
        __set_ext_adv_channel_hop(hw, 1, 1, 1);
        __ext_kickstart_next_hw(ble5, HW_ID(hw), 0);
        __set_ext_primary_or_period_adv_phy(hw, period->chain_phy);
        __set_ble5_tx_phy_compens(hw);
        __set_ext_adv_other_hw(hw);
        hw->ble5_hw->ble5_fp.EXT_WIDEN1 &= (u32)-1024;
        __set_ext_hw_state(hw, 2);
        hw->ble5_hw->soft_state = 5;
        __set_ext_mode_enable(hw, 1);
        __set_ble_hw_irq_init(hw);
        __aux_sync_pdu(hw, period);
        ble5 = hw->ble5_hw;
        ble5->chain_adv_total_nums = ble5->chain_adv_cnt;
        __ext_adv_tx_loop_buffer_data_build(ble5);
        __ext_tx_loop_buffer_hw_build(hw->ble5_hw);
        break;
    }
    case 3:
        le_hw_ext_primary_adv_switch(hw, (u8)va_arg(ap, int));
        break;
    case 4:
        le_hw_ext_secondary_adv_switch(hw, (u8)va_arg(ap, int));
        break;
    case 5:
        le_hw_ext_advertising_switch(hw, (u8)va_arg(ap, int), 10);
        break;
    case 6: {
        struct ble_hw *correlated = va_arg(ap, struct ble_hw *);
        struct ble5_extended *ext = hw->ble5_ext;
        if ( !ext) {
            if (log_tag_const_i_LE5_BB)
                log_print(2, NULL, "[Info]: [LE5_BB]__set_ext_correlated_hw alloc ble5_ext\r\n");
            ext = btctler_nv_memory_malloc(32);
            hw->ble5_ext = ext;
        }
        ext->correlated_hw = correlated;
        break;
    }
    case 7:
        hw->ble5_ext->correlated_period_hw = va_arg(ap, struct ble_hw *);
        break;
    case 8: {
        struct ble_ext_adv *adv = va_arg(ap, struct ble_ext_adv *);
        struct ble_ext_period_adv *period = va_arg(ap, struct ble_ext_period_adv *);
        if (!(config_btctler_le_roles & 2)) break;
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]--func=%s\r\n", "le_hw_ext_adv_data_update");
        struct ble5_hw *ble5 = hw->ble5_hw;
        ble5->ext_tx_head_index = (u8)(ble5->ext_tx_head_index == 0);
        (void)__adv_ext_pdu(hw, adv);
        hw->ble5_hw->update_adv_data = 1;
        struct ble_hw *secondary = hw->ble5_ext->correlated_hw;
        if (!secondary) break;
        struct ble5_hw *secondary_ble5 = secondary->ble5_hw;
        secondary_ble5->ext_tx_head_index = (u8)(secondary_ble5->ext_tx_head_index == 0);
        __aux_adv_pdu(secondary, adv);
        secondary->ble5_hw->update_adv_data = 1;
        struct ble_hw *period_hw = secondary->ble5_ext->correlated_period_hw;
        if (!period_hw) break;
        struct ble5_hw *period_ble5 = period_hw->ble5_hw;
        period_ble5->ext_tx_head_index = (u8)(period_ble5->ext_tx_head_index == 0);
        __aux_sync_pdu(period_hw, period);
        period_hw->ble5_hw->update_adv_data = 1;
        break;
    }
    default:
        break;
    }
    va_end(ap);
}

BLE5_CODE static void le_hw_ext_ioctrl(struct ble_hw *hw, int ctrl, ...) {
    va_list ap;
    va_start(ap, ctrl);
    switch (ctrl) {
    case 9: {
        int addr_type = va_arg(ap, int);
        const u8 *addr = va_arg(ap, const u8 *);
        struct ble5_hw *ble5 = hw->ble5_hw;
        if (log_tag_const_i_LE5_BB)
            log_print(2, NULL, "[Info]: [LE5_BB]ext Local MAC\r\n");
        ble5->ble5_fp.EXT_AUXCTL1 = (ble5->ble5_fp.EXT_AUXCTL1 & (u32)-17) |
                                     (((u16)addr_type << 4) & 16);
        ble5->ble5_fp.EXT_LOCALADRL = (addr[1] << 8) | addr[0];
        ble5->ble5_fp.EXT_LOCALADRM = (addr[3] << 8) | addr[2];
        ble5->ble5_fp.EXT_LOCALADRU = (addr[5] << 8) | addr[4];
        ble5->ble5_fp.EXT_TARGETADRL = 0;
        ble5->ble5_fp.EXT_TARGETADRM = 0;
        ble5->ble5_fp.EXT_TARGETADRU = 0;
        break;
    }
    case 10: {
        int enable = va_arg(ap, int);
        const u8 *addr = va_arg(ap, const u8 *);
        struct ble5_hw *ble5 = hw->ble5_hw;
        ble5->ble5_fp.EXT_AUXCTL0 = (ble5->ble5_fp.EXT_AUXCTL0 & (u32)-2049) |
                                     (((u8)enable != 0) << 11);
        ble5->ble5_fp.EXT_WLISTL = (addr[1] << 8) | addr[0];
        ble5->ble5_fp.EXT_WLISTM = (addr[3] << 8) | addr[2];
        ble5->ble5_fp.EXT_WLISTU = (addr[5] << 8) | addr[4];
        ble5->ble5_fp.EXT_AUXCTL0 |= 1024;
        break;
    }
    case 11: {
        int result = hw_dma_empty(&hw->ble5_ext->ext_rx_head);
        int *empty = va_arg(ap, int *);
        *empty = result;
        break;
    }
    case 12: {
        int flag = va_arg(ap, int);
        const u8 *header = va_arg(ap, const u8 *);
        int member = va_arg(ap, int);
        u8 *buffer = va_arg(ap, u8 *);
        __ext_header_resolve((u8)flag, header, (u8)member, buffer);
        break;
    }
    case 13: {
        struct __SyncInfo_field *sync = va_arg(ap, struct __SyncInfo_field *);
        __set_ext_aux_event_hold_ir(hw, 0);
        __set_ext_period_channel_map(hw, sync->chmapl);
        __set_ext_period_adv_aa_crcinit(hw, sync->aa, sync->crcinit);
        __set_ext_adv_channel_hop(hw, 1, 1, 1);
        u8 phy = __get_ext_secondary_phy(hw);
        u32 interval = ((((u32)(((sync->chmaph | (sync->sca << 5)) >> 5) + 500) *
                           sync->interval) / 800) + (phy == 2 ? 381 : 1)) & 65535;
        u32 rem = interval % 625;
        bool rounded = rem > 487;
        struct ble5_hw *ble5 = hw->ble5_hw;
        ble5->ble5_fp.EXT_WIDEN0 = (u16)(rounded + interval / 625);
        ble5->ble5_fp.EXT_WIDEN1 = rounded ? 20480 : ((u16)rem | 20480);
        ble5->ble5_fp.EXT_AUXCTL0 &= (u32)-33;
        break;
    }
    case 14: {
        __set_anchor_cnt_disable_with_default(hw);
        __set_ext_adv_channel_hop(hw, 1, 0, 1);
        __set_ext_adv_aa_crcinit(hw);
        struct ble5_hw *ble5 = hw->ble5_hw;
        ble5->ble5_fp.EXT_AUXCTL0 |= 32;
        ble5->ble5_fp.EXT_WIDEN0 = 0;
        ble5->ble5_fp.EXT_WIDEN1 = 20480;
        __set_ext_aux_event_hold_ir(hw->ble5_ext->correlated_hw, 0);
        hw->ble5_hw->soft_state = 0;
        break;
    }
    case 15:
        if (config_btctler_le_roles & 2)
            set_connecting_data_phy(hw);
        break;
    case 16:
        if (config_btctler_le_roles & 1) {
            hw->ble4_hw->state = 3;
            set_connecting_data_phy(hw);
        }
        break;
    case 18: {
        u8 *tx_phy = va_arg(ap, u8 *);
        u8 *rx_phy = va_arg(ap, u8 *);
        if (!(config_btctler_le_features & 2304)) break;
        if (tx_phy) *tx_phy = hw->phy.tx_phy;
        if (rx_phy) *rx_phy = hw->phy.rx_phy;
        break;
    }
    case 19: {
        int air_tx = va_arg(ap, int);
        int air_rx = va_arg(ap, int);
        int coded_s = va_arg(ap, int);
        if (!(config_btctler_le_features & 2304)) break;
        hw->phy.coded_s = (u8)coded_s;
        u8 tx_phy = exchange_AirPhyParam_2_HardwarePhyParam((u8)air_tx);
        if (tx_phy == 4) tx_phy = hw->phy.tx_phy;
        u8 rx_phy = exchange_AirPhyParam_2_HardwarePhyParam((u8)air_rx);
        if (rx_phy == 4) rx_phy = hw->phy.rx_phy;
        if (tx_phy == 2)
            __set_data_tx_phy_coded_s(hw);
        struct ble4_hw *hw4 = hw->ble4_hw;
        if (rx_phy == 2 && hw->phy.rx_phy != 2) {
            if (hw4->state == 7) {
                __set_winsize(hw4, 400, hw->phy.widen + 736);
                __set_widen(hw->ble4_hw, hw->phy.widen + 476);
            } else {
                __set_winsize(hw4, 400, 0);
            }
        } else if (rx_phy != 2 && hw->phy.rx_phy == 2) {
            if (hw4->state == 7) {
                __set_winsize(hw4, 80, hw->phy.widen + 667);
                __set_widen(hw->ble4_hw, hw->phy.widen + 30);
            } else {
                __set_winsize(hw4, 50, 0);
            }
        }
        __set_ble4_ifs_phy_compens(hw);
        hw4 = hw->ble4_hw;
        if (rx_phy == 2) {
            switch (tx_phy) {
            case 0:
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-128) | 13;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-16257) | 1664;
                break;
            case 1:
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-128) | 12;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-16257) | 1536;
                break;
            case 2:
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-128) | 20;
                hw4->ble_fp.EXTIFSDRF0 = (hw4->ble_fp.EXTIFSDRF0 & (u32)-16257) | 2560;
                break;
            }
        } else if (rx_phy == 1) {
            if (tx_phy == 0) {
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF1 &= (u32)-128;
            } else if (tx_phy == 1) {
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF1 |= 127;
            } else if (tx_phy == 2) {
                hw4->ble_fp.IFSCNT = (u16)-32107;
                hw4->ble_fp.EXTIFSDRF1 = (hw4->ble_fp.EXTIFSDRF1 & (u32)-128) | 7;
            }
        } else if (tx_phy == 0) {
            hw4->ble_fp.IFSCNT = (u16)-32107;
        } else if (tx_phy == 1) {
            hw4->ble_fp.IFSCNT = (u16)-31851;
        } else if (tx_phy == 2) {
            hw4->ble_fp.IFSCNT = 31381;
        }
        hw4->ble_fp.EXTPKTDRF = 0;
        __set_data_tx_phy(hw, tx_phy);
        __set_data_rx_phy(hw, rx_phy);
        hw->phy.tx_phy = tx_phy;
        hw->phy.rx_phy = rx_phy;
        __set_conn_dead_times(hw);
        break;
    }
    default:
        break;
    }
    va_end(ap);
}

#ifdef UNITTEST
#include "test.h"
#include "../../../tools/btctrler/rf_ble5_test_mocks.c"

const u8 log_tag_const_e_LE5_BB = 0;
const u8 log_tag_const_i_LE5_BB = 0;

u64 config_btctler_le_features;
int config_btctler_le_roles;
static u32 anchor_value, anchor_written;
static u8 anchor_index;
static unsigned queued_rx, freed_rx;
static struct ble_ext_rx *last_queued_rx;
static struct ble_hw *anchor_replace_hw;
static struct ble5_hw *anchor_replacement;
static struct ble_hw *map_hw;
static struct ble5_hw *map_replacement[2];
u32 __get_ble_anchor_con(struct ble_hw *hw, u8 index)
{
    (void)hw; anchor_index = index; return anchor_value;
}
void __set_ble_anchor_con(struct ble_hw *hw, u8 index, u32 value)
{
    (void)hw; anchor_index = index; anchor_written = value;
    if (anchor_replace_hw)
        anchor_replace_hw->ble5_hw = anchor_replacement;
}
void hw_dma_push(struct _dma_head *head, struct _dma *dma)
{
    (void)head; last_queued_rx = (void *)dma; ++queued_rx;
}
void hw_dma_free(void *dma) { (void)dma; ++freed_rx; }
u16 btctler_little_endian_read_16(const u8 *data, int pos)
{
    if (map_hw) map_hw->ble5_hw = map_replacement[pos / 2];
    return data[pos] | ((u16)data[pos + 1] << 8);
}


int main(void)
{
    /* The raw table initializer is 01 06 02 06 08 02 10 03 20 12 40 01. */
    static const u8 raw_ext_header_table[6][2] = {
        {1, 6}, {2, 6}, {8, 2}, {16, 3}, {32, 18}, {64, 1},
    };
    CHECK(memcmp(ext_header_table, raw_ext_header_table,
                 sizeof(raw_ext_header_table)) == 0);
    CHECK(aux_data_channel == 0xff);
    CHECK(__ble5_ops == &ble5_ops);
    CHECK(ble5_ops.ext_open != NULL && ble5_ops.ext_close != NULL &&
          ble5_ops.ext_scanning != NULL && ble5_ops.ext_initiating != NULL);

    /* Packet timing formulas are translated from the three switch arms in
     * le_get_ext_adv_packet_len_us (raw IR, PHY cases 0, 1, and 2). */
    CHECK(le_get_ext_adv_packet_len_us(10, 0, 0) == 144);
    CHECK(le_get_ext_adv_packet_len_us(11, 1, 0) == 80);
    CHECK(le_get_ext_adv_packet_len_us(10, 2, 0) == 1232);
    CHECK(le_get_ext_adv_packet_len_us(10, 2, 1) == 590);
    CHECK(le_get_ext_adv_packet_len_us(10, 7, 1) == 0);
    CHECK(le_get_ext_adv_packet_len_us(0x20000000U, 1, 0) == 36);
    CHECK(le_get_ext_adv_packet_len_us(0xffffffffU, 1, 0) == 0x80000020U);

    /* Ext-header fields are packed in table order. ADI follows AdvA. */
    const u8 header[8] = {0, 1, 2, 3, 4, 5, 0x34, 0x12};
    u8 resolved[6] = {0};
    __ext_header_resolve(9, header, 8, resolved);
    CHECK(resolved[0] == 0x34 && resolved[1] == 0x12);
    memset(resolved, 0xa5, sizeof(resolved));
    __ext_header_resolve(9, header, 2, resolved);
    CHECK(resolved[0] == 0xa5);

    /* Circular write/read wrap at the end of a five-byte buffer. */
    struct __loop_cbuf cb;
    u8 ring[5], out[4] = {0};
    static const u8 first[4] = {1, 2, 3, 4};
    static const u8 wrapped[3] = {5, 6, 7};
    static const u8 expected[4] = {4, 5, 6, 7};
    loop_cbuf_init(&cb, ring, sizeof(ring));
    CHECK(loop_cbuf_write(&cb, first, sizeof(first)) == 4);
    loop_cbuf_read(&cb, out, 3);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && cb.can_read_len == 1);
    CHECK(loop_cbuf_write(&cb, wrapped, sizeof(wrapped)) == 3);
    loop_cbuf_read(&cb, out, sizeof(out));
    CHECK(memcmp(out, expected, sizeof(expected)) == 0 && cb.can_read_len == 0);

    /* Nonwrapping operations advance the old cursor, not the buffer base. */
    u8 longer_ring[16], successive[8] = {0};
    loop_cbuf_init(&cb, longer_ring, sizeof(longer_ring));
    CHECK(loop_cbuf_write(&cb, first, 4) == 4);
    CHECK(loop_cbuf_write(&cb, wrapped, 3) == 3);
    CHECK(cb.write_ptr == longer_ring + 7);
    loop_cbuf_read(&cb, successive, 2);
    loop_cbuf_read(&cb, successive + 2, 2);
    CHECK(cb.read_ptr == longer_ring + 4);
    CHECK(memcmp(successive, first, 4) == 0);
    loop_cbuf_read(&cb, successive + 4, 3);
    CHECK(memcmp(successive + 4, wrapped, 3) == 0);
    CHECK(cb.read_ptr == longer_ring + 7 && cb.can_read_len == 0);

    /* A rejected write clears the available tail as the SDK does, but leaves
     * its read/write cursors and byte count unchanged. */
    loop_cbuf_init(&cb, ring, sizeof(ring));
    CHECK(loop_cbuf_write(&cb, first, sizeof(first)) == 4);
    u8 *old_read = cb.read_ptr;
    u8 *old_write = cb.write_ptr;
    CHECK(loop_cbuf_write(&cb, wrapped, 2) == 0);
    CHECK(cb.read_ptr == old_read && cb.write_ptr == old_write &&
          cb.can_read_len == 4 && ring[4] == 0);

    struct ble_hw test_hw = {0};
    struct ble4_hw hw4 = {0};
    struct ble5_hw hw5 = {0}, replacement = {0};
    struct ble5_extended ext = {0};
    test_hw.ble4_hw = &hw4; test_hw.ble5_hw = &hw5;
    test_hw.ble5_ext = &ext;
    /* -1793 and -14337 clear their field, preserving bit zero. */
    hw4.ble_fp.INTFRAME = 0xffff;
    __set_data_tx_phy(&test_hw, 0);
    CHECK(hw4.ble_fp.INTFRAME == 0xf8ff);
    __set_data_rx_phy(&test_hw, 0);
    CHECK(hw4.ble_fp.INTFRAME == 0xc0ff);
    hw4.ble_fp.EXTIFSDRF0 = hw4.ble_fp.EXTIFSDRF1 = 0xffff;
    test_hw.phy.coded_s = 1;
    __set_ble4_ifs_phy_compens(&test_hw);
    CHECK(hw4.ble_fp.EXTIFSDRF0 == 0xca14);
    CHECK(hw4.ble_fp.EXTIFSDRF1 == 0x87ff);
    hw5.ble5_fp.EXT_IFSDRF0 = hw5.ble5_fp.EXT_IFSDRF1 = 0xffff;
    __set_ble5_ifs_phy_compens(&test_hw);
    CHECK(hw5.ble5_fp.EXT_IFSDRF0 == 0xca14);
    CHECK(hw5.ble5_fp.EXT_IFSDRF1 == 0xc4ff);
    anchor_value = 0xffff;
    __set_ext_hw_state(&test_hw, 0);
    CHECK(anchor_written == 0xffe1 && anchor_index == 10);
    __set_ext_adv_channel_hop(&test_hw, 0, 0, 0);
    CHECK((anchor_written & 1) == 1);
    __set_ext_aux_event_hold_ir(&test_hw, 2);
    CHECK(anchor_written == 0x7fff && anchor_index == 2);
    anchor_value = 0x8000;
    CHECK(get_if_aux_enable(&test_hw) == 1);

    /* Positive-length RX packets enter the DMA queue; zero-length packets
     * bypass the queue and are freed after the optional callback. */
    struct ble_ext_rx rx = {0};
    rx.len = 1;
    ext_rx_probe_handler(&test_hw, &rx);
    CHECK(queued_rx == 1 && freed_rx == 0);
    rx.len = 0;
    ext_rx_probe_handler(&test_hw, &rx);
    CHECK(queued_rx == 1 && freed_rx == 1);

    /* Each endian helper may replace the hardware frame. The following
     * channel-map stores must target that newly returned frame. */
    const u8 map[5] = {1, 2, 3, 4, 5};
    map_hw = &test_hw;
    map_replacement[0] = &hw5; map_replacement[1] = &replacement;
    __set_ext_period_channel_map(&test_hw, (u8 *)map);
    CHECK(hw5.ble5_fp.EXT_CHMAP0 == 0x0201);
    CHECK(hw5.ble5_fp.EXT_CHMAP1 == 0);
    CHECK(replacement.ble5_fp.EXT_CHMAP1 == 0x0403);
    CHECK(replacement.ble5_fp.EXT_CHMAP2 == (5 | (7 << 5)));
    map_hw = NULL;

    /* SDK event soft-state15 reads AUXCTL1 (not AUXCTL0), sets anchor bit10
     * only when bit12 is clear, then reloads the hardware pointer. */
    test_hw.ble5_hw = &hw5;
    config_btctler_le_features = (1ULL << 12) | (1ULL << 13);
    config_btctler_le_roles = 1;
    hw5.soft_state = 15;
    hw5.scan_rx_state = 1;
    hw5.ble5_fp.EXT_AUXCTL0 = 4096;
    hw5.ble5_fp.EXT_AUXCTL1 = 0;
    replacement.scan_rx_state = 2;
    anchor_value = 0x5a;
    anchor_replace_hw = &test_hw;
    anchor_replacement = &replacement;
    __ble5_hw_event_process(&test_hw);
    CHECK(anchor_written == 0x45a && anchor_index == 10);
    CHECK(replacement.scan_rx_state == 0 && hw5.scan_rx_state == 1);
    anchor_replace_hw = NULL;
    test_hw.ble5_hw = &hw5;
    hw5.scan_rx_state = 2;
    hw5.ble5_fp.EXT_AUXCTL1 = 4096;
    anchor_written = 0x12345678;
    __ble5_hw_event_process(&test_hw);
    CHECK(hw5.scan_rx_state == 0 && anchor_written == 0x12345678);

    /* Raw RX snapshots the two-byte header, reads its extra status byte,
     * samples RSSI, then follows ext->correlated_hw when disabling an anchor.
     * Static DMA storage stays representable by the SDK's 32-bit addresses. */
    static u8 dma_ring[8] = {7, 1, 0x40, 0};
    struct ble_hw correlated = {0};
    test_hw.ble5_hw = &hw5;
    ext.correlated_hw = &correlated;
    hw5.hw_state = 5;
    hw5.soft_state = 0;
    hw5.loop_buf_rx_len = sizeof(dma_ring);
    loop_cbuf_init(&hw5.loop_cbuf_rx, dma_ring, sizeof(dma_ring));
    rf_ble5_mock_ble_to_phy_result = (u32)(uintptr_t)(dma_ring + 4);
    rf_ble5_mock_rssi_value = -42;
    rf_ble5_mock_rssi_calls = 0;
    rf_ble5_mock_anchor_disable_calls = 0;
    anchor_value = 1;
    unsigned before_rx = queued_rx;
    __ble5_hw_rx_process(&test_hw);
    CHECK(queued_rx == before_rx + 1 && hw5.loop_cbuf_rx.can_read_len == 0);
    CHECK(rf_ble5_mock_rssi_calls == 1 && rf_ble5_mock_rssi_frame == &hw5.ble5_fp);
    CHECK((s8)last_queued_rx->rssi == -42 && last_queued_rx->data[0] == 0x40);
    CHECK(rf_ble5_mock_anchor_disable_calls == 1 &&
          rf_ble5_mock_anchor_disable_hw == &correlated);
    CHECK(test_hw.phy.tx_phy == 1 && test_hw.phy.rx_phy == 1 && hw5.soft_state == 12);
    free(last_queued_rx);

    assign_aux_data_channel(6);
    CHECK(aux_data_channel == 6);
    return TEST_DONE("btctrler_RF_ble5");
}
#endif
