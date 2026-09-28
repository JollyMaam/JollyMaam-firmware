#ifndef CYCLONE2_CALIBRATION_MATH_H
#define CYCLONE2_CALIBRATION_MATH_H

#include <stdint.h>

#define CAL_STICK_MAX        32767
#define CAL_TRIGGER_MAX      65535U

#define CAL_GATE_KNOTS       16U
#define CAL_ANGLE_FULL       4096U
#define CAL_ANGLE_STEP       (CAL_ANGLE_FULL / CAL_GATE_KNOTS)

struct cal_stick {

    uint16_t minimum[2];
    uint16_t center[2];
    uint16_t maximum[2];

    uint16_t gate[CAL_GATE_KNOTS];

    uint16_t inner_deadzone;
    uint16_t outer_deadzone;
};

struct cal_trigger {

    uint16_t rest;
    uint16_t full;

    uint16_t inner_deadzone;
    uint16_t outer_deadzone;
};

struct cal_stick_capture {
    uint32_t sum[2];
    uint16_t count;
    uint16_t rest_low[2];
    uint16_t rest_high[2];
    uint16_t center[2];
    uint16_t low[2];
    uint16_t high[2];

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
    CAL_NO_SAMPLES,
    CAL_MOVED_AT_REST,
    CAL_BAD_CENTER,
    CAL_SHORT_RANGE,
    CAL_NOT_CIRCLED,
    CAL_STOP_ENGAGED,
};

const char *cal_result_name(enum cal_result result);

uint32_t cal_isqrt(uint32_t value);

uint32_t cal_pseudo_angle(int32_t x, int32_t y);

void cal_stick_default(struct cal_stick *stick, const uint16_t minimum[2],
                       const uint16_t center[2], const uint16_t maximum[2]);
int cal_stick_is_sane(const struct cal_stick *stick);

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
