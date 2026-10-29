/* Recovered from BD19 tws_task.c.o occurrence 1. It owns the public TWS task
 * APIs, task lifecycle, queue dispatch and local address/channel accessors.
 * The UUID wrapper remains in bd19/tws_sync_call.c, where the moved BD19
 * implementation provides it.
 * SDK quirks kept: the request block is {function, argc, args...} and
 * __call_api_and_wait appends a result slot and a semaphore (one malloc of
 * 84 bytes, the first 80 the semaphore) after the arguments, ORs 0x300 into
 * argc and posts argc + 4 words; a full queue (21) is retried every 3 ticks
 * without limit, any other post error is returned as the result;
 * tws_api_detach and tws_task_exit retry 40 times at 5 ticks; detach waits
 * 500 ticks for the answer, exit waits forever and then kills the task;
 * allocation failure is asserted with the SDK's file/line message and carries
 * on if the assertion returns; tws_api_wait_pair_by_ble/by_code drop their
 * timeout and both marshal the same code/name pair;
 * tws_api_send_data_to_slave takes the irq-off section twice (the inner
 * enable re-enables early); bt_get_tws_local_addr is weak and fills 0xff;
 * tws_api_get_role_internal returns the link role bit during a role switch;
 * tws_key_event_handler prints the key value. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_TASK_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_task.c"

extern void p33_soft_reset(void);
extern int printf(const char *format, ...);
extern int puts(const char *s);
extern void put_buf(const void *buffer, int size);
extern int os_taskq_post_type(const char *name, int type, int argc, int *argv);
extern int os_taskq_pend(const char *name, int *msg, int len);
extern int os_sem_create(OS_SEM *sem, int count);
extern int os_sem_pend(OS_SEM *sem, int timeout);
extern int task_create(void (*func)(void *), void *priv, const char *name);
extern int task_kill(const char *name);
extern int tws_pair_create_connection(int timeout);
extern int tws_pair_cancle_create_connection(void);
extern int tws_pair_wait_connection(int timeout);
extern int tws_pair_wait_connect_in_esco(void);
extern int tws_pair_cancle_wait_connect_in_esco(void);
extern void tws_lmp_core_sync_enable(void);
extern BTCTRLER_CONFIG int CONFIG_LOW_LATENCY_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;

#ifdef UNITTEST
void *tws_test_malloc(size_t size);
void tws_test_free(void *p);
int tws_test_printf(const char *format, ...);
#define malloc tws_test_malloc
#define free tws_test_free
#define printf tws_test_printf
#endif

static void cpu_reset(void)
{
    p33_soft_reset();
}

/* The SDK's ASSERT(p): with config_asser print file and line and call
 * cpu_assert_debug (execution then continues), otherwise reset. */
#define ASSERT_SEM(p, line) do { \
        if (config_asser) { \
            if (!(p)) { \
                printf("file:%s, line:%d", TWS_TASK_FILE, line); \
                printf("ASSERT-FAILD: sem "); \
                cpu_assert_debug(); \
            } \
        } else if (!(p)) { \
            cpu_reset(); \
        } \
    } while (0)

/* The request words hold addresses of the functions the task runs. */
#define FN(f) ((int)(uintptr_t)(f))

void tws_key_event_handler(struct sys_event *event, void *priv) TWS_CODE;
void tws_key_event_handler(struct sys_event *event, void *priv)
{
    (void)priv;
    printf("tws_key_event: %d\n", event->u.key.value);
}

void __role_switch_test(void *p) TWS_CODE;
void __role_switch_test(void *p)
{
    (void)p;
    if ((tws_conn.monitor_start == 2) & (tws_conn.role == 0)) {
        if (tws_conn.lmp_conn->sbc_open)
            tws_conn_switch_role();
    }
}

void tws_api_pair_all_way(u8 en) TWS_CODE;
void tws_api_pair_all_way(u8 en)
{
    tws_pair_all_way = en;
}

void tws_api_common_addr_en(u8 en) TWS_CODE;
void tws_api_common_addr_en(u8 en)
{
    tws_common_addr_en = en;
}

void tws_api_auto_role_switch_disable(void) TWS_CODE;
void tws_api_auto_role_switch_disable(void)
{
    tws_auto_role_switch = 0;
}

void tws_api_auto_role_switch_enable(void) TWS_CODE;
void tws_api_auto_role_switch_enable(void)
{
    tws_auto_role_switch = 1;
}

/* Run the request {function, argc, args...} on the "tws" task and return what
 * it leaves in the result slot (or the post error). */
static int __call_api_and_wait(int *argv) TWS_CODE;
static int __call_api_and_wait(int *argv)
{
    u8 *sem = malloc(84);
    int *result = (int *)(sem + 80);
    int argc, err;

    ASSERT_SEM(sem, 128);
    argc = argv[1];
    argv[1] = argc | 768;
    argv[argc + 2] = (int)(uintptr_t)result;
    argv[argc + 3] = (int)(uintptr_t)sem;
    os_sem_create((OS_SEM *)sem, 0);
    for (;;) {
        err = os_taskq_post_type("tws", 3145728, argc + 4, argv);
        if (err == 0) {
            int value;
            os_sem_pend((OS_SEM *)sem, 0);
            value = *result;
            free(sem);
            return value;
        }
        if (err != 21) {
            free(sem);
            return err;
        }
        os_time_dly(3);
    }
}

int tws_api_create_connection(int timeout) TWS_CODE;
int tws_api_create_connection(int timeout)
{
    int msg[8];
    msg[0] = FN(tws_pair_create_connection);
    msg[1] = 1;
    msg[2] = timeout;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_create_connection(void) TWS_CODE;
int tws_api_cancle_create_connection(void)
{
    int msg[8];
    msg[0] = FN(tws_pair_cancle_create_connection);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_wait_connection(int timeout) TWS_CODE;
int tws_api_wait_connection(int timeout)
{
    int msg[8];
    msg[0] = FN(tws_pair_wait_connection);
    msg[1] = 1;
    msg[2] = timeout;
    return __call_api_and_wait(msg);
}

int tws_api_detach(enum tws_detach_reason reason) TWS_CODE;
int tws_api_detach(enum tws_detach_reason reason)
{
    int msg[4];
    void *sem;
    int err, cnt;

    if (!CONFIG_BTCTLER_TWS_ENABLE)
        return 0;
    sem = malloc(80);
    ASSERT_SEM(sem, 224);
    msg[0] = FN(tws_conn_detach_internal);
    msg[1] = 1;
    msg[2] = reason;
    os_sem_create(sem, 0);
    tws_conn.sem_tws_detach = sem;
    cnt = 0;
    do {
        err = os_taskq_post_type("tws", 3145728, 3, msg);
        if (err == 0) {
            os_sem_pend(sem, 500);
            tws_conn.sem_tws_detach = 0;
            free(sem);
            return 0;
        }
        if (err != 21) {
            free(sem);
            return err;
        }
        os_time_dly(5);
        cnt++;
    } while (cnt < 40);
    free(sem);
    return 21;
}

int tws_api_get_role_internal(void) TWS_CODE;
int tws_api_get_role_internal(void)
{
    if (!tws_link.link)
        return 0;
    if (tws_conn.role_switch == 0)
        return tws_conn.role;
    return tws_link.link->role;
}

int tws_api_get_role(void) TWS_CODE;
int tws_api_get_role(void)
{
    int msg[8];

    if (!tws_link.link)
        return 0;
    if (!tws_auto_role_switch)
        return tws_conn.role;
    if (cpu_in_irq())
        return tws_conn.role_switch ? -22 : tws_conn.role;
    msg[0] = FN(tws_api_get_role_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_get_tws_state(void) TWS_CODE;
int tws_api_get_tws_state(void)
{
    struct lmp_connection *conn;
    int state;

    local_irq_disable();
    state = tws_link.link ? 2 : 1;
    conn = lmp_conn_for_address(NULL);
    if (!conn || !conn->link || (conn->core.state & 8)) {
        state |= 4;
        local_irq_enable();
        return state;
    }
    state |= 8;
    if (conn->esco_conn)
        state |= 16;
    if (conn->sbc_open)
        state |= 32;
    if (tws_conn.monitor_start == 2)
        state |= 64;
    if (tws_conn.monitor_start == 1)
        state |= 512;
    local_irq_enable();
    return state;
}

int tws_api_search_sibling_by_code(u16 code, int timeout_ms) TWS_CODE;
int tws_api_search_sibling_by_code(u16 code, int timeout_ms)
{
    int msg[8];
    msg[0] = FN(tws_pair_search_sibling_by_code);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = timeout_ms;
    return __call_api_and_wait(msg);
}

int tws_api_wait_pair_by_ble(u16 code, const char *name, int timeout_ms) TWS_CODE;
int tws_api_wait_pair_by_ble(u16 code, const char *name, int timeout_ms)
{
    int msg[8];
    (void)timeout_ms;
    msg[0] = FN(tws_pair_wait_pair_by_code);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = (int)(uintptr_t)name;
    return __call_api_and_wait(msg);
}

int tws_api_wait_pair_by_code(u16 code, const char *name, int timeout_ms) TWS_CODE;
int tws_api_wait_pair_by_code(u16 code, const char *name, int timeout_ms)
{
    int msg[8];
    (void)timeout_ms;
    msg[0] = FN(tws_pair_wait_pair_by_code);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = (int)(uintptr_t)name;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_wait_pair(void) TWS_CODE;
int tws_api_cancle_wait_pair(void)
{
    int msg[8];
    msg[0] = FN(tws_pair_cancle_wait_pair);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_remove_pairs(void) TWS_CODE;
int tws_api_remove_pairs(void)
{
    int msg[8];
    msg[0] = FN(tws_api_remove_pairs_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}


int tws_api_set_sibling_addr(u8 *addr) TWS_CODE;
int tws_api_set_sibling_addr(u8 *addr)
{
    __builtin_memcpy(gbredr_local_dev.internal_addr_remote, addr, 6);
    return 0;
}

int tws_api_get_sibling_addr(u8 *addr) TWS_CODE;
int tws_api_get_sibling_addr(u8 *addr)
{
    __builtin_memcpy(addr, gbredr_local_dev.internal_addr_remote, 6);
    return 0;
}

int tws_api_get_local_addr(u8 *addr) TWS_CODE;
int tws_api_get_local_addr(u8 *addr)
{
    __builtin_memcpy(addr, hci_param.addr, 6);
    return 0;
}

void tws_api_set_local_channel(char channel) TWS_CODE;
void tws_api_set_local_channel(char channel)
{
    tws_local_channel = channel;
}

char tws_api_get_local_channel(void) TWS_CODE;
char tws_api_get_local_channel(void)
{
    return tws_local_channel;
}

int tws_api_send_data_to_sibling(void *data, u16 len, u32 func_id) TWS_CODE;
int tws_api_send_data_to_sibling(void *data, u16 len, u32 func_id)
{
    struct bredr_tx_bulk *bulk;
    u16 total, field;
    int size;

    local_irq_disable();
    if (!tws_link.link) {
        local_irq_enable();
        return -22;
    }
    size = len + 8;
    bulk = tws_tx_bulk_alloc(size);
    if (!bulk) {
        local_irq_enable();
        return -12;
    }
    total = (u16)size;
    bulk->len = total;
    bulk->event_post = 63;
    bulk->data[2] = 0;
    bulk->data[3] = 18;
    field = (u16)(total - 4);
    __builtin_memcpy(&bulk->data[0], &field, 2);
    __builtin_memcpy(&bulk->data[4], &func_id, 4);
    __builtin_memcpy(&bulk->data[8], &data[0], len);
    link_conn_tx_bulk_avaiable(tws_link.link, bulk);
    local_irq_enable();
    return 0;
}

int tws_api_send_data_to_slave(void *data, int len, u32 func_id) TWS_CODE;
int tws_api_send_data_to_slave(void *data, int len, u32 func_id)
{
    int ret = -22;

    local_irq_disable();
    if (tws_conn.role == 0)
        ret = tws_api_send_data_to_sibling(data, (u16)len, func_id);
    local_irq_enable();
    return ret;
}

int tws_api_connect_in_esco(void) TWS_CODE;
int tws_api_connect_in_esco(void)
{
    int msg[8];
    msg[0] = FN(tws_pair_wait_connect_in_esco);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_connect_in_esco(void) TWS_CODE;
int tws_api_cancle_connect_in_esco(void)
{
    int msg[8];
    msg[0] = FN(tws_pair_cancle_wait_connect_in_esco);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int bd19_tws_api_low_latency_enable(u8 enable)
    __asm__("tws_api_low_latency_enable") TWS_CODE;
int bd19_tws_api_low_latency_enable(u8 enable)
{
    if (!CONFIG_LOW_LATENCY_ENABLE)
        return -22;
    if (enable)
        return tws_core_enter_low_latency_mode();
    return tws_core_exit_low_latency_mode();
}

int tws_api_get_low_latency_state(void) TWS_CODE;
int tws_api_get_low_latency_state(void)
{
    if (!CONFIG_LOW_LATENCY_ENABLE)
        return 0;
    return tws_core_low_latency_enable();
}

static u8 tws_quick_conn_addr[6] = {0x35, 0x27, 0x2a, 0xae, 0x85, 0x9c};

void tws_api_set_quick_connect_addr(u8 *addr) TWS_CODE;
void tws_api_set_quick_connect_addr(u8 *addr)
{
    __builtin_memcpy(tws_quick_conn_addr, addr, 6);
}

u8 *tws_api_get_quick_connect_addr(void) TWS_CODE;
u8 *tws_api_get_quick_connect_addr(void)
{
    return tws_quick_conn_addr;
}

int tws_api_power_saving_mode_enable(void) TWS_CODE;
int tws_api_power_saving_mode_enable(void)
{
    return link_task_idle_enable();
}

int tws_api_power_saving_mode_disable(void) TWS_CODE;
int tws_api_power_saving_mode_disable(void)
{
    return link_task_idle_disable();
}

__attribute__((weak)) void bt_get_tws_local_addr(u8 *addr) TWS_CODE;
__attribute__((weak)) void bt_get_tws_local_addr(u8 *addr)
{
    __builtin_memset(addr, 0xff, 6);
}

/* The task: bring up the TWS modules, then hand every task message of type
 * OS_TASKQ (13) to the connection event handler. */
static void tws_task(void *p) TWS_CODE;
static void tws_task(void *p)
{
    int msg[8];
    (void)p;

    __builtin_memset(&tws_conn.role, 0, 196);
    __builtin_memset(&tws_link.wait_tx_suss, 0, 80);
    tws_pair_init();
    tws_link_init();
    tws_event_sync_init();
    tws_data_sync_init();
    tws_power_banance_init();
    tws_sync_call_init();
    tws_lmp_core_sync_enable();
    for (;;) {
        if (os_taskq_pend(NULL, msg, 8) == 13)
            tws_conn_event_handler(msg);
    }
}

void tws_task_init(void) TWS_CODE;
void tws_task_init(void)
{
    puts("<<<<<<<<<<<<<<<<<<<<<<tws_task_init>>>>>>>>>>>>>>>>>>>>>>>>\n");
    bt_get_tws_local_addr(gbredr_local_dev.internal_addr_local);
    put_buf(gbredr_local_dev.internal_addr_local, 6);
    task_create(tws_task, NULL, "tws");
}

static void tws_task_exit_internal(void *p) TWS_CODE;
static void tws_task_exit_internal(void *p)
{
    tws_pair_cancle_wait_pair();
    tws_pair_cancle_create_connection();
    os_sem_post((OS_SEM *)p);
    os_time_dly(10000);
}

void tws_task_exit(void) TWS_CODE;
void tws_task_exit(void)
{
    int msg[3];
    void *sem;
    int err, cnt;

    sem = malloc(80);
    ASSERT_SEM(sem, 600);
    msg[0] = FN(tws_task_exit_internal);
    msg[1] = 1;
    msg[2] = (int)(uintptr_t)sem;
    os_sem_create(sem, 0);
    cnt = 0;
    do {
        err = os_taskq_post_type("tws", 3145728, 3, msg);
        if (err == 0) {
            os_sem_pend(sem, 0);
            task_kill("tws");
            break;
        }
        if (err != 21) {
            free(sem);
            return;
        }
        os_time_dly(5);
        cnt++;
    } while (cnt < 40);
    free(sem);
    puts("tws_task_exit\n");
}

