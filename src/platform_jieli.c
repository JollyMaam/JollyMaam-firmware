#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/power/p33.h>
#include <asm/power/power_port.h>
#include <asm/charge.h>
#include <asm/clock.h>
#include <asm/rtc.h>
#include <asm/wdt.h>
#include <asm/adc_api.h>
#include <asm/mcpwm.h>
#include <usb/otg.h>
#include <usb/scsi.h>
#include <usb/device/msd.h>
#include <usb/device/usb_stack.h>
#include "app_config.h"
#include "board.h"

/* The chip, the SDK, USB and the tick; the board's pins are in board_pad.c
 * or board_dongle.c (board.h). */

#define DEBOUNCE_TICKS  8
#define LINES_MAX       32

#define USB_CON0        (*(volatile u32 *)0x001e1800)
#define USB_CON1        (*(volatile u32 *)0x001e1804)
#define USB_IO_CON0     (*(volatile u32 *)0x001e5100)
#define MASKROM_TOKEN   ((volatile u8 *)0x00010800)
#define USB_VENDOR_OP   0xf5
#define USB_SOF_STALE_MS 250

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr);
extern void nvram_set_boot_state(u32 state);
extern void flush_dcache(const void *address, u32 size);
extern void flushinv_dcache(const void *address, u32 size);
extern u32 msd_mcu2usb(const struct usb_device_t *usb_device, const u8 *buffer, u32 len);
extern u32 msd_usb2mcu(const struct usb_device_t *usb_device, u8 *buffer, u32 len);
extern u8 *get_norflash_uuid(void) __attribute__((weak));
extern void hci_jieli_init(void);

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

/* ---- MaskROM calls: out of range of a direct CALL from flash ------------- */

void plat_mdelay(u32 ms)
{
    void (* volatile f)(u32) = (void (*)(u32))0x00111524;
    f(ms);
}

void plat_flush_dcache(const void *address, u32 size)
{
    void (* volatile f)(const void *, u32) = flush_dcache;
    f(address, size);
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

static u16 port_mask[4];                 /* the button pins on ports A-D */
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
    u32 port[4] = { JL_PORTA->IN, JL_PORTB->IN, JL_PORTC->IN, JL_PORTD->IN };

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

/* Once a boot: the tick against a host's frames, one a millisecond (the
 * intermittent ~20x slow tick).  Counted in the tick while frames come
 * steadily; a jump (a re-init, a gap) starts again.  Logged by the loop. */
#define CLOCK_CHECK_FRAMES 1000
static struct { u8 state; u16 last, frames; u32 since_ms, took_ms; } clock_check;

static HOT void clock_check_tick(void)
{
    u16 f, df;

    if (clock_check.state) return;
    f = usb_read_sofframe(USB0);
    df = (u16)((f - clock_check.last) & 0x7ff);
    clock_check.last = f;
    if (!df || df > 50) {
        clock_check.frames = 0;
        return;
    }
    if (!clock_check.frames) clock_check.since_ms = ms_now;
    clock_check.frames += df;
    if (clock_check.frames < CLOCK_CHECK_FRAMES) return;
    clock_check.took_ms = ms_now - clock_check.since_ms;
    clock_check.state = 1;
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
        clock_check_tick();
    }
    {
        /* Most ticks: no pin moved and nothing in its lockout - done. */
        u32 port[4] = { JL_PORTA->IN, JL_PORTB->IN, JL_PORTC->IN, JL_PORTD->IN };
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
    port_last[0] = (u16)(JL_PORTA->IN & port_mask[0]);
    port_last[1] = (u16)(JL_PORTB->IN & port_mask[1]);
    port_last[2] = (u16)(JL_PORTC->IN & port_mask[2]);
    port_last[3] = (u16)(JL_PORTD->IN & port_mask[3]);
    os_sem_create(&wake_sem, 0);
    JL_TIMER3->CON = BIT(14);
    JL_TIMER3->CNT = 0;
    JL_TIMER3->PRD = 24 * TICK_US;
    request_irq(IRQ_TIME3_IDX, 1, tick_isr, 0);
    /* The lsb clock (24 MHz: sys 48 MHz / 2).  On the "osc" input (BIT(3)),
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
    if (pending && cnt < 24 * TICK_US / 2) cnt += 24 * TICK_US;
    return ms * 1000 + q * TICK_US + cnt / 24;
}

/* ---- sampling profiler ------------------------------------------------------
 * Timer 2 at priority 6, above the controller's BLE interrupts (2), the stick
 * UART (3) and the tick (1): each interrupt reads RETI, the address it
 * interrupted, and counts it in a 64-byte (or larger) bucket.  The period is
 * jittered so samples do not lock to the 5 ms connection events.  Code that
 * runs with interrupts off is counted at the instruction that turns them on. */

extern char text_code_begin[], text_code_end[], _data_code_begin[], _data_code_end[];

#define PROF_FLASH_BUCKETS 4096
#define PROF_RAM_BUCKETS   512
#define PROF_ROM_BUCKETS   512
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

/* ---- USB: mass storage with no medium; vendor commands to usb.c ----------- */

static const struct usb_device_t *volatile usb_pending;
static u8 usb_buf[512] __attribute__((aligned(4)));

static void usb_wakeup(struct usb_device_t *usb_device)
{
    usb_pending = usb_device;
    plat_raise(EV_USB);
}

u32 private_scsi_cmd(const struct usb_device_t *usb_device, struct usb_scsi_cbw *cbw)
{
    const u8 *cdb = &cbw->operationCode;
    u32 len = cbw->dCBWDataTransferLength;

    if (cdb[0] != USB_VENDOR_OP && cdb[0] != 0xfb && cdb[0] != 0xfc && cdb[0] != 0xfd)
        return 0;
    if (len > sizeof(usb_buf)) len = sizeof(usb_buf);
    memset(usb_buf, 0, sizeof(usb_buf));
    if (!(cbw->bmCBWFlags & 0x80)) {
        /* Host to pad: the data comes first, a packet at a time. */
        u32 got = 0, n;

        while (got < len) {
            n = msd_usb2mcu(usb_device, usb_buf + got, len - got < 64 ? len - got : 64);
            if (!n) break;
            got += n;
        }
        usb_command(cdb, usb_buf, got, 1);
        return 1;
    }
    usb_command(cdb, usb_buf, len, 0);
    if (len) msd_mcu2usb(usb_device, usb_buf, len);
    return 1;
}

void plat_usb_service(void)
{
    const struct usb_device_t *dev = usb_pending;

    plat_usb_jack();
    if (!dev) return;
    usb_pending = NULL;
    USB_MassStorage(dev);
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
    if (clock_check.state == 1) {
        clock_check.state = 2;
        util_log("clock check: %u host frames in %u ms (sys %u lsb %u timer %u, timer3 con %08x)",
                 clock_check.frames, clock_check.took_ms, clk_get("sys"), clk_get("lsb"),
                 clk_get("timer"), JL_TIMER3->CON);
    }
    return (s32)(ms_now - sof_seen_ms) < USB_SOF_STALE_MS;
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

void plat_usb_tool_mode(void)
{
    usb_device_mode(0, MASSSTORAGE_CLASS);
    msd_set_wakeup_handle(usb_wakeup);
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
    flushinv_dcache((const void *)MASKROM_TOKEN, 16);
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

/* Off the bus for a second first, so the host sees the loader as a new
 * device. */
void plat_reboot_loader(void)
{
    plat_watch_stop();
    plat_audio_off();
    board_to_loader();
    plat_mdelay(100);
    local_irq_disable();
    USB_CON0 = 0;
    USB_CON1 = 0;
    USB_IO_CON0 = BIT(IO_MODE);
    plat_mdelay(1000);
    USB_IO_CON0 = 0;
    plat_mdelay(250);
    while (!board_usb_sense()) plat_mdelay(100);
    plat_enter_loader();
}

void plat_reboot(void)
{
    plat_watch_stop();
    plat_audio_off();
    local_irq_disable();
    USB_CON0 = 0;
    USB_CON1 = 0;
    /* Select GPIO mode for the pins and pulls. With DPPU/DMPU,
     * DPIE/DMIE and DPOUT/DMOUT clear, both lines are driven low until
     * reset, giving the hub time to detach. */
    USB_IO_CON0 = BIT(IO_MODE) | BIT(IO_PU_MODE);
    plat_mdelay(100);
    p33_soft_reset();
}

/* Every pin floats at power-off but the flash, USB and the board's keep list
 * (wake-up and sense pins, as board_latch() set them). */
static void soft_poweroff_pins(void)
{
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
    .osc_type = OSC_TYPE_LRC, .dcdc_port = TCFG_DCDC_PORT_SEL,
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

void board_init(void)
{
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
    CHGBG_EN(0);
    power_set_mode(PWR_LDO15);
}


static void ctl_thread(void *arg)
{
    (void)arg;
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    /* Without this the radio stays at its lowest step, 20 dB below stock. */
    bt_max_pwr_set(10, 5, 8, 6);
    util_log_init();
    board_start();
    usb_init();
    tick_start();
    plat_prof_start(0, 0, 0);    /* the hang watch */
    {
        /* The SDK's setup.c reads why the chip started (bits not decoded
         * yet: compare a power-on, a reboot and a watchdog). */
        extern u8 power_reset_src;

        util_log("boot: reset source %02x", power_reset_src);
    }
    util_log("tick: timer3 at %08x con %08x prd %u", (u32)JL_TIMER3, JL_TIMER3->CON, JL_TIMER3->PRD);
    hci_jieli_init();
    ctl_main();
}

void app_main(void)
{
    task_create(ctl_thread, NULL, "ctl");
}
