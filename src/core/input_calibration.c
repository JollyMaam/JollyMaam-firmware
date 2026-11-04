#include <stdint.h>
#include <string.h>
#include "pad.h"

/* Stick and trigger calibration: the maths, applying it, and the routine
 * that measures it (Home+L3 or Home+R3, settings_ux.c).  The routine keeps its
 * own progress; the controller starts it, feeds it raw input, and stores what
 * it returns. */

#define CAL_STICK_MAX    32767
#define CAL_TRIGGER_MAX  65535U
#define CAL_ANGLE_FULL   4096U
#define CAL_ANGLE_STEP   (CAL_ANGLE_FULL / CAL_GATE_KNOTS)

struct cal_stick_capture {
    uint32_t sum[2];
    uint16_t count;
    uint16_t rest_low[2], rest_high[2], center[2], low[2], high[2];
    int16_t far_x[CAL_GATE_KNOTS], far_y[CAL_GATE_KNOTS];   /* within CAL_STICK_MAX */
};

struct cal_trigger_capture {
    uint32_t sum;
    uint16_t count, rest_low, rest_high, rest, low;
    uint8_t stop_seen;
};

enum cal_result { CAL_OK, CAL_NO_SAMPLES, CAL_MOVED_AT_REST, CAL_BAD_CENTER,
                  CAL_SHORT_RANGE, CAL_NOT_CIRCLED, CAL_STOP_ENGAGED };

#define STICK_RAW_MIDPOINT        0x4000
#define STICK_CENTER_TOLERANCE    0x1000
#define STICK_REST_NOISE_MAX      0x0300U
#define STICK_MIN_HALF_RANGE      0x1000U

#define STICK_EDGE_MIN_RADIUS     0x1400U

#define STICK_NORMALIZED_LIMIT    46340

#define STICK_GATE_MIN            22937U
#define STICK_GATE_MAX            47513U

#define DEADZONE_3_PERCENT        983U
#define DEADZONE_15_PERCENT       4915U
#define DEADZONE_SUM_MAX          16383U

#define TRIGGER_REST_NOISE_MAX    24U
#define TRIGGER_MIN_TRAVEL        96U

static const char *cal_result_name(enum cal_result result)
{
    switch (result) {
    case CAL_OK: return "ok";
    case CAL_NO_SAMPLES: return "no samples";
    case CAL_MOVED_AT_REST: return "moved while it should rest";
    case CAL_BAD_CENTER: return "rest position out of range";
    case CAL_SHORT_RANGE: return "not moved far enough";
    case CAL_NOT_CIRCLED: return "edge not followed all the way round";
    case CAL_STOP_ENGAGED: return "trigger stop engaged";
    }
    return "?";
}

static uint32_t cal_isqrt(uint32_t value)
{
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;

    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static uint32_t cal_pseudo_angle(int32_t x, int32_t y)
{
    uint32_t ax = (uint32_t)(x < 0 ? -x : x);
    uint32_t ay = (uint32_t)(y < 0 ? -y : y);
    uint32_t sum = ax + ay;
    uint32_t quarter = CAL_ANGLE_FULL / 4U;

    if (sum == 0U) return 0U;
    if (y >= 0) {
        if (x >= 0) return ay * quarter / sum;
        return quarter + ax * quarter / sum;
    }
    if (x < 0) return 2U * quarter + ay * quarter / sum;
    return (3U * quarter + ax * quarter / sum) % CAL_ANGLE_FULL;
}

static uint32_t nearest_knot(uint32_t angle)
{
    return ((angle + CAL_ANGLE_STEP / 2U) / CAL_ANGLE_STEP) % CAL_GATE_KNOTS;
}

static int32_t clamp_s32(int32_t value, int32_t limit)
{
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

static int32_t axis_normalize(uint16_t raw, uint16_t minimum, uint16_t center,
                              uint16_t maximum)
{
    int32_t value;

    if (raw < center) {
        value = -(int32_t)(((uint32_t)(center - raw) * CAL_STICK_MAX) /
                           (uint32_t)(center - minimum));
    } else {
        value = (int32_t)(((uint32_t)(raw - center) * CAL_STICK_MAX) /
                          (uint32_t)(maximum - center));
    }
    return clamp_s32(value, STICK_NORMALIZED_LIMIT);
}

static uint32_t gate_at(const struct cal_stick *stick, uint32_t angle)
{
    uint32_t knot = angle / CAL_ANGLE_STEP;
    uint32_t fraction = angle % CAL_ANGLE_STEP;
    uint32_t here = stick->gate[knot];
    uint32_t next = stick->gate[(knot + 1U) % CAL_GATE_KNOTS];

    return (here * (CAL_ANGLE_STEP - fraction) + next * fraction) /
           CAL_ANGLE_STEP;
}

static void cal_stick_default(struct cal_stick *stick, const uint16_t minimum[2],
                       const uint16_t center[2], const uint16_t maximum[2])
{
    unsigned index;

    memset(stick, 0, sizeof(*stick));
    for (index = 0; index < 2U; index++) {
        stick->minimum[index] = minimum[index];
        stick->center[index] = center[index];
        stick->maximum[index] = maximum[index];
    }
    for (index = 0; index < CAL_GATE_KNOTS; index++) {
        stick->gate[index] = CAL_STICK_MAX;
    }
}

static int cal_stick_is_sane(const struct cal_stick *stick)
{
    unsigned index;

    for (index = 0; index < 2U; index++) {
        if (stick->minimum[index] >= stick->center[index] ||
            stick->center[index] >= stick->maximum[index]) return 0;
    }
    for (index = 0; index < CAL_GATE_KNOTS; index++) {
        if (stick->gate[index] < STICK_GATE_MIN ||
            stick->gate[index] > STICK_GATE_MAX) return 0;
    }
    return 1;
}

static void cal_stick_map(const struct cal_stick *stick, uint16_t raw_x,
                   uint16_t raw_y, uint32_t inner, uint32_t outer,
                   int32_t *x, int32_t *y)
{
    int32_t nx = axis_normalize(raw_x, stick->minimum[0], stick->center[0],
                                stick->maximum[0]);
    int32_t ny = axis_normalize(raw_y, stick->minimum[1], stick->center[1],
                                stick->maximum[1]);
    uint32_t radius = cal_isqrt((uint32_t)(nx * nx) + (uint32_t)(ny * ny));
    uint32_t span = CAL_STICK_MAX - inner - outer;
    uint32_t scaled;
    uint32_t output;

    if (radius == 0U) {
        *x = 0;
        *y = 0;
        return;
    }
    scaled = radius * CAL_STICK_MAX / gate_at(stick, cal_pseudo_angle(nx, ny));
    if (scaled <= inner) {
        *x = 0;
        *y = 0;
        return;
    }
    output = (scaled - inner) * CAL_STICK_MAX / span;
    if (output > CAL_STICK_MAX) output = CAL_STICK_MAX;
    *x = nx * (int32_t)output / (int32_t)radius;
    *y = ny * (int32_t)output / (int32_t)radius;
}

static int cal_trigger_is_sane(const struct cal_trigger *trigger)
{
    return trigger->rest > trigger->full &&
           (uint32_t)(trigger->rest - trigger->full) >= TRIGGER_MIN_TRAVEL / 2U &&
           (uint32_t)trigger->inner_deadzone + trigger->outer_deadzone <=
           DEADZONE_SUM_MAX;
}

static uint16_t cal_trigger_map(const struct cal_trigger *trigger, uint16_t raw)
{
    uint32_t travel = trigger->rest - trigger->full;
    uint32_t inner = trigger->inner_deadzone;
    uint32_t span = CAL_STICK_MAX - inner - trigger->outer_deadzone;
    uint32_t fraction;
    uint32_t output;

    if (raw >= trigger->rest) return 0U;
    fraction = (uint32_t)(trigger->rest - raw) * CAL_STICK_MAX / travel;
    if (fraction > CAL_STICK_MAX) fraction = CAL_STICK_MAX;
    if (fraction <= inner) return 0U;
    output = (fraction - inner) * CAL_TRIGGER_MAX / span;
    return output > CAL_TRIGGER_MAX ? (uint16_t)CAL_TRIGGER_MAX :
                                      (uint16_t)output;
}

static void cal_stick_capture_begin(struct cal_stick_capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    capture->rest_low[0] = capture->rest_low[1] = 0xffffU;
}

static void cal_stick_rest_sample(struct cal_stick_capture *capture, uint16_t x,
                           uint16_t y)
{
    uint16_t value[2] = {x, y};
    unsigned axis;

    if (capture->count == 0xffffU) return;
    capture->count++;
    for (axis = 0; axis < 2U; axis++) {
        capture->sum[axis] += value[axis];
        if (value[axis] < capture->rest_low[axis])
            capture->rest_low[axis] = value[axis];
        if (value[axis] > capture->rest_high[axis])
            capture->rest_high[axis] = value[axis];
    }
}

static enum cal_result cal_stick_rest_end(struct cal_stick_capture *capture)
{
    unsigned axis;
    int32_t offset;

    if (capture->count == 0U) return CAL_NO_SAMPLES;
    for (axis = 0; axis < 2U; axis++) {
        capture->center[axis] = (uint16_t)((capture->sum[axis] +
                                            capture->count / 2U) /
                                           capture->count);
        capture->low[axis] = capture->center[axis];
        capture->high[axis] = capture->center[axis];
        if ((uint32_t)(capture->rest_high[axis] - capture->rest_low[axis]) >
            STICK_REST_NOISE_MAX) return CAL_MOVED_AT_REST;
        offset = (int32_t)capture->center[axis] - STICK_RAW_MIDPOINT;
        if (offset > STICK_CENTER_TOLERANCE ||
            offset < -STICK_CENTER_TOLERANCE) return CAL_BAD_CENTER;
    }
    return CAL_OK;
}

static uint32_t far_r2(const struct cal_stick_capture *capture, uint32_t knot)
{
    int32_t x = capture->far_x[knot], y = capture->far_y[knot];

    return (uint32_t)(x * x) + (uint32_t)(y * y);
}

static void cal_stick_sweep_sample(struct cal_stick_capture *capture, uint16_t x,
                            uint16_t y)
{
    int32_t dx = clamp_s32((int32_t)x - capture->center[0], CAL_STICK_MAX);
    int32_t dy = clamp_s32((int32_t)y - capture->center[1], CAL_STICK_MAX);
    uint32_t r2 = (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
    uint32_t knot;

    if (x < capture->low[0]) capture->low[0] = x;
    if (x > capture->high[0]) capture->high[0] = x;
    if (y < capture->low[1]) capture->low[1] = y;
    if (y > capture->high[1]) capture->high[1] = y;

    knot = nearest_knot(cal_pseudo_angle(dx, dy));
    if (r2 > far_r2(capture, knot)) {
        capture->far_x[knot] = (int16_t)dx;
        capture->far_y[knot] = (int16_t)dy;
    }
}

static void fill_gate_gaps(uint16_t gate[CAL_GATE_KNOTS])
{
    uint16_t filled[CAL_GATE_KNOTS];
    unsigned knot;
    unsigned before;
    unsigned after;

    memcpy(filled, gate, sizeof(filled));
    for (knot = 0; knot < CAL_GATE_KNOTS; knot++) {
        if (gate[knot] != 0U) continue;
        for (before = 1; before < CAL_GATE_KNOTS; before++) {
            if (gate[(knot + CAL_GATE_KNOTS - before) % CAL_GATE_KNOTS]) break;
        }
        for (after = 1; after < CAL_GATE_KNOTS; after++) {
            if (gate[(knot + after) % CAL_GATE_KNOTS]) break;
        }
        filled[knot] = (uint16_t)(
            ((uint32_t)gate[(knot + CAL_GATE_KNOTS - before) % CAL_GATE_KNOTS] *
                 after +
             (uint32_t)gate[(knot + after) % CAL_GATE_KNOTS] * before) /
            (before + after));
    }
    memcpy(gate, filled, sizeof(filled));
}

static enum cal_result cal_stick_finish(const struct cal_stick_capture *capture,
                                 struct cal_stick *stick)
{
    struct cal_stick result;
    unsigned axis;
    unsigned knot;

    if (capture->count == 0U) return CAL_NO_SAMPLES;
    for (axis = 0; axis < 2U; axis++) {
        uint32_t below = (uint32_t)(capture->center[axis] - capture->low[axis]);
        uint32_t above = (uint32_t)(capture->high[axis] - capture->center[axis]);

        if (below < STICK_MIN_HALF_RANGE || above < STICK_MIN_HALF_RANGE)
            return CAL_SHORT_RANGE;
    }
    for (knot = 0; knot < CAL_GATE_KNOTS; knot++) {
        if (far_r2(capture, knot) <
            STICK_EDGE_MIN_RADIUS * STICK_EDGE_MIN_RADIUS)
            return CAL_NOT_CIRCLED;
    }

    memset(&result, 0, sizeof(result));
    for (axis = 0; axis < 2U; axis++) {
        result.minimum[axis] = capture->low[axis];
        result.center[axis] = capture->center[axis];
        result.maximum[axis] = capture->high[axis];
    }

    for (knot = 0; knot < CAL_GATE_KNOTS; knot++) {
        int32_t nx = axis_normalize(
            (uint16_t)(capture->center[0] + capture->far_x[knot]),
            result.minimum[0], result.center[0], result.maximum[0]);
        int32_t ny = axis_normalize(
            (uint16_t)(capture->center[1] + capture->far_y[knot]),
            result.minimum[1], result.center[1], result.maximum[1]);
        uint32_t radius = cal_isqrt((uint32_t)(nx * nx) + (uint32_t)(ny * ny));
        uint32_t at = nearest_knot(cal_pseudo_angle(nx, ny));

        if (radius > result.gate[at]) result.gate[at] = (uint16_t)radius;
    }
    fill_gate_gaps(result.gate);
    for (knot = 0; knot < CAL_GATE_KNOTS; knot++) {
        if (result.gate[knot] < STICK_GATE_MIN)
            result.gate[knot] = STICK_GATE_MIN;
        if (result.gate[knot] > STICK_GATE_MAX)
            result.gate[knot] = STICK_GATE_MAX;
    }

    *stick = result;
    return CAL_OK;
}

static void cal_trigger_capture_begin(struct cal_trigger_capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    capture->rest_low = 0xffffU;
}

static void cal_trigger_rest_sample(struct cal_trigger_capture *capture,
                             uint16_t raw)
{
    if (capture->count == 0xffffU) return;
    capture->count++;
    capture->sum += raw;
    if (raw < capture->rest_low) capture->rest_low = raw;
    if (raw > capture->rest_high) capture->rest_high = raw;
}

static enum cal_result cal_trigger_rest_end(struct cal_trigger_capture *capture)
{
    if (capture->count == 0U) return CAL_NO_SAMPLES;
    capture->rest = (uint16_t)((capture->sum + capture->count / 2U) /
                               capture->count);
    capture->low = capture->rest;
    if ((uint32_t)(capture->rest_high - capture->rest_low) > TRIGGER_REST_NOISE_MAX)
        return CAL_MOVED_AT_REST;
    return CAL_OK;
}

static void cal_trigger_sweep_sample(struct cal_trigger_capture *capture,
                              uint16_t raw, int stop_closed)
{
    if (raw < capture->low) capture->low = raw;
    if (stop_closed) capture->stop_seen = 1U;
}

static enum cal_result cal_trigger_finish(const struct cal_trigger_capture *capture,
                                   struct cal_trigger *trigger)
{
    uint32_t travel;
    uint32_t inner;

    if (capture->count == 0U) return CAL_NO_SAMPLES;

    if (capture->stop_seen) return CAL_STOP_ENGAGED;
    travel = capture->rest - capture->low;
    if (capture->low >= capture->rest || travel < TRIGGER_MIN_TRAVEL)
        return CAL_SHORT_RANGE;
    trigger->rest = capture->rest;
    trigger->full = capture->low;
    inner = DEADZONE_3_PERCENT +
            2U * (uint32_t)(capture->rest_high - capture->rest_low) *
            CAL_STICK_MAX / travel;
    trigger->inner_deadzone = (uint16_t)(inner > DEADZONE_15_PERCENT ?
                                         DEADZONE_15_PERCENT : inner);
    trigger->outer_deadzone = DEADZONE_3_PERCENT;
    return CAL_OK;
}

/* ---- applying -------------------------------------------------------------- */

/* Uncalibrated sticks: the measured ranges of this pad's sensors. */
static const u16 default_min[2][2] = { { 0x1600, 0x1850 }, { 0x1600, 0x1a40 } };
static const u16 default_center[2] = { 0x4000, 0x4000 };
static const u16 default_max[2] = { 0x6600, 0x6600 };
static const s8 stick_sign[2] = { 1, -1 };

/* An uncalibrated trigger follows its own rest position.  The rest only
 * creeps up, one step per reading after the first: a single stray ADC
 * sample (one of 735 against a rest of 482 left L2 half pressed until
 * reboot) moves it by one, not for good.  Such jumps are logged. */
#define LEGACY_JUMP 64

static u16 legacy_rest[2];
static u8 legacy_jumps[2];

static u16 legacy_trigger(u16 raw, u16 *rest)
{
    u32 travel, moved;

    if (raw > *rest && raw <= 0x3ff) {
        if (*rest && raw >= *rest + LEGACY_JUMP && legacy_jumps[rest - legacy_rest] < 8) {
            legacy_jumps[rest - legacy_rest]++;
            util_log("input: %s reading %u above rest %u", rest == legacy_rest ? "L2" : "R2", raw, *rest);
        }
        *rest = *rest ? (u16)(*rest + 1) : raw;
    }
    if (*rest < 64 || raw + 8 >= *rest) return 0;
    moved = *rest - raw - 8;
    travel = *rest * 3 / 4;
    return moved >= travel ? CAL_TRIGGER_MAX : (u16)(moved * CAL_TRIGGER_MAX / travel);
}

void cal_apply(const struct set_cal *cal, const struct cal_deadzone *deadzone,
               const struct in_raw *raw, struct pad *pad)
{
    static const struct cal_deadzone none[2];
    struct cal_stick def;
    int32_t x, y;
    u32 i;
    s16 *out[2][2] = { { &pad->lx, &pad->ly }, { &pad->rx, &pad->ry } };

    for (i = 0; i < 2; i++) {
        const struct cal_stick *s = &cal->stick[i];

        x = y = 0;
        if (!cal_stick_is_sane(s)) {
            cal_stick_default(&def, default_min[i], default_center, default_max);
            s = &def;
        }
        if (raw->sticks_valid)
            cal_stick_map(s, raw->stick[2 * i], raw->stick[2 * i + 1],
                          (uint32_t)(deadzone ? deadzone : none)[i].inner * CAL_STICK_MAX / 100,
                          (uint32_t)(deadzone ? deadzone : none)[i].outer * CAL_STICK_MAX / 100, &x, &y);
        *out[i][0] = (s16)(x * stick_sign[i]);
        *out[i][1] = (s16)(y * stick_sign[i]);
    }
    pad->l2 = cal_trigger_is_sane(&cal->trigger[0]) ? cal_trigger_map(&cal->trigger[0], raw->trigger[0])
                                                    : legacy_trigger(raw->trigger[0], &legacy_rest[0]);
    pad->r2 = cal_trigger_is_sane(&cal->trigger[1]) ? cal_trigger_map(&cal->trigger[1], raw->trigger[1])
                                                    : legacy_trigger(raw->trigger[1], &legacy_rest[1]);
    if (raw->trigger_stop[0]) pad->l2 = CAL_TRIGGER_MAX;
    if (raw->trigger_stop[1]) pad->r2 = CAL_TRIGGER_MAX;
}

/* ---- the routine: hands off, rest, sweep, result ---------------------------- */

#define SETTLE_MS   1000
#define REST_MS     1500
#define SWEEP_MS    120000
#define RESULT_MS   1500
#define SUCCESS_ON_MS 1000
#define FAILURE_BLINK_MS 100
#define FAILURE_BLINKS 3
#define LED_LEVEL   12

enum { PH_IDLE, PH_SETTLE, PH_REST, PH_SWEEP, PH_RESULT };

static const char *const names[4] = { "left stick", "right stick", "L2", "R2" };

static struct {
    u8 phase;
    u32 since;
    u8 released;
    struct rgb result;
    enum cal_result outcome[4];
    struct cal_stick_capture stick[2];
    struct cal_trigger_capture trigger[2];
} r;

static void enter(u8 phase, u32 now)
{
    r.phase = phase;
    r.since = now;
}

static void show(u8 red, u8 green, u32 now)
{
    r.result = (struct rgb){ red, green, 0 };
    enter(PH_RESULT, now);
}

void cal_start(u32 now)
{
    if (r.phase != PH_IDLE && r.phase != PH_RESULT) return;
    util_log("cal: hands off the sticks and triggers");
    enter(PH_SETTLE, now);
}

static void cal_cancel(u32 now)
{
    if (r.phase == PH_IDLE || r.phase == PH_RESULT) return;
    util_log("cal: cancelled; calibration unchanged");
    enter(PH_IDLE, now);         /* no result light: nothing went wrong */
}

static int finish(struct set_cal *cal, u32 now)
{
    struct cal_stick s;
    struct cal_trigger t;
    u32 i, applied = 0;

    for (i = 0; i < 2; i++) {
        if (r.outcome[i] == CAL_OK) r.outcome[i] = cal_stick_finish(&r.stick[i], &s);
        if (r.outcome[i] == CAL_OK) {
            cal->stick[i] = s;
            applied++;
        }
        if (r.outcome[2 + i] == CAL_OK) r.outcome[2 + i] = cal_trigger_finish(&r.trigger[i], &t);
        if (r.outcome[2 + i] == CAL_OK) {
            cal->trigger[i] = t;
            applied++;
        }
    }
    for (i = 0; i < 4; i++)
        if (r.outcome[i] != CAL_OK) util_log("cal: %s unchanged: %s", names[i], cal_result_name(r.outcome[i]));
    util_log("cal: done, %u of 4 calibrated", applied);
    if (applied == 4) show(0, LED_LEVEL, now);
    else show(LED_LEVEL, 0, now);
    return applied ? CAL_SAVE : CAL_RUNNING;
}

/* One step with the latest raw input.  CAL_SAVE: *cal was updated and should
 * be stored.  Buttons: south or east finishes the sweep (whichever is "accept"
 * to the user: Xbox or Nintendo habit), Home (which started it) cancels. */
int cal_step(const struct in_raw *raw, u32 now, struct set_cal *cal)
{
    u32 t = now - r.since, i;
    u16 ab = raw->buttons & (PAD_SOUTH | PAD_EAST | PAD_HOME);

    switch (r.phase) {
    case PH_SETTLE:
        if (t >= SETTLE_MS) {
            for (i = 0; i < 2; i++) {
                cal_stick_capture_begin(&r.stick[i]);
                cal_trigger_capture_begin(&r.trigger[i]);
            }
            enter(PH_REST, now);
        }
        return CAL_RUNNING;
    case PH_REST:
        for (i = 0; i < 2; i++) {
            if (raw->sticks_valid) cal_stick_rest_sample(&r.stick[i], raw->stick[2 * i], raw->stick[2 * i + 1]);
            cal_trigger_rest_sample(&r.trigger[i], raw->trigger[i]);
        }
        if (t >= REST_MS) {
            for (i = 0; i < 2; i++) {
                r.outcome[i] = cal_stick_rest_end(&r.stick[i]);
                r.outcome[2 + i] = cal_trigger_rest_end(&r.trigger[i]);
            }
            for (i = 0; i < 4; i++)
                if (r.outcome[i] != CAL_OK) util_log("cal: %s at rest: %s", names[i], cal_result_name(r.outcome[i]));
            r.released = !ab;
            util_log("cal: circle both sticks along the edge, press L2 and R2 fully; A or B done, Home cancel");
            enter(PH_SWEEP, now);
        }
        return CAL_RUNNING;
    case PH_SWEEP:
        for (i = 0; i < 2; i++) {
            if (raw->sticks_valid) cal_stick_sweep_sample(&r.stick[i], raw->stick[2 * i], raw->stick[2 * i + 1]);
            cal_trigger_sweep_sample(&r.trigger[i], raw->trigger[i], raw->trigger_stop[i]);
        }
        if (!ab) r.released = 1;
        else if (r.released && (ab & PAD_HOME)) cal_cancel(now);
        else if (r.released) return finish(cal, now);
        if (t >= SWEEP_MS) {
            util_log("cal: timed out; calibration unchanged");
            show(LED_LEVEL, 0, now);
        }
        return CAL_RUNNING;
    case PH_RESULT:
        if (t >= RESULT_MS) r.phase = PH_IDLE;
        return r.phase == PH_IDLE ? CAL_IDLE : CAL_RUNNING;
    default:
        return CAL_IDLE;
    }
}

int cal_led(u32 now, struct rgb *out)
{
    switch (r.phase) {
    case PH_SETTLE:
    case PH_REST:
        *out = (struct rgb){ 0, 0, LED_LEVEL };
        return 1;
    case PH_SWEEP:
        *out = (struct rgb){ 0, 0, (u8)((now / 250) & 1 ? 0 : LED_LEVEL) };
        return 1;
    case PH_RESULT: {
        u32 t = now - r.since;
        int lit = r.result.g ? t < SUCCESS_ON_MS :
                  t < 2 * FAILURE_BLINK_MS * FAILURE_BLINKS &&
                  (t / FAILURE_BLINK_MS) % 2 == 0;

        *out = lit ? r.result : (struct rgb){ 0, 0, 0 };
        return 1;
    }
    default:
        return 0;
    }
}

#ifdef UNITTEST
void util_log(const char *fmt, ...) { (void)fmt; }

int main(void)
{
    struct cal_stick s;
    struct cal_trigger t = { 900, 300, 0, 0 };
    struct cal_stick_capture c;
    struct cal_trigger_capture tc;
    struct set_cal cal;
    struct cal_deadzone dz[2] = { { 0, 0 }, { 0, 0 } };
    struct in_raw raw;
    struct pad pad;
    int32_t x, y;
    u32 i, now;

    cal_stick_default(&s, default_min[0], default_center, default_max);
    CHECK(cal_stick_is_sane(&s));
    cal_stick_map(&s, 0x4000, 0x4000, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    cal_stick_map(&s, 0x6600, 0x4000, 0, 0, &x, &y);
    CHECK(x == CAL_STICK_MAX && y == 0);
    cal_stick_map(&s, 0x1600, 0x4000, 0, 0, &x, &y);
    CHECK(x == -CAL_STICK_MAX);
    CHECK(cal_trigger_is_sane(&t));
    CHECK(cal_trigger_map(&t, 900) == 0 && cal_trigger_map(&t, 300) == CAL_TRIGGER_MAX);
    CHECK(cal_isqrt(1000000) == 1000);

    cal_stick_capture_begin(&c);
    for (i = 0; i < 100; i++) cal_stick_rest_sample(&c, 0x4000 + (i & 1), 0x4000);
    CHECK(cal_stick_rest_end(&c) == CAL_OK);
    for (i = 0; i < 4096; i += 8) {
        int32_t a = (int32_t)i;
        int32_t dx = a < 1024 ? 1024 - a : a < 2048 ? -(a - 1024) : a < 3072 ? -(3072 - a) : a - 3072;
        int32_t dy = a < 2048 ? 1024 - (a < 1024 ? 1024 - a : a - 1024) : -(1024 - (a < 3072 ? 3072 - a : a - 3072));
        cal_stick_sweep_sample(&c, (u16)(0x4000 + dx * 8), (u16)(0x4000 + dy * 8));
    }
    CHECK(cal_stick_finish(&c, &s) == CAL_OK && cal_stick_is_sane(&s));

    cal_trigger_capture_begin(&tc);
    for (i = 0; i < 50; i++) cal_trigger_rest_sample(&tc, 880);
    CHECK(cal_trigger_rest_end(&tc) == CAL_OK);
    cal_trigger_sweep_sample(&tc, 200, 0);
    CHECK(cal_trigger_finish(&tc, &t) == CAL_OK && t.rest == 880 && t.full == 200);

    memset(&cal, 0, sizeof(cal));
    memset(&raw, 0, sizeof(raw));
    raw.sticks_valid = 1;
    raw.stick[0] = raw.stick[1] = raw.stick[2] = raw.stick[3] = 0x4000;
    raw.stick[2] = 0x6600;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.lx == 0 && pad.rx == -CAL_STICK_MAX);

    /* Stored sensor measurements ignore the old embedded stick deadzones;
     * only the independently configured percentages affect response. */
    cal_stick_default(&cal.stick[0], default_min[0], default_center, default_max);
    cal.stick[0].legacy_inner_deadzone = 4915;
    cal.stick[0].legacy_outer_deadzone = 1638;
    raw.stick[0] = 0x4100;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.lx > 0 && !pad.ly);
    dz[0].inner = 5;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(!pad.lx && !pad.ly);
    raw.stick[0] = 0x6400;
    dz[0].inner = 0;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.lx < STICK_MAX);
    dz[0].outer = 10;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.lx == STICK_MAX);
    dz[0].outer = 0;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.lx < STICK_MAX); /* runtime change requires no recalibration */

    /* Uncalibrated trigger: one stray high reading does not leave it pressed. */
    raw.trigger[0] = 482;
    for (i = 0; i < 4; i++) cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.l2 == 0 && legacy_rest[0] == 482);
    raw.trigger[0] = 735;
    cal_apply(&cal, dz, &raw, &pad);
    raw.trigger[0] = 482;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.l2 == 0 && legacy_jumps[0] == 1);
    raw.trigger[0] = 100;
    cal_apply(&cal, dz, &raw, &pad);
    CHECK(pad.l2 == CAL_TRIGGER_MAX);

    now = 0;
    cal_start(now);
    CHECK(cal_step(&raw, now, &cal) == CAL_RUNNING);
    for (now = 0; now < SETTLE_MS + REST_MS + 10; now += 5) cal_step(&raw, now, &cal);
    raw.buttons = PAD_HOME;         /* still held from the gesture: ignored */
    cal_step(&raw, now, &cal);
    raw.buttons = 0;
    cal_step(&raw, now + 5, &cal);
    raw.buttons = PAD_HOME;
    cal_step(&raw, now + 10, &cal);
    CHECK(cal_step(&raw, now + 15, &cal) == CAL_IDLE && !cal_led(now + 15, &(struct rgb){ 0 }));
    cal_start(now);
    for (i = 0; i < SETTLE_MS + REST_MS + SWEEP_MS + 20; i += 5) {
        cal_step(&raw, now + i, &cal);
        if (i == SETTLE_MS + REST_MS + SWEEP_MS - 5) CHECK(r.phase == PH_SWEEP);
    }
    CHECK(r.phase == PH_RESULT && r.result.r && !r.result.g);
    {
        struct rgb led;
        struct set_cal before = cal;
        u32 started = r.since;

        /* Timeout leaves saved values alone and shows exactly three flashes. */
        for (i = 0; i < RESULT_MS; i += FAILURE_BLINK_MS) {
            CHECK(cal_led(started + i, &led));
            CHECK(led.r == (i < 600 && (i / 100) % 2 == 0 ? LED_LEVEL : 0));
            CHECK(!led.g && !led.b);
        }
        CHECK(cal_step(&raw, started + RESULT_MS, &cal) == CAL_IDLE);
        CHECK(!cal_led(started + RESULT_MS, &led));
        CHECK(memcmp(&before, &cal, sizeof(cal)) == 0);

        /* All four valid captures produce one slow green blink. */
        memset(r.outcome, 0, sizeof(r.outcome));
        r.stick[0] = r.stick[1] = c;
        r.trigger[0] = r.trigger[1] = tc;
        CHECK(finish(&cal, started) == CAL_SAVE);
        CHECK(cal_led(started, &led) && !led.r && led.g == LED_LEVEL && !led.b);
        CHECK(cal_led(started + 999, &led) && led.g == LED_LEVEL);
        CHECK(cal_led(started + 1000, &led) && !led.r && !led.g && !led.b);

        /* Partial calibration is a failure signal; valid inputs still save
         * and the failed input keeps its previous calibration. */
        before = cal;
        r.outcome[0] = CAL_NOT_CIRCLED;
        cal.stick[0].center[0]++;
        before.stick[0] = cal.stick[0];
        CHECK(finish(&cal, started) == CAL_SAVE);
        CHECK(memcmp(&before.stick[0], &cal.stick[0], sizeof(cal.stick[0])) == 0);
        CHECK(cal_led(started, &led) && led.r == LED_LEVEL && !led.g && !led.b);
        CHECK(cal_led(started + 100, &led) && !led.r && !led.g && !led.b);
        for (i = 0; i < 4; i++) r.outcome[i] = CAL_NO_SAMPLES;
        before = cal;
        CHECK(finish(&cal, started) == CAL_RUNNING);
        CHECK(memcmp(&before, &cal, sizeof(cal)) == 0);
        CHECK(cal_led(started, &led) && led.r == LED_LEVEL && !led.g);
    }
    return TEST_DONE("input_calibration");
}
#endif
