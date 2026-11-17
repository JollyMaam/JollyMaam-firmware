/* Recovered from BD19 tws_ci_data.c.o, occurrence 1. */
#include "btctrler/glue.h"
#include <string.h>

#define CLASSIC_TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_CHANNEL __attribute__((section(".tws_sync_channel.11"), aligned(4)))

extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;
extern int os_sem_create(OS_SEM *sem, int initial);
extern int os_sem_set(OS_SEM *sem, u16 value);
extern int os_sem_pend(OS_SEM *sem, u32 timeout);

static struct tws_config_interface tws_ci_handle;

static int tws_ci_data_tx(struct lmp_connection *conn, u8 *buffer, int ignored)
    CLASSIC_TWS_CODE;
static void tws_ci_data_tx_suss(void) CLASSIC_TWS_CODE;
static int tws_ci_data_rx_and_tx(struct lmp_connection *conn, u8 *rx, int len,
                                 u8 *tx) CLASSIC_TWS_CODE;
static void tws_ci_data_rx(struct lmp_connection *conn, u8 *buffer, int len)
    CLASSIC_TWS_CODE;
static int tws_ci_data_event_handler(struct lmp_connection *conn, int event,
                                     int *arg) CLASSIC_TWS_CODE;

void *tws_ci_sync_open(void *priv, void (*rx_handler)(void *, void *, int))
    CLASSIC_TWS_CODE;
void *tws_ci_sync_open(void *priv, void (*rx_handler)(void *, void *, int))
{
    tws_ci_handle.priv = priv;
    tws_ci_handle.rx_handler = rx_handler;
    (void)os_sem_create(&tws_ci_handle.sem, 0);
    return &tws_ci_handle;
}

int tws_ci_data_sync(void *priv, void *data, int len, u32 time)
    CLASSIC_TWS_CODE;
int tws_ci_data_sync(void *priv, void *data, int len, u32 time)
{
    int result = 0;
    if (CONFIG_BTCTLER_TWS_ENABLE && tws_link.link != NULL) {
        struct tws_config_interface *handle = priv != NULL ? priv : &tws_ci_handle;
        (void)os_sem_set(&handle->sem, 0);
        local_irq_disable();
        handle->data = data;
        handle->len = len;
        handle->tx_enable = 1;
        local_irq_enable();
        if (time != 0 && os_sem_pend(&handle->sem, time / 10) != 0)
            return 0;
        result = len;
    }
    return result;
}

static int tws_ci_data_tx(struct lmp_connection *conn, u8 *buffer, int ignored)
{
    int len = 0;
    (void)conn;
    (void)ignored;
    if (tws_ci_handle.tx_enable) {
        __builtin_memcpy(buffer, tws_ci_handle.data, (u32)tws_ci_handle.len);
        len = tws_ci_handle.len;
    }
    return len;
}

static void tws_ci_data_tx_suss(void)
{
    tws_ci_handle.tx_enable = 0;
    (void)os_sem_post(&tws_ci_handle.sem);
}

static int tws_ci_data_rx_and_tx(struct lmp_connection *conn, u8 *rx, int len,
                                 u8 *tx)
{
    int tx_len = 0;
    if (len != 0) {
        void (*handler)(void *, void *, int) = tws_ci_handle.rx_handler;
        if (handler != NULL)
            handler(tws_ci_handle.priv, rx, len);
    }
    if (tx != NULL && tws_ci_handle.tx_enable) {
        __builtin_memcpy(tx, tws_ci_handle.data, (u32)tws_ci_handle.len);
        tx_len = tws_ci_handle.len;
    }
    (void)conn;
    return tx_len;
}

static void tws_ci_data_rx(struct lmp_connection *conn, u8 *buffer, int len)
{
    void (*handler)(void *, void *, int) = tws_ci_handle.rx_handler;
    (void)conn;
    if (handler != NULL)
        handler(tws_ci_handle.priv, buffer, len);
}

static int tws_ci_data_event_handler(struct lmp_connection *conn, int event,
                                     int *arg)
{
    (void)conn;
    (void)arg;
    if (event == 45 || event == 46) {
        local_irq_disable();
        if (tws_ci_handle.tx_enable) {
            (void)os_sem_post(&tws_ci_handle.sem);
            tws_ci_handle.tx_enable = 0;
        }
        local_irq_enable();
    }
    return 0;
}

const struct tws_sync_channel tws_ci_data TWS_CHANNEL = {
    .channel = 11,
    .attribute = 7,
    .tx_sync_req = tws_ci_data_tx,
    .tx_sync_req_suss = (void (*)())tws_ci_data_tx_suss,
    .rx_sync_req_and_res = tws_ci_data_rx_and_tx,
    .tx_sync_res_suss = (void (*)())tws_ci_data_tx_suss,
    .rx_sync_res = tws_ci_data_rx,
    .event_handler = tws_ci_data_event_handler,
};

#ifdef UNITTEST
#include "test.h"

int CONFIG_BTCTLER_TWS_ENABLE;
struct tws_link_connection tws_link;
static int sem_create_result, sem_set_result, sem_pend_result, sem_post_result;
static int sem_creates, sem_sets, sem_pends, sem_posts, irq_depth, pend_ticks;
static void *rx_priv_seen;
static int rx_len_seen, rx_calls, mutate_len_in_rx;
static int clear_tx_in_rx;
static int event_post_saw_enabled;
static OS_SEM *last_sem;
static int last_post_flag, last_post_depth, mutate_post_flag, mutate_enable_len;
static int replace_data_in_rx, pend_event;
static struct tws_config_interface *enable_context;
static u8 rx_reply[3] = {0x91, 0x82, 0x73};
static char hook_trace[256];
static unsigned int hook_trace_len;
static void mark(char c)
{
    CHECK(hook_trace_len < sizeof(hook_trace) - 1);
    if (hook_trace_len < sizeof(hook_trace) - 1) hook_trace[hook_trace_len++] = c;
    hook_trace[hook_trace_len] = 0;
}
static void clear_trace(void) { hook_trace_len = 0; hook_trace[0] = 0; }

int os_sem_create(OS_SEM *sem, int initial)
{ last_sem = sem; mark('C'); CHECK(initial == 0); sem_creates++; return sem_create_result; }
int os_sem_set(OS_SEM *sem, u16 value)
{ last_sem = sem; mark('S'); CHECK(value == 0); sem_sets++; return sem_set_result; }
int os_sem_pend(OS_SEM *sem, u32 ticks)
{
    last_sem = sem; mark('W'); CHECK(irq_depth == 0); sem_pends++; pend_ticks = (int)ticks;
    if (pend_event) tws_ci_data.event_handler(NULL, pend_event, NULL);
    return sem_pend_result;
}
int os_sem_post(OS_SEM *sem)
{
    last_sem = sem; mark('P'); sem_posts++;
    last_post_flag = tws_ci_handle.tx_enable;
    last_post_depth = irq_depth;
    if (mutate_post_flag) tws_ci_handle.tx_enable = 1;
    if (sem_posts == 2) event_post_saw_enabled = tws_ci_handle.tx_enable;
    return sem_post_result;
}
void local_irq_disable(void) { mark('D'); irq_depth++; }
void local_irq_enable(void)
{
    mark('E'); CHECK(irq_depth == 1); irq_depth--;
    if (mutate_enable_len) enable_context->len = 99;
}

static void rx_test(void *priv, void *data, int len)
{
    (void)data;
    rx_calls++; rx_priv_seen = priv; rx_len_seen = len;
    if (mutate_len_in_rx) tws_ci_handle.len = 2;
    if (clear_tx_in_rx) tws_ci_handle.tx_enable = 0;
    if (replace_data_in_rx) {
        tws_ci_handle.data = rx_reply;
        tws_ci_handle.len = sizeof(rx_reply);
        tws_ci_handle.tx_enable = 1;
    }
}

static void test_reset(void)
{
    memset(&tws_ci_handle, 0, sizeof(tws_ci_handle));
    memset(&tws_link, 0, sizeof(tws_link));
    CONFIG_BTCTLER_TWS_ENABLE = 1;
    sem_create_result = sem_set_result = sem_pend_result = sem_post_result = 0;
    sem_creates = sem_sets = sem_pends = sem_posts = irq_depth = pend_ticks = 0;
    rx_priv_seen = NULL; rx_len_seen = rx_calls = mutate_len_in_rx = clear_tx_in_rx = 0;
    event_post_saw_enabled = -1;
    last_sem = NULL; last_post_flag = last_post_depth = 0;
    mutate_post_flag = mutate_enable_len = replace_data_in_rx = pend_event = 0;
    enable_context = NULL; clear_trace();
}

int main(void)
{
    static struct link_connection peer;
    u8 src[8] = {1,2,3,4,5,6,7,8}, out[12] = {0};
    int n;
#if UINTPTR_MAX == UINT32_MAX
    CHECK(sizeof(struct tws_config_interface) == 100);
#endif
    CHECK(tws_ci_data.channel == 11 && tws_ci_data.attribute == 7);
    CHECK(tws_ci_data.tx_sync_req_suss == tws_ci_data.tx_sync_res_suss);

    test_reset(); sem_create_result = -1;
    void *ctx = tws_ci_sync_open((void *)0x1234, rx_test);
    CHECK(ctx == &tws_ci_handle && sem_creates == 1);
    CHECK(tws_ci_handle.priv == (void *)0x1234 && tws_ci_handle.rx_handler == rx_test);

    /* No linked peer or disabled config leaves context/semaphore untouched. */
    CHECK(tws_ci_data_sync(ctx, src, 4, 1) == 0 && sem_sets == 0);
    tws_link.link = &peer;
    CONFIG_BTCTLER_TWS_ENABLE = 0;
    CHECK(tws_ci_data_sync(ctx, src, 4, 1) == 0 && sem_sets == 0);
    CONFIG_BTCTLER_TWS_ENABLE = 1;

    /* Timeout is unsigned floor(time/10); failure does not cancel publication. */
    sem_pend_result = -1;
    CHECK(tws_ci_data_sync(ctx, src, 4, 9) == 0 && pend_ticks == 0);
    CHECK(tws_ci_handle.tx_enable && tws_ci_handle.data == src && tws_ci_handle.len == 4);
    sem_pend_result = 0;
    CHECK(tws_ci_data_sync(ctx, src, 4, 10) == 4 && pend_ticks == 1);
    CHECK(tws_ci_data_sync(ctx, src, 4, UINT32_MAX) == 4 && pend_ticks == UINT32_MAX / 10u);
    sem_pends = 0;
    CHECK(tws_ci_data_sync(ctx, src, 4, 0) == 4 && sem_pends == 0);
    CHECK(irq_depth == 0 && sem_sets == 4);

    /* Explicit context is honored; null selects the static context. */
    struct tws_config_interface external = {0};
    CHECK(tws_ci_data_sync(&external, src, 1, 0) == 1 && external.len == 1);

    n = tws_ci_data.tx_sync_req(NULL, out, 123);
    CHECK(n == 4 && memcmp(out, src, 4) == 0);
    /* Return length reload follows memcpy, so an output alias to stored len is observable. */
    tws_ci_handle.data = src;
    tws_ci_handle.len = 4;
    src[0] = 2; src[1] = src[2] = src[3] = 0;
    tws_ci_handle.tx_enable = 1;
    n = tws_ci_data.tx_sync_req(NULL, (u8 *)&tws_ci_handle.len, 0);
    CHECK(n == 2 && tws_ci_handle.len == 2);

    tws_ci_handle.data = src; tws_ci_handle.len = 4; tws_ci_handle.tx_enable = 1;
    CHECK(tws_ci_data.rx_sync_req_and_res(NULL, out, 0, out) == 4 && rx_calls == 0);
    mutate_len_in_rx = 1;
    CHECK(tws_ci_data.rx_sync_req_and_res(NULL, out, 3, out + 4) == 2);
    CHECK(rx_calls == 1 && rx_priv_seen == (void *)0x1234 && rx_len_seen == 3);
    mutate_len_in_rx = 0; clear_tx_in_rx = 1; tws_ci_handle.tx_enable = 1;
    CHECK(tws_ci_data.rx_sync_req_and_res(NULL, out, 1, out + 4) == 0);
    CHECK(rx_calls == 2 && !tws_ci_handle.tx_enable);
    clear_tx_in_rx = 0;
    tws_ci_data.rx_sync_res(NULL, out, 0);
    CHECK(rx_calls == 3 && rx_len_seen == 0);

    /* tx_suss clears before posting; events post before clearing under IRQ mask. */
    tws_ci_handle.tx_enable = 1;
    tws_ci_data.tx_sync_req_suss();
    CHECK(!tws_ci_handle.tx_enable && sem_posts == 1);
    tws_ci_handle.tx_enable = 1;
    CHECK(tws_ci_data.event_handler(NULL, 45, NULL) == 0);
    CHECK(!tws_ci_handle.tx_enable && sem_posts == 2 && event_post_saw_enabled == 1 && irq_depth == 0);
    tws_ci_handle.tx_enable = 1;
    CHECK(tws_ci_data.event_handler(NULL, 46, NULL) == 0 && sem_posts == 3);
    CHECK(tws_ci_data.event_handler(NULL, -1, NULL) == 0 && sem_posts == 3);

    test_reset(); tws_link.link = &peer;
    CONFIG_BTCTLER_TWS_ENABLE = -1;
    sem_set_result = -7;
    CHECK(tws_ci_data_sync(NULL, src, 3, 0) == 3);
    CHECK(last_sem == &tws_ci_handle.sem && tws_ci_handle.data == src && tws_ci_handle.len == 3);
    CHECK(tws_ci_handle.tx_enable == 1 && !strcmp(hook_trace, "SDE"));
    sem_set_result = 0; clear_trace();
    CHECK(tws_ci_data_sync(NULL, src, 3, 1) == 3 && pend_ticks == 0);
    CHECK(!strcmp(hook_trace, "SDEW"));
    tws_ci_handle.data = src; tws_ci_handle.len = 3; tws_ci_handle.tx_enable = 1;
    sem_create_result = -1;
    CHECK(tws_ci_sync_open((void *)0x5678, rx_test) == &tws_ci_handle);
    CHECK(tws_ci_handle.data == src && tws_ci_handle.len == 3 && tws_ci_handle.tx_enable == 1);
    CHECK(tws_ci_handle.priv == (void *)0x5678 && tws_ci_handle.rx_handler == rx_test);

    clear_trace(); mutate_enable_len = 1; enable_context = &external;
    CHECK(tws_ci_data_sync(&external, src, 7, 0) == 7);
    CHECK(external.len == 99 && external.data == src && external.tx_enable == 1);
    CHECK(last_sem == &external.sem && tws_ci_handle.len == 3);
    CHECK(!strcmp(hook_trace, "SDE")); mutate_enable_len = 0;
    /* A custom publication does not redirect the transport's singleton. */
    CHECK(tws_ci_data.tx_sync_req(NULL, out + 1, 0) == 3);
    CHECK(!memcmp(out + 1, src, 3));
    CHECK(tws_ci_data_sync(NULL, NULL, -1, 0) == -1);
    CHECK(tws_ci_handle.data == NULL && tws_ci_handle.len == -1 && tws_ci_handle.tx_enable);
    tws_ci_handle.tx_enable = 0; /* Do not execute the SDK's huge unsafe copy. */

    clear_trace(); pend_event = 45;
    CHECK(tws_ci_data_sync(NULL, src, 4, 10) == 4 && tws_ci_handle.tx_enable == 0);
    CHECK(!strcmp(hook_trace, "SDEWDPE") && last_post_flag == 1);
    pend_event = 0;

    int replacement_len = 2;
    tws_ci_handle.data = &replacement_len; tws_ci_handle.len = sizeof(replacement_len);
    tws_ci_handle.tx_enable = 1;
    CHECK(tws_ci_data.rx_sync_req_and_res(NULL, NULL, 0, (u8 *)&tws_ci_handle.len) == 2);
    CHECK(tws_ci_handle.len == 2);
    tws_ci_handle.rx_handler = rx_test; replace_data_in_rx = 1;
    tws_ci_handle.data = NULL; tws_ci_handle.len = 0; tws_ci_handle.tx_enable = 0;
    memset(out, 0xa5, sizeof(out));
    CHECK(tws_ci_data.rx_sync_req_and_res(NULL, out, -1, out + 1) == 3);
    CHECK(rx_len_seen == -1 && !memcmp(out + 1, rx_reply, sizeof(rx_reply)));
    CHECK(out[0] == 0xa5 && out[4] == 0xa5); replace_data_in_rx = 0;

    /* Reentrant post writes survive completion but are overwritten by disconnect. */
    mutate_post_flag = 1; sem_post_result = -9;
    tws_ci_handle.tx_enable = 1; clear_trace();
    tws_ci_data.tx_sync_res_suss();
    CHECK(last_post_flag == 0 && last_post_depth == 0 && tws_ci_handle.tx_enable == 1);
    CHECK(!strcmp(hook_trace, "P"));
    clear_trace(); CHECK(tws_ci_data.event_handler(NULL, 45, NULL) == 0);
    CHECK(last_post_flag == 1 && last_post_depth == 1 && tws_ci_handle.tx_enable == 0);
    CHECK(!strcmp(hook_trace, "DPE") && irq_depth == 0);
    clear_trace(); int old_posts = sem_posts;
    CHECK(tws_ci_data.event_handler(NULL, 46, NULL) == 0);
    CHECK(sem_posts == old_posts && !strcmp(hook_trace, "DE"));
    clear_trace();
    const int other_events[] = {44, 47, -2147483647 - 1, 2147483647};
    for (unsigned int i = 0; i < sizeof(other_events) / sizeof(*other_events); i++)
        CHECK(tws_ci_data.event_handler(NULL, other_events[i], NULL) == 0);
    CHECK(!hook_trace[0] && sem_posts == old_posts);
    return TEST_DONE("bd19_tws_ci_data");
}
#endif
