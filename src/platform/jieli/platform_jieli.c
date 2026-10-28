#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/power/p33.h>
#ifndef CONFIG_CPU_BD19
#include <asm/power/power_port.h>
#endif
#include <asm/charge.h>
#include <asm/clock.h>
#include <asm/rtc.h>
#include <asm/wdt.h>
#include <asm/adc_api.h>
#include <asm/mcpwm.h>
#include <usb/otg.h>
#include <usb/device/usb_stack.h>
#include "app_config.h"
#include "board.h"

/* The chip, the SDK, USB and the tick; the board's pins are in board_pad.c
 * or board_dongle.c (board.h). */

#define DEBOUNCE_TICKS  8
#define LINES_MAX       32

#ifdef CONFIG_CPU_BD19
#define USB_CON0        JL_USB->CON0
#define USB_CON1        JL_USB->CON1
#define USB_IO_CON0     JL_USB_IO->CON0
#define MASKROM_TOKEN   ((volatile u8 *)0xfc00)
#else
#define USB_CON0        (*(volatile u32 *)0x001e1800)
#define USB_CON1        (*(volatile u32 *)0x001e1804)
#define USB_IO_CON0     (*(volatile u32 *)0x001e5100)
#define USB_IO_CON1     (*(volatile u32 *)0x001e5104)
#define MASKROM_TOKEN   ((volatile u8 *)0x00010800)
#endif
#define USB_SOF_STALE_MS 250

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr);
extern void nvram_set_boot_state(u32 state);
extern void flush_dcache(const void *address, u32 size);
extern void flushinv_dcache(const void *address, u32 size);
extern u8 *get_norflash_uuid(void) __attribute__((weak));
extern void hci_jieli_init(void);

#ifdef CONFIG_CPU_BD19
/* The BD19's heap is about 18 KB, and running out of it resets the chip:
 * the SDK's BD19 HID sizes (in words), ours smaller than on the BR23.
 * Stack use not measured yet. */
const struct task_info task_info_table[] = {
    {"app_core",  1, 0, 640,  128 },
    {"ctl",       2, 0, 512,  0   },
    {"sys_event", 7, 0, 256,  0   },
    {"btctrler",  4, 0, 512,  256 },
    {"btencry",   1, 0, 512,  128 },
    {"btstack",   3, 0, 1024, 256 },
    {"systimer",  7, 0, 128,  0   },
    {"update",    1, 0, 512,  0   },
    {0, 0},
};
#else
const struct task_info task_info_table[] = {
    {"app_core",  1, 0, 1024, 128 },
    {"ctl",       2, 0, 1024, 0   },
    {"sys_event", 7, 0, 256,  0   },
    {"btctrler",  4, 0, 512,  256 },
    {"btencry",   1, 0, 512,  128 },
    {"btstack",   3, 0, 2048, 256 },
    {"p256",      1, 0, 2048, 0   },     /* below ctl: a key takes 0.75 s */
    {"systimer",  7, 0, 128,  0   },
    {"update",    1, 0, 512,  0   },
    {0, 0},
};
#endif

/* ---- MaskROM calls: out of range of a direct CALL from flash ------------- */

void plat_mdelay(u32 ms)
{
#ifdef CONFIG_CPU_BD19
    void (* volatile f)(u32) = (void (*)(u32))0x001051fa;
    f(ms);
#else
    void (* volatile f)(u32) = (void (*)(u32))0x00111524;
    f(ms);
#endif
}

void plat_flush_dcache(const void *address, u32 size)
{
#ifdef CONFIG_CPU_BD19           /* its RAM is not behind a data cache (no flush_dcache) */
    (void)size;
    asm volatile("csync" :: "r"(address) : "memory");   /* the address: the stores stay */
#else
    void (* volatile f)(const void *, u32) = flush_dcache;
    f(address, size);
#endif
}

/* ---- the last slot: two RTC data registers, kept through a soft power-off -- */

static u8 retained_slot = SLOT_NONE;

static void retained_read(void)
{
    u8 slot = p33_rx_1byte(R3_RTC_DAT0), check = p33_rx_1byte(R3_RTC_DAT4);
    retained_slot = slot < SLOTS && check == (u8)~slot ? slot : SLOT_NONE;
}

u8 plat_retained_slot(void)
{
    return retained_slot;
}

void plat_retain_slot(u8 slot)
{
    p33_tx_1byte(R3_RTC_DAT0, slot);
    p33_tx_1byte(R3_RTC_DAT4, (u8)~slot);
    retained_slot = slot;
}

/* ---- buttons: sampled and debounced in the 250 us timer interrupt -------- */

volatile u32 plat_now_ms;
#define ms_now plat_now_ms
/* Keep the ISR divider in its own word: byte globals may share a
 * compiler-merged word with task-written flags on this toolchain. */
static volatile u32 quarter;
static u32 tick_mhz = 24;                /* timer 3's clock, lsb: sys / 2 at 48 MHz, / 3 at 96 */
static u32 line_state;
static u8 line_lock[LINES_MAX];
static volatile u32 events;
static volatile u32 deadline;
static volatile u8 deadline_set;
static volatile u32 deadline_us;         /* to the tick: 250 µs */
static volatile u8 deadline_us_set;
static volatile u8 posted;
static OS_SEM wake_sem;
static volatile u32 changed_us;
struct plat_stats plat_counters;

/* The four GPIO groups, as gpio / 16 numbers them: A-D on the BR23; A, B,
 * D and P on the BD19, which has no port C (reading it faults). */
#ifdef CONFIG_CPU_BD19
#define PORT_GROUPS(in) { JL_PORTA->in, JL_PORTB->in, JL_PORTD->in, JL_PORTP->in }
#else
#define PORT_GROUPS(in) { JL_PORTA->in, JL_PORTB->in, JL_PORTC->in, JL_PORTD->in }
#endif

static u16 port_mask[4];                 /* the button pins, by group */
static u16 port_last[4];
static u8 locks;                         /* lines still in their debounce lockout */

static HOT u32 lines_from(const u32 port[4])
{
    u32 down = 0;
    u32 i;

    for (i = 0; i < board_line_count; i++)
        if (!(port[board_lines[i].gpio / 16] & BIT(board_lines[i].gpio % 16))) down |= BIT(i);
    return down;
}

HOT u32 plat_read_lines(void)
{
    u32 port[4] = PORT_GROUPS(IN);

    return lines_from(port);
}

HOT void plat_raise(u32 ev)
{
    events |= ev;
    if (!posted) {
        posted = 1;
        os_sem_post(&wake_sem);
    }
}

___interrupt
static HOT void tick_isr(void)
{
    u32 raw;
    u32 changed = 0;
    u32 i;

    JL_TIMER3->CON |= BIT(14);
    if (++quarter >= 4) {
        quarter = 0;
        ms_now++;
    }
    {
        /* Most ticks: no pin moved and nothing in its lockout - done. */
        u32 port[4] = PORT_GROUPS(IN);
        u32 same = 1;

        for (i = 0; i < 4; i++) {
            u16 v = (u16)(port[i] & port_mask[i]);

            if (v != port_last[i]) {
                port_last[i] = v;
                same = 0;
            }
        }
        if (!same || locks) {
            raw = lines_from(port);
            locks = 0;
            for (i = 0; i < board_line_count; i++) {
                if (line_lock[i]) {
                    /* counted until the tick after it ends: that tick
                     * compares the line again */
                    line_lock[i]--;
                    locks++;
                } else if ((raw ^ line_state) & BIT(i)) {
                    line_state ^= BIT(i);
                    line_lock[i] = DEBOUNCE_TICKS;
                    locks++;
                    changed = 1;
                }
            }
        }
    }
    if (changed) {
        changed_us = ms_now * 1000 + quarter * TICK_US;
        plat_counters.button_changes++;
        plat_raise(EV_BUTTONS);
    }
    board_tick();
    if (deadline_us_set && (s32)(ms_now * 1000 + quarter * TICK_US - deadline_us) >= 0) {
        deadline_us_set = 0;
        plat_raise(EV_DEADLINE);
    }
    if (deadline_set && (s32)(ms_now - deadline) >= 0) {
        deadline_set = 0;
        plat_raise(EV_DEADLINE);
    }
}

static void tick_start(void)
{
    u32 i;

    for (i = 0; i < board_line_count; i++) port_mask[board_lines[i].gpio / 16] |= (u16)BIT(board_lines[i].gpio % 16);
    line_state = plat_read_lines();
    {
        u32 port[4] = PORT_GROUPS(IN);

        for (i = 0; i < 4; i++) port_last[i] = (u16)(port[i] & port_mask[i]);
    }
    os_sem_create(&wake_sem, 0);
    JL_TIMER3->CON = BIT(14);
    JL_TIMER3->CNT = 0;
    tick_mhz = clk_get("lsb") / 1000000;
    JL_TIMER3->PRD = tick_mhz * TICK_US;
    /* Above the controller's interrupts (2) and the stick UART (3): time is
     * counted in ticks, and at 1, with a link up, ticks held off past the
     * next one were lost, 2-6 % (plat_ms ran that much slow). */
    request_irq(IRQ_TIME3_IDX, 5, tick_isr, 0);
    /* The lsb clock (24 MHz at sys 48 MHz).  On the "osc" input (BIT(3)),
     * as timer 2 still is, timer 3 counted about 1 MHz, unsteadily, on some
     * boots (one in 15 to 35): the slow tick. */
    JL_TIMER3->CON = BIT(0);
}

u32 plat_ms(void)
{
    return ms_now;
}

void plat_lock(void)
{
    local_irq_disable();
}

void plat_unlock(void)
{
    local_irq_enable();
}

/* The hang watch's clock, counted by timer 2 (the profiler's), and when the
 * main loop and the bluetooth thread last came by. */
static volatile u32 watch_us, ctl_seen_us, bt_seen_us;

u32 plat_wait(u32 deadline_ms, u32 at_us)
{
    u32 ev;

    for (;;) {
        local_irq_disable();
        ev = events;
        events = 0;
        posted = 0;
        if (!ev && deadline_ms && (s32)(ms_now - deadline_ms) >= 0) ev = EV_DEADLINE;
        if (!ev && at_us && (s32)(ms_now * 1000 + quarter * TICK_US - at_us) >= 0) ev = EV_DEADLINE;
        if (!ev) {
            deadline = deadline_ms;
            deadline_set = deadline_ms != 0;
            deadline_us = at_us;
            deadline_us_set = at_us != 0;
        }
        local_irq_enable();
        ctl_seen_us = watch_us;
        /* The SDK's setup arms the 4 s watchdog, and only its mass-storage
         * commands and the OS's sleep clear it: a USB personality on a busy
         * bus has neither (the dongle reset 4 s after becoming the slot's
         * type).  The main loop clears it; the 3 s hang watch still logs
         * and restarts first. */
        wdt_clear();
        if (ev) return ev;
        os_sem_pend(&wake_sem, 100);
    }
}

void plat_wake(u32 ev)
{
    local_irq_disable();
    plat_raise(ev);
    local_irq_enable();
}

void plat_delay_ms(u32 ms)
{
    os_time_dly((ms + 9) / 10);
}

static OS_SEM bt_sem;
static volatile u8 bt_woken;
static void (*bt_body)(void);

static void bt_thread(void *arg)
{
    (void)arg;
    bt_body();
}

/* The controller task's start-up (btctrler/task_dispatch.c): the radio's
 * calibration busy-waits about 250 ms; at the task's own priority, above
 * the main loop, the pad stood still that long at every session start.
 * Below it the main loop runs on; the bluetooth thread waits for the
 * controller anyway (btctrler_task_ready). */
#ifndef CONFIG_CPU_BD19
static UBaseType_t controller_priority;

void btctrlermod_init_begin(void)
{
    controller_priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 1);           /* p256's: below "ctl" (task table) */
}

void btctrlermod_init_end(void)
{
    vTaskPrioritySet(NULL, controller_priority);
}
#endif

void plat_bt_thread(void (*body)(void))
{
    bt_body = body;
    os_sem_create(&bt_sem, 0);
    task_create(bt_thread, NULL, "btstack");
}

static OS_SEM p256_sem;
static void (*p256_body)(void);

static void p256_thread(void *arg)
{
    (void)arg;
    p256_body();
}

void plat_p256_thread(void (*body)(void))
{
    p256_body = body;
    os_sem_create(&p256_sem, 0);
    task_create(p256_thread, NULL, "p256");
}

void plat_p256_wait(void)
{
    os_sem_pend(&p256_sem, 0);
}

void plat_p256_wake(void)
{
    os_sem_post(&p256_sem);
}

void plat_bt_wait(u32 ms)
{
    os_sem_pend(&bt_sem, (ms + 9) / 10);
    bt_woken = 0;
    bt_seen_us = watch_us;
}

void plat_bt_wake(void)
{
    if (bt_woken) return;
    bt_woken = 1;
    os_sem_post(&bt_sem);
}

u32 plat_lines(u32 *changed)
{
    *changed = changed_us;
    return line_state;
}

void plat_pull_up_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

/* The tick count plus the timer's progress into the current tick.  A timer
 * that has wrapped but whose interrupt has not run yet (pending) is a tick
 * ahead of ms_now and quarter. */
u32 plat_us(void)
{
    u32 ms, q, cnt, pending;

    do {
        ms = ms_now;
        q = quarter;
        cnt = JL_TIMER3->CNT;
        pending = JL_TIMER3->CON & BIT(15);
    } while (ms != ms_now || q != quarter);
    if (pending && cnt < tick_mhz * TICK_US / 2) cnt += tick_mhz * TICK_US;
    return ms * 1000 + q * TICK_US + cnt / tick_mhz;
}

/* ---- sampling profiler ------------------------------------------------------
 * Timer 2 at priority 6, above the controller's BLE interrupts (2), the stick
 * UART (3) and the tick (5): each interrupt reads RETI, the address it
 * interrupted, and counts it in a 64-byte (or larger) bucket.  The period is
 * jittered so samples do not lock to the 5 ms connection events.  Code that
 * runs with interrupts off is counted at the instruction that turns them on. */

extern char text_code_begin[], text_code_end[], _data_code_begin[], _data_code_end[];

#ifdef CONFIG_CPU_BD19
#define PROF_FLASH_BUCKETS 256
#define PROF_RAM_BUCKETS   32
#define PROF_ROM_BUCKETS   32
#else
#define PROF_FLASH_BUCKETS 4096
#define PROF_RAM_BUCKETS   512
#define PROF_ROM_BUCKETS   512
#endif
#define PROF_ROM_BEGIN     0x110000u
#define PROF_ROM_SIZE      0x8000u
#define PROF_VERSION       1

static struct {
    u16 version, hz;
    u8 flash_shift, ram_shift, rom_shift, running;
    u32 flash_begin, flash_buckets, ram_begin, ram_buckets, rom_begin, rom_buckets;
    u32 samples, other, saturated;
    u32 other_pc[8];
} prof;
static u16 prof_flash[PROF_FLASH_BUCKETS], prof_ram[PROF_RAM_BUCKETS], prof_rom[PROF_ROM_BUCKETS];
static u32 prof_period, prof_lfsr = 0xace1u;

static HOT void prof_count(u16 *b, u32 i)
{
    if (b[i] != 0xffff) b[i]++;
    else prof.saturated++;
}

/* ---- hang watch -------------------------------------------------------------
 * The same interrupt checks that the main loop still comes by: 3 s without it
 * and the log gets what the processor was doing over the last second (task,
 * interrupted addresses), then the pad restarts.  The log survives that. */

#define WATCH_HZ      500       /* timer 2 counts to 65535 at most: 24 MHz / 500 fits */
#define WATCH_TRIP_US 3000000u
#define WATCH_PCS     8

extern void *CurrentTCB;
static u32 watch_pc[WATCH_PCS], watch_rets[WATCH_PCS], watch_n;
static u32 timer2_period;

static void watch(u32 pc, u32 rets, const u32 *usp)
{
    u32 gone, i;
    const char *task;

    watch_us += timer2_period / 24;
    gone = watch_us - ctl_seen_us;
    if (gone < WATCH_TRIP_US - 1000000u) {
        watch_n = 0;
        return;
    }
    watch_rets[watch_n % WATCH_PCS] = rets;
    watch_pc[watch_n++ % WATCH_PCS] = pc;
    if (gone < WATCH_TRIP_US) return;
    task = CurrentTCB ? (const char *)CurrentTCB + 8 : "-";
    util_log("hang: main loop gone %u ms, bt %u ms; task %.12s; tick at %u ms",
             gone / 1000, (watch_us - bt_seen_us) / 1000, task, ms_now);
    for (i = 0; i < WATCH_PCS; i += 4)
        util_log("hang: pc %08x %08x %08x %08x", watch_pc[i], watch_pc[i + 1],
                 watch_pc[i + 2], watch_pc[i + 3]);
    for (i = 0; i < WATCH_PCS; i += 4)
        util_log("hang: rets %08x %08x %08x %08x", watch_rets[i], watch_rets[i + 1],
                 watch_rets[i + 2], watch_rets[i + 3]);
    {   /* code addresses on the task's stack: its callers, roughly */
        u32 found[8], n = 0;

        for (i = 0; i < 96 && n < 8; i++) {
            u32 v = usp[i];

            if (v - (u32)text_code_begin < (u32)(text_code_end - text_code_begin)) found[n++] = v;
        }
        while (n < 8) found[n++] = 0;
        util_log("hang: stack %08x %08x %08x %08x %08x %08x %08x %08x", found[0], found[1],
                 found[2], found[3], found[4], found[5], found[6], found[7]);
    }
    p33_soft_reset();
}

___interrupt
static HOT void prof_isr(void)
{
    u32 pc, rets, usp, i;

    __asm__ volatile("%0 = reti" : "=r"(pc));
    __asm__ volatile("%0 = rets" : "=r"(rets));
    __asm__ volatile("%0 = usp" : "=r"(usp));
    JL_TIMER2->CON |= BIT(14);
    watch(pc, rets, (const u32 *)usp);
    if (!prof.running) return;
    prof_lfsr = (prof_lfsr >> 1) ^ (-(prof_lfsr & 1u) & 0xb400u);
    timer2_period = prof_period + (prof_lfsr & 0x3ff);
    JL_TIMER2->PRD = timer2_period;
    prof.samples++;
    if (pc - prof.flash_begin < (prof.flash_buckets << prof.flash_shift)) {
        prof_count(prof_flash, (pc - prof.flash_begin) >> prof.flash_shift);
    } else if (pc - prof.ram_begin < (prof.ram_buckets << prof.ram_shift)) {
        prof_count(prof_ram, (pc - prof.ram_begin) >> prof.ram_shift);
    } else if (pc - prof.rom_begin < (prof.rom_buckets << prof.rom_shift)) {
        prof_count(prof_rom, (pc - prof.rom_begin) >> prof.rom_shift);
    } else {
        i = prof.other++ & 7;
        prof.other_pc[i] = pc;
    }
}

static u8 shift_for(u32 size, u32 buckets)
{
    u8 s = 6;

    while ((buckets << s) < size) s++;
    return s;
}

/* Before a long wait on purpose: entering the loader, powering off. */
void plat_watch_stop(void)
{
    JL_TIMER2->CON = BIT(14);
    prof.running = 0;
}

/* hz 0 stops profiling; timer 2 then runs on at WATCH_HZ for the hang watch. */
void plat_prof_start(u16 hz, u32 zoom, u8 zoom_shift)
{
    JL_TIMER2->CON = BIT(14);
    prof.running = 0;
    if (hz) {
        if (hz < 400) hz = 400;  /* the period register counts to 65535 */
        memset(&prof, 0, sizeof(prof));
        memset(prof_flash, 0, sizeof(prof_flash));
        memset(prof_ram, 0, sizeof(prof_ram));
        memset(prof_rom, 0, sizeof(prof_rom));
        prof.version = PROF_VERSION;
        prof.hz = hz;
        prof.flash_begin = (u32)text_code_begin;
        prof.flash_shift = shift_for((u32)(text_code_end - text_code_begin), PROF_FLASH_BUCKETS);
        prof.flash_buckets = PROF_FLASH_BUCKETS;
        if (zoom) {              /* the flash buckets over a narrower window */
            prof.flash_begin = zoom;
            prof.flash_shift = zoom_shift;
        }
        prof.ram_begin = (u32)_data_code_begin;
        prof.ram_shift = shift_for((u32)(_data_code_end - _data_code_begin), PROF_RAM_BUCKETS);
        prof.ram_buckets = PROF_RAM_BUCKETS;
        prof.rom_begin = PROF_ROM_BEGIN;
        prof.rom_shift = shift_for(PROF_ROM_SIZE, PROF_ROM_BUCKETS);
        prof.rom_buckets = PROF_ROM_BUCKETS;
        /* 24 MHz timer clock, as the tick; the jitter adds up to 1023 counts */
        prof_period = 24000000u / hz - 512;
        timer2_period = prof_period;
    } else {
        timer2_period = 24000000u / WATCH_HZ;
    }
    JL_TIMER2->CNT = 0;
    JL_TIMER2->PRD = timer2_period;
    request_irq(IRQ_TIME2_IDX, 6, prof_isr, 0);
    prof.running = hz != 0;
    JL_TIMER2->CON = BIT(0) | BIT(3);
}

u32 plat_peek(u32 addr, u8 *out, u32 max)
{
    u32 n;

    addr &= ~3u;
    for (n = 0; n + 4 <= max; n += 4) {
        u32 v = *(volatile u32 *)(addr + n);

        memcpy(out + n, &v, 4);
    }
    return n;
}

/* The header, then the flash, RAM and ROM counts, read from a byte offset. */
u32 plat_prof_read(u32 offset, u8 *out, u32 max)
{
    u32 n = 0, k;
    const u8 *parts[4] = { (const u8 *)&prof, (const u8 *)prof_flash, (const u8 *)prof_ram,
                           (const u8 *)prof_rom };
    const u32 sizes[4] = { sizeof(prof), sizeof(prof_flash), sizeof(prof_ram), sizeof(prof_rom) };
    u32 i;

    for (i = 0; i < 4 && n < max; i++) {
        if (offset >= sizes[i]) {
            offset -= sizes[i];
            continue;
        }
        k = sizes[i] - offset;
        if (k > max - n) k = max - n;
        memcpy(out + n, parts[i] + offset, k);
        n += k;
        offset = 0;
    }
    return n;
}

void plat_stats(struct plat_stats *out)
{
    plat_lock();
    *out = plat_counters;
    plat_unlock();
}

u16 plat_battery_mv(void)
{
    return adc_get_voltage(AD_CH_VBAT) * 4;
}

const u8 *plat_unique_id(void)
{
    return get_norflash_uuid ? get_norflash_uuid() : NULL;
}

int plat_store_read(u16 key, void *buf, u16 len)
{
    return syscfg_read(key, buf, len);
}

int plat_store_write(u16 key, const void *buf, u16 len)
{
    return syscfg_write(key, (void *)buf, len);
}

/* ---- USB ------------------------------------------------------------------ */

#ifdef USB_PROBE
/* Test: what a device attached by the ROM loader's GPIO pull-up, answering
 * nothing, sees on the bus (the ROM learns its crystal from it).  Phases:
 * off 300 ms, then twice on 1000 ms / off 200 ms, as the ROM's attempts.
 * Each run of non-idle line states is an event of two words at PROBE_AT:
 * its start in timer 1 counts (lsb), and its length << 2 | the first state
 * (USB_IO_CON1 bits 1:0); a phase change is (count, 0xfffffff0 | phase).
 * The argument's bit 0 records instead what the ROM's own measurement
 * captures (timer 3 on D+, osc / 4): (count, 0x80000000 | captured value).
 * Its bits 0x7100 are added to USB_IO_CON0.  PROBE_AT[-4..-1]: event count,
 * idle state, events lost | the added bits << 16, done marker.
 * tools/usbprobe.py runs it and reads the events back. */
#define PROBE_AT        ((u32 *)0x22210)
#define IOMAP_CON1      (*(volatile u32 *)0x001e511c)
#define IOMAP_CON2      (*(volatile u32 *)0x001e5120)
#define PROBE_MAX       4800
#define PROBE_TICKS_MS  24000
#define PROBE_SPIN_MAX  100000000u     /* a backstop: some 10 s */

static u8 probe_requested;
static u32 probe_n, probe_lost, probe_ms, probe_spins;

/* Timer 1 wraps every millisecond; the wraps are counted here. */
static u32 probe_now(void)
{
    u32 c;

    if (JL_TIMER1->CON & BIT(15)) {
        JL_TIMER1->CON |= BIT(14);
        probe_ms++;
    }
    c = JL_TIMER1->CNT;
    if ((JL_TIMER1->CON & BIT(15)) && c < PROBE_TICKS_MS / 2) {
        JL_TIMER1->CON |= BIT(14);
        probe_ms++;
    }
    if (!(++probe_spins & 0xfff)) wdt_clear();    /* interrupts are off */
    return probe_ms * PROBE_TICKS_MS + c;
}
#define T1              probe_now()

static void probe_event(u32 a, u32 b)
{
    if (probe_n < PROBE_MAX) {
        PROBE_AT[2 * probe_n] = a;
        PROBE_AT[2 * probe_n + 1] = b;
        probe_n++;
    } else {
        probe_lost++;
    }
}

static u8 probe_capture;

static void probe_phase(u32 io, u32 phase, u32 ms, u32 idle)
{
    u32 until;

    USB_IO_CON0 = io;
    probe_event(T1, 0xfffffff0 | phase);
    until = T1 + ms * PROBE_TICKS_MS;
    if (probe_capture) {
        /* As the ROM: timer 3 captures D+ rising edges, counting osc / 4. */
        while ((s32)(T1 - until) < 0 && probe_spins < PROBE_SPIN_MAX) {
            if (JL_TIMER3->CON & BIT(15)) {
                u32 v = JL_TIMER3->PRD;

                JL_TIMER3->CON |= BIT(14);
                probe_event(T1, 0x80000000 | (v & 0x7fffffff));
            }
        }
        return;
    }
    while ((s32)(T1 - until) < 0 && probe_spins < PROBE_SPIN_MAX) {
        u32 l = USB_IO_CON1 & 3, start, last, first;

        if (l == idle) continue;
        start = last = T1;
        first = l;
        for (;;) {
            u32 now = T1;

            if ((USB_IO_CON1 & 3) != idle) last = now;
            else if (now - last > 24) break;
            if ((s32)(now - until) > 0 || probe_spins >= PROBE_SPIN_MAX) break;
        }
        probe_event(start, (last - start) << 2 | first);
    }
}

static u32 probe_extra;

void plat_usb_probe_request(u32 extra)
{
    probe_extra = extra & 0x7100;    /* IO_PU_MODE, SR, DPDIEH, DMDIEH */
    probe_capture = extra & 1;       /* the ROM's own measurement instead */
    probe_requested = 1;
}

static void usb_probe(void)
{
    const u32 attached = BIT(IO_MODE) | BIT(DMDIE) | BIT(DPDIE) | BIT(DPPU) | BIT(DPIE) | BIT(DMIE) | probe_extra;
    const u32 off = BIT(IO_MODE) | BIT(DMDIE) | BIT(DPDIE) | BIT(DPIE) | BIT(DMIE) | probe_extra;
    u32 io = USB_IO_CON0, i, count[4] = { 0 }, idle, iomap1, iomap2;

    plat_mdelay(50);             /* the command's status reaches the host */
    plat_watch_stop();
    local_irq_disable();
    JL_TIMER1->CON = BIT(14);
    JL_TIMER1->CNT = 0;
    JL_TIMER1->PRD = PROBE_TICKS_MS;
    JL_TIMER1->CON = BIT(0);
    probe_n = probe_lost = probe_ms = probe_spins = 0;
    PROBE_AT[-1] = 0;
    iomap1 = IOMAP_CON1;
    iomap2 = IOMAP_CON2;
    if (probe_capture) {
        IOMAP_CON2 = (IOMAP_CON2 & ~(0x3fu << 16)) | 0x3du << 16;   /* input channel 2: D+ */
        IOMAP_CON1 |= BIT(31);
        JL_TIMER3->CON = BIT(14);
        JL_TIMER3->CNT = 0;
        JL_TIMER3->PRD = 0;
        JL_TIMER3->CON = 0x1a;
    }
    probe_phase(off, 0, 300, 0);
    USB_IO_CON0 = attached;
    for (i = 0; i < 2000; i++) count[USB_IO_CON1 & 3]++;
    idle = count[1] >= count[2] ? 1 : 2;
    probe_phase(attached, 1, 1000, idle);
    probe_phase(off, 2, 200, idle);
    probe_phase(attached, 3, 1000, idle);
    probe_phase(off, 4, 300, idle);
    probe_event(T1, 0xfffffff5);
    PROBE_AT[-4] = probe_n;
    PROBE_AT[-3] = idle;
    PROBE_AT[-2] = probe_lost | probe_extra << 16;
    PROBE_AT[-1] = 0x50524f42;
    JL_TIMER1->CON = BIT(14);
    if (probe_capture) {
        JL_TIMER3->CON = BIT(14);
        JL_TIMER3->CNT = 0;
        JL_TIMER3->PRD = tick_mhz * TICK_US;
        JL_TIMER3->CON = BIT(0);
    }
    IOMAP_CON1 = iomap1;
    IOMAP_CON2 = iomap2;
    USB_IO_CON0 = io;
    local_irq_enable();
    plat_prof_start(0, 0, 0);
}
#endif

void plat_usb_service(void)
{
#ifndef USB_TOOL_ONLY            /* only ever the tool device: no personalities */
    plat_usb_jack();
#endif
    plat_usb_tool_service();
#ifdef USB_PROBE
    if (probe_requested) {
        probe_requested = 0;
        usb_probe();
    }
#endif
}

static u32 sof_seen_ms;
static u16 frame;

int plat_usb_present(void)
{
    static u8 started;
    u16 f = usb_read_sofframe(USB0);

    if (!started) {
        started = 1;
        frame = f;
        sof_seen_ms = ms_now - USB_SOF_STALE_MS;
    } else if (f != frame) {
        frame = f;
        if ((s32)(ms_now - sof_seen_ms) > 0) sof_seen_ms = ms_now;
    }
    return (s32)(ms_now - sof_seen_ms) < USB_SOF_STALE_MS;
}

int plat_usb_power(void)
{
    return board_usb_sense();
}

/* The core's POWER register, bit 1 (SuspendMode): set while the bus is
 * suspended, cleared by resume or reset. */
int plat_usb_suspended(void)
{
    return (usb_read_power(USB0) & BIT(1)) != 0;
}

/* Re-initialising the core resets its frame number: that change is ours,
 * not a frame from a host. */
void plat_usb_frames_rebase(void)
{
    frame = usb_read_sofframe(USB0);
}

/* A re-enumeration (platform_jieli_usb.c) stops the frames for a while:
 * a host that was there counts as there. */
void plat_usb_hold_present(u32 ms)
{
    if (plat_usb_present()) sof_seen_ms = ms_now + ms;
}

static void usb_init(void)
{
    plat_usb_tool_mode();
}

/* ---- power ---------------------------------------------------------------- */

static void arm_loader_token(void)
{
    nvram_set_boot_state(2);
    __asm__ volatile("csync");
#ifndef CONFIG_CPU_BD19
    flushinv_dcache((const void *)MASKROM_TOKEN, 16);
#endif
}

void plat_enter_loader(void)
{
    plat_watch_stop();
    local_irq_disable();
    arm_loader_token();
    wdt_init(WDT_256MS);
    for (;;) {
    }
}

/* Still attached through the reset, as the SDK enters it.  Before it
 * answers, the ROM loader times the host's frames on the lines to learn its
 * crystal (attached by a GPIO pull-up, not answering), then detaches and
 * attaches as itself.  A port the host already has enabled and leaves alone
 * gives it those frames at once; detached first, the loader is a new device
 * the host resets and asks for descriptors throughout, and behind hub
 * 05e3:0608 it never learned the crystal.  The pins go to the ROM's own
 * attach state (USB_IO_CON0 0xe4c) before the USB function stops, so the
 * D+ pull-up stays. */
void plat_reboot_loader(void)
{
    plat_watch_stop();
    plat_audio_off();
    board_to_loader();
    plat_mdelay(100);
    while (!board_usb_sense()) plat_mdelay(100);
    local_irq_disable();
    USB_IO_CON0 = BIT(IO_MODE) | BIT(DMDIE) | BIT(DPDIE) | BIT(DPPU) | BIT(DPIE) | BIT(DMIE);
    USB_CON0 = 0;
    USB_CON1 = 0;
    plat_enter_loader();
}

/* The updater (src/updater/) stays at the next boot instead of starting
 * the app; without an updater this is a reboot. */
u32 plat_updater_mailbox[3] __attribute__((section(".reset_mailbox")));

void plat_reboot_updater(void)
{
    plat_updater_mailbox[0] = UPDATER_STAY;
    plat_updater_mailbox[1] = ~UPDATER_STAY;
    plat_flush_dcache(plat_updater_mailbox, sizeof(plat_updater_mailbox));
    plat_reboot();
}

void plat_reboot(void)
{
    plat_watch_stop();
    plat_audio_off();
    local_irq_disable();
    p33_soft_reset();
}

/* Every pin floats at power-off but the flash, USB and the board's keep list
 * (wake-up and sense pins, as board_latch() set them). */
static void soft_poweroff_pins(void)
{
#ifndef CONFIG_CPU_BD19
    u16 group[4] = { 0xffff, 0xffff, 0xffff, 0xffff };
    u32 i;

    if (spi_get_port() == 0) {
        port_protect(group, SPI0_PWR_A);
        port_protect(group, SPI0_CS_A);
        port_protect(group, SPI0_CLK_A);
        port_protect(group, SPI0_DO_D0_A);
        port_protect(group, SPI0_DI_D1_A);
        if (get_sfc_bit_mode() == 4) {
            port_protect(group, SPI0_WP_D2_A);
            port_protect(group, SPI0_HOLD_D3_A);
        }
    } else {
        port_protect(group, SPI0_PWR_B);
        port_protect(group, SPI0_CS_B);
        port_protect(group, SPI0_CLK_B);
        port_protect(group, SPI0_DO_D0_B);
        port_protect(group, SPI0_DI_D1_B);
        if (get_sfc_bit_mode() == 4) {
            port_protect(group, SPI0_WP_D2_B);
            port_protect(group, SPI0_HOLD_D3_B);
        }
    }
    for (i = 0; i < board_keep_count; i++) port_protect(group, board_keep_pins[i]);
    gpio_dir(GPIOA, 0, 16, group[0], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_dir(GPIOB, 0, 16, group[1], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_dir(GPIOC, 0, 16, group[2], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_dir(GPIOD, 0, 16, group[3], GPIO_OR);
    gpio_set_pu(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_set_pd(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_die(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_dieh(GPIOD, 0, 16, ~group[3], GPIO_AND);
    usb_iomode(1);
    gpio_set_pull_up(IO_PORT_DP, 0);
    gpio_set_pull_down(IO_PORT_DP, 0);
    gpio_set_direction(IO_PORT_DP, 1);
    gpio_set_die(IO_PORT_DP, 0);
    gpio_set_dieh(IO_PORT_DP, 0);
    gpio_set_pull_up(IO_PORT_DM, 0);
    gpio_set_pull_down(IO_PORT_DM, 0);
    gpio_set_direction(IO_PORT_DM, 1);
    gpio_set_die(IO_PORT_DM, 0);
    gpio_set_dieh(IO_PORT_DM, 0);
#endif
}

static void sleep_enter(u8 step)
{
    (void)step;
}

static void sleep_exit(u32 usec)
{
    (void)usec;
}

static const struct low_power_param power_param = {
    .config = TCFG_LOWPOWER_LOWPOWER_SEL, .btosc_hz = TCFG_CLOCK_OSC_HZ,
    .delay_us = TCFG_CLOCK_SYS_HZ / 1000000L, .btosc_disable = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev = TCFG_LOWPOWER_VDDIOM_LEVEL, .vddiow_lev = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .osc_type = OSC_TYPE_LRC,
#ifndef CONFIG_CPU_BD19
    .dcdc_port = TCFG_DCDC_PORT_SEL,
#endif
};

/* ---- start-up --------------------------------------------------------------- */

/* Never let the SDK put the chip into low-power sleep on its own: the pad
 * sleeps only by powering off, the dongle not at all. */
static u8 never_idle(void)
{
    return 0;
}

REGISTER_LP_TARGET(pad_lp_target) = {
    .name = "pad",
    .is_idle = never_idle,
};

static u32 boot_usb_io;          /* USB_IO_CON0 as the boot found it */

#ifdef DEBUG_UART_DP
/* Bring-up only (make EXTRA_DEFINES=-DDEBUG_UART_DP, with -DDEBUG_NO_USB to
 * keep it past usb_init()): UART0 TX on USB D+ at 1 Mbaud, which
 * jl-forced-usb's `u` mode reads: log lines, the libraries' printf_lite,
 * exceptions.  Started again after the clock change. */
#include <asm/uart.h>
extern void putbyte(char c);

static u8 debug_uart_up;

static void debug_uart_start(void)
{
    static const struct uart_platform_data uart = {
        .irq = IRQ_UART0_IDX, .tx_pin = IO_PORT_DP, .rx_pin = (u8)NO_CONFIG_PORT,
        .flags = UART_DEBUG, .baudrate = 1000000,
    };

    uart_init(&uart);
    debug_uart_up = 1;
}

void plat_debug_line(const char *text)
{
    if (!debug_uart_up) return;
    while (*text) putbyte(*text++);
    putbyte('\r');
    putbyte('\n');
}

static void debug_hex(const char *what, u32 v)
{
    static const char hex[] = "0123456789abcdef";
    char t[40];
    u32 n = 0, i;

    while (*what && n < 28) t[n++] = *what++;
    t[n++] = ' ';
    for (i = 0; i < 8; i++) t[n++] = hex[(v >> (28 - 4 * i)) & 15];
    t[n] = 0;
    plat_debug_line(t);
}

/* Where the CPU faulted, then wait for the watchdog. */
___interrupt
static void debug_exception(void)
{
    u32 rete, reti, retx, rets, sp;

    __asm__ volatile("%0 = rete" : "=r"(rete));
    __asm__ volatile("%0 = reti" : "=r"(reti));
    __asm__ volatile("%0 = retx" : "=r"(retx));
    __asm__ volatile("%0 = rets" : "=r"(rets));
    __asm__ volatile("%0 = sp" : "=r"(sp));
    plat_debug_line("EXCEPTION");
    debug_hex("rete", rete);
    debug_hex("reti", reti);
    debug_hex("retx", retx);
    debug_hex("rets", rets);
    debug_hex("sp", sp);
    debug_hex("cpu emu_msg", j32CPU(0)->EMU_MSG);
    debug_hex("cemu msg0", JL_CEMU->MSG0);
    debug_hex("cemu msg1", JL_CEMU->MSG1);
    debug_hex("cemu msg2", JL_CEMU->MSG2);
    debug_hex("cemu msg3", JL_CEMU->MSG3);
    debug_hex("dcu emu_msg", JL_DCU->EMU_MSG);
    for (;;) {
    }
}
#else
#define debug_uart_start() ((void)0)

/* Where the CPU faulted, in the log (its ring outlives the soft reset). */
___interrupt
static void log_exception(void)
{
    u32 rete, reti, retx, rets, sp;

    __asm__ volatile("%0 = rete" : "=r"(rete));
    __asm__ volatile("%0 = reti" : "=r"(reti));
    __asm__ volatile("%0 = retx" : "=r"(retx));
    __asm__ volatile("%0 = rets" : "=r"(rets));
    __asm__ volatile("%0 = sp" : "=r"(sp));
    util_log("exception: rete %08x reti %08x retx %08x rets %08x sp %08x", rete, reti, retx, rets, sp);
    p33_soft_reset();
}
#endif

void board_init(void)
{
    boot_usb_io = USB_IO_CON0;
    debug_uart_start();
    plat_debug_line("board_init");
#if defined(DEBUG_UART_DP) && defined(CONFIG_CPU_BD19)
    request_irq(1, 7, debug_exception, 0);
    debug_hex("reset source", P3_RST_SRC);
#elif !defined(DEBUG_UART_DP)
    request_irq(1, 7, log_exception, 0);
#endif
    board_latch();
    retained_read();             /* before power_init(), which might touch the RTC domain */
    power_init(&power_param);
    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);
    gpio_shortpress_reset_config(0);
    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter, sleep_exit,
                       soft_poweroff_pins);
    power_keep_dacvdd_en(0);
    if (board_wakeups) power_wakeup_init(board_wakeups);
    adc_vbg_init();
    adc_init();
    devices_init();
    CHARGE_EN(0);
#ifndef CONFIG_CPU_BD19
    CHGBG_EN(0);
#endif
    power_set_mode(PWR_LDO15);
}


#if !defined(MAIN_AT) && !defined(BOARD_DONGLE)
/* An updater chord (ux_updater_chord) held with USB at start-up goes to
 * the updater before the app's USB comes up.  One without USB is the
 * controller's: it waits a while for USB. */
static void updater_at_start(void)
{
    struct plat_boot b;

    plat_boot_info(&b);
    if (board_usb_sense() && ux_updater_chord(b.buttons_held)) {
        util_log("boot: updater chord %05x", b.buttons_held);
        plat_reboot_updater();
    }
}
#endif

static void ctl_thread(void *arg)
{
    (void)arg;
    clk_set("sys", BT_NORMAL_HZ);
    debug_uart_start();
#ifndef NO_BT
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    /* Without this the radio stays at its lowest step, 20 dB below stock. */
    bt_max_pwr_set(10, 5, 8, 6);
#endif
    util_log_init();
    board_start();
#if !defined(MAIN_AT) && !defined(BOARD_DONGLE)
    updater_at_start();
#endif
#ifdef DEBUG_NO_USB
    plat_debug_line("usb_init skipped");   /* the D+ log goes on */
#else
    usb_init();
#endif
    tick_start();
    plat_prof_start(0, 0, 0);    /* the hang watch */
    {
        /* The SDK's setup.c reads why the chip started (bits not decoded
         * yet: compare a power-on, a reboot and a watchdog). */
#ifdef CONFIG_CPU_BD19
        u8 power_reset_src = P3_RST_SRC;
#else
        extern u8 power_reset_src;
#endif

        util_log("boot: reset source %02x, usb io %x", power_reset_src, boot_usb_io);
    }
#ifndef NO_BT
    hci_jieli_init();
#endif
#ifndef MAIN_AT                  /* the app: up, the updater's count of its starts ends */
    plat_updater_mailbox[2] = 0;
    plat_flush_dcache(plat_updater_mailbox, sizeof(plat_updater_mailbox));
#endif
    ctl_main();
}

void app_main(void)
{
    task_create(ctl_thread, NULL, "ctl");
}
