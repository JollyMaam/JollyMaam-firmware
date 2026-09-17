/* SDK entry point and task inventory.
 *
 * Boot flow for this firmware:
 *   SDK startup -> app_main() below -> registered "cyclone2" application
 *   -> cyclone2_start() in controller.c -> input.c and the selected profile.
 *
 * Most controller policy belongs in controller.c.  Keep this file limited to
 * the SDK-wide startup contract and resources required by its binary libs.
 */
/*********************************************************************************************
    *   Filename        : app_main.c

    *   Description     :

    *   Author          : Bingquan

    *   Email           : caibingquan@zh-jieli.com

    *   Last modifiled  : 2019-05-11 14:54

    *   Copyright:(c)JIELI  2011-2019  @ , All Rights Reserved.
*********************************************************************************************/
#include "system/includes.h"
#include "app_config.h"
#include "app_action.h"
#include "app_main.h"
#include "update.h"
#include "update_loader_download.h"
#include "app_charge.h"
#include "app_power_manage.h"
#include "asm/charge.h"

#define LOG_TAG_CONST       APP
#define LOG_TAG             "[APP]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

/* TRANSLATED: Task table. */
const struct task_info task_info_table[] = {
    {"app_core",            1,     0,   640,   128  },
    {"sys_event",           7,     0,   256,   0    },
    {"btctrler",            4,     0,   512,   256  },
    {"btencry",             1,     0,   512,   128  },
    {"btstack",             3,     0,   768,   256  },
    {"systimer",		    7,	   0,   128,   0    },
    {"update",				1,	   0,   512,   0    },

    {0, 0},
};

APP_VAR app_var;

__attribute__((weak))
u8 get_charge_online_flag(void)
{
    return 0;
}

void app_main()
{
    struct intent it;

    if (!UPDATE_SUPPORT_DEV_IS_NULL()) {
        int update = 0;
        update = update_result_deal();
    }

    printf(">>>>>>>>>>>>>>>>>app_main...\n");

    if (get_charge_online_flag()) {
        vbat_check_init();
    } else {
        check_power_on_voltage();
    }

    init_intent(&it);

    it.name = "cyclone2";
    it.action = ACTION_CYCLONE2;

    log_info("run app>>> %s", it.name);
    log_info("%s,%s", __DATE__, __TIME__);

    start_app(&it);

}

int eSystemConfirmStopStatus(void)
{
    /* TRANSLATED: With no pending task timeout, choose indefinite sleep or
     * a periodic 100 ms wakeup. */
    //1:Endless Sleep
    //0:100 ms wakeup
    /* log_info("100ms wakeup"); */
    return 1;
}

__attribute__((used)) int *__errno()
{
    static int err;
    return &err;
}
