#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"

/* YC3121 platform basics: the clock from SysTick (24 bits on the CPU clock,
 * counted down), the chip reset, and what utils.c asks for.  The clock
 * advances only when read: something must call plat_ms() or plat_us() at
 * least every 0.34 s. */

static struct {
    uint32_t last;               /* SysTick's count when last read */
    uint32_t cycles;             /* under a microsecond, carried */
    uint32_t us;
    uint32_t ms, ms_at;          /* ms whole at us ms_at: no jump when us wraps */
} clock;

void plat_clock_init(void)
{
    YC_LOADER_MODE = 1;                 /* faults to us, not the loader's endless loop */
    YC_SYST_RVR = 0xffffff;
    YC_SYST_CVR = 0;
    YC_SYST_CSR = 5;                    /* on, CPU clock, no interrupt */
    clock.last = YC_SYST_CVR;
}

u32 plat_us(void)
{
    uint32_t now, us;

    plat_lock();                        /* the USB interrupt reads it too */
    now = YC_SYST_CVR;
    clock.cycles += (clock.last - now) & 0xffffff;
    clock.last = now;
    clock.us += clock.cycles / YC_CPU_MHZ;
    clock.cycles %= YC_CPU_MHZ;
    us = clock.us;
    plat_unlock();
    return us;
}

u32 plat_ms(void)
{
    uint32_t us, ms;

    plat_lock();
    us = plat_us();
    clock.ms += (us - clock.ms_at) / 1000;
    clock.ms_at = us - (us - clock.ms_at) % 1000;
    ms = clock.ms;
    plat_unlock();
    return ms;
}

void plat_delay_ms(u32 ms)
{
    u32 start = plat_us();

    while (plat_us() - start < ms * 1000)
        ;
}

void yc_delay_us(uint32_t us)
{
    u32 start = plat_us();

    while (plat_us() - start < us)
        ;
}

/* Interrupts off, nesting; only USB's runs (it serves the tool device). */
static u32 lock_depth, lock_primask;

void plat_lock(void)
{
    u32 primask;

    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r" (primask) :: "memory");
    if (!lock_depth++) lock_primask = primask;
}

void plat_unlock(void)
{
    if (--lock_depth == 0 && !(lock_primask & 1)) __asm__ volatile ("cpsie i" ::: "memory");
}

/* Out through the boot loader, as stock does (its 0x01021410): its mode
 * register to 2, then its reset handler, which starts the app again unless
 * the mailbox holds it.  RAM stays (but for the loader's own data), so the
 * log and a fault record carry over; a chip reset would clear them.  USB
 * drops first (off the bus, its clock gated as stock does), so the host
 * sees the device go.  The Bluetooth core is halted, and the mailbox's
 * second word (stock's 0x20210624) is not set: with it the loader takes the
 * running core as its own instead of restarting it, and with ours running
 * it never came up on USB. */
void plat_restart(int loader)
{
    *(volatile uint8_t *)0xf6000 = 0;   /* USB CONFIG: off the bus */
    plat_delay_ms(20);
    plat_usb_drain();                   /* the loader would read it as its first SETUP */
    *(volatile uint8_t *)0xf6012 = 0x40;
    __asm__ volatile ("cpsid i");
    YC_SYST_CSR = 0;
    /* A SysTick still pending (the hang watch's) would go to the loader's
     * own handler, an endless loop, once it enables interrupts. */
    *(volatile uint32_t *)0xe000ed04 = 1u << 25;   /* ICSR: PENDSTCLR */
    /* Interrupts as a reset leaves them: the loader does not use ours (USB
     * is IRQ 0), and one still enabled and pending would go to its endless
     * handler once it enables interrupts (2 of 3 loader entries hung off USB
     * until a power cycle, 2026-11-04). */
    *(volatile uint32_t *)0xe000e180 = ~0u;        /* NVIC ICER */
    *(volatile uint32_t *)0xe000e280 = ~0u;        /* NVIC ICPR */
    *(volatile uint32_t *)0xf856c |= 1u << 6;   /* CLK_CLS: the USB clock off */
    *(volatile uint8_t *)0xf853c = 0x94;        /* WAKEUP_BT: the core halted */
    if (loader) {
        YC_LOADER_MAILBOX = YC_LOADER_STAY;
        YC_LOADER_MAILBOX2 = 0;
    }
    YC_LOADER_MODE = 2;
    ((void (*)(void))0x01000201)();
    for (;;)
        ;
}

static void fault_restart(void)
{
    plat_restart(0);
}

/* The last fault, for the next boot's log. */
static struct {
    u32 magic, ipsr, pc, lr, sp;
} fault __attribute__((section(".noinit")));
#define FAULT_MAGIC 0x544c5546

void yc_fault(const uint32_t *frame, uint32_t ipsr)
{
    fault.ipsr = ipsr;
    fault.pc = frame[6];
    fault.lr = frame[5];
    fault.sp = (u32)frame;
    fault.magic = FAULT_MAGIC;
    ((uint32_t *)frame)[6] = (uint32_t)fault_restart;   /* the exception returns there */
    ((uint32_t *)frame)[7] = 0x01000000;                /* Thumb */
}

/* The hang watch: SysTick's interrupt (each wrap of its 24 bits, 0.35 s at
 * 48 MHz) finds the wait loop's last pass more than HANG_MS ago, and the
 * restart goes as a fault's, with where it was.  Off until the first kick,
 * and between plat_watch_stop() and the next.  Proven on the U2CB: a 4 s
 * stall in the main loop restarted and logged as a hang (HANG_WATCH_SELFTEST).
 * The restart clears SysTick's pending interrupt, which the loader would
 * otherwise take into its own endless handler. */
#define HANG_MS 3000

static volatile u32 watch_ms;
static volatile u8 watch_on;

void plat_watch_kick(void)
{
    watch_ms = plat_ms();
    watch_on = 1;
}

void plat_watch_stop(void)
{
    watch_on = 0;
}

volatile u32 yc_watch_stale;     /* HANG_WATCH_DRYRUN: how stale it found the loop */

void yc_systick(uint32_t *frame)
{
    if (watch_on && plat_ms() - watch_ms > HANG_MS) {
        watch_on = 0;
#ifdef HANG_WATCH_DRYRUN
        yc_watch_stale = plat_ms() - watch_ms;
        (void)frame;
#else
        yc_fault(frame, 15);
#endif
    }
}

/* The last restart's fault or hang into the log; returns 15 after a hang,
 * another exception number after a fault, 0 for none. */
u32 plat_log_fault(void)
{
    u32 was = fault.magic == FAULT_MAGIC ? fault.ipsr : 0;

    if (fault.magic == FAULT_MAGIC)
        util_log("%s: exception %u at pc %08x lr %08x sp %08x", fault.ipsr == 15 ? "hang" : "fault",
                 (unsigned)fault.ipsr,
                 (unsigned)fault.pc, (unsigned)fault.lr, (unsigned)fault.sp);
    fault.magic = 0;
    return was;
}

void yc_reset(void)
{
    YC_RST_EN |= 1;
    YC_RESET = 0x55;
    for (;;)
        ;
}

/* newlib's heap (vsnprintf may take a little): from the end of .bss, up to
 * 8 KiB below the stack's top. */
void *_sbrk(int incr)
{
    extern char end[], __stack_top[];
    static char *brk = end;
    char *prev = brk;

    if ((uintptr_t)(brk + incr) > (uintptr_t)__stack_top - 8192) return (void *)-1;
    brk += incr;
    return prev;
}

/* ---- LPM register 1 -------------------------------------------------------- */

#define LPM_DATA(n)     (*(volatile uint8_t *)(0xc804c + (n)))
#define LPM_WRITE       (*(volatile uint8_t *)0xc8005)
#define LPM_CLK_CLS     (*(volatile uint32_t *)0xf856c)
#define LPM_CLK_BT      (1u << 13)
#define ROM_LPM_BT_READ ((uint32_t (*)(int))0x4d25)

void plat_lpm_release_pins(void)
{
    uint32_t v = ROM_LPM_BT_READ(1);

    if (!(v & 0x20000)) return;
    v &= ~0x20000u;
    LPM_CLK_CLS &= ~LPM_CLK_BT;
    for (int i = 0; i < 4; i++) {
        LPM_DATA(i) = (uint8_t)(v >> 8 * i);
        yc_delay_us(1);
    }
    LPM_WRITE = 1u << 1;
    plat_delay_ms(1);
}
