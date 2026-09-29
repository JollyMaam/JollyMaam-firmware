/*
 * `debug ps`: the OS's tasks and where the CPU goes.
 *
 * Task list, state, priority and stack headroom come from FreeRTOS's own
 * uxTaskGetSystemState() (the SDK builds tasks.c with the trace facility).
 * Run-time statistics are compiled out, so CPU share is sampled instead: the
 * control clock's timer interrupt notes which task it interrupted
 * (CurrentTCB, the SDK's name for pxCurrentTCB), every tick.
 */

#include <string.h>

#include <system/includes.h>

#include "console.h"
#include "ps.h"

/* The SDK's name for FreeRTOS's pxCurrentTCB. */
extern void *volatile CurrentTCB;

#define PS_MAX_TASKS 16U
/* Its own timer at a period unrelated to the 2 ms control clock and the
 * 10 ms system tick: sampling from the control clock's own interrupt only
 * ever saw the moments between polls. */
#define PS_TIMER JL_TIMER2
#define PS_PERIOD_US 731U

static struct {
    void *task;
    u32 samples;
} ps_samples[PS_MAX_TASKS];
static u32 ps_total;
static u32 ps_other;

static u32 ps_remaining;

void cyclone2_ps_sample(void)
{
    void *task = CurrentTCB;
    unsigned index;

    if (ps_remaining && --ps_remaining == 0U) PS_TIMER->CON = BIT(14);
    ps_total++;
    for (index = 0U; index < PS_MAX_TASKS; index++) {
        if (ps_samples[index].task == task) {
            ps_samples[index].samples++;
            return;
        }
        if (!ps_samples[index].task) {
            ps_samples[index].task = task;
            ps_samples[index].samples = 1U;
            return;
        }
    }
    ps_other++;
}

___interrupt
static void ps_timer_isr(void)
{
    PS_TIMER->CON |= BIT(14);
    cyclone2_ps_sample();
}

/* Sampling costs more than its interrupt: it evicts the poll's code from
 * the flash cache (the poll ran ~1.8x slower while it was on).  So it runs
 * only on request, for a fixed time. */
void cyclone2_ps_sample_for(u32 seconds)
{
    PS_TIMER->CON = BIT(14);
    local_irq_disable();
    memset(ps_samples, 0, sizeof(ps_samples));
    ps_total = 0U;
    ps_other = 0U;
    ps_remaining = seconds * (1000000U / PS_PERIOD_US);
    local_irq_enable();
    PS_TIMER->CNT = 0U;
    PS_TIMER->PRD = 24U * PS_PERIOD_US;
    request_irq(IRQ_TIME2_IDX, 1, ps_timer_isr, 0);
    PS_TIMER->CON = BIT(0) | BIT(3);
}

int cyclone2_ps_sampling(void)
{
    return ps_remaining != 0U;
}

static u32 samples_of(const void *task)
{
    unsigned index;

    for (index = 0U; index < PS_MAX_TASKS; index++)
        if (ps_samples[index].task == task) return ps_samples[index].samples;
    return 0U;
}

static const char *state_name(int state)
{
    static const char *const names[] = {
        "running", "ready", "blocked", "suspended", "deleted",
    };

    return state >= 0 && state < 5 ? names[state] : "?";
}

/* Named in task_info_table (main.c); SDK-internal tasks show up once the
 * sampler has seen them run. */
extern const struct task_info task_info_table[];

static void print_task(void *task, const char *fallback, void *idle)
{
    const char *name = task ? pcTaskGetName(task) : fallback;
    u32 samples = task ? samples_of(task) : 0U;
    u32 total = ps_total ? ps_total : 1U;

    console_printf("%-10s %-9s %4lu %3u.%u%s\r\n", name ? name : "?",
                   task ? state_name((int)eTaskGetState(task)) : "absent",
                   task ? (unsigned long)uxTaskPriorityGet(task) : 0UL,
                   (unsigned)(samples * 100U / total),
                   (unsigned)(samples * 1000U / total % 10U),
                   task == idle ? "  (idle)" : "");
}

/* uxTaskGetSystemState() is not used: it walks every task's stack for the
 * high-water mark, and that resets this chip (as uxTaskGetStackHighWaterMark
 * does, docs/power-reset.md). */
void cyclone2_ps_print(void)
{
    TaskHandle_t idle = xTaskGetIdleTaskHandle();
    const struct task_info *info;
    unsigned index;

    console_printf("%-10s %-9s prio cpu%%  (%u samples%s, %u tasks)\r\n",
                   "task", "state", (unsigned)ps_total,
                   ps_remaining ? ", still sampling" : "",
                   (unsigned)uxTaskGetNumberOfTasks());
    for (info = task_info_table; info->name; info++)
        print_task(xTaskGetHandle(info->name), info->name, idle);
    for (index = 0U; index < PS_MAX_TASKS && ps_samples[index].task; index++) {
        void *task = ps_samples[index].task;
        int named = 0;

        for (info = task_info_table; info->name; info++)
            if (xTaskGetHandle(info->name) == task) named = 1;
        if (!named) print_task(task, "?", idle);
    }
    if (ps_other)
        console_printf("more tasks than the sampler tracks: %u samples\r\n",
                       (unsigned)ps_other);
}
