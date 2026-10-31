#include "pad.h"
#include "yc3121.h"

/* YC3121 platform basics: the clock from SysTick (24 bits on the CPU clock,
 * counted down), the chip reset, and what utils.c asks for.  No interrupts
 * run, so the lock is empty and the clock advances only when read: the
 * main loop must call plat_ms() or plat_us() at least every 0.34 s. */

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
    uint32_t now = YC_SYST_CVR;

    clock.cycles += (clock.last - now) & 0xffffff;
    clock.last = now;
    clock.us += clock.cycles / YC_CPU_MHZ;
    clock.cycles %= YC_CPU_MHZ;
    return clock.us;
}

u32 plat_ms(void)
{
    uint32_t us = plat_us();

    clock.ms += (us - clock.ms_at) / 1000;
    clock.ms_at = us - (us - clock.ms_at) % 1000;
    return clock.ms;
}

void plat_delay_ms(u32 ms)
{
    u32 start = plat_us();

    while (plat_us() - start < ms * 1000)
        ;
}

void plat_lock(void) {}
void plat_unlock(void) {}

/* Out through the boot loader, as stock does (its 0x01021410): its mode
 * register to 2, then its reset handler, which starts the app again unless
 * the mailbox holds it.  RAM stays (but for the loader's own data), so the
 * log and a fault record carry over; a chip reset would clear them.  USB
 * drops first, so the host sees the device go. */
void plat_restart(int loader)
{
    *(volatile uint8_t *)0xf6000 = 0;   /* USB CONFIG: off the bus */
    plat_delay_ms(20);
    __asm__ volatile ("cpsid i");
    YC_SYST_CSR = 0;
    if (loader) {
        YC_LOADER_MAILBOX = YC_LOADER_STAY;
        YC_LOADER_MAILBOX2 = YC_LOADER_FROM_APP;
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

void plat_log_fault(void)
{
    if (fault.magic == FAULT_MAGIC)
        util_log("fault: exception %u at pc %08x lr %08x sp %08x", (unsigned)fault.ipsr,
                 (unsigned)fault.pc, (unsigned)fault.lr, (unsigned)fault.sp);
    fault.magic = 0;
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
