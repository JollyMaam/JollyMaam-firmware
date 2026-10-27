#include "app_config.h"
#include "system/includes.h"
#include "system/user_cfg.h"
#include "btctrler/config.h"

/* Constants the Jieli libraries read at link time.  Only those the linked
 * archives reference; the values are the SDK defaults this pad was
 * measured with.  Every log tag is 0: the libraries print nothing. */

const int config_btctler_modules = 3;  /* classic and LE */
const int config_btctler_le_tws = 0;
const int CONFIG_BTCTLER_TWS_ENABLE = BTCFG_TWS;
const int CONFIG_TWS_AFH_ENABLE = 0;
const int CONFIG_BTCTLER_FAST_CONNECT_ENABLE = BTCFG_FAST_CONNECT;
const int config_btctler_hci_standard = BTCFG_HCI_STANDARD;
const int config_btctler_mode = 0x01;
const int CONFIG_TWS_POWER_BALANCE_ENABLE = 0;
const int config_force_bt_pwr_tab_using_normal_level = 0;
const int CONFIG_TWS_SUPER_TIMEOUT = 2000;
const int CONFIG_A2DP_DATA_CACHE_LOW_AAC = 100;
const int CONFIG_A2DP_DATA_CACHE_HI_AAC = 150;
const int CONFIG_A2DP_DATA_CACHE_LOW_SBC = 120;
const int CONFIG_A2DP_DATA_CACHE_HI_SBC = 160;
const u8 rx_fre_offset_adjust_enable = 1;
const int ble_disable_wait_enable = 1;
const int config_btctler_eir_version_info_len = 0;
const int CONFIG_TEST_DUT_CODE = 0;
const int CONFIG_TEST_FCC_CODE = 0;
const int CONFIG_TEST_DUT_ONLY_BOX_CODE = 0;
const int CONFIG_BREDR_INQUIRY = BTCFG_BREDR_INQUIRY;
const int CONFIG_INQUIRY_PAGE_OFFSET_ADJUST = 0;
const int CONFIG_ESCO_MUX_RX_BULK_ENABLE = 0;
const int CONFIG_LMP_NAME_REQ_ENABLE = BTCFG_NAME_REQ;
const int CONFIG_LMP_PASSKEY_ENABLE = BTCFG_PASSKEY;
const int CONFIG_LMP_MASTER_ESCO_ENABLE = BTCFG_MASTER_ESCO;
const int CONFIG_WIFI_DETECT_ENABLE = 0;
const int ESCO_FORWARD_ENABLE = BTCFG_ESCO_FORWARD;
const int config_bt_function = 0;
const int config_btctler_bredr_master = BTCFG_BREDR_MASTER;
const int config_btctler_dual_a2dp = 0;
const int config_bredr_afh_user = 0;
const int config_bt_temperature_pll_trim = 0;
const int config_bt_security_vulnerability = 0;
const int config_dut_protocol_mode = 0;
const int config_btctler_le_afh_en = 0;
const int config_btctler_le_slave_multilink = 0;
#ifdef BOARD_DONGLE
/* A central link per hub slot (link.h HUB_SLOTS), as the SDK's dongle with
 * two clients, and one more for scanning or connecting while they run (a
 * scan with no room left reset the chip). */
#define LE_LINKS (2 + 1)
const int config_btctler_le_master_multilink = 1;
#else
#define LE_LINKS 1
const int config_btctler_le_master_multilink = 0;
#endif
const uint64_t config_btctler_le_features = (1ULL << ( 0)) | (1ULL << ( 5)) | (1ULL << ( 8));
const int config_btctler_le_acl_packet_length = 251;
#ifdef BOARD_DONGLE
const int config_btctler_le_roles = (1UL << (4)) | (1UL << (3)) | (1UL << (0));   /* init, scan, master */
#else
const int config_btctler_le_roles = (1UL << (2)) | (1UL << (1));                  /* adv, slave */
#endif
const int config_btctler_le_hw_nums = LE_LINKS;
const int config_btctler_le_rx_nums = (LE_LINKS * 3) + 2;
const int config_btctler_le_acl_total_nums = (LE_LINKS * 3) + 1;
const int config_btctler_le_slave_conn_update_winden = 500;
const int config_btctler_single_carrier_en = 1;
/* 0: a sniffing slave wakes at every anchor point.  With 1 the controller
 * skips anchors for sniff_long_interval (500 ms) once a short-interval link
 * goes quiet, and only the SDK's private host API (lmp_hci_send_packet) pulls
 * the next anchor back when data arrives; the standard HCI path BTstack uses
 * does not, so reports waited up to 500 ms in sniff. */
const int sniff_support_reset_anchor_point = 0;
const int sniff_long_interval = (500 / 0.625);
const int clock_sys_src_use_lrc_hw = 0;
const int config_printf_time = 1;
#ifdef DEBUG_UART_DP
const int config_asser = 1;            /* a bring-up build prints the failure */
#else
const int config_asser = BTCFG_ASSER;
#endif
const int config_system_info = 0;
const int CONFIG_CPU_UNMASK_IRQ_ENABLE = 2;
const int config_update_mode = (1UL << (0)) | (1UL << (4)) | (1UL << (3)) | (1UL << (2));
const int support_vm_data_keep = 1;
u32 config_vendor_le_bb = 0;

#define TAG(name) const char log_tag_const_##name AT(.LOG_TAG_CONST) = 0;
TAG(c_BL) TAG(c_PMU) TAG(c_TWS_LE) TAG(d_Analog) TAG(d_BL) TAG(d_CLOCK)
TAG(d_HCI_LL) TAG(d_HCI_STD) TAG(d_HEAP_MEM) TAG(d_LE_BB) TAG(d_LL) TAG(d_LL_AFH)
TAG(d_LL_CIS) TAG(d_LL_E) TAG(d_LL_ISO) TAG(d_LMP) TAG(d_LRC) TAG(d_PMU)
TAG(d_SYSCFG) TAG(d_SYS_TMR) TAG(d_TWS) TAG(d_TWS_LMP) TAG(d_USB) TAG(e_Analog)
TAG(e_BL) TAG(e_CLOCK) TAG(e_FM) TAG(e_HCI_LL) TAG(e_HCI_LL5) TAG(e_HCI_STD)
TAG(e_HEAP_MEM) TAG(e_KTASK) TAG(e_LE5_BB) TAG(e_LE_BB) TAG(e_LL) TAG(e_LL_ADV)
TAG(e_LL_AFH) TAG(e_LL_CIS) TAG(e_LL_E) TAG(e_LL_EXT_ADV) TAG(e_LL_EXT_INIT)
TAG(e_LL_EXT_SCAN) TAG(e_LL_INIT) TAG(e_LL_ISO) TAG(e_LL_M) TAG(e_LL_PADV)
TAG(e_LL_PHY) TAG(e_LL_RL) TAG(e_LL_S) TAG(e_LL_SCAN) TAG(e_LL_TWS_ADV)
TAG(e_LL_TWS_SCAN) TAG(e_LL_WL) TAG(e_LMP) TAG(e_P33_MISC) TAG(e_P_MEM)
TAG(e_SYSCFG) TAG(e_USB) TAG(e_VM) TAG(i_AES) TAG(i_Analog) TAG(i_BL) TAG(i_CHARGE)
TAG(i_CLOCK) TAG(i_FM) TAG(i_HCI_LL) TAG(i_HCI_LL5) TAG(i_HCI_LMP) TAG(i_HCI_STD)
TAG(i_HEAP_MEM) TAG(i_KTASK) TAG(i_LE5_BB) TAG(i_LE_BB) TAG(i_LL) TAG(i_LL_ADV)
TAG(i_LL_AFH) TAG(i_LL_CIS) TAG(i_LL_DX) TAG(i_LL_E) TAG(i_LL_EXT_ADV) TAG(i_LL_EXT_INIT)
TAG(i_LL_EXT_SCAN) TAG(i_LL_INIT) TAG(i_LL_ISO) TAG(i_LL_M) TAG(i_LL_PADV)
TAG(i_LL_PHY) TAG(i_LL_RL) TAG(i_LL_S) TAG(i_LL_SCAN) TAG(i_LL_TWS_ADV)
TAG(i_LL_TWS_SCAN) TAG(i_LL_WL) TAG(i_LMP) TAG(i_LP_TIMER) TAG(i_LRC) TAG(i_PMU)
TAG(i_P_MEM) TAG(i_RF) TAG(i_SYSCFG) TAG(i_SYS_TMR) TAG(i_TMR) TAG(i_TWS)
TAG(i_TWS_LE) TAG(i_TWS_LMP) TAG(i_Thread) TAG(i_USB) TAG(i_VM) TAG(i_V_MEM)
TAG(i_WKUP) TAG(v_LMP) TAG(w_LL_AFH)
#ifdef DEBUG_UART_DP
const char log_tag_const_e_DEBUG AT(.LOG_TAG_CONST) = 1;  /* exception_analyze */
#endif

const int vm_max_size_config = VM_MAX_SIZE_CONFIG;

const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6},
    {CFG_BT_FRE_OFFSET, 6},
    {0, 0},
};

u8 get_vbat_percent(void) { return 0xff; }
u8 is_pwm_led_on(void) { return 0; }
u16 update_result_get(void) { return 0; }
void debug_uart_init(const struct uart_platform_data *data) { (void)data; }
int eSystemConfirmStopStatus(void) { return 1; }
__attribute__((used)) int *__errno(void) { static int err; return &err; }
#ifdef DEBUG_UART_DP
/* A bring-up build shows the libraries' own lines (exceptions among them). */
#include <stdarg.h>
#include "pad.h"
int vsnprintf(char *, unsigned long, const char *, va_list);
int printf_lite(const char *format, ...)
{
    char line[128];
    va_list ap;

    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    plat_debug_line(line);
    return 0;
}
#else
int printf_lite(const char *format, ...) { (void)format; return 0; }
#endif

#ifdef CONFIG_CPU_BD19
/* BD19 SDK HID defaults. */
const u8 lp_bt_sniff_use_btosc = 0;
const int set_to_close_timer0_delay = 0;
const int CONFIG_BLE_SYNC_WORD_BIT = 30;
const int CONFIG_PAGE_POWER = 4;
const int CONFIG_PAGE_SCAN_POWER = 7;
const int CONFIG_INQUIRY_SCAN_POWER = 7;
const int CONFIG_BTCTLER_QOS_ENABLE = 1;
TAG(e_PMU) TAG(i_DEBUG)
int filter_out_sbc_data_en(void) { return 0; }
#endif
