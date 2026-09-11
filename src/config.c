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

/* cfg_btif.c needs this address conversion even when the runtime JLFS/VFS is
 * disabled. The SDK's implementation is only these two BOOT_INFO fields, but
 * lives in sdfile_new.c.o alongside the filesystem initcall. Keeping the
 * conversion here avoids linking and executing that unrelated mount code. */
u32 sdfile_cpu_addr2flash_addr(u32 cpu_address)
{
    return cpu_address - boot_info.sfc.app_addr + boot_info.sfc.sfc_base_addr;
}

/* vm.c also imports this tiny helper from the filesystem object. The startup
 * code has already copied the same physical flash capacity into BOOT_INFO. */
u32 sdfile_get_disk_capacity(void)
{
    return boot_info.flash_size;
}

/* The filesystem implementation normally builds an in-RAM table of protected
 * JLFS file ranges. With JLFS disabled that table would remain empty, for
 * which the SDK helper returns zero. Keep the same empty-table behavior while
 * bringing up VM/syscfg; this is not an authorization for application-level
 * arbitrary flash writes. */
int sdfile_check_write_protect(u32 flash_address, u32 size)
{
    (void)flash_address;
    (void)size;
    return 0;
}

u16 update_result_get(void)
{
    return 0; /* no pending SDK update result */
}

int eSystemConfirmStopStatus(void)
{
    return 1; /* no forced periodic 100 ms idle wake */
}

/* The board's RGB chain is SPI-driven, not the BR23 PWM-LED peripheral. */
int is_pwm_led_on(void)
{
    return 0;
}

/* No debug transport is configured yet. Library code still imports these
 * one-byte runtime switches, so provide disabled entries centrally. */
#define C2_DISABLED_LOG_TAG(tag) \
    const char log_tag_const_v_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_i_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_d_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_w_##tag AT(.LOG_TAG_CONST) = 0; \
    const char log_tag_const_e_##tag AT(.LOG_TAG_CONST) = 0

C2_DISABLED_LOG_TAG(CLOCK);
C2_DISABLED_LOG_TAG(HEAP_MEM);
C2_DISABLED_LOG_TAG(KTASK);
C2_DISABLED_LOG_TAG(LP_TIMER);
C2_DISABLED_LOG_TAG(LRC);
C2_DISABLED_LOG_TAG(PMU);
C2_DISABLED_LOG_TAG(P_MEM);
C2_DISABLED_LOG_TAG(SYSCFG);
C2_DISABLED_LOG_TAG(SYS_TMR);
C2_DISABLED_LOG_TAG(TMR);
C2_DISABLED_LOG_TAG(VM);
C2_DISABLED_LOG_TAG(V_MEM);
C2_DISABLED_LOG_TAG(WKUP);

const char log_tag_const_c_PMU AT(.LOG_TAG_CONST) = 0;
