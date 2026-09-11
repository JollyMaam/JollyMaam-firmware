#include "typedef.h"
#include "asm/wdt.h"
#include "system/app_core.h"
#include "system/boot.h"
#include "system/includes.h"
#include "system/init.h"

#include "board.h"
#include "control.h"
#include "rom.h"

#define CYCLONE2_APP_NAME "cyclone2"
#define ACTION_CYCLONE2_MAIN 0x43593201

/* First project checkpoint in the stock initcall dispatcher. */
static int cyclone2_early_init_checkpoint(void)
{
    disable_sdk_watchdog();
#if C2_EARLY_LED_ENABLE
    cyclone2_led_initialize();
    cyclone2_led_set_all(C2_EARLY_RED, C2_EARLY_GREEN, C2_EARLY_BLUE);
#endif
    return 0;
}

/* The SDK startup code discovers this table by symbol name. Applications add
 * named tasks here so task_create() can use fixed, auditable resources. */
const struct task_info task_info_table[] = {
    {"app_core",   1, 0, 640, 128},
    {"sys_event",  7, 0, 256,   0},
    {"systimer",   7, 0, 128,   0},
    {"c2_control", 2, 0, 256,   0},
    {0, 0},
};

/* The SDK calls here after loading */
void app_main(void)
{
    /* Bring-up checkpoint: prove that SDK startup reaches app_main without
     * involving application dispatch or dynamic task creation. */
    cyclone2_led_initialize();
    cyclone2_led_set_all(8U, 0U, 0U);
    for (;;) {
        wdt_clear();
        rom_mdelay(100U);
    }
}

static int cyclone2_state_machine(struct application *app,
                                  enum app_state state,
                                  struct intent *intent)
{
    (void)app;

    switch (state) {
    case APP_STA_START:
        if (intent && intent->action == ACTION_CYCLONE2_MAIN) {
            cyclone2_control_start();
        }
        break;
    case APP_STA_CREATE:
    case APP_STA_PAUSE:
    case APP_STA_RESUME:
    case APP_STA_STOP:
    case APP_STA_DESTROY:
        break;
    }
    return 0;
}

static int cyclone2_event_handler(struct application *app,
                                  struct sys_event *event)
{
    (void)app;
    (void)event;
    return 0;
}

static const struct application_operation cyclone2_operations = {
    .state_machine = cyclone2_state_machine,
    .event_handler = cyclone2_event_handler,
};

REGISTER_APPLICATION(cyclone2_application) = {
    .name = CYCLONE2_APP_NAME,
    .action = ACTION_CYCLONE2_MAIN,
    .ops = &cyclone2_operations,
    .state = APP_STA_DESTROY,
};
