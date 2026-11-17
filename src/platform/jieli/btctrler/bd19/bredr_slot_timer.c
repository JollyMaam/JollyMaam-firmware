/* BD19 has sixteen half-slot compares and separate pending/enable/clear
 * registers. Its callbacks are direct pointers, without BR23's tag bit. */
#include <stdio.h>
#include <stdint.h>
#include "btctrler/glue.h"
#define RF_CODE __attribute__((section(".classic_rf_code")))
#define SLOT_PENDING 0x1c00a8u
#define SLOT_ENABLE 0x1c00acu
#define SLOT_CLEAR 0x1c00b0u
#define SLOT_COMPARE(i) (0x1c0068u + (u32)(i) * 4)
#define SLOT_FINE(i) (0x1c0028u + (u32)(i) * 4)
#define SLOT_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/bredr_slot_timer.c"
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);
void unrequest_irq(u8 index);
#ifdef UNITTEST
#include "test.h"
static u32 st_read(u32 address);
static void st_write(u32 address, u32 value);
static u32 st_rets(void);
#define ST_READ(a) st_read(a)
#define ST_WRITE(a, v) st_write(a, v)
#define RF_IRQ
#else
#define ST_READ(a) (*(volatile u32 *)(uintptr_t)(a))
#define ST_WRITE(a, v) (ST_READ(a) = (u32)(v))
#define RF_IRQ __attribute__((interrupt("")))
static __attribute__((always_inline)) inline u32 st_rets(void)
{
    u32 value;
    __asm__ volatile("%0 = rets ;" : "=r"(value));
    return value;
}
_Static_assert(sizeof(bredr_link_v.handle) + sizeof(bredr_link_v.timer_used) == 130, "BD19 timer state");
#endif
#define SLOT_ASSERT(condition, line, format, ...) do { \
    if (!(condition)) { \
        if (config_asser) { \
            printf("file:%s, line:%d", SLOT_FILE, (line)); \
            printf((format), __VA_ARGS__); \
            cpu_assert_debug(); \
        } else p33_soft_reset(); \
    } \
} while (0)

static RF_CODE RF_IRQ void slot_timer_irq_handler(void)
{
    for (unsigned i = 0; i < 16; i++) {
        u32 bit = 1u << i;
        if (!(ST_READ(SLOT_PENDING) & bit)) continue;
        ST_WRITE(SLOT_CLEAR, ST_READ(SLOT_CLEAR) | bit);
        if (!(ST_READ(SLOT_ENABLE) & bit)) continue;
        ST_WRITE(SLOT_ENABLE, ST_READ(SLOT_ENABLE) & ~bit);
        void (*handler)(void *) = bredr_link_v.handle[i].func;
        if (handler) handler(bredr_link_v.handle[i].priv);
    }
}
RF_CODE void slot_timer_irq_get_time_out(void)
{
    u32 now = (READ_SLOT_CLK() >> 1) & 65535;
    for (unsigned i = 0; i < 16; i++) {
        u32 bit = 1u << i;
        if (!(ST_READ(SLOT_ENABLE) & bit)) continue;
        u32 compare = ST_READ(SLOT_COMPARE(i)) & 65535;
        int expired = now > compare ? (int)(now - compare) < 32767 : (int)(compare - now) > 32767;
        if (!expired || !(ST_READ(SLOT_ENABLE) & bit)) continue;
        ST_WRITE(SLOT_CLEAR, ST_READ(SLOT_CLEAR) | bit);
        ST_WRITE(SLOT_ENABLE, ST_READ(SLOT_ENABLE) & ~bit);
        void (*handler)(void *) = bredr_link_v.handle[i].func;
        if (handler) handler(bredr_link_v.handle[i].priv);
    }
}
RF_CODE void slot_timer_init(void)
{
    ST_WRITE(SLOT_ENABLE, 0);
    __builtin_memset((u8 *)&bredr_link_v + offsetof(struct bredr_link_var, handle), 0,
                     sizeof bredr_link_v.handle + sizeof bredr_link_v.timer_used);
    request_irq(28, 2, slot_timer_irq_handler, 0);
}
RF_CODE void slot_timer_uninit(void) { ST_WRITE(SLOT_ENABLE, 0); unrequest_irq(28); }
RF_CODE u8 slot_timer_get(void)
{
    local_irq_disable();
    u16 used = bredr_link_v.timer_used;
    for (unsigned i = 0; i < 16; i++) {
        if (!(used & (1u << i))) {
            bredr_link_v.timer_used = (u16)(used | (1u << i));
            local_irq_enable();
            return (u8)i;
        }
    }
    local_irq_enable();
    SLOT_ASSERT(0, 145, "ASSERT-FAILD: 0 no_slot_timer: %x\n", bredr_link_v.timer_used);
    return 255;
}
RF_CODE void slot_timer_info(void) {}
RF_CODE void slot_timer_set_finecnt(u8 i, void (*handler)(void *), void *priv, int slot, short finecnt)
{
    SLOT_ASSERT(i < 16, 162, "ASSERT-FAILD: i < 16 finecnt: %d\n", i);
    bredr_link_v.handle[i].priv = priv;
    bredr_link_v.handle[i].func = handler;
    if (slot > 134217727) slot -= 134217728;
    ST_WRITE(SLOT_COMPARE(i), (u32)(slot / 2));
    ST_WRITE(SLOT_FINE(i), (u16)finecnt);
    ST_WRITE(SLOT_CLEAR, ST_READ(SLOT_CLEAR) | (1u << i));
    ST_WRITE(SLOT_ENABLE, ST_READ(SLOT_ENABLE) | (1u << i));
}
RF_CODE void slot_timer_set_ext(u8 i, void (*handler)(void *), void *priv, int slot, int ext_slot)
{
    int fine = (int)ST_READ(0x1c00bc);
    if (slot & 1) fine = (fine + 625) % 1250;
    slot_timer_set_finecnt(i, handler, priv, ext_slot + slot, (short)((u16)fine & 2047));
}
RF_CODE void slot_timer_set(u8 i, void (*handler)(void *), void *priv, int slot)
{
    u32 rets = st_rets();
    SLOT_ASSERT(i < 16, 182, "ASSERT-FAILD: i < 16 set: %d, %x\n", i, rets);
    slot_timer_set_ext(i, handler, priv, slot, READ_SLOT_CLK());
}
RF_CODE int slot_timer_get_func(u8 i) { return (int)(uintptr_t)bredr_link_v.handle[i].func; }
RF_CODE void slot_timer_reset(u8 i, int slot)
{
    SLOT_ASSERT(i < 16, 193, "ASSERT-FAILD: i < 16 reset: %d\n", i);
    slot_timer_set(i, bredr_link_v.handle[i].func, bredr_link_v.handle[i].priv, slot);
}
RF_CODE void slot_timer_put(u8 i)
{
    u32 rets = st_rets();
    SLOT_ASSERT(i < 16, 201, "ASSERT-FAILD: i < 16 put: %d, %x\n", i, rets);
    local_irq_disable();
    slot_timer_disable(i);
    bredr_link_v.timer_used &= (u16)~(1u << i);
    bredr_link_v.handle[i].func = NULL;
    local_irq_enable();
}
RF_CODE void slot_timer_disable(u8 i)
{
    u32 rets = st_rets();
    SLOT_ASSERT(i < 16, 217, "ASSERT-FAILD: i < 16 dis: %d, %x\n", i, rets);
    ST_WRITE(SLOT_ENABLE, ST_READ(SLOT_ENABLE) & ~(1u << i));
    ST_WRITE(SLOT_CLEAR, ST_READ(SLOT_CLEAR) | (1u << i));
}
RF_CODE void slot_timer_enable(u8 i)
{
    u32 rets = st_rets();
    SLOT_ASSERT(i < 16, 227, "ASSERT-FAILD: i < 16 enable: %d, %x\n", i, rets);
    ST_WRITE(SLOT_ENABLE, ST_READ(SLOT_ENABLE) | (1u << i));
}
RF_CODE int slot_timer_is_enable(u8 i) { return (ST_READ(SLOT_ENABLE) & (1u << i)) != 0; }
RF_CODE int slot_timer_remain(u8 i)
{
    if (i > 16) return 0;
    u32 compare = (ST_READ(SLOT_COMPARE(i)) << 1) & 65534;
    int left = (int)compare - (int)(READ_SLOT_CLK() & 65535);
    return left < -32768 ? left + 65535 : left;
}
RF_CODE void *slot_timer_handler(u8 i) { return (void *)(uintptr_t)bredr_link_v.handle[i].func; }

#ifdef UNITTEST
#include <string.h>
#include <setjmp.h>
struct bredr_link_var bredr_link_v;
int config_asser;
static u32 regs[0xc0 / 4];
static struct access { char op; u32 address, value; } accesses[256];
static unsigned naccess, irq_depth, disables, enables, assertions, resets, rets_reads;
static unsigned calls[16], order[16], norder, clock_reads;
static int clear_on_compare = -1, pending_from_callback, jump_on_failure;
static u32 clock_value;
static void (*registered_irq)(void);
static jmp_buf fail_jmp;
static u32 *reg(u32 address) { CHECK(address >= 0x1c0000 && address < 0x1c00c0 && !(address & 3)); return regs + (address - 0x1c0000) / 4; }
static void record(char op, u32 address, u32 value) { CHECK(naccess < sizeof accesses / sizeof accesses[0]); accesses[naccess++] = (struct access){op,address,value}; }
static u32 st_read(u32 address)
{
    u32 value = *reg(address);
    if (clear_on_compare >= 0 && address == SLOT_COMPARE(clear_on_compare)) *reg(SLOT_ENABLE) &= ~(1u << clear_on_compare);
    record('R', address, value); return value;
}
static void st_write(u32 address, u32 value) { record('W', address, value); *reg(address) = value; }
static u32 st_rets(void) { rets_reads++; return 0x12345678; }
void local_irq_disable(void) { irq_depth++; disables++; }
void local_irq_enable(void) { CHECK(irq_depth > 0); irq_depth--; enables++; }
void cpu_assert_debug(void) { assertions++; if (jump_on_failure) longjmp(fail_jmp, 1); }
void p33_soft_reset(void) { resets++; if (jump_on_failure) longjmp(fail_jmp, 1); }
u32 READ_SLOT_CLK(void) { clock_reads++; return clock_value; }
void request_irq(u8 i, u8 priority, void (*handler)(void), u8 cpu) { CHECK(i == 28 && priority == 2 && cpu == 0); registered_irq = handler; }
void unrequest_irq(u8 i) { CHECK(i == 28); registered_irq = NULL; }
static void callback(void *priv)
{
    unsigned i = (unsigned)(uintptr_t)priv;
    CHECK(i < 16 && norder < 16 && !(*reg(SLOT_ENABLE) & (1u << i)));
    calls[i]++; order[norder++] = i;
    if (pending_from_callback && i == 1) *reg(SLOT_PENDING) |= 1u << 3;
}
static void clear_trace(void) { naccess = 0; }
int main(void)
{
    memset(&bredr_link_v, 0xa5, sizeof bredr_link_v);
    bredr_link_v.fcc_lc = (void *)0x55;
    slot_timer_init();
    CHECK(registered_irq && *reg(SLOT_ENABLE) == 0 && bredr_link_v.timer_used == 0);
    for (unsigned i = 0; i < 16; i++) CHECK(!bredr_link_v.handle[i].priv && !bredr_link_v.handle[i].func);
    CHECK(bredr_link_v.fcc_lc == (void *)0x55);
    CHECK(bredr_link_v.esco_last_seqn == 0xa5);
    for (unsigned i = 0; i < 16; i++) CHECK(slot_timer_get() == i);
    CHECK(bredr_link_v.timer_used == 65535 && disables == 16 && enables == 16 && irq_depth == 0);
    for (config_asser = 0; config_asser <= 1; config_asser++) CHECK(slot_timer_get() == 255);
    CHECK(assertions == 1 && resets == 1 && irq_depth == 0);
    clear_trace(); *reg(SLOT_CLEAR) = 0; *reg(SLOT_ENABLE) = 0;
    slot_timer_set_finecnt(15, callback, (void *)15, 134217728, -1);
    CHECK(bredr_link_v.handle[15].func == callback && bredr_link_v.handle[15].priv == (void *)15);
    CHECK(naccess == 6 && accesses[0].address == SLOT_COMPARE(15) && accesses[0].value == 0 && accesses[1].address == SLOT_FINE(15) && accesses[1].value == 65535);
    CHECK(accesses[2].op == 'R' && accesses[2].address == SLOT_CLEAR && accesses[3].op == 'W' && accesses[4].address == SLOT_ENABLE && accesses[5].op == 'W');
    clear_trace(); slot_timer_set_finecnt(0, callback, NULL, -3, 0);
    CHECK(*reg(SLOT_COMPARE(0)) == 0xffffffff); /* signed division truncates toward zero */
    *reg(0x1c00bc) = 1000;
    clear_trace(); slot_timer_set_ext(1, callback, (void *)1, 3, 100);
    CHECK(accesses[0].address == 0x1c00bc && *reg(SLOT_COMPARE(1)) == 51 && *reg(SLOT_FINE(1)) == 375);
    clock_value = 100; slot_timer_set(2, callback, (void *)2, 4);
    CHECK(*reg(SLOT_COMPARE(2)) == 52 && *reg(SLOT_FINE(2)) == 1000);
    slot_timer_reset(2, 6); CHECK(*reg(SLOT_COMPARE(2)) == 53 && bredr_link_v.handle[2].func == callback && bredr_link_v.handle[2].priv == (void *)2);
    CHECK(slot_timer_get_func(2) == (int)(uintptr_t)callback && slot_timer_handler(2) == (void *)(uintptr_t)callback);
    slot_timer_disable(2); CHECK(!slot_timer_is_enable(2));
    slot_timer_enable(2); CHECK(slot_timer_is_enable(2));
    slot_timer_put(2); CHECK(!(bredr_link_v.timer_used & 4) && !bredr_link_v.handle[2].func && bredr_link_v.handle[2].priv == (void *)2);
    CHECK(slot_timer_get() == 2);
    /* Pending is reread for each index: callback 1 makes timer 3 pending. */
    *reg(SLOT_PENDING) = 1u << 1; *reg(SLOT_ENABLE) = (1u << 1) | (1u << 3);
    bredr_link_v.handle[3].func = callback; bredr_link_v.handle[3].priv = (void *)3;
    pending_from_callback = 1; clear_trace(); registered_irq(); pending_from_callback = 0;
    CHECK(norder == 2 && order[0] == 1 && order[1] == 3 && bredr_link_v.timer_used == 65535);
    CHECK(!(*reg(SLOT_ENABLE) & 10));
    /* The timeout path rechecks enable after reading compare. */
    clock_value = 202; *reg(SLOT_COMPARE(1)) = 100; *reg(SLOT_ENABLE) = 2;
    clear_on_compare = 1; clear_trace(); slot_timer_irq_get_time_out(); clear_on_compare = -1;
    CHECK(calls[1] == 1 && *reg(SLOT_ENABLE) == 0);
    *reg(SLOT_ENABLE) = 2; clear_trace(); slot_timer_irq_get_time_out(); CHECK(calls[1] == 2);
    clock_value = 200; *reg(SLOT_ENABLE) = 2; clear_trace(); slot_timer_irq_get_time_out(); CHECK(calls[1] == 2); /* equal is not expired */
    clock_value = 2; *reg(SLOT_COMPARE(1)) = 65535; *reg(SLOT_ENABLE) = 2; clear_trace(); slot_timer_irq_get_time_out(); CHECK(calls[1] == 3); /* 16-bit wrap */
    *reg(SLOT_COMPARE(0)) = 0; clock_value = 32768; CHECK(slot_timer_remain(0) == -32768);
    clock_value = 32769; CHECK(slot_timer_remain(0) == 32766); /* SDK adds 65535, not 65536 */
    *reg(SLOT_PENDING) = 0x1234; clock_value = 0x2000; CHECK(slot_timer_remain(16) == 0x468); /* SDK allows 16 */
    unsigned old_reads = clock_reads; clear_trace(); CHECK(slot_timer_remain(17) == 0 && naccess == 0 && clock_reads == old_reads);
    jump_on_failure = 1;
    for (config_asser = 0; config_asser <= 1; config_asser++) {
        unsigned before = rets_reads;
        if (!setjmp(fail_jmp)) { slot_timer_enable(16); CHECK(0); }
        CHECK(rets_reads == before + 1 && irq_depth == 0);
    }
    jump_on_failure = 0;
    clear_trace(); slot_timer_info(); CHECK(naccess == 0);
    slot_timer_uninit(); CHECK(!registered_irq && *reg(SLOT_ENABLE) == 0);
    return TEST_DONE("btctrler_bredr_slot_timer_bd19");
}
#endif
