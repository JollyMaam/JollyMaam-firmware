#include "sdk.h"
#include "board.h"
#include "store.h"
#ifdef HAVE_BUILD_ID_H
#include "build_id.h"
#endif

/* The Telink B80: start-up, time, the wait loop, resets.  One thread: the
 * control loop's plat_wait() runs everything that polls (USB, the board's
 * input scanner) until an event or its deadline.  Interrupts only for what
 * the SDK's radio needs. */

/* ---- time ---------------------------------------------------------------
 * The system timer counts at 16 MHz and wraps in 268 s; time_update() runs
 * on every pass of the wait loop, far more often than that. */

static u32 last_tick, now_us, us_ticks, now_ms, ms_us;

static void time_update(void)
{
    u32 t = clock_time(), whole;

    us_ticks += t - last_tick;
    last_tick = t;
    whole = us_ticks / sys_tick_per_us;
    us_ticks -= whole * sys_tick_per_us;
    now_us += whole;
    ms_us += whole;
    if (ms_us >= 1000) {
        now_ms += ms_us / 1000;
        ms_us %= 1000;
    }
}

u32 plat_us(void)
{
    time_update();
    return now_us;
}

u32 plat_ms(void)
{
    time_update();
    return now_ms;
}

void plat_delay_ms(u32 ms)
{
    u32 start = plat_ms();

    while (plat_ms() - start < ms) wd_clear();
}

static u8 lock_saved, lock_depth;

void plat_lock(void)
{
    u8 r = irq_disable();

    if (!lock_depth++) lock_saved = r;
}

void plat_unlock(void)
{
    if (lock_depth && !--lock_depth) irq_restore(lock_saved);
}

/* ---- the stack ------------------------------------------------------------
 * The RAM between the end of .bss and the stack is painted at start-up; what
 * stays painted was never used.  The log says so when it shrinks. */

#define PAINT 0x5a5a5a5au

extern u32 _end_bss_[];
static u32 stack_low;            /* the least unused seen, bytes */

static void stack_paint(void)
{
    volatile u32 *p = (u32 *)(((u32)_end_bss_ + 3) & ~3u);
    volatile u32 *sp = (u32 *)__builtin_frame_address(0);

    while (p < sp - 32) *p++ = PAINT;
    stack_low = 0xffffffffu;
}

static void stack_check(void)
{
    const u32 *p = (const u32 *)(((u32)_end_bss_ + 3) & ~3u);
    u32 n = 0;

    while (p[n] == PAINT) n++;
    if (4 * n < stack_low) {
        stack_low = 4 * n;
        util_log("stack: %u bytes never used", stack_low);
    }
}

/* ---- the wait loop ------------------------------------------------------ */

static volatile u32 pending;

void plat_wake(u32 events)
{
    plat_lock();
    pending |= events;
    plat_unlock();
}

static u32 take(void)
{
    u32 e;

    plat_lock();
    e = pending;
    pending = 0;
    plat_unlock();
    return e;
}

/* Polls until an event, deadline_ms (plat_ms) or at_us (plat_us); 0: none.
 * The hang watch: the watchdog resets the chip when no pass of this loop
 * (or of a wait in plat_delay_ms, plat_usb_settle) comes for HANG_MS. */
#define HANG_MS 3000

u32 plat_wait(u32 deadline_ms, u32 at_us)
{
    u32 e;

    for (;;) {
        wd_clear();
        plat_wake(plat_usb_poll() | board_poll(plat_bt_jit() != 0) | plat_bt_poll());
        if ((now_ms & 0x3ff) == 0) stack_check();
        e = take();
        if (e) return e;
        if (deadline_ms && (s32)(plat_ms() - deadline_ms) >= 0) return EV_DEADLINE;
        if (at_us && (s32)(plat_us() - at_us) >= 0) return EV_DEADLINE;
    }
}

/* ---- identity, inputs, the tools' extras -------------------------------- */

static u8 uid[16];

const u8 *plat_unique_id(void)
{
    return uid;
}

/* A report about to be built reads the sticks for itself. */
void plat_input(struct in_raw *raw)
{
    u32 lead = plat_bt_jit();

    if (lead && plat_bt_input_window()) board_sweep(lead);
    board_input(raw);
}

/* The board owns the sensor, voltage scale and charging-status pins. */
void plat_battery(struct battery_status *out)
{
    board_battery(out);
}

/* The SDK's 32k-RC sleep entry stops the 32k watchdog through this (from
 * its drivers/watchdog.c, whose RAM-code copies we do not link: RAM is
 * full and the code runs from flash). */
__attribute__((section(".text.wd_32k_stop"))) void wd_32k_stop(void)
{
    analog_write(0x70, analog_read(0x70) & 0xfe);
}

/* Slot retention is independent of the board's sleep policy. Analog
 * registers survive deep sleep, but not the board losing its power hold.
 * Flash uses the former last-slot record format, only when opted in. */
#if defined(U2CW_LAST_SLOT_ANALOG) && defined(U2CW_LAST_SLOT_FLASH)
#error "Select only one last-slot retention method"
#endif

#ifdef U2CW_LAST_SLOT_ANALOG
#define SLOT_REG       0x38
#define SLOT_CHECK_REG 0x39
#define SLOT_CHECK(s)  ((u8)(~(s) ^ 0x5a))

/* Power-on values 0x00/0xff must not look like a retained slot A. */
u8 plat_retained_slot(void)
{
    u8 slot = analog_read(SLOT_REG);

    return slot < SLOTS && analog_read(SLOT_CHECK_REG) == SLOT_CHECK(slot) ? slot : SLOT_NONE;
}

void plat_retain_slot(u8 slot)
{
    if (slot >= SLOTS) return;
    analog_write(SLOT_REG, slot);
    analog_write(SLOT_CHECK_REG, SLOT_CHECK(slot));
}
#elif defined(U2CW_LAST_SLOT_FLASH)
#define LAST_SLOT_KEY STORE_LAST_SLOT_KEY
static u8 retained_slot = SLOT_NONE, retained_loaded;

/* Load the legacy record once; unchanged selections do not wear flash. */
u8 plat_retained_slot(void)
{
    u8 record[STORE_LAST_SLOT_SIZE];

    if (!retained_loaded) {
        retained_loaded = 1;
        if (plat_store_read(LAST_SLOT_KEY, record, sizeof(record)) == sizeof(record) &&
            record[0] == 'S' && record[1] == 1 && record[2] < SLOTS &&
            record[3] == (u8)~record[2]) retained_slot = record[2];
    }
    return retained_slot;
}

void plat_retain_slot(u8 slot)
{
    u8 record[STORE_LAST_SLOT_SIZE] = { 'S', 1, slot, (u8)~slot };

    if (slot >= SLOTS || slot == plat_retained_slot()) return;
    if (plat_store_write(LAST_SLOT_KEY, record, sizeof(record)) == sizeof(record))
        retained_slot = slot;
    else util_log("slot: could not save last selection");
}
#else
/* No register or flash access; only the controller's current RAM remembers. */
u8 plat_retained_slot(void) { return SLOT_NONE; }
void plat_retain_slot(u8 slot) { (void)slot; }
#endif

/* 32-bit words from addr: SRAM 0x840000-0x843fff, registers from 0x800000;
 * PEEK_OTP + n: the OTP at n (0-0x3ffc), which is not on the bus. */
#define PEEK_OTP 0xf0000000u

u32 plat_peek(u32 addr, u8 *out, u32 max)
{
    u32 n = 0, w;

    if (addr & 3) return 0;
    if (addr >= PEEK_OTP) {
        addr -= PEEK_OTP;
        otp_set_active_mode();
        for (; n + 4 <= max && n < 256 && addr + n <= 0x3ffc; n += 4) {
            otp_read(addr + n, 1, &w);
            util_put32(out + n, w);
        }
        otp_set_deep_standby_mode();
        return n;
    }
    for (; n + 4 <= max && n < 256; n += 4) util_put32(out + n, *(volatile u32 *)(addr + n));
    return n;
}

/* ---- sampling profiler ---------------------------------------------------
 * Test builds only (PROF_RING): no RAM for counts, so timer 1 drops each
 * interrupted address into a ring in the spare .data below the cache tags,
 * and padtool profile drains it over USB as it fills (read: version 2, then
 * the addresses).  The period is jittered against the connection events;
 * code that runs with interrupts off is counted where it turns them on.
 * Only the check for the timer is RAM code: the flash's own operations run
 * with interrupts off, so the rest can stay in flash. */

#ifdef PROF_RING
#define PROF_VERSION 2

static struct {
    u32 head, tail, lost, period;        /* addresses [tail, head) not read yet */
    u16 hz, rng;
    u16 pc[PROF_RING];                   /* address >> 2: all code is below 0x40000 */
} prof __attribute__((section(".data")));

/* __irq (cstartup_flash.S) pushes the address it returns to first, then 56
 * bytes more, and calls here: that address is the code interrupted. */
__asm__(".section .ram_code,\"ax\",%progbits\n"
        "\t.align 2\n\t.global irq_handler\n\t.code 16\n\t.tc32_func\n"
        "\t.type irq_handler, %function\n"
        "irq_handler:\n"
        "\ttmov r0, sp\n"
        "\ttloadr r0, [r0, #56]\n"
        "\ttpush {lr}\n"
        "\ttjl prof_irq\n"
        "\ttpop {pc}\n"
        "\t.size irq_handler, .-irq_handler\n"
        "\t.text\n");

static __attribute__((noinline)) void prof_sample(u32 pc)
{
    reg_tmr_sta = FLD_TMR_STA_TMR1;
    prof.rng = (u16)((prof.rng >> 1) ^ (-(prof.rng & 1u) & 0xb400u));
    reg_tmr1_capt = prof.period + (prof.rng & 0x3ff);
    if (prof.head - prof.tail < PROF_RING)
        prof.pc[prof.head++ % PROF_RING] = (u16)(pc < 0x40000 ? pc >> 2 : 0xffff);
    else
        prof.lost++;
}

_attribute_ram_code_ void prof_irq(u32 pc)
{
    u32 timer = reg_irq_src & FLD_IRQ_SYSTEM_TIMER, fired = reg_system_tick_irq_level;

    if (reg_tmr_sta & FLD_TMR_STA_TMR1) prof_sample(pc);
    blc_sdk_irq_handler();
    if (timer) plat_bt_systimer(fired);
}

void plat_prof_start(u16 hz, u32 zoom, u8 zoom_shift)
{
    (void)zoom;
    (void)zoom_shift;
    timer_stop(TIMER1);
    reg_irq_mask &= ~FLD_IRQ_TMR1_EN;
    if (!hz) return;
    if (hz < 400) hz = 400;
    prof.head = prof.tail = prof.lost = 0;
    prof.rng = 0xace1u;
    prof.hz = hz;
    prof.period = CLOCK_SYS_CLOCK_HZ / hz - 512;
    timer1_set_mode(TIMER_MODE_SYSCLK, 0, prof.period);
    timer_start(TIMER1);
}

/* u16 version, u16 hz, u32 addresses so far, u32 lost, u32 count, then count
 * u16 addresses >> 2 (0xffff: elsewhere), the oldest unread first; reading
 * them frees their room. */
u32 plat_prof_read(u32 offset, u8 *out, u32 max)
{
    u32 head = prof.head, tail = prof.tail, n = 0;

    (void)offset;
    if (max < 16) return 0;
    for (; tail != head && 16 + 2 * (n + 1) <= max; n++, tail++)
        util_put16(out + 16 + 2 * n, prof.pc[tail % PROF_RING]);
    prof.tail = tail;
    util_put16(out, PROF_VERSION);
    util_put16(out + 2, prof.hz);
    util_put32(out + 4, head);
    util_put32(out + 8, prof.lost);
    util_put32(out + 12, n);
    return 16 + 2 * n;
}
#else
/* No profiler in this build (PROF_RING). */
void plat_prof_start(u16 hz, u32 zoom, u8 zoom_shift)
{
    (void)hz;
    (void)zoom;
    (void)zoom_shift;
}

u32 plat_prof_read(u32 offset, u8 *out, u32 max)
{
    (void)offset;
    (void)out;
    (void)max;
    return 0;
}
#endif

/* ---- resets -------------------------------------------------------------- */

void plat_power_off(void)
{
    if (board_usb_present()) return;      /* a cable arrived during shutdown */
    board_power_off();
}

void plat_reboot(void)
{
    plat_usb_settle(20);
    usb_dp_pullup_en(0);
    start_reboot();
}

/* Into the updater, where the app has one (board.h); without, a reboot. */
void plat_reboot_updater(void)
{
    analog_write(UPDATER_STAY_REG, UPDATER_STAY);
    plat_reboot();
}

/* No config mode here to restart into. */
void plat_reboot_config(void) { plat_reboot(); }
int plat_config_asked(void) { return 0; }

/* The 8BitDo boot loader stays in its USB loader after a reset when analog
 * register 0x3c holds 0x5a (it survives the reset). */
void plat_reboot_loader(void)
{
    plat_usb_settle(20);
    irq_disable();
    analog_write(0x3c, 0x5a);
    if (analog_read(0x3c) != 0x5a) {
        util_log("loader: the boot flag did not stick");
        return;
    }
    usb_dp_pullup_en(0);
    start_reboot();
}

/* ---- start-up ------------------------------------------------------------ */

#ifndef PROF_RING
/* The system timer runs the radio's connection events: what it fired at
 * and set next tell bt_telink.c when the next one starts. */
_attribute_ram_code_ void irq_handler(void)
{
    u32 timer = reg_irq_src & FLD_IRQ_SYSTEM_TIMER, fired = reg_system_tick_irq_level;

    blc_sdk_irq_handler();
    if (timer) plat_bt_systimer(fired);
}
#endif

int main(void)
{
#ifdef UPDATER
    updater_boot();
#else                                    /* a hang before the hang watch resets too, and counts */
    wd_set_interval_ms(HANG_MS, CLOCK_SYS_CLOCK_1MS);   /* the clock not set up yet: roughly */
    wd_start();
#endif
    /* Keep the debug wire on D+ (cstartup_flash.S, DP_THROUGH_SWIRE_DIS). */
    usb_set_pin(1);
    cpu_wakeup_init(INTERNAL_CAP_XTAL24M);
    rf_drv_ble_init();
    clock_init(SYS_CLK_TYPE);
    usb_set_pin(1);
    stack_paint();
    last_tick = clock_time();
    flash_read_mid();
    flash_read_uid(0x4b, uid);           /* every flash the SDK supports on the B80 */
    util_log_init();
    util_log("boot: %s %s", board_name, BUILD_ID);
    plat_bt_init();
    board_init();
#ifdef BOOT_HANG_TEST                    /* 1: 10 s before the hang watch, under main's watchdog */
    if (BOOT_HANG_TEST == 1) {
        u32 at = clock_time();

        while (!clock_time_exceed(at, 10000000)) {
        }
        util_log("boot: no reset in 10 s");
    }
#endif
#ifndef UPDATER
    {   /* an updater chord held with USB at start-up: there before USB comes up */
        struct plat_boot b;

        plat_boot_info(&b);
        if (board_usb_present() && ux_updater_chord(b.buttons_held)) {
            analog_write(UPDATER_STAY_REG, UPDATER_STAY);
            start_reboot();
        }
    }
#endif
    wd_set_interval_ms(HANG_MS, CLOCK_SYS_CLOCK_1MS);
    wd_start();
    plat_usb_init();
    irq_enable();
#ifdef BOOT_HANG_TEST                    /* 2: a hang under the hang watch */
    if (BOOT_HANG_TEST == 2) for (;;) {
    }
#endif
#ifndef UPDATER                          /* the app: up, the updater's count of its starts ends */
    analog_write(UPDATER_STAY_REG, UPDATER_UP);
#endif
    ctl_main();
    return 0;
}
