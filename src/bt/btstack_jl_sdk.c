/*
 * Jieli SDK side of the BTstack port.
 *
 * The application modules (profile_switch.c, controller.c, ...) are written
 * against the vendor host library's little API - btstack_init(),
 * btstack_edr_start_before_init(), user_send_cmd_prepare(), edr_hid_data_send()
 * and the SYS_BT_EVENT notifications.  This file keeps that API and implements
 * it on top of the upstream BTstack host in btstack_jl_core.c, so the vendor
 * btstack.a is not linked any more.
 *
 * It is the only file that includes both the Jieli SDK headers and the port
 * interface; btstack_jl_core.c stays free of SDK types.
 */

#include <system/includes.h>
#include <system/task.h>
#include <system/timer.h>
#include <btstack/avctp_user.h>

#include "app_config.h"
#include "usb.h"

#include "app_comm_bt.h"
#include "main.h"
#include "btstack_jl.h"

#ifndef CFG_REMOTE_DB_00
#define CFG_REMOTE_DB_00 62
#endif

/* One entry per bonded peer: address, link key, key type. */
#define BTSTACK_JL_LINK_KEY_ENTRIES 16
#define BTSTACK_JL_LINK_KEY_SIZE    (6 + 16 + 1)

static const edr_init_cfg_t *edr_config;
static const u8 *edr_report_map;
static u16 edr_report_map_size;
static u32 edr_class_of_device;

static u8 stack_ready;
static u8 user_pairing_requested;
static u8 peer_address[6];
static u8 peer_address_valid;
static u8 hid_ready;
static u8 task_exited;

static void (*hid_output_handler)(u8 *packet, u16 size, u16 channel);
static void (*hid_send_wakeup)(void);

/* ----------------------------------------------------------------- logging */

#define BTSTACK_JL_LOG(...) cyclone2_usb_debug("BTSTACK: " __VA_ARGS__)

/* ------------------------------------------------------------- link keys */

static int link_key_slot_read(u8 slot, u8 *record)
{
    int length = syscfg_read((u16)(CFG_REMOTE_DB_00 + slot), record,
                             BTSTACK_JL_LINK_KEY_SIZE);
    return length == BTSTACK_JL_LINK_KEY_SIZE;
}

static int link_key_slot_write(u8 slot, const u8 *address, const u8 *key, u8 type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];

    memcpy(record, address, 6);
    memcpy(record + 6, key, 16);
    record[22] = type;
    return syscfg_write((u16)(CFG_REMOTE_DB_00 + slot), record,
                        sizeof(record)) == sizeof(record);
}

static void link_key_slot_clear(u8 slot)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];

    memset(record, 0, sizeof(record));
    syscfg_write((u16)(CFG_REMOTE_DB_00 + slot), record, sizeof(record));
}

static int jl_link_key_get(const u8 address[6], u8 key[16], u8 *type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 slot;

    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) continue;
        if (memcmp(record, address, 6) != 0) continue;
        memcpy(key, record + 6, 16);
        *type = record[22];
        return 1;
    }
    return 0;
}

static void jl_link_key_put(const u8 address[6], const u8 key[16], u8 type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 slot;

    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) break;
        if (memcmp(record, address, 6) == 0) break;
    }
    if (slot == BTSTACK_JL_LINK_KEY_ENTRIES) {
        slot = 0;
    }
    link_key_slot_write(slot, address, key, type);
}

static void jl_link_key_delete(const u8 address[6])
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 slot;

    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) continue;
        if (memcmp(record, address, 6) != 0) continue;
        link_key_slot_clear(slot);
    }
}

static int jl_link_key_iterator(u8 index, u8 address[6], u8 key[16], u8 *type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 slot = index;
    u8 valid;

    while (slot < BTSTACK_JL_LINK_KEY_ENTRIES) {
        if (link_key_slot_read(slot, record)) {
            valid = 0;
            for (u8 i = 0; i < 6; i++) valid |= record[i];
            if (valid != 0) {
                memcpy(address, record, 6);
                memcpy(key, record + 6, 16);
                *type = record[22];
                return 1;
            }
        }
        slot++;
    }
    return 0;
}

/* ----------------------------------------------------------------- events */

static u32 jl_time_ms(void)
{
    return sys_timer_get_ms();
}

static void jl_critical_enter(void)
{
    local_irq_disable();
}

static void jl_critical_exit(void)
{
    local_irq_enable();
}

static void jl_trace(u32 code)
{
    cyclone2_bringup_trace = (code << 16) | (cyclone2_boot_count & 0xffffU);
    cyclone2_bringup_record(code);
}

static void bt_event_post(u32 arg_type, u8 event, const u8 *args, u32 value)
{
    struct sys_event e;

    e.type = SYS_BT_EVENT;
    e.arg = (void *) arg_type;
    e.u.bt.event = event;
    if (args != 0) {
        memcpy(e.u.bt.args, args, 7);
    } else {
        memset(e.u.bt.args, 0, sizeof(e.u.bt.args));
    }
    e.u.bt.value = value;
    sys_event_notify(&e);
}

/* Notifications from the BTstack task.  Runs in the "btstack" task context;
 * sys_event_notify() hands the event to the application task. */
static void jl_event_handler(const btstack_jl_event_t *event)
{
    switch (event->type) {
        case BTSTACK_JL_EVENT_READY:
            stack_ready = 1;
            BTSTACK_JL_LOG("stack ready");
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_INIT_OK, 0, 0);
            break;

        case BTSTACK_JL_EVENT_CONNECTED:
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_FIRST_CONNECTED,
                          event->address, 0);
            break;

        case BTSTACK_JL_EVENT_ENCRYPTED:
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_ENCRY_COMPLETE,
                          event->address, 0);
            break;

        case BTSTACK_JL_EVENT_DISCONNECTED:
            hid_ready = 0;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_FIRST_DISCONNECT,
                          event->address, event->status);
            break;

        case BTSTACK_JL_EVENT_HID_OPENED:
            hid_ready = 1;
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            BTSTACK_JL_LOG("hid channels open");
            break;

        case BTSTACK_JL_EVENT_HID_CLOSED:
            hid_ready = 0;
            break;

        case BTSTACK_JL_EVENT_HCI_STATUS:
            bt_event_post(SYS_BT_EVENT_TYPE_HCI_STATUS, event->code,
                          event->address, event->status);
            break;

        default:
            break;
    }
}

/* ----------------------------------------------------------- stack task */

static void btstack_task(void *argument)
{
    (void) argument;

    cyclone2_bringup_record(6U);
    btstack_jl_core_run(0);
    cyclone2_bringup_record(7U);
    task_exited = 1;

    /* The application task deletes us; never return from task_create(). */
    for (;;) {
        os_time_dly(50);
    }
}

/* ------------------------------------------------- vendor host API ---- */

void btstack_edr_start_before_init(const edr_init_cfg_t *cfg, int param)
{
    (void) param;

    cyclone2_bringup_record(1U);
    edr_config = cfg;
    edr_report_map = cfg->report_map;
    edr_report_map_size = cfg->report_map_size;
    edr_class_of_device = cfg->class_type;
    hid_ready = 0;
    stack_ready = 0;
    task_exited = 0;
    peer_address_valid = 0;
}

void btstack_init(void)
{
    btstack_jl_config_t config;
    const u8 *address = bt_get_mac_addr();

    cyclone2_bringup_record(2U);
    memset(&config, 0, sizeof(config));
    config.local_name = bt_get_local_name();
    config.class_of_device = edr_class_of_device;
    if (address != 0) {
        memcpy(config.bd_addr, address, sizeof(config.bd_addr));
    }
    config.report_descriptor = edr_report_map;
    config.report_descriptor_size = edr_report_map_size;
    config.io_capability = edr_config != 0 ? edr_config->io_capabilities : 3;
    config.authentication_requirement = edr_config != 0 ? edr_config->authentication_req : 4;
    config.page_timeout_ms = edr_config != 0 ? edr_config->page_timeout : 8000;
    config.supervision_timeout_ms = edr_config != 0 ? edr_config->super_timeout : 8000;
    config.link_key_get = &jl_link_key_get;
    config.link_key_put = &jl_link_key_put;
    config.link_key_delete = &jl_link_key_delete;
    config.link_key_iterator = &jl_link_key_iterator;
    config.time_ms = &jl_time_ms;
    config.critical_enter = &jl_critical_enter;
    config.critical_exit = &jl_critical_exit;
    config.trace = &jl_trace;

    cyclone2_bringup_record(3U);
    BTSTACK_JL_LOG("init name=%s", config.local_name ? config.local_name : "?");

    if (btstack_jl_core_init(&config, &jl_event_handler) != 0) {
        BTSTACK_JL_LOG("init failed");
        return;
    }

    cyclone2_bringup_record(4U);
    BTSTACK_JL_LOG("creating task");
    task_create(btstack_task, NULL, "btstack");
    cyclone2_bringup_record(5U);
}

void btstack_edr_exit(int param)
{
    int wait;

    (void) param;

    user_pairing_requested = 0;
    hid_ready = 0;
    stack_ready = 0;
    btstack_jl_core_power_off();

    for (wait = 0; (wait < 100) && !task_exited; wait++) {
        os_time_dly(1);
    }
    task_delete("btstack");
    task_exited = 0;
}

/* Enable page and inquiry scan (pairing), or disable both (paging out). */
void bt_wait_phone_connect_control(u8 enable)
{
    user_pairing_requested = enable ? 1 : 0;
    btstack_jl_core_set_scan(enable ? 1 : 0, enable ? 1 : 0);
}

void bt_wait_phone_connect_control_ext(u8 inquiry_en, u8 page_scan_en)
{
    btstack_jl_core_set_scan(page_scan_en ? 1 : 0, inquiry_en ? 1 : 0);
}

/* Page the peer this controller was bonded to last. */
int bt_connect_phone_back_start(void)
{
    u8 address[6];
    u8 key[16];
    u8 type;
    u8 index;

    if (peer_address_valid) {
        return btstack_jl_core_hid_connect(peer_address) == 0 ? 1 : 0;
    }

    for (index = 0; index < BTSTACK_JL_LINK_KEY_ENTRIES; index++) {
        if (!jl_link_key_iterator(index, address, key, &type)) {
            break;
        }
        if (btstack_jl_core_hid_connect(address) == 0) {
            memcpy(peer_address, address, sizeof(peer_address));
            peer_address_valid = 1;
            return 1;
        }
    }
    return 0;
}

u32 user_send_cmd_prepare(USER_CMD_TYPE cmd, u16 param_len, u8 *param)
{
    switch (cmd) {
        case USER_CTRL_START_CONNEC_VIA_ADDR:
        case USER_CTRL_START_CONNEC_VIA_ADDR_MANUALLY:
            if (param_len < 6 || param == 0) return 2;
            memcpy(peer_address, param, sizeof(peer_address));
            peer_address_valid = 1;
            return btstack_jl_core_hid_connect(param) == 0 ? 0 : 1;

        case USER_CTRL_DISCONNECTION_HCI:
            return btstack_jl_core_disconnect();

        case USER_CTRL_HID_CONN:
            if (!peer_address_valid) return 1;
            return btstack_jl_core_hid_connect(peer_address) == 0 ? 0 : 1;

        case USER_CTRL_HID_DISCONNECT:
            return btstack_jl_core_disconnect();

        case USER_CTRL_HID_SEND_DATA: {
            hid_s_param_t *send = (hid_s_param_t *) param;
            if (param_len < sizeof(hid_s_param_t) || send == 0) return 2;
            /* data_ptr already carries the HIDP header and report id. */
            return btstack_jl_core_send_report(send->data_ptr, send->data_len);
        }

        case USER_CTRL_DEL_ALL_REMOTE_INFO:
            peer_address_valid = 0;
            hid_ready = 0;
            memset(peer_address, 0, sizeof(peer_address));
            btstack_jl_core_delete_all_link_keys();
            for (u8 slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
                link_key_slot_clear(slot);
            }
            return 0;

        case USER_CTRL_ALL_SNIFF_EXIT:
            /* The controller owns the sniff state machine. */
            return 0;

        default:
            return 1;
    }
}

/* Vendor controller-library hook: keep the link alive when a peer rejects a
 * stale link key so the same connection can fall back to a fresh pairing. */
extern void edr_vendor_keep_connect_for_keymiss(u8 enable);

int user_hid_send_data(u8 *buf, u32 len)
{
    return btstack_jl_core_send_report(buf, (u16) len);
}

void user_hid_disconnect(void)
{
    btstack_jl_core_disconnect();
}

void user_hid_init(void (*output_handler)(u8 *packet, u16 size, u16 channel))
{
    hid_output_handler = output_handler;
}

void user_hid_exit(void)
{
    hid_output_handler = 0;
    hid_send_wakeup = 0;
    hid_ready = 0;
}

void user_hid_enable(u8 enable)
{
    if (!enable) {
        user_hid_disconnect();
    }
}

void user_hid_set_icon(u32 class_type)
{
    edr_class_of_device = class_type;
}

void user_hid_set_ReportMap(u8 *map, u16 size)
{
    edr_report_map = map;
    edr_report_map_size = size;
}

void user_hid_regiser_wakeup_send(void *cbk)
{
    hid_send_wakeup = (void (*)(void)) cbk;
}

int edr_hid_data_send_ext(u8 report_type, u8 report_id, u8 *data, u16 len)
{
    u8 message[64];

    if (len + 2 > sizeof(message)) {
        return 2;
    }
    message[0] = report_type;
    message[1] = report_id;
    memcpy(message + 2, data, len);
    return btstack_jl_core_send_report(message, (u16)(len + 2));
}

int edr_hid_data_send(u8 report_id, u8 *data, u16 len)
{
    return edr_hid_data_send_ext(0xa1U, report_id, data, len);
}

int edr_hid_is_connected(void)
{
    return btstack_jl_core_hid_connected();
}

int edr_hid_is_busy(void)
{
    return 0;
}

int edr_hid_tx_buff_is_ok(void)
{
    return 1;
}

/* The application still calls this before it starts a session; the controller
 * in this build keeps keys itself, so only the host bookkeeping is affected. */
void delete_link_key(bd_addr_t *address, u8 index)
{
    (void) index;
    if (address == 0) return;
    jl_link_key_delete((const u8 *) address);
    btstack_jl_core_delete_link_key((const u8 *) address);
}

/* The vendor stack emulated the console's own event handling here; keep the
 * symbols so the profile code keeps compiling, they only trace now. */
int bt_comm_edr_status_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

int bt_comm_edr_hci_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

int bt_comm_edr_sniff_clean(void)
{
    return 1;
}

void set_multi_devices_adv_flag(u8 flag)
{
    (void) flag;
}

/* BTstack asserts through the C library's assert(); the Jieli runtime has no
 * __assert_func, so trap here where the console can show the failure. */
void __assert_func(const char *file, int line, const char *function,
                   const char *expression)
{
    cyclone2_usb_debug("BTSTACK assert %s:%d %s: %s", file, line,
                       function != 0 ? function : "?", expression);
    for (;;) {
    }
}

u8 btstack_jl_is_ready(void)
{
    return stack_ready;
}

u8 btstack_jl_hid_is_ready(void)
{
    return hid_ready;
}
