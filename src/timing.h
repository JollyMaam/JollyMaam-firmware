#ifndef CYCLONE2_TIMING_H
#define CYCLONE2_TIMING_H

#include "generic/typedef.h"

/* Hot path, run from RAM: code executes from SPI flash through a small
 * cache, and the poll's code kept getting evicted (it ran ~1.8x slower
 * with one more interrupt source active). */
#define CYCLONE2_HOT __attribute__((section(".volatile_ram_code")))

/* Microseconds since boot (wraps after ~71 minutes): the system tick count
 * (jiffies, 10 ms each) plus the CPU tick timer's position inside the tick.
 * Any task or interrupt. */
u32 cyclone2_time_us(void);

/* Latency probe: a synthetic A press/release at the GPIO sample, before the
 * debounce, at random instants.  Each edge records when it "happened" and
 * when the report carrying it was handed to the profile, both in
 * cyclone2_time_us(); a host correlates that with its own clock (`debug
 * clock`) and the time the edge reached it. */
struct cyclone2_probe_record {
    u16 index;
    u8 value;
    u8 reserved;
    u32 arrival_us;
    u32 sent_us;
};

int cyclone2_probe_start(u32 edges, u32 period_ms);
void cyclone2_probe_stop(void);
int cyclone2_probe_active(void);
/* Oldest unread record; 0 when there is none. */
int cyclone2_probe_take(struct cyclone2_probe_record *record);
u32 cyclone2_probe_lost(void);

/* From the control poll: the A bit to use for this sample (-1: leave it). */
int cyclone2_probe_sample(u32 now_us);
/* From the control poll, after a report went to the profile. */
void cyclone2_probe_sent(u16 buttons, u32 now_us);

/* Report timing: intervals between polls and between accepted reports. */
#define CYCLONE2_TIMING_BINS 24U
struct cyclone2_timing {
    u32 poll_count;
    u32 poll_min_us;
    u32 poll_max_us;
    /* Time spent inside the poll itself. */
    u32 busy_max_us;
    u64 busy_total_us;
    u32 send_count;
    u32 send_min_us;
    u32 send_max_us;
    u64 send_total_us;
    /* Accepted-report intervals by whole milliseconds; the last bin holds
     * everything longer. */
    u32 send_bins[CYCLONE2_TIMING_BINS];
    /* Accepted reports in the last whole second. */
    u16 last_second_hz;
};

/* Run `poll` on the app_core task every period_us, clocked by hardware
 * timer 3 from the 24 MHz oscillator (the system tick is 10 ms).  A poll
 * still running when the next one is due is not queued twice. */
void cyclone2_control_clock_start(void (*poll)(void *), u32 period_us);

struct cyclone2_control_clock {
    u32 skipped;            /* ticks while the last poll was still queued */
    u32 post_failed;        /* ticks whose post to app_core failed */
    int post_error;         /* the last failure's return value */
    u32 queue_avg_us;       /* timer interrupt -> poll starting */
    u32 queue_max_us;
};
void cyclone2_control_clock_get(struct cyclone2_control_clock *out);

/* Where the poll's time goes: cyclone2_prof_start() at its top, then
 * cyclone2_prof_mark(section) after each part, cyclone2_prof_end(). */
enum cyclone2_prof_section {
    CYCLONE2_PROF_USB,      /* USB console: receive, commands, output */
    CYCLONE2_PROF_CONFIG,   /* settings flash writes */
    CYCLONE2_PROF_RUMBLE,   /* wake step and rumble */
    CYCLONE2_PROF_UART,     /* stick frames from the UART */
    CYCLONE2_PROF_GPIO,     /* buttons and d-pad */
    CYCLONE2_PROF_ADC,      /* trigger ADC values */
    CYCLONE2_PROF_STOPS,    /* trigger end-stop pins (RTC domain) */
    CYCLONE2_PROF_CAL,      /* calibration maths */
    CYCLONE2_PROF_LOGIC,    /* debounce, chords, calibration, host report */
    CYCLONE2_PROF_SEND,     /* handing a report to the profile */
    CYCLONE2_PROF_LED,
    CYCLONE2_PROF_TAIL,     /* timeouts and sleep */
    CYCLONE2_PROF_COUNT,
};
struct cyclone2_profile_stats {
    u32 total_us[CYCLONE2_PROF_COUNT];
    u32 max_us[CYCLONE2_PROF_COUNT];
    u32 count[CYCLONE2_PROF_COUNT];
    u32 polls;
    u32 poll_total_us;
    u32 poll_max_us;
};
void cyclone2_prof_start(void);
void cyclone2_prof_mark(u8 section);
void cyclone2_prof_end(void);
void cyclone2_prof_get(struct cyclone2_profile_stats *out);

void cyclone2_timing_poll(u32 now_us);
void cyclone2_timing_poll_done(u32 started_us);
void cyclone2_timing_sent(u32 now_us);
void cyclone2_timing_get(struct cyclone2_timing *out);
void cyclone2_timing_reset(void);

#endif
