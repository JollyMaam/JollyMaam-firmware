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
typedef u16 task_aligned_u16 __attribute__((aligned(4)));
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
    conn = lmp_conn_for_address(NULL);
    state = tws_link.link ? 2 : 1;
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
    *(task_aligned_u16 *)(void *)bulk->data = field;
    __builtin_memcpy(&bulk->data[4], &func_id, 4);
    __builtin_memcpy(&bulk->data[8], data, len);
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

    __builtin_memset(&tws_conn, 0, sizeof(tws_conn));
    __builtin_memset(&tws_link, 0, sizeof(tws_link));
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


#ifdef UNITTEST
#include <setjmp.h>
#include <stdarg.h>
#include "test.h"
#undef malloc
#undef free
#undef printf
int config_asser, CONFIG_BTCTLER_TWS_ENABLE, CONFIG_LOW_LATENCY_ENABLE;
u8 tws_pair_all_way, tws_common_addr_en, tws_auto_role_switch;
char tws_local_channel;
struct tws_connection tws_conn;
struct tws_link_connection tws_link;
struct bredr_local_device gbredr_local_dev;
struct hci_parameter hci_param;
static struct lmp_connection phone;
static struct link_connection link;
static struct bredr_esco_link esco;
static void *allocation, *last_freed;
static size_t allocation_size;
static int mallocs, frees, post_err, full_retries, post_calls, post_words, post_args[8];
static int sem_creates, sem_pends, pend_timeout, sem_posts, delays, delay_ticks, killed;
static int result_value, irq, max_irq, in_irq, alloc_fail, tx_size, tx_calls, mutate_peer;
static int mode_enters, mode_exits, switch_calls, assertion_calls, reset_calls, printf_arg;
static struct lmp_connection *address_result;
static u8 tx_storage[70000] __attribute__((aligned(16)));
static struct bredr_tx_bulk *queued_tx;
static char init_trace[32];
static int init_count, queue_pends, event_calls, task_created, buf_calls;
static void (*created_task)(void *);
static jmp_buf task_escape;
static void record(char c) { init_trace[init_count++]=c; init_trace[init_count]=0; }
void *tws_test_malloc(size_t n)
{ CHECK(!allocation); ++mallocs; allocation_size=n; allocation=malloc(n); CHECK(allocation); memset(allocation,0xa5,n); return allocation; }
void tws_test_free(void *p) { CHECK(p==allocation); ++frees; last_freed=p; free(p); allocation=NULL; }
int tws_test_printf(const char *fmt,...)
{ if(!strcmp(fmt,"tws_key_event: %d\n")) { va_list ap; va_start(ap,fmt); printf_arg=va_arg(ap,int); va_end(ap); } return 0; }
void cpu_assert_debug(void) { ++assertion_calls; }
void p33_soft_reset(void) { ++reset_calls; }
int cpu_in_irq(void) { return in_irq; }
void local_irq_disable(void) { ++irq; if(irq>max_irq) max_irq=irq; }
void local_irq_enable(void) { CHECK(irq>0); --irq; }
int os_sem_create(OS_SEM *s,int n) { CHECK((void *)s==allocation && !n); ++sem_creates; return -5; }
int os_sem_pend(OS_SEM *s,int n) { CHECK((void *)s==allocation); ++sem_pends; pend_timeout=n; return -6; }
int os_sem_post(OS_SEM *s) { CHECK((void *)s==&phone); ++sem_posts; return -7; }
int os_taskq_post_type(const char *name,int type,int n,int *v)
{
    CHECK(!strcmp(name,"tws") && type==3145728 && n<=8); ++post_calls; post_words=n;
    memcpy(post_args,v,n*sizeof(int));
    if(full_retries) { --full_retries; return 21; }
    if(!post_err && allocation_size>=84 && (v[1]&0x300)) *(int *)((u8 *)allocation+80)=result_value;
    return post_err;
}
void os_time_dly(int n) { ++delays; delay_ticks=n; }
int task_create(void (*fn)(void *),void *p,const char *name)
{ CHECK(!p && !strcmp(name,"tws")); created_task=fn; ++task_created; return -8; }
int task_kill(const char *name) { CHECK(!strcmp(name,"tws")); ++killed; return -9; }
int os_taskq_pend(const char *name,int *msg,int n)
{ CHECK(!name && n==8); if(queue_pends++==2) longjmp(task_escape,1); msg[0]=123; return queue_pends==1 ? 0 : 13; }
void tws_conn_event_handler(int *msg) { CHECK(msg[0]==123); ++event_calls; }
void tws_pair_init(void) { record('p'); }
void tws_link_init(void) { record('l'); }
void tws_event_sync_init(void) { record('e'); }
void tws_data_sync_init(void) { record('d'); }
void tws_power_banance_init(void) { record('b'); }
void tws_sync_call_init(void) { record('s'); }
void tws_lmp_core_sync_enable(void) { record('c'); }
void put_buf(const void *buf,int n) { CHECK(buf==gbredr_local_dev.internal_addr_local && n==6); ++buf_calls; }
void tws_conn_switch_role(void) { ++switch_calls; }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(!addr && irq>0); if(mutate_peer) tws_link.link=NULL; return address_result; }
struct bredr_tx_bulk *tws_tx_bulk_alloc(int n)
{ CHECK(irq>0); tx_size=n; if(alloc_fail) return NULL; if(mutate_peer) tws_link.link=&link; memset(tx_storage,0xa5,sizeof(tx_storage)); return (void *)tx_storage; }
void link_conn_tx_bulk_avaiable(struct link_connection *l,struct bredr_tx_bulk *tx)
{ CHECK(l==&link && irq>0); ++tx_calls; queued_tx=tx; }
int tws_pair_create_connection(int n) { return n; }
int tws_pair_cancle_create_connection(void) { record('C'); return 0; }
int tws_pair_wait_connection(int n) { return n; }
int tws_pair_search_sibling_by_code(int code,int n) { return code+n; }
int tws_pair_wait_pair_by_code(int code,const char *name) { (void)name; return code; }
int tws_pair_cancle_wait_pair(void) { record('W'); return 0; }
int tws_api_remove_pairs_internal(void) { return 0; }
int tws_pair_wait_connect_in_esco(void) { return 0; }
int tws_pair_cancle_wait_connect_in_esco(void) { return 0; }
void tws_conn_detach_internal(int reason) { (void)reason; }
int tws_core_enter_low_latency_mode(void) { ++mode_enters; return 42; }
int tws_core_exit_low_latency_mode(void) { ++mode_exits; return -42; }
int tws_core_low_latency_enable(void) { return 7; }
u32 link_task_idle_enable(void) { return 0x87654321u; }
u32 link_task_idle_disable(void) { return 0x12345678u; }
static void fixture(void)
{
    CHECK(!allocation); memset(&phone,0,sizeof(phone)); memset(&link,0,sizeof(link));
    memset(&tws_conn,0,sizeof(tws_conn)); memset(&tws_link,0,sizeof(tws_link));
    memset(&gbredr_local_dev,0,sizeof(gbredr_local_dev)); memset(&hci_param,0,sizeof(hci_param));
    mallocs=frees=post_err=full_retries=post_calls=post_words=0;
    sem_creates=sem_pends=pend_timeout=sem_posts=delays=delay_ticks=killed=0;
    result_value=0x12345678; irq=max_irq=in_irq=alloc_fail=tx_size=tx_calls=mutate_peer=0;
    mode_enters=mode_exits=switch_calls=assertion_calls=reset_calls=printf_arg=0;
    init_count=queue_pends=event_calls=task_created=buf_calls=0; init_trace[0]=0; created_task=NULL;
    allocation_size=0; last_freed=NULL; queued_tx=NULL; address_result=NULL;
    CONFIG_BTCTLER_TWS_ENABLE=1; CONFIG_LOW_LATENCY_ENABLE=0; config_asser=0;
    tws_auto_role_switch=0;
}
int main(void)
{
    fixture(); full_retries=2;
    CHECK(tws_api_create_connection(321)==result_value);
    CHECK(allocation_size==84 && mallocs==1 && frees==1 && sem_creates==1 && sem_pends==1 && pend_timeout==0);
    CHECK(post_calls==3 && delays==2 && delay_ticks==3 && post_words==5 && post_args[0]==FN(tws_pair_create_connection) && post_args[1]==0x301 && post_args[2]==321);
    CHECK((u32)post_args[3]==(u32)(uintptr_t)((u8 *)last_freed+80) && (u32)post_args[4]==(u32)(uintptr_t)last_freed);
    fixture(); post_err=18; CHECK(tws_api_wait_connection(99)==18 && frees==1 && sem_pends==0);
    fixture(); char name[]="peer";
    CHECK(tws_api_wait_pair_by_ble(0xffff,name,100)==result_value && post_words==6 && post_args[0]==FN(tws_pair_wait_pair_by_code) && post_args[2]==65535 && post_args[3]==FN(name));
    CHECK(tws_api_wait_pair_by_code(1,name,200)==result_value && post_args[0]==FN(tws_pair_wait_pair_by_code));
    CHECK(tws_api_search_sibling_by_code(0x8000,-30)==result_value && post_args[0]==FN(tws_pair_search_sibling_by_code) && post_args[2]==32768 && post_args[3]==-30);
    CHECK(tws_api_cancle_create_connection()==result_value && post_args[0]==FN(tws_pair_cancle_create_connection));
    CHECK(tws_api_cancle_wait_pair()==result_value && post_args[0]==FN(tws_pair_cancle_wait_pair));
    CHECK(tws_api_connect_in_esco()==result_value && post_args[0]==FN(tws_pair_wait_connect_in_esco));
    CHECK(tws_api_cancle_connect_in_esco()==result_value && post_args[0]==FN(tws_pair_cancle_wait_connect_in_esco));
    CHECK(tws_api_remove_pairs()==result_value && post_args[0]==FN(tws_api_remove_pairs_internal));
    /* Failure paths deliberately leave the semaphore pointer dangling. */
    fixture(); post_err=9; CHECK(tws_api_detach(0)==9 && tws_conn.sem_tws_detach==last_freed && frees==1 && sem_pends==0);
    fixture(); post_err=21; CHECK(tws_api_detach(0)==21 && post_calls==40 && delays==40 && delay_ticks==5 && tws_conn.sem_tws_detach==last_freed);
    fixture(); full_retries=2; CHECK(tws_api_detach(0)==0 && sem_pends==1 && pend_timeout==500 && !tws_conn.sem_tws_detach && post_calls==3);
    fixture(); CONFIG_BTCTLER_TWS_ENABLE=0; CHECK(tws_api_detach(0)==0 && !mallocs);
    fixture(); post_err=18; tws_task_exit(); CHECK(frees==1 && !killed && !sem_pends);
    fixture(); post_err=21; tws_task_exit(); CHECK(post_calls==40 && delays==40 && !killed && frees==1);
    fixture(); tws_task_exit(); CHECK(killed==1 && sem_pends==1 && pend_timeout==0 && frees==1);
    /* IRQ-role policy differs from the internal role-switch query. */
    fixture(); CHECK(tws_api_get_role_internal()==0 && tws_api_get_role()==0 && !mallocs);
    tws_link.link=&link; tws_conn.role=1; CHECK(tws_api_get_role()==1 && !mallocs);
    tws_conn.role_switch=1; link.role=0; CHECK(tws_api_get_role_internal()==0);
    link.role=1; tws_conn.role=0; CHECK(tws_api_get_role_internal()==1);
    tws_auto_role_switch=1; in_irq=1; CHECK(tws_api_get_role()==-22 && !mallocs);
    tws_conn.role_switch=0; CHECK(tws_api_get_role()==0);
    in_irq=0; CHECK(tws_api_get_role()==result_value && post_args[0]==FN(tws_api_get_role_internal));
    /* Phone core state, rather than link state, gates reported connection state. */
    fixture(); tws_link.link=&link; address_result=&phone; phone.link=&link; link.state=8;
    CHECK(tws_api_get_tws_state()==10);
    phone.core.state=8; CHECK(tws_api_get_tws_state()==6);
    phone.core.state=0; phone.esco_conn=(void *)&esco; phone.sbc_open=1; tws_conn.monitor_start=2;
    CHECK(tws_api_get_tws_state()==122);
    tws_conn.monitor_start=1; CHECK(tws_api_get_tws_state()==570);
    mutate_peer=1; address_result=NULL; CHECK(tws_api_get_tws_state()==5 && !irq);
    /* Data framing and nested IRQ, including 16-bit source length truncation. */
    fixture(); u8 data[4]={1,2,3,4}; CHECK(tws_api_send_data_to_sibling(data,4,0x12345678)==-22);
    tws_link.link=&link; alloc_fail=1; CHECK(tws_api_send_data_to_sibling(data,4,0)==-12 && !irq);
    alloc_fail=0; CHECK(tws_api_send_data_to_slave(data,65540,0x12345678)==0 && max_irq==2 && !irq && tx_size==12 && tx_calls==1);
    CHECK(queued_tx->len==12 && queued_tx->event_post==63 && !memcmp(queued_tx->data,"\x08\x00\x00\x12\x78\x56\x34\x12\x01\x02\x03\x04",12));
    tws_conn.role=1; CHECK(tws_api_send_data_to_slave(data,4,0)==-22 && tx_calls==1);
    /* Byte ABI accepts noncanonical boolean bytes. */
    fixture(); CHECK(bd19_tws_api_low_latency_enable(2)==-22 && !mode_enters && tws_api_get_low_latency_state()==0);
    CONFIG_LOW_LATENCY_ENABLE=1; CHECK(bd19_tws_api_low_latency_enable(2)==42 && bd19_tws_api_low_latency_enable(0)==-42 && mode_enters==1 && mode_exits==1 && tws_api_get_low_latency_state()==7);
    CHECK((u32)tws_api_power_saving_mode_enable()==0x87654321u && (u32)tws_api_power_saving_mode_disable()==0x12345678u);
    u8 addr[6]={1,2,3,4,5,6}, out[6];
    CHECK(!memcmp(tws_api_get_quick_connect_addr(),"5'*\xae\x85\x9c",6));
    tws_api_set_quick_connect_addr(addr); tws_api_get_quick_connect_addr()[0]=9; CHECK(tws_api_get_quick_connect_addr()[0]==9);
    tws_api_set_sibling_addr(addr); tws_api_get_sibling_addr(out); CHECK(!memcmp(addr,out,6));
    memcpy(hci_param.addr,addr,6); tws_api_get_local_addr(out); CHECK(!memcmp(out,addr,6));
    tws_api_set_local_channel((char)0xfe); CHECK(tws_api_get_local_channel()==(char)0xfe);
    tws_api_pair_all_way(9); tws_api_common_addr_en(3); tws_api_auto_role_switch_enable(); CHECK(tws_pair_all_way==9 && tws_common_addr_en==3 && tws_auto_role_switch==1);
    tws_api_auto_role_switch_disable(); CHECK(!tws_auto_role_switch);
    /* Task init order and dispatch are bounded by a test-only longjmp. */
    fixture(); tws_task_init(); CHECK(task_created==1 && buf_calls==1 && created_task);
    CHECK(!memcmp(gbredr_local_dev.internal_addr_local,"\xff\xff\xff\xff\xff\xff",6));
    memset(&tws_conn,0xa5,sizeof(tws_conn)); memset(&tws_link,0xa5,sizeof(tws_link));
    if(!setjmp(task_escape)) created_task(NULL);
    CHECK(!strcmp(init_trace,"pledbsc") && event_calls==1 && queue_pends==3);
    tws_task_exit_internal(&phone); CHECK(!strcmp(init_trace,"pledbscWC") && sem_posts==1 && delay_ticks==10000);
    fixture(); tws_conn.lmp_conn=&phone; tws_conn.monitor_start=2; phone.sbc_open=1; __role_switch_test(NULL); CHECK(switch_calls==1);
    struct sys_event event={0}; event.u.key.value=0x12345678; tws_key_event_handler(&event,NULL); CHECK(printf_arg==0x12345678);
    CHECK(!irq && !allocation);
    return TEST_DONE("btctrler_bd19_tws_task");
}
#endif
