#ifndef CYCLONE2_CALIBRATION_H
#define CYCLONE2_CALIBRATION_H

#include "generic/typedef.h"
#include "calibration_math.h"
#include "input.h"

enum cyclone2_calibration_phase {
    CYCLONE2_CAL_IDLE,
    CYCLONE2_CAL_SETTLE,
    CYCLONE2_CAL_REST,
    CYCLONE2_CAL_SWEEP,
    CYCLONE2_CAL_RESULT,
};

void cyclone2_calibration_apply(const struct cyclone2_raw_input *raw,
                                struct cyclone2_input_report *report);

int cyclone2_calibration_start(void);
void cyclone2_calibration_finish(void);
void cyclone2_calibration_cancel(void);
void cyclone2_calibration_step(u32 now, u16 buttons);
enum cyclone2_calibration_phase cyclone2_calibration_phase(void);
const char *cyclone2_calibration_phase_name(enum cyclone2_calibration_phase phase);

int cyclone2_calibration_led(u32 now, u8 *red, u8 *green, u8 *blue);

#endif
