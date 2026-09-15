#include "system/app_core.h"
#include "system/includes.h"
#include "btcontroller_config.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "le_common.h"

#include "app_action.h"
#include "app_comm_bt.h"
#include "app_config.h"
#include "control.h"

#if CONFIG_APP_CYCLONE2

#define CYCLONE2_REPORT_ID              1U
#define CYCLONE2_CLEAR_BOND_ON_BOOT      0
#define CYCLONE2_REPORT_PERIOD_MS        100U
#define CYCLONE2_REPORT_WARMUP_TICKS     30U

/* Report 1: sixteen buttons, four unsigned stick axes, and two unsigned
 * analog triggers.  The report ID is passed separately to the SDK API. */
static const u8 cyclone2_hid_report_map[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x05,             /* Usage (Game Pad) */
    0xa1, 0x01,             /* Collection (Application) */
    0x85, CYCLONE2_REPORT_ID,

    0x05, 0x09,             /* Usage Page (Button) */
    0x19, 0x01,             /* Usage Minimum (Button 1) */
    0x29, 0x10,             /* Usage Maximum (Button 16) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x25, 0x01,             /* Logical Maximum (1) */
    0x75, 0x01,             /* Report Size (1) */
    0x95, 0x10,             /* Report Count (16) */
    0x81, 0x02,             /* Input (Data, Variable, Absolute) */

    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x26, 0xff, 0x00,       /* Logical Maximum (255) */
    0x75, 0x08,             /* Report Size (8) */
    0x95, 0x06,             /* Report Count (6) */
    0x09, 0x30,             /* X  (left stick X) */
    0x09, 0x31,             /* Y  (left stick Y) */
    0x09, 0x33,             /* Rx (right stick X) */
    0x09, 0x34,             /* Ry (right stick Y) */
    0x09, 0x32,             /* Z  (left trigger) */
    0x09, 0x35,             /* Rz (right trigger) */
    0x81, 0x02,             /* Input (Data, Variable, Absolute) */
    0xc0,                   /* End Collection */
};

struct cyclone2_input_report {
    u16 buttons;
    u8 left_x;
    u8 left_y;
    u8 right_x;
    u8 right_y;
    u8 left_trigger;
    u8 right_trigger;
} __attribute__((packed));

static const ble_init_cfg_t cyclone2_ble_config = {
    .same_address = 0,
    .appearance = BLE_APPEARANCE_HID_GAMEPAD,
    .report_map = cyclone2_hid_report_map,
    .report_map_size = sizeof(cyclone2_hid_report_map),
};

/* USB VID source, GameSir VID 0x3537, Cyclone 2 PID 0x0575, version 1.00. */
static const u8 cyclone2_pnp_id[] = {
    0x02, 0x37, 0x35, 0x75, 0x05, 0x00, 0x01,
};

static struct cyclone2_input_report test_report = {
    .buttons = 0,
    .left_x = 0x80,
    .left_y = 0x80,
    .right_x = 0x80,
    .right_y = 0x80,
    .left_trigger = 0,
    .right_trigger = 0,
};

static u16 report_timer;
static u8 connection_ticks;
static int sweep_step = 4;

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void le_hogp_set_PNP_info(const u8 *information);
extern void le_hogp_set_pair_allow(void);
extern void ble_module_enable(u8 enable);
extern int ble_hid_is_connected(void);
extern int ble_hid_data_send(u8 report_id, u8 *data, u16 length);

static void cyclone2_send_test_report(void *private_data)
{
    int next;

    (void)private_data;

    if (!ble_hid_is_connected()) {
        connection_ticks = 0;
        test_report.left_x = 0x80;
        sweep_step = 4;
        return;
    }

    if (connection_ticks < CYCLONE2_REPORT_WARMUP_TICKS) {
        connection_ticks++;
        return;
    }

    next = test_report.left_x + sweep_step;
    if (next >= 0xf0 || next <= 0x10) {
        sweep_step = -sweep_step;
        next = test_report.left_x + sweep_step;
    }
    test_report.left_x = next;

    ble_hid_data_send(CYCLONE2_REPORT_ID,
                      (u8 *)&test_report,
                      sizeof(test_report));
}

static void cyclone2_start(void)
{
    u32 system_clock;

    cyclone2_led_initialize();
    cyclone2_led_set_all(8, 0, 0);

    clk_set("sys", BT_NORMAL_HZ);
    system_clock = clk_get("sys");
    bt_pll_para(TCFG_CLOCK_OSC_HZ, system_clock, 0, 0);

    le_hogp_set_PNP_info(cyclone2_pnp_id);
    btstack_ble_start_before_init(&cyclone2_ble_config, 0);
    btstack_init();

    report_timer = sys_timer_add(NULL,
                                 cyclone2_send_test_report,
                                 CYCLONE2_REPORT_PERIOD_MS);
}

static int cyclone2_state_machine(struct application *application,
                                  enum app_state state,
                                  struct intent *intent)
{
    (void)application;

    if (state == APP_STA_START && intent &&
        intent->action == ACTION_CYCLONE2) {
        cyclone2_start();
    }

    return 0;
}

static int cyclone2_connection_event(struct bt_event *event)
{
    bt_comm_ble_status_event_handler(event);

#if CYCLONE2_CLEAR_BOND_ON_BOOT
    if (event->event == BT_STATUS_INIT_OK) {
        /* bt_ble_init() has just restored the old peer from VM and may have
         * started directed advertising.  Restart advertising only after both
         * halves of the retained bond have been cleared. */
        ble_module_enable(0);
        le_hogp_set_pair_allow();
        ble_module_enable(1);
    }
#endif

    return 0;
}

static int cyclone2_event_handler(struct application *application,
                                  struct sys_event *event)
{
    (void)application;

    if (event->type != SYS_BT_EVENT) {
        return 0;
    }

    if ((u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
        return cyclone2_connection_event(&event->u.bt);
    }

    if ((u32)event->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        return bt_comm_ble_hci_event_handler(&event->u.bt);
    }

    return 0;
}

/* Prevent automatic deep sleep until the application-level power policy and
 * wake sources have been brought over deliberately. */
static u8 cyclone2_idle_query(void)
{
    return 0;
}

REGISTER_LP_TARGET(cyclone2_low_power_target) = {
    .name = "cyclone2",
    .is_idle = cyclone2_idle_query,
};

static const struct application_operation cyclone2_operations = {
    .state_machine = cyclone2_state_machine,
    .event_handler = cyclone2_event_handler,
};

REGISTER_APPLICATION(cyclone2_application) = {
    .name = "cyclone2",
    .action = ACTION_CYCLONE2,
    .ops = &cyclone2_operations,
    .state = APP_STA_DESTROY,
};

#endif
