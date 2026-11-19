/* BD19 Classic sleep handshake and half-slot clock restoration. Register
 * sequencing differs from BR23, whose implementation stays in the parent dir. */
#include <stdio.h>
#include "btctrler/glue.h"

#define RF_CODE __attribute__((section(".classic_rf_code")))
#define CLKN_WRAP 134217728u
void *low_power_get(void *priv, const struct low_power_operation *ops);
void low_power_put(void *priv);
void low_power_request(void);
#ifdef UNITTEST
#include "test.h"
static u32 lp_read(u32 address);
static void lp_write(u32 address, u32 value);
static int lp_putchar(int c);
#define LP_READ(a) lp_read(a)
#define LP_WRITE(a, v) lp_write(a, v)
#define putchar lp_putchar
#else
#define LP_READ(a) (*(volatile u32 *)(uintptr_t)(a))
#define LP_WRITE(a, v) (LP_READ(a) = (u32)(v))
_Static_assert(sizeof(struct low_power_operation) == 24, "BD19 power callback table");
#endif

static RF_CODE u32 __power_get_timeout(void *priv)
{
    u32 now = READ_SLOT_CLK();
    if (!bredr_link_v.edr_power_hd.interval) return 0;
    struct bredr_power *power = priv;
    if (!bredr_clkn_after(power->interval, now)) return 0;
    u32 wake = power->interval;
    return ((wake < now ? wake + CLKN_WRAP : wake) - now) * 625;
}
static RF_CODE void __power_suspend_probe(void *priv)
{
    (void)priv;
    LP_WRITE(0x1c0020, LP_READ(0x1c0020) | 0x800);
    LP_WRITE(0x1c012c, LP_READ(0x1c012c) | 1);
}
static RF_CODE void __power_suspend_post(void *priv, u32 usec)
{
    (void)priv; (void)usec;
    while ((LP_READ(0x1c012c) & 6) != 4) {}
    while (LP_READ(0x1cfc6c) & 0x800) {}
}
static RF_CODE void __power_resume(void *priv, u32 usec)
{
    (void)priv;
    if (!usec) {
        if (config_asser) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/bredr_low_power.c", 104);
            printf("ASSERT-FAILD: usec ");
            cpu_assert_debug();
        } else p33_soft_reset();
    }
    (void)LP_READ(0x1c012c);
    u32 saved_fine = LP_READ(0x1c00bc);
    u32 fine = bt_get_fine_cnt(usec) + saved_fine;
    u32 remainder = fine % 1250;
    u32 low = LP_READ(0x1c00b4);
    u32 high = LP_READ(0x1c00b8);
    u32 clock = ((high << 16) + low) & ~3u;
    u32 phase = clkn0_phase((short)remainder);
    clock += ((fine / 1250) << 2) - 1 + phase;
    LP_WRITE(0x1c0120, clock & 65535);
    LP_WRITE(0x1c0124, clock >> 16);
    LP_WRITE(0x1c0128, remainder);
    LP_WRITE(0x1c0124, LP_READ(0x1c0124) | 0x8000);
    LP_WRITE(0x1c0128, LP_READ(0x1c0128) | 0x8000);
    (void)LP_READ(0x1c012c);
}
static RF_CODE void __power_resume_post(void *priv, u32 usec)
{
    (void)priv; (void)usec;
    while (!(LP_READ(0x1c0018) & 0x800)) {}
    LP_WRITE(0x1cfc70, LP_READ(0x1cfc70) | 2);
    LP_WRITE(0x1c0020, LP_READ(0x1c0020) | 0x800);
    slot_timer_irq_get_time_out();
    putchar('w');
}
static const struct low_power_operation bredr_power_ops
    __attribute__((section(".classic_rf_const"), aligned(4))) = {
    .name = "edr", .get_timeout = __power_get_timeout,
    .suspend_probe = __power_suspend_probe, .suspend_post = __power_suspend_post,
    .resume = __power_resume, .resume_post = __power_resume_post,
};
RF_CODE void bredr_power_get(void)
{
    bredr_link_v.edr_power_hd.power_ctrl = low_power_get(&bredr_link_v.edr_power_hd, &bredr_power_ops);
    bredr_link_v.edr_power_hd.interval = 0;
}
RF_CODE void bredr_power_put(void)
{
    if (bredr_link_v.edr_power_hd.power_ctrl) {
        low_power_put(bredr_link_v.edr_power_hd.power_ctrl);
        bredr_link_v.edr_power_hd.power_ctrl = NULL;
    }
}
RF_CODE void bredr_power_off(int slot)
{
    if (bredr_link_v.edr_power_hd.state & 1) return;
    u32 wake = READ_SLOT_CLK() + (u32)slot;
    bredr_link_v.edr_power_hd.interval = wake > CLKN_WRAP ? wake - CLKN_WRAP : wake;
    low_power_request();
}
RF_CODE void bredr_power_on(void) { bredr_link_v.edr_power_hd.interval = 0; }
RF_CODE void bredr_power_suspend(void) { bredr_link_v.edr_power_hd.state |= 1; }
RF_CODE void bredr_power_resume(void) { bredr_link_v.edr_power_hd.state &= (u8)~1u; }

#ifdef UNITTEST
#include <string.h>
struct bredr_link_var bredr_link_v;
int config_asser;
static u32 test_bb[0x130 / 4], test_bt[0x74 / 4 + 1];
static struct access { char op; u32 address, value; } accesses[64];
static unsigned access_count, wait_mode, control_reads, busy_reads, ready_reads;
static unsigned fine_calls, phase_calls, expired, prints, asserts, resets, requests, puts_;
static u32 clk, fine_value, phase_value, fine_usec;
static int phase_input, mutate_registers;
static struct bredr_power *mutate_interval;
static u32 new_interval;
static int after_result = 1;
static const struct low_power_operation *got_ops;
static void *got_priv;
static void record(char op, u32 address, u32 value)
{
    CHECK(access_count < sizeof accesses / sizeof accesses[0]);
    accesses[access_count++] = (struct access){ op, address, value };
}
static u32 *reg(u32 address)
{
    CHECK(!(address & 3));
    if (address >= 0x1c0000 && address < 0x1c0130) return test_bb + (address - 0x1c0000) / 4;
    CHECK(address >= 0x1cfc00 && address <= 0x1cfc74);
    return test_bt + (address - 0x1cfc00) / 4;
}
static u32 lp_read(u32 address)
{
    u32 value = *reg(address);
    if (wait_mode == 1 && address == 0x1c012c) {
        static const u32 values[] = {0, 6, 4};
        CHECK(control_reads < 3); value = values[control_reads++];
    }
    if (wait_mode == 1 && address == 0x1cfc6c) {
        CHECK(busy_reads < 2); value = busy_reads++ ? 0 : 0x800;
    }
    if (wait_mode == 2 && address == 0x1c0018) {
        CHECK(ready_reads < 2); value = ready_reads++ ? 0x800 : 0;
    }
    record('R', address, value); return value;
}
static void lp_write(u32 address, u32 value) { record('W', address, value); *reg(address) = value; }
static int lp_putchar(int c) { CHECK(c == 'w' && expired == 1); prints++; return c; }
void slot_timer_irq_get_time_out(void) { CHECK(access_count == 6); expired++; }
u32 READ_SLOT_CLK(void) { return clk; }
int bredr_clkn_after(int wake, int now)
{
    (void)wake; (void)now;
    if (mutate_interval) mutate_interval->interval = new_interval;
    return after_result;
}
u32 bt_get_fine_cnt(u32 usec)
{
    fine_calls++; fine_usec = usec;
    CHECK(access_count == 2);
    if (mutate_registers) *reg(0x1c00bc) = 9999;
    return fine_value;
}
unsigned char clkn0_phase(short phase)
{
    phase_calls++; phase_input = phase;
    CHECK(access_count == 4);
    if (mutate_registers) { *reg(0x1c00b4) = 0xdead; *reg(0x1c00b8) = 0xbeef; }
    return phase_value;
}
void cpu_assert_debug(void) { asserts++; }
void p33_soft_reset(void) { resets++; }
void *low_power_get(void *priv, const struct low_power_operation *ops)
{
    got_priv = priv; got_ops = ops; return (void *)0x55;
}
void low_power_put(void *p) { CHECK(p == (void *)0x55); puts_++; bredr_link_v.edr_power_hd.power_ctrl = (void *)0x66; }
void low_power_request(void) { requests++; }
static void clear_accesses(void) { access_count = 0; memset(accesses, 0, sizeof accesses); }
static void check_accesses(const struct access *want, unsigned count)
{
    CHECK(access_count == count);
    for (unsigned i = 0; i < count; i++) {
        CHECK(accesses[i].op == want[i].op);
        CHECK(accesses[i].address == want[i].address);
        CHECK(accesses[i].value == want[i].value);
    }
}
int main(void)
{
    bredr_power_get();
    CHECK(got_priv == &bredr_link_v.edr_power_hd && got_ops == &bredr_power_ops);
    CHECK(bredr_link_v.edr_power_hd.power_ctrl == (void *)0x55 && bredr_link_v.edr_power_hd.interval == 0);
    bredr_power_put(); bredr_power_put();
    CHECK(puts_ == 1 && bredr_link_v.edr_power_hd.power_ctrl == NULL);
    clk = CLKN_WRAP - 10; bredr_power_off(10);
    CHECK(bredr_link_v.edr_power_hd.interval == CLKN_WRAP);
    bredr_power_off(11); CHECK(bredr_link_v.edr_power_hd.interval == 1 && requests == 2);
    bredr_link_v.edr_power_hd.state = 0xa4;
    bredr_power_suspend(); CHECK(bredr_link_v.edr_power_hd.state == 0xa5);
    bredr_power_off(99); CHECK(requests == 2);
    bredr_power_resume(); CHECK(bredr_link_v.edr_power_hd.state == 0xa4);
    bredr_power_off(99); CHECK(requests == 3);
    clk = 300; bredr_link_v.edr_power_hd.interval = 1;
    struct bredr_power other = { .interval = 400 };
    mutate_interval = &other; new_interval = 1600;
    CHECK(got_ops->get_timeout(&other) == 1300 * 625);
    after_result = 0; CHECK(got_ops->get_timeout(&other) == 0); after_result = 1;
    mutate_interval = NULL; other.interval = 1; clk = CLKN_WRAP - 5;
    CHECK(got_ops->get_timeout(&other) == 6 * 625);
    bredr_power_on(); CHECK(got_ops->get_timeout(&other) == 0);
    *reg(0x1c0020) = 0x123; *reg(0x1c012c) = 0x20;
    clear_accesses(); got_ops->suspend_probe(NULL);
    static const struct access probe[] = {
        {'R',0x1c0020,0x123}, {'W',0x1c0020,0x923}, {'R',0x1c012c,0x20}, {'W',0x1c012c,0x21}
    };
    check_accesses(probe, sizeof probe / sizeof probe[0]);
    clear_accesses(); wait_mode = 1; got_ops->suspend_post(NULL, 0); wait_mode = 0;
    CHECK(access_count == 5 && control_reads == 3 && busy_reads == 2);
    CHECK(accesses[3].address == 0x1cfc6c && accesses[4].address == 0x1cfc6c);
    *reg(0x1c012c) = 0x55; *reg(0x1c00bc) = 200;
    *reg(0x1c00b4) = 0x1237; *reg(0x1c00b8) = 2;
    fine_value = 2301; phase_value = 2; mutate_registers = 1;
    clear_accesses(); got_ops->resume(NULL, 17); mutate_registers = 0;
    static const struct access resume[] = {
        {'R',0x1c012c,0x55}, {'R',0x1c00bc,200}, {'R',0x1c00b4,0x1237}, {'R',0x1c00b8,2},
        {'W',0x1c0120,0x123d}, {'W',0x1c0124,2}, {'W',0x1c0128,1},
        {'R',0x1c0124,2}, {'W',0x1c0124,0x8002}, {'R',0x1c0128,1}, {'W',0x1c0128,0x8001}, {'R',0x1c012c,0x55}
    };
    check_accesses(resume, sizeof resume / sizeof resume[0]);
    CHECK(fine_calls == 1 && fine_usec == 17 && phase_calls == 1 && phase_input == 1);
    /* Unsigned fine-counter addition wraps before division; quotient zero
     * and phase zero subtract one from the masked clock. */
    *reg(0x1c00bc) = 500; *reg(0x1c00b4) = *reg(0x1c00b8) = 0;
    fine_value = 0xffffffff; phase_value = 0;
    clear_accesses(); got_ops->resume(NULL, 1);
    CHECK(phase_input == 499 && *reg(0x1c0120) == 65535 && *reg(0x1c0124) == 65535 && *reg(0x1c0128) == (0x8000 | 499));
    for (config_asser = 0; config_asser <= 1; config_asser++) { clear_accesses(); got_ops->resume(NULL, 0); }
    CHECK(resets == 1 && asserts == 1);
    *reg(0x1cfc70) = 0x10; *reg(0x1c0020) = 0x20;
    clear_accesses(); wait_mode = 2; got_ops->resume_post(NULL, 0); wait_mode = 0;
    static const struct access post[] = {
        {'R',0x1c0018,0}, {'R',0x1c0018,0x800}, {'R',0x1cfc70,0x10}, {'W',0x1cfc70,0x12}, {'R',0x1c0020,0x20}, {'W',0x1c0020,0x820}
    };
    check_accesses(post, sizeof post / sizeof post[0]); CHECK(expired == 1 && prints == 1);
    return TEST_DONE("btctrler_bredr_low_power_bd19");
}
#endif
