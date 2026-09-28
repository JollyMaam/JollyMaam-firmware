#ifndef CYCLONE2_CALIBRATION_H
#define CYCLONE2_CALIBRATION_H

#include "generic/typedef.h"
#include "calibration_math.h"
#include "input.h"

enum cyclone2_calibration_phase {
    CYCLONE2_CAL_IDLE,
    CYCLONE2_CAL_SETTLE,  /* hands off: let the controls come to rest */
    CYCLONE2_CAL_REST,    /* hands off: measuring the rest positions */
    CYCLONE2_CAL_SWEEP,   /* circle the sticks, press the triggers, then A */
    CYCLONE2_CAL_RESULT,  /* showing the outcome on the Home LED */
};

/* Raw sensors to the report's sticks and triggers. */
void cyclone2_calibration_apply(const struct cyclone2_raw_input *raw,
                                struct cyclone2_input_report *report);

/* The routine.  start returns 0, or -1 while one is already running.
 * step runs once per input poll with the debounced buttons. */
int cyclone2_calibration_start(void);
void cyclone2_calibration_finish(void);
void cyclone2_calibration_cancel(void);
void cyclone2_calibration_step(u32 now, u16 buttons);
enum cyclone2_calibration_phase cyclone2_calibration_phase(void);
const char *cyclone2_calibration_phase_name(enum cyclone2_calibration_phase phase);
/* While the routine owns the Home LED: its colour, and non-zero. */
int cyclone2_calibration_led(u32 now, u8 *red, u8 *green, u8 *blue);

#endif
