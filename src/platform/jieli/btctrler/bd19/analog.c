/* BD19 analog.c.o occurrence 1. Keep the SDK power tables, packed syscfg
 * records, PLL/ADC waits and volatile read/write order. Calibration block
 * labels retain the SDK IR control flow, including its undef recurrence seed.
 * The original BR23 source remains independent. */
#include "btctrler/glue.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#define memcpy __builtin_memcpy
#define memset __builtin_memset

#define BT_RF_CODE __attribute__((section(".bt_rf_code")))
#define BT_RF_BSS __attribute__((section(".bt_rf_bss")))
#define BT_RF_DATA __attribute__((section(".bt_rf_data")))
#define BT_RF_CONST __attribute__((section(".bt_rf_const")))
#define REG32(address) (*(volatile u32 *)(uintptr_t)(address))

extern BTCTRLER_CONFIG int config_force_bt_pwr_tab_using_normal_level;
extern int config_btctler_single_carrier_en;
extern BTCTRLER_CONFIG int config_bt_temperature_pll_trim;
struct syscfg_operataions {
 int (*f0)(void);int (*check_id)(u16);int (*f2)(u16,u8*,u16);int (*f3)(u16,u8*,u16);int (*f4)(u16,u8*,u16);int (*f5)(u16,u8*,u16,u8);u8 *(*f6)(u16,u16*);
};
extern struct syscfg_operataions *get_syscfg_btif_ops(void);
extern BTCTRLER_CONFIG int CONFIG_BLE_SYNC_WORD_BIT;
extern BTCTRLER_CONFIG u8 lp_bt_sniff_use_btosc;
extern u32 clock_get_pll_target_frequency(void);
extern void clk_set_osc_cap(u8,u8);
extern u16 get_vbg_trim(void);
extern int syscfg_read(u16 key, u8 *data, u16 len);
extern int syscfg_write(u16 key, u8 *data, u16 len);
extern u16 CRC16(const u8 *data, u32 len);
extern int clk_get(const char *name);
extern void delay(u32 us);
extern void power_reset_close(void);
extern void btagc_init(u8 lnav_flag, u8 agc_table_switch);
extern void bit_clr_ie(u8 bit);
extern void bit_set_ie(u8 bit);
extern u8 get_idle_task_state(void);
extern void bredr_frame_suspend_control(u8 suspend);
extern void le_hw_all_event_hold_en(u8 hold);
extern const char agc_dbm_tlb[33];
extern u32 adc_sample(u32 select);
extern void adc_pmu_detect_en(int enable);

static void BT_RF_CODE bt_analog_part_init(void);
static void BT_RF_CODE bta_pll_config_init(void);
static __attribute__((noinline)) void BT_RF_CODE set_bt_trim_mode(u8);
static void BT_RF_CODE freq_2_cap(void);
static void BT_RF_CODE reset_trim_info(void);
static void BT_RF_CODE rxtrim_analog_init(void);
static int BT_RF_CODE vco_lock_trim(void);
u8 BT_RF_CODE bta_pll_bank_scan_tmp(u32 set_freq, u8 cap_high);

struct rf_analog_trim BT_RF_BSS trim_info __attribute__((aligned(1)));
struct ldo_trim_res BT_RF_BSS ldo_trim_res __attribute__((aligned(1)));
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

const u32 BT_RF_CONST TXPWR_table[11] = {503372497,503438033,503569105,503831249,504355537,505404113,507501265,511695569,520084177,536861393,536861417};
const u32 BT_RF_CONST TXSET_table[11] = {242,242,242,242,242,242,242,242,242,242,242};
const u32 BT_RF_CONST TXPWR_table_fcc[11] = {503340752,503373520,503439056,503570128,503832272,504356560,505405136,507502288,511696592,520085200,536862416};
const u32 BT_RF_CONST TXPWR_table_bqb[7] = {503373520,503439056,503570128,503832272,505405136,511696592,536862416};
static u8 BT_RF_DATA analog_rx_control_rx_ok_cnt __asm__("analog_rx_control.rx_ok_cnt");
static u8 BT_RF_DATA analog_rx_control_rx_total_cnt __asm__("analog_rx_control.rx_total_cnt");
static u16 BT_RF_DATA analog_rx_control_signal_intensity_ava __asm__("analog_rx_control.signal_intensity_ava");
static u8 BT_RF_BSS lnav_flag;
volatile u8 BT_RF_BSS lna_g,dm_g,bp_g,tsel;
u8 BT_RF_DATA config_mesh_certification;

u8 BT_RF_CODE get_jl_chip_id(void) { return 15; }
u8 BT_RF_CODE get_jl_chip_id2(void) { return 15; }

void BT_RF_CODE bt_run_clk_sel(u8 enable)
{
    u32 value = REG32(1899556);
    if (enable) {
        REG32(1899556) = value | 12288u;
        value = REG32(1972564) & ~4096u;
    } else {
        REG32(1899556) = value & ~12288u;
        value = REG32(1972564) | 4096u;
    }
    REG32(1972564) = value;
    REG32(1972496) = REG32(1972496) & ~256u;
    REG32(1972496) = REG32(1972496) | 512u;
}

void BT_RF_CODE RF_analog_recover(void) { }

extern const u8 log_tag_const_i_Analog;
extern const u8 log_tag_const_e_Analog;
extern void log_print(int level, void *format, const char *message, ...);
#define ANALOG_LOG_INFO(enabled, ...) do { if (enabled) log_print(2, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_LOW(enabled, ...) do { if (enabled) log_print(1, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_DEBUG(enabled, ...) do { if (enabled) log_print(2, NULL, __VA_ARGS__); } while (0)
#define ANALOG_LOG_ERROR(enabled, ...) do { if (enabled) log_print(4, NULL, __VA_ARGS__); } while (0)



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
        ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]offset set%d \n\r\n", (s16)sum);
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
 REG32(1899692)=0;
 for(u32 i=0;i<81;i++) {
 u8 bank=get_bta_pll_bank((u8)(i+79));s32 frac=bt_frac_pll_frac_48m[i];u32 integer=bt_frac_pll_int_48m[i];
 if((u32)(frac-4194304)>8388607u) REG32(1899696)=((u32)bank<<12)|524580;
 else { REG32(1899696)=((u32)bank<<12)|1573156;
 if(frac<8388608) { integer--;frac+=8388608; } else frac-=8388608; }
 REG32(1899696)=(integer<<20)|(u32)(frac>>4);
 }
}

void BT_RF_CODE bt_reload_trim(void)
{
 REG32(1899600)|=64;REG32(1899528)&=~1023u;REG32(1899528)&=~1047552u;
 REG32(1899528)|=trim_info.txi_dc&1023;REG32(1899528)|=(trim_info.txq_dc&1023u)<<10;
 REG32(1899532)=0x80808080;bta_pll_config_init();REG32(1899600)&=~64u;
}

void BT_RF_CODE bt_set_single_carrier(void)
{
 REG32(1899600)|=64;REG32(1899528)&=~1023u;REG32(1899528)&=~1047552u;
 REG32(1899528)|=768;REG32(1899528)|=786432;REG32(1899532)=0;REG32(1899692)=0;
 for(u32 i=0;i<81;i++) {
 u8 bank=get_bta_pll_bank((u8)(i+79));s32 frac=bt_frac_pll_frac_48m[i],value=frac-349525;u32 integer=bt_frac_pll_int_48m[i];
 if((u32)(frac-4543829)>8388607u) REG32(1899696)=((u32)bank<<12)|524580;
 else { REG32(1899696)=((u32)bank<<12)|1573156;
 if(value<8388608) { integer--;value=frac+8039083; } else value=frac-8738133; }
 REG32(1899696)=(integer<<20)|(u32)(value>>4);
 }REG32(1899600)&=~64u;
}

void BT_RF_CODE bt_analog_reset(void)
{
 int mode=config_btctler_mode;if(!(mode&1))power_reset_close();
 if(mode&2) { ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]bqb reset \n\r\n");bredr_radiopwrupdn_reset(); }
 if(mode&4) { ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]fcc reset \n\r\n");bredr_radiopwrupdn_reset(); }
}

void BT_RF_CODE RF_mdm_init(void)
{
 REG32(1899676)&=~31u;REG32(1899676)|=8;
 REG32(1899672)&=~255u;REG32(1899672)|=50;REG32(1899672)&=~16711680u;REG32(1899672)|=3276800;
 REG32(1899672)&=~65280u;REG32(1899672)|=2048;REG32(1899672)&=16777215;REG32(1899672)=REG32(1899672);
 REG32(1899676)&=~16711680u;REG32(1899676)|=3276800;REG32(1899676)&=16777215;REG32(1899676)|=838860800;
 delay(10);REG32(1899680)&=~255u;REG32(1899680)|=10;REG32(1899680)&=~65280u;REG32(1899680)|=2560;
 REG32(1899592)=185;REG32(1899520)=218091844;REG32(1899524)=(CONFIG_BLE_SYNC_WORD_BIT&31)|1845509312;
 REG32(1899528)=config_btctler_single_carrier_en && (config_btctler_mode&1)?839909376:1048576;
 REG32(1899532)=0x80808080;REG32(1899536)=30840;REG32(1899540)=603717887;REG32(1899544)=0;REG32(1899548)=0;
 REG32(1899552)=911562039;REG32(1899556)=2087551040;REG32(1899560)=101042208;REG32(1899564)=52428984;
 REG32(1900004)=1369564;REG32(1900008)=524352;REG32(1900012)=2097408;REG32(1900016)=8389632;REG32(1900020)=1327136;REG32(1900024)=4141055;
 if(config_mesh_certification==1)spur_control();REG32(1899560)&=~268435456u;REG32(1899560)&=~536870912u;
}

void BT_RF_CODE bt_dut_reset_mdm(void) { REG32(1899528)&=16777215;REG32(1899528)=REG32(1899528); }

void BT_RF_CODE certification(u8 tx_level_d)
{
    REG32(1899556) |= 0x80000000u;
    REG32(1899684) = 65536;
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
    REG32(1899600) |= 64u;
    REG32(1899556) &= 2147483647u;
    REG32(1899600) &= ~64u;
}

void BT_RF_CODE bqb_2m_setting(void) { REG32(1899556) = 6963264; }
void BT_RF_CODE bsb_normal_setting(void) { REG32(1899556) = 7028800; }



void BT_RF_CODE bt_rf_analog_init(void)
{
 REG32(1972560)=0;REG32(1972564)=128;REG32(1899600)|=64;RF_mdm_init();if(lp_bt_sniff_use_btosc)bt_run_clk_sel(1);
 bt_analog_part_init();REG32(1972524)&=268435455;REG32(1972524)|=1073741824;REG32(1972528)=14504530;REG32(1972532)=106615;
 REG32(1972520)|=134217728;REG32(1972564)&=~1920u;REG32(1972564)|=128;REG32(1972532)&=~255u;REG32(1972532)|=74;
 u16 scan=0;u16 mid=0,lo=0,hi=0;u32 cap;
 for(cap=7;cap<120;cap+=16) {
 for(int j=0;j<3;j++) {
 REG32(1972528)&=~33292288u;u32 code=j==0?cap:j==1?cap-7:cap+8;REG32(1972528)|=(code<<18)&33292288;
 REG32(1972520)&=~4u;delay((u32)rf.delay_sys*5);REG32(1972520)|=4;delay((u32)rf.delay_sys*60);
 u16 val=((REG32(1972600)|REG32(1972600)|REG32(1972600))>>1)&3;
 if(j==0)mid=val;else if(j==1)lo=val;else hi=val;
 }
 if(lo==mid && hi==mid) { scan=mid;continue; }scan=cap;break;
 }
 u8 info=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(info,"[Info]: [Analog] 2402 bank: %x \r\n",3);
 rf.bta_pll_bank_limit[31]=(u8)scan|15;
 u8 low=74,last=74;
 for(u32 bank=0;bank<31;bank++) {
 REG32(1972528)&=~33292288u;u8 code=rf.bta_pll_bank_limit[31]-(u8)bank;REG32(1972528)|=((u32)code<<18)&33292288;
 ANALOG_LOG_INFO(info,"[Info]: [Analog]bak: %x %d \r\n",code,bank);
 u8 cur=low,prior=low,high;
 for(;;) {
 REG32(1972532)&=~255u;REG32(1972532)|=cur;
 REG32(1972520)&=~4u;delay((u32)rf.delay_sys*5);REG32(1972520)|=4;delay((u32)rf.delay_sys*60);
 unsigned state=((REG32(1972600)|REG32(1972600)|REG32(1972600))>>1)&3;
 if(state==1) { low=prior;high=cur;break; }
 u8 next=cur+1;
 if(state==2) { if(next>167) { low=167;high=167;break; }prior=cur;cur=next; }
 else { if(next>167) { low=prior;high=167;break; }cur=next; }
 }
 u32 avg=((u32)last+3+low)>>1;rf.bta_pll_bank_limit[bank]=avg;
 ANALOG_LOG_INFO(info,"[Info]: [Analog][%d : %d] %d\r\n",low,high,avg&255);
 last=high;
 }
 REG32(1972532)&=~255u;REG32(1972532)|=155;u8 bank=get_bta_pll_bank(155);
 REG32(1972528)&=~33292288u;REG32(1972528)|=((u32)bank<<18)&33292288;
 ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]setbak:%x\r\n",bank);REG32(1972520)&=~134217728u;
 REG32(1972528)=((bank&127u)<<18)|83538;REG32(1972532)=54124663;
 REG32(1972536)&=~16777215u;REG32(1972536)|=2446677;REG32(1972520)|=1;delay((u32)rf.delay_sys*150);
 REG32(1972532)|=0x80000000;delay((u32)rf.delay_sys*10);REG32(1972520)|=4;
 set_bt_trim_mode(config_btctler_mode&1?0:1);
 REG32(1972564)|=128;REG32(1972480)|=786465;REG32(1972484)|=4194304;REG32(1972484)&=~2048u;
 u8 prev=0;u32 rsel;
 for(rsel=0;rsel<16;rsel++) {
 REG32(1972484)&=~503316480u;REG32(1972484)|=rsel<<25;
 delay(600);(void)REG32(1972600);delay(600);(void)REG32(1972600);delay(600);u8 state=(REG32(1972600)>>3)&1;
 ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]rsel : %d  lnav_o : %d\n\r\n",rsel,state);
 if(prev && !state) {
 ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]rsel_set : %x\n\r\n",rsel);
 REG32(1972484)&=~503316480u;REG32(1972484)|=((rsel<<25)+134217728)&503316480;REG32(1972484)|=2048;break;
 }prev=state;
 }
 lnav_flag=((u8)rsel!=0) && (rsel+4<10);freq_2_cap();
 ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]\n BD19 analog  init sucessful! \n\r\n");
 REG32(1972564)&=~16384u;REG32(1972560)=(u32)-21626898;REG32(1972564)=33525857;
 bta_pll_config_init();bta_gain_init();clk_set_osc_cap(rf.cap_l,rf.cap_r);btagc_init(lnav_flag,0);start_cap=get_bta_pll_bank(80);
 ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]start_cap : %x\n\r\n",start_cap);
 REG32(1972564)|=4096;REG32(1972564)&=~2048u;REG32(1972564)&=~8192u;REG32(1899600)&=~64u;
}

void BT_RF_CODE get_bta_pll_midbank_temp(void)
{
 if(!config_bt_temperature_pll_trim||!rf.start)return;int modules=config_btctler_modules;
 if(modules&1) { bit_clr_ie(27);bit_clr_ie(28);if(!get_idle_task_state())bredr_frame_suspend_control(1); }
 if(modules&2)le_hw_all_event_hold_en(1);
 REG32(1972560)=0;REG32(1972564)=128;start_cap=bta_pll_bank_scan_tmp(2401,start_cap);freq_2_cap();bta_pll_config_init();
 REG32(1972560)=(u32)-21626898;REG32(1972564)=33525857;
 modules=config_btctler_modules;
 if(modules&1) { if(!get_idle_task_state())bredr_frame_suspend_control(0);bit_set_ie(27);bit_set_ie(28); }
 if(modules&2)le_hw_all_event_hold_en(0);printf("cap0 >>>>>>>>>>>>:%x\n ",start_cap);
}

u8 BT_RF_CODE btpll_status_det(void)
{
 REG32(1972560)=0;REG32(1972564)=128;
 u32 a=REG32(1972524),b=REG32(1972528),c=REG32(1972532);
 REG32(1972524)&=268435455;REG32(1972524)|=1073741824;REG32(1972528)=14504530;REG32(1972532)=106615;
 REG32(1972520)|=134217728;REG32(1972564)&=~1920u;REG32(1972564)|=128;
 u8 bad=0;for(int i=0;i<6;i++) {
 REG32(1972528)&=~33292288u;REG32(1972528)|=(cap_freq_r[i][0]&127u)<<18;
 REG32(1972532)&=~255u;REG32(1972532)|=(cap_freq_r[i][1]+239u)&255;
 btpll_udelay(300);REG32(1972516)&=~4u;btpll_udelay(8);REG32(1972516)|=4;btpll_udelay(110);
 if((REG32(1972600)|REG32(1972600)|REG32(1972600))&6) { bad=1;break; }
 }
 REG32(1972524)=a;REG32(1972528)=b;REG32(1972532)=c;REG32(1972560)=(u32)-21626898;REG32(1972564)=33525857;REG32(1972564)&=~1920u;
 if(bad)ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]unlock\n\r\n");return bad;
}

void BT_RF_CODE btagc_enalbe(u8 en) { REG32(1972560)=en?(u32)-139266:(u32)-11673602; }

void BT_RF_CODE analog_rx_control(u8 rx_okflag)
{
    u32 value = REG32(1899916);
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

static void BT_RF_CODE freq_2_cap(void)
{
    s16 cap_freq[60][2];
    u8 previous = get_bta_pll_bank(81);
    u8 count = 0;
    for (u16 frequency = 2402; frequency != 2481; ++frequency) {
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
    u8 end = count - 2;
    cap_freq_r[0][0] = cap_freq[1][0]; cap_freq_r[0][1] = cap_freq[1][1];
    cap_freq_r[1][0] = cap_freq[2][0]; cap_freq_r[1][1] = cap_freq[2][1];
    int index = (end >> 1) | 1;
    cap_freq_r[2][0] = cap_freq[index][0]; cap_freq_r[2][1] = cap_freq[index][1];
    cap_freq_r[3][0] = cap_freq[index + 1][0]; cap_freq_r[3][1] = cap_freq[index + 1][1];
    cap_freq_r[4][0] = cap_freq[end - 1][0]; cap_freq_r[4][1] = cap_freq[end - 1][1];
    cap_freq_r[5][0] = cap_freq[end][0]; cap_freq_r[5][1] = cap_freq[end][1];
}











u8 BT_RF_CODE bta_pll_bank_scan_tmp(unsigned int set_freq, u8 cap_high)
{
    u32 saved_bank = REG32(1972524);
    u32 saved_freq = REG32(1972528);
    u32 saved_third=REG32(1972532);
    REG32(1972524)&=268435455;REG32(1972524)|=1073741824;
    REG32(1972528)=14504530;
    REG32(1972532)=106615;
    REG32(1972520)|=134217728;
    REG32(1972564) &= (u32)-1921;
    REG32(1972564) |= 128;
    u8 debug_log=log_tag_const_i_Analog;
    ANALOG_LOG_INFO(debug_log, "[Info]: [Analog]cap_high:%x\n\r\n", cap_high);
    u8 low_nibble = cap_high & 15u;
    u8 adjusted = low_nibble < 2 ? (u8)(cap_high - 16u) :
                  (low_nibble > 13 ? (u8)(cap_high + 16u) : cap_high);
    u8 cap_tmp = adjusted | 15u;
    ANALOG_LOG_INFO(debug_log, "[Info]: [Analog]cap_tmp:%x\n\r\n",
                     cap_tmp);
    rf.bta_pll_bank_limit[31] = cap_tmp;
    s8 dat_l = 74, dat_buf1 = 74;
    u8 info_log=debug_log;
    for (int bank = 0; bank < 31; ++bank) {
        REG32(1972528)&=~33292288u;
        u8 code = (u8)(rf.bta_pll_bank_limit[31] - (u8)bank);
        REG32(1972528)|=((u32)code<<18)&33292288;
        ANALOG_LOG_DEBUG(info_log, "[Info]: [Analog]bak: %x %d \r\n", code, bank);
        s8 dat_buf = dat_l;
        s8 dat_l_ph = dat_l;
        s8 dat_h;
        for (;;) {
            REG32(1972532)&=~255u;
            REG32(1972532)|=(u8)dat_buf;
            REG32(1972516)&=~4u;
            btpll_udelay(8);
            REG32(1972516)|=4;
            btpll_udelay(110);
            int state = (int)((REG32(1972600) |
                               REG32(1972600) |
                               REG32(1972600)) >> 1) & 3;
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
    REG32(1972532)&=~255u;
    REG32(1972532)|=(set_freq-2321)&255;
    u8 bank = get_bta_pll_bank((u8)(set_freq - 2321));
    REG32(1972528)&=~33292288u;
    REG32(1972528)|=((u32)bank<<18)&33292288;
    ANALOG_LOG_INFO(log_tag_const_i_Analog, "[Info]: [Analog]setbak:%x\r\n", bank);
    REG32(1972520)&=~134217728u;
    REG32(1972524) = saved_bank;
    REG32(1972528)=saved_freq;REG32(1972532)=saved_third;
    return bank;
}



u8 BT_RF_CODE bta_pll_bank_set(u8 bank)
{
    return (u8)(rf.bta_pll_bank_limit[31] - bank);
}

void BT_RF_CODE btpll_udelay(u32 us)
{
    u32 count = (u32)(clk_get("sys") / 8000000) * (us < 2 ? 0 : us - 1);
    while (count != 0) {
        --count;
        __asm__ volatile("nop");
    }
}

void BT_RF_CODE mdelay(u32 ms)
{
    for (u32 i = 0; i != ms; ++i)
        btpll_udelay(1000);
}

void BT_RF_CODE BT_CP_EN(u8 en) { REG32(1972480)=(REG32(1972480)&~14336u)|((en&7u)<<11); }

void BT_RF_CODE rxadc_ldo_trim_set(u8 sel) { REG32(1972480)=1151816677;REG32(1972492)=34422272;REG32(1972496)=((u32)sel<<5)|23926277; }



void BT_RF_CODE rxadc_ref_trim_set(int sel) { REG32(1972480)=1151816677;REG32(1972492)=34422272;REG32(1972496)=(((u32)sel<<22)&12582912)|17634981; }



void BT_RF_CODE umixer_vblo_trim_set(int sel) { REG32(1972480)=1151783909;REG32(1972500)=147899803;REG32(1972504)=(((u32)sel<<14)&114688)|(u32)-1698982; }













void BT_RF_CODE bt_vref_init(void) { }
void BT_RF_CODE bt_vref_close(void) { }

u8 BT_RF_CODE bt_get_pwr_max_level(void)
{ if(config_force_bt_pwr_tab_using_normal_level || (config_btctler_mode&33))return 10;return config_btctler_mode&2?6:10; }



void BT_RF_CODE txtrim_analog_init(void)
{
 bt_analog_part_init();
 REG32(1972480) = (REG32(1972480) | 786432);
 REG32(1972484) = (REG32(1972484) & -2049);
 REG32(1972488) = (REG32(1972488) & -2);
 REG32(1972488) = (REG32(1972488) & -449);
 REG32(1972488) = (REG32(1972488) & -2);
 REG32(1972492) = (REG32(1972492) & -1966081);
 REG32(1972492) = (REG32(1972492) & -31745);
 REG32(1972496) = (REG32(1972496) & -5);
 REG32(1972496) = (REG32(1972496) & -65537);
 REG32(1972492) = (REG32(1972492) | 1442816);
 REG32(1972496) = (REG32(1972496) | 5);
 REG32(1972496) = (REG32(1972496) | 65536);
 REG32(1972492) = (REG32(1972492) | 65536);
 REG32(1972500) = (REG32(1972500) | 15);
 REG32(1972500) = (REG32(1972500) | 49280);
 REG32(1972504) = REG32(1972504);
 REG32(1972508) = (REG32(1972508) | 1572880);
 if(config_btctler_mode&4) {
 REG32(1972504) = (REG32(1972504) & -193);
 REG32(1972504) = (REG32(1972504) | 192);
 REG32(1972500) = (REG32(1972500) & -113);
 REG32(1972500) = REG32(1972500);
 }
}





static void BT_RF_CODE bt_analog_part_init(void)
{
 REG32(1972564) = (REG32(1972564) | 4096);
 REG32(1972564) = (REG32(1972564) & -2049);
 REG32(1972564) = (REG32(1972564) & -8193);
 REG32(1972564) = (REG32(1972564) | 16384);
 REG32(1972564) = (REG32(1972564) | 128);
 REG32(1972480) = 1216381925;
 REG32(1972484) = 68976662;
 REG32(1972488) = 34264;
 REG32(1972492) = 8928768;
 REG32(1972496) = 20983394;
 REG32(1972480) = (REG32(1972480) & -1985);
 REG32(1972484) = (REG32(1972484) & -3731457);
 REG32(1972480) = (REG32(1972480) | 832);
 REG32(1972484) = (REG32(1972484) | 1605632);
 REG32(1972500) = 13632792;
 REG32(1972504) = -1649830;
 REG32(1972508) = 15;
 REG32(1972512) = (REG32(1972512) & -522241);
 REG32(1972512) = (REG32(1972512) | 348160);
 REG32(1972520) = 44631775;
 REG32(1972524) = 1107735553;
 u32 target=clock_get_pll_target_frequency();
 btadc_sel(target);
}



void BT_RF_CODE set_ldo_trim_res(u8 first)
{
 int zero=(ldo_trim_res.chargpump_ldo_lev==0)+(ldo_trim_res.divider_ldo_lev==0)+(ldo_trim_res.vco_1ldo_lev==0)+(ldo_trim_res.vco_2ldo_lev==0)+(ldo_trim_res.xosc_ldo_lev==0)+(ldo_trim_res.rxadc_ref_lev==0)+(ldo_trim_res.umx_vblo_lev==0)+(ldo_trim_res.rx_ldo_lev==0);
 u8 logged=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]zero_cnt = %d \r\n",zero);delay(20);
 if(zero>2) {
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>>>>>>> LDO TRIM FAIL : ldo voltage from SARADC abnormal !!!\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]chargpump level: [%d]\n\r\n",ldo_trim_res.chargpump_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]divider   level: [%d]\n\r\n",ldo_trim_res.divider_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]vco1      level: [%d]\n\r\n",ldo_trim_res.vco_1ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]vco2      level: [%d]\n\r\n",ldo_trim_res.vco_2ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]xosc      level: [%d]\n\r\n",ldo_trim_res.xosc_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]rxadc     level: [%d]\n\r\n",ldo_trim_res.rxadc_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]rxadcref  level: [%d]\n\r\n",ldo_trim_res.rxadc_ref_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]umixer    level: [%d]\n\r\n",ldo_trim_res.umx_vblo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]rxldo     level: [%d]\n\r\n",ldo_trim_res.rx_ldo_lev);return;
 }
 int before=first?vco_lock_trim():0;
#define APPLY(field,en,addr,mask,value,good,bad) do { \
 if(ldo_trim_res.en) { REG32(addr)&=~(u32)(mask);u8 level=ldo_trim_res.field;REG32(addr)|=(value);logged=log_tag_const_i_Analog;ANALOG_LOG_INFO(logged,good,level); } \
 else ANALOG_LOG_INFO(logged,bad); \
} while(0)
 APPLY(chargpump_ldo_lev,chargpump_ldo_en,1972524,7168,(level&7u)<<10,"[Info]: [Analog]chargpump level: [%d]\n\r\n","[Info]: [Analog][chargpump] ldo trim abnormal and finish!!!\n\r\n");
 APPLY(divider_ldo_lev,divider_ldo_en,1972524,57344,(level&7u)<<13,"[Info]: [Analog]divider   level: [%d]\n\r\n","[Info]: [Analog][divider]   ldo trim abnormal and finish!!!\n\r\n");
 APPLY(vco_1ldo_lev,vco_1ldo_en,1972524,458752,(level&7u)<<16,"[Info]: [Analog]vco1      level: [%d]\n\r\n","[Info]: [Analog][vco1]      ldo trim abnormal and finish!!!\n\r\n");
 APPLY(vco_2ldo_lev,vco_2ldo_en,1972520,117440512,(level&7u)<<24,"[Info]: [Analog]vco2      level: [%d]\n\r\n","[Info]: [Analog][vco2]      ldo trim abnormal and finish!!!\n\r\n");
 APPLY(xosc_ldo_lev,xosc_ldo_en,1972512,28,(level<<2)&28u,"[Info]: [Analog]xosc      level: [%d]\n\r\n","[Info]: [Analog][xosc]      ldo trim abnormal and finish!!!\n\r\n");
 APPLY(rxadc_ref_lev,rxadc_ref_en,1972496,12582912,(level&3u)<<22,"[Info]: [Analog]rxadcref  level: [%d]\n\r\n","[Info]: [Analog][rxadc]     ref trim abnormal and finish!!!\n\r\n");
 APPLY(umx_vblo_lev,umx_vblo_en,1972504,114688,(level&7u)<<14,"[Info]: [Analog]umixer    level: [%d]\n\r\n","[Info]: [Analog][umx]       vblo trim abnormal and finish!!!\n\r\n");
 APPLY(rx_ldo_lev,rx_ldo_en,1972480,14680064,(level&7u)<<21,"[Info]: [Analog]btrxldo   level: [%d]\n\r\n","[Info]: [Analog][btrxldo]   rxldo trim abnormal and finish!!!\n\r\n");
#undef APPLY
 if(first) {
 delay(50);int after=vco_lock_trim(),diff=before-after;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]temp 1 = %d\n\r\n",before);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]temp 2 = %d\n\r\n",after);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]diff = %d\n\r\n",diff);
 if(diff>4800 && ldo_trim_res.vco_1ldo_en) {
 while(ldo_trim_res.vco_1ldo_en) {
 u8 lev=ldo_trim_res.vco_1ldo_lev;if(lev>=6)return;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]vco1 bad! level : [%d] temp1 = %d, temp2 = %d\n\r\n",lev,before,after);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]diff = %d\n\r\n",diff);
 ++ldo_trim_res.vco_1ldo_lev;REG32(1972524)&=~458752u;REG32(1972524)|=(ldo_trim_res.vco_1ldo_lev&7u)<<16;
 }
 }
 u8 lev=ldo_trim_res.vco_1ldo_lev;if(lev<6) { REG32(1972524)&=~458752u;ldo_trim_res.vco_1ldo_lev=lev+1;REG32(1972524)|=((lev+1u)&7)<<16; }
 }
}

void BT_RF_CODE bt_ldo_trim(void)
{
 u8 logged=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]====================================================================================\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]//                                                                                //\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]//                              bt ldo trim start                                 //\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]//                                                                                //\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]====================================================================================\n\r\n");
 u32 a=REG32(1972480);delay(20);u32 b=REG32(1972484);delay(20);u32 c=REG32(1972488);delay(20);u32 d=REG32(1972492);delay(20);u32 e=REG32(1972496);delay(20);u32 f=REG32(1972500);delay(20);u32 g=REG32(1972504);delay(20);u32 h=REG32(1972512);delay(20);u32 j=REG32(1972516);delay(20);u32 k=REG32(1972520);delay(20);(void)j;(void)k;
 ldo_trim_res.rx_ldo_lev=0;typedef u64 low64 __attribute__((aligned(1),may_alias));*(low64 *)(void *)&ldo_trim_res=0;
 typedef u16 low16 __attribute__((aligned(1),may_alias));
 low16 *flags=(void *)((u8 *)&ldo_trim_res+11);*flags&=0xfd00;

 local_irq_disable();btpll_trim_en(1,2);delay(20);logged=log_tag_const_i_Analog;
#define SCAN(field,en,setter,select,limit,cap,min,max,refresh,start,err) do { \
 do { if(refresh)logged=log_tag_const_i_Analog;ANALOG_LOG_INFO(logged,start,ldo_trim_res.field);setter(ldo_trim_res.field);delay(2000);if(get_ldo_voltage(select)>(limit))break;ldo_trim_res.field++; } while(ldo_trim_res.field<(cap)+1); \
 if(ldo_trim_res.field>(cap))ldo_trim_res.field=(cap);ldo_trim_res.en=ldo_trim_res.field>=(min)&&ldo_trim_res.field<=(max); \
 if(!ldo_trim_res.en)ANALOG_LOG_INFO((refresh)?log_tag_const_i_Analog:logged,err); \
} while(0)
 SCAN(chargpump_ldo_lev,chargpump_ldo_en,charg_pump_ldo_trim_set,2,1050,7,2,5,0,"[Info]: [Analog]chargpump_ldo_lev: %d\n\r\n","[Info]: [Analog]CP LDO TRIM ABNORMAL\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>chargpump_ldo_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.chargpump_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");
 btpll_trim_en(1,3);delay(20);logged=log_tag_const_i_Analog;
 SCAN(divider_ldo_lev,divider_ldo_en,divider_ldo_trim_set,2,1050,7,2,5,0,"[Info]: [Analog]divider_ldo_lev: %d\n\r\n","[Info]: [Analog]DIVIDER LDO TRIM ABNORMAL\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>divider_ldo_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.divider_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");
 btpll_trim_en(1,4);delay(20);logged=log_tag_const_i_Analog;
 SCAN(vco_1ldo_lev,vco_1ldo_en,vco_first_ldo_trim_set,2,1050,7,2,5,0,"[Info]: [Analog]vco_1ldo_lev: %d\n\r\n","[Info]: [Analog]VCO1 LDO TRIM ABNORMAL\n\r\n");
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>vco_1_ldo_trim finish: level is [%d]<<<\n\r\n",ldo_trim_res.vco_1ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");
 btpll_trim_en(1,5);delay(20);
 SCAN(vco_2ldo_lev,vco_2ldo_en,vco_second_ldo_trim_set,2,900,7,1,4,1,"[Info]: [Analog]vco_2ldo_lev: %d\n\r\n","[Info]: [Analog]VCO2 LDO TRIM ABNORMAL\n\r\n");
 logged=log_tag_const_i_Analog;ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>vco_2_ldo_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.vco_2ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");
 btpll_trim_en(0,0);delay(20);
 SCAN(xosc_ldo_lev,xosc_ldo_en,xosc_ldo_trim_set,1,1050,7,2,5,1,"[Info]: [Analog]xosc_ldo_lev: %d\n\r\n","[Info]: [Analog]XOSC LDO TRIM ABNORMAL\n\r\n");
 REG32(1972512)=h;delay(20);
 SCAN(rxadc_ref_lev,rxadc_ref_en,rxadc_ref_trim_set,2,850,3,0,3,1,"[Info]: [Analog]rxadc_ref_lev: %d\n\r\n","[Info]: [Analog]RXADC REF TRIM ABNORMAL\n\r\n");
 REG32(1972480)=a;REG32(1972492)=d;REG32(1972496)=e;logged=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>rxadc_ref_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.rxadc_ref_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");delay(20);
 SCAN(umx_vblo_lev,umx_vblo_en,umixer_vblo_trim_set,2,1050,7,2,5,0,"[Info]: [Analog]umx_vblo_lev: %d\n\r\n","[Info]: [Analog]UMX VBLO TRIM ABNORMAL\n\r\n");
 REG32(1972480)=a;REG32(1972500)=f;REG32(1972504)=g;logged=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>umx_vblo_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.umx_vblo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");delay(20);
 SCAN(rx_ldo_lev,rx_ldo_en,rx_ldo_trim_set,2,1100,7,3,6,0,"[Info]: [Analog]rx_ldo_lev: %d\n\r\n","[Info]: [Analog]RX LDO TRIM ABNORMAL\n\r\n");
 REG32(1972480)=a;REG32(1972484)=b;REG32(1972488)=c;logged=log_tag_const_i_Analog;
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]>>>btrx_ldo_trim finish : level is [%d]<<<\n\r\n",ldo_trim_res.rx_ldo_lev);
 ANALOG_LOG_INFO(logged,"[Info]: [Analog]======================================================================================\n\r\n");BT_CP_EN(0);local_irq_enable();
#undef SCAN
}
#undef RUN_LDO_TRIM

u32 BT_RF_CODE get_ldo_voltage(u8 sel)
{
 BT_CP_EN(0);adc_pmu_detect_en(1);adc_sample(13);u32 vbg=0,ldo=0;
 for(int i=0;i<10;i++) { while((s8)REG32(1978624)>=0){}vbg+=REG32(1978628);REG32(1978624)|=64; }
 BT_CP_EN(sel);delay(200);adc_sample(15);
 for(int i=0;i<10;i++) { while((s8)REG32(1978624)>=0){}ldo+=REG32(1978628);REG32(1978624)|=64; }
 vbg/=10;ldo/=10;
 if(log_tag_const_i_Analog) { log_print(2,0,"[Info]: [Analog]adc_vbg = %d\n\r\n",vbg);log_print(2,0,"[Info]: [Analog]adc_res = %d\n\r\n",ldo);
 u32 trim=get_vbg_trim(),delta=(trim&31)*3;u32 ref=(trim&32)?801-delta:801+delta;
 log_print(2,0,"[Info]: [Analog]====== %d \r\n",ref*ldo/vbg); }
 u32 trim=get_vbg_trim(),delta=(trim&31)*3;u32 ref=(trim&32)?801-delta:801+delta;return ref*ldo/vbg;
}


void BT_RF_CODE mesh_certification_api(u8 en) { config_mesh_certification=en; }

void BT_RF_CODE spur_control(void) { REG32(1900004)|=1;ANALOG_LOG_INFO(log_tag_const_i_Analog,"[Info]: [Analog]MDM12_Value:%x\n\r\n",REG32(1900004)); }

void BT_RF_CODE bta_set_amp_sepa(u8 mode) { (void)mode; }

void BT_RF_CODE bta_gain_init(void)
{
 u8 count=bt_get_pwr_max_level()+1;REG32(1899692)=180;
 if(count) { int mode=config_btctler_mode;for(u32 i=0;i<count;i++) {
 REG32(1899696)=0;delay(10);
 const u32 *tab=config_force_bt_pwr_tab_using_normal_level?TXPWR_table:mode&4?TXPWR_table_fcc:mode&2?TXPWR_table_bqb:TXPWR_table;
 REG32(1899696)=tab[i];delay(10);REG32(1899696)=TXSET_table[i];delay(10);
 } }
}

void BT_RF_CODE charg_pump_ldo_trim_set(u8 sel) { REG32(1972520)&=~117440512u;REG32(1972524)&=~523264u;REG32(1972520)|=50331648;REG32(1972524)=REG32(1972524)|((sel&7u)<<10)|434176; }

void BT_RF_CODE divider_ldo_trim_set(u8 sel) { REG32(1972520)&=~117440512u;REG32(1972524)&=~523264u;REG32(1972520)|=50331648;REG32(1972524)=REG32(1972524)|((sel&7u)<<13)|398336; }

void BT_RF_CODE vco_first_ldo_trim_set(u8 sel) { REG32(1972520)&=~117440512u;REG32(1972524)&=~523264u;REG32(1972520)|=50331648;REG32(1972524)=REG32(1972524)|((sel&7u)<<16)|46080; }

void BT_RF_CODE vco_second_ldo_trim_set(u8 sel) { REG32(1972520)&=~117440512u;REG32(1972524)&=~523264u;REG32(1972520)|=(sel&7u)<<24;REG32(1972524)|=439296; }

void BT_RF_CODE xosc_ldo_trim_set(u8 sel) { REG32(1972512)&=~14680094u;REG32(1972512)=REG32(1972512)|((sel<<2)&28u)|6291456; }

void BT_RF_CODE rx_ldo_trim_set(u8 sel) { REG32(1972480)=((sel&7u)<<21)|1225773797;REG32(1972484)|=4194304;REG32(1972488)|=1; }

void BT_RF_CODE btpll_trim_en(u8 en,u8 sel) { REG32(1972524)&=~7864320u;REG32(1972524)=REG32(1972524)|(en?((sel&7u)<<20)|((en&1u)<<19):0); }

void BT_RF_CODE rf_mdm_con8_config(int mode) { REG32(1899552)=mode?911562069:911562039; }

void BT_RF_CODE rf_mdm_con_ble_sync_word(int mode) { REG32(1899524)&=~31u;REG32(1899524)|=mode?29:(CONFIG_BLE_SYNC_WORD_BIT&31); }

void BT_RF_CODE clk_out_debug(void) { REG32(1986568)&=~1u;REG32(1966092)&=~7168u;REG32(1986824)&=~16u;REG32(1966092)=(REG32(1966092)&~57344u)|49152; }

void BT_RF_CODE btadc_sel(u32 n)
{
 switch(n) {
 case 96:REG32(1984268)|=32;REG32(1984268)&=~448u;REG32(1972496)|=256;REG32(1972496)|=33554432;REG32(1972496)&=~512u;break;
 case 240:case 144:REG32(1984268)&=~32u;REG32(1972496)&=~256u;REG32(1972496)&=~33554432u;REG32(1972496)|=512;break;
 case 192:REG32(1984268)|=32;REG32(1984268)=(REG32(1984268)&~448u)|256;REG32(1972496)|=256;REG32(1972496)|=33554432;REG32(1972496)&=~512u;break;
 default:printf("btadc clock set error, no this option ,jl fix dvdd !!!");break;
 }
}

static int BT_RF_CODE vco_lock_trim(void)
{
 u32 a=REG32(1972480),b=REG32(1972484),c=REG32(1972488),d=REG32(1972492),e=REG32(1972496),f=REG32(1972500),g=REG32(1972504),h=REG32(1972508);
 txtrim_analog_init();REG32(1899968)=0;REG32(1899972)=0;REG32(1899976)=0;
 REG32(1899680)|=65536;delay(2);REG32(1899644)|=2;delay(2);REG32(1899644)|=4;delay(2);
 REG32(1899976)|=21;delay(2);REG32(1899976)|=33554432;delay(2);REG32(1899976)=REG32(1899976);delay(2);REG32(1899976)=REG32(1899976);delay(2);
 REG32(1899968)|=42;delay(2);REG32(1899968)|=256;delay(2);REG32(1899968)&=~512u;delay(2);REG32(1899968)|=3072;delay(2);
 REG32(1899968)|=65536;delay(2);REG32(1899968)|=131072;delay(2);REG32(1899968)|=2097152;delay(2);
 REG32(1899972)|=128;delay(2);REG32(1899972)|=64;delay(2);REG32(1899556)|=2048;
 REG32(1899536)&=~65280u;REG32(1899536)=REG32(1899536);REG32(1899536)&=~255u;REG32(1899536)|=120;delay(10);
 for(int i=0;i<3;i++) { REG32(1899968)&=~131072u;delay(2);REG32(1899968)|=131072;while(REG32(1899980)&67108864){} }
 u32 sum=0;
 for(int i=0;i<25;i++) { REG32(1899968)&=~131072u;delay(2);REG32(1899968)|=131072;while(REG32(1899980)&67108864){}
 s32 x=(s32)(REG32(1899984)<<8)>>16,y=(s32)(REG32(1899988)<<8)>>16;sum+=(u32)(x*x)+(u32)(y*y); }
 REG32(1899536)&=~16711680u;REG32(1899536)|=(trim_info.txpsk3&255u)<<16;
 REG32(1899536)&=~65280u;REG32(1899536)|=(u32)trim_info.txpuk2<<8;
 REG32(1899536)&=~255u;REG32(1899536)|=trim_info.txpuk1&255;
 REG32(1899680)&=~65536u;delay(2);REG32(1899644)&=~2u;delay(2);REG32(1899644)&=~4u;delay(2);
 REG32(1899968)=0;REG32(1899972)=0;REG32(1899976)=0;delay(2);REG32(1899556)&=~2048u;
 REG32(1972480)=a;REG32(1972484)=b;REG32(1972488)=c;REG32(1972492)=d;REG32(1972496)=e;REG32(1972500)=f;REG32(1972504)=g;REG32(1972508)=h;return sum/25;
}

typedef s8 i8;typedef s16 i16;typedef s32 i32;
static __attribute__((noinline)) void BT_RF_CODE set_bt_trim_mode(u8 mode)
{
    u32 v0 = mode;
    u32 v2[65];
    u8 v3[37];
    u8 *v4;
    u32 v7;
    u32 v60;
    u32 v62;
    u32 v63;
    u16 v64;
    u16 v65;
    u16 v66;
    u32 v68;
    u32 v69;
    u16 v70;
    u32 v76;
    u32 v83;
    u32 v89;
    u32 v98;
    u32 v100;
    u32 v113;
    u32 v116;
    u32 v119;
    u32 v120;
    u16 v122;
    u32 v123;
    u32 v124;
    u32 v130;
    u32 v138;
    u16 v145;
    u16 v146;
    u32 v147;
    u32 v148;
    u32 v213;
    u32 v214;
    u16 v215;
    u32 v229;
    u32 v232;
    u32 v235;
    u32 v236;
    u16 v238;
    u32 v239;
    u32 v240;
    u32 v245;
    u16 v259;
    u32 v260;
    u16 v261;
    u32 v278;
    u32 v281;
    u32 v284;
    u32 v285;
    u16 v286;
    u32 v287;
    u16 v288;
    u32 v293;
    u16 v301;
    u16 v303;
    u16 v305;
    u16 v306;
    u16 v307;
    u16 v308;
    u16 v316;
    u16 v318;
    u16 v319;
    u16 v320;
    u16 v328;
    u16 v330;
    u32 v377;
    u16 v392;
    u16 v397;
    u32 v400;
    u16 v401;
    u16 v402;
    u32 v403;
    u32 v407;
    u32 v408;
    u16 v409;
    u16 v410;
    u32 v411;
    u16 v413;
    u16 v418;
    u32 v422;
    u32 v423;
    u32 v425;
    u32 v434;
    u32 v435;
    u32 v436;
    u32 v439;
    u32 v440;
    u32 v441;
    u32 v442;
    u32 v446;
    u32 v448;
    u32 v453;
    u16 v454;
    u32 v458;
    u32 v463;
    u16 v464;
    u16 v468;
    u16 v469;
    u32 v470;
    u32 v471;
    u32 v487;
    u32 v500;
    u32 v567;
    u32 v568;
    u16 v569;
    u32 v573;
    u16 v574;
    u32 v586;
    u32 v589;
    u32 v593;
    u16 v594;
    u32 v597;
    u32 v598;
    u16 v600;
    u32 v601;
    u32 v602;
    u32 v607;
    u32 v621;
    u16 v622;
    u16 v623;
    u32 v630;
    u16 v631;
    u32 v643;
    u32 v646;
    u32 v650;
    u16 v651;
    u32 v654;
    u32 v655;
    u16 v656;
    u32 v657;
    u16 v658;
    u32 v663;
    u32 v683;
    volatile u32 *v472;
    v4 = v3;
    bt_fre_offset_value_init();
    if ((v0 == 0)) {
        goto L_6;
    } else {
        goto L_15;
    }
L_6: ;
    v7 = syscfg_read(93, v4, 37);
    if (((v7 & 255) == 37)) {
        goto L_10;
    } else {
        goto L_15;
    }
L_10: ;
    memcpy(&trim_info, v4, 24);
    memcpy(&ldo_trim_res.chargpump_ldo_lev, &v3[24], 13);
    if (((*(u16 *)(&trim_info.version)) == 4098)) {
        goto L_14;
    } else {
        goto L_15;
    }
L_14: ;
    reset_trim_info();
    set_ldo_trim_res(0);
    goto L_684;
L_15: ;
    if ((log_tag_const_i_Analog == 0)) {
        goto L_19;
    } else {
        goto L_18;
    }
L_18: ;
    log_print(2, 0, "[Info]: [Analog]\n ------- bt trim -------\n\r\n");
    goto L_19;
L_19: ;
    *(u16 *)(&trim_info.version) = 4098;
    txtrim_analog_init();
    REG32(1899976) = 0;
    delay(2);
    REG32(1899972) = 0;
    delay(2);
    REG32(1899968) = 0;
    delay(2);
    REG32(1899680) = (REG32(1899680) | 65536);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 2);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 4);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 32);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 33554432);
    delay(2);
    delay(2);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 32);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 256);
    delay(2);
    REG32(1899968) = (REG32(1899968) & -513);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 3072);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 65536);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 2097152);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 128);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 64);
    delay(2);
    REG32(1899556) = (REG32(1899556) | 2048);
    REG32(1899968) = (REG32(1899968) & -131073);
    delay(5);
    REG32(1899968) = (REG32(1899968) | 131072);
    goto L_54;
L_54: ;
    if (((REG32(1899980) & 67108864) == 0)) {
        goto L_58;
    } else {
        goto L_54;
    }
L_58: ;
    v60 = log_tag_const_i_Analog == 0;
    v62 = 3;
    v63 = -1;
    v64 = 0;
    v65 = 0;
    v66 = 0;
    goto L_61;
L_61: ;
    v68 = -100;
    v69 = v63;
    v70 = v66;
    goto L_67;
L_67: ;
    switch (v62) {
    case 0:
        goto L_71;
    case 1:
        goto L_77;
    case 2:
        goto L_84;
    case 3:
        goto L_90;
    default:
        goto L_101;
    }
L_71: ;
    REG32(1899528) = (REG32(1899528) & -1024);
    v76 = REG32(1899528) | (v68 & 1023);
    v100 = v76;
    goto L_99;
L_77: ;
    REG32(1899528) = (REG32(1899528) & -1047553);
    v83 = REG32(1899528) | ((v68 << 10) & 1047552);
    v100 = v83;
    goto L_99;
L_84: ;
    REG32(1899528) = (REG32(1899528) & -1024);
    v89 = REG32(1899528) | (v68 & 1023);
    v100 = v89;
    goto L_99;
L_90: ;
    REG32(1899528) = (REG32(1899528) & -1047553);
    REG32(1899528) = (REG32(1899528) | ((v68 << 10) & 1047552));
    v98 = REG32(1899528) & -1024;
    v100 = v98;
    goto L_99;
L_99: ;
    REG32(1899528) = v100;
    goto L_101;
L_101: ;
    REG32(1899968) = (REG32(1899968) & -131073);
    delay(5);
    REG32(1899968) = (REG32(1899968) | 131072);
    goto L_106;
L_106: ;
    if (((REG32(1899980) & 67108864) == 0)) {
        goto L_110;
    } else {
        goto L_106;
    }
L_110: ;
    v113 = ((s32)((REG32(1899984) << 8)) >> (16));
    v116 = ((s32)((REG32(1899988) << 8)) >> (16));
    v119 = (v116 * v116) + (v113 * v113);
    v120 = v119 < v69;
    v122 = (v120 ? (i16)v68 : v70);
    v123 = (v120 ? v119 : v69);
    v124 = v68 + 1;
    if ((v124 == 100)) {
        goto L_126;
    } else {
        v68 = v124;
        v69 = v123;
        v70 = v122;
        goto L_67;
    }
L_126: ;
    switch (v62) {
    case 2:
        goto L_127;
    case 0:
        goto L_127;
    case 1:
        goto L_135;
    case 3:
        goto L_135;
    default:
        v145 = v65;
        v146 = v64;
        v147 = v123;
        goto L_144;
    }
L_127: ;
    REG32(1899528) = (REG32(1899528) & -1024);
    v130 = (s32)(s16)v122;
    REG32(1899528) = (REG32(1899528) | (v130 & 1023));
    if (v60) {
        v145 = v122;
        v146 = v64;
        v147 = -1;
        goto L_144;
    } else {
        goto L_134;
    }
L_134: ;
    log_print(2, 0, "[Info]: [Analog]I trim ok,value : %d \n\r\n", (s32)v130);
    v145 = v122;
    v146 = v64;
    v147 = -1;
    goto L_144;
L_135: ;
    REG32(1899528) = (REG32(1899528) & -1047553);
    v138 = (s32)(s16)v122;
    REG32(1899528) = (REG32(1899528) | ((v138 << 10) & 1047552));
    if (v60) {
        v145 = v65;
        v146 = v122;
        v147 = -1;
        goto L_144;
    } else {
        goto L_143;
    }
L_143: ;
    log_print(2, 0, "[Info]: [Analog]Q trim ok,value : %d \n\r\n", (s32)v138);
    v145 = v65;
    v146 = v122;
    v147 = -1;
    goto L_144;
L_144: ;
    v148 = v62 + -1;
    if ((((s32)(v148) > (s32)(-1)))) {
        v62 = v148;
        v63 = v147;
        v64 = v146;
        v65 = v145;
        v66 = v122;
        goto L_61;
    } else {
        goto L_150;
    }
L_150: ;
    if ((log_tag_const_i_Analog == 0)) {
        goto L_156;
    } else {
        goto L_153;
    }
L_153: ;
    log_print(2, 0, "[Info]: [Analog]I_dc : %d  Q_dc : %d\n\r\n", (s32)(s16)v145, (s32)(s16)v146);
    goto L_156;
L_156: ;
    *(u16 *)(&trim_info.txi_dc) = v145;
    *(u16 *)(&trim_info.txq_dc) = v146;
    REG32(1899680) = (REG32(1899680) & -65537);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -3);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -5);
    delay(2);
    REG32(1899968) = 0;
    REG32(1899972) = 0;
    REG32(1899976) = 0;
    delay(2);
    REG32(1899556) = (REG32(1899556) & -2049);
    txtrim_analog_init();
    REG32(1899968) = 0;
    REG32(1899972) = 0;
    REG32(1899976) = 0;
    REG32(1899680) = (REG32(1899680) | 65536);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 2);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 4);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 21);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 33554432);
    delay(2);
    REG32(1899976) = REG32(1899976);
    delay(2);
    REG32(1899976) = REG32(1899976);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 42);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 256);
    delay(2);
    REG32(1899968) = (REG32(1899968) & -513);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 3072);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 65536);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 2097152);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 128);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 64);
    delay(2);
    REG32(1899556) = (REG32(1899556) | 2048);
    REG32(1899536) = (REG32(1899536) & -65281);
    REG32(1899536) = (REG32(1899536) | 16384);
    REG32(1899968) = (REG32(1899968) & -131073);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    goto L_205;
L_205: ;
    if (((REG32(1899980) & 67108864) == 0)) {
        goto L_209;
    } else {
        goto L_205;
    }
L_209: ;
    REG32(1899536) = (REG32(1899536) & -256);
    v213 = 30;
    v214 = -1;
    v215 = 0;
    goto L_212;
L_212: ;
    REG32(1899536) = (REG32(1899536) | v213);
    REG32(1899968) = (REG32(1899968) & -131073);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    goto L_222;
L_222: ;
    if (((REG32(1899980) & 67108864) == 0)) {
        goto L_226;
    } else {
        goto L_222;
    }
L_226: ;
    v229 = ((s32)((REG32(1899984) << 8)) >> (16));
    v232 = ((s32)((REG32(1899988) << 8)) >> (16));
    v235 = (v232 * v232) + (v229 * v229);
    v236 = v235 < v214;
    v238 = (v236 ? (i16)v213 : v215);
    v239 = (v236 ? v235 : v214);
    v240 = v213 + 1;
    REG32(1899536) = (REG32(1899536) & -256);
    if ((v240 == 90)) {
        goto L_244;
    } else {
        v213 = v240;
        v214 = v239;
        v215 = v238;
        goto L_212;
    }
L_244: ;
    v245 = (s32)(s16)v238;
    REG32(1899536) = (REG32(1899536) | (v245 & 255));
    if ((log_tag_const_i_Analog == 0)) {
        goto L_252;
    } else {
        goto L_251;
    }
L_251: ;
    log_print(2, 0, "[Info]: [Analog]TX_CMPUK1:%d\n\r\n", (s32)v245);
    goto L_252;
L_252: ;
    *(u16 *)(&trim_info.txpuk1) = v238;
    REG32(1899536) = (REG32(1899536) & -16711681);
    REG32(1899536) = REG32(1899536);
    REG32(1899536) = (REG32(1899536) & -16711681);
    v259 = -30;
    v260 = -1;
    v261 = v238;
    goto L_258;
L_258: ;
    REG32(1899536) = (REG32(1899536) | ((v259 & 255) << 16));
    REG32(1899968) = (REG32(1899968) & -131073);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    goto L_271;
L_271: ;
    if (((REG32(1899980) & 67108864) == 0)) {
        goto L_275;
    } else {
        goto L_271;
    }
L_275: ;
    v278 = ((s32)((REG32(1899984) << 8)) >> (16));
    v281 = ((s32)((REG32(1899988) << 8)) >> (16));
    v284 = (v281 * v281) + (v278 * v278);
    v285 = v284 < v260;
    v286 = (v285 ? v259 : v261);
    v287 = (v285 ? v284 : v260);
    v288 = v259 + 1;
    REG32(1899536) = (REG32(1899536) & -16711681);
    if ((v288 == 30)) {
        goto L_292;
    } else {
        v259 = v288;
        v260 = v287;
        v261 = v286;
        goto L_258;
    }
L_292: ;
    v293 = (s32)(s16)v286;
    REG32(1899536) = (REG32(1899536) | (((v293 << 16) + 16711680) & 16711680));
    if ((v238 < 64)) {
        goto L_300;
    } else {
        goto L_302;
    }
L_300: ;
    v301 = 64 / v238;
    v305 = v301;
    goto L_304;
L_302: ;
    v303 = v238 >> 6;
    v305 = v303;
    goto L_304;
L_304: ;
    v306 = v305 * 10;
    v307 = 1200 / v306;
    v308 = v307 * 10;
    if ((((s32)((v308 + (v307 * -10))) > (s32)(4)))) {
        goto L_314;
    } else {
        v318 = v307;
        goto L_317;
    }
L_314: ;
    v316 = (v308 + 10) / 10;
    v318 = v316;
    goto L_317;
L_317: ;
    v319 = 1500 / v306;
    v320 = v319 * 10;
    if ((((s32)((v320 + (v319 * -10))) > (s32)(4)))) {
        goto L_326;
    } else {
        v330 = v319;
        goto L_329;
    }
L_326: ;
    v328 = (v320 + 10) / 10;
    v330 = v328;
    goto L_329;
L_329: ;
    if ((log_tag_const_i_Analog == 0)) {
        goto L_334;
    } else {
        goto L_333;
    }
L_333: ;
    log_print(2, 0, "[Info]: [Analog]TX_CMPSK3:%d\n\r\n", (s32)v293);
    goto L_334;
L_334: ;
    *(u16 *)(&trim_info.txpsk3) = v286;
    REG32(1899536) = (REG32(1899536) & -65281);
    REG32(1899536) = (REG32(1899536) | 30720);
    REG32(1899536) = (REG32(1899536) & -256);
    REG32(1899536) = (REG32(1899536) | (v318 & 255));
    *(u16 *)(&trim_info.txpuk2) = 120;
    *(u16 *)(&trim_info.txpuk1) = v318;
    *(u16 *)(&trim_info.txpuk2_1) = 150;
    *(u16 *)(&trim_info.txpuk1_1) = v330;
    REG32(1899680) = (REG32(1899680) & -65537);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -3);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -5);
    delay(2);
    REG32(1899968) = 0;
    REG32(1899972) = 0;
    REG32(1899976) = 0;
    delay(2);
    REG32(1899556) = (REG32(1899556) & -2049);
    rxtrim_analog_init();
    REG32(1972488) = (REG32(1972488) & -449);
    REG32(1972488) = (REG32(1972488) | 448);
    REG32(1972492) = (REG32(1972492) & -1966081);
    REG32(1972492) = (REG32(1972492) | 1376256);
    REG32(1972484) = (REG32(1972484) & -2049);
    REG32(1972488) = (REG32(1972488) | 1);
    REG32(1972484) = (REG32(1972484) & -2);
    REG32(1899644) = (REG32(1899644) | 2);
    REG32(1899680) = (REG32(1899680) | 65536);
    REG32(1899644) = (REG32(1899644) | 16);
    REG32(1899556) = REG32(1899556);
    REG32(1899520) = (REG32(1899520) & -201326593);
    v377 = v2;
    REG32(1899708) = v377;
    memset(v2, 0, 256);
    REG32(1972488) = (REG32(1972488) & -33423361);
    REG32(1972488) = REG32(1972488);
    REG32(1972492) = (REG32(1972492) & -256);
    REG32(1972492) = REG32(1972492);
    REG32(1972492) = (REG32(1972492) & -513);
    REG32(1972492) = (REG32(1972492) | 512);
    v392 = 0;
    goto L_391;
L_388: ;
    v400 = 0;
    v401 = 0;
    v402 = 0;
    /* SDK IR supplies undef on this edge; zero is one permitted value.
     * See revisit-bd19.md before changing the calibration recurrence. */
    v403 = 0;
    goto L_399;
L_391: ;
    REG32(1899704) = 4194304;
    goto L_393;
L_393: ;
    if ((REG32(1899704) > 65535)) {
        goto L_393;
    } else {
        goto L_396;
    }
L_396: ;
    v397 = v392 + 1;
    if ((v397 == 20)) {
        goto L_388;
    } else {
        v392 = v397;
        goto L_391;
    }
L_399: ;
    v407 = v403;
    v408 = 0;
    v409 = v402;
    v410 = v401;
    v411 = 0;
    goto L_406;
L_406: ;
    v413 = 0;
    goto L_412;
L_412: ;
    REG32(1899704) = (v400 | 4194304);
    goto L_414;
L_414: ;
    if ((REG32(1899704) > 65535)) {
        goto L_414;
    } else {
        goto L_417;
    }
L_417: ;
    v418 = v413 + 1;
    if ((v418 == 2)) {
        goto L_420;
    } else {
        v413 = v418;
        goto L_412;
    }
L_420: ;
    v422 = 63;
    v423 = 0;
    goto L_421;
L_421: ;
    v425 = v2[v422];
    v434 = ((((s32)(((((s32)((v425 << 16)) >> (3))) + (v423 * 4088))) / (s32)(4096))) * 4088) + ((u32)((s32)v425 >> 16) << 13);
    v435 = ((s32)(v434) / (s32)(4096));
    v436 = v422 + -1;
    if ((((s32)(v436) > (s32)(0)))) {
        v422 = v436;
        v423 = v435;
        goto L_421;
    } else {
        goto L_438;
    }
L_438: ;
    v439 = ((s32)(v434) / (s32)(524288));
    v440 = ((s32)(v434) < (s32)(-524287));
    v441 = 0 - v439;
    v442 = (v440 ? v441 : v439);
    v446 = ((((s32)(v442) > (s32)(120))) ? 4 : ((((s32)(v442) > (s32)(60))) ? 2 : 1));
    if ((v400 == 0)) {
        goto L_447;
    } else {
        goto L_457;
    }
L_447: ;
    v448 = REG32(1972488);
    REG32(1972488) = (v440 ? (v448 | 33554432) : (v448 & -33554433));
    v453 = v446 + v409;
    v454 = (i16)v453;
    REG32(1972488) = (REG32(1972488) & -33423361);
    v468 = v454;
    v469 = v410;
    v470 = v453;
    v471 = 17;
    v472 = 1972488;
    goto L_467;
L_457: ;
    v458 = REG32(1972492);
    REG32(1972492) = (v440 ? (v458 | 256) : (v458 & -257));
    v463 = v446 + v410;
    v464 = (i16)v463;
    REG32(1972492) = (REG32(1972492) & -256);
    v468 = v409;
    v469 = v464;
    v470 = v463;
    v471 = 0;
    v472 = 1972492;
    goto L_467;
L_467: ;
    *v472 = (*v472 | ((v470 & 255) << v471));
    if ((((((s32)(((((s32)(v434) > (s32)(-524288))) ? v439 : v441)) < (s32)(26))) | (((v439 * v407) >> 31) & v411)) == 0)) {
        goto L_486;
    } else {
        goto L_498;
    }
L_486: ;
    v487 = v408 + 1;
    if ((((s32)(v408) > (s32)(3000)))) {
        goto L_489;
    } else {
        v407 = v439;
        v408 = v487;
        v409 = v468;
        v410 = v469;
        v411 = 1;
        goto L_406;
    }
L_489: ;
    if ((log_tag_const_e_Analog == 0)) {
        goto L_491;
    } else {
        goto L_490;
    }
L_490: ;
    log_print(4, 0, "<Error>: [Analog]rx dc_trim error!!!\n\r\n");
    goto L_491;
L_491: ;
    REG32(1972488) = (REG32(1972488) & -33423361);
    REG32(1972488) = REG32(1972488);
    REG32(1972492) = (REG32(1972492) & -256);
    REG32(1972492) = REG32(1972492);
    goto L_499;
L_498: ;
    goto L_499;
L_499: ;
    v500 = v400 + 1;
    if ((v500 == 2)) {
        goto L_502;
    } else {
        v400 = v500;
        v401 = v469;
        v402 = v468;
        v403 = v439;
        goto L_399;
    }
L_502: ;
    *(u16 *)(&trim_info.rxi_trim_cnt) = v468;
    *(u16 *)(&trim_info.rxq_trim_cnt) = v469;
    REG32(1899644) = (REG32(1899644) & -3);
    REG32(1899680) = (REG32(1899680) & -65537);
    REG32(1899644) = (REG32(1899644) & -17);
    REG32(1899520) = (REG32(1899520) | 201326592);
    rxtrim_analog_init();
    REG32(1899976) = 0;
    delay(2);
    REG32(1899972) = 0;
    delay(2);
    REG32(1899968) = 0;
    delay(2);
    REG32(1899680) = (REG32(1899680) | 65536);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 2);
    delay(2);
    REG32(1899644) = (REG32(1899644) | 4);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 32);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 33554432);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 65536);
    delay(2);
    REG32(1899976) = (REG32(1899976) | 1048576);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 32);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 256);
    delay(2);
    REG32(1899968) = (REG32(1899968) & -513);
    delay(2);
    REG32(1899968) = REG32(1899968);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 65536);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 131072);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 2097152);
    delay(2);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 16777216);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 33554432);
    delay(2);
    REG32(1899968) = (REG32(1899968) | 536870912);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 128);
    delay(2);
    REG32(1899972) = (REG32(1899972) | 64);
    if ((log_tag_const_i_Analog == 0)) {
        goto L_551;
    } else {
        goto L_550;
    }
L_550: ;
    log_print(2, 0, "[Info]: [Analog]2\n\r\n");
    goto L_551;
L_551: ;
    delay(2);
    REG32(1899556) = (REG32(1899556) | 2048);
    if ((log_tag_const_i_Analog == 0)) {
        goto L_557;
    } else {
        goto L_556;
    }
L_556: ;
    log_print(2, 0, "[Info]: [Analog]3\n\r\n");
    goto L_557;
L_557: ;
    REG32(1899540) = (REG32(1899540) & -133955585);
    delay(5);
    REG32(1899540) = (REG32(1899540) | 66846720);
    delay(5);
    REG32(1899540) = (REG32(1899540) & -261633);
    delay(20);
    REG32(1899540) = (REG32(1899540) & -512);
    delay(5);
    v567 = 220;
    v568 = -1;
    v569 = 0;
    goto L_566;
L_566: ;
    REG32(1899540) = (REG32(1899540) | v567);
    delay(20);
    v573 = 0;
    v574 = 0;
    goto L_572;
L_572: ;
    REG32(1899968) = (REG32(1899968) & -33554433);
    delay(5);
    REG32(1899968) = (REG32(1899968) | 33554432);
    goto L_579;
L_579: ;
    if (((REG32(1899992) & 67108864) == 0)) {
        goto L_583;
    } else {
        goto L_579;
    }
L_583: ;
    v586 = ((s32)((REG32(1899996) << 8)) >> (16));
    v589 = ((s32)((REG32(1900000) << 8)) >> (16));
    v593 = ((v586 * v586) + v573) + (v589 * v589);
    v594 = v574 + 1;
    if ((v594 == 20)) {
        goto L_596;
    } else {
        v573 = v593;
        v574 = v594;
        goto L_572;
    }
L_596: ;
    v597 = ((s32)(v593) / (s32)(20));
    v598 = v597 < v568;
    v600 = (v598 ? (i16)v567 : v569);
    v601 = (v598 ? v597 : v568);
    v602 = v567 + 1;
    REG32(1899540) = (REG32(1899540) & -512);
    delay(5);
    if ((v602 == 280)) {
        goto L_606;
    } else {
        v567 = v602;
        v568 = v601;
        v569 = v600;
        goto L_566;
    }
L_606: ;
    v607 = (s32)(s16)v600;
    REG32(1899540) = (REG32(1899540) | (v607 & 511));
    delay(20);
    if ((log_tag_const_i_Analog == 0)) {
        goto L_614;
    } else {
        goto L_613;
    }
L_613: ;
    log_print(2, 0, "[Info]: [Analog]min index : %d\n\r\n", (s32)v607);
    goto L_614;
L_614: ;
    *(u16 *)(&trim_info.rxpuk2) = v600;
    REG32(1899540) = (REG32(1899540) & -261633);
    REG32(1899540) = REG32(1899540);
    REG32(1899540) = (REG32(1899540) & -261633);
    v621 = -1;
    v622 = -40;
    v623 = v600;
    goto L_620;
L_620: ;
    delay(5);
    REG32(1899540) = (REG32(1899540) | ((v622 & 511) << 9));
    delay(20);
    v630 = 0;
    v631 = 0;
    goto L_629;
L_629: ;
    REG32(1899968) = (REG32(1899968) & -33554433);
    delay(5);
    REG32(1899968) = (REG32(1899968) | 33554432);
    goto L_636;
L_636: ;
    if (((REG32(1899992) & 67108864) == 0)) {
        goto L_640;
    } else {
        goto L_636;
    }
L_640: ;
    v643 = ((s32)((REG32(1899996) << 8)) >> (16));
    delay(5);
    v646 = ((s32)((REG32(1900000) << 8)) >> (16));
    delay(5);
    v650 = ((v643 * v643) + v630) + (v646 * v646);
    v651 = v631 + 1;
    if ((v651 == 20)) {
        goto L_653;
    } else {
        v630 = v650;
        v631 = v651;
        goto L_629;
    }
L_653: ;
    v654 = ((s32)(v650) / (s32)(20));
    v655 = v654 < v621;
    v656 = (v655 ? v622 : v623);
    v657 = (v655 ? v654 : v621);
    v658 = v622 + 1;
    REG32(1899540) = (REG32(1899540) & -261633);
    if ((v658 == 40)) {
        goto L_662;
    } else {
        v621 = v657;
        v622 = v658;
        v623 = v656;
        goto L_620;
    }
L_662: ;
    v663 = (s32)(s16)v656;
    REG32(1899540) = (REG32(1899540) | ((v663 << 9) & 261632));
    if ((log_tag_const_i_Analog == 0)) {
        goto L_671;
    } else {
        goto L_670;
    }
L_670: ;
    log_print(2, 0, "[Info]: [Analog]min index : %d\n\r\n", (s32)v663);
    goto L_671;
L_671: ;
    *(u16 *)(&trim_info.rxpsk3) = v656;
    REG32(1899680) = (REG32(1899680) & -65537);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -3);
    delay(2);
    REG32(1899644) = (REG32(1899644) & -5);
    delay(2);
    REG32(1899976) = (REG32(1899976) & -50331649);
    delay(2);
    REG32(1899556) = (REG32(1899556) & -2049);
    bt_analog_part_init();
    reset_trim_info();
    bt_ldo_trim();
    bt_analog_part_init();
    reset_trim_info();
    set_ldo_trim_res(1);
    memcpy(v4, &trim_info, 24);
    memcpy(&v3[24], &ldo_trim_res.chargpump_ldo_lev, 13);
    v683 = syscfg_write(93, v4, 37);
    goto L_684;
L_684: ;
    return;
}

static void BT_RF_CODE reset_trim_info(void)
{
    if (log_tag_const_i_Analog) {
        log_print(2, 0, "[Info]: [Analog] -------- reset trim info -----------  \n\r\n");
        log_print(2, 0, "[Info]: [Analog]txi_dc =%d txq_dc =%d\n\r\n", (s16)trim_info.txi_dc, (s16)trim_info.txq_dc);
        log_print(2, 0, "[Info]: [Analog]txpuk1 =%d txpuk2 =%d txpsk3 =%d\n\r\n", (s16)trim_info.txpuk1, (s16)trim_info.txpuk2, (s16)trim_info.txpsk3);
        log_print(2, 0, "[Info]: [Analog]rxi_cnt=%d rxq_cnt=%d\n\r\n", (s16)trim_info.rxi_trim_cnt, (s16)trim_info.rxq_trim_cnt);
        log_print(2, 0, "[Info]: [Analog]rxpuk2 =%d rxpsk3 =%d\n\r\n", (s16)trim_info.rxpuk2, (s16)trim_info.rxpsk3);
        log_print(2, 0, "[Info]: [Analog]txpuk1_1=%d txpuk2_1=%d\n\r\n", (s16)trim_info.txpuk1_1, (s16)trim_info.txpuk2_1);
    }
    REG32(1899528) = REG32(1899528) & ~1023u;
    REG32(1899528) = (trim_info.txi_dc & 1023u) | REG32(1899528);
    REG32(1899528) = REG32(1899528) & ~1047552u;
    REG32(1899528) = ((u32)(trim_info.txq_dc & 1023u) << 10) | REG32(1899528);
    REG32(1899536) = REG32(1899536) & ~255u;
    REG32(1899536) = (trim_info.txpuk1 & 255u) | REG32(1899536);
    REG32(1899536) = REG32(1899536) & ~65280u;
    REG32(1899536) = ((u32)trim_info.txpuk2 << 8) | REG32(1899536);
    REG32(1899536) = REG32(1899536) & ~16711680u;
    REG32(1899536) = ((u32)(trim_info.txpsk3 & 255u) << 16) | REG32(1899536);
    REG32(1972488) = REG32(1972488) & ~33423360u;
    REG32(1972488) = (((u32)(trim_info.rxi_trim_cnt & 255u) << 17) | REG32(1972488));
    REG32(1972492) = REG32(1972492) & ~255u;
    REG32(1972492) = ((u32)(trim_info.rxq_trim_cnt & 255u)) | REG32(1972492);
    REG32(1899540) = REG32(1899540) & ~511u;
    REG32(1899540) = ((u32)(trim_info.rxpuk2 & 511u)) | REG32(1899540);
    REG32(1899540) = REG32(1899540) & ~261632u;
    REG32(1899540) = (((u32)(trim_info.rxpsk3 & 511u) << 9) | REG32(1899540));
}

static void BT_RF_CODE rxtrim_analog_init(void)
{
    bt_analog_part_init();
    REG32(1972480) = REG32(1972480) & (u32)-1985;
    REG32(1972480) = REG32(1972480) | 960u;
    REG32(1972504) = REG32(1972504) & 2097151u;
    REG32(1972504) = REG32(1972504) | 132120576u;
    REG32(1972488) = REG32(1972488) & (u32)-449;
    REG32(1972484) = REG32(1972484) & (u32)-61441;
    REG32(1972492) = REG32(1972492) & (u32)-1997825;
    REG32(1972488) = REG32(1972488) | 448u;
    REG32(1972484) = REG32(1972484) | 20480u;
    REG32(1972492) = REG32(1972492) | 1408000u;
    REG32(1972496) = REG32(1972496) | 5u;
    REG32(1972496) = REG32(1972496) | 65536u;
    REG32(1972484) = REG32(1972484) | 2048u;
    REG32(1972488) = REG32(1972488) | 1u;
    REG32(1972480) = REG32(1972480) | 786432u;
    REG32(1972484) = REG32(1972484) | 1u;
    REG32(1972500) = REG32(1972500) | 13u;
    REG32(1972500) = REG32(1972500) | 49280u;
    REG32(1972504) = REG32(1972504) | 1048576u;
}
