#include "system/includes.h"
#include "system/boot.h"
#include "system/syscfg_id.h"

/* Runtime configuration ABI for prebuilt cpu.a/system.a. Jieli's libraries
 * import ordinary globals rather than accepting one configuration object. */

const int config_printf_time = 0;
const int config_asser = 0;
const int config_system_info = 0;

/* IDs registered here live in the preserved BTIF flash area. Other writable
 * IDs default to the VM area. */
const int vm_max_size_config = VM_MAX_SIZE_CONFIG;
const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6},
    {CFG_BT_FRE_OFFSET, 6},
    {0, 0},
};

const int clock_sys_src_use_lrc_hw = 0;
const int config_dcdc_mode = 0;
const int config_spi_code_user_cache = 0;
const char config_vad_lowpower_clock = 0;
const int sd0_sd1_use_the_same_hw = 0;
const int keep_card_at_active_status = 0;
const int sdx_can_operate_mmc_card = 0;
const unsigned char lp_bt_sniff_use_btosc = 0;

const int SDFILE_VFS_REDUCE_ENABLE = 1;
const int device_bulk_read_async_enable = 0;
const int ENABLE_LUA_VIRTUAL_MACHINE = 0;
const int CONFIG_CPU_UNMASK_IRQ_ENABLE = 2;
const int set_to_close_timer0_delay = 0;

u16 update_result_get(void)
{
    return 0; /* no pending SDK update result */
}

int eSystemConfirmStopStatus(void)
{
    return 1; /* no forced periodic 100 ms idle wake */
}

/* defined in cpu/br23/pwm_led.c */
#if 0
/* The board's RGB chain is SPI-driven, not the BR23 PWM-LED peripheral. */
int is_pwm_led_on(void)
{
    return 0;
}
#endif

/* No debug transport is configured yet. Library code still imports these
 * one-byte runtime switches, so provide disabled entries centrally. */
#define C2_DISABLED_LOG_TAG(tag) \
    const char log_tag_const_c_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_v_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_i_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_d_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_w_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_e_##tag AT(.LOG_TAG_CONST) = 0

C2_DISABLED_LOG_TAG(Analog);
C2_DISABLED_LOG_TAG(BL);
C2_DISABLED_LOG_TAG(CLOCK);
C2_DISABLED_LOG_TAG(HEAP_MEM);
C2_DISABLED_LOG_TAG(KTASK);
C2_DISABLED_LOG_TAG(LE5_BB);
C2_DISABLED_LOG_TAG(LE_BB);
C2_DISABLED_LOG_TAG(LL);
C2_DISABLED_LOG_TAG(LL_ADV);
C2_DISABLED_LOG_TAG(LL_AFH);
C2_DISABLED_LOG_TAG(LL_EXT_ADV);
C2_DISABLED_LOG_TAG(LL_INIT);
C2_DISABLED_LOG_TAG(LL_PADV);
C2_DISABLED_LOG_TAG(LL_SCAN);
C2_DISABLED_LOG_TAG(LMP);
C2_DISABLED_LOG_TAG(LP_TIMER);
C2_DISABLED_LOG_TAG(LRC);
C2_DISABLED_LOG_TAG(P_MEM);
C2_DISABLED_LOG_TAG(PMU);
C2_DISABLED_LOG_TAG(SYSCFG);
C2_DISABLED_LOG_TAG(SYS_TMR);
C2_DISABLED_LOG_TAG(TMR);
C2_DISABLED_LOG_TAG(TWS);
C2_DISABLED_LOG_TAG(TWS_LMP);
C2_DISABLED_LOG_TAG(VM);
C2_DISABLED_LOG_TAG(V_MEM);
C2_DISABLED_LOG_TAG(WKUP);

/* From apps/hid/config/lib_media_config.c */
const int audio_dec_app_sync_en = 0;

/* From apps/hid/config/lib_btctrler_config.c */
const int ble_disable_wait_enable = 1;
const int config_btctler_modules = 0;
const int CONFIG_WIFI_DETECT_ENABLE = 0;
const int config_btctler_le_roles = 0;

/* From apps/hid/config/lib_profile_config.c */
const int config_stack_modules = 0;
