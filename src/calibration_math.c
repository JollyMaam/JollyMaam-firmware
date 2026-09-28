#include <string.h>

#include "calibration_math.h"

/* The stick board reports each axis around a nominal 0x4000, reaching about
 * 0x1600..0x6600 (src/input.c).  These bounds only reject captures that
 * cannot be right; they are not the calibration. */
#define STICK_RAW_MIDPOINT        0x4000
#define STICK_CENTER_TOLERANCE    0x1000
#define STICK_REST_NOISE_MAX      0x0300U
#define STICK_MIN_HALF_RANGE      0x1000U

#define STICK_EDGE_MIN_RADIUS     0x1400U

#define STICK_NORMALIZED_LIMIT    46340

#define STICK_GATE_MIN            22937U
#define STICK_GATE_MAX            47513U

#define DEADZONE_3_PERCENT        983U
#define DEADZONE_5_PERCENT        1638U
#define DEADZONE_15_PERCENT       4915U
#define DEADZONE_SUM_MAX          16383U

#define TRIGGER_REST_NOISE_MAX    24U
#define TRIGGER_MIN_TRAVEL        96U

const char *cal_result_name(enum cal_result result)
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

uint32_t cal_isqrt(uint32_t value)
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

uint32_t cal_pseudo_angle(int32_t x, int32_t y)
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

void cal_stick_default(struct cal_stick *stick, const uint16_t minimum[2],
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

int cal_stick_is_sane(const struct cal_stick *stick)
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
    return (uint32_t)stick->inner_deadzone + stick->outer_deadzone <=
           DEADZONE_SUM_MAX;
}

/* Per-axis scaling first, which takes out the centre offset and any gain
 * difference between the two sensors.  Then the radius is corrected by the
 * gate's measured reach in that direction, the radial dead zones are cut
 * out, and the result is clamped to the unit circle.  Doing the dead zones on
 * the radius rather than per axis keeps the direction intact: a nearly
 * horizontal push does not snap to exactly horizontal. */
void cal_stick_map(const struct cal_stick *stick, uint16_t raw_x,
                   uint16_t raw_y, int32_t *x, int32_t *y)
{
    int32_t nx = axis_normalize(raw_x, stick->minimum[0], stick->center[0],
                                stick->maximum[0]);
    int32_t ny = axis_normalize(raw_y, stick->minimum[1], stick->center[1],
                                stick->maximum[1]);
    uint32_t radius = cal_isqrt((uint32_t)(nx * nx) + (uint32_t)(ny * ny));
    uint32_t inner = stick->inner_deadzone;
    uint32_t span = CAL_STICK_MAX - inner - stick->outer_deadzone;
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

int cal_trigger_is_sane(const struct cal_trigger *trigger)
{
    return trigger->rest > trigger->full &&
           (uint32_t)(trigger->rest - trigger->full) >= TRIGGER_MIN_TRAVEL / 2U &&
           (uint32_t)trigger->inner_deadzone + trigger->outer_deadzone <=
           DEADZONE_SUM_MAX;
}

uint16_t cal_trigger_map(const struct cal_trigger *trigger, uint16_t raw)
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

void cal_stick_capture_begin(struct cal_stick_capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    capture->rest_low[0] = capture->rest_low[1] = 0xffffU;
}

void cal_stick_rest_sample(struct cal_stick_capture *capture, uint16_t x,
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

enum cal_result cal_stick_rest_end(struct cal_stick_capture *capture)
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

void cal_stick_sweep_sample(struct cal_stick_capture *capture, uint16_t x,
                            uint16_t y)
{
    /* Clamped only so that a corrupt frame cannot overflow the square. */
    int32_t dx = clamp_s32((int32_t)x - capture->center[0], CAL_STICK_MAX);
    int32_t dy = clamp_s32((int32_t)y - capture->center[1], CAL_STICK_MAX);
    uint32_t r2 = (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
    uint32_t knot;

    if (x < capture->low[0]) capture->low[0] = x;
    if (x > capture->high[0]) capture->high[0] = x;
    if (y < capture->low[1]) capture->low[1] = y;
    if (y > capture->high[1]) capture->high[1] = y;

    knot = nearest_knot(cal_pseudo_angle(dx, dy));
    if (r2 > capture->far_r2[knot]) {
        capture->far_r2[knot] = r2;
        capture->far_x[knot] = dx;
        capture->far_y[knot] = dy;
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

enum cal_result cal_stick_finish(const struct cal_stick_capture *capture,
                                 struct cal_stick *stick)
{
    struct cal_stick result;
    uint32_t noise = 0;
    uint32_t half_range = 0xffffU;
    uint32_t inner;
    unsigned axis;
    unsigned knot;

    if (capture->count == 0U) return CAL_NO_SAMPLES;
    for (axis = 0; axis < 2U; axis++) {
        uint32_t below = (uint32_t)(capture->center[axis] - capture->low[axis]);
        uint32_t above = (uint32_t)(capture->high[axis] - capture->center[axis]);

        if (below < STICK_MIN_HALF_RANGE || above < STICK_MIN_HALF_RANGE)
            return CAL_SHORT_RANGE;
        if (below < half_range) half_range = below;
        if (above < half_range) half_range = above;
        if ((uint32_t)(capture->rest_high[axis] - capture->rest_low[axis]) >
            noise)
            noise = capture->rest_high[axis] - capture->rest_low[axis];
    }
    for (knot = 0; knot < CAL_GATE_KNOTS; knot++) {
        if (capture->far_r2[knot] <
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

    inner = DEADZONE_3_PERCENT + 2U * noise * CAL_STICK_MAX / half_range;
    result.inner_deadzone = (uint16_t)(inner > DEADZONE_15_PERCENT ?
                                       DEADZONE_15_PERCENT : inner);
    result.outer_deadzone = DEADZONE_5_PERCENT;
    *stick = result;
    return CAL_OK;
}

void cal_trigger_capture_begin(struct cal_trigger_capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    capture->rest_low = 0xffffU;
}

void cal_trigger_rest_sample(struct cal_trigger_capture *capture,
                             uint16_t raw)
{
    if (capture->count == 0xffffU) return;
    capture->count++;
    capture->sum += raw;
    if (raw < capture->rest_low) capture->rest_low = raw;
    if (raw > capture->rest_high) capture->rest_high = raw;
}

enum cal_result cal_trigger_rest_end(struct cal_trigger_capture *capture)
{
    if (capture->count == 0U) return CAL_NO_SAMPLES;
    capture->rest = (uint16_t)((capture->sum + capture->count / 2U) /
                               capture->count);
    capture->low = capture->rest;
    if ((uint32_t)(capture->rest_high - capture->rest_low) > TRIGGER_REST_NOISE_MAX)
        return CAL_MOVED_AT_REST;
    return CAL_OK;
}

void cal_trigger_sweep_sample(struct cal_trigger_capture *capture,
                              uint16_t raw, int stop_closed)
{
    if (raw < capture->low) capture->low = raw;
    if (stop_closed) capture->stop_seen = 1U;
}

enum cal_result cal_trigger_finish(const struct cal_trigger_capture *capture,
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
