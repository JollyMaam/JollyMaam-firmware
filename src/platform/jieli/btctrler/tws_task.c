/* The SDK's TWS task (tws_task.c): the API entry points that run a function
 * on the "tws" task and wait for its result, the task loop with its init and
 * exit, and the small accessors for the sibling and local address and the
 * local channel.  This pad never uses TWS, but tws_pair and tws_le_link call
 * the local channel accessors, and the member's references pull the other
 * retained TWS members (tws_event, tws_sync_call, tws_power_balance) into the
 * link, as they were with the SDK member; the rest is translated whole.
 * SDK quirks kept: the request block is {function, argc, args...} and
 * __call_api_and_wait appends a result slot and a semaphore (one malloc of
 * 84 bytes, the first 80 the semaphore) after the arguments, ORs 0x300 into
 * argc and posts argc + 4 words; a full queue (21) is retried every 3 ticks
 * without limit, any other post error is returned as the result;
 * tws_api_detach and tws_task_exit retry 40 times at 5 ticks; detach waits
 * 500 ticks for the answer, exit waits forever and then kills the task;
 * allocation failure is asserted with the SDK's file/line message and carries
 * on if the assertion returns; tws_api_wait_pair_by_ble/by_code drop their
 * timeout; the two wait_pair "_internal" calls get only code and name;
 * tws_api_send_data_to_slave takes the irq-off section twice (the inner
 * enable re-enables early); bt_get_tws_local_addr is weak and fills 0xff;
 * tws_api_get_role_internal is 0 without a link and -22 during a role
 * switch; the tws_key_event_handler only logs. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_TASK_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol/tws_task.c"

extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP;
extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE, CONFIG_TWS_POWER_BALANCE_ENABLE;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern void p33_soft_reset(void);
extern int os_taskq_post_type(const char *name, int type, int argc, int *argv);
extern int os_taskq_pend(const char *name, int *msg, int len);
extern int os_sem_create(OS_SEM *sem, int count);
extern int os_sem_pend(OS_SEM *sem, int timeout);
extern int task_create(void (*func)(void *), void *priv, const char *name);
extern int task_kill(const char *name);

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
    if (log_tag_const_i_TWS_LMP)
        log_print(2, NULL, "[Info]: [TWS_LMP]tws_key_event: %d\n\r\n", event->u.key.value);
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

    ASSERT_SEM(sem, 127);
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
    msg[0] = FN(tws_create_connection_internal);
    msg[1] = 1;
    msg[2] = timeout;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_create_connection(void) TWS_CODE;
int tws_api_cancle_create_connection(void)
{
    int msg[8];
    msg[0] = FN(tws_cancle_create_connection_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_wait_connection(int timeout) TWS_CODE;
int tws_api_wait_connection(int timeout)
{
    int msg[8];
    msg[0] = FN(tws_wait_connection_internal);
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
    return -22;
}

int tws_api_get_role(void) TWS_CODE;
int tws_api_get_role(void)
{
    int msg[8];

    if (cpu_in_irq())
        return tws_api_get_role_internal();
    msg[0] = FN(tws_api_get_role_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_get_tws_state(void) TWS_CODE;
int tws_api_get_tws_state(void)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);
    int state = tws_link.link ? 2 : 1;

    if (!conn)
        return state | 4;
    state |= 8;
    if (conn->esco_conn) {
        state |= 16;
        if (conn->link->esco)
            state |= 256;
    }
    if (conn->sbc_open)
        state |= 32;
    if (tws_conn.monitor_start == 2)
        state |= 64;
    return state;
}

int tws_api_search_sibling_by_code(u16 code, int timeout_ms) TWS_CODE;
int tws_api_search_sibling_by_code(u16 code, int timeout_ms)
{
    int msg[8];
    msg[0] = FN(tws_api_search_sibling_by_code_internal);
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
    msg[0] = FN(tws_api_wait_pair_by_ble_internal);
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
    msg[0] = FN(tws_api_wait_pair_by_code_internal);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = (int)(uintptr_t)name;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_wait_pair(void) TWS_CODE;
int tws_api_cancle_wait_pair(void)
{
    int msg[8];
    msg[0] = FN(tws_api_cancle_wait_pair_internal);
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

int tws_api_sync_call_by_uuid(int uuid, int priv, int delay_ms) TWS_CODE;
int tws_api_sync_call_by_uuid(int uuid, int priv, int delay_ms)
{
    int msg[8];
    msg[0] = FN(tws_api_sync_call_func_internal);
    msg[1] = 3;
    msg[2] = uuid;
    msg[3] = priv;
    msg[4] = delay_ms;
    return __call_api_and_wait(msg);
}

int tws_api_set_sibling_addr(u8 *addr) TWS_CODE;
int tws_api_set_sibling_addr(u8 *addr)
{
    memcpy(gbredr_local_dev.internal_addr_remote, addr, 6);
    return 0;
}

int tws_api_get_sibling_addr(u8 *addr) TWS_CODE;
int tws_api_get_sibling_addr(u8 *addr)
{
    memcpy(addr, gbredr_local_dev.internal_addr_remote, 6);
    return 0;
}

int tws_api_get_local_addr(u8 *addr) TWS_CODE;
int tws_api_get_local_addr(u8 *addr)
{
    memcpy(addr, hci_param.addr, 6);
    return 0;
}

void tws_api_set_local_channel(char channel) TWS_CODE;
void tws_api_set_local_channel(char channel)
{
    tws_local_channel = channel;
    if (log_tag_const_i_TWS_LMP)
        log_print(2, NULL, "[Info]: [TWS_LMP]tws_local_channel: %c\n\r\n", channel);
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
    memcpy(&bulk->data[0], &field, 2);
    memcpy(&bulk->data[4], &func_id, 4);
    memcpy(&bulk->data[8], data, len);
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
    msg[0] = FN(tws_le_connect_in_esco_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_api_cancle_connect_in_esco(void) TWS_CODE;
int tws_api_cancle_connect_in_esco(void)
{
    int msg[8];
    msg[0] = FN(tws_le_cancle_connect_in_esco_internal);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

__attribute__((weak)) void bt_get_tws_local_addr(u8 *addr) TWS_CODE;
__attribute__((weak)) void bt_get_tws_local_addr(u8 *addr)
{
    memset(addr, 0xff, 6);
}

void bt_set_tws_local_addr(u8 *addr) TWS_CODE;
void bt_set_tws_local_addr(u8 *addr)
{
    memcpy(gbredr_local_dev.internal_addr_local, addr, 6);
}

/* The task: bring up the TWS modules, then hand every task message of type
 * OS_TASKQ (13) to the connection event handler. */
static void tws_task(void *p) TWS_CODE;
static void tws_task(void *p)
{
    int msg[8];
    (void)p;

    tws_link_init();
    tws_event_sync_init();
    tws_data_sync_init();
    tws_sync_call_init();
    tws_tx_sync_init();
    if (CONFIG_TWS_POWER_BALANCE_ENABLE)
        tws_power_banance_init();
    for (;;) {
        if (os_taskq_pend(NULL, msg, 8) == 13)
            tws_conn_event_handler(msg);
    }
}

void tws_task_init(void) TWS_CODE;
void tws_task_init(void)
{
    if (log_tag_const_i_TWS_LMP)
        log_print(2, NULL, "[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<<<<<<<tws_task_init>>>>>>>>>>>>>>>>>>>>>>>>\n\r\n");
    bt_get_tws_local_addr(gbredr_local_dev.internal_addr_local);
    if (log_tag_const_i_TWS_LMP)
        printf_buf(gbredr_local_dev.internal_addr_local, 6);
    task_create(tws_task, NULL, "tws");
}

static void tws_task_exit_internal(void *p) TWS_CODE;
static void tws_task_exit_internal(void *p)
{
    tws_api_cancle_wait_pair_internal();
    tws_cancle_create_connection_internal();
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
    ASSERT_SEM(sem, 546);
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
    if (log_tag_const_i_TWS_LMP)
        log_print(2, NULL, "[Info]: [TWS_LMP]tws_task_exit\n\r\n");
}

int tws_open_phone_wait_pair(u16 code, const char *name) TWS_CODE;
int tws_open_phone_wait_pair(u16 code, const char *name)
{
    int msg[8];
    msg[0] = FN(tws_api_open_phone_wait_pair);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = (int)(uintptr_t)name;
    return __call_api_and_wait(msg);
}

int tws_open_tws_wait_pair(u16 code, const char *name) TWS_CODE;
int tws_open_tws_wait_pair(u16 code, const char *name)
{
    int msg[8];
    msg[0] = FN(tws_api_open_tws_wait_pair);
    msg[1] = 2;
    msg[2] = code;
    msg[3] = (int)(uintptr_t)name;
    return __call_api_and_wait(msg);
}

int tws_open_tws_wait_conn(int timeout) TWS_CODE;
int tws_open_tws_wait_conn(int timeout)
{
    int msg[8];
    msg[0] = FN(tws_api_open_tws_wait_conn);
    msg[1] = 1;
    msg[2] = timeout;
    return __call_api_and_wait(msg);
}

int tws_open_tws_conn(int timeout) TWS_CODE;
int tws_open_tws_conn(int timeout)
{
    int msg[8];
    msg[0] = FN(tws_api_open_tws_conn);
    msg[1] = 1;
    msg[2] = timeout;
    return __call_api_and_wait(msg);
}

int tws_close_tws_pair(void) TWS_CODE;
int tws_close_tws_pair(void)
{
    int msg[8];
    msg[0] = FN(tws_api_close_tws_pair);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_close_phone_wait_pair(void) TWS_CODE;
int tws_close_phone_wait_pair(void)
{
    int msg[8];
    msg[0] = FN(tws_api_close_phone_wait_pair);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_remove_tws_pairs(void) TWS_CODE;
int tws_remove_tws_pairs(void)
{
    int msg[8];
    msg[0] = FN(tws_api_remove_tws_pairs);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_disconnect(void) TWS_CODE;
int tws_disconnect(void)
{
    int msg[8];
    msg[0] = FN(tws_api_disconnect);
    msg[1] = 1;
    msg[2] = 0;
    return __call_api_and_wait(msg);
}

int tws_fast_conn_with_pair(u8 role_type) TWS_CODE;
int tws_fast_conn_with_pair(u8 role_type)
{
    int msg[8];
    msg[0] = FN(tws_api_fast_conn_with_pair);
    msg[1] = 1;
    msg[2] = role_type;
    return __call_api_and_wait(msg);
}

#ifdef UNITTEST
#include <setjmp.h>
#include <stdarg.h>
#include "test.h"
#undef printf

/* ---- host stand-ins for the SDK and OS ---- */
BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP;
BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE = 1, CONFIG_TWS_POWER_BALANCE_ENABLE;
int config_asser;
u8 tws_pair_all_way, tws_common_addr_en, tws_auto_role_switch;
char tws_local_channel;
struct tws_connection tws_conn;
struct tws_link_connection tws_link;
struct bredr_local_device gbredr_local_dev;
struct hci_parameter hci_param;

static char trace[256];
static int trace_len;
#define T(c) do { if (trace_len < (int)sizeof(trace) - 1) { trace[trace_len++] = (c); trace[trace_len] = 0; } } while (0)

static int malloc_size, malloc_count, fail_malloc, free_count;
static void *last_alloc, *last_freed;
static int capture_printf, printf_count;
static char printf_text[2][64];
static int printf_line;
static int assert_debug, resets;

void *tws_test_malloc(size_t size)
{
    malloc_size = (int)size; malloc_count++;
    T('m');
    if (fail_malloc) return NULL;
    last_alloc = calloc(1, size);
    return last_alloc;
}
void tws_test_free(void *p) { T('f'); free_count++; last_freed = p; }
int tws_test_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (capture_printf) {
        if (printf_count < 2) {
            if (printf_count == 0) {
                const char *file = va_arg(ap, const char *);
                printf_line = va_arg(ap, int);
                snprintf(printf_text[0], sizeof printf_text[0], "%s", format);
                CHECK(!strcmp(file, TWS_TASK_FILE));
            } else {
                snprintf(printf_text[1], sizeof printf_text[1], "%s", format);
            }
        }
        printf_count++;
    } else {
        vprintf(format, ap);
    }
    va_end(ap);
    return 0;
}
void cpu_assert_debug(void) { assert_debug++; T('A'); }
void p33_soft_reset(void) { resets++; T('R'); }

static int log_count, log_level, log_arg;
static const void *log_tag;
static char log_fmt[100];
void log_print(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_arg = va_arg(ap, int);
    va_end(ap);
    log_level = level; log_tag = tag;
    snprintf(log_fmt, sizeof log_fmt, "%s", format);
    log_count++;
}
static const void *printf_buf_ptr; static int printf_buf_len;
void printf_buf(const void *buffer, int size) { T('B'); printf_buf_ptr = buffer; printf_buf_len = size; }

static int irq, in_irq, irq_disables, irq_enables;
int cpu_in_irq(void) { return in_irq; }
void local_irq_disable(void) { irq++; irq_disables++; T('d'); }
void local_irq_enable(void) { irq--; irq_enables++; T('e'); }

/* the "tws" task queue: record the posted request, answer from a script */
static int post_script[8], post_n, post_i, post_default;
static int post_count, post_type, post_argc;
static char post_name[8];
static int post_argv[8];
static int post_result = 0x1234;
static void *last_sem;
static int sem_created, sem_pended, sem_timeout, sem_posted;
static int dly[64], dly_n;
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    int code = post_i < post_n ? post_script[post_i++] : post_default;
    post_count++; post_type = type; post_argc = argc;
    snprintf(post_name, sizeof post_name, "%s", name);
    memcpy(post_argv, argv, (argc < 8 ? argc : 8) * sizeof(int));
    T('p');
    if (code == 0 && last_sem) *(int *)((u8 *)last_sem + 80) = post_result;
    return code;
}
int os_sem_create(OS_SEM *sem, int count) { last_sem = sem; sem_created++; T('c'); CHECK(count == 0); return 0; }
int os_sem_pend(OS_SEM *sem, int timeout) { sem_pended++; sem_timeout = timeout; T('w'); CHECK(sem == last_sem); return 0; }
int os_sem_post(OS_SEM *sem) { (void)sem; sem_posted++; T('S'); return 0; }
void os_time_dly(int ticks) { if (dly_n < 64) dly[dly_n++] = ticks; T('y'); }

static int task_created, killed;
static void (*created_fn)(void *);
static char created_name[8];
int task_create(void (*func)(void *), void *priv, const char *name)
{
    (void)priv;
    task_created++; created_fn = func; snprintf(created_name, sizeof created_name, "%s", name); T('t'); return 0;
}
int task_kill(const char *name) { killed++; T('k'); CHECK(!strcmp(name, "tws")); return 0; }

/* the loop: deliver scripted messages, then leave through longjmp */
static jmp_buf loop_exit;
static int pend_script[4], pend_n, pend_i, handled;
int os_taskq_pend(const char *name, int *msg, int len)
{
    CHECK(name == NULL && len == 8);
    if (pend_i >= pend_n) longjmp(loop_exit, 1);
    msg[0] = 100 + pend_i;
    return pend_script[pend_i++];
}
void tws_conn_event_handler(int *msg) { handled++; T('E'); CHECK(msg[0] >= 100); }
void tws_link_init(void) { T('1'); }
void tws_event_sync_init(void) { T('2'); }
void tws_data_sync_init(void) { T('3'); }
void tws_sync_call_init(void) { T('4'); }
void tws_tx_sync_init(void) { T('5'); }
void tws_power_banance_init(void) { T('6'); }
void tws_conn_switch_role(void) { T('s'); }

/* functions the tws task would run: only their addresses matter */
int tws_create_connection_internal(int timeout) { return timeout; }
int tws_cancle_create_connection_internal() { return 0; }
int tws_wait_connection_internal(int timeout) { return timeout; }
void tws_conn_detach_internal(int reason) { (void)reason; }
int tws_api_search_sibling_by_code_internal(int code, int timeout_ms) { return code + timeout_ms; }
int tws_api_wait_pair_by_ble_internal(int code, const char *name) { (void)name; return code; }
int tws_api_wait_pair_by_code_internal(int code, const char *name) { (void)name; return code; }
int tws_api_cancle_wait_pair_internal() { T('i'); return 0; }
int tws_api_remove_pairs_internal() { return 0; }
int tws_api_sync_call_func_internal(int uuid, int argc, int *argv) { (void)uuid; (void)argc; (void)argv; return 0; }
int tws_le_connect_in_esco_internal() { return 0; }
int tws_le_cancle_connect_in_esco_internal() { return 0; }
int tws_api_open_phone_wait_pair(int code, const char *name) { (void)name; return code; }
int tws_api_open_tws_wait_pair(int code, const char *name) { (void)name; return code; }
int tws_api_open_tws_wait_conn(int timeout) { return timeout; }
int tws_api_open_tws_conn(int timeout) { return timeout; }
void tws_api_close_tws_pair() { }
void tws_api_close_phone_wait_pair() { }
int tws_api_remove_tws_pairs() { return 0; }
int tws_api_disconnect() { return 0; }
int tws_api_fast_conn_with_pair(int role_type) { return role_type; }

static struct lmp_connection *conn_result;
struct lmp_connection *lmp_conn_for_address(u8 *address) { CHECK(address == NULL); return conn_result; }

static struct { struct bredr_tx_bulk b; u8 space[64]; } bulk_mem;
static int bulk_size, bulk_null;
struct bredr_tx_bulk *tws_tx_bulk_alloc(int size)
{
    bulk_size = size; T('a');
    if (bulk_null) return NULL;
    memset(&bulk_mem, 0xee, sizeof bulk_mem);
    return &bulk_mem.b;
}
static struct link_connection *avail_lc; static struct bredr_tx_bulk *avail_bulk;
void link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx) { avail_lc = lc; avail_bulk = tx; T('v'); }

static void reset(void)
{
    trace_len = 0; trace[0] = 0;
    malloc_count = malloc_size = free_count = 0; fail_malloc = 0; last_freed = NULL;
    capture_printf = printf_count = 0; assert_debug = resets = 0; config_asser = 0;
    post_n = post_i = 0; post_default = 0; post_count = 0; post_result = 0x1234; last_sem = NULL;
    sem_created = sem_pended = sem_posted = 0; dly_n = 0;
    log_count = 0; log_tag_const_i_TWS_LMP = 0; CONFIG_BTCTLER_TWS_ENABLE = 1;
    irq = irq_disables = irq_enables = 0; in_irq = 0;
    task_created = killed = 0; handled = 0; bulk_null = 0; bulk_size = 0;
    memset(&tws_conn, 0, sizeof tws_conn); memset(&tws_link, 0, sizeof tws_link);
}

#define WORDS(a, ...) do { int w_[] = { __VA_ARGS__ }; for (unsigned i_ = 0; i_ < sizeof w_ / sizeof w_[0]; i_++) CHECK((a)[i_] == w_[i_]); } while (0)

int main(void)
{
    /* request blocks: {function, argc | 0x300, args..., result slot, semaphore}, posted as argc + 4
     * words to "tws" with type 0x300000; the answer is what the task left in the result slot. */
    reset();
    CHECK(tws_api_create_connection(77) == 0x1234);
    CHECK(post_count == 1 && !strcmp(post_name, "tws") && post_type == 3145728 && post_argc == 5);
    WORDS(post_argv, FN(tws_create_connection_internal), 1 | 768, 77);
    CHECK(malloc_size == 84 && free_count == 1 && last_freed == last_alloc && sem_created == 1 && sem_pended == 1 && sem_timeout == 0);
    CHECK(!strcmp(trace, "mcpwf"));
    CHECK(post_argv[3] == (int)(uintptr_t)((u8 *)last_alloc + 80) && post_argv[4] == (int)(uintptr_t)last_alloc);

    /* a full queue (21) is retried after 3 ticks, without limit */
    reset(); post_script[0] = post_script[1] = post_script[2] = 21; post_n = 3; post_result = -5;
    CHECK(tws_api_cancle_create_connection() == -5);
    CHECK(post_count == 4 && dly_n == 3 && dly[0] == 3 && dly[1] == 3 && dly[2] == 3 && !strcmp(trace, "mcpypypypwf"));
    WORDS(post_argv, FN(tws_cancle_create_connection_internal), 1 | 768, 0);
    /* any other error is the result; the semaphore is freed, never waited on */
    reset(); post_script[0] = 5; post_n = 1;
    CHECK(tws_api_wait_connection(9) == 5 && !strcmp(trace, "mcpf") && sem_pended == 0);
    WORDS(post_argv, FN(tws_wait_connection_internal), 1 | 768, 9);
    /* allocation failure: reset when asserts are off, message and cpu_assert_debug when on, and carries on */
    reset(); fail_malloc = 1; post_script[0] = 7; post_n = 1;
    CHECK(tws_api_wait_connection(9) == 7 && resets == 1 && assert_debug == 0 && !strcmp(trace, "mRcpf"));
    reset(); fail_malloc = 1; config_asser = 1; capture_printf = 1; post_script[0] = 7; post_n = 1;
    CHECK(tws_api_wait_connection(9) == 7 && resets == 0 && assert_debug == 1 && printf_count == 2 && printf_line == 127);
    CHECK(!strcmp(printf_text[0], "file:%s, line:%d") && !strcmp(printf_text[1], "ASSERT-FAILD: sem "));

    /* the other wrappers */
    reset(); tws_api_search_sibling_by_code(0xbeef, 33);
    CHECK(post_argc == 6); WORDS(post_argv, FN(tws_api_search_sibling_by_code_internal), 2 | 768, 0xbeef, 33);
    {
        const char *nm = "nm";
        /* the timeout is dropped: only code and name are sent */
        reset(); tws_api_wait_pair_by_ble(0x1234, nm, 55);
        WORDS(post_argv, FN(tws_api_wait_pair_by_ble_internal), 2 | 768, 0x1234, (int)(uintptr_t)nm);
        CHECK(post_argc == 6);
        reset(); tws_api_wait_pair_by_code(0x4321, nm, 55);
        WORDS(post_argv, FN(tws_api_wait_pair_by_code_internal), 2 | 768, 0x4321, (int)(uintptr_t)nm);
        reset(); tws_open_phone_wait_pair(0x77, nm);
        WORDS(post_argv, FN(tws_api_open_phone_wait_pair), 2 | 768, 0x77, (int)(uintptr_t)nm);
        reset(); tws_open_tws_wait_pair(0x78, nm);
        WORDS(post_argv, FN(tws_api_open_tws_wait_pair), 2 | 768, 0x78, (int)(uintptr_t)nm);
    }
    reset(); tws_api_cancle_wait_pair(); WORDS(post_argv, FN(tws_api_cancle_wait_pair_internal), 1 | 768, 0);
    reset(); tws_api_remove_pairs(); WORDS(post_argv, FN(tws_api_remove_pairs_internal), 1 | 768, 0);
    reset(); tws_api_sync_call_by_uuid(11, 22, 33);
    CHECK(post_argc == 7); WORDS(post_argv, FN(tws_api_sync_call_func_internal), 3 | 768, 11, 22, 33);
    reset(); tws_api_connect_in_esco(); WORDS(post_argv, FN(tws_le_connect_in_esco_internal), 1 | 768, 0);
    reset(); tws_api_cancle_connect_in_esco(); WORDS(post_argv, FN(tws_le_cancle_connect_in_esco_internal), 1 | 768, 0);
    reset(); tws_open_tws_wait_conn(5); WORDS(post_argv, FN(tws_api_open_tws_wait_conn), 1 | 768, 5);
    reset(); tws_open_tws_conn(6); WORDS(post_argv, FN(tws_api_open_tws_conn), 1 | 768, 6);
    reset(); tws_close_tws_pair(); WORDS(post_argv, FN(tws_api_close_tws_pair), 1 | 768, 0);
    reset(); tws_close_phone_wait_pair(); WORDS(post_argv, FN(tws_api_close_phone_wait_pair), 1 | 768, 0);
    reset(); tws_remove_tws_pairs(); WORDS(post_argv, FN(tws_api_remove_tws_pairs), 1 | 768, 0);
    reset(); tws_disconnect(); WORDS(post_argv, FN(tws_api_disconnect), 1 | 768, 0);
    reset(); tws_fast_conn_with_pair(0xfe); WORDS(post_argv, FN(tws_api_fast_conn_with_pair), 1 | 768, 0xfe);

    /* detach: nothing without TWS; else a 80-byte semaphore, the request {fn, 1, reason}, 3 words, answer within 500 ticks */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 0;
    CHECK(tws_api_detach(3) == 0 && post_count == 0 && malloc_count == 0);
    reset();
    CHECK(tws_api_detach(3) == 0 && malloc_size == 80 && post_argc == 3 && post_type == 3145728);
    WORDS(post_argv, FN(tws_conn_detach_internal), 1, 3);
    CHECK(sem_timeout == 500 && tws_conn.sem_tws_detach == NULL && !strcmp(trace, "mcpwf"));
    reset(); post_script[0] = 21; post_script[1] = 21; post_n = 2;
    CHECK(tws_api_detach(1) == 0 && dly_n == 2 && dly[0] == 5 && dly[1] == 5 && post_count == 3);
    /* forty full-queue answers give up with 21; the semaphore pointer stays published */
    reset(); post_default = 21;
    CHECK(tws_api_detach(1) == 21 && post_count == 40 && dly_n == 40 && dly[0] == 5 && dly[39] == 5);
    CHECK(free_count == 1 && sem_pended == 0 && tws_conn.sem_tws_detach == last_sem);
    reset(); post_script[0] = 9; post_n = 1;
    CHECK(tws_api_detach(1) == 9 && tws_conn.sem_tws_detach == last_sem && last_sem != NULL && free_count == 1 && sem_pended == 0);
    reset(); fail_malloc = 1; post_script[0] = 9; post_n = 1;
    CHECK(tws_api_detach(1) == 9 && resets == 1 && !strcmp(trace, "mRcpf"));
    reset(); fail_malloc = 1; config_asser = 1; capture_printf = 1; post_script[0] = 9; post_n = 1; tws_api_detach(1);
    CHECK(printf_line == 224 && assert_debug == 1);

    /* role: no link 0; role switch in progress -22; else the role byte; in an interrupt it is read directly */
    reset(); tws_conn.role = 1;
    CHECK(tws_api_get_role_internal() == 0);
    tws_link.link = (struct link_connection *)&tws_link;
    CHECK(tws_api_get_role_internal() == 1);
    tws_conn.role_switch = 1;
    CHECK(tws_api_get_role_internal() == -22);
    tws_conn.role_switch = 0; tws_conn.role = 0xff;
    CHECK(tws_api_get_role_internal() == 255);
    reset(); in_irq = 1; post_result = 99;
    CHECK(tws_api_get_role() == 0 && post_count == 0);
    in_irq = 0;
    CHECK(tws_api_get_role() == 99 && post_count == 1);
    WORDS(post_argv, FN(tws_api_get_role_internal), 1 | 768, 0);

    /* state: link 2 / no link 1; no connection |4; connection |8; eSCO |16, with a radio link |256; sbc |32; monitor |64 */
    {
        struct lmp_connection c;
        struct link_connection lc;
        reset(); memset(&c, 0, sizeof c); memset(&lc, 0, sizeof lc);
        conn_result = NULL;
        CHECK(tws_api_get_tws_state() == (1 | 4));
        tws_link.link = &lc;
        CHECK(tws_api_get_tws_state() == (2 | 4));
        conn_result = &c; c.link = &lc;
        CHECK(tws_api_get_tws_state() == (2 | 8));
        c.sbc_open = 1;
        CHECK(tws_api_get_tws_state() == (2 | 8 | 32));
        tws_conn.monitor_start = 2;
        CHECK(tws_api_get_tws_state() == (2 | 8 | 32 | 64));
        tws_conn.monitor_start = 1;
        c.esco_conn = (struct lmp_esco *)&c;
        CHECK(tws_api_get_tws_state() == (2 | 8 | 16 | 32));
        lc.esco = (struct bredr_esco_link *)&lc;
        CHECK(tws_api_get_tws_state() == (2 | 8 | 16 | 256 | 32));
        tws_link.link = NULL; c.sbc_open = 0; tws_conn.monitor_start = 2;
        CHECK(tws_api_get_tws_state() == (1 | 8 | 16 | 256 | 64));
    }

    /* tiny setters and the address copies */
    tws_api_pair_all_way(7); tws_api_common_addr_en(8); CHECK(tws_pair_all_way == 7 && tws_common_addr_en == 8);
    tws_api_auto_role_switch_enable(); CHECK(tws_auto_role_switch == 1);
    tws_api_auto_role_switch_disable(); CHECK(tws_auto_role_switch == 0);
    {
        u8 a[8] = { 1, 2, 3, 4, 5, 6, 0xaa, 0xbb }, b[8];
        memset(&gbredr_local_dev, 0, sizeof gbredr_local_dev); memset(&hci_param, 0, sizeof hci_param);
        CHECK(tws_api_set_sibling_addr(a) == 0 && !memcmp(gbredr_local_dev.internal_addr_remote, a, 6) && gbredr_local_dev.internal_addr_remote[0] == 1);
        memset(b, 0, sizeof b); CHECK(tws_api_get_sibling_addr(b) == 0 && !memcmp(b, a, 6) && b[6] == 0);
        memcpy(hci_param.addr, a, 6); hci_param.addr[0] = 9;
        memset(b, 0, sizeof b); CHECK(tws_api_get_local_addr(b) == 0 && b[0] == 9 && !memcmp(b + 1, a + 1, 5) && b[6] == 0);
        bt_set_tws_local_addr(a); CHECK(!memcmp(gbredr_local_dev.internal_addr_local, a, 6) && 1);
        memset(b, 0, sizeof b); bt_get_tws_local_addr(b); CHECK(b[0] == 0xff && b[5] == 0xff && b[6] == 0);
    }
    /* channel */
    reset(); tws_local_channel = 'L'; CHECK(tws_api_get_local_channel() == 'L');
    tws_local_channel = (char)0xff; CHECK(tws_api_get_local_channel() == -1);
    tws_api_set_local_channel('R'); CHECK(tws_local_channel == 'R' && log_count == 0);
    log_tag_const_i_TWS_LMP = 1; tws_api_set_local_channel((char)0x80);
    CHECK(tws_local_channel == (char)0x80 && log_count == 1 && log_level == 2 && log_tag == NULL && log_arg == -128);
    CHECK(!strcmp(log_fmt, "[Info]: [TWS_LMP]tws_local_channel: %c\n\r\n"));

    /* key event: log the value (an int) only with the tag */
    {
        struct sys_event ev;
        reset(); memset(&ev, 0, sizeof ev); ev.u.key.value = 0xfffffffeu;
        tws_key_event_handler(&ev, NULL); CHECK(log_count == 0);
        log_tag_const_i_TWS_LMP = 1; tws_key_event_handler(&ev, NULL);
        CHECK(log_count == 1 && log_level == 2 && log_tag == NULL && log_arg == -2 && !strcmp(log_fmt, "[Info]: [TWS_LMP]tws_key_event: %d\n\r\n"));
    }
    /* role switch test: only as master (role 0) with the monitor at 2 and sbc open */
    {
        struct lmp_connection c;
        memset(&c, 0, sizeof c); reset(); tws_conn.lmp_conn = &c;
        __role_switch_test(NULL); CHECK(trace_len == 0);
        tws_conn.monitor_start = 2; __role_switch_test(NULL); CHECK(trace_len == 0);
        c.sbc_open = 1; __role_switch_test(NULL); CHECK(!strcmp(trace, "s"));
        tws_conn.role = 1; __role_switch_test(NULL); CHECK(!strcmp(trace, "s"));
        tws_conn.role = 0; tws_conn.monitor_start = 1; __role_switch_test(NULL); CHECK(!strcmp(trace, "s"));
    }

    /* send to the sibling: irq off throughout; -22 without a link, -12 without a buffer */
    {
        struct link_connection lc;
        u8 payload[4] = { 0xa1, 0xa2, 0xa3, 0xa4 };
        reset();
        CHECK(tws_api_send_data_to_sibling(payload, 4, 5) == -22 && !strcmp(trace, "de") && irq == 0);
        reset(); tws_link.link = &lc; bulk_null = 1;
        CHECK(tws_api_send_data_to_sibling(payload, 4, 5) == -12 && !strcmp(trace, "dae") && bulk_size == 12 && irq == 0);
        reset(); tws_link.link = &lc;
        CHECK(tws_api_send_data_to_sibling(payload, 4, 0x01020304) == 0 && !strcmp(trace, "dave") && bulk_size == 12);
        {
            const u8 *bd = (const u8 *)&bulk_mem.b + offsetof(struct bredr_tx_bulk, data);
            CHECK(avail_lc == &lc && avail_bulk == &bulk_mem.b && bulk_mem.b.len == 12 && bulk_mem.b.event_post == 63);
            CHECK(bd[0] == 8 && bd[1] == 0 && bd[2] == 0 && bd[3] == 18);
            CHECK(bd[4] == 4 && bd[5] == 3 && bd[6] == 2 && bd[7] == 1);
            CHECK(!memcmp(bd + 8, payload, 4) && bd[12] == 0xee);
        }
        /* the length is 16 bits: a 0xffff payload wraps the sum */
        reset(); tws_link.link = &lc; bulk_null = 1;
        tws_api_send_data_to_sibling(payload, 0xfffc, 5); CHECK(bulk_size == 0x10004);
        /* to the slave: only as master, with the whole call under the irq lock (the inner enable ends it early) */
        reset(); tws_link.link = &lc; tws_conn.role = 1;
        CHECK(tws_api_send_data_to_slave(payload, 4, 5) == -22 && !strcmp(trace, "de") && irq == 0);
        reset(); tws_link.link = &lc; tws_conn.role = 0;
        CHECK(tws_api_send_data_to_slave(payload, 0x10004, 5) == 0 && !strcmp(trace, "ddavee") && irq == 0);
        CHECK(bulk_size == 12);
    }

    /* task init: log, address, hex dump of it, then the task named "tws" */
    reset(); memset(&gbredr_local_dev, 0, sizeof gbredr_local_dev);
    tws_task_init();
    CHECK(log_count == 0 && task_created == 1 && !strcmp(created_name, "tws") && !strcmp(trace, "t"));
    CHECK(gbredr_local_dev.internal_addr_local[0] == 0xff && gbredr_local_dev.internal_addr_local[5] == 0xff);
    reset(); log_tag_const_i_TWS_LMP = 1; tws_task_init();
    CHECK(log_count == 1 && log_level == 2 && log_tag == NULL && !strcmp(trace, "Bt") && printf_buf_len == 6 && printf_buf_ptr == gbredr_local_dev.internal_addr_local);
    CHECK(!strcmp(log_fmt, "[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<<<<<<<tws_task_init>>>>>>>>>>>>>>>>>>>>>>>>\n\r\n"));
    /* the task: module inits in order, power balance by config, only message type 13 goes to the handler */
    reset(); pend_n = 3; pend_i = 0; pend_script[0] = 13; pend_script[1] = 11; pend_script[2] = 13;
    if (!setjmp(loop_exit)) created_fn(NULL);
    CHECK(!strcmp(trace, "12345EE") && handled == 2);
    reset(); pend_n = 0; pend_i = 0; CONFIG_TWS_POWER_BALANCE_ENABLE = 1;
    if (!setjmp(loop_exit)) created_fn(NULL);
    CHECK(!strcmp(trace, "123456"));
    CONFIG_TWS_POWER_BALANCE_ENABLE = 0;

    /* exit request: cancel waits, post the semaphore, sleep 10000 */
    reset(); tws_task_exit_internal((void *)0x10);
    CHECK(!strcmp(trace, "iSy") && sem_posted == 1 && dly[0] == 10000);
    /* exit: semaphore 80 bytes, request {fn, 1, sem} as 3 words, wait forever, kill the task, free, log */
    reset(); log_tag_const_i_TWS_LMP = 1;
    tws_task_exit();
    CHECK(malloc_size == 80 && post_argc == 3 && post_type == 3145728 && post_argv[0] == FN(tws_task_exit_internal) && post_argv[1] == 1);
    CHECK(post_argv[2] == (int)(uintptr_t)last_alloc && sem_timeout == 0);
    CHECK(!strcmp(trace, "mcpwkf") && killed == 1 && log_count == 1 && !strcmp(log_fmt, "[Info]: [TWS_LMP]tws_task_exit\n\r\n"));
    reset(); post_script[0] = 21; post_n = 1; tws_task_exit();
    CHECK(dly_n == 1 && dly[0] == 5 && killed == 1 && log_count == 0);
    /* forty full-queue answers: give up, free and still log */
    reset(); post_default = 21; log_tag_const_i_TWS_LMP = 1; tws_task_exit();
    CHECK(post_count == 40 && dly_n == 40 && killed == 0 && free_count == 1 && log_count == 1);
    reset(); post_script[0] = 4; post_n = 1; log_tag_const_i_TWS_LMP = 1; tws_task_exit();
    CHECK(!strcmp(trace, "mcpf") && killed == 0 && log_count == 0);
    reset(); fail_malloc = 1; post_script[0] = 4; post_n = 1; tws_task_exit();
    CHECK(resets == 1);
    reset(); fail_malloc = 1; config_asser = 1; capture_printf = 1; post_script[0] = 4; post_n = 1; tws_task_exit();
    CHECK(printf_line == 546 && assert_debug == 1);

    return TEST_DONE("btctrler_tws_task");
}
#endif
