#include <system/includes.h>

#include "calibration.h"
#include "config.h"
#include "usb.h"

#define CAL_SETTLE_MS           1000U
#define CAL_REST_MS             1500U
#define CAL_SWEEP_TIMEOUT_MS    120000U
#define CAL_RESULT_MS           2000U
#define CAL_BLINK_HALF_MS       250U
#define CAL_LED_LEVEL           12U

static const s8 stick_sign[2] = {1, -1};

#define LEGACY_TRIGGER_DEAD_ZONE    8U
#define LEGACY_TRIGGER_TRAVEL_NUM   3U
#define LEGACY_TRIGGER_TRAVEL_DEN   4U

enum {
    CONTROL_LEFT_STICK,
    CONTROL_RIGHT_STICK,
    CONTROL_LEFT_TRIGGER,
    CONTROL_RIGHT_TRIGGER,
    CONTROL_COUNT,
};

static const char *const control_name[CONTROL_COUNT] = {
    "left stick", "right stick", "L2", "R2",
};

/* The values live in config.cal (config_schema.c has the defaults).  A
 * stick whose stored values do not make sense uses its defaults instead. */
static struct cal_stick default_stick[2];
static u8 defaults_ready;
static u16 legacy_trigger_rest[2];

static enum cyclone2_calibration_phase phase;
static u32 phase_started_ms;
static u8 finish_requested;
static u8 buttons_seen_released;
static u8 result_colour[3];
static enum cal_result outcome[CONTROL_COUNT];
static struct cal_stick_capture stick_capture[2];
static struct cal_trigger_capture trigger_capture[2];

static const struct cal_stick *stick_in_use(u8 stick)
{
    if (cal_stick_is_sane(&config.cal.stick[stick]))
        return &config.cal.stick[stick];
    if (!defaults_ready) {
        static struct config defaults;

        config_defaults(&defaults);
        default_stick[0] = defaults.cal.stick[0];
        default_stick[1] = defaults.cal.stick[1];
        defaults_ready = 1U;
    }
    return &default_stick[stick];
}

static u16 legacy_trigger(u16 sample, u16 *rest)
{
    u32 travel;
    u32 displacement;

    if (sample > *rest && sample <= 0x3ffU) *rest = sample;
    if (*rest < 64U || sample + LEGACY_TRIGGER_DEAD_ZONE >= *rest) return 0U;
    displacement = *rest - sample - LEGACY_TRIGGER_DEAD_ZONE;
    travel = (*rest * LEGACY_TRIGGER_TRAVEL_NUM) / LEGACY_TRIGGER_TRAVEL_DEN;
    if (displacement >= travel) return CYCLONE2_TRIGGER_MAX;
    return (u16)((displacement * CYCLONE2_TRIGGER_MAX) / travel);
}

static u16 map_trigger(const struct cyclone2_raw_input *raw, u8 trigger)
{

    if (cal_trigger_is_sane(&config.cal.trigger[trigger]))
        return cal_trigger_map(&config.cal.trigger[trigger],
                               raw->trigger[trigger]);
    return legacy_trigger(raw->trigger[trigger],
                          &legacy_trigger_rest[trigger]);
}

void cyclone2_calibration_apply(const struct cyclone2_raw_input *raw,
                                struct cyclone2_input_report *report)
{
    int32_t x = 0;
    int32_t y = 0;

    if (raw->sticks_valid) {
        cal_stick_map(stick_in_use(0U), raw->stick[0], raw->stick[1],
                      &x, &y);
    }
    report->left_x = (s16)(x * stick_sign[0]);
    report->left_y = (s16)(y * stick_sign[0]);
    x = y = 0;
    if (raw->sticks_valid) {
        cal_stick_map(stick_in_use(1U), raw->stick[2], raw->stick[3],
                      &x, &y);
    }
    report->right_x = (s16)(x * stick_sign[1]);
    report->right_y = (s16)(y * stick_sign[1]);
    report->left_trigger = map_trigger(raw, 0U);
    report->right_trigger = map_trigger(raw, 1U);
}

static void enter_phase(enum cyclone2_calibration_phase next)
{
    phase = next;
    phase_started_ms = sys_timer_get_ms();
}

int cyclone2_calibration_start(void)
{
    if (phase != CYCLONE2_CAL_IDLE && phase != CYCLONE2_CAL_RESULT) return -1;
    finish_requested = 0U;
    enter_phase(CYCLONE2_CAL_SETTLE);
    cyclone2_usb_debug("cal: hands off the sticks and triggers");
    return 0;
}

void cyclone2_calibration_finish(void)
{
    if (phase == CYCLONE2_CAL_SWEEP) finish_requested = 1U;
}

static void show_result(u8 red, u8 green, u8 blue)
{
    result_colour[0] = red;
    result_colour[1] = green;
    result_colour[2] = blue;
    enter_phase(CYCLONE2_CAL_RESULT);
}

void cyclone2_calibration_cancel(void)
{
    if (phase == CYCLONE2_CAL_IDLE || phase == CYCLONE2_CAL_RESULT) return;
    cyclone2_usb_debug("cal: cancelled; calibration unchanged");
    show_result(CAL_LED_LEVEL, 0U, 0U);
}

static void begin_rest(void)
{
    cal_stick_capture_begin(&stick_capture[0]);
    cal_stick_capture_begin(&stick_capture[1]);
    cal_trigger_capture_begin(&trigger_capture[0]);
    cal_trigger_capture_begin(&trigger_capture[1]);
    enter_phase(CYCLONE2_CAL_REST);
}

static void rest_sample(const struct cyclone2_raw_input *raw)
{
    if (raw->sticks_valid) {
        cal_stick_rest_sample(&stick_capture[0], raw->stick[0], raw->stick[1]);
        cal_stick_rest_sample(&stick_capture[1], raw->stick[2], raw->stick[3]);
    }
    cal_trigger_rest_sample(&trigger_capture[0], raw->trigger[0]);
    cal_trigger_rest_sample(&trigger_capture[1], raw->trigger[1]);
}

static void end_rest(u16 buttons)
{
    u8 control;

    outcome[CONTROL_LEFT_STICK] = cal_stick_rest_end(&stick_capture[0]);
    outcome[CONTROL_RIGHT_STICK] = cal_stick_rest_end(&stick_capture[1]);
    outcome[CONTROL_LEFT_TRIGGER] = cal_trigger_rest_end(&trigger_capture[0]);
    outcome[CONTROL_RIGHT_TRIGGER] = cal_trigger_rest_end(&trigger_capture[1]);
    for (control = 0U; control < CONTROL_COUNT; control++) {
        if (outcome[control] != CAL_OK)
            cyclone2_usb_debug("cal: %s at rest: %s", control_name[control],
                               cal_result_name(outcome[control]));
    }

    buttons_seen_released = !(buttons & (CYCLONE2_REPORT_BUTTON_A |
                                         CYCLONE2_REPORT_BUTTON_B));
    enter_phase(CYCLONE2_CAL_SWEEP);
    cyclone2_usb_debug("cal: circle both sticks along the edge 3 times,");
    cyclone2_usb_debug("cal: press L2 and R2 fully (stops off); A = done, B = cancel");
}

static void sweep_sample(const struct cyclone2_raw_input *raw)
{
    u8 trigger;

    if (raw->sticks_valid) {
        cal_stick_sweep_sample(&stick_capture[0], raw->stick[0], raw->stick[1]);
        cal_stick_sweep_sample(&stick_capture[1], raw->stick[2], raw->stick[3]);
    }
    for (trigger = 0U; trigger < 2U; trigger++) {
        cal_trigger_sweep_sample(&trigger_capture[trigger],
                                 raw->trigger[trigger],
                                 raw->trigger_stop[trigger]);
    }
}

static void report_stick(u8 stick)
{
    const struct cal_stick *cal = &config.cal.stick[stick];

    cyclone2_usb_debug("cal: %s ok x %04x/%04x/%04x y %04x/%04x/%04x dz %u%%",
                       control_name[CONTROL_LEFT_STICK + stick],
                       cal->minimum[0], cal->center[0], cal->maximum[0],
                       cal->minimum[1], cal->center[1], cal->maximum[1],
                       (unsigned)((cal->inner_deadzone * 100U +
                                   CAL_STICK_MAX / 2U) / CAL_STICK_MAX));
}

static void finish_sweep(void)
{
    struct cal_stick stick;
    struct cal_trigger trigger;
    u8 applied = 0U;
    u8 index;
    u8 control;

    for (index = 0U; index < 2U; index++) {
        control = CONTROL_LEFT_STICK + index;
        if (outcome[control] == CAL_OK)
            outcome[control] = cal_stick_finish(&stick_capture[index], &stick);
        if (outcome[control] == CAL_OK) {
            config_set_stick_cal(index, &stick);
            applied++;
            report_stick(index);
        }
        control = CONTROL_LEFT_TRIGGER + index;
        if (outcome[control] == CAL_OK)
            outcome[control] = cal_trigger_finish(&trigger_capture[index],
                                                  &trigger);
        if (outcome[control] == CAL_OK) {
            config_set_trigger_cal(index, &trigger);
            legacy_trigger_rest[index] = 0U;
            applied++;
            cyclone2_usb_debug("cal: %s ok rest %u full %u", control_name[control],
                               trigger.rest, trigger.full);
        }
    }
    for (control = 0U; control < CONTROL_COUNT; control++) {
        if (outcome[control] != CAL_OK)
            cyclone2_usb_debug("cal: %s unchanged: %s", control_name[control],
                               cal_result_name(outcome[control]));
    }
    if (applied == CONTROL_COUNT) show_result(0U, CAL_LED_LEVEL, 0U);
    else if (applied) show_result(CAL_LED_LEVEL, CAL_LED_LEVEL / 2U, 0U);
    else show_result(CAL_LED_LEVEL, 0U, 0U);
    cyclone2_usb_debug("cal: done, %u of 4 controls calibrated", applied);
}

void cyclone2_calibration_step(u32 now, u16 buttons)
{
    struct cyclone2_raw_input raw;
    u32 elapsed = now - phase_started_ms;

    if (phase == CYCLONE2_CAL_IDLE) return;
    cyclone2_input_get_raw(&raw);
    switch (phase) {
    case CYCLONE2_CAL_SETTLE:
        if (elapsed >= CAL_SETTLE_MS) begin_rest();
        break;
    case CYCLONE2_CAL_REST:
        rest_sample(&raw);
        if (elapsed >= CAL_REST_MS) end_rest(buttons);
        break;
    case CYCLONE2_CAL_SWEEP:
        sweep_sample(&raw);
        if (!(buttons & (CYCLONE2_REPORT_BUTTON_A | CYCLONE2_REPORT_BUTTON_B))) {
            buttons_seen_released = 1U;
        } else if (buttons_seen_released &&
                   (buttons & CYCLONE2_REPORT_BUTTON_B)) {
            cyclone2_calibration_cancel();
            break;
        } else if (buttons_seen_released) {
            finish_requested = 1U;
        }
        if (finish_requested) {
            finish_requested = 0U;
            finish_sweep();
        } else if (elapsed >= CAL_SWEEP_TIMEOUT_MS) {
            cyclone2_usb_debug("cal: timed out");
            cyclone2_calibration_cancel();
        }
        break;
    case CYCLONE2_CAL_RESULT:
        if (elapsed >= CAL_RESULT_MS) phase = CYCLONE2_CAL_IDLE;
        break;
    default:
        phase = CYCLONE2_CAL_IDLE;
        break;
    }
}

enum cyclone2_calibration_phase cyclone2_calibration_phase(void)
{
    return phase;
}

const char *cyclone2_calibration_phase_name(enum cyclone2_calibration_phase which)
{
    switch (which) {
    case CYCLONE2_CAL_IDLE: return "idle";
    case CYCLONE2_CAL_SETTLE: return "settle";
    case CYCLONE2_CAL_REST: return "rest";
    case CYCLONE2_CAL_SWEEP: return "sweep";
    case CYCLONE2_CAL_RESULT: return "result";
    }
    return "?";
}

int cyclone2_calibration_led(u32 now, u8 *red, u8 *green, u8 *blue)
{
    switch (phase) {
    case CYCLONE2_CAL_SETTLE:
    case CYCLONE2_CAL_REST:
        *red = 0U;
        *green = 0U;
        *blue = CAL_LED_LEVEL;
        return 1;
    case CYCLONE2_CAL_SWEEP:
        *red = 0U;
        *green = 0U;
        *blue = ((now / CAL_BLINK_HALF_MS) & 1U) ? 0U : CAL_LED_LEVEL;
        return 1;
    case CYCLONE2_CAL_RESULT:
        *red = result_colour[0];
        *green = result_colour[1];
        *blue = result_colour[2];
        return 1;
    default:
        return 0;
    }
}
