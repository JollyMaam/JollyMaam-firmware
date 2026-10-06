/* The SDK's bredr_slot_timer.c: eight one-shot timers on the Classic
 * baseband's slot clock, each a compare register and a callback in
 * bredr_link_v.handle[].  The control register holds the enables (bits 0-7),
 * pending-clear bits (8-15, write 1) and the interrupt flags (16-23).  A
 * callback pointer with bit 0 set is a one-shot timer that frees its slot
 * after firing.
 *
 * Kept as the SDK has it: init clears 65 bytes, the eight handles and the
 * timer_used byte after them; slot_timer_remain accepts index 8; expiry is
 * judged on the low 16 bits of the clock.  Changed: the compare registers
 * are accessed as volatile (plain loads and stores in the SDK), and
 * slot_timer_irq_get_time_out's carried flag starts at 0 (undefined in the
 * SDK; it only matters if timer 0 is enabled between two reads of the
 * control register in one pass). */
#include "btctrler/glue.h"

#include <stdint.h>
#include <string.h>

#define RF_CODE __attribute__((section(".classic_rf_code")))

void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
void unrequest_irq(u8 index);
void log_print(int level, const char *tag, const char *format, ...);
extern const u8 log_tag_const_e_BL, log_tag_const_i_BL;

#define SLOT_TIMER_IRQ 28

#ifdef UNITTEST
static volatile u32 test_regs[9];
#define SLOT_CON (test_regs[0])
#define SLOT_CMP ((volatile u32 *)&test_regs[1])
#define RF_IRQ
#else
#define SLOT_CON (*(volatile u32 *)0x1cfd40)
#define SLOT_CMP ((volatile u32 *)0x1cfd44)
#define RF_IRQ __attribute__((interrupt("")))
#endif

/* Run timer i's callback if it is enabled: clear its pending bit and enable,
 * call, and free a one-shot slot. */
static inline void fire(int i)
{
    SLOT_CON |= 1u << (i + 8);
    if (!(SLOT_CON & 1u << i)) return;
    SLOT_CON &= ~(1u << i);
    uintptr_t func = (uintptr_t)bredr_link_v.handle[i].func;
    if (!func) return;
    ((void (*)(void *))(func & ~(uintptr_t)1))(bredr_link_v.handle[i].priv);
    if ((uintptr_t)bredr_link_v.handle[i].func & 1)
        bredr_link_v.timer_used &= (u8)~(1u << i);
}

RF_CODE RF_IRQ static void slot_timer_irq_handler(void)
{
    link_conn_super_timeout(0);
    for (int i = 0; i < 8; i++)
        if (SLOT_CON & 1u << (i + 16)) fire(i);
}

/* Fire every enabled timer whose compare value the clock has passed, judged
 * on the low 16 bits (bredr_low_power.c, after waking).  A disabled timer
 * after an expired one is tried too, which does nothing unless it became
 * enabled meanwhile. */
RF_CODE void slot_timer_irq_get_time_out(void)
{
    u32 now = READ_SLOT_CLK() & 0xffff;
    u8 timeout_flag = 0;

    for (int i = 0; i < 8; i++) {
        u32 bit = 1u << i;
        if (SLOT_CON & bit) {
            u32 cmp = SLOT_CMP[i] & 0xffff;
            timeout_flag = now > cmp ? (int)(now - cmp) < 32767 : (int)(cmp - now) > 32767;
            if (!timeout_flag) continue;
        } else if (!timeout_flag) {
            continue;
        }
        if (!(SLOT_CON & bit)) continue;
        SLOT_CON |= 1u << (i + 8);
        SLOT_CON &= ~bit;
        uintptr_t func = (uintptr_t)bredr_link_v.handle[i].func;
        if (!func) continue;
        ((void (*)(void *))(func & ~(uintptr_t)1))(bredr_link_v.handle[i].priv);
        if ((uintptr_t)bredr_link_v.handle[i].func & 1)
            bredr_link_v.timer_used &= (u8)~bit;
    }
}

RF_CODE void slot_timer_init(void)
{
    SLOT_CON = 0xff00;
    memset(bredr_link_v.handle, 0, sizeof(bredr_link_v.handle) + 1);   /* and timer_used */
    request_irq(SLOT_TIMER_IRQ, 2, slot_timer_irq_handler, 0);
}

RF_CODE void slot_timer_uninit(void)
{
    SLOT_CON = 0;
    unrequest_irq(SLOT_TIMER_IRQ);
}

/* A free timer index, or 0xff (after logging every slot and asserting). */
RF_CODE u8 slot_timer_get(void)
{
    u8 used = bredr_link_v.timer_used;
    for (int i = 0; i < 8; i++) {
        if (!(used & 1u << i)) {
            bredr_link_v.timer_used = (u8)(used | 1u << i);
            return (u8)i;
        }
    }
    for (int i = 0; i < 8; i++)
        if (log_tag_const_e_BL)
            log_print(4, NULL, "<Error>: [BL]%d, fun:0x%x\r\n", i, bredr_link_v.handle[i].func);
    btctrler_assert_failed("no_slot_timer", 0);
    return 0xff;
}

RF_CODE void slot_timer_info(void)
{
    if (log_tag_const_i_BL) {
        log_print(2, NULL, "[Info]: [BL]BT_CLKN_CON:0x%x\r\n", SLOT_CON);
        log_print(2, NULL, "[Info]: [BL]timer_used:0x%x\r\n", bredr_link_v.timer_used);
    }
    for (int i = 0; i < 8; i++)
        if (log_tag_const_i_BL)
            log_print(2, NULL, "[Info]: [BL]i:%d, 0x%x, 0x%x\r\n", i, bredr_link_v.handle[i].func,
                      bredr_link_v.handle[i].priv);
}

RF_CODE void slot_timer_set_ext(u8 i, void (*handler)(void *), void *priv, int slot, int ext_slot)
{
    bredr_link_v.handle[i].priv = priv;
    bredr_link_v.handle[i].func = handler;
    SLOT_CMP[i] = ext_slot + slot;
    SLOT_CON |= 0x100u << i;
    SLOT_CON |= 1u << i;
}

RF_CODE void slot_timer_set(u8 i, void (*handler)(void *), void *priv, int slot)
{
    slot_timer_set_ext(i, handler, priv, slot, READ_SLOT_CLK());
}

RF_CODE int slot_timer_get_func(u8 i)
{
    return (int)(uintptr_t)bredr_link_v.handle[i].func;
}

RF_CODE void slot_timer_reset(u8 i, int slot)
{
    SLOT_CMP[i] = READ_SLOT_CLK() + slot;
    SLOT_CON |= 0x100u << i;
    SLOT_CON |= 1u << i;
}

RF_CODE void slot_timer_put(u8 i)
{
    SLOT_CON |= 0x100u << i;
    SLOT_CON &= ~(1u << i);
    SLOT_CMP[i] = 0;
    bredr_link_v.timer_used &= (u8)~(1u << i);
}

RF_CODE void slot_timer_disable(u8 i)
{
    SLOT_CON &= ~(1u << i);
    SLOT_CON |= 0x100u << i;
}

RF_CODE void slot_timer_enable(u8 i)
{
    SLOT_CON |= 1u << i;
}

RF_CODE int slot_timer_remain(u8 i)
{
    if (i > 8) return 0;
    return SLOT_CMP[i] - READ_SLOT_CLK();
}

RF_CODE void *slot_timer_handler(u8 i)
{
    return bredr_link_v.handle[i].func;
}

RF_CODE int slot_timer_oneshort_set(void (*handler)(void *), void *priv, int slot)
{
    u8 i = slot_timer_get();
    if (i == 0xff) return -14;
    bredr_link_v.handle[i].priv = priv;
    bredr_link_v.handle[i].func = (void (*)(void *))((uintptr_t)handler | 1);
    SLOT_CMP[i] = READ_SLOT_CLK() + slot;
    SLOT_CON |= 0x100u << i;
    SLOT_CON |= 1u << i;
    return 0;
}

#ifdef UNITTEST
#include "test.h"
struct bredr_link_var bredr_link_v;
const u8 log_tag_const_e_BL = 0, log_tag_const_i_BL = 0;
static u32 clk;
static int super_calls, asserts, irq_index = -1, fired[8];
static void (*irq_handler)(void);
u32 READ_SLOT_CLK(void) { return clk; }
void link_conn_super_timeout(void *p) { (void)p; super_calls++; }
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id)
{ (void)priority; (void)cpu_id; irq_index = index; irq_handler = handler; }
void unrequest_irq(u8 index) { if (index == irq_index) irq_index = -1; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
static void cb(void *priv) { fired[(int)(long)priv]++; }

int main(void)
{
    bredr_link_v.timer_used = 0xff;
    bredr_link_v.esco_last_seqn = 0x5a;
    slot_timer_init();
    CHECK(SLOT_CON == 0xff00 && irq_index == SLOT_TIMER_IRQ);
    CHECK(bredr_link_v.timer_used == 0 && bredr_link_v.esco_last_seqn == 0x5a);   /* 65 bytes */

    /* Allocation: lowest free index; full asserts and returns 0xff. */
    CHECK(slot_timer_get() == 0 && slot_timer_get() == 1 && bredr_link_v.timer_used == 3);
    bredr_link_v.timer_used = 0xff;
    CHECK(slot_timer_get() == 0xff && asserts == 1);
    CHECK(slot_timer_oneshort_set(cb, 0, 1) == -14);
    bredr_link_v.timer_used = 0;

    /* set: compare = clock + slot, pending cleared, enabled. */
    SLOT_CON = 0;
    clk = 1000;
    slot_timer_set(2, cb, (void *)2, 50);
    CHECK(SLOT_CMP[2] == 1050 && SLOT_CON == (0x400u | 4u));
    CHECK(slot_timer_remain(2) == 50 && slot_timer_remain(9) == 0);
    CHECK(slot_timer_get_func(2) == (int)(uintptr_t)cb && slot_timer_handler(2) == (void *)cb);

    /* Interrupt for timer 2: callback runs once, enable cleared, slot kept. */
    bredr_link_v.timer_used = 4;
    SLOT_CON |= 1u << 18;
    irq_handler();
    CHECK(super_calls == 1 && fired[2] == 1 && !(SLOT_CON & 4) && bredr_link_v.timer_used == 4);
    SLOT_CON &= ~(1u << 18);

    /* One-shot: bit 0 marks it; firing frees the slot. */
    bredr_link_v.timer_used = 0;
    CHECK(slot_timer_oneshort_set(cb, (void *)0, 10) == 0);
    CHECK(((uintptr_t)bredr_link_v.handle[0].func & 1) && SLOT_CMP[0] == 1010 && bredr_link_v.timer_used == 1);
    SLOT_CON |= 1u << 16;
    irq_handler();
    CHECK(fired[0] == 1 && bredr_link_v.timer_used == 0);
    SLOT_CON &= ~(1u << 16);

    /* get_time_out: low 16 bits, half-range window, either direction of wrap. */
    memset(fired, 0, sizeof(fired));
    SLOT_CON = 0;
    clk = 0x12345;                               /* low half 0x2345 */
    slot_timer_set_ext(1, cb, (void *)1, 0, 0x2300);   /* passed */
    slot_timer_set_ext(3, cb, (void *)3, 0, 0x2400);   /* not yet */
    slot_timer_set_ext(5, cb, (void *)5, 0, 0xa400);   /* 0xa400-0x2345 > 32767: wrapped, passed */
    slot_timer_irq_get_time_out();
    CHECK(fired[1] == 1 && fired[3] == 0 && fired[5] == 1);
    CHECK(!(SLOT_CON & 2) && (SLOT_CON & 8) && !(SLOT_CON & 0x20));
    clk = 0x2400 + 32767;                        /* difference 32767: not passed (< 32767) */
    slot_timer_irq_get_time_out();
    CHECK(fired[3] == 0);
    clk = 0x2400 + 32766;
    slot_timer_irq_get_time_out();
    CHECK(fired[3] == 1);

    /* put / disable / enable / reset. */
    slot_timer_put(3);
    CHECK(SLOT_CMP[3] == 0 && !(SLOT_CON & 8) && (SLOT_CON & 0x800));
    SLOT_CON = 0;
    slot_timer_enable(4);
    CHECK(SLOT_CON == 0x10);
    slot_timer_disable(4);
    CHECK(SLOT_CON == 0x1000);
    clk = 7;
    slot_timer_reset(6, 3);
    CHECK(SLOT_CMP[6] == 10 && (SLOT_CON & 0x40) && (SLOT_CON & 0x4000));

    slot_timer_uninit();
    CHECK(SLOT_CON == 0 && irq_index == -1);
    return TEST_DONE("btctrler_bredr_slot_timer");
}
#endif
