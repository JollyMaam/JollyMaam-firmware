/* Controller lifecycle and Classic queue draining, recovered from
 * btctrler_task.c. Radio primitives and the task loop remain SDK APIs. */
#include <stdlib.h>
#include "btctrler/glue.h"

#ifdef CONFIG_CPU_BD19
#define TASK_CODE __attribute__((section(".hci_controller_code")))
#else
#define TASK_CODE
#endif

extern BTCTRLER_CONFIG u8 log_tag_const_i_LMP, log_tag_const_e_LMP;
void log_print(int level, const char *tag, const char *format, ...);
void sys_event_notify(struct sys_event *event);
int os_sem_create(OS_SEM *sem, int count);
int os_sem_pend(OS_SEM *sem, int timeout);
int os_taskq_pend(const char *name, int *msg, int count);
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
/* The host transport hci_interface_setup registers (btctrler_task.c). */
__attribute__((section(".hci_controller_bss"))) const hci_transport_t *h4_transport;
__attribute__((section(".hci_controller_bss"))) const void *h4_config;
#ifdef CONFIG_CPU_BD19
static __attribute__((section(".hci_controller_data")))
void (*bt_testbox_update_msg_handle)(int);
#endif

int task_create(void (*entry)(void *), void *priv, const char *name);
int task_kill(const char *name);
#ifdef CONFIG_CPU_BD19
static void btctrler_task(void);
#else
void btctrler_task(void);
#endif

#ifdef UNITTEST
static void *test_alloc(size_t size);
static void test_free(void *ptr);
#define malloc test_alloc
#define free test_free
#else
_Static_assert(sizeof(OS_SEM) == 80, "SDK task semaphore size");
_Static_assert(sizeof(void *) == sizeof(int), "SDK task message pointer size");
#endif

static TASK_CODE void info(const char *message)
{ if (log_tag_const_i_LMP) log_print(2, NULL, message); }
static TASK_CODE int modules_supported(void)
{
    if (config_btctler_modules & 3) return 1;
    if (log_tag_const_e_LMP)
        log_print(4, NULL, "<Error>: [LMP]>>>BT_MODULES_IS_SUPPORT None<<<\r\n");
    return 0;
}

/* The task acknowledges these messages through the supplied semaphore.
 * A failed post must not leave the caller waiting for an absent message. */
static TASK_CODE int synchronize_task(int type)
{
    OS_SEM *sem = malloc(sizeof(*sem));
    if (!sem) {
        btctrler_assert_failed("controller task semaphore allocation", 0);
#ifndef CONFIG_CPU_BD19
        return -22;
#endif
    }
    int create_result = os_sem_create(sem, 0);
#ifdef CONFIG_CPU_BD19
    (void)create_result;
#else
    if (create_result) {
        free(sem);
        return create_result;
    }
#endif
    int word = (int)(uintptr_t)sem;
    int result = os_taskq_post_type("btctrler", type, 1, &word);
#ifdef CONFIG_CPU_BD19
    /* Queue/pend callbacks can update the escaped message word. */
    if (!result) os_sem_pend((OS_SEM *)(uintptr_t)(u32)word, 0);
    free((OS_SEM *)(uintptr_t)(u32)word);
#else
    if (!result) os_sem_pend(sem, 0);
    free(sem);
#endif
    return result;
}
TASK_CODE int btctrler_task_ready(void)
{ return synchronize_task(MSG_TASK_READY) == 0; }
TASK_CODE void btctrler_resume_req(void)
{
    struct sys_event event = {0};
    event.type = 16;
    event.arg = (void *)(uintptr_t)1129599488;
    event.u.bt.event = BTCTRLER_EVENT_RESUME_REQ;
    sys_event_notify(&event);
}
TASK_CODE void bredr_bulk_change(u8 mode)
{
    bredr_close_all_scan();
    link_task_schedule_resume();
    for (;;) {
        lmp_conn_suspend(0);
        if (bredr_rx_bulk_empty()) break;
        info("[Info]: [LMP]wait_suspend 0\n\r\n");
        os_time_dly(2);
    }
    for (;;) {
        lmp_tx_suspend();
        if (bredr_tx_bulk_empty()) break;
        info("[Info]: [LMP]wait_suspend 1\n\r\n");
        os_time_dly(2);
    }
    link_change_bulk_init(mode);
}
TASK_CODE int btctrler_suspend(u8 suspend_rx_bulk)
{
    if (!(config_btctler_modules & 1)) return 0;
    bredr_power_suspend();
    if (suspend_rx_bulk) bredr_rx_bulk_suspend();
    bredr_goto_background();
    unsigned count = 0;
    for (;;) {
        lmp_conn_suspend(suspend_rx_bulk);
        if (bredr_rx_bulk_empty()) break;
        info("[Info]: [LMP]wait_suspend\n\r\n");
        os_time_dly(2);
        if (!suspend_rx_bulk && count++ == 5) break;
    }
    info("[Info]: [LMP]suspend_exit\n\r\n");
    return 0;
}
TASK_CODE void btctrler_resume(void)
{
    bredr_rx_bulk_resume();
    lmp_conn_resume();
    bredr_power_resume();
}
TASK_CODE u8 bredr_bulk_change_rx_bulk(u8 mode)
{
    bredr_close_all_scan();
    link_task_schedule_resume();
    if (bredr_rx_bulk_empty() && (mode || bredr_rx_remain_bulk_empty())) {
        link_change_rx_bulk_size(!!mode);
        return 1;
    }
    info(mode ? "[Info]: [LMP]wait_suspend 0\n\r\n" :
                "[Info]: [LMP]wait_suspend 1\n\r\n");
    return 0;
}
TASK_CODE int btctrler_task_init(const void *transport, const void *config)
{
    if (!modules_supported()) return 0;
    info("[Info]: [LMP]>>>btctrler_task_init<<<\r\n");
    h4_transport = transport;
    h4_config = config;
    task_create((void (*)(void *))btctrler_task, NULL, "btctrler");
    return 0;
}
static TASK_CODE void wait_links(void)
{
    /* Probe once more at the limit, as the SDK does, before proceeding. */
    for (unsigned count = 0; bredr_link_check_used() && count < 100; ++count)
        os_time_dly(3);
}
static TASK_CODE void wait_connections(void)
{
    for (unsigned count = 0; lmp_get_conn_num() && count < 100; ++count)
        os_time_dly(3);
}
TASK_CODE int btctrler_task_exit(void)
{
#ifdef CONFIG_CPU_BD19
    /* The SDK snapshots module selection before allocation and callbacks. */
    int modules = config_btctler_modules;
    if (!(modules & 3)) {
        if (log_tag_const_e_LMP)
            log_print(4, NULL, "<Error>: [LMP]>>>BT_MODULES_IS_SUPPORT None<<<\r\n");
        return 0;
    }
#else
    if (!modules_supported()) return 0;
#endif
#ifdef CONFIG_CPU_BD19
    OS_SEM *sem = malloc(sizeof(*sem));
    if (!sem) btctrler_assert_failed("controller task semaphore allocation", 0);
    int create_result = os_sem_create(sem, 0);
    (void)create_result;
    if (modules & 1) {
        wait_links();
        btctrler_hci_cmd_to_task(LMP_HCI_CMD, 1, 0x0c03);
        wait_connections();
    }
    int word = (int)(uintptr_t)sem;
    os_taskq_post_type("btctrler", MSG_TASK_DEL, 1, &word);
    os_sem_pend((OS_SEM *)(uintptr_t)(u32)word, 0);
    free((OS_SEM *)(uintptr_t)(u32)word);
    info("[Info]: [LMP]btctrler_exit\n\r\n");
    task_kill("btctrler");
    return 0;
#else
    /* Allocate before draining, preserving the SDK's allocation-failure order. */
    OS_SEM *sem = malloc(sizeof(*sem));
    if (!sem) {
        btctrler_assert_failed("controller task semaphore allocation", 0);
        return -22;
    }
    int result = os_sem_create(sem, 0);
    if (result) { free(sem); return result; }
    if (config_btctler_modules & 1) {
        wait_links();
        btctrler_hci_cmd_to_task(LMP_HCI_CMD, 1, 0x0c03);
        wait_connections();
    }
    int word = (int)(uintptr_t)sem;
    result = os_taskq_post_type("btctrler", MSG_TASK_DEL, 1, &word);
    if (!result) os_sem_pend(sem, 0);
    free(sem);
    if (result) return result;
    info("[Info]: [LMP]btctrler_exit\n\r\n");
    task_kill("btctrler");
    return 0;
#endif
}
TASK_CODE int btctrler_task_close_bredr(void)
{
    if (!modules_supported()) return 0;
    wait_links();
    wait_connections();
    link_task_close_all();
    bredr_link_close();
    bredr_bd_close();
    return 0;
}
TASK_CODE void btctrler_task_init_bredr(void) { bredr_bd_init(); }

#ifdef CONFIG_CPU_BD19
TASK_CODE void btctrler_testbox_update_msg_handle_register(void (*handle)(int))
{ bt_testbox_update_msg_handle = handle; }

/* The SDK's task entry has internal linkage. Keep it with task_create in the
 * BD19 split source so the owned object does not export this dormant symbol. */
static TASK_CODE void btctrler_task(void)
{
    int msg[14];
    hci_controller_init();
    const int classic = config_btctler_modules & 1;
    const int le = (config_btctler_modules & 2) && (config_btctler_le_roles & 255);
    for (;;) {
        if (os_taskq_pend(NULL, msg, 14) != 13) continue;
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
#endif

#ifdef UNITTEST
#include <assert.h>
#include <stdio.h>
#include <string.h>
int config_btctler_modules;
u8 log_tag_const_i_LMP, log_tag_const_e_LMP;
static OS_SEM test_sem;
#ifdef CONFIG_CPU_BD19
static OS_SEM post_sem, pend_sem;
static int mutate_word, create_modules;
static int *saved_word;
#endif
static int allocation_fails, create_result, post_result, allocations, frees, assertions;
static int rx_busy, tx_busy, remain_busy, links_busy, connections_busy, remaining_checks;
static int last_suspend, last_mode, last_type, logs;
static struct sys_event notified;
static char trace[4096];
static unsigned at;
static void record(char c) { assert(at + 1 < sizeof(trace)); trace[at++] = c; trace[at] = 0; }
static void *test_alloc(size_t size)
{ assert(size == sizeof(OS_SEM)); ++allocations; record('a'); return allocation_fails ? NULL : &test_sem; }
static void test_free(void *ptr)
{
#ifdef CONFIG_CPU_BD19
    void *expected = !mutate_word ? &test_sem :
        last_type == MSG_TASK_READY && post_result ? &post_sem : &pend_sem;
    assert(ptr == expected);
#else
    assert(ptr == &test_sem);
#endif
    ++frees; record('f');
}
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; ++assertions; record('!'); }
void log_print(int level, const char *tag, const char *format, ...)
{ assert(level == 2 || level == 4); assert(!tag && format); ++logs; }
int os_sem_create(OS_SEM *sem, int count)
{
    assert(sem == &test_sem && !count); record('c');
#ifdef CONFIG_CPU_BD19
    if (create_modules >= 0) config_btctler_modules = create_modules;
#endif
    return create_result;
}
int os_sem_pend(OS_SEM *sem, int timeout)
{
#ifdef CONFIG_CPU_BD19
    assert(sem == (mutate_word ? &post_sem : &test_sem) && !timeout);
    if (mutate_word) *saved_word = (int)(uintptr_t)&pend_sem;
#else
    assert(sem == &test_sem && !timeout);
#endif
#ifndef CONFIG_CPU_BD19
    assert(!post_result);
#endif
    record('p'); return 0;
}
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    assert(!strcmp(name, "btctrler") && argc == 1);
    assert(argv[0] == (int)(uintptr_t)&test_sem);
    assert(type == MSG_TASK_READY || type == MSG_TASK_DEL);
#ifdef CONFIG_CPU_BD19
    saved_word = argv;
    if (mutate_word) *argv = (int)(uintptr_t)&post_sem;
#endif
    last_type = type; record('q'); return post_result;
}
int task_create(void (*entry)(void *), void *priv, const char *name)
{
    assert(entry == (void (*)(void *))btctrler_task && !priv && !strcmp(name, "btctrler"));
    assert(h4_transport == (const void *)(uintptr_t)0x1234 && h4_config == (const void *)(uintptr_t)0x5678);
    record('t'); return -1; /* SDK initializer ignores this result. */
}
#ifndef CONFIG_CPU_BD19
void btctrler_task(void) { assert(0); }
#endif
int task_kill(const char *name) { assert(!strcmp(name, "btctrler")); record('k'); return 0; }
void sys_event_notify(struct sys_event *event) { notified = *event; record('e'); }
void bredr_close_all_scan(void) { record('s'); }
void link_task_schedule_resume(void) { record('r'); }
int lmp_conn_suspend(u8 mode) { last_suspend = mode; record('l'); return -1; }
int bredr_rx_bulk_empty(void) { record('x'); return rx_busy-- <= 0; }
void lmp_tx_suspend(void) { record('v'); }
int bredr_tx_bulk_empty(void) { record('y'); return tx_busy-- <= 0; }
int link_change_bulk_init(u8 mode) { last_mode = mode; record('b'); return -1; }
void os_time_dly(int ticks) { assert(ticks == 2 || ticks == 3); record(ticks == 2 ? '2' : '3'); }
void bredr_power_suspend(void) { record('P'); }
void bredr_rx_bulk_suspend(void) { record('S'); }
void bredr_goto_background(void) { record('B'); }
void bredr_rx_bulk_resume(void) { record('R'); }
void lmp_conn_resume(void) { record('L'); }
void bredr_power_resume(void) { record('W'); }
int bredr_rx_remain_bulk_empty(void) { ++remaining_checks; record('z'); return !remain_busy; }
int link_change_rx_bulk_size(u8 mode) { last_mode = mode; record('m'); return -1; }
u8 bredr_link_check_used(void) { record('u'); return links_busy-- > 0; }
u8 lmp_get_conn_num(void) { record('n'); return connections_busy-- > 0; }
int btctrler_hci_cmd_to_task(int type, int argc, ...)
{
    va_list args; va_start(args, argc);
    assert(type == LMP_HCI_CMD && argc == 1 && va_arg(args, int) == 0x0c03);
    va_end(args); record('h'); return 0;
}
void link_task_close_all(void) { record('C'); }
void bredr_link_close(void) { record('D'); }
void bredr_bd_close(void) { record('E'); }
void bredr_bd_init(void) { record('I'); }
static void reset(void)
{
#ifdef CONFIG_CPU_BD19
    mutate_word = 0; create_modules = -1; saved_word = NULL;
#endif
    allocation_fails = create_result = post_result = allocations = frees = assertions = 0;
    rx_busy = tx_busy = remain_busy = links_busy = connections_busy = remaining_checks = 0;
    last_suspend = last_mode = last_type = logs = 0;
    config_btctler_modules = 3; log_tag_const_i_LMP = log_tag_const_e_LMP = 0;
    h4_transport = NULL; h4_config = NULL; at = 0; trace[0] = 0;
}
static unsigned occurrences(char c)
{ unsigned n = 0; for (unsigned i = 0; i < at; ++i) n += trace[i] == c; return n; }
int main(void)
{
    reset(); assert(btctrler_task_ready() == 1 && !strcmp(trace, "acqpf") && last_type == MSG_TASK_READY);
    reset(); post_result = 21;
    assert(!btctrler_task_ready() && !strcmp(trace, "acqf"));
#ifndef CONFIG_CPU_BD19
    reset(); allocation_fails = 1;
    assert(!btctrler_task_ready() && assertions == 1 && !frees && !strcmp(trace, "a!"));
    reset(); create_result = -4;
    assert(!btctrler_task_ready() && !strcmp(trace, "acf"));
#else
    reset(); create_result = -4;
    assert(btctrler_task_ready() && !strcmp(trace, "acqpf"));
#endif
#ifdef CONFIG_CPU_BD19
    reset(); mutate_word = 1;
    assert(btctrler_task_ready() && !strcmp(trace, "acqpf") && frees == 1);
    reset(); mutate_word = 1; post_result = 21;
    assert(!btctrler_task_ready() && !strcmp(trace, "acqf") && frees == 1);
    reset(); mutate_word = 1;
    assert(!btctrler_task_exit() && !strcmp(trace, "acuhnqpfk") && frees == 1);
    reset(); mutate_word = 1; post_result = 21;
    assert(!btctrler_task_exit() && !strcmp(trace, "acuhnqpfk") && frees == 1);
    reset(); create_modules = 2;
    assert(!btctrler_task_exit() && !strcmp(trace, "acuhnqpfk"));
    reset(); config_btctler_modules = 2; create_modules = 3;
    assert(!btctrler_task_exit() && !strcmp(trace, "acqpfk"));
#endif
    reset(); btctrler_resume_req();
    assert(!strcmp(trace, "e") && notified.type == 16 && !notified.consumed);
    assert((uintptr_t)notified.arg == 1129599488 && notified.u.bt.event == 1 && !notified.u.bt.value);
    for (unsigned mode = 0; mode < 256; ++mode) {
        reset(); rx_busy = 2; tx_busy = 1;
        bredr_bulk_change(mode);
        assert(!strcmp(trace, "srlx2lx2lxvy2vyb") && !last_suspend && last_mode == (int)mode);
        reset(); btctrler_resume(); assert(!strcmp(trace, "RLW"));
        for (int busy = 0; busy < 2; ++busy)
            for (int remain = 0; remain < 2; ++remain) {
                reset(); rx_busy = busy; remain_busy = remain;
                int changed = !busy && (mode || !remain);
                assert(bredr_bulk_change_rx_bulk(mode) == changed);
                assert(remaining_checks == (!busy && !mode));
                assert(occurrences('m') == (unsigned)changed && (!changed || last_mode == !!mode));
                assert(!strncmp(trace, "srx", 3));
            }
        reset(); rx_busy = 8; assert(!btctrler_suspend(mode));
        assert(last_suspend == (int)mode && occurrences('l') == (mode ? 9u : 6u));
        assert(occurrences('2') == (mode ? 8u : 6u));
        assert(!strncmp(trace, mode ? "PSB" : "PB", mode ? 3 : 2));
    }
    for (int modules = 0; modules < 8; ++modules) {
        reset(); config_btctler_modules = modules;
        assert(!btctrler_task_init((void *)(uintptr_t)0x1234, (void *)(uintptr_t)0x5678));
        assert(!strcmp(trace, modules & 3 ? "t" : ""));
        if (!(modules & 3)) assert(!h4_transport && !h4_config);
        reset(); config_btctler_modules = modules; rx_busy = 100;
        assert(!btctrler_suspend(0)); assert(occurrences('l') == (modules & 1 ? 6u : 0u));
        reset(); config_btctler_modules = modules;
        assert(!btctrler_task_close_bredr()); assert(!strcmp(trace, modules & 3 ? "unCDE" : ""));
        reset(); config_btctler_modules = modules;
        assert(!btctrler_task_exit());
        assert(!strcmp(trace, !(modules & 3) ? "" : modules & 1 ? "acuhnqpfk" : "acqpfk"));
    }
    for (int busy = 0; busy <= 105; ++busy) {
        reset(); links_busy = connections_busy = busy;
        assert(!btctrler_task_exit());
        unsigned checks = (busy < 100 ? busy : 100) + 1;
        assert(occurrences('u') == checks && occurrences('n') == checks && occurrences('3') == 2 * (checks - 1));
        assert(strchr(trace, 'h') > strrchr(trace, 'u') && strchr(trace, 'h') < strchr(trace, 'n'));
        assert(strchr(trace, 'q') > strrchr(trace, 'n') && !strcmp(strchr(trace, 'q'), "qpfk"));
        reset(); links_busy = connections_busy = busy;
        assert(!btctrler_task_close_bredr());
        assert(occurrences('u') == checks && occurrences('n') == checks && !occurrences('h'));
        assert(!strcmp(trace + at - 3, "CDE"));
    }
    reset(); post_result = 21;
#ifdef CONFIG_CPU_BD19
    assert(!btctrler_task_exit() && !strcmp(trace, "acuhnqpfk") && frees == 1);
#else
    assert(btctrler_task_exit() == 21 && !strcmp(trace, "acuhnqf") && frees == 1);
#endif
    reset(); create_result = -3;
#ifdef CONFIG_CPU_BD19
    assert(!btctrler_task_exit() && !strcmp(trace, "acuhnqpfk") && frees == 1);
#else
    assert(btctrler_task_exit() == -3 && !strcmp(trace, "acf"));
#endif
#ifndef CONFIG_CPU_BD19
    reset(); allocation_fails = 1;
    assert(btctrler_task_exit() == -22 && !strcmp(trace, "a!") && !frees);
#endif
    reset(); btctrler_task_init_bredr(); assert(!strcmp(trace, "I"));
    reset(); log_tag_const_i_LMP = 1; rx_busy = 1;
    btctrler_suspend(0); assert(logs == 2);
    reset(); log_tag_const_e_LMP = 1; config_btctler_modules = 0;
    btctrler_task_init(NULL, NULL); btctrler_task_exit(); btctrler_task_close_bredr();
    assert(logs == 3 && !at);
    puts("btctrler task lifecycle tests passed");
}
#endif
