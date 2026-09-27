#ifndef CYCLONE2_CALIBRATION_MATH_H
#define CYCLONE2_CALIBRATION_MATH_H

/*
 * Stick and trigger calibration: the maths only.
 *
 * Deliberately free of SDK includes, like src/wake/wakeup_code.c, so that
 * tools/calibration_test.c can build and run it on the host
 * (`make calibration-test`).  The capture state machine, the syscfg record
 * and the console live in src/calibration.c.  docs/calibration.md explains
 * the method.
 *
 * Output ranges, shared by every profile:
 *   stick    -32767..+32767, 0 at rest, +x right, +y down (HID orientation)
 *   trigger  0 (released)..65535 (fully pressed)
 * Each profile scales these to its own report range (input.h has helpers).
 */

#include <stdint.h>

#define CAL_STICK_MAX        32767
#define CAL_TRIGGER_MAX      65535U
/* Fractions (gate radius, dead zones) use the stick's full scale as 1.0:
 * 32767 means "all the way". */
/* Gate radius knots around the circle.  Knot k sits at pseudo-angle
 * k * CAL_ANGLE_STEP; cal_pseudo_angle() puts the cardinals on knots
 * 0/4/8/12 and the diagonals on 2/6/10/14. */
#define CAL_GATE_KNOTS       16U
#define CAL_ANGLE_FULL       4096U
#define CAL_ANGLE_STEP       (CAL_ANGLE_FULL / CAL_GATE_KNOTS)

struct cal_stick {
    /* Raw sensor values, per axis (0 = x, 1 = y). */
    uint16_t minimum[2];
    uint16_t center[2];
    uint16_t maximum[2];
    /* How far from the centre the stick reaches in each direction, after the
     * per-axis scaling above, as a fraction of full scale.  A perfectly circular
     * gate reads 32767 everywhere; a gate whose diagonals fall short reads
     * less there.  The mapping divides by it, so every direction reaches
     * full output at the gate. */
    uint16_t gate[CAL_GATE_KNOTS];
    /* Radial dead zones as fractions of the gate radius: below inner the stick
     * reads 0, above 1 - outer it reads full scale. */
    uint16_t inner_deadzone;
    uint16_t outer_deadzone;
};

struct cal_trigger {
    /* Raw ADC values; this pad's sensors fall as the trigger is pressed. */
    uint16_t rest;
    uint16_t full;
    /* Fractions of the travel between rest and full. */
    uint16_t inner_deadzone;
    uint16_t outer_deadzone;
};

/* What was seen while the user left the controls alone, and then while they
 * swept them. */
struct cal_stick_capture {
    uint32_t sum[2];
    uint16_t count;
    uint16_t rest_low[2];
    uint16_t rest_high[2];
    uint16_t center[2];
    uint16_t low[2];
    uint16_t high[2];
    /* The farthest point seen near each knot, relative to the centre. */
    int32_t far_x[CAL_GATE_KNOTS];
    int32_t far_y[CAL_GATE_KNOTS];
    uint32_t far_r2[CAL_GATE_KNOTS];
};

struct cal_trigger_capture {
    uint32_t sum;
    uint16_t count;
    uint16_t rest_low;
    uint16_t rest_high;
    uint16_t rest;
    uint16_t low;
    uint8_t stop_seen;
};

enum cal_result {
    CAL_OK = 0,
    CAL_NO_SAMPLES,      /* nothing was captured at all */
    CAL_MOVED_AT_REST,   /* the control moved while it should have been left alone */
    CAL_BAD_CENTER,      /* rest position implausibly far from the sensor's centre */
    CAL_SHORT_RANGE,     /* not moved far enough (stick axis, trigger travel) */
    CAL_NOT_CIRCLED,     /* the stick was not taken round its whole edge */
    CAL_STOP_ENGAGED,    /* the trigger stop closed during the sweep */
};

const char *cal_result_name(enum cal_result result);

uint32_t cal_isqrt(uint32_t value);
/* A monotonic stand-in for atan2 over 0..CAL_ANGLE_FULL-1 that needs no
 * trigonometry: 0 at +x, a quarter of the range at +y, and exactly the eighths
 * at the diagonals. */
uint32_t cal_pseudo_angle(int32_t x, int32_t y);

void cal_stick_default(struct cal_stick *stick, const uint16_t minimum[2],
                       const uint16_t center[2], const uint16_t maximum[2]);
int cal_stick_is_sane(const struct cal_stick *stick);
/* Raw sensor values to -32767..32767 per axis, without orientation. */
void cal_stick_map(const struct cal_stick *stick, uint16_t raw_x,
                   uint16_t raw_y, int32_t *x, int32_t *y);

int cal_trigger_is_sane(const struct cal_trigger *trigger);
uint16_t cal_trigger_map(const struct cal_trigger *trigger, uint16_t raw);

void cal_stick_capture_begin(struct cal_stick_capture *capture);
void cal_stick_rest_sample(struct cal_stick_capture *capture, uint16_t x,
                           uint16_t y);
enum cal_result cal_stick_rest_end(struct cal_stick_capture *capture);
void cal_stick_sweep_sample(struct cal_stick_capture *capture, uint16_t x,
                            uint16_t y);
enum cal_result cal_stick_finish(const struct cal_stick_capture *capture,
                                 struct cal_stick *stick);

void cal_trigger_capture_begin(struct cal_trigger_capture *capture);
void cal_trigger_rest_sample(struct cal_trigger_capture *capture,
                             uint16_t raw);
enum cal_result cal_trigger_rest_end(struct cal_trigger_capture *capture);
void cal_trigger_sweep_sample(struct cal_trigger_capture *capture,
                              uint16_t raw, int stop_closed);
enum cal_result cal_trigger_finish(const struct cal_trigger_capture *capture,
                                   struct cal_trigger *trigger);

#endif
