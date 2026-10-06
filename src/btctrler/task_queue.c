/* Controller task message construction and queue-full policy, recovered from
 * btctrler_task.c. The scheduler and interrupt-state probes remain SDK APIs. */
#include <string.h>
#include "btctrler/glue.h"

__attribute__((section(".hci_controller_bss"))) u8 btctrl_qfull_cnt;
int btctrl_msg_qfull_hook(u8 count);
#ifndef UNITTEST
/* The SDK's cpu.h probes: inside an interrupt handler; interrupts disabled
 * or masked (IPMASK at the controller's level 6). */
static inline int cpu_in_irq(void)
{
    int flag;
    __asm__ volatile("%0 = icfg" : "=r"(flag));
    return flag & 0xff;
}
static inline int cpu_irq_disabled(void)
{
    int flag;
    __asm__ volatile("%0 = icfg" : "=r"(flag));
    return (flag & 0x300) != 0x300 || *(volatile u32 *)0x10f1a8 == 6;
}
#else
int cpu_in_irq(void);
int cpu_irq_disabled(void);
#endif
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
const char *os_current_task(void);

static void btctrl_msg_qfull_handler(void)
{
    if (btctrl_qfull_cnt != 255) ++btctrl_qfull_cnt;
    btctrl_msg_qfull_hook(btctrl_qfull_cnt);
}

/* Connection commands deliberately do not update the queue-full counter.
 * Only link events avoid retrying from the controller task itself. */
static int post_message(int type, int count, int *words, int report_full, int link_event)
{
    for (;;) {
        int result = os_taskq_post_type("btctrler", type, count, words);
        if (result != 21) {
            if (report_full) btctrl_qfull_cnt = 0;
            return result;
        }
        if (report_full) btctrl_msg_qfull_handler();
        if (cpu_in_irq() || cpu_irq_disabled()) return result;
        if (link_event && !strcmp(os_current_task(), "btctrler")) return result;
        os_time_dly(2);
    }
}

static int valid_count(int count, int maximum)
{
    if (count >= 0 && count <= maximum) return 1;
    btctrler_assert_failed("controller message argument count", count);
    return 0;
}

static int cmd_to_conn(void *conn, int count, va_list args)
{
    int words[8];
    if (!conn) return -22;
    /* SDK requires count + 1 < 8, leaving one spare slot. */
    if (!valid_count(count, 6)) return -22;
    words[0] = (int)(uintptr_t)conn;
    for (int i = 0; i < count; ++i) words[i + 1] = va_arg(args, int);
    return post_message(0x400003, count + 1, words, 0, 0);
}

static int classic_supported(void)
{
    if (config_btctler_modules & 1) return 1;
    btctrler_assert_failed("BT_MODULE_CLASSIC", 0);
    return 0;
}

int lmp_hci_cmd_to_conn_for_addr(u8 *addr, int argc, ...)
{
    if (!classic_supported()) return 0;
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    va_list args; va_start(args, argc);
    int result = cmd_to_conn(conn, argc, args);
    va_end(args);
    return result;
}
int lmp_hci_cmd_to_conn_for_handle(u16 handle, int argc, ...)
{
    if (!classic_supported()) return 0;
    struct lmp_connection *conn = lmp_conn_for_handle(handle);
    va_list args; va_start(args, argc);
    int result = cmd_to_conn(conn, argc, args);
    va_end(args);
    return result;
}
int lmp_hci_cmd_to_conn(void *conn, int argc, ...)
{
    if (!classic_supported()) return 0;
    va_list args; va_start(args, argc);
    int result = cmd_to_conn(conn, argc, args);
    va_end(args);
    return result;
}
int btctrler_hci_cmd_to_task(int cmd, int argc, ...)
{
    int words[16];
    if (!valid_count(argc, 15)) return -22;
    va_list args; va_start(args, argc);
    for (int i = 0; i < argc; ++i) words[i] = va_arg(args, int);
    va_end(args);
    return post_message(cmd, argc, words, 1, 0);
}
int bredr_link_event(void *link, int argc, ...)
{
    int words[4];
    /* SDK had no bound check on this four-word message. */
    if (!valid_count(argc, 3)) return -22;
    words[0] = (int)(uintptr_t)link;
    va_list args; va_start(args, argc);
    for (int i = 0; i < argc; ++i) words[i + 1] = va_arg(args, int);
    va_end(args);
    return post_message(0x400001, argc + 1, words, 1, 1);
}

#ifdef UNITTEST
#include <assert.h>
#include <limits.h>
#include <stdio.h>
int config_btctler_modules = 3;
static struct lmp_connection connection;
static u8 address[6];
static int missing, lookups, assertions, posts, delays, hooks, in_irq, disabled, task_reads;
static int full_remaining, final_result, last_type, last_count, last_words[16];
static const char *current_task = "host";
static int last_hook, probe_irq, probe_disabled;
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; ++assertions; }
struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ assert(addr == address); ++lookups; return missing ? NULL : &connection; }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ assert(handle == 0x1234); ++lookups; return missing ? NULL : &connection; }
int cpu_in_irq(void) { ++probe_irq; return in_irq; }
int cpu_irq_disabled(void) { ++probe_disabled; return disabled; }
const char *os_current_task(void) { ++task_reads; return current_task; }
void os_time_dly(int ticks) { assert(ticks == 2); ++delays; }
int btctrl_msg_qfull_hook(u8 count) { ++hooks; last_hook = count; return 1; }
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    assert(!strcmp(name, "btctrler")); assert(argc >= 0 && argc <= 16);
    if (posts) {
        assert(last_type == type && last_count == argc);
        assert(!memcmp(last_words, argv, argc * sizeof(int)));
    }
    ++posts; last_type = type; last_count = argc;
    memcpy(last_words, argv, argc * sizeof(int));
    if (full_remaining) { --full_remaining; return 21; }
    return final_result;
}
static void reset(void)
{
    missing = lookups = assertions = posts = delays = hooks = in_irq = disabled = task_reads = 0;
    full_remaining = final_result = probe_irq = probe_disabled = 0;
    btctrl_qfull_cnt = 0; config_btctler_modules = 3; current_task = "host";
}
static int send_kind(int kind)
{
    if (kind == 0) return lmp_hci_cmd_to_conn(&connection, 2, 0x12345678, -7);
    if (kind == 1) return btctrler_hci_cmd_to_task(0x400008, 2, 0x12345678, -7);
    return bredr_link_event(&connection, 2, 0x12345678, -7);
}
int main(void)
{
    reset();
    assert(lmp_hci_cmd_to_conn_for_addr(address, 2, 1, -2) == 0);
    assert(lookups == 1 && last_type == 0x400003 && last_count == 3);
    assert(last_words[0] == (int)(uintptr_t)&connection && last_words[1] == 1 && last_words[2] == -2);
    reset(); assert(lmp_hci_cmd_to_conn_for_handle(0x1234, 0) == 0);
    assert(lookups == 1 && last_count == 1);
    reset(); missing = 1;
    assert(lmp_hci_cmd_to_conn_for_addr(address, 0) == -22);
    assert(lmp_hci_cmd_to_conn_for_handle(0x1234, 0) == -22);
    assert(lmp_hci_cmd_to_conn(NULL, INT_MAX) == -22 && !posts && !assertions);
    for (int modules = 0; modules < 4; ++modules) {
        reset(); config_btctler_modules = modules;
        int expected = modules & 1;
        assert(lmp_hci_cmd_to_conn_for_addr(address, 0) == 0);
        assert(lmp_hci_cmd_to_conn_for_handle(0x1234, 0) == 0);
        assert(lmp_hci_cmd_to_conn(&connection, 0) == 0);
        assert(posts == expected * 3 && lookups == expected * 2 && assertions == !expected * 3);
    }
    const int invalid[] = { INT_MIN, -2, -1, 16, INT_MAX };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset(); int n = invalid[i];
        assert(lmp_hci_cmd_to_conn(&connection, n) == -22);
        assert(lmp_hci_cmd_to_conn_for_addr(address, n) == -22);
        assert(lmp_hci_cmd_to_conn_for_handle(0x1234, n) == -22);
        assert(btctrler_hci_cmd_to_task(7, n) == -22);
        assert(bredr_link_event(&connection, n) == -22);
        assert(assertions == 5 && !posts);
    }
    for (int n = 7; n <= 15; ++n) {
        reset(); assert(lmp_hci_cmd_to_conn(&connection, n) == -22 && !posts);
    }
    for (int n = 4; n <= 15; ++n) {
        reset(); assert(bredr_link_event(&connection, n) == -22 && !posts);
    }
    reset(); assert(lmp_hci_cmd_to_conn(&connection, 6, 1, 2, 3, 4, 5, 6) == 0);
    assert(last_count == 7 && last_words[6] == 6);
    reset(); assert(btctrler_hci_cmd_to_task(77, 15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15) == 0);
    assert(last_type == 77 && last_count == 15 && last_words[14] == 15);
    reset(); assert(btctrler_hci_cmd_to_task(77, 0) == 0 && last_count == 0);
    reset(); assert(bredr_link_event(NULL, 3, 1, 2, 3) == 0);
    assert(last_type == 0x400001 && last_count == 4 && last_words[0] == 0 && last_words[3] == 3);
    for (int kind = 0; kind < 3; ++kind)
        for (int irq = 0; irq < 2; ++irq)
            for (int masked = 0; masked < 2; ++masked)
                for (int self = 0; self < 2; ++self) {
                    reset(); full_remaining = 2; btctrl_qfull_cnt = 254;
                    in_irq = irq; disabled = masked; current_task = self ? "btctrler" : "host";
                    int stop = irq || masked || (kind == 2 && self);
                    assert(send_kind(kind) == (stop ? 21 : 0));
                    assert(posts == (stop ? 1 : 3) && delays == (stop ? 0 : 2));
                    assert(hooks == (kind ? (stop ? 1 : 2) : 0));
                    assert(btctrl_qfull_cnt == (kind ? (stop ? 255 : 0) : 254));
                    assert(!kind || last_hook == 255);
                    assert(probe_disabled == (irq ? 0 : stop ? 1 : 2));
                    assert(task_reads == (kind == 2 && !irq && !masked ? (stop ? 1 : 2) : 0));
                }
    reset(); full_remaining = 300;
    assert(btctrler_hci_cmd_to_task(4, 0) == 0 && hooks == 300 && last_hook == 255 && !btctrl_qfull_cnt);
    for (int kind = 0; kind < 3; ++kind) {
        reset(); final_result = -5; btctrl_qfull_cnt = 12;
        assert(send_kind(kind) == -5 && posts == 1 && !delays && !hooks && !probe_irq);
        assert(btctrl_qfull_cnt == (kind ? 0 : 12));
    }
    puts("btctrler task queue tests passed");
}
#endif
