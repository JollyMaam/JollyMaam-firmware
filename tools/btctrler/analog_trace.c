/* Software input model for check_analog.py, not a radio emulator.
 * LLVM MMIO operations call these hooks; no physical addresses are accessed.
 * Fingerprints cover ordered loads/stores, delays, callbacks, formatted logs,
 * ABI-width return values and final RF/trim/bank records. The model supplies
 * ready ADC/calibration status, a monotonic PLL response and bounded samples.
 * clk_get returns zero to avoid host busy-waiting on target delay loops.
 * The private SDK entry points are exported only in isolated validation IR.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
uint8_t rf[1024] __attribute__((aligned(4)));
extern unsigned char trim_info[24],ldo_trim_res[13],cap_freq_r[24];
int config_bt_temperature_pll_trim=1,config_btctler_modules=3,config_btctler_mode=0,config_btctler_single_carrier_en,CONFIG_BLE_SYNC_WORD_BIT=24,config_force_bt_pwr_tab_using_normal_level;
uint8_t log_tag_const_i_Analog=1,log_tag_const_e_Analog=1,lp_bt_sniff_use_btosc=1;
const signed char agc_dbm_tlb[33]={-30,-29,-28};
static uint32_t regs[65536],adc=1000,adc_count;static int scenario,verbose;static uint64_t hash=1469598103934665603ull,events;
static void event(uint32_t kind,uint32_t addr,uint32_t val)
{ uint32_t v[]={kind,addr,val};for(int i=0;i<3;i++)hash=(hash^v[i])*1099511628211ull;++events;if(verbose)fprintf(stdout,"%u %u %u\n",kind,addr,val);if(events>10000000){fprintf(stderr,"event limit\n");exit(3);} }
uint32_t mmio_read(uint32_t a)
{
 if(a<0x1c0000 || a>=0x200000 || (a&3))abort();
 uint32_t v=regs[(a-0x1c0000)/4];
 if(a==1899916)v=((scenario%33)<<8)|17;
 if(a==1899704)v&=65535;
 if(a==1978624)v|=128;
 if(a==1978628)v=adc;
 if(a==1899980||a==1899992)v=0;
 if(a==1972600) { int bank=(regs[(1972528-0x1c0000)/4]>>18)&127;int freq=regs[(1972532-0x1c0000)/4]&255;v=freq<74+(111-bank)*3?4:2; }
 if(a==1899984||a==1899988||a==1899996||a==1900000)v=(uint32_t)(int32_t)(int16_t)((int)(regs[(1899528-0x1c0000)/4]&1023)-(scenario==9?512:0))<<8;
 event(1,a,v);return v;
}
void mmio_write(uint32_t a,uint32_t v) {if(a<0x1c0000 || a>=0x200000 || (a&3))abort();if(a==1899708)v=0x12340000;event(2,a,v);regs[(a-0x1c0000)/4]=v;}
void delay(uint32_t n){event(3,1,n);}
int clk_get(char *s){(void)s;event(3,2,0);return 0;}
void log_print(int level,void *tag,const char *fmt,...)
{(void)tag;char b[1000];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);event(4,level,0);for(char *p=b;*p;p++)hash=(hash^(unsigned char)*p)*1099511628211ull;if(verbose)fprintf(stdout,"LOG %s",b);}
int printf(const char *fmt,...)
{char b[1000];va_list ap;va_start(ap,fmt);int n=vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);event(5,n,0);for(char *p=b;*p;p++)hash=(hash^(unsigned char)*p)*1099511628211ull;return n;}
void bredr_pll_comp_ext_reset(void){event(3,3,0);}
uint16_t CRC16(uint8_t *data,uint32_t n){for(unsigned i=0;i<n;i++)event(6,i,data[i]);return 0x55aa;}
int syscfg_read(uint16_t k,uint8_t *p,uint16_t n)
{event(3,k,n);memset(p,0,n);if(k==110){*(uint16_t *)p=0x55aa;int32_t value=scenario==3?201:scenario==2?-200:50;memcpy(p+2,&value,4);return 262;}if(k==93&&scenario==1){uint16_t t[12]={4098,1,2,3,4,5,6,7,8,9,10,11};memcpy(p,t,24);memset(p+24,3,11);p[35]=0xdf;p[36]=2;return 37;}return 0;}
int syscfg_write(uint16_t k,uint8_t *p,uint16_t n){event(3,k,n);for(int i=0;i<n;i++)event(7,i,p[i]);return 7;}
int check_id(uint16_t k){event(3,8,k);return 1;}
static void *ops[7];void *get_syscfg_btif_ops(void){ops[1]=check_id;return ops;}
void bit_clr_ie(uint8_t b){event(3,9,b);}void bit_set_ie(uint8_t b){event(3,10,b);}
uint8_t get_idle_task_state(void){event(3,11,0);return scenario&1;}
void bredr_frame_suspend_control(uint8_t b){event(3,12,b);}void le_hw_all_event_hold_en(uint8_t b){event(3,13,b);}
void power_reset_close(void){event(3,14,0);}void bredr_radiopwrupdn_reset(void){event(3,15,0);}
void clk_set_osc_cap(uint8_t a,uint8_t b){event(3,16,(a<<8)|b);}void btagc_init(uint8_t a,uint8_t b){event(3,17,(a<<8)|b);}
void adc_pmu_detect_en(int b){event(3,18,b);}uint32_t adc_sample(uint32_t n){event(3,19,n);adc=n==13?1000:scenario>=8?900+(adc_count++%8)*150:1500;return 0;}
void local_irq_disable(void){event(3,20,0);}void local_irq_enable(void){event(3,21,0);}
uint32_t clock_get_pll_target_frequency(void){event(3,22,0);return scenario&1?192:96;}
uint16_t get_vbg_trim(void){event(3,23,0);return scenario&1?0x21:2;}
int main(int argc,char **argv)
{
 if(argc<4)return 2;scenario=atoi(argv[2]);verbose=argc>4;
 config_btctler_mode=scenario==1?1:scenario==2?2:scenario==3?4:scenario==5?6:scenario==6?32:0;
 if(scenario==7)log_tag_const_i_Analog=log_tag_const_e_Analog=0;
 config_force_bt_pwr_tab_using_normal_level=scenario==4;
 for(int i=0;i<65536;i++)regs[i]=0x13572468+i*7;
 rf[4]=11;rf[5]=22;rf[6]=1;rf[9]=1;
 for(int i=0;i<31;i++)rf[19+i]=74+3*i;rf[50]=111;
 uint16_t ti[12]={4098,11,12,2,4,5,6,7,8,9,10,11};memcpy(trim_info,ti,24);
 memset(ldo_trim_res,3,11);ldo_trim_res[11]=0xdf;ldo_trim_res[12]=2;memset(cap_freq_r,0,24);
 void *p=dlsym(RTLD_DEFAULT,argv[1]);if(!p){fprintf(stderr,"missing %s\n",argv[1]);return 2;}
 unsigned arg=strtoul(argv[3],0,0),result=0;
 if(!strcmp(argv[1],"get_jl_chip_id")) { result=((uint8_t(*)(void))p)(); }
 if(!strcmp(argv[1],"get_jl_chip_id2")) { result=((uint8_t(*)(void))p)(); }
 if(!strcmp(argv[1],"get_bt_osc_offset_flag")) { result=((uint8_t(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_osc_offset_set")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"bt_run_clk_sel")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"rf_osc_offset_initial_val_get")) { result=((int16_t(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_osc_offset_ext_updata")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"bt_fre_offset_store_handle_register")) { ((void(*)(void (*)(int,int)))p)(NULL); }
 if(!strcmp(argv[1],"bt_osc_offset_ext_save")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"bt_fre_offset_get")) { result=((uint32_t(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_osc_offset_save")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_fre_offset_value_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bta_pll_bank_set")) { result=((uint8_t(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"get_bta_pll_bank")) { result=((uint8_t(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"btpll_udelay")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"mdelay")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"bta_pll_bank_scan_tmp")) { result=((uint8_t(*)(uint32_t,uint8_t))p)(arg,100); }
 if(!strcmp(argv[1],"get_bta_pll_midbank_temp")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"btpll_status_det")) { result=((uint8_t(*)(void))p)(); }
 if(!strcmp(argv[1],"txtrim_analog_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_analog_part_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"clk_out_debug")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"btadc_sel")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"btagc_enalbe")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"RF_analog_recover")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_reload_trim")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_set_single_carrier")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_analog_reset")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"mesh_certification_api")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"spur_control")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"RF_mdm_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_dut_reset_mdm")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_rf_analog_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"certification")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"close_zeroif")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bqb_2m_setting")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bsb_normal_setting")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_get_pwr_max_level")) { result=((uint8_t(*)(void))p)(); }
 if(!strcmp(argv[1],"bta_set_amp_sepa")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"bta_gain_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_vref_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_vref_close")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"analog_rx_control")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"BT_CP_EN")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"get_ldo_voltage")) { result=((uint32_t(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"btpll_trim_en")) { ((void(*)(uint8_t,uint8_t))p)(arg,100); }
 if(!strcmp(argv[1],"charg_pump_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"divider_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"vco_first_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"vco_second_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"xosc_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"rxadc_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"rxadc_ref_trim_set")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"umixer_vblo_trim_set")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"rx_ldo_trim_set")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"set_ldo_trim_res")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"vco_lock_trim")) { result=((uint32_t(*)(void))p)(); }
 if(!strcmp(argv[1],"bt_ldo_trim")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"rf_mdm_con8_config")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"rf_mdm_con_ble_sync_word")) { ((void(*)(uint32_t))p)(arg); }
 if(!strcmp(argv[1],"freq_2_cap")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"bta_pll_config_init")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"set_bt_trim_mode")) { ((void(*)(uint8_t))p)(arg); }
 if(!strcmp(argv[1],"reset_trim_info")) { ((void(*)(void))p)(); }
 if(!strcmp(argv[1],"rxtrim_analog_init")) { ((void(*)(void))p)(); }
 event(8,0,result);for(int i=0;i<1024;i++)event(9,i,rf[i]);for(int i=0;i<24;i++)event(10,i,trim_info[i]);for(int i=0;i<13;i++)event(11,i,ldo_trim_res[i]);for(int i=0;i<24;i++)event(12,i,cap_freq_r[i]);
 fprintf(stderr,"%016llx %llu\n",(unsigned long long)hash,(unsigned long long)events);return 0;
}
