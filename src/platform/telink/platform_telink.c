#include "sdk.h"
#include "board.h"

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

    while (plat_ms() - start < ms) {}
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

/* Polls until an event, deadline_ms (plat_ms) or at_us (plat_us); 0: none. */
u32 plat_wait(u32 deadline_ms, u32 at_us)
{
    u32 e;

    for (;;) {
        plat_wake(plat_usb_poll() | board_poll());
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

void plat_input(struct in_raw *raw)
{
    board_input(raw);
}

u16 plat_battery_mv(void)
{
    return 0;                            /* not read yet */
}

int plat_usb_present(void)
{
    return plat_usb_configured();
}

/* 32-bit words from addr: SRAM 0x840000-0x843fff, registers from 0x800000. */
u32 plat_peek(u32 addr, u8 *out, u32 max)
{
    u32 n = 0;

    if (addr & 3) return 0;
    for (; n + 4 <= max && n < 256; n += 4) util_put32(out + n, *(volatile u32 *)(addr + n));
    return n;
}

/* No sampling profiler on this chip. */
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

/* ---- resets -------------------------------------------------------------- */

void plat_reboot(void)
{
    plat_usb_settle(20);
    usb_dp_pullup_en(0);
    start_reboot();
}

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

_attribute_ram_code_ void irq_handler(void)
{
    blc_sdk_irq_handler();
}

int main(void)
{
    /* Keep the debug wire on D+ (cstartup_flash.S, DP_THROUGH_SWIRE_DIS). */
    usb_set_pin(1);
    cpu_wakeup_init(INTERNAL_CAP_XTAL24M);
    rf_drv_ble_init();
    clock_init(SYS_CLK_TYPE);
    usb_set_pin(1);
    last_tick = clock_time();
    flash_read_mid();
    flash_read_uid(0x4b, uid);           /* every flash the SDK supports on the B80 */
    util_log_init();
    util_log("boot: %s %s", board_name, BUILD_ID);
    board_init();
    plat_usb_init();
    irq_enable();
    ctl_main();
    return 0;
}
