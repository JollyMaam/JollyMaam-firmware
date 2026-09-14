#include "system/includes.h"
#include "system/boot.h"

/* required by system.a (lib/system/printf/log.c) */
const int config_printf_time = 0;

/* required by system.a (lib/system/task/task.c) */
const int config_system_info = 0;

/* required by system.a (lib/system/cpu/br23/memory/pmalloc.c) and cpu.a (lib/driver/cpu/br23/debug.c) */
const int config_asser = 0;

/* required by system.a (system/cfg_tool/cfg_btif.c) */
const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6},
    {CFG_BT_FRE_OFFSET, 6},
    {0, 0},
};

/* required by cpu.a (lib/driver/cpu/br23/vm/vm.c) */
const int vm_max_size_config = VM_MAX_SIZE_CONFIG;

/* required by cpu.a (lib/driver/cpu/br23/timer.c) */
const int clock_sys_src_use_lrc_hw = 0;

/* required by cpu.a(startup.S.o) */
u16 update_result_get(void)
{
    return 0; /* no pending SDK update result */
}

/* required by media_app.a (lib/media/media_develop/application/audio_dec_app_sync.c) */
const int audio_dec_app_sync_en = 0;

/* required by btctrler.a (lib/btctrler/...) */
const int config_btctler_modules = 0;
const int config_btctler_le_roles = 0;

/* required by btstack.a (lib/bt_protocol/btstack/Profile/BR_EDR/a2dp_sink.c) */
const int config_stack_modules = 0;

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

#if VFS_ENABLE==0
/* VM and BTIF need these three helpers, but the SDK defines them in the same
 * archive object as the runtime sdfile/VFS mount initcall. If VFS is disabled
 * provide the small non-filesystem behavior directly instead of pulling that
 * unrelated initcall into startup. */
u32 sdfile_cpu_addr2flash_addr(u32 cpu_address)
{
    return cpu_address - boot_info.sfc.app_addr + boot_info.sfc.sfc_base_addr;
}

u32 sdfile_get_disk_capacity(void)
{
    return boot_info.flash_size;
}

int sdfile_check_write_protect(u32 flash_address, u32 size)
{
    (void)flash_address;
    (void)size;
    return 0;
}
#endif

/* 1: endless sleep, 0: 100msec wakeup. ??? */
int eSystemConfirmStopStatus(void)
{
    return 1;
}
