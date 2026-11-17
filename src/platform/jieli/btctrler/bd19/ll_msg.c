/* BD19 LE and ISO message pools. Keep the SDK's four-byte message prefix,
 * cached logging flag, returning assertion paths and queue pointer reloads. */
#include <stdlib.h>
#include <stdio.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_d_LL, log_tag_const_i_LL;
extern void log_print(int level, const char *tag, const char *format, ...);
extern u32 sys_timer_get_ms(void);

#ifdef UNITTEST
#include "test.h"
static void *msg_test_malloc(size_t size);
static void msg_test_free(void *ptr);
static u32 msg_test_rets(void);
#define malloc msg_test_malloc
#define free msg_test_free
#endif

void ll_msg_queue_init(void)
{
    void *memory = malloc(512);
    ll_hdl.pMsgQueue = memory;
    if (!memory) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_msg.c", 35);
            printf("ASSERT-FAILD: __ll->pMsgQueue ");
            cpu_assert_debug();
        } else p33_soft_reset();
    }
    ll_hdl.pMsgQueue = (void *)lbuf_init_btctrler(ll_hdl.pMsgQueue, 512);
}

void ll_msg_queue_destroy(void) { free(ll_hdl.pMsgQueue); }
void *ll_msg_alloc(u16 size)
{
    return lbuf_alloc_btctrler((void *)ll_hdl.pMsgQueue, (u32)size + 4);
}
void ll_msg_free(void *msg) { lbuf_free_btctrler(msg); }
void ll_msg_send(void *msg)
{
    lbuf_push_btctrler(msg);
    thread_resume(&ll_hdl.thread);
}
ll_msg_t *ll_msg_recv(void) { return lbuf_pop_btctrler((void *)ll_hdl.pMsgQueue); }

void ll_msg_process(struct thread *th)
{
    (void)th;
    (void)sys_timer_get_ms();
    ll_msg_t *msg = ll_msg_recv();
    if (!msg) return;
    u8 logging = log_tag_const_d_LL;
    do {
        if (logging) log_print(1, NULL, "[Debug]: [LL]msg->state_id %d\r\n", msg->state_id);
        u16 state = msg->state_id;
        /* SDK checks only the special all-states value, not table bounds. */
        if (state != 255) {
            ll_dispatch_msg_t handler = ll_dispatch_msg_table[state];
            if (handler) handler((u8)msg->msg_id, msg->payload);
        }
        lbuf_free_btctrler(msg);
        msg = lbuf_pop_btctrler((void *)ll_hdl.pMsgQueue);
    } while (msg);
}

void ll_iso_msg_queue_init(void)
{
    void *memory = malloc(512);
    ll_iso_hdl.pMsgQueue = memory;
    if (!memory) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_msg.c", 120);
            printf("ASSERT-FAILD: __ll_iso->pMsgQueue ");
            cpu_assert_debug();
        } else p33_soft_reset();
    }
    ll_iso_hdl.pMsgQueue = (void *)lbuf_init_btctrler(ll_iso_hdl.pMsgQueue, 512);
}
void ll_iso_msg_queue_destroy(void) { free(ll_iso_hdl.pMsgQueue); }
void *ll_iso_msg_alloc(u16 size)
{
    return lbuf_alloc_btctrler((void *)ll_iso_hdl.pMsgQueue, (u32)size + 4);
}
void ll_iso_msg_free(void *msg) { lbuf_free_btctrler(msg); }
void ll_iso_msg_send(void *msg)
{
    u32 rets;
#ifdef UNITTEST
    rets = msg_test_rets();
#else
    __asm__ volatile("%0 = rets" : "=r"(rets));
#endif
    if (log_tag_const_i_LL) log_print(2, NULL, "[Info]: [LL]ISO Msg -> 0x%x\r\n", rets);
    lbuf_push_btctrler(msg);
    thread_resume(&ll_iso_hdl.thread);
}
ll_msg_t *ll_iso_msg_recv(void)
{
    if (log_tag_const_i_LL) log_print(2, NULL, "[Info]: [LL]ISO Msg <- \r\n");
    return lbuf_pop_btctrler((void *)ll_iso_hdl.pMsgQueue);
}

#ifdef UNITTEST
#include <string.h>
#include <stdarg.h>
struct link_layer_handle ll_hdl;
ll_iso_handle_t ll_iso_hdl;
ll_dispatch_msg_t ll_dispatch_msg_table[15];
u8 log_tag_const_d_LL, log_tag_const_i_LL;
int config_asser;
static struct lbuff_head_btctrler pool[2];
static u8 memory[512];
static int fail_malloc, assertions, resets, init_calls, timer_calls, rets_reads;
static void *init_input, *last_freed;
static struct lbuff_head **failure_slot;
static u32 alloc_size;
static struct lbuff_head_btctrler *alloc_pool;
static char trace[128];
static unsigned trace_len;
static void trace_op(char c) { CHECK(trace_len + 1 < sizeof trace); trace[trace_len++] = c; trace[trace_len] = 0; }
static void *msg_test_malloc(size_t size) { CHECK(size == 512); return fail_malloc ? NULL : memory; }
static void msg_test_free(void *p) { last_freed = p; }
void cpu_assert_debug(void) { assertions++; *failure_slot = (void *)memory; }
void p33_soft_reset(void) { resets++; *failure_slot = (void *)memory; }
struct lbuff_head_btctrler *lbuf_init_btctrler(void *p, u32 size)
{
    CHECK(size == 512); init_input = p; init_calls++; return &pool[0];
}
void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *p, u32 size) { alloc_pool = p; alloc_size = size; return memory; }
static struct { ll_msg_t msg; u8 data[4]; } messages[3];
static ll_msg_t *queued[4];
static unsigned pop_index, nfreed, dispatched, debug_logs, info_logs;
static void *freed[4];
static struct lbuff_head_btctrler *popped_pool[4];
void *lbuf_pop_btctrler(struct lbuff_head_btctrler *p)
{
    CHECK(pop_index < 4); trace_op('p'); popped_pool[pop_index] = p; return queued[pop_index++];
}
void lbuf_free_btctrler(void *p) { CHECK(nfreed < 4); trace_op('f'); freed[nfreed++] = p; }
void lbuf_push_btctrler(void *p) { CHECK(p == memory); trace_op('s'); }
void thread_resume(struct thread *th) { CHECK(th == &ll_hdl.thread || th == &ll_iso_hdl.thread); trace_op(th == &ll_hdl.thread ? 'l' : 'i'); }
u32 sys_timer_get_ms(void) { timer_calls++; trace_op('t'); return 123; }
static u32 msg_test_rets(void) { rets_reads++; trace_op('r'); return 0x12345678; }
void log_print(int level, const char *tag, const char *fmt, ...)
{
    CHECK(tag == NULL);
    va_list ap; va_start(ap, fmt);
    if (level == 1) {
        CHECK(strcmp(fmt, "[Debug]: [LL]msg->state_id %d\r\n") == 0);
        CHECK(va_arg(ap, int) == (debug_logs == 0 ? 1 : debug_logs == 1 ? 255 : 1));
        debug_logs++; log_tag_const_d_LL = 0;
        if (debug_logs == 1) messages[0].msg.state_id = 2;
        trace_op('d');
    } else {
        CHECK(level == 2); info_logs++; trace_op('g');
        if (strstr(fmt, "->")) CHECK(va_arg(ap, u32) == 0x12345678);
    }
    va_end(ap);
}
static void dispatch(u8 id, void *data)
{
    CHECK(id == (dispatched == 0 ? 0x34 : 0xcd));
    CHECK(data == messages[dispatched == 0 ? 0 : 2].data);
    CHECK(*(u8 *)data == (dispatched == 0 ? 0xa5 : 0x5a));
    ll_hdl.pMsgQueue = (void *)&pool[1]; dispatched++; trace_op('c');
}
static void clear_trace(void) { trace_len = 0; trace[0] = 0; }
int main(void)
{
    for (int fail = 0; fail <= 1; fail++) {
        fail_malloc = fail;
        for (config_asser = 0; config_asser <= 1; config_asser++) {
            init_calls = assertions = resets = 0;
            failure_slot = &ll_hdl.pMsgQueue;
            ll_msg_queue_init();
            CHECK(init_calls == 1 && init_input == memory && ll_hdl.pMsgQueue == (void *)&pool[0]);
            failure_slot = &ll_iso_hdl.pMsgQueue;
            ll_iso_msg_queue_init();
            CHECK(init_calls == 2 && init_input == memory && ll_iso_hdl.pMsgQueue == (void *)&pool[0]);
            CHECK(assertions == (fail && config_asser ? 2 : 0));
            CHECK(resets == (fail && !config_asser ? 2 : 0));
        }
    }
    ll_msg_queue_destroy(); CHECK(last_freed == &pool[0] && ll_hdl.pMsgQueue == (void *)&pool[0]);
    ll_iso_msg_queue_destroy(); CHECK(last_freed == &pool[0] && ll_iso_hdl.pMsgQueue == (void *)&pool[0]);
    CHECK(ll_msg_alloc(65535) == memory && alloc_size == 65539 && alloc_pool == &pool[0]);
    CHECK(ll_iso_msg_alloc(65535) == memory && alloc_size == 65539 && alloc_pool == &pool[0]);
    messages[0].msg.state_id = 1; messages[0].msg.msg_id = 0x1234; messages[0].data[0] = 0xa5;
    messages[1].msg.state_id = 255;
    messages[2].msg.state_id = 1; messages[2].msg.msg_id = 0xabcd; messages[2].data[0] = 0x5a;
    for (unsigned i = 0; i < 3; i++) queued[i] = &messages[i].msg;
    ll_dispatch_msg_table[1] = ll_dispatch_msg_table[2] = dispatch;
    log_tag_const_d_LL = 1;
    clear_trace(); ll_msg_process(NULL);
    CHECK(strcmp(trace, "tpdcfpdfpdcfp") == 0);
    CHECK(timer_calls == 1 && dispatched == 2 && debug_logs == 3 && nfreed == 3 && pop_index == 4);
    for (unsigned i = 0; i < 3; i++) CHECK(freed[i] == &messages[i].msg);
    CHECK(popped_pool[0] == &pool[0]);
    for (unsigned i = 1; i < 4; i++) CHECK(popped_pool[i] == &pool[1]);
    clear_trace(); ll_msg_send(memory); CHECK(strcmp(trace, "sl") == 0);
    for (log_tag_const_i_LL = 0; log_tag_const_i_LL <= 1; log_tag_const_i_LL++) {
        clear_trace(); ll_iso_msg_send(memory);
        CHECK(strcmp(trace, log_tag_const_i_LL ? "rgsi" : "rsi") == 0);
        pop_index = 0; queued[0] = NULL;
        clear_trace(); CHECK(ll_iso_msg_recv() == NULL);
        CHECK(strcmp(trace, log_tag_const_i_LL ? "gp" : "p") == 0 && popped_pool[0] == &pool[0]);
    }
    CHECK(rets_reads == 2 && info_logs == 2);
    return TEST_DONE("btctrler_ll_msg_bd19");
}
#endif
