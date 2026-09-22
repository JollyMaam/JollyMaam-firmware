#include <system/includes.h>

#include "main.h"

/* VM_MAX_SIZE_CONFIG comes from CFLAGS in makefiles */
const int vm_max_size_config = VM_MAX_SIZE_CONFIG;

/* TRANSLATED: Task table. */
const struct task_info task_info_table[] = {
    {"app_core",   2,  0,  640,  128  },
    {"imu",        1,  0,  256,  0    },
    {"sys_event",  7,  0,  256,  0    },
    {"btctrler",   4,  0,  512,  256  },
    {"btencry",    1,  0,  512,  128  },
    {"btstack",    3,  0,  2048, 256  },
    {"systimer",   7,  0,  128,  0    },
    {"update",     1,  0,  512,  0    },
    {0, 0},
};

/* required by app */
u8 get_vbat_percent(void)
{
  return 0xff;
}

/* required by ??? */
u8 is_pwm_led_on(void)
{
  return 0;
}

/* required by cpu/br23/liba/cpu.a(startup.S.o) */
u16 update_result_get(void)
{
  return 0; /* UPDATA_NON from include_lib/update/update.h */
};

/* from setup.c if debug define is enabled */
void debug_uart_init(const struct uart_platform_data *data)
{
}

/* TRANSLATED: With no pending task timeout, choose indefinite sleep or a
 * periodic 100 ms wakeup. */
int eSystemConfirmStopStatus(void)
{
    //1:Endless Sleep
    //0:100 ms wakeup
    return 1;
}

__attribute__((used)) int *__errno()
{
    static int err;
    return &err;
}

/* Deliberately outside .bss: p33_soft_reset() preserves internal SRAM, so this
 * one-shot record can carry the next profile through reset without wearing
 * flash. ld scripts reserve its fixed address from the heap. */
volatile struct cyclone2_boot_request reset_mailbox
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

/* Bring-up milestone record; see main.h.  Kept next to the boot request so a
 * crash during Bluetooth startup leaves evidence behind. */
volatile u32 cyclone2_bringup_trace
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

/* Incremented once per boot so a stale trace value can be told apart from the
 * one the crashed boot wrote. */
volatile u32 cyclone2_boot_count
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

/* Ring of bring-up milestones.  A crashed boot leaves the order in which the
 * Bluetooth stack came up (or failed) readable through the console. */
volatile u32 cyclone2_bringup_log[CYCLONE2_BRINGUP_LOG_ENTRIES]
    __attribute__((section(".reset_mailbox"), used, aligned(4)));
volatile u32 cyclone2_bringup_log_head
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

void cyclone2_bringup_record(u32 code)
{
    u32 head = cyclone2_bringup_log_head;

    cyclone2_bringup_log[head % CYCLONE2_BRINGUP_LOG_ENTRIES] =
        (code << 16) | (cyclone2_boot_count & 0xffffU);
    cyclone2_bringup_log_head = head + 1U;
}

/* entry point from SDK */
void app_main()
{
    struct intent it;
    cyclone2_boot_count++;
    init_intent(&it);
    it.name = "cyclone2";
    it.action = ACTION_CYCLONE2;
    start_app(&it);
}
