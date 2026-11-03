#include <string.h>
#include "pad.h"
#include "store.h"
#include "yc3121.h"
#include "usb_yc3121.h"
#include "board.h"

/* The YC3121 pad: start-up, the wait loop, identity, resets.  One thread:
 * the control loop's plat_wait() runs everything that polls (the board's
 * scanner, the radio core's rings, the tool commands the USB interrupt
 * took) until an event or its deadline.  Interrupts: USB only. */

void plat_clock_init(void);
u32 plat_log_fault(void);

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

/* A tool command waiting is EV_USB: usb_poll() (the controller's) answers
 * it through plat_usb_service(). */
u32 plat_wait(u32 deadline_ms, u32 at_us)
{
    u32 e;

    for (;;) {
        plat_watch_kick();
        plat_wake(board_poll() | plat_bt_poll() | (plat_usb_tool_waiting() ? EV_USB : 0));
        e = take();
        if (e) return e;
        if (deadline_ms && (s32)(plat_ms() - deadline_ms) >= 0) return EV_DEADLINE;
        if (at_us && (s32)(plat_us() - at_us) >= 0) return EV_DEADLINE;
    }
}

/* ---- identity, inputs ---------------------------------------------------- */

/* The chip's ID: 6 bytes of OTP at 2 (the ROM's read_chipid), as stock
 * reads it for its address; the rest zero. */
#define ROM_INIT_OTP   ((void (*)(void))0x442d)
#define ROM_DEINIT_OTP ((void (*)(void))0x4481)
#define ROM_READ_OTP   ((void (*)(u32, u8 *, u32))0x449d)

static u8 uid[16];

const u8 *plat_unique_id(void)
{
    return uid;
}

void plat_input(struct in_raw *raw)
{
    board_input(raw);
}

void plat_battery(struct battery_status *out)
{
    board_battery(out);
}

/* 32-bit words from addr.  Not the flash's data pages (a read there resets
 * the chip) nor the boot loader's protected part. */
u32 plat_peek(u32 addr, u8 *out, u32 max)
{
    u32 n = 0;

    if (addr & 3) return 0;
    if (addr >= 0x01010000 && addr < 0x01018000) return 0;
    if (addr >= 0x0107c000 && addr < 0x01080000) return 0;
    for (; n + 4 <= max && n < 256; n += 4) util_put32(out + n, *(volatile u32 *)(addr + n));
    return n;
}

/* No sampling profiler here. */
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

void plat_stats(struct plat_stats *out)
{
    memset(out, 0, sizeof(*out));
}

/* The last slot, for Home at the next power-on: a store record. */
static u8 retained_slot = SLOT_NONE, retained_loaded;

u8 plat_retained_slot(void)
{
    u8 record[STORE_LAST_SLOT_SIZE];

    if (!retained_loaded) {
        retained_loaded = 1;
        if (plat_store_read(STORE_LAST_SLOT_KEY, record, sizeof(record)) == sizeof(record) &&
            record[0] == 'S' && record[1] == 1 && record[2] < SLOTS && record[3] == (u8)~record[2])
            retained_slot = record[2];
    }
    return retained_slot;
}

void plat_retain_slot(u8 slot)
{
    u8 record[STORE_LAST_SLOT_SIZE] = { 'S', 1, slot, (u8)~slot };

    if (slot >= SLOTS || slot == plat_retained_slot()) return;
    if (plat_store_write(STORE_LAST_SLOT_KEY, record, sizeof(record)) == sizeof(record))
        retained_slot = slot;
}

/* ---- boot and resets ------------------------------------------------------ */

static u8 boot_cause;
static u32 boot_held;

void plat_boot_info(struct plat_boot *out)
{
    out->cause = boot_cause;
    out->buttons_held = boot_held;
}


void plat_power_off(void)
{
    if (board_usb_present()) return;     /* a cable arrived during shutdown */
    board_power_off();
}

/* Restarts go through the 8BitDo boot loader, which starts the app again
 * (plat_restart); the loader is also the way to new firmware. */
void plat_reboot(void)
{
    plat_restart(0);
}

void plat_reboot_loader(void)
{
    plat_restart(1);
}

void plat_reboot_updater(void)
{
    plat_restart(1);
}

void plat_reboot_config(void)
{
    plat_restart(0);
}

int plat_config_asked(void)
{
    return 0;
}

/* Home held at power-on is Home's boot (on battery the pad starts only
 * while Home is down); else USB power started it, or a restart. */
int main(void)
{
    plat_clock_init();
    board_init();
    util_log_init();
    util_log("boot: %s %s", board_name, BUILD_ID);
#ifdef BOOT_HANG_TEST                    /* a test build: a hang at boot, unless the last boot was one */
    u32 last_fault = plat_log_fault();
#else
    plat_log_fault();
#endif
    ROM_INIT_OTP();
    ROM_READ_OTP(2, uid, 6);
    ROM_DEINIT_OTP();
    boot_held = board_held();
    boot_cause = boot_held & PAD_HOME ? BOOT_HOME : board_usb_present() ? BOOT_USB : BOOT_POWER_ON;
    *(volatile u32 *)0xe000e180 = ~0u;   /* NVIC: what the loader left on, off */
    plat_usb_init();
    plat_bt_init();
    __asm__ volatile ("cpsie i" ::: "memory");
#ifdef HANG_WATCH                        /* not working yet (it never fired on the pad): test builds only */
    YC_SYST_CSR = 7;                     /* SysTick's interrupt: the hang watch */
#endif
#ifdef BOOT_HANG_TEST
    if (last_fault != 15) {
        util_log("boot: hanging on purpose (BOOT_HANG_TEST)");
        plat_watch_kick();
        for (;;) {
        }
    }
#endif
    ctl_main();
    return 0;
}
