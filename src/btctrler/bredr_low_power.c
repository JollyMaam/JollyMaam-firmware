/* The SDK's bredr_low_power.c: the Classic baseband's hooks into the
 * system's low-power manager.  bredr_power_off asks for sleep until a slot
 * clock value; the manager then asks how long that is (get_timeout, in us),
 * stops the baseband (suspend_probe/post), and after waking reloads the fine
 * counter (resume), waits for the baseband to come back and fires any slot
 * timer that expired meanwhile (resume_post).
 *
 * Kept as the SDK has it: the clock wraps at 2^27 and the wrap test is
 * "greater than", not "at least"; the sleep length comes from
 * bredr_clkn_after's very wide window; a fine count of 311 or 624 modulo 625
 * is decremented before it is loaded; all wait loops are unbounded. */
#include "btctrler/glue.h"

#define RF_CODE  __attribute__((section(".classic_rf_code")))
#define RF_CONST __attribute__((section(".classic_rf_const")))

void *low_power_get(void *priv, const struct low_power_operation *ops);
void low_power_put(void *priv);
void low_power_request(void);

#define CLKN_WRAP 134217728u

#ifdef UNITTEST
static volatile u32 test_bb[0x50 / 4], test_bt[0x80 / 4];
#define BB(off) (test_bb[(off) / 4])
#define BT(off) (test_bt[(off) / 4])
#else
#define BB(off) (*(volatile u32 *)(0x1c0000 + (off)))
#define BT(off) (*(volatile u32 *)(0x1cfc00 + (off)))
#endif

/* Microseconds until the requested wake-up slot, or 0. */
RF_CODE static u32 __power_get_timeout(void *priv)
{
    u32 now = READ_SLOT_CLK();
    if (bredr_link_v.edr_power_hd.interval == 0) return 0;
    u32 wake = ((struct bredr_power *)priv)->interval;
    if (!bredr_clkn_after(wake, now)) return 0;
    wake = ((struct bredr_power *)priv)->interval;
    return ((wake < now ? wake + CLKN_WRAP : wake) - now) * 625;
}

RF_CODE static void __power_suspend_probe(void *priv)
{
    (void)priv;
    BB(0x00) |= 0x400;
    BT(0x40) |= 0x400;
    BB(0x0c) |= 4;
    BB(0x30) |= 0x80000004;
}

RF_CODE static void __power_suspend_post(void *priv, u32 usec)
{
    (void)priv; (void)usec;
    while (!(BT(0x40) & 0x1000)) {}
    BB(0x0c) &= ~2u;
    BB(0x18) |= 0x200;
    while (BT(0x6c) & 0x800) {}
    BB(0x30) &= ~8u;
}

RF_CODE static void __power_resume(void *priv, u32 usec)
{
    (void)priv;
    if (!usec) btctrler_assert_failed("usec", 0);
    u32 fine = bt_get_fine_cnt(usec);
    if (fine % 625 == 624 || fine % 625 == 311) fine--;
    BB(0x44) = (fine / 625) << 1 | 0x80000000u;
    BB(0x40) = fine % 625;
}

RF_CODE static void __power_resume_post(void *priv, u32 usec)
{
    (void)priv; (void)usec;
    while (!(BB(0x10) & 4)) {}
    BT(0x70) |= 2;
    BT(0x40) |= 0x400;
    BB(0x30) |= 8;
    radio_set_eninv(0);
    BB(0x0c) |= 2;
    BB(0x00) |= 0x80000000u;
    while (BB(0x30) & 8) {}
    slot_timer_irq_get_time_out();
}

static const struct low_power_operation bredr_power_ops RF_CONST = {
    .name = "edr",
    .get_timeout = __power_get_timeout,
    .suspend_probe = __power_suspend_probe,
    .suspend_post = __power_suspend_post,
    .resume = __power_resume,
    .resume_post = __power_resume_post,
};

RF_CODE void bredr_power_get(void)
{
    bredr_link_v.edr_power_hd.power_ctrl = low_power_get(&bredr_link_v.edr_power_hd, &bredr_power_ops);
    bredr_link_v.edr_power_hd.interval = 0;
}

RF_CODE void bredr_power_put(void)
{
    if (bredr_link_v.edr_power_hd.power_ctrl) low_power_put(bredr_link_v.edr_power_hd.power_ctrl);
}

/* Sleep until slot clock now + slot, unless sleep is held off (state bit 0). */
RF_CODE void bredr_power_off(int slot)
{
    if (bredr_link_v.edr_power_hd.state & 1) return;
    u32 wake = READ_SLOT_CLK() + (u32)slot;
    bredr_link_v.edr_power_hd.interval = wake > CLKN_WRAP ? wake - CLKN_WRAP : wake;
    low_power_request();
}

RF_CODE void bredr_power_on(void)
{
    bredr_link_v.edr_power_hd.interval = 0;
}

RF_CODE void bredr_power_suspend(void) {}

RF_CODE void bredr_power_resume(void) {}

#ifdef UNITTEST
#include "test.h"
struct bredr_link_var bredr_link_v;
static u32 clk, fine_value, requests, puts_, asserts, eninv = 1, expired;
static const struct low_power_operation *got_ops;
static void *got_priv;
u32 READ_SLOT_CLK(void) { return clk; }
void *low_power_get(void *priv, const struct low_power_operation *ops) { got_priv = priv; got_ops = ops; return (void *)0x55; }
void low_power_put(void *priv) { CHECK(priv == (void *)0x55); puts_++; }
void low_power_request(void) { requests++; }
u32 bt_get_fine_cnt(u32 usec) { (void)usec; return fine_value; }
void radio_set_eninv(int v) { eninv = (u32)v; }
void slot_timer_irq_get_time_out(void) { expired++; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
/* The SDK's clock window (bredr_utils.c). */
int bredr_clkn_after(int a, int b)
{
    return (int)((a > b ? (u32)a : (u32)a + CLKN_WRAP) - (u32)b) < 69120000;
}

int main(void)
{
    bredr_link_v.edr_power_hd.interval = 99;
    bredr_power_get();
    CHECK(got_priv == &bredr_link_v.edr_power_hd && got_ops == &bredr_power_ops);
    CHECK(bredr_link_v.edr_power_hd.power_ctrl == (void *)0x55 && bredr_link_v.edr_power_hd.interval == 0);
    CHECK(got_ops->off_probe == 0 && got_ops->off_post == 0 && got_ops->on == 0);
    void *priv = got_priv;

    /* No wake-up requested: no timeout. */
    CHECK(got_ops->get_timeout(priv) == 0);

    /* Off: wake = now + slot, one wrap only above 2^27 (not at it). */
    clk = 1000;
    bredr_power_off(600);
    CHECK(bredr_link_v.edr_power_hd.interval == 1600 && requests == 1);
    clk = 300;
    CHECK(got_ops->get_timeout(priv) == 1300 * 625);
    clk = CLKN_WRAP - 10;
    bredr_power_off(10);
    CHECK(bredr_link_v.edr_power_hd.interval == CLKN_WRAP);
    bredr_power_off(11);
    CHECK(bredr_link_v.edr_power_hd.interval == 1);
    clk = CLKN_WRAP - 5;                         /* wake 1 is 6 slots ahead, across the wrap */
    CHECK(got_ops->get_timeout(priv) == 6 * 625);
    clk = 2;                                     /* wake 1 just passed: outside the window, 0 */
    CHECK(got_ops->get_timeout(priv) == 0);
    bredr_link_v.edr_power_hd.state = 1;         /* held off */
    bredr_power_off(50);
    CHECK(requests == 3 && bredr_link_v.edr_power_hd.interval == 1);
    bredr_power_on();
    CHECK(bredr_link_v.edr_power_hd.interval == 0);

    /* Suspend: the bits set and cleared, the waits satisfied up front. */
    got_ops->suspend_probe(priv);
    CHECK(BB(0x00) == 0x400 && BT(0x40) == 0x400 && BB(0x0c) == 4 && BB(0x30) == 0x80000004u);
    BT(0x40) |= 0x1000;
    BB(0x0c) |= 2;
    BB(0x30) |= 8;
    got_ops->suspend_post(priv, 5);
    CHECK(!(BB(0x0c) & 2) && BB(0x18) == 0x200 && !(BB(0x30) & 8));

    /* Resume: fine count split into slots (<<1, top bit) and remainder. */
    fine_value = 625 * 7 + 100;
    got_ops->resume(priv, 1000);
    CHECK(BB(0x44) == (0x80000000u | 14) && BB(0x40) == 100 && asserts == 0);
    fine_value = 625 * 3 + 624;                 /* 624 and 311 are pulled back one */
    got_ops->resume(priv, 1000);
    CHECK(BB(0x44) == (0x80000000u | 6) && BB(0x40) == 623);
    fine_value = 625 * 3 + 311;
    got_ops->resume(priv, 0);
    CHECK(BB(0x40) == 310 && asserts == 1);

    /* resume_post sets BB(0x30) bit 3 and waits for the hardware to clear
     * it, so it cannot run here; its register sequence is compared with the
     * SDK's on the target object instead. */
    (void)eninv; (void)expired;

    bredr_power_put();
    CHECK(puts_ == 1);
    bredr_link_v.edr_power_hd.power_ctrl = 0;
    bredr_power_put();
    CHECK(puts_ == 1);
    bredr_power_suspend();
    bredr_power_resume();
    return TEST_DONE("btctrler_bredr_low_power");
}
#endif
