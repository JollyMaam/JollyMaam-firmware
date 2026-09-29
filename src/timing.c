#include <string.h>

#include <asm/includes.h>
#include <system/includes.h>

#include "input.h"
#include "timing.h"

/* The SDK's tick counter: one per 10 ms system tick. */
extern volatile unsigned long jiffies;

#define TICK_US 10000U
#define PROBE_RING 128U

u32 cyclone2_time_us(void)
{
    static u32 last;
    u32 ticks;
    u32 count;
    u32 period;
    u32 now;

    do {
        ticks = jiffies;
        count = q32DSP(0)->TTMR_CNT;
        period = q32DSP(0)->TTMR_PRD;
    } while (ticks != jiffies);
    if (!period) period = 1U;
    now = ticks * TICK_US + (u32)(((u64)count * TICK_US) / period);
    /* The counter wraps before its interrupt has counted the tick when
     * interrupts are held off; do not step back by that tick. */
    if ((s32)(now - last) < 0 && last - now < 2U * TICK_US) now += TICK_US;
    last = now;
    return now;
}

/* --- latency probe ----------------------------------------------------- */

static struct {
    u8 active;
    u8 state;
    u8 pending;
    u16 index;
    u32 edges_left;
    u32 half_us;
    u32 base_us;
    u32 next_us;
    u32 arrival_us;
    u32 random;
    u32 lost;
} probe;

/* In RAM bank 1: the main bank's .bss sits right below the Bluetooth
 * controller's own RAM, and growing it pushed the controller's LE state over
 * the MaskROM boot token at 0x10800 (`uboot` then rebooted into the app). */
#define BANK1 __attribute__((section(".bss1")))

static struct cyclone2_probe_record probe_ring[PROBE_RING] BANK1;
static volatile u16 probe_head;
static volatile u16 probe_tail;

static u32 probe_random(void)
{
    probe.random = probe.random * 1664525U + 1013904223U;
    return probe.random >> 8;
}

/* Edge k lands in the first half of its slot at a random phase, so edges
 * fall at every position relative to the poll and report ticks. */
static void probe_schedule(void)
{
    probe.next_us = probe.base_us + (u32)probe.index * probe.half_us +
                    probe_random() % (probe.half_us / 2U);
}

int cyclone2_probe_start(u32 edges, u32 period_ms)
{
    if (!edges || period_ms < 40U || period_ms > 2000U) return -1;
    probe.active = 0U;
    probe.state = 0U;
    probe.pending = 0U;
    probe.index = 0U;
    probe.edges_left = edges;
    probe.half_us = period_ms * 500U;
    probe.random ^= cyclone2_time_us();
    probe.lost = 0U;
    probe_head = probe_tail = 0U;
    probe.base_us = cyclone2_time_us() + probe.half_us;
    probe_schedule();
    probe.active = 1U;
    return 0;
}

void cyclone2_probe_stop(void)
{
    probe.active = 0U;
    probe.state = 0U;
    probe.pending = 0U;
}

int cyclone2_probe_active(void)
{
    return probe.active || probe.state;
}

int cyclone2_probe_sample(u32 now_us)
{
    if (!probe.active) return probe.state ? 1 : -1;
    if ((s32)(now_us - probe.next_us) >= 0) {
        if (probe.pending) probe.lost++;
        probe.state ^= 1U;
        probe.arrival_us = probe.next_us;
        probe.pending = 1U;
        probe.index++;
        if (--probe.edges_left == 0U) {
            probe.active = 0U;
        } else {
            probe_schedule();
        }
    }
    return probe.state;
}

void cyclone2_probe_sent(u16 buttons, u32 now_us)
{
    struct cyclone2_probe_record *record;
    u16 next;

    if (!probe.pending ||
        ((buttons & CYCLONE2_REPORT_BUTTON_A) != 0U) != probe.state) return;
    probe.pending = 0U;
    next = (u16)((probe_head + 1U) % PROBE_RING);
    if (next == probe_tail) {
        probe.lost++;
        return;
    }
    record = &probe_ring[probe_head];
    record->index = (u16)(probe.index - 1U);
    record->value = probe.state;
    record->reserved = 0U;
    record->arrival_us = probe.arrival_us;
    record->sent_us = now_us;
    probe_head = next;
}

int cyclone2_probe_take(struct cyclone2_probe_record *record)
{
    if (probe_tail == probe_head) return 0;
    *record = probe_ring[probe_tail];
    probe_tail = (u16)((probe_tail + 1U) % PROBE_RING);
    return 1;
}

u32 cyclone2_probe_lost(void)
{
    return probe.lost;
}

/* --- control clock ---------------------------------------------------- */

#define CONTROL_TIMER JL_TIMER3
#define CONTROL_OSC_HZ 24000000U

static void (*control_poll)(void *);
static volatile u8 control_queued;
static u32 control_skipped;

static void control_run(void *unused)
{
    (void)unused;
    control_queued = 0U;
    control_poll(NULL);
}

___interrupt
static void control_timer_isr(void)
{
    int message[3];

    CONTROL_TIMER->CON |= BIT(14);
    if (control_queued) {
        control_skipped++;
        return;
    }
    message[0] = (int)control_run;
    message[1] = 1;
    message[2] = 0;
    if (os_taskq_post_type("app_core", Q_CALLBACK, 3, message) == 0)
        control_queued = 1U;
    else
        control_skipped++;
}

void cyclone2_control_clock_start(void (*poll)(void *), u32 period_us)
{
    control_poll = poll;
    CONTROL_TIMER->CON = BIT(14);
    CONTROL_TIMER->CNT = 0U;
    CONTROL_TIMER->PRD = (CONTROL_OSC_HZ / 1000000U) * period_us;
    request_irq(IRQ_TIME3_IDX, 1, control_timer_isr, 0);
    /* Count mode, the oscillator as source (bit 3), no prescaler. */
    CONTROL_TIMER->CON = BIT(0) | BIT(3);
}

u32 cyclone2_control_clock_skipped(void)
{
    return control_skipped;
}

/* --- report timing ----------------------------------------------------- */

static struct cyclone2_timing timing BANK1;
static u32 last_poll_us;
static u32 last_send_us;
static u32 second_start_us;
static u16 second_count;

void cyclone2_timing_reset(void)
{
    memset(&timing, 0, sizeof(timing));
    timing.poll_min_us = 0xffffffffU;
    timing.send_min_us = 0xffffffffU;
    last_poll_us = 0U;
    last_send_us = 0U;
}

void cyclone2_timing_poll(u32 now_us)
{
    u32 interval = now_us - last_poll_us;

    if (last_poll_us) {
        if (interval < timing.poll_min_us) timing.poll_min_us = interval;
        if (interval > timing.poll_max_us) timing.poll_max_us = interval;
    }
    timing.poll_count++;
    last_poll_us = now_us;
    if (now_us - second_start_us >= 1000000U) {
        timing.last_second_hz = second_count;
        second_count = 0U;
        second_start_us = now_us;
    }
}

void cyclone2_timing_poll_done(u32 started_us)
{
    u32 busy = cyclone2_time_us() - started_us;

    timing.busy_total_us += busy;
    if (busy > timing.busy_max_us) timing.busy_max_us = busy;
}

void cyclone2_timing_sent(u32 now_us)
{
    u32 interval = now_us - last_send_us;
    u32 bin;

    second_count++;
    if (last_send_us) {
        if (interval < timing.send_min_us) timing.send_min_us = interval;
        if (interval > timing.send_max_us) timing.send_max_us = interval;
        timing.send_total_us += interval;
        timing.send_count++;
        bin = interval / 1000U;
        if (bin >= CYCLONE2_TIMING_BINS) bin = CYCLONE2_TIMING_BINS - 1U;
        timing.send_bins[bin]++;
    }
    last_send_us = now_us;
}

void cyclone2_timing_get(struct cyclone2_timing *out)
{
    *out = timing;
}
