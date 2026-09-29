/*
 * `debug bounce [SECONDS]`: how long the button contacts bounce.
 *
 * Samples the four GPIO ports' input registers in a tight loop (a few
 * register loads per pass, far finer than the control poll) and records
 * every change of a button or d-pad line with its microsecond time.  Edges
 * of one line closer than BOUNCE_GAP_US belong to one actuation; the time
 * from its first to its last edge is that actuation's bounce.
 *
 * It holds app_core for the whole capture: no reports go out meanwhile, and
 * it is kept to 3 s so the idle task still clears the 4 s watchdog.  The
 * trigger end-stop switches are not covered (RTC-domain pins, ~30 us a read).
 */

#include <string.h>

#include <asm/includes.h>
#include <system/includes.h>

#include "bounce.h"
#include "console.h"
#include "input.h"
#include "timing.h"

#define BOUNCE_MAX_EDGES 1024U
#define BOUNCE_GAP_US    20000U

static const struct {
    const char *name;
    u32 gpio;
} lines[] = {
    { "A", CYCLONE2_BUTTON_A }, { "B", CYCLONE2_BUTTON_B },
    { "X", CYCLONE2_BUTTON_X }, { "Y", CYCLONE2_BUTTON_Y },
    { "L1", CYCLONE2_BUTTON_L1 }, { "R1", CYCLONE2_BUTTON_R1 },
    { "L3", CYCLONE2_BUTTON_L3 }, { "R3", CYCLONE2_BUTTON_R3 },
    { "L4", CYCLONE2_BUTTON_L4 }, { "R4", CYCLONE2_BUTTON_R4 },
    { "select", CYCLONE2_BUTTON_SELECT }, { "start", CYCLONE2_BUTTON_START },
    { "home", CYCLONE2_BUTTON_HOME }, { "capture", CYCLONE2_BUTTON_SCREENSHOT },
    { "M", CYCLONE2_BUTTON_M },
    { "up", CYCLONE2_DPAD_UP }, { "down", CYCLONE2_DPAD_DOWN },
    { "left", CYCLONE2_DPAD_LEFT }, { "right", CYCLONE2_DPAD_RIGHT },
};
#define LINE_COUNT (sizeof(lines) / sizeof(lines[0]))

struct edge {
    u32 time_us;
    u32 state;      /* one bit per line, 1 = pressed */
};

static struct edge edges[BOUNCE_MAX_EDGES];
static u32 initial_state;

static volatile u32 *const port_in[4] = {
    &JL_PORTA->IN, &JL_PORTB->IN, &JL_PORTC->IN, &JL_PORTD->IN,
};

static u32 decode_lines(const u32 ports[4])
{
    u32 state = 0U;
    unsigned index;

    for (index = 0U; index < LINE_COUNT; index++) {
        u32 gpio = lines[index].gpio;

        /* Active low. */
        if (!(ports[(gpio / IO_GROUP_NUM) & 3U] & BIT(gpio % IO_GROUP_NUM)))
            state |= BIT(index);
    }
    return state;
}

static void read_ports(u32 ports[4])
{
    ports[0] = *port_in[0];
    ports[1] = *port_in[1];
    ports[2] = *port_in[2];
    ports[3] = *port_in[3];
}

/* Only the pins the lines use: the ports carry other signals too. */
static void port_masks(u32 masks[4])
{
    unsigned index;

    memset(masks, 0, 4U * sizeof(u32));
    for (index = 0U; index < LINE_COUNT; index++)
        masks[(lines[index].gpio / IO_GROUP_NUM) & 3U] |=
            BIT(lines[index].gpio % IO_GROUP_NUM);
}

/* Every edge: time since the first edge (us), gap to the previous edge of
 * the same line (us), the line and its new level. */
static void print_raw(u32 count)
{
    u32 last_time[LINE_COUNT];
    u32 levels = 0U;
    u32 index;
    unsigned line;

    memset(last_time, 0, sizeof(last_time));
    levels = initial_state;
    console_printf("%10s %10s  line level\r\n", "t_us", "gap_us");
    for (index = 0U; index < count; index++) {
        for (line = 0U; line < LINE_COUNT; line++) {
            if (!(edges[index].state & BIT(line))) continue;
            levels ^= BIT(line);
            if (last_time[line])
                console_printf("%10u %10u  %-7s %s\r\n",
                               (unsigned)(edges[index].time_us - edges[0].time_us),
                               (unsigned)(edges[index].time_us - last_time[line]),
                               lines[line].name,
                               levels & BIT(line) ? "pressed" : "released");
            else
                console_printf("%10u %10s  %-7s %s\r\n",
                               (unsigned)(edges[index].time_us - edges[0].time_us),
                               "-", lines[line].name,
                               levels & BIT(line) ? "pressed" : "released");
            last_time[line] = edges[index].time_us;
        }
    }
}

int cyclone2_bounce_capture(u32 seconds, int raw_output)
{
    u32 start;
    u32 now;
    u32 last_pass;
    u32 passes = 0U;
    u32 max_gap = 0U;
    u32 count = 0U;
    u32 dropped = 0U;
    u32 state;
    u32 previous;
    u32 raw[4];
    u32 masks[4];
    unsigned index;
    unsigned line;

    if (!seconds || seconds > 3U) return -1;
    port_masks(masks);
    read_ports(raw);
    for (index = 0U; index < 4U; index++) raw[index] &= masks[index];
    previous = decode_lines(raw);
    initial_state = previous;
    start = last_pass = cyclone2_time_us();
    /* The clock is read only on a change and every 256 passes: the loop is
     * then four register loads and a compare. */
    for (;;) {
        u32 ports[4];

        ports[0] = *port_in[0] & masks[0];
        ports[1] = *port_in[1] & masks[1];
        ports[2] = *port_in[2] & masks[2];
        ports[3] = *port_in[3] & masks[3];
        passes++;
        if (ports[0] != raw[0] || ports[1] != raw[1] ||
            ports[2] != raw[2] || ports[3] != raw[3]) {
            now = cyclone2_time_us();
            memcpy(raw, ports, sizeof(raw));
            state = decode_lines(raw);
            if (count < BOUNCE_MAX_EDGES) {
                edges[count].time_us = now;
                edges[count].state = state ^ previous;
                count++;
            } else {
                dropped++;
            }
            previous = state;
        } else if (!(passes & 255U)) {
            now = cyclone2_time_us();
            if (now - last_pass > max_gap) max_gap = now - last_pass;
            last_pass = now;
            if (now - start >= seconds * 1000000U) break;
        }
    }

    console_printf("bounce: %u s, %u samples (every %u ns on average; "
                   "longest 256-sample stretch %u us), %u edges%s\r\n",
                   (unsigned)seconds, (unsigned)passes,
                   passes ? (unsigned)((u64)seconds * 1000000000U / passes) : 0U,
                   (unsigned)max_gap, (unsigned)count,
                   dropped ? " (buffer full)" : "");
    if (raw_output) print_raw(count);
    console_printf("%-8s %8s %7s %14s %10s\r\n", "line", "changes",
                   "bouncy", "max_bounce_us", "max_edges");
    for (line = 0U; line < LINE_COUNT; line++) {
        u32 actuations = 0U;
        u32 bouncy = 0U;
        u32 max_bounce = 0U;
        u32 max_edges = 0U;
        u32 first = 0U;
        u32 last = 0U;
        u32 run = 0U;
        u32 index;

        for (index = 0U; index <= count; index++) {
            int edge = index < count && (edges[index].state & BIT(line));

            if (edge && run && edges[index].time_us - last < BOUNCE_GAP_US) {
                last = edges[index].time_us;
                run++;
                continue;
            }
            if (index < count && !edge) continue;
            if (run) {
                /* An actuation ended: first..last is its bounce. */
                actuations++;
                if (run > 1U) bouncy++;
                if (last - first > max_bounce) max_bounce = last - first;
                if (run > max_edges) max_edges = run;
                run = 0U;
            }
            if (edge) {
                first = last = edges[index].time_us;
                run = 1U;
            }
        }
        if (actuations)
            console_printf("%-8s %8u %7u %14u %10u\r\n", lines[line].name,
                           (unsigned)actuations, (unsigned)bouncy,
                           (unsigned)max_bounce, (unsigned)max_edges);
    }
    return 0;
}
