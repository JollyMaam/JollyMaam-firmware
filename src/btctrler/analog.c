#ifdef UNITTEST
#define _GNU_SOURCE
#endif
/* Classic RF power tables and PLL setup. Keep the SDK's lookup-table
 * selection, bank arithmetic, direct volatile MMIO order, and PLL constants. */
#include "btctrler/glue.h"
#include <string.h>
#include <stdio.h>
#ifdef UNITTEST
#include <sys/mman.h>
#endif

#define BT_RF_CODE __attribute__((section(".bt_rf_code")))
#define BT_RF_BSS __attribute__((section(".bt_rf_bss")))
#define BT_RF_DATA __attribute__((section(".bt_rf_data")))
#define BT_RF_CONST __attribute__((section(".bt_rf_const")))

extern BTCTRLER_CONFIG int config_force_bt_pwr_tab_using_normal_level;
extern BTCTRLER_CONFIG int config_btctler_single_carrier_en;
extern BTCTRLER_CONFIG int config_bt_temperature_pll_trim;
struct syscfg_operataions {
    int (*reserved0)(void);
    int (*check_id)(u16);
    int (*reserved2)(u16, u8 *, u16);
    int (*reserved3)(u16, u8 *, u16);
    int (*reserved4)(u16, u8 *, u16);
    int (*reserved5)(u16, u8 *, u16, u8);
    u8 *(*reserved6)(u16, u16 *);
};
extern struct syscfg_operataions *get_syscfg_btif_ops(void);
extern int syscfg_read(u16 key, u8 *data, u16 len);
extern int syscfg_write(u16 key, u8 *data, u16 len);
extern u16 CRC16(const u8 *data, u32 len);
extern int clk_get(const char *name);
extern void P33_CON_SET(u16 port, u8 value, u8 mask, u8 direction);
extern void delay(u32 us);
extern void power_reset_close(void);
extern u8 br23_fm_tx_mode(void);
extern u8 fm_tx_ch(void);
extern void clk_set_default_osc_cap(void);
extern void btagc_init(void);
extern void bit_clr_ie(u8 bit);
extern void bit_set_ie(u8 bit);
extern u8 get_idle_task_state(void);
extern void bredr_frame_suspend_control(u8 suspend);
extern void le_hw_all_event_hold_en(u8 hold);
extern const char agc_dbm_tlb[33];
extern u32 adc_sample(u32 select);
extern void adc_pmu_detect_en(int enable);
extern u32 adc_value_to_voltage(u32 vbg, u32 ldo);
extern u32 get_chip_version(void);

static void BT_RF_CODE bt_analog_part_init(void);

struct rf_analog_trim BT_RF_BSS trim_info;
struct ldo_trim_res BT_RF_BSS ldo_trim_res;
static s16 BT_RF_DATA rf_osc_offset_initial_val;
static void (* BT_RF_DATA bt_fre_offset_store_callback)(int, int);
static u8 BT_RF_BSS start_cap;
u16 BT_RF_BSS cap_freq_r[6][2];
static const u32 BT_RF_CONST bt_frac_pll_frac_48m[83] = {
    349525,1048576,1747627,2446677,3145728,3844779,4543829,5242880,
    5941931,6640981,7340032,8039083,8738133,9437184,10136235,10835285,
    11534336,12233387,12932437,13631488,14330539,15029589,15728640,16427691,
    349525,1048576,1747627,2446677,3145728,3844779,4543829,5242880,
    5941931,6640981,7340032,8039083,8738133,9437184,10136235,10835285,
    11534336,12233387,12932437,13631488,14330539,15029589,15728640,16427691,
    349525,1048576,1747627,2446677,3145728,3844779,4543829,5242880,
    5941931,6640981,7340032,8039083,8738133,9437184,10136235,10835285,
    11534336,12233387,12932437,13631488,14330539,15029589,15728640,16427691,
    349525,1048576,1747627,2446677,3145728,3844779,4543829,5242880,
    5941931,0,0
};
static const u8 BT_RF_CONST bt_frac_pll_int_48m[83] =
    "ddddddddddddddddddddddddeeeeeeeeeeeeeeeeeeeeeeeeffffffffffffffffffffffffggggggggg";

const u32 BT_RF_CONST TXPWR_table_pro[5] = {7348351,7348351,7348351,7348351,7348863};
const u32 BT_RF_CONST TXSET_table_pro[5] = {15732722,15745010,15794162,15990770,16777202};
const u32 BT_RF_CONST TXPWR_table[5] = {7348953,7348953,7348953,7348953,7348953};
const u32 BT_RF_CONST TXSET_table[5] = {15732674,15744962,15794114,15990722,16777154};
const u32 BT_RF_CONST TXPWR_table_fcc[10] = {
    7348905,7348905,7348905,7348905,7348905,7348905,7348905,7348905,7348905,7348905
};
const u32 BT_RF_CONST TXSET_table_fcc[10] = {
    15729602,15730626,15732674,15736770,15744962,
    15761346,15794114,15859650,15990722,16252866
};
const u32 BT_RF_CONST TXPWR_table_dut[7] = {
    7348905,7348905,7348905,7348905,7348905,7348905,7348905
};
const u32 BT_RF_CONST TXSET_table_dut[7] = {
    15729602,15730626,15736770,15744962,15794114,15990722,16777154
};
static u8 BT_RF_DATA analog_rx_control_rx_ok_cnt;
static u8 BT_RF_DATA analog_rx_control_rx_total_cnt;
static u16 BT_RF_DATA analog_rx_control_signal_intensity_ava;
volatile u8 BT_RF_BSS lna_g, dm_g, bp_g, tsel;

u8 BT_RF_CODE get_jl_chip_id(void) { return 6; }
u8 BT_RF_CODE get_jl_chip_id2(void) { return 6; }

extern const u8 log_tag_const_i_Analog;
extern const u8 log_tag_const_d_Analog;
extern const u8 log_tag_const_e_Analog;
extern void log_print(int level, void *format, const char *message, ...);
#define ANALOG_LOG_INFO(enabled, ...) do { if (enabled) log_print(2, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_LOW(enabled, ...) do { if (enabled) log_print(1, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_DEBUG(enabled, ...) do { if (enabled) log_print(2, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_ERROR(enabled, ...) do { if (enabled) log_print(4, NULL, __VA_ARGS__); } while (0)

void BT_RF_CODE reset_trim_info(void)
{
    u8 log_enabled = log_tag_const_i_Analog;
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\n -------- reset trim info -----------  \n\r\n");
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\ntxi_dc =%d txq_dc =%d\n\r\n",
                    (s16)trim_info.txi_dc, (s16)trim_info.txq_dc);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\ntxpuk1 =%d txpsk3 =%d\n\r\n",
                    (s16)trim_info.txpuk1, (s16)trim_info.txpsk3);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nrxi_cnt=%d rxq_cnt=%d\n\r\n",
                    (s16)trim_info.rxi_trim_cnt, (s16)trim_info.rxq_trim_cnt);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nrxpuk2 =%d rxpsk3 =%d\n\r\n",
                    (s16)trim_info.rxpuk2, (s16)trim_info.rxpsk3);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nchargpump_ldo_lev =%d divider_ldo_lev =%d\n\r\n",
                    ldo_trim_res.chargpump_ldo_lev, ldo_trim_res.divider_ldo_lev);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nvco_1ldo_lev =%d vco_2ldo_lev =%d\n\r\n",
                    ldo_trim_res.vco_1ldo_lev, ldo_trim_res.vco_2ldo_lev);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nrxadc_ldo_lev=%d rxadc_ref_lev=%d\n\r\n",
                    ldo_trim_res.rxadc_ldo_lev, ldo_trim_res.rxadc_ref_lev);
    ANALOG_LOG_INFO(log_enabled, "[Info]: [Analog]\nxosc_ldo_lev =%d umx_vblo_lev =%d\n\r\n",
                    ldo_trim_res.xosc_ldo_lev, ldo_trim_res.umx_vblo_lev);
    volatile u32 *r0 = (volatile u32 *)1899528;
    *r0 = *r0 & ~1023u;
    *r0 = *r0 | (trim_info.txi_dc & 1023u);
    *r0 = *r0 & ~1047552u;
    *r0 = *r0 | ((u32)(trim_info.txq_dc & 1023u) << 10);
    volatile u32 *r1 = (volatile u32 *)1899536;
    *r1 = *r1 & ~255u;
    *r1 = *r1 | (trim_info.txpuk1 & 255u);
    *r1 = *r1 & ~65280u;
    *r1 = *r1 | ((u32)trim_info.txpuk2 << 8);
    *r1 = *r1 & ~16711680u;
    *r1 = *r1 | ((u32)(trim_info.txpsk3 & 255u) << 16);
    volatile u32 *r2 = (volatile u32 *)1972484;
    *r2 = *r2 & ~255u;
    *r2 = *r2 | (trim_info.rxi_trim_cnt & 255u);
    *r2 = *r2 & ~130560u;
    *r2 = *r2 | ((u32)(trim_info.rxq_trim_cnt & 255u) << 9);
    volatile u32 *r3 = (volatile u32 *)1899540;
    *r3 = *r3 & ~255u;
    *r3 = *r3 | (trim_info.rxpuk2 & 255u);
    *r3 = *r3 & ~65280u;
    *r3 = *r3 | ((u32)trim_info.rxpsk3 << 8);
}

u8 BT_RF_CODE get_bt_osc_offset_flag(void)
{
    struct syscfg_operataions *ops = get_syscfg_btif_ops();
    int status = ops->check_id(110);
    return status == 0 ? 0 : rf.osc_offset_flag;
}

void BT_RF_CODE bt_osc_offset_set(int offset)
{
    if (rf.osc_offset_flag == 0) {
        int sum = (s16)rf.osc_offset + offset;
        rf.osc_offset = (s16)sum;
        ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]offset %d \n\r\n", (s16)sum);
    }
}

s16 BT_RF_CODE rf_osc_offset_initial_val_get(void)
{
    return rf_osc_offset_initial_val;
}

void BT_RF_CODE bt_osc_offset_ext_updata(int offset)
{
    s16 value = (s16)offset;
    rf.osc_offset = value;
    rf_osc_offset_initial_val = value;
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]updata offset %d \n\r\n", (s16)offset);
    bredr_pll_comp_ext_reset();
}

void BT_RF_CODE bt_fre_offset_store_handle_register(void (*handle)(int, int))
{
    bt_fre_offset_store_callback = handle;
}

void BT_RF_CODE bt_osc_offset_ext_save(int offset)
{
    struct osc_offset_t osc;
    rf.osc_offset = (s16)offset;
    osc.offset = (s16)offset;
    osc.crc = CRC16((u8 *)&osc.offset, 4);
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]save offset %d \n\r\n", (s16)rf.osc_offset);
    int result = syscfg_write(110, (u8 *)&osc, 6);
    void (*callback)(int, int) = bt_fre_offset_store_callback;
    if (callback)
        callback(offset, result);
    bredr_pll_comp_ext_reset();
}

int BT_RF_CODE bt_fre_offset_get(void)
{
    return (s16)rf.osc_offset;
}

void BT_RF_CODE bt_osc_offset_save(void)
{
    struct osc_offset_t osc;
    if (rf.osc_offset_flag != 0)
        return;
    osc.offset = (s16)rf.osc_offset;
    osc.crc = CRC16((u8 *)&osc.offset, 4);
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]save offset %d \n\r\n", (s16)rf.osc_offset);
    (void)syscfg_write(110, (u8 *)&osc, 6);
}

void BT_RF_CODE bt_fre_offset_value_init(void)
{
    struct osc_offset_t osc;
    rf.osc_offset = rf_osc_offset_initial_val;
    rf.osc_offset_flag = 0;
    int result = syscfg_read(110, (u8 *)&osc, 6);
    if (((u32)result & 255u) == 6u && osc.crc == CRC16((u8 *)&osc.offset, 4)) {
        u32 value = (u32)osc.offset;
        rf.osc_offset = (s16)value;
        rf.osc_offset_flag = 1;
        if ((u32)((value << 16) + 13107200u) > 26214400u)
            rf.osc_offset = 0;
    }
    ANALOG_LOG_INFO(log_tag_const_i_Analog,
                    "[Info]: [Analog]\n ---  fre_offset %d %d---- \n\r\n",
                    (s16)rf.osc_offset, rf.osc_offset_flag);
}

u8 BT_RF_CODE get_bta_pll_bank(u8 set_freq)
{
    for (u32 i = 1; i < 31; ++i) {
        if (rf.bta_pll_bank_limit[i] > set_freq)
            return (u8)(1u - i + rf.bta_pll_bank_limit[31]);
    }
    return 0;
}

static void BT_RF_CODE bta_pll_config_init(void)
{
    *(volatile u32 *)1899656 = (u32)(uintptr_t)rf.pll_config_tab;
    for (u32 i = 0; i < 81; ++i) {
        u8 bank = get_bta_pll_bank((u8)(i + 79));
        u32 frac = bt_frac_pll_frac_48m[i];
        u32 integer = bt_frac_pll_int_48m[i];
        u32 first = i * 3;
        if ((u32)(frac - 4194304u) > 8388607u) {
            rf.pll_config_tab[first] = ((u32)bank << 2) | 512u;
        } else {
            rf.pll_config_tab[first] = ((u32)bank << 2) | 1536u;
            if ((s32)frac < 8388608) {
                integer -= 1u;
                frac += 8388608u;
            } else {
                frac -= 8388608u;
            }
        }
        rf.pll_config_tab[first + 1] = (integer << 20) | ((u32)((s32)frac >> 4));
        rf.pll_config_tab[first + 2] = 150;
    }
}

void BT_RF_CODE bt_reload_trim(void)
{
    volatile u32 *reg = (volatile u32 *)1899528;
    *reg = *reg & ~1023u;
    *reg = *reg & ~1047552u;
    *reg = *reg | (trim_info.txi_dc & 1023u);
    *reg = *reg | ((u32)(trim_info.txq_dc & 1023u) << 10);
    *(volatile u32 *)1899532 = (u32)-2139062144;
    bta_pll_config_init();
}

void BT_RF_CODE bt_set_single_carrier(void)
{
    volatile u32 *reg = (volatile u32 *)1899528;
    *reg = *reg & ~1023u;
    *reg = *reg & ~1047552u;
    *reg = *reg | 768u;
    *reg = *reg | 786432u;
    *(volatile u32 *)1899532 = 0;
    for (u32 i = 0; i < 81; ++i) {
        u8 bank = get_bta_pll_bank((u8)(i + 79));
        u32 first = i * 3;
        rf.pll_config_tab[first] = (u32)bank << 2;
        rf.pll_config_tab[first + 1] =
            ((u32)bt_frac_pll_int_48m[i] << 20) |
            ((u32)((s32)(bt_frac_pll_frac_48m[i] - 349525u) >> 4));
        rf.pll_config_tab[first + 2] = 150;
    }
}

void BT_RF_CODE bt_analog_reset(void)
{
    u32 mode = (u32)config_btctler_mode;
    if ((mode & 1u) == 0)
        power_reset_close();
    if (mode & 2u) {
        ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]bqb reset \n\r\n");
        bredr_radiopwrupdn_reset();
    }
    if (mode & 4u) {
        ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]fcc reset \n\r\n");
        bredr_radiopwrupdn_reset();
        bt_set_fdv(100, 120, 100);
    }
}

void BT_RF_CODE RF_mdm_init(void)
{
    volatile u32 *r = (volatile u32 *)1899676;
    *r &= ~15u;
    *r |= 8u;
    r = (volatile u32 *)1899672;
    *r &= ~255u;
    *r |= 54u;
    *r &= ~16711680u;
    *r |= 3538944u;
    *r &= ~65280u;
    *r |= 512u;
    *r &= 16777215u;
    *r |= 33554432u;
    r = (volatile u32 *)1899676;
    *r &= ~16711680u;
    *r |= 3538944u;
    *r &= 16777215u;
    *r |= 905969664u;
    delay(10);
    r = (volatile u32 *)1899680;
    *r = *r & ~255u;
    *r = *r;
    *r = *r & ~65280u;
    *r = *r;
    *(volatile u32 *)1899592 = 185;
    *(volatile u32 *)1899520 = 83874116;
    *(volatile u32 *)1899524 = 1845509342;
    *(volatile u32 *)1899528 =
        (config_btctler_single_carrier_en == 0 || (config_btctler_mode & 33) == 0)
            ? 1048576u : 839909376u;
    *(volatile u32 *)1899532 = (u32)-2139062144;
    *(volatile u32 *)1899536 = (u32)-2147452808;
    *(volatile u32 *)1899540 = 274989184;
    *(volatile u32 *)1899544 = 0;
    *(volatile u32 *)1899548 = 0;
    *(volatile u32 *)1899552 = (u32)rf.fdv21 | ((u32)rf.fdv40 << 8) |
                                  ((u32)rf.fdv50 << 16) | 905969664u;
    *(volatile u32 *)1899556 = 2087559232;
    *(volatile u32 *)1899560 = 33933344;
}

void BT_RF_CODE bt_dut_reset_mdm(void)
{
    volatile u32 *r = (volatile u32 *)1899528;
    *r = *r & 16777215u;
    *r = *r;
}

void BT_RF_CODE certification(u8 tx_level_d)
{
    *(volatile u32 *)1899556 |= 0x80000000u;
    *(volatile u32 *)1899684 = 65536;
    volatile u32 *r = (volatile u32 *)1899536;
    *r &= ~65280u;
    *r &= ~255u;
    s32 txpuk2 = (s16)trim_info.txpuk2;
    s32 txpuk1 = (s16)trim_info.txpuk1;
    if (txpuk2 > txpuk1) {
        s32 ratio = (txpuk2 / txpuk1) * 100;
        s32 power = ((s32)tx_level_d * 100) / ratio;
        *r |= (u32)tx_level_d << 8;
        *r |= (u32)power & 255u;
        ANALOG_LOG_INFO(log_tag_const_i_Analog,
                        "[Info]: [Analog]puk1 : %d puk2 \xef\xbc\x9a%d \n\r\n",
                        (u32)power & 65535u, tx_level_d);
    } else {
        s32 ratio = (txpuk1 / txpuk2) * 100;
        s32 power = ((s32)tx_level_d * 100) / ratio;
        *r |= tx_level_d;
        *r |= ((u32)power << 8) & 65280u;
        ANALOG_LOG_INFO(log_tag_const_i_Analog,
                        "[Info]: [Analog]puk1 : %d puk2 \xef\xbc\x9a%d \n\r\n",
                        tx_level_d, (u32)power & 65535u);
    }
}

void BT_RF_CODE close_zeroif(void)
{
    *(volatile u32 *)1899600 |= 64u;
    *(volatile u32 *)1899556 &= 2147483647u;
    *(volatile u32 *)1899600 &= ~64u;
}

void BT_RF_CODE bqb_2m_setting(void) { *(volatile u32 *)1899556 = 6963264; }
void BT_RF_CODE bsb_normal_setting(void) { *(volatile u32 *)1899556 = 7028800; }

void BT_RF_CODE RF_analog_init(char en)
{
    if (en == 0) {
        *(volatile u32 *)1972560 = 0;
        if (br23_fm_tx_mode() == 1 && fm_tx_ch() == 0)
            *(volatile u32 *)1972564 = 10376;
        else if (br23_fm_tx_mode() == 1 && fm_tx_ch() == 1)
            *(volatile u32 *)1972564 = 18568;
        else if (br23_fm_tx_mode() == 0)
            *(volatile u32 *)1972564 = 128;
    } else {
        *(volatile u32 *)1972560 = 2147483646u;
        if (br23_fm_tx_mode() == 1 && fm_tx_ch() == 0)
            *(volatile u32 *)1972564 = 10351;
        else if (br23_fm_tx_mode() == 1 && fm_tx_ch() == 1)
            *(volatile u32 *)1972564 = 18543;
        else if (br23_fm_tx_mode() == 0)
            *(volatile u32 *)1972564 = 103;
    }
}

void BT_RF_CODE bt_rf_analog_init(void)
{
    RF_analog_init(0);
    *(volatile u32 *)1899600 |= 64;
    RF_mdm_init();
    bt_analog_part_init();
    u8 bank = bta_pll_bank_scan(2476);
    *(volatile u32 *)1972524 = (((u32)(bank & 127u) << 22) | 25894u);
    *(volatile u32 *)1972528 = 27001719;
    *(volatile u32 *)1972532 = 2446677;
    *(volatile u32 *)1972516 |= 32;
    delay((u32)rf.delay_sys * 150u);
    *(volatile u32 *)1972516 |= 128;
    set_bt_trim_mode((config_btctler_mode & 1) ? 0 : 1);
    freq_2_cap();
    *(volatile u32 *)1972524 &= (u32)-1025;
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]\n BR23 analog  init sucessful! \n\r\n");
    *(volatile u32 *)1899520 &= (u32)-201326593;
    RF_analog_init(1);
    bta_pll_config_init();
    clk_set_default_osc_cap();
    btagc_init();
    start_cap = get_bta_pll_bank(80);
    *(volatile u32 *)1899600 &= (u32)-65;
}

void BT_RF_CODE get_bta_pll_midbank_temp(void)
{
    if (config_bt_temperature_pll_trim == 0 || rf.start == 0)
        return;
    u32 modules = (u32)config_btctler_modules;
    int no_classic = (modules & 1u) == 0;
    int no_le = (modules & 2u) == 0;
    if (!no_classic) {
        bit_clr_ie(27); bit_clr_ie(28);
        if (get_idle_task_state() == 0)
            bredr_frame_suspend_control(1);
    }
    if (!no_le)
        le_hw_all_event_hold_en(1);
    RF_analog_init(0);
    start_cap = bta_pll_bank_scan_tmp(2401, start_cap);
    freq_2_cap();
    bta_pll_config_init();
    RF_analog_init(1);
    if (!no_classic) {
        if (get_idle_task_state() == 0)
            bredr_frame_suspend_control(0);
        bit_set_ie(27); bit_set_ie(28);
    }
    if (!no_le)
        le_hw_all_event_hold_en(0);
    printf("cap0:%x", start_cap);
}

u8 BT_RF_CODE btpll_status_det(void)
{
    RF_analog_init(0);
    u32 v0 = *(volatile u32 *)1972524;
    u32 v1 = *(volatile u32 *)1972528;
    *(volatile u32 *)1972524 = 232073213;
    *(volatile u32 *)1972528 = 53623;
    *(volatile u32 *)1972520 |= 32;
    *(volatile u32 *)1972564 &= (u32)-1921;
    *(volatile u32 *)1972564 |= 128;
    for (u32 i = 0; i < 6; ++i) {
        *(volatile u32 *)1972524 &= (u32)-532676609;
        *(volatile u32 *)1972524 |= (u32)(cap_freq_r[i][0] & 127u) << 22;
        *(volatile u32 *)1972528 &= (u32)-256;
        *(volatile u32 *)1972528 |= (u32)(cap_freq_r[i][1] + 239) & 255u;
        btpll_udelay(300);
        *(volatile u32 *)1972516 &= (u32)-129;
        btpll_udelay(8);
        *(volatile u32 *)1972516 |= 128;
        btpll_udelay(110);
        if (((*(volatile u32 *)1972600 | *(volatile u32 *)1972600 |
              *(volatile u32 *)1972600) & 6u) != 0) {
            *(volatile u32 *)1972524 = v0;
            *(volatile u32 *)1972528 = v1;
            RF_analog_init(1);
            *(volatile u32 *)1972564 &= (u32)-1921;
            ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]unlock\n\r\n");
            return 1;
        }
    }
    *(volatile u32 *)1972524 = v0;
    *(volatile u32 *)1972528 = v1;
    RF_analog_init(1);
    *(volatile u32 *)1972564 &= (u32)-1921;
    return 0;
}

void BT_RF_CODE btagc_enalbe(u8 en)
{
    *(volatile u32 *)1972560 = en != 0 ? (u32)-2 : (u32)-11534338;
}

void BT_RF_CODE analog_rx_control(u8 rx_okflag)
{
    u32 value = *(volatile u32 *)1899916;
    u8 count = analog_rx_control_rx_total_cnt++;
    if (count < 20) {
        if (rx_okflag == 143)
            ++analog_rx_control_rx_ok_cnt;
        u32 index = (value >> 8) & 63u;
        analog_rx_control_signal_intensity_ava =
            (u16)((((value | 65472u) ^ 63u) - (s8)agc_dbm_tlb[index]) +
                  analog_rx_control_signal_intensity_ava);
    } else {
        analog_rx_control_signal_intensity_ava = 0;
        analog_rx_control_rx_total_cnt = 0;
        analog_rx_control_rx_ok_cnt = 0;
    }
}

void BT_RF_CODE freq_2_cap(void)
{
    s16 cap_freq[60][2];
    u8 previous = get_bta_pll_bank(81);
    int count = 0;
    for (int frequency = 2402; frequency != 2481; ++frequency) {
        u8 bank = get_bta_pll_bank((u8)(frequency - 17));
        if (bank != previous) {
            cap_freq[count][0] = previous;
            cap_freq[count][1] = (s16)(frequency - 1);
            cap_freq[count + 1][0] = bank;
            cap_freq[count + 1][1] = (s16)frequency;
            count += 2;
        }
        previous = bank;
    }
    int end = count - 2;
    cap_freq_r[0][0] = cap_freq[1][0]; cap_freq_r[0][1] = cap_freq[1][1];
    cap_freq_r[1][0] = cap_freq[2][0]; cap_freq_r[1][1] = cap_freq[2][1];
    int index = (end >> 1) | 1;
    cap_freq_r[2][0] = cap_freq[index][0]; cap_freq_r[2][1] = cap_freq[index][1];
    cap_freq_r[3][0] = cap_freq[index + 1][0]; cap_freq_r[3][1] = cap_freq[index + 1][1];
    cap_freq_r[4][0] = cap_freq[end - 1][0]; cap_freq_r[4][1] = cap_freq[end - 1][1];
    cap_freq_r[5][0] = cap_freq[end][0]; cap_freq_r[5][1] = cap_freq[end][1];
}

void BT_RF_CODE btrx_iqtrim(void)
{
    rxtrim_analog_init();
    *(volatile u32 *)1899976 = 0; delay(2);
    *(volatile u32 *)1899972 = 0; delay(2);
    *(volatile u32 *)1899968 = 0; delay(2);
    *(volatile u32 *)1899680 |= 65536; delay(2);
    *(volatile u32 *)1899644 |= 2; delay(2);
    *(volatile u32 *)1899644 |= 4; delay(2);
    *(volatile u32 *)1899976 |= 32; delay(2);
    *(volatile u32 *)1899976 |= 33554432; delay(2);
    *(volatile u32 *)1899976 |= 65536; delay(2);
    *(volatile u32 *)1899976 |= 1048576; delay(2);
    *(volatile u32 *)1899968 |= 32; delay(2);
    *(volatile u32 *)1899968 |= 256; delay(2);
    *(volatile u32 *)1899968 &= (u32)-513; delay(2);
    *(volatile u32 *)1899968 = *(volatile u32 *)1899968; delay(2);
    *(volatile u32 *)1899968 |= 65536; delay(2);
    *(volatile u32 *)1899968 |= 131072; delay(2);
    *(volatile u32 *)1899968 |= 2097152; delay(2); delay(2);
    *(volatile u32 *)1899968 |= 16777216; delay(2);
    *(volatile u32 *)1899968 |= 33554432; delay(2);
    *(volatile u32 *)1899968 |= 536870912; delay(2);
    *(volatile u32 *)1899972 |= 128; delay(2);
    *(volatile u32 *)1899972 |= 64; delay(2);
    *(volatile u32 *)1899556 |= 2048;
    *(volatile u32 *)1899536 &= 16777215u;
    *(volatile u32 *)1899536 |= 0x80000000u;
    *(volatile u32 *)1899540 &= (u32)-65281;

    u32 best_score = UINT32_MAX;
    s16 best_index = 0;
    for (int index = 64; ; ++index) {
        *(volatile u32 *)1899540 &= (u32)-256;
        if (index == 180) break;
        *(volatile u32 *)1899540 |= (u32)index;
        for (int sample = 0; sample != 10; ++sample) {
            *(volatile u32 *)1899968 &= (u32)-33554433;
            delay(2);
            *(volatile u32 *)1899968 |= 33554432;
            while ((*(volatile u32 *)1899992 & 67108864u) != 0) { }
            (void)*(volatile u32 *)1899996;
            (void)*(volatile u32 *)1900000;
        }
        s32 i = (s16)(*(volatile u32 *)1899996 >> 8);
        s32 q = (s16)(*(volatile u32 *)1900000 >> 8);
        u32 score = (u32)(i * i) + (u32)(q * q);
        if (score < best_score) {
            best_score = score;
            best_index = (s16)index;
        }
    }
    *(volatile u32 *)1899540 |= (u16)best_index & 255u;
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]min index : %d\n\r\n", (int)best_index);
    trim_info.rxpuk2 = best_index;
    *(volatile u32 *)1899540 &= (u32)-65281;
    *(volatile u32 *)1899540 = *(volatile u32 *)1899540;

    best_score = UINT32_MAX;
    for (int index = -30; ; ++index) {
        *(volatile u32 *)1899540 &= (u32)-65281;
        if (index == 30) break;
        *(volatile u32 *)1899540 |= ((u32)index << 8);
        u32 total = 0;
        for (int sample = 0; sample != 10; ++sample) {
            *(volatile u32 *)1899968 &= (u32)-33554433;
            delay(2);
            *(volatile u32 *)1899968 |= 33554432;
            while ((*(volatile u32 *)1899992 & 67108864u) != 0) { }
            s32 i = (s16)(*(volatile u32 *)1899996 >> 8);
            s32 q = (s16)(*(volatile u32 *)1900000 >> 8);
            total += (u32)(i * i) + (u32)(q * q);
        }
        u32 score = (u32)((s32)total / 10);
        if (score < best_score) {
            best_score = score;
            best_index = (s16)index;
        }
    }
    *(volatile u32 *)1899540 |= ((u32)best_index << 8) & 65280u;
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]min index : %d\n\r\n", (int)best_index);
    trim_info.rxpsk3 = best_index;
    *(volatile u32 *)1899680 &= (u32)-65537; delay(2);
    *(volatile u32 *)1899644 &= (u32)-3; delay(2);
    *(volatile u32 *)1899644 &= (u32)-5; delay(2);
    *(volatile u32 *)1899976 &= (u32)-50331649; delay(2);
    *(volatile u32 *)1899556 &= (u32)-2049;
}

void BT_RF_CODE bttx_dctrim(void)
{
    txtrim_analog_init();
    *(volatile u32 *)1899976 = 0; delay(2);
    *(volatile u32 *)1899972 = 0; delay(2);
    *(volatile u32 *)1899968 = 0; delay(2);
    *(volatile u32 *)1899680 |= 65536; delay(2);
    *(volatile u32 *)1899644 |= 2; delay(2);
    *(volatile u32 *)1899644 |= 4; delay(2);
    *(volatile u32 *)1899976 |= 32; delay(2);
    *(volatile u32 *)1899976 |= 33554432; delay(2); delay(2); delay(2);
    *(volatile u32 *)1899968 |= 32; delay(2);
    *(volatile u32 *)1899968 |= 256; delay(2);
    *(volatile u32 *)1899968 &= (u32)-513; delay(2);
    *(volatile u32 *)1899968 |= 3072; delay(2);
    *(volatile u32 *)1899968 |= 65536; delay(2);
    *(volatile u32 *)1899968 |= 131072; delay(2);
    *(volatile u32 *)1899968 |= 2097152; delay(2);
    *(volatile u32 *)1899972 |= 128; delay(2);
    *(volatile u32 *)1899972 |= 64; delay(2);
    *(volatile u32 *)1899556 |= 2048;
    *(volatile u32 *)1899968 &= (u32)-131073;
    delay(5);
    *(volatile u32 *)1899968 |= 131072;
    while ((*(volatile u32 *)1899980 & 67108864u) != 0) { }
    s16 best_i = 0, best_q = 0;
    for (int phase = 3; phase >= 0; --phase) {
        u32 best_score = UINT32_MAX;
        s16 best = 0;
        for (int index = -100; ; ++index) {
            volatile u32 *r = (volatile u32 *)1899528;
            if (index == 100) break;
            if (phase == 0 || phase == 2) {
                *r &= (u32)-1024;
                *r |= (u32)index & 1023u;
            } else {
                *r &= (u32)-1047553;
                *r |= ((u32)index << 10) & 1047552u;
                if (phase == 3) *r &= (u32)-1024;
            }
            *(volatile u32 *)1899968 &= (u32)-131073;
            delay(5);
            *(volatile u32 *)1899968 |= 131072;
            while ((*(volatile u32 *)1899980 & 67108864u) != 0) { }
            s32 i = (s16)(*(volatile u32 *)1899984 >> 8);
            s32 q = (s16)(*(volatile u32 *)1899988 >> 8);
            u32 score = (u32)(i * i) + (u32)(q * q);
            if (score < best_score) { best_score = score; best = (s16)index; }
        }
        if (phase == 0 || phase == 2) {
            *(volatile u32 *)1899528 &= (u32)-1024;
            *(volatile u32 *)1899528 |= (u32)best & 1023u;
            best_i = best;
        } else {
            *(volatile u32 *)1899528 &= (u32)-1047553;
            *(volatile u32 *)1899528 |= ((u32)best << 10) & 1047552u;
            best_q = best;
        }
    }
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]I_dc : %d  Q_dc : %d\n\r\n",
                     (int)best_i, (int)best_q);
    trim_info.txi_dc = best_i;
    trim_info.txq_dc = best_q;
    *(volatile u32 *)1899680 &= (u32)-65537; delay(2);
    *(volatile u32 *)1899644 &= (u32)-3; delay(2);
    *(volatile u32 *)1899644 &= (u32)-5; delay(2);
    *(volatile u32 *)1899968 = 0;
    *(volatile u32 *)1899972 = 0;
    *(volatile u32 *)1899976 = 0;
    delay(2);
    *(volatile u32 *)1899556 &= (u32)-2049;
}

void BT_RF_CODE btrx_dctrim(void)
{
    u32 adc_buf[65];
    bt_analog_part_init();
    *(volatile u32 *)1972488 &= (u32)-28673;
    *(volatile u32 *)1972488 |= 28672u;
    *(volatile u32 *)1972492 &= (u32)-3841;
    *(volatile u32 *)1972492 |= 2561u;
    *(volatile u32 *)1972484 &= (u32)-524289;
    *(volatile u32 *)1972488 |= 128;
    *(volatile u32 *)1972488 &= (u32)-16777217;
    *(volatile u32 *)1899644 |= 2;
    *(volatile u32 *)1899680 |= 65536;
    *(volatile u32 *)1899644 |= 16;
    *(volatile u32 *)1899556 = *(volatile u32 *)1899556;
    *(volatile u32 *)1899520 &= (u32)-201326593;
    *(volatile u32 *)1899704 = (u32)(uintptr_t)adc_buf;
    for (u32 i = 0; i < 64; ++i) adc_buf[i] = 0;
    *(volatile u32 *)1972484 &= (u32)-256;
    *(volatile u32 *)1972484 = *(volatile u32 *)1972484;
    *(volatile u32 *)1972484 &= (u32)-130561;
    *(volatile u32 *)1972484 = *(volatile u32 *)1972484;
    *(volatile u32 *)1972484 |= 262144;
    for (int j = 0; j != 10; ++j) {
        *(volatile u32 *)1899700 = 4194304;
        while (*(volatile u32 *)1899700 > 65535u) { }
    }

    s32 dc = 0, i_dc = 0, q_dc = 0;
    s16 i_trim_count = 0, q_trim_count = 0;
    u8 debug_log = log_tag_const_d_Analog;
    u8 error_log = log_tag_const_e_Analog;
    int overflow_count = 0;
    for (int phase = 0; phase != 2; ++phase) {
        overflow_count = 0;
        for (;;) {
            *(volatile u32 *)1899700 = (u32)phase | 4194304u;
            while (*(volatile u32 *)1899700 > 65535u) { }
            s32 accumulator = 0;
            for (int index = 63; index > 0; --index) {
                u32 sample = adc_buf[index];
                accumulator = (((((s32)(sample << 16) >> 3) +
                                  (accumulator * 4088)) / 4096) * 4088 +
                                  (((s32)sample >> 16) * 8192)) / 4096;
            }
            s32 measured = accumulator / 128;
            int negative = accumulator < -127;
            s32 magnitude = negative ? -measured : measured;
            s32 adjust = magnitude > 60 ? 8 : magnitude > 40 ? 4 : magnitude > 20 ? 2 : 1;
            u32 control = *(volatile u32 *)1972484;
            s16 next_i_count = i_trim_count, next_q_count = q_trim_count;
            if (phase == 0) {
                *(volatile u32 *)1972484 = negative ? (control & (u32)-257) : (control | 256u);
                next_i_count = (s16)(i_trim_count + adjust);
                *(volatile u32 *)1972484 &= (u32)-256;
                *(volatile u32 *)1972484 |= (u32)(u8)next_i_count;
            } else {
                *(volatile u32 *)1972484 = negative ? (control & (u32)-131073) : (control | 131072u);
                next_q_count = (s16)(q_trim_count + adjust);
                *(volatile u32 *)1972484 &= (u32)-130561;
                *(volatile u32 *)1972484 |= ((u32)(u8)next_q_count << 9);
            }
            s32 old_dc = dc;
            ANALOG_LOG_LOW(debug_log,
                            "[Debug]: [Analog]i_cnt : %d q_cnt : %d i_dc : %d q_dc : %d adj : %d\n \r\n",
                            (int)next_i_count, (int)next_q_count,
                            phase == 0 ? measured : i_dc,
                            phase == 1 ? measured : q_dc, (int)adjust);
            if (phase == 0) {
                i_dc = measured;
            } else {
                q_dc = measured;
            }
            dc = measured;
            if ((measured * measured < 26) || (measured * old_dc < 0)) {
                s32 step = accumulator > -128 ? measured : -measured;
                s32 abs_dc = old_dc > -1 ? old_dc : -old_dc;
                if (step > abs_dc) {
                    if (phase == 0) --next_i_count;
                    else --next_q_count;
                }
            } else {
                int previous_overflow_count = overflow_count;
                ++overflow_count;
                if (previous_overflow_count > 2000) {
                    ANALOG_LOG_ERROR(error_log, "<Error>: [Analog]rx dc_trim error!!!\n\r\n");
                    *(volatile u32 *)1972484 &= (u32)-256;
                    *(volatile u32 *)1972484 = *(volatile u32 *)1972484;
                    *(volatile u32 *)1972484 &= (u32)-130561;
                    *(volatile u32 *)1972484 = *(volatile u32 *)1972484;
                    if (phase == 0) i_trim_count = next_i_count;
                    else q_trim_count = next_q_count;
                    break;
                }
            }
            if (phase == 0)
                i_trim_count = next_i_count;
            else
                q_trim_count = next_q_count;
            if (measured * measured < 26 || measured * old_dc < 0)
                break;
        }
    }
    trim_info.rxi_trim_cnt = i_trim_count;
    trim_info.rxq_trim_cnt = q_trim_count;
    *(volatile u32 *)1899644 &= (u32)-3;
    *(volatile u32 *)1899680 &= (u32)-65537;
    *(volatile u32 *)1899644 &= (u32)-17;
    *(volatile u32 *)1899520 |= 67108864;
    (void)i_dc; (void)q_dc;
}

void BT_RF_CODE bttx_iqtrim(void)
{
    txtrim_analog_init();
    *(volatile u32 *)1899968 = 0;
    *(volatile u32 *)1899972 = 0;
    *(volatile u32 *)1899976 = 0;
    *(volatile u32 *)1899680 |= 65536; delay(2);
    *(volatile u32 *)1899644 |= 2; delay(2);
    *(volatile u32 *)1899644 |= 4; delay(2);
    *(volatile u32 *)1899976 |= 21; delay(2);
    *(volatile u32 *)1899976 |= 33554432; delay(2);
    *(volatile u32 *)1899976 = *(volatile u32 *)1899976; delay(2);
    *(volatile u32 *)1899976 = *(volatile u32 *)1899976; delay(2);
    *(volatile u32 *)1899968 |= 42; delay(2);
    *(volatile u32 *)1899968 |= 256; delay(2);
    *(volatile u32 *)1899968 &= (u32)-513; delay(2);
    *(volatile u32 *)1899968 |= 3072; delay(2);
    *(volatile u32 *)1899968 |= 65536; delay(2);
    *(volatile u32 *)1899968 |= 131072; delay(2);
    *(volatile u32 *)1899968 |= 2097152; delay(2);
    *(volatile u32 *)1899972 |= 128; delay(2);
    *(volatile u32 *)1899972 |= 64; delay(2);
    *(volatile u32 *)1899556 |= 2048;
    *(volatile u32 *)1899536 &= (u32)-65281;
    *(volatile u32 *)1899536 |= 16384;
    trim_info.txpuk2 = 64;
    *(volatile u32 *)1899968 &= (u32)-131073; delay(2);
    *(volatile u32 *)1899968 |= 131072;
    while ((*(volatile u32 *)1899980 & 67108864u) != 0) { }
    u32 best_score = UINT32_MAX;
    s16 best_puk = 0;
    for (int index = 30; ; ++index) {
        *(volatile u32 *)1899536 &= (u32)-256;
        if (index == 90) break;
        *(volatile u32 *)1899536 |= (u32)index;
        *(volatile u32 *)1899968 &= (u32)-131073; delay(2);
        *(volatile u32 *)1899968 |= 131072;
        while ((*(volatile u32 *)1899980 & 67108864u) != 0) { }
        s32 i = (s16)(*(volatile u32 *)1899984 >> 8);
        s32 q = (s16)(*(volatile u32 *)1899988 >> 8);
        u32 score = (u32)(i * i) + (u32)(q * q);
        if (score < best_score) { best_score = score; best_puk = (s16)index; }
    }
    *(volatile u32 *)1899536 |= (u16)best_puk & 255u;
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]TX_CMPUK1:%d\n\r\n", (int)best_puk);
    *(volatile u32 *)1899536 &= (u32)-16711681;
    *(volatile u32 *)1899536 = *(volatile u32 *)1899536;
    best_score = UINT32_MAX;
    s16 best_psk = best_puk;
    for (int index = -30; ; ++index) {
        *(volatile u32 *)1899536 &= (u32)-16711681;
        if (index == 30) break;
        *(volatile u32 *)1899536 |= ((u32)index & 255u) << 16;
        *(volatile u32 *)1899968 &= (u32)-131073; delay(2);
        *(volatile u32 *)1899968 |= 131072;
        while ((*(volatile u32 *)1899980 & 67108864u) != 0) { }
        s32 i = (s16)(*(volatile u32 *)1899984 >> 8);
        s32 q = (s16)(*(volatile u32 *)1899988 >> 8);
        u32 score = (u32)(i * i) + (u32)(q * q);
        if (score < best_score) { best_score = score; best_psk = (s16)index; }
    }
    *(volatile u32 *)1899536 |= ((u32)best_psk << 16) & 16711680u;
    s32 temp = best_puk < 64 ? 64 / best_puk : best_puk >> 6;
    s32 puk1 = 1200 / (temp * 10);
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]TX_CMPSK3:%d\n\r\n", (int)best_psk);
    trim_info.txpsk3 = best_psk;
    *(volatile u32 *)1899536 &= (u32)-65281;
    *(volatile u32 *)1899536 |= 30720;
    *(volatile u32 *)1899536 &= (u32)-256;
    *(volatile u32 *)1899536 |= (u32)puk1 & 255u;
    trim_info.txpuk1 = (s16)puk1;
    trim_info.txpuk2 = 120;
    *(volatile u32 *)1899680 &= (u32)-65537; delay(2);
    *(volatile u32 *)1899644 &= (u32)-3; delay(2);
    *(volatile u32 *)1899644 &= (u32)-5; delay(2);
    *(volatile u32 *)1899968 = 0;
    *(volatile u32 *)1899972 = 0;
    *(volatile u32 *)1899976 = 0;
    delay(2);
    *(volatile u32 *)1899556 &= (u32)-2049;
}

u8 BT_RF_CODE bta_pll_bank_scan(unsigned int set_freq)
{
    *(volatile u32 *)1972524 = 232073213;
    *(volatile u32 *)1972528 = 53623;
    *(volatile u32 *)1972520 |= 32;
    *(volatile u32 *)1972564 &= (u32)-897;
    *(volatile u32 *)1972564 |= 128;
    *(volatile u32 *)1972528 &= (u32)-256;
    *(volatile u32 *)1972528 |= 79u;
    int cap_l = 0, cap_h = 0, cap_m = 0, found = 0;
    for (int bank = 7; bank < 120; bank += 16) {
        cap_m = cap_l;
        for (int test = 0; test != 3; ++test) {
            *(volatile u32 *)1972524 &= (u32)-532676609;
            int value = bank + (test == 1 ? -7 : test == 2 ? 8 : 0);
            *(volatile u32 *)1972524 |= ((u32)value << 22) & 532676608u;
            *(volatile u32 *)1972516 &= (u32)-129;
            delay((u32)rf.delay_sys * 5u);
            *(volatile u32 *)1972516 |= 128;
            delay((u32)rf.delay_sys * 60u);
            int result = (s16)((*(volatile u32 *)1972600 |
                                *(volatile u32 *)1972600 |
                                *(volatile u32 *)1972600) >> 1) & 3;
            if (test == 0) cap_m = result;
            else if (test == 1) cap_l = result;
            else cap_h = result;
        }
    if (cap_h != cap_m || cap_l != cap_m) {
        cap_m = bank;
        found = 1;
        break;
    }
    }
    (void)found;
    if (!found) cap_m = cap_l;
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog] 2402 bank: %x \r\n", 3);
    rf.bta_pll_bank_limit[31] = (u8)(cap_m | 15);
    u8 info_log = log_tag_const_d_Analog;
    int scan_low = 74, scan_high = 74;
    for (int bank = 0; bank != 31; ++bank) {
        *(volatile u32 *)1972524 &= (u32)-532676609;
        u8 code = (u8)(rf.bta_pll_bank_limit[31] - (u8)bank);
        *(volatile u32 *)1972524 |= ((u32)code << 22) & 532676608u;
        ANALOG_LOG_LOW(info_log, "[Debug]: [Analog]bak: %x %d \r\n", code, bank);
        u8 previous_high = (u8)scan_high;
        s8 low = (s8)scan_low, high = (s8)scan_high, value = (s8)scan_low;
        for (;;) {
            *(volatile u32 *)1972528 &= (u32)-256;
            *(volatile u32 *)1972528 |= (u8)value;
            *(volatile u32 *)1972516 &= (u32)-129;
            delay((u32)rf.delay_sys * 5u);
            *(volatile u32 *)1972516 |= 128;
            delay((u32)rf.delay_sys * 60u);
            int state = (int)((*(volatile u32 *)1972600 |
                               *(volatile u32 *)1972600 |
                               *(volatile u32 *)1972600) >> 1) & 3;
            if (state == 2) state = -2;
            if (state == 1) { high = value; break; }
            s8 previous = value;
            value = (s8)(value + 1);
            if (state == -2) {
                if ((u8)value > (u8)-89) { high = low = -89; break; }
                low = previous;
            } else if ((u8)value > (u8)-89) {
                high = -89;
                break;
            }
        }
        scan_low = low;
        scan_high = high;
        int result = (int)(((u32)(u8)previous_high + 3u + (u8)low) >> 1);
        rf.bta_pll_bank_limit[bank] = (u8)result;
        ANALOG_LOG_LOW(info_log, "[Debug]: [Analog][%d : %d] %d \n\r\n",
                         (u8)low, (u8)high, result & 255);
    }
    *(volatile u32 *)1972528 &= (u32)-256;
    *(volatile u32 *)1972528 |= (u32)(set_freq - 2321) & 255u;
    u8 result = get_bta_pll_bank((u8)(set_freq - 2321));
    *(volatile u32 *)1972524 &= (u32)-532676609;
    *(volatile u32 *)1972524 |= ((u32)result << 22) & 532676608u;
    ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]setbak:%x\r\n", result);
    *(volatile u32 *)1972520 &= (u32)-33;
    return result;
}

u8 BT_RF_CODE bta_pll_bank_scan_tmp(unsigned int set_freq, u8 cap_high)
{
    u32 saved_bank = *(volatile u32 *)1972524;
    u32 saved_freq = *(volatile u32 *)1972528;
    *(volatile u32 *)1972524 = 232073213;
    *(volatile u32 *)1972528 = 53623;
    *(volatile u32 *)1972520 |= 32;
    *(volatile u32 *)1972564 &= (u32)-897;
    *(volatile u32 *)1972564 |= 128;
    u8 debug_log = log_tag_const_d_Analog;
    ANALOG_LOG_LOW(debug_log, "[Debug]: [Analog]cap_high:%x\n\r\n", cap_high);
    u8 low_nibble = cap_high & 15u;
    u8 adjusted = low_nibble < 2 ? (u8)(cap_high - 16u) :
                  (low_nibble > 13 ? (u8)(cap_high + 16u) : cap_high);
    u8 cap_tmp = adjusted | 15u;
    ANALOG_LOG_LOW(debug_log, "[Debug]: [Analog]cap_tmp:%x\n\r\n",
                     cap_tmp);
    rf.bta_pll_bank_limit[31] = cap_tmp;
    s8 dat_l = 74, dat_buf1 = 74;
    u8 info_log = log_tag_const_i_Analog;
    for (int bank = 0; bank < 31; ++bank) {
        *(volatile u32 *)1972524 &= (u32)-532676609;
        u8 code = (u8)(rf.bta_pll_bank_limit[31] - (u8)bank);
        *(volatile u32 *)1972524 |= ((u32)code << 22) & 532676608u;
        ANALOG_LOG_DEBUG(info_log, "[Info]: [Analog]bak: %x %d \r\n", code, bank);
        s8 dat_buf = dat_l;
        s8 dat_l_ph = dat_l;
        s8 dat_h;
        for (;;) {
            *(volatile u32 *)1972528 &= (u32)-256;
            *(volatile u32 *)1972528 |= (u8)dat_buf;
            *(volatile u32 *)1972516 &= (u32)-129;
            btpll_udelay(8);
            *(volatile u32 *)1972516 |= 128;
            btpll_udelay(110);
            int state = (int)((*(volatile u32 *)1972600 |
                               *(volatile u32 *)1972600 |
                               *(volatile u32 *)1972600) >> 1) & 3;
            if (state == 2) state = -2;
            if (state == 1) {
                dat_h = dat_buf;
                dat_l = dat_l_ph;
                break;
            }
            if (state == -2) {
                s8 next = (s8)(dat_buf + 1);
                if ((u8)next > (u8)-89) {
                    dat_h = -89;
                    dat_l = -89;
                    break;
                }
                dat_l_ph = dat_buf;
                dat_buf = next;
                continue;
            }
            s8 next = (s8)(dat_buf + 1);
            if ((u8)next > (u8)-89) {
                dat_h = -89;
                dat_l = dat_l_ph;
                break;
            }
            dat_buf = next;
        }
        int average = (int)(((u32)(u8)dat_buf1 + 3u + (u8)dat_l) >> 1);
        rf.bta_pll_bank_limit[bank] = (u8)average;
        dat_buf1 = dat_h;
        u8 stored = (u8)average;
        if (info_log) {
            log_print(2, NULL, "[Info]: [Analog][%d : %d] %d\r\n",
                      (u8)dat_l, (u8)dat_h, average & 255);
            stored = rf.bta_pll_bank_limit[bank];
        }
        if (stored > (u8)-90)
            break;
    }
    *(volatile u32 *)1972528 &= (u32)-256;
    *(volatile u32 *)1972528 |= (u32)(set_freq - 2321) & 255u;
    u8 bank = get_bta_pll_bank((u8)(set_freq - 2321));
    *(volatile u32 *)1972524 &= (u32)-532676609;
    *(volatile u32 *)1972524 |= ((u32)bank << 22) & 532676608u;
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]setbak:%x\r\n", bank);
    *(volatile u32 *)1972520 &= (u32)-33;
    *(volatile u32 *)1972524 = saved_bank;
    *(volatile u32 *)1972528 = saved_freq;
    return bank;
}

u8 BT_RF_CODE get_ldo_trim_res(u8 *res)
{
    memcpy(res, &ldo_trim_res.chargpump_ldo_lev, 9);
    return 9;
}

u8 BT_RF_CODE bta_pll_bank_set(u8 bank)
{
    return (u8)(rf.bta_pll_bank_limit[31] - bank);
}

void BT_RF_CODE btpll_udelay(u32 us)
{
    int count = (clk_get("sys") / 8000000) * (us < 2 ? 0 : (int)(us - 1));
    while (count != 0) {
        --count;
        __asm__ volatile("nop");
    }
}

void BT_RF_CODE btpll_mdelay(u32 ms)
{
    for (u32 i = 0; i != ms; ++i)
        btpll_udelay(1000);
}

void BT_RF_CODE BT_CP_EN(u8 en)
{
    volatile u32 *r = (volatile u32 *)1972480;
    *r &= ~14336u;
    if (en != 0)
        *r |= ((u32)(en & 7u) << 11);
}

void BT_RF_CODE rxadc_ldo_trim_set(u8 sel)
{
    volatile u32 *r = (volatile u32 *)1972480;
    *r &= (u32)-16762880;
    r = (volatile u32 *)1972492; *r &= (u32)-1677328385;
    r = (volatile u32 *)1972496; *r &= (u32)-2041;
    r = (volatile u32 *)1972480; *r |= 2050021u;
    r = (volatile u32 *)1972492; *r |= ((u32)(sel & 3u) << 24) | 540147712u;
    r = (volatile u32 *)1972496; *r |= 856u;
}

void BT_RF_CODE rxadc_ldo_trim_finish(void)
{
    *(volatile u32 *)1972480 = 4491045;
    *(volatile u32 *)1972492 = 976374565;
    *(volatile u32 *)1972496 = 537;
}

void BT_RF_CODE rxadc_ref_trim_set(int sel)
{
    volatile u32 *r = (volatile u32 *)1972480;
    *r &= (u32)-16762880;
    r = (volatile u32 *)1972492; *r &= (u32)-1677328385;
    r = (volatile u32 *)1972496; *r &= (u32)-2041;
    r = (volatile u32 *)1972480; *r |= 2050021u;
    r = (volatile u32 *)1972492; *r |= 573702144u;
    r = (volatile u32 *)1972496; *r |= (((u32)sel << 9) & 1536u) | 88u;
}

void BT_RF_CODE rxadc_ref_trim_finish(void)
{
    *(volatile u32 *)1972480 = 4491045;
    *(volatile u32 *)1972492 = 976374565;
    *(volatile u32 *)1972496 = 537;
}

void BT_RF_CODE umixer_vblo_trim_set(int sel)
{
    P33_CON_SET(6, 0, 3, 4);
    volatile u32 *r = (volatile u32 *)1972480; *r &= (u32)-507968;
    r = (volatile u32 *)1972500; *r &= (u32)-534808580;
    r = (volatile u32 *)1972504; *r &= (u32)-15585;
    r = (volatile u32 *)1972480; *r |= 442405u;
    r = (volatile u32 *)1972492; *r |= 33685504u;
    r = (volatile u32 *)1972500; *r |= 144738305u;
    r = (volatile u32 *)1972504; *r |= (((u32)sel << 10) & 7168u) | 224u;
}

void BT_RF_CODE umixer_vblo_trim_finish(void)
{
    P33_CON_SET(6, 0, 3, 3);
    *(volatile u32 *)1972480 = 4491045;
    *(volatile u32 *)1972492 = 976374565;
    *(volatile u32 *)1972500 = 146148302;
    *(volatile u32 *)1972504 = 536868064;
}

void BT_RF_CODE cp_trim_set(u8 sel)
{
    *(volatile u32 *)1972516 = 169574624;
    *(volatile u32 *)1972520 = (((u32)(sel & 3u) << 18) | 636485642u);
}

void BT_RF_CODE div_trim_set(u8 sel)
{
    *(volatile u32 *)1972516 = 169574624;
    *(volatile u32 *)1972520 = (((u32)(sel & 3u) << 20) | 667680778u);
}

void BT_RF_CODE vco1_trim_set(u8 sel)
{
    *(volatile u32 *)1972516 = 169574624;
    *(volatile u32 *)1972520 = (((u32)(sel & 3u) << 22) | 691798026u);
}

void BT_RF_CODE vco2_trim_set(u8 sel)
{
    *(volatile u32 *)1972516 = 169574624;
    *(volatile u32 *)1972520 = (((u32)sel << 2) & 12u) | 737935362u;
}

void BT_RF_CODE xosc_trim_set(u8 sel)
{
    volatile u32 *r = (volatile u32 *)1972512;
    *r &= (u32)-14680077;
    *r |= (((u32)sel << 2) & 12u) | 6291456u;
}

void BT_RF_CODE bt_vref_init(void) { }
void BT_RF_CODE bt_vref_close(void) { }

u8 BT_RF_CODE bt_get_pwr_max_level(void)
{
    if (config_force_bt_pwr_tab_using_normal_level != 0)
        return 4;
    u32 mode = (u32)config_btctler_mode;
    if (mode & 33u)
        return 4;
    if (mode & 2u)
        return 6;
    return (mode & 4u) ? 9 : 4;
}

void BT_RF_CODE rxtrim_analog_init(void)
{
    bt_analog_part_init();
    *(volatile u32 *)1972504 &= (u32)-33538049;
    *(volatile u32 *)1972504 |= 16384;
    *(volatile u32 *)1972488 &= (u32)-28673;
    *(volatile u32 *)1972484 &= (u32)-62914561;
    *(volatile u32 *)1972492 &= (u32)-3965;
    u32 new_chip = (get_chip_version() & 15u) != 0;
    *(volatile u32 *)1972488 |= new_chip ? 20480u : 24576u;
    *(volatile u32 *)1972484 |= new_chip ? 16777216u : 12582912u;
    *(volatile u32 *)1972492 |= new_chip ? 1060u : 2852u;
    *(volatile u32 *)1972484 &= (u32)-262145;
    *(volatile u32 *)1972484 |= 524288;
    *(volatile u32 *)1972488 |= 128;
    *(volatile u32 *)1972480 |= 262144;
    *(volatile u32 *)1972500 |= 7;
    *(volatile u32 *)1972500 |= 2112;
    u32 version = get_chip_version();
    if ((version & 15u) != 0)
        *(volatile u32 *)1972504 |= 8192;
    else
        *(volatile u32 *)1972504 &= (u32)-8193;
}

void BT_RF_CODE txtrim_analog_init(void)
{
    bt_analog_part_init();
    *(volatile u32 *)1972480 |= 262144;
    *(volatile u32 *)1972484 &= (u32)-524289;
    *(volatile u32 *)1972488 &= (u32)-129;
    *(volatile u32 *)1972488 &= (u32)-28673;
    *(volatile u32 *)1972492 &= (u32)-2;
    *(volatile u32 *)1972488 &= (u32)-16777217;
    *(volatile u32 *)1972492 &= (u32)-3841;
    *(volatile u32 *)1972492 &= (u32)-129;
    *(volatile u32 *)1972492 &= (u32)-125;
    *(volatile u32 *)1972492 &= (u32)-1048577;
    *(volatile u32 *)1972492 &= (u32)-2097153;
    *(volatile u32 *)1972496 &= (u32)-9;
    *(volatile u32 *)1972492 |= 3279488;
    *(volatile u32 *)1972496 |= 8;
    *(volatile u32 *)1972492 |= 1;
    *(volatile u32 *)1972500 |= 3;
    *(volatile u32 *)1972500 |= 2112;
    *(volatile u32 *)1972504 = *(volatile u32 *)1972504;
    *(volatile u32 *)1972508 |= 98305;
}

u32 BT_RF_CODE bt_get_txpwr_tb(u8 level)
{
    u8 max_level = bt_get_pwr_max_level();
    u8 clamped = max_level < level ? max_level : level;
    s32 forced = config_force_bt_pwr_tab_using_normal_level;
    if (forced != 0) {
        u8 index = (u8)(forced > (s32)max_level ? max_level : (u8)forced);
        return TXPWR_table[index];
    }
    u32 mode = (u32)config_btctler_mode;
    if (mode & 33u)
        return rf.production_test ? TXPWR_table_pro[clamped] : TXPWR_table[clamped];
    if (mode & 2u)
        return TXPWR_table_dut[clamped];
    if (mode & 4u)
        return TXPWR_table_fcc[clamped];
    return TXPWR_table[clamped];
}

u32 BT_RF_CODE bt_get_txset_tb(u8 level)
{
    u8 max_level = bt_get_pwr_max_level();
    u8 clamped = max_level < level ? max_level : level;
    s32 forced = config_force_bt_pwr_tab_using_normal_level;
    if (forced != 0) {
        u8 index = (u8)(forced > (s32)max_level ? max_level : (u8)forced);
        return TXSET_table[index];
    }
    u32 mode = (u32)config_btctler_mode;
    if (mode & 33u)
        return rf.production_test ? TXSET_table_pro[clamped] : TXSET_table[clamped];
    if (mode & 2u)
        return TXSET_table_dut[clamped];
    if (mode & 4u)
        return TXSET_table_fcc[clamped];
    return TXSET_table[clamped];
}

static void BT_RF_CODE bt_analog_part_init(void)
{
    *(volatile u32 *)1972564 |= 128;
    *(volatile u32 *)1972480 = 4491045;
    *(volatile u32 *)1972484 = 203161600;
    *(volatile u32 *)1972488 = 259095168;
    *(volatile u32 *)1972492 = 976898853;
    *(volatile u32 *)1972496 = 537;
    *(volatile u32 *)1972500 = 146148302;
    *(volatile u32 *)1972504 = 536868064;
    *(volatile u32 *)1972508 = 0;
    *(volatile u32 *)1972516 |= 169582816;
    *(volatile u32 *)1972520 = 620494858;
}

void BT_RF_CODE set_bt_trim_mode(u8 mode)
{
    u8 data[29];
    bt_fre_offset_value_init();
    int read_saved = mode == 0 && ((u32)syscfg_read(93, data, 29) & 255u) == 29u;
    if (read_saved) {
        memcpy(&trim_info, data, 20);
        memcpy(&ldo_trim_res.chargpump_ldo_lev, &data[20], 9);
        reset_trim_info();
        set_ldo_trim_res();
        return;
    }
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]\n ------- bt trim -------\n\r\n");
    bttx_dctrim();
    bttx_iqtrim();
    btrx_dctrim();
    btrx_iqtrim();
    bt_analog_part_init();
    reset_trim_info();
    bt_ldo_trim();
    set_ldo_trim_res();
    memcpy(data, &trim_info, 20);
    memcpy(&data[20], &ldo_trim_res.chargpump_ldo_lev, 9);
    (void)syscfg_write(93, data, 29);
}

void BT_RF_CODE set_ldo_trim_res(void)
{
    u32 chip = get_chip_version();
    u32 zero_count = 0;
    if ((chip & 15u) != 0) {
        u32 cp_zero = ldo_trim_res.chargpump_ldo_lev == 0;
        u32 div_zero = ldo_trim_res.divider_ldo_lev == 0;
        zero_count = (ldo_trim_res.vco_1ldo_lev == 0) +
                     (div_zero ? (cp_zero ? 2u : 1u) : cp_zero) +
                     (ldo_trim_res.vco_2ldo_lev == 0) +
                     (ldo_trim_res.xosc_ldo_lev == 0);
    }
    zero_count += (ldo_trim_res.rxadc_ref_lev == 0) +
                  (ldo_trim_res.umx_vblo_lev == 0);
    u8 log_state = log_tag_const_i_Analog;
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]zero_cnt = %d \r\n", zero_count);
    delay(20);
    if (zero_count > 1) {
        ANALOG_LOG_INFO(log_state,
                        "[Info]: [Analog]>>>>>>>>> LDO TRIM FAIL : ldo voltage from SARADC abnormal !!!\n\r\n");
        u32 fail_chip = get_chip_version();
        if ((fail_chip & 15u) != 0) {
            ANALOG_LOG_INFO(log_state, "[Info]: [Analog]chargpump level: [%d]\n\r\n",
                            ldo_trim_res.chargpump_ldo_lev);
            ANALOG_LOG_INFO(log_state, "[Info]: [Analog]divider   level: [%d]\n\r\n",
                            ldo_trim_res.divider_ldo_lev);
            ANALOG_LOG_INFO(log_state, "[Info]: [Analog]vco1      level: [%d]\n\r\n",
                            ldo_trim_res.vco_1ldo_lev);
            ANALOG_LOG_INFO(log_state, "[Info]: [Analog]vco2      level: [%d]\n\r\n",
                            ldo_trim_res.vco_2ldo_lev);
            ANALOG_LOG_INFO(log_state, "[Info]: [Analog]xosc      level: [%d]\n\r\n",
                            ldo_trim_res.xosc_ldo_lev);
        }
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]rxadcref  level debug: [%d]\n\r\n",
                        ldo_trim_res.rxadc_ref_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]umixer    level debug: [%d]\n\r\n",
                        ldo_trim_res.umx_vblo_lev);
        return;
    }
    chip = get_chip_version();
    u8 error_log_state = log_state;
    if ((chip & 15u) != 0 && ldo_trim_res.chargpump_ldo_en) {
        volatile u32 *r = (volatile u32 *)1972520;
        *r &= (u32)-786433;
        u8 level = ldo_trim_res.chargpump_ldo_lev;
        *r |= ((u32)(level & 3u) << 18);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]cp ldo level: [%d]\n\r\n", level);
    } else if ((chip & 15u) != 0 && error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][pllcp]     ldo trim abnormal and finish!!!\n\r\n");
    }
    if ((chip & 15u) != 0 && ldo_trim_res.divider_ldo_en) {
        volatile u32 *r = (volatile u32 *)1972520;
        *r &= (u32)-3145729;
        u8 level = ldo_trim_res.divider_ldo_lev;
        *r |= ((u32)(level & 3u) << 20);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]div ldo level: [%d]\n\r\n", level);
    } else if ((chip & 15u) != 0 && error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][plldiv]     ldo trim abnormal and finish!!!\n\r\n");
    }
    if ((chip & 15u) != 0 && ldo_trim_res.vco_1ldo_en) {
        volatile u32 *r = (volatile u32 *)1972520;
        *r &= (u32)-12582913;
        u8 level = ldo_trim_res.vco_1ldo_lev;
        *r |= ((u32)(level & 3u) << 22);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]vco1 ldo level: [%d]\n\r\n", level);
    } else if ((chip & 15u) != 0 && error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][pllvco1]     ldo trim abnormal and finish!!!\n\r\n");
    }
    if ((chip & 15u) != 0 && ldo_trim_res.vco_2ldo_en) {
        volatile u32 *r = (volatile u32 *)1972520;
        *r &= (u32)-13;
        u8 level = ldo_trim_res.vco_2ldo_lev;
        *r |= ((u32)(level << 2) & 12u);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]vco2 ldo level: [%d]\n\r\n", level);
    } else if ((chip & 15u) != 0 && error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][pllvco2]     ldo trim abnormal and finish!!!\n\r\n");
    }
    if ((chip & 15u) != 0 && ldo_trim_res.xosc_ldo_en) {
        volatile u32 *r = (volatile u32 *)1972512;
        *r &= (u32)-13;
        u8 level = ldo_trim_res.xosc_ldo_lev;
        *r |= ((u32)(level << 2) & 12u);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]xosc ldo level: [%d]\n\r\n", level);
    } else if ((chip & 15u) != 0 && error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][xosc]     ldo trim abnormal and finish!!!\n\r\n");
    }
    if (ldo_trim_res.rxadc_ref_en) {
        volatile u32 *r = (volatile u32 *)1972496;
        *r &= (u32)-1537;
        u8 level = ldo_trim_res.rxadc_ref_lev;
        *r |= ((u32)(level & 3u) << 9);
        error_log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(error_log_state, "[Info]: [Analog]rxadcref  level: [%d]\n\r\n", level);
    } else if (error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][rxadc]     ref trim abnormal and finish!!!\n\r\n");
    }
    if (ldo_trim_res.umx_vblo_en) {
        volatile u32 *r = (volatile u32 *)1972504;
        *r &= (u32)-7169;
        u8 level = ldo_trim_res.umx_vblo_lev;
        *r |= ((u32)(level & 7u) << 10);
        ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]umixer    level: [%d]\n\r\n", level);
    } else if (error_log_state) {
        ANALOG_LOG_INFO(1, "[Info]: [Analog][umx]       vblo trim abnormal and finish!!!\n\r\n");
    }
}

#define RUN_LDO_TRIM(member, enable_member, setter, select, limit, min_level, max_level, log_state, startfmt, errorfmt) do { \
    do { \
        ANALOG_LOG_INFO(log_state, startfmt, ldo_trim_res.member); \
        setter(ldo_trim_res.member); \
        delay(2000); \
        if (get_ldo_voltage(select) > (limit)) break; \
        ++ldo_trim_res.member; \
    } while (ldo_trim_res.member < ((max_level) + 1)); \
    if (ldo_trim_res.member > (max_level)) \
        ldo_trim_res.member = (max_level); \
    ldo_trim_res.enable_member = ldo_trim_res.member >= (min_level) && \
        ldo_trim_res.member <= (max_level); \
    if (!ldo_trim_res.enable_member) \
        ANALOG_LOG_INFO(log_state, errorfmt); \
} while (0)

void BT_RF_CODE bt_ldo_trim(void)
{
    u8 log_state = log_tag_const_i_Analog;
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]====================================================================================\n\r\n");
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]//                                                                                //\n\r\n");
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]//                              bt ldo trim start                                 //\n\r\n");
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]//                                                                                //\n\r\n");
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]====================================================================================\n\r\n");
    u32 r0 = *(volatile u32 *)1972480; delay(20);
    u32 r1 = *(volatile u32 *)1972484; delay(20);
    u32 r2 = *(volatile u32 *)1972488; delay(20);
    (void)r1; (void)r2;
    u32 r3 = *(volatile u32 *)1972492; delay(20);
    u32 r4 = *(volatile u32 *)1972496; delay(20);
    u32 r5 = *(volatile u32 *)1972500; delay(20);
    u32 r6 = *(volatile u32 *)1972504; delay(20);
    u32 r7 = *(volatile u32 *)1972512; delay(20);
    u32 r8 = *(volatile u32 *)1972516; delay(20);
    u32 r9 = *(volatile u32 *)1972520; delay(20);
    (void)*(volatile u32 *)1972552; delay(20);
    memset(&ldo_trim_res.chargpump_ldo_lev, 0, 9);
    local_irq_disable();
    if ((get_chip_version() & 15u) != 0) {
        delay(20);
        RUN_LDO_TRIM(chargpump_ldo_lev, chargpump_ldo_en, cp_trim_set, 2, 1075, 1, 3,
                     log_state, "[Info]: [Analog]btpll_cp_lev: %d\n\r\n",
                     "[Info]: [Analog]PLL CP TRIM ABNORMAL\n\r\n");
        *(volatile u32 *)1972516 = r8;
        *(volatile u32 *)1972520 = r9;
        log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>chargpump_trim finish : level is [%d]<<<\n\r\n",
                        ldo_trim_res.chargpump_ldo_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
        delay(20);
        RUN_LDO_TRIM(divider_ldo_lev, divider_ldo_en, div_trim_set, 2, 1075, 1, 3,
                     log_state, "[Info]: [Analog]btpll_div_lev: %d\n\r\n",
                     "[Info]: [Analog]PLL DIV TRIM ABNORMAL\n\r\n");
        *(volatile u32 *)1972516 = r8;
        *(volatile u32 *)1972520 = r9;
        log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>divider_trim finish : level is [%d]<<<\n\r\n",
                        ldo_trim_res.divider_ldo_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
        delay(20);
        RUN_LDO_TRIM(vco_1ldo_lev, vco_1ldo_en, vco1_trim_set, 2, 1075, 1, 3,
                     log_state, "[Info]: [Analog]btpll_vco1_lev: %d\n\r\n",
                     "[Info]: [Analog]PLL VCO1 TRIM ABNORMAL\n\r\n");
        *(volatile u32 *)1972516 = r8;
        *(volatile u32 *)1972520 = r9;
        log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>vco1_trim finish : level is [%d]<<<\n\r\n",
                        ldo_trim_res.vco_1ldo_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
        delay(20);
        RUN_LDO_TRIM(vco_2ldo_lev, vco_2ldo_en, vco2_trim_set, 2, 875, 1, 3,
                     log_state, "[Info]: [Analog]btpll_vco2_lev: %d\n\r\n",
                     "[Info]: [Analog]PLL VCO2 TRIM ABNORMAL\n\r\n");
        *(volatile u32 *)1972516 = r8;
        *(volatile u32 *)1972520 = r9;
        log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>vco2_trim finish : level is [%d]<<<\n\r\n",
                        ldo_trim_res.vco_2ldo_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
        delay(20);
        RUN_LDO_TRIM(xosc_ldo_lev, xosc_ldo_en, xosc_trim_set, 1, 1045, 1, 3,
                     log_state, "[Info]: [Analog]xosc_ldo_lev: %d\n\r\n",
                     "[Info]: [Analog]XOSC LDO TRIM ABNORMAL\n\r\n");
        *(volatile u32 *)1972512 = r7;
        log_state = log_tag_const_i_Analog;
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>xosc_trim finish : level is [%d]<<<\n\r\n",
                        ldo_trim_res.xosc_ldo_lev);
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
    }
    delay(20);
    do {
        ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]rxadc_ref_lev: %d\n\r\n",
                        ldo_trim_res.rxadc_ref_lev);
        rxadc_ref_trim_set(ldo_trim_res.rxadc_ref_lev);
        delay(2000);
        if (get_ldo_voltage(2) > 850) break;
        ++ldo_trim_res.rxadc_ref_lev;
    } while (ldo_trim_res.rxadc_ref_lev < 4);
    if (ldo_trim_res.rxadc_ref_lev > 3)
        ldo_trim_res.rxadc_ref_lev = 3;
    ldo_trim_res.rxadc_ref_en = ldo_trim_res.rxadc_ref_lev >= 1 && ldo_trim_res.rxadc_ref_lev <= 3;
    if (!ldo_trim_res.rxadc_ref_en)
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]RXADC REF TRIM ABNORMAL\n\r\n");
    *(volatile u32 *)1972480 = r0;
    *(volatile u32 *)1972492 = r3;
    *(volatile u32 *)1972496 = r4;
    log_state = log_tag_const_i_Analog;
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>rxadc_ref_trim finish : level is [%d]<<<\n\r\n",
                    ldo_trim_res.rxadc_ref_lev);
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
    delay(20);
    do {
        ANALOG_LOG_LOW(log_tag_const_d_Analog, "[Debug]: [Analog]umx_vblo_lev: %d\n\r\n",
                        ldo_trim_res.umx_vblo_lev);
        umixer_vblo_trim_set(ldo_trim_res.umx_vblo_lev);
        delay(2000);
        if (get_ldo_voltage(2) > 980) break;
        ++ldo_trim_res.umx_vblo_lev;
    } while (ldo_trim_res.umx_vblo_lev < 8);
    if (ldo_trim_res.umx_vblo_lev > 7)
        ldo_trim_res.umx_vblo_lev = 7;
    ldo_trim_res.umx_vblo_en = ldo_trim_res.umx_vblo_lev >= 4 && ldo_trim_res.umx_vblo_lev <= 7;
    if (!ldo_trim_res.umx_vblo_en)
        ANALOG_LOG_INFO(log_state, "[Info]: [Analog]UMX VBLO TRIM ABNORMAL\n\r\n");
    *(volatile u32 *)1972480 = r0;
    *(volatile u32 *)1972492 = r3;
    *(volatile u32 *)1972500 = r5;
    *(volatile u32 *)1972504 = r6;
    log_state = log_tag_const_i_Analog;
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]>>>umx_vblo_trim finish : level is [%d]<<<\n\r\n",
                    ldo_trim_res.umx_vblo_lev);
    ANALOG_LOG_INFO(log_state, "[Info]: [Analog]======================================================================================\n\r\n");
    BT_CP_EN(0);
    local_irq_enable();
}
#undef RUN_LDO_TRIM

u32 BT_RF_CODE get_ldo_voltage(u8 sel)
{
    BT_CP_EN(0);
    adc_pmu_detect_en(1);
    (void)adc_sample(15);
    u32 vbg = 0;
    for (int i = 0; i != 10; ++i) {
        while ((s8)*(volatile u32 *)1978624 > -1) { }
        vbg += *(volatile u32 *)1978628;
        *(volatile u32 *)1978624 |= 64;
    }
    BT_CP_EN(sel);
    delay(200);
    (void)adc_sample(262159);
    u32 ldo = 0;
    for (int i = 0; i != 10; ++i) {
        while ((s8)*(volatile u32 *)1978624 > -1) { }
        ldo += *(volatile u32 *)1978628;
        *(volatile u32 *)1978624 |= 64;
    }
    u32 average_vbg = vbg / 10u;
    u32 average_ldo = ldo / 10u;
    u8 log_enabled = log_tag_const_i_Analog;
    if (log_enabled) {
        log_print(2, NULL, "[Info]: [Analog]adc_vbg = %d\n\r\n", average_vbg);
        log_print(2, NULL, "[Info]: [Analog]adc_res = %d\n\r\n", average_ldo);
        u32 logged_voltage = adc_value_to_voltage(average_vbg, average_ldo);
        log_print(2, NULL, "[Info]: [Analog]====== %d \r\n", logged_voltage);
    }
    return adc_value_to_voltage(average_vbg, average_ldo);
}

#ifdef UNITTEST
#include "test.h"

struct common_rf_param rf;
static int analog_test_scan_rows;
static int analog_test_mutate_limit;
static int analog_test_ldo_setter_calls;
static int analog_test_watch_phase3;
static int analog_test_phase3_seen;
static int analog_test_phase3_clear;
const u8 log_tag_const_i_Analog = 1;
const u8 log_tag_const_d_Analog = 0;
const u8 log_tag_const_e_Analog = 0;
void delay(u32 us)
{
    if (us == 2000) ++analog_test_ldo_setter_calls;
    if (analog_test_watch_phase3 && us == 5) {
        u32 reg = *(volatile u32 *)1899528;
        if ((reg & 1047552u) == (((u32)-100 << 10) & 1047552u)) {
            analog_test_phase3_seen = 1;
            analog_test_phase3_clear = (reg & 1023u) == 0;
            analog_test_watch_phase3 = 0;
        }
    }
}
int clk_get(const char *name) { (void)name; return 0; }
void P33_CON_SET(u16 port, u8 value, u8 mask, u8 direction)
{ (void)port; (void)value; (void)mask; (void)direction; }
void local_irq_disable(void) { }
void local_irq_enable(void) { }
u32 get_chip_version(void) { return 1; }
void adc_pmu_detect_en(int enable) { (void)enable; }
u32 adc_sample(u32 select) { (void)select; return 0; }
u32 adc_value_to_voltage(u32 vbg, u32 ldo) { (void)vbg; (void)ldo; return 0; }

void log_print(int level, void *format, const char *message, ...)
{
    (void)level; (void)format;
    if (strcmp(message, "[Info]: [Analog][%d : %d] %d\r\n") == 0) {
        ++analog_test_scan_rows;
        if (analog_test_mutate_limit >= 0)
            rf.bta_pll_bank_limit[0] = (u8)analog_test_mutate_limit;
    }
}

static void analog_test_map_mmio(void)
{
    void *a = mmap((void *)0x1cf000, 0x1000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *b = mmap((void *)0x1e1000, 0x4000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(a == (void *)0x1cf000);
    CHECK(b == (void *)0x1e1000);
}

BTCTRLER_CONFIG int config_force_bt_pwr_tab_using_normal_level = 0;
BTCTRLER_CONFIG int config_btctler_mode = 0;

int main(void)
{
    static const u32 expected_bt_frac_pll_frac_48m[83] = {
        349525, 1048576, 1747627, 2446677, 3145728, 3844779, 4543829, 5242880,
        5941931, 6640981, 7340032, 8039083, 8738133, 9437184, 10136235, 10835285,
        11534336, 12233387, 12932437, 13631488, 14330539, 15029589, 15728640, 16427691,
        349525, 1048576, 1747627, 2446677, 3145728, 3844779, 4543829, 5242880,
        5941931, 6640981, 7340032, 8039083, 8738133, 9437184, 10136235, 10835285,
        11534336, 12233387, 12932437, 13631488, 14330539, 15029589, 15728640, 16427691,
        349525, 1048576, 1747627, 2446677, 3145728, 3844779, 4543829, 5242880,
        5941931, 6640981, 7340032, 8039083, 8738133, 9437184, 10136235, 10835285,
        11534336, 12233387, 12932437, 13631488, 14330539, 15029589, 15728640, 16427691,
        349525, 1048576, 1747627, 2446677, 3145728, 3844779, 4543829, 5242880,
        5941931, 0, 0
    };
    CHECK(memcmp(bt_frac_pll_frac_48m, expected_bt_frac_pll_frac_48m, sizeof(expected_bt_frac_pll_frac_48m)) == 0);
    static const u8 expected_bt_frac_pll_int_48m[83] = {
        100, 100, 100, 100, 100, 100, 100, 100,
        100, 100, 100, 100, 100, 100, 100, 100,
        100, 100, 100, 100, 100, 100, 100, 100,
        101, 101, 101, 101, 101, 101, 101, 101,
        101, 101, 101, 101, 101, 101, 101, 101,
        101, 101, 101, 101, 101, 101, 101, 101,
        102, 102, 102, 102, 102, 102, 102, 102,
        102, 102, 102, 102, 102, 102, 102, 102,
        102, 102, 102, 102, 102, 102, 102, 102,
        103, 103, 103, 103, 103, 103, 103, 103,
        103, 0, 0
    };
    CHECK(memcmp(bt_frac_pll_int_48m, expected_bt_frac_pll_int_48m, sizeof(expected_bt_frac_pll_int_48m)) == 0);
    static const u32 expected_TXPWR_table_pro[5] = {
        7348351, 7348351, 7348351, 7348351, 7348863
    };
    CHECK(memcmp(TXPWR_table_pro, expected_TXPWR_table_pro, sizeof(expected_TXPWR_table_pro)) == 0);
    static const u32 expected_TXSET_table_pro[5] = {
        15732722, 15745010, 15794162, 15990770, 16777202
    };
    CHECK(memcmp(TXSET_table_pro, expected_TXSET_table_pro, sizeof(expected_TXSET_table_pro)) == 0);
    static const u32 expected_TXPWR_table[5] = {
        7348953, 7348953, 7348953, 7348953, 7348953
    };
    CHECK(memcmp(TXPWR_table, expected_TXPWR_table, sizeof(expected_TXPWR_table)) == 0);
    static const u32 expected_TXSET_table[5] = {
        15732674, 15744962, 15794114, 15990722, 16777154
    };
    CHECK(memcmp(TXSET_table, expected_TXSET_table, sizeof(expected_TXSET_table)) == 0);
    static const u32 expected_TXPWR_table_fcc[10] = {
        7348905, 7348905, 7348905, 7348905, 7348905, 7348905, 7348905, 7348905,
        7348905, 7348905
    };
    CHECK(memcmp(TXPWR_table_fcc, expected_TXPWR_table_fcc, sizeof(expected_TXPWR_table_fcc)) == 0);
    static const u32 expected_TXSET_table_fcc[10] = {
        15729602, 15730626, 15732674, 15736770, 15744962, 15761346, 15794114, 15859650,
        15990722, 16252866
    };
    CHECK(memcmp(TXSET_table_fcc, expected_TXSET_table_fcc, sizeof(expected_TXSET_table_fcc)) == 0);
    static const u32 expected_TXPWR_table_dut[7] = {
        7348905, 7348905, 7348905, 7348905, 7348905, 7348905, 7348905
    };
    CHECK(memcmp(TXPWR_table_dut, expected_TXPWR_table_dut, sizeof(expected_TXPWR_table_dut)) == 0);
    static const u32 expected_TXSET_table_dut[7] = {
        15729602, 15730626, 15736770, 15744962, 15794114, 15990722, 16777154
    };
    CHECK(memcmp(TXSET_table_dut, expected_TXSET_table_dut, sizeof(expected_TXSET_table_dut)) == 0);
    rf.bta_pll_bank_limit[31] = 10;
    rf.bta_pll_bank_limit[1] = 80;
    rf.bta_pll_bank_limit[2] = 82;
    CHECK(get_jl_chip_id() == 6);
    CHECK(get_bta_pll_bank(79) == 10);
    CHECK(get_bta_pll_bank(80) == 9);
    CHECK(get_bta_pll_bank(82) == 0);
    CHECK(get_bta_pll_bank(255) == 0);

    memset(rf.bta_pll_bank_limit, 0, sizeof(rf.bta_pll_bank_limit));
    rf.bta_pll_bank_limit[1] = 85;
    rf.bta_pll_bank_limit[2] = 95;
    rf.bta_pll_bank_limit[3] = 105;
    rf.bta_pll_bank_limit[31] = 20;
    freq_2_cap();
    CHECK(cap_freq_r[0][0] == 19 && cap_freq_r[0][1] == 2406);
    CHECK(cap_freq_r[1][0] == 19 && cap_freq_r[1][1] == 2415);
    CHECK(cap_freq_r[2][0] == 18 && cap_freq_r[2][1] == 2416);
    CHECK(cap_freq_r[3][0] == 18 && cap_freq_r[3][1] == 2425);
    CHECK(cap_freq_r[4][0] == 18 && cap_freq_r[4][1] == 2416);
    CHECK(cap_freq_r[5][0] == 18 && cap_freq_r[5][1] == 2425);

    /* Mapped host registers give the trim searches deterministic zero samples. */
    analog_test_map_mmio();
    bttx_iqtrim();
    CHECK(trim_info.txpsk3 == -30); /* strict best-score update on zero-valued samples */

    /* Phase 3 clears the I field after programming Q, before sampling. */
    *(volatile u32 *)1899528 = 0x155u;
    analog_test_phase3_seen = analog_test_phase3_clear = 0;
    analog_test_watch_phase3 = 1;
    bttx_dctrim();
    CHECK(analog_test_phase3_seen && analog_test_phase3_clear);

    /* All loops reach their last legal setter level when ADC stays below limit. */
    *(volatile u32 *)1978624 = 0xffffff80u;
    *(volatile u32 *)1978628 = 100;
    analog_test_ldo_setter_calls = 0;
    bt_ldo_trim();
    CHECK(analog_test_ldo_setter_calls == 32);
    CHECK(ldo_trim_res.chargpump_ldo_lev == 3);
    CHECK(ldo_trim_res.rxadc_ref_lev == 3);
    CHECK(ldo_trim_res.umx_vblo_lev == 7);

    /* The raw cutoff is unsigned: stored 75 is not above 166. */
    memset(rf.bta_pll_bank_limit, 0, sizeof(rf.bta_pll_bank_limit));
    analog_test_scan_rows = 0;
    analog_test_mutate_limit = -1;
    (void)bta_pll_bank_scan_tmp(2402, 80);
    CHECK(analog_test_scan_rows == 31);

    /* The log callback may mutate the just-stored entry; raw code reloads it. */
    memset(rf.bta_pll_bank_limit, 0, sizeof(rf.bta_pll_bank_limit));
    analog_test_scan_rows = 0;
    analog_test_mutate_limit = 167;
    (void)bta_pll_bank_scan_tmp(2402, 80);
    CHECK(analog_test_scan_rows == 1);
    analog_test_mutate_limit = -1;

    CHECK(bt_get_pwr_max_level() == 4);
    CHECK(bt_get_txpwr_tb(0) == 7348953u);
    CHECK(bt_get_txpwr_tb(4) == 7348953u);
    CHECK(bt_get_txpwr_tb(255) == 7348953u);
    CHECK(bt_get_txset_tb(0) == 15732674u);
    CHECK(bt_get_txset_tb(4) == 16777154u);
    CHECK(TXPWR_table_pro[4] == 7348863u);
    CHECK(TXSET_table_fcc[9] == 16252866u);
    CHECK(TXSET_table_dut[6] == 16777154u);

    config_btctler_mode = 1;
    rf.production_test = 1;
    CHECK(bt_get_pwr_max_level() == 4);
    CHECK(bt_get_txpwr_tb(4) == 7348863u);
    CHECK(bt_get_txset_tb(4) == 16777202u);
    config_btctler_mode = 2;
    CHECK(bt_get_pwr_max_level() == 6);
    CHECK(bt_get_txset_tb(6) == 16777154u);
    config_btctler_mode = 4;
    CHECK(bt_get_pwr_max_level() == 9);
    CHECK(bt_get_txset_tb(9) == 16252866u);
    config_force_bt_pwr_tab_using_normal_level = 3;
    CHECK(bt_get_pwr_max_level() == 4);
    CHECK(bt_get_txset_tb(255) == 15990722u);
    config_force_bt_pwr_tab_using_normal_level = 99;
    CHECK(bt_get_txset_tb(0) == 16777154u);
    return TEST_DONE("btctrler_analog");
}
#endif
