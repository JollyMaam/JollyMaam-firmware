/* Remaining Classic sniff settings, queries and pre-anchor reset policy,
 * recovered from SDK lmp.c. Slot timers remain SDK APIs. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LMP;
void log_print(int level, const char *tag, const char *format, ...);

__attribute__((weak)) void sniff_achor_point_hook(u32 slot) { (void)slot; }
__attribute__((weak)) void sniff_hook(u32 slot, u8 num, u8 first_conn, int t_sniff)
{ (void)slot; (void)num; (void)first_conn; (void)t_sniff; }
__attribute__((weak)) void sniff_negotiation_hook(u16 *interval, u16 *attempt, u16 *timeout, u8 negotiation)
{ (void)interval; (void)attempt; (void)timeout; (void)negotiation; }
__attribute__((weak)) u8 lmp_rx_sniff_req_check(void) { return 0; }

void lmp_set_sniff_establish_by_remote(u8 enable)
{
    if (log_tag_const_i_LMP)
        log_print(2, NULL, "[Info]: [LMP]%s %d\n\r\n", __func__, enable);
    sniff_establish_by_remote = enable;
}
void lmp_set_sniff_disable(void)
{
    if (log_tag_const_i_LMP)
        log_print(2, NULL, "[Info]: [LMP]%s\n\r\n", __func__);
    hci_param.default_link_policy &= ~4u;
}
u8 get_1to2_conn_exit_sniff(void)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn)
        if (conn->core_s.handle && conn->link && conn->link->sniff_sta) return 1;
    return 0;
}
int lmp_in_sniff_mode(struct lmp_connection *conn)
{
    local_irq_disable();
    int result = conn && conn->link && conn->core.operation_mode == 2;
    local_irq_enable();
    return result;
}
void lmp_sniff_pre_anchor_point_reset(struct lmp_connection *conn)
{
    if (!conn || !conn->link) return;
    struct link_connection *link = conn->link;
    struct lmp_sniff *sniff = conn->sniff;
    if (!link->sniff_pre_point_reset_flag || conn->core.operation_mode != 2) return;
    /* Missing state and zero intervals cannot supply a valid anchor timer. */
    if (!sniff || !sniff->T_sniff || sniff->T_sniff > 100 || link->sniff_state != 1) return;
    link->sniff_long_interval_filter = 2;
    u32 clock = bredr_read_slot_clk();
    if (clock > sniff->wake_instant) return;
    conn->link->sniff_state = 4;
    local_irq_disable();
    u32 interval = sniff->T_sniff;
    if (!interval) { local_irq_enable(); return; }
    /* Fold the SDK's repeated interval subtraction into one operation.
     * Use the difference to avoid overflow in clock + interval. */
    u32 delta = sniff->wake_instant - clock;
    if (delta > interval)
        sniff->wake_instant -= ((delta - 1) / interval) * interval;
    if (sniff->wake_instant - clock < 3) sniff->wake_instant += interval;
    slot_timer_reset(sniff->timer, (int)(sniff->wake_instant - clock));
    link_task_set_period((int)clock);
    idle_reset(0);
    conn->link->sniff_pre_point_reset_flag = 0;
    local_irq_enable();
}

#ifdef UNITTEST
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct lmp_connection lmp_acl_link[1];
struct hci_parameter hci_param;
u8 sniff_establish_by_remote, log_tag_const_i_LMP;
static struct link_connection link, replacement;
static struct lmp_sniff sniff;
static u32 slot_clock, timer_delay, timer_wake;
static int irq_depth, clock_calls, timer_calls, logs, mutate_link;
static char trace[32];
static unsigned at;
static void record(char c) { assert(at + 1 < sizeof(trace)); trace[at++] = c; trace[at] = 0; }
void local_irq_disable(void) { assert(!irq_depth); irq_depth = 1; record('D'); }
void local_irq_enable(void) { assert(irq_depth == 1); irq_depth = 0; record('E'); }
u32 bredr_read_slot_clk(void)
{
    assert(!irq_depth && link.sniff_long_interval_filter == 2);
    ++clock_calls; record('C');
    if (mutate_link) lmp_acl_link->link = &replacement;
    return slot_clock;
}
void slot_timer_reset(u8 id, int delay)
{
    assert(irq_depth && id == sniff.timer); ++timer_calls;
    timer_delay = delay; timer_wake = sniff.wake_instant; record('T');
}
void link_task_set_period(int clock) { assert(irq_depth && (u32)clock == slot_clock); record('P'); }
void idle_reset(int slot) { assert(irq_depth && !slot); record('I'); }
void log_print(int level, const char *tag, const char *format, ...)
{ assert(level == 2 && !tag && format); ++logs; }
static void reset(void)
{
    assert(!irq_depth); memset(lmp_acl_link, 0, sizeof(lmp_acl_link));
    memset(&link, 0, sizeof(link)); memset(&replacement, 0, sizeof(replacement)); memset(&sniff, 0, sizeof(sniff));
    lmp_acl_link->link = &link; lmp_acl_link->sniff = &sniff;
    lmp_acl_link->core.operation_mode = 2; link.sniff_pre_point_reset_flag = 1; link.sniff_state = 1;
    sniff.T_sniff = 10; sniff.timer = 7; sniff.wake_instant = 100; slot_clock = 20;
    clock_calls = timer_calls = logs = mutate_link = 0; at = 0; trace[0] = 0;
}
int main(void)
{
    for (unsigned value = 0; value < 256; ++value) {
        reset(); log_tag_const_i_LMP = value & 1;
        lmp_set_sniff_establish_by_remote(value);
        assert(sniff_establish_by_remote == value && logs == (int)(value & 1));
        for (unsigned policy = 0; policy < 8; ++policy) {
            hci_param.default_link_policy = policy; hci_param.scan_enable = 5; hci_param.need_enc = 1;
            lmp_set_sniff_disable();
            assert(hci_param.default_link_policy == (policy & ~4u) && hci_param.scan_enable == 5 && hci_param.need_enc);
        }
        for (int live = 0; live < 2; ++live) {
            reset(); lmp_acl_link->core.operation_mode = value;
            if (!live) lmp_acl_link->link = NULL;
            assert(lmp_in_sniff_mode(lmp_acl_link) == (live && value == 2));
            assert(!strcmp(trace, "DE"));
        }
    }
    reset(); assert(!lmp_in_sniff_mode(NULL) && !strcmp(trace, "DE"));
    for (int handle = 0; handle < 2; ++handle)
        for (int live = 0; live < 2; ++live)
            for (int active = 0; active < 2; ++active) {
                reset(); lmp_acl_link->core_s.handle = handle ? 0x30 : 0;
                if (!live) lmp_acl_link->link = NULL;
                link.sniff_sta = active;
                assert(get_1to2_conn_exit_sniff() == (handle && live && active) && !at);
            }
    /* Compare against the SDK loop for valid clocks and intervals. */
    for (unsigned interval = 1; interval <= 100; ++interval)
        for (unsigned delta = 0; delta <= 400; ++delta) {
            reset(); sniff.T_sniff = interval; sniff.wake_instant = slot_clock + delta;
            u32 expected = sniff.wake_instant;
            while (expected > slot_clock + interval) expected -= interval;
            if (expected - slot_clock < 3) expected += interval;
            lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
            assert(!strcmp(trace, "CDTPIE") && clock_calls == 1 && timer_calls == 1);
            assert(sniff.wake_instant == expected && timer_wake == expected && timer_delay == expected - slot_clock);
            assert(link.sniff_state == 4 && link.sniff_long_interval_filter == 2 && !link.sniff_pre_point_reset_flag);
        }
    for (unsigned mode = 0; mode < 256; ++mode)
        for (unsigned state = 0; state < 256; ++state) {
            reset(); lmp_acl_link->core.operation_mode = mode; link.sniff_state = state;
            lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
            assert(timer_calls == (mode == 2 && state == 1));
        }
    const u16 invalid[] = { 0, 101, 800, 65535 };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset(); sniff.T_sniff = invalid[i]; lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
        assert(!at && !link.sniff_long_interval_filter && link.sniff_state == 1);
    }
    reset(); lmp_sniff_pre_anchor_point_reset(NULL); assert(!at);
    reset(); lmp_acl_link->link = NULL; lmp_sniff_pre_anchor_point_reset(lmp_acl_link); assert(!at);
    reset(); lmp_acl_link->sniff = NULL; lmp_sniff_pre_anchor_point_reset(lmp_acl_link); assert(!at);
    reset(); link.sniff_pre_point_reset_flag = 0; lmp_sniff_pre_anchor_point_reset(lmp_acl_link); assert(!at);
    reset(); slot_clock = sniff.wake_instant + 1;
    lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
    assert(!strcmp(trace, "C") && link.sniff_long_interval_filter == 2 && link.sniff_state == 1 && link.sniff_pre_point_reset_flag);
    reset(); sniff.wake_instant = 0x7ffffff;
    lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
    assert(timer_delay > 0 && timer_delay <= 12 && !strcmp(trace, "CDTPIE"));
    reset(); slot_clock = 0xfffffffau; sniff.wake_instant = 0xffffffffu;
    lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
    assert(timer_delay == 5 && sniff.wake_instant == 0xffffffffu);
    reset(); mutate_link = 1; replacement.sniff_pre_point_reset_flag = 1;
    lmp_sniff_pre_anchor_point_reset(lmp_acl_link);
    assert(replacement.sniff_state == 4 && !replacement.sniff_pre_point_reset_flag && link.sniff_pre_point_reset_flag);
    puts("btctrler sniff support tests passed");
}
#endif
