#include "system/includes.h"
#include "system/app_core.h"
#include "cyclone2_app.h"
#include "app_main.h"

/* The SDK startup code discovers this table by symbol name. Applications add
 * named tasks here so task_create() can use fixed, auditable resources. */
const struct task_info task_info_table[] = {
    {"app_core",   1, 0, 640, 128},
    {"sys_event",  7, 0, 256,   0},
    {"systimer",   7, 0, 128,   0},
    {"c2_control", 2, 0, 256,   0},
    {0, 0},
};

APP_VAR app_var;

void app_var_init(void)
{
    app_var.auto_off_time = 0;
    app_var.warning_tone_v = 340;
    app_var.poweroff_tone_v = 330;
}

u16 update_result_get(void)
{
    return 0;
}

int eSystemConfirmStopStatus(void)
{
    return 1; /* no periodic 100 ms wake while the system is idle */
}

void app_main(void)
{
    struct intent intent;

    init_intent(&intent);
    intent.name = CYCLONE2_APP_NAME;
    intent.action = ACTION_CYCLONE2_MAIN;
    start_app(&intent);
}
