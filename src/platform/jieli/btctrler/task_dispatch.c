/* Controller task dispatch recovered from btctrler_task.c. */
#include <stdio.h>
#include "btctrler/glue.h"

int os_taskq_pend(const char *name, int *msg, int count);
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
extern BTCTRLER_CONFIG int config_btctler_le_roles;
__attribute__((section(".hci_controller_data"))) void (*bt_testbox_update_msg_handle)(int);
/* Our change: hci_controller_init() spends about 250 ms busy in the radio's
 * calibration (bt_rf_init); the platform runs it below the app's own tasks
 * and puts this task back after. */
void btctrlermod_init_begin(void);
void btctrlermod_init_end(void);

void btctrler_testbox_update_msg_handle_register(void (*handle)(int))
{ bt_testbox_update_msg_handle = handle; }

void btctrler_task(void)
{
    int msg[14];
    btctrlermod_init_begin();
    hci_controller_init();
    btctrlermod_init_end();
    const int classic = config_btctler_modules & 1;
    const int le = (config_btctler_modules & 2) && (config_btctler_le_roles & 255);
    for (;;) {
        if (os_taskq_pend(NULL, msg, 14) != 13) continue; /* OS_TASKQ */
        int *args = msg + 1;
        switch (msg[0]) {
        case LMP_EVENT:
            if (classic && get_bredr_is_init()) lmp_event_handler(args);
            break;
        case LMP_HCI_CMD:
            if (classic && get_bredr_is_init()) lmp_hci_cmd_handler(args);
            break;
        case LMP_HCI_CMD_TO_CONN:
            if (classic && get_bredr_is_init()) lmp_hci_cmd_to_conn_handler(args);
            break;
        case LL_EVENT:
            if (le) ll_event_handler(args);
            break;
        case HCI_CMD_TO_LL:
            if (le) ll_hci_cmd_handler(args);
            break;
        case MSG_TASK_READY:
            os_sem_post((OS_SEM *)(uintptr_t)(u32)args[0]);
            break;
        case MSG_TASK_DEL:
            hci_controller_destory();
            os_sem_post((OS_SEM *)(uintptr_t)(u32)args[0]);
            os_time_dly(20);
            break;
        case MSG_BT_UPDATA_START:
        case MSG_BT_UPDATE_LOADER_DOWNLOAD_START:
        case MSG_BLE_TEST_UPDATA_START:
        case MSG_BLE_TEST_OTA_LOADER_DOWNLOAD_START:
            if (bt_testbox_update_msg_handle) bt_testbox_update_msg_handle(msg[0]);
            break;
        }
        hci_controller_run_loop();
    }
}

/* The SDK's thread owner callbacks intentionally do no work, except resume,
 * which posts a wake message without retrying or propagating its result. */
int bt_task_create(void (*fun)(u8), u8 priority)
{ (void)fun; (void)priority; return 0; }
int bt_task_delete(u8 priority) { (void)priority; return 0; }
int bt_task_suspend(u8 priority, u8 timeout)
{ (void)priority; (void)timeout; return 0; }
int bt_task_resume(void)
{ os_taskq_post_type("btctrler", 255, 0, NULL); return 0; }

/* The owner of the controller task's threads (thread_create): the HCI
 * command thread and the LE link layer's. */
__attribute__((section(".hci_controller_data"))) struct thread_owner bt_task_thread = {
    .create = (int (*)(void (*)(struct thread_owner *), u8))bt_task_create,
    .delete = bt_task_delete, .suspend = bt_task_suspend, .resume = bt_task_resume,
};

#ifdef UNITTEST
static int hook_putchar(int c);
#define putchar hook_putchar
#endif
__attribute__((weak)) int btctrl_msg_qfull_hook(u8 qfull_cnt)
{ (void)qfull_cnt; putchar('H'); return 0; }

#ifdef UNITTEST
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <string.h>
int config_btctler_modules, config_btctler_le_roles;
void btctrlermod_init_begin(void) {}
void btctrlermod_init_end(void) {}
static jmp_buf stop;
struct message { int result, type, initialized; };
static struct message script[64];
static unsigned script_count, next_message;
static int payload[13], probes, run_calls, init_calls, callback_type, hook_char, post_result, posts;
static int expected_modules, expected_roles, mutate_config;
static char trace[512];
static unsigned at;
static void record(char c) { assert(at + 1 < sizeof(trace)); trace[at++] = c; trace[at] = 0; }
static void add(int result, int type, int initialized)
{ assert(script_count < 64); script[script_count++] = (struct message){ result, type, initialized }; }
void hci_controller_init(void)
{
    ++init_calls; record('i');
    /* Configuration is sampled after controller initialization. */
    config_btctler_modules = expected_modules;
    config_btctler_le_roles = expected_roles;
}
int os_taskq_pend(const char *name, int *msg, int count)
{
    assert(!name && count == 14);
    if (next_message == script_count) longjmp(stop, 1);
    struct message *item = &script[next_message++];
    msg[0] = item->type;
    memcpy(msg + 1, payload, sizeof(payload));
    if (mutate_config) config_btctler_modules = config_btctler_le_roles = 0;
    record('q'); return item->result;
}
u8 get_bredr_is_init(void)
{ ++probes; record('b'); return script[next_message - 1].initialized; }
static void check_args(int *args)
{ assert(!memcmp(args, payload, sizeof(payload))); }
void lmp_event_handler(int *args) { check_args(args); record('e'); }
void lmp_hci_cmd_handler(int *args) { check_args(args); record('h'); }
void lmp_hci_cmd_to_conn_handler(int *args) { check_args(args); record('c'); }
void ll_event_handler(int *args) { check_args(args); record('l'); }
void ll_hci_cmd_handler(int *args) { check_args(args); record('a'); }
void hci_controller_destory(void) { record('d'); }
int os_sem_post(OS_SEM *sem)
{ assert((uintptr_t)sem == (u32)payload[0]); record('s'); return -1; }
void os_time_dly(int ticks) { assert(ticks == 20); record('2'); }
void hci_controller_run_loop(void) { ++run_calls; record('r'); }
static void update(int type) { callback_type = type; record('u'); }
static int hook_putchar(int c) { hook_char = c; return -1; }
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{ assert(!strcmp(name, "btctrler") && type == 255 && !argc && !argv); ++posts; return post_result; }
static void reset(int modules, int roles)
{
    script_count = next_message = probes = run_calls = init_calls = callback_type = 0;
    hook_char = post_result = posts = mutate_config = 0;
    expected_modules = modules; expected_roles = roles;
    config_btctler_modules = config_btctler_le_roles = 0;
    bt_testbox_update_msg_handle = NULL; at = 0; trace[0] = 0;
    for (unsigned i = 0; i < 13; ++i) payload[i] = (int)(0x81234567u + i);
}
static void run(void)
{
    if (!setjmp(stop)) btctrler_task();
    assert(init_calls == 1 && next_message == script_count);
}
int main(void)
{
    const int classic_types[] = { LMP_EVENT, LMP_HCI_CMD, LMP_HCI_CMD_TO_CONN };
    const char classic_handlers[] = { 'e', 'h', 'c' };
    for (int modules = 0; modules < 8; ++modules)
        for (int roles = 0; roles <= 511; ++roles)
            for (int initialized = 0; initialized < 2; ++initialized) {
                for (unsigned kind = 0; kind < 3; ++kind) {
                    reset(modules, roles); mutate_config = 1;
                    add(13, classic_types[kind], initialized); run();
                    char expected[8] = "iq";
                    if (modules & 1) strcat(expected, "b");
                    if ((modules & 1) && initialized) {
                        unsigned n = strlen(expected); expected[n] = classic_handlers[kind]; expected[n + 1] = 0;
                    }
                    strcat(expected, "r");
                    assert(!strcmp(trace, expected) && probes == !!(modules & 1) && run_calls == 1);
                }
                for (int kind = 0; kind < 2; ++kind) {
                    reset(modules, roles); mutate_config = 1;
                    add(13, kind ? HCI_CMD_TO_LL : LL_EVENT, initialized); run();
                    int enabled = (modules & 2) && (roles & 255);
                    assert(!strcmp(trace, enabled ? (kind ? "iqar" : "iqlr") : "iqr"));
                    assert(!probes && run_calls == 1);
                }
            }
    for (int result = -2; result < 70; ++result) {
        reset(3, 3); add(result, MSG_TASK_DEL, 1); run();
        assert(!strcmp(trace, result == 13 ? "iqds2r" : "iq"));
        assert(run_calls == (result == 13));
    }
    reset(3, 3);
    add(22, LMP_EVENT, 1); add(13, LMP_EVENT, 0); add(13, LMP_EVENT, 1);
    add(13, MSG_TASK_READY, 1); add(13, MSG_TASK_DEL, 1); add(13, LL_EVENT, 1);
    run(); assert(!strcmp(trace, "iqqbrqberqsrqds2rqlr") && run_calls == 5 && probes == 2);
    /* Unknown and intentionally unhandled queue types still run the controller. */
    const int unknown[] = { INT_MIN, -1, 0, 255, HCI_COMMON_CMD, TWS_LMP_EVENT, INT_MAX };
    for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        reset(3, 3); add(13, unknown[i], 1); run(); assert(!strcmp(trace, "iqr") && !probes);
    }
    for (int type = MSG_BT_UPDATA_START; type <= MSG_BLE_TEST_OTA_LOADER_DOWNLOAD_START; ++type) {
        reset(0, 0); add(13, type, 0); run(); assert(!strcmp(trace, "iqr"));
        reset(0, 0); btctrler_testbox_update_msg_handle_register(update);
        add(13, type, 0); run(); assert(!strcmp(trace, "iqur") && callback_type == type);
        reset(3, 3); btctrler_testbox_update_msg_handle_register(update);
        btctrler_testbox_update_msg_handle_register(NULL);
        add(13, type, 1); run(); assert(!strcmp(trace, "iqr"));
    }
    reset(0, 0);
    assert(!bt_task_create(NULL, 255) && !bt_task_delete(255) && !bt_task_suspend(255, 255));
    for (int result = -1; result < 70; ++result) {
        post_result = result; assert(!bt_task_resume());
    }
    assert(posts == 71);
    for (unsigned count = 0; count < 256; ++count) {
        hook_char = 0; assert(!btctrl_msg_qfull_hook(count) && hook_char == 'H');
    }
    puts("btctrler task dispatch tests passed");
}
#endif
