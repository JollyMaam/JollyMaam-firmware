#ifndef CYCLONE2_CALIBRATION_H
#define CYCLONE2_CALIBRATION_H

#include "generic/typedef.h"
#include "calibration_math.h"
#include "input.h"

/* Bits of cyclone2_calibration.measured: which controls carry values from the
 * calibration routine rather than the built-in defaults. */
#define CYCLONE2_CAL_LEFT_STICK     (1U << 0)
#define CYCLONE2_CAL_RIGHT_STICK    (1U << 1)
#define CYCLONE2_CAL_LEFT_TRIGGER   (1U << 2)
#define CYCLONE2_CAL_RIGHT_TRIGGER  (1U << 3)

/* The syscfg record (CFG_CYCLONE2_CALIBRATION).  Stick 0 is the left stick;
 * trigger 0 is L2. */
struct cyclone2_calibration {
    u32 magic;
    u16 version;
    u16 size;
    u8 measured;
    u8 reserved[3];
    struct cal_stick stick[2];
    struct cal_trigger trigger[2];
};

enum cyclone2_calibration_phase {
    CYCLONE2_CAL_IDLE,
    CYCLONE2_CAL_SETTLE,  /* hands off: let the controls come to rest */
    CYCLONE2_CAL_REST,    /* hands off: measuring the rest positions */
    CYCLONE2_CAL_SWEEP,   /* circle the sticks, press the triggers, then A */
    CYCLONE2_CAL_RESULT,  /* showing the outcome on the Home LED */
};

/* Read the record from syscfg; built-in defaults for anything missing. */
void cyclone2_calibration_load(void);
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

void cyclone2_calibration_get(struct cyclone2_calibration *calibration);
/* Forget the measured values, back to the built-in defaults. */
void cyclone2_calibration_reset(void);
/* Dead zones in percent of full scale; triggers selects the triggers
 * instead of the sticks.  Returns -1 for values out of range. */
int cyclone2_calibration_set_deadzones(int triggers, u8 inner_percent,
                                       u8 outer_percent);

/* Changes are written to syscfg by cyclone2_poll() as soon as the radio is
 * connected or off, i.e. not in a transition.  pending says a write is due. */
int cyclone2_calibration_pending(void);
void cyclone2_calibration_flush(void);

#endif
