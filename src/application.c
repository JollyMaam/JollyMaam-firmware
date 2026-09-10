#include "system/app_core.h"
#include "system/includes.h"
#include "cyclone2_app.h"

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
