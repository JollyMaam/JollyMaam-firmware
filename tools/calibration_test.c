/*
 * Host check for the stick and trigger calibration maths (src/calibration_math.c).
 *
 *     make calibration-test
 *
 * Simulates what the pad's sensors report during the calibration routine - a
 * stick that rests off the nominal centre, has different gains per side and
 * per axis, and a gate whose diagonals fall short of its cardinals - runs the
 * capture and finish steps on it, and checks that the resulting mapping
 * reads 0 at rest, full scale at the gate in every direction, keeps the
 * direction of a push, and is monotonic along a ray.  Then the same for a
 * trigger, and the rejections.  It never touches the pad.
 */

#define _DEFAULT_SOURCE   /* M_PI */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "calibration_math.h"

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* A simulated stick: rest position, per-side raw reach, and gate shape. */
struct sim_stick {
    double rest_x, rest_y;
    double reach_neg_x, reach_pos_x, reach_neg_y, reach_pos_y;
    double diagonal_shortfall; /* 0.08 = diagonals reach 92 % */
};

/* Unit-circle direction (angle) at physical deflection `amount` of the gate,
 * turned into raw sensor counts. */
static void sim_raw(const struct sim_stick *s, double angle, double amount,
                    unsigned short *rx, unsigned short *ry)
{
    double gate = 1.0 - s->diagonal_shortfall * fabs(sin(2.0 * angle));
    double x = cos(angle) * amount * gate;
    double y = sin(angle) * amount * gate;

    *rx = (unsigned short)lround(s->rest_x + x * (x < 0 ? s->reach_neg_x : s->reach_pos_x));
    *ry = (unsigned short)lround(s->rest_y + y * (y < 0 ? s->reach_neg_y : s->reach_pos_y));
}

static int noise(int amplitude)
{
    return rand() % (2 * amplitude + 1) - amplitude;
}

static enum cal_result calibrate(const struct sim_stick *s, struct cal_stick *out,
                                 int revolutions, int rest_noise)
{
    struct cal_stick_capture capture;
    enum cal_result result;
    unsigned short rx, ry;
    int i;

    cal_stick_capture_begin(&capture);
    for (i = 0; i < 300; i++) {
        cal_stick_rest_sample(&capture,
                              (unsigned short)(s->rest_x + noise(rest_noise)),
                              (unsigned short)(s->rest_y + noise(rest_noise)));
    }
    result = cal_stick_rest_end(&capture);
    if (result != CAL_OK) return result;
    for (i = 0; i < revolutions * 400; i++) {
        double angle = 2.0 * M_PI * i / 400.0;
        /* A user pressing against the gate varies a little. */
        sim_raw(s, angle, 0.985 + 0.015 * sin(i * 0.37), &rx, &ry);
        cal_stick_sweep_sample(&capture, rx, ry);
    }
    return cal_stick_finish(&capture, out);
}

static void test_stick(const char *name, const struct sim_stick *s)
{
    struct cal_stick cal;
    enum cal_result result = calibrate(s, &cal, 3, 0x20);
    unsigned short rx, ry;
    int32_t x, y;
    double worst_edge = 1.0, worst_angle = 0.0;
    int step, k;

    printf("%s: %s, centre %04x/%04x, inner dz %.1f %%, gate", name,
           cal_result_name(result), cal.center[0], cal.center[1],
           cal.inner_deadzone * 100.0 / CAL_STICK_MAX);
    for (k = 0; k < (int)CAL_GATE_KNOTS; k++) printf(" %.2f", cal.gate[k] / 32767.0);
    printf("\n");
    CHECK(result == CAL_OK, "%s: calibration failed: %s", name, cal_result_name(result));
    if (result != CAL_OK) return;
    CHECK(cal_stick_is_sane(&cal), "%s: result not sane", name);

    for (step = 0; step < 100; step++) {
        cal_stick_map(&cal, (unsigned short)(s->rest_x + noise(0x20)),
                      (unsigned short)(s->rest_y + noise(0x20)), &x, &y);
        CHECK(x == 0 && y == 0, "%s: rest reads %d,%d", name, x, y);
    }

    for (step = 0; step < 360; step++) {
        double angle = 2.0 * M_PI * step / 360.0;
        double r, err;

        /* 97 % of the gate is a firm push: it must read full scale. */
        sim_raw(s, angle, 0.97, &rx, &ry);
        cal_stick_map(&cal, rx, ry, &x, &y);
        r = sqrt((double)x * x + (double)y * y) / CAL_STICK_MAX;
        if (r < worst_edge) worst_edge = r;
        CHECK(r > 0.985 && r <= 1.0001, "%s: edge at %d deg reads radius %.3f",
              name, step, r);

        /* Direction is kept at half deflection. */
        sim_raw(s, angle, 0.5, &rx, &ry);
        cal_stick_map(&cal, rx, ry, &x, &y);
        err = atan2(y, x) - angle;
        while (err > M_PI) err -= 2 * M_PI;
        while (err < -M_PI) err += 2 * M_PI;
        if (fabs(err) > worst_angle) worst_angle = fabs(err);
        CHECK(fabs(err) < 3.0 * M_PI / 180.0, "%s: %d deg reads %.1f deg", name,
              step, (angle + err) * 180.0 / M_PI);
        r = sqrt((double)x * x + (double)y * y) / CAL_STICK_MAX;
        CHECK(fabs(r - 0.5) < 0.06, "%s: half push at %d deg reads %.3f", name,
              step, r);
    }

    for (step = 0; step < 16; step++) {
        double angle = 2.0 * M_PI * step / 16.0;
        double last = -1.0, r;
        int i;

        for (i = 0; i <= 120; i++) {
            sim_raw(s, angle, i / 100.0, &rx, &ry);
            cal_stick_map(&cal, rx, ry, &x, &y);
            r = sqrt((double)x * x + (double)y * y);
            CHECK(r + 1.5 >= last, "%s: not monotonic at %d deg, %d %%", name,
                  step * 360 / 16, i);
            CHECK(abs(x) <= CAL_STICK_MAX && abs(y) <= CAL_STICK_MAX,
                  "%s: out of range %d,%d", name, x, y);
            last = r;
        }
    }
    printf("%s: weakest edge %.3f, worst direction error %.2f deg\n", name,
           worst_edge, worst_angle * 180.0 / M_PI);
}

static void test_stick_rejections(void)
{
    struct sim_stick s = {0x4000, 0x4000, 0x2a00, 0x2a00, 0x2a00, 0x2a00, 0.0};
    struct cal_stick cal;
    struct cal_stick_capture capture;
    int i;

    CHECK(calibrate(&s, &cal, 3, 0x200) == CAL_MOVED_AT_REST,
          "a stick moving at rest was accepted");

    /* Only pushed left/right/up/down, never round the edge. */
    cal_stick_capture_begin(&capture);
    for (i = 0; i < 100; i++) cal_stick_rest_sample(&capture, 0x4000, 0x4000);
    CHECK(cal_stick_rest_end(&capture) == CAL_OK, "clean rest rejected");
    for (i = 0; i < 4; i++) {
        unsigned short rx, ry;
        sim_raw(&s, i * M_PI / 2.0, 1.0, &rx, &ry);
        cal_stick_sweep_sample(&capture, rx, ry);
    }
    CHECK(cal_stick_finish(&capture, &cal) == CAL_NOT_CIRCLED,
          "cardinal pushes alone were accepted");

    /* Not moved at all. */
    cal_stick_capture_begin(&capture);
    for (i = 0; i < 100; i++) cal_stick_rest_sample(&capture, 0x4000, 0x4000);
    cal_stick_rest_end(&capture);
    CHECK(cal_stick_finish(&capture, &cal) == CAL_SHORT_RANGE,
          "an untouched stick was accepted");

    cal_stick_capture_begin(&capture);
    for (i = 0; i < 100; i++) cal_stick_rest_sample(&capture, 0x6000, 0x4000);
    CHECK(cal_stick_rest_end(&capture) == CAL_BAD_CENTER,
          "a stick held over during rest was accepted");
}

static void test_default_stick(void)
{
    static const uint16_t minimum[2] = {0x1600, 0x1850};
    static const uint16_t center[2] = {0x4000, 0x4000};
    static const uint16_t maximum[2] = {0x6600, 0x6600};
    struct cal_stick cal;
    int32_t x, y;

    cal_stick_default(&cal, minimum, center, maximum);
    CHECK(cal_stick_is_sane(&cal), "default not sane");
    cal_stick_map(&cal, 0x4000, 0x4000, &x, &y);
    CHECK(x == 0 && y == 0, "default rest reads %d,%d", x, y);
    cal_stick_map(&cal, 0x6600, 0x4000, &x, &y);
    CHECK(x == CAL_STICK_MAX && y == 0, "default right reads %d,%d", x, y);
    cal_stick_map(&cal, 0x4000, 0x1850, &x, &y);
    CHECK(x == 0 && y == -CAL_STICK_MAX, "default up reads %d,%d", x, y);
    cal_stick_map(&cal, 0xffff, 0x0000, &x, &y);
    CHECK(abs(x) <= CAL_STICK_MAX && abs(y) <= CAL_STICK_MAX,
          "default corner out of range %d,%d", x, y);
}

static void test_trigger(void)
{
    struct cal_trigger_capture capture;
    struct cal_trigger cal;
    uint16_t last = 0, value;
    int i;

    cal_trigger_capture_begin(&capture);
    for (i = 0; i < 300; i++) cal_trigger_rest_sample(&capture, (uint16_t)(880 + noise(3)));
    CHECK(cal_trigger_rest_end(&capture) == CAL_OK, "trigger rest rejected");
    for (i = 0; i < 400; i++) {
        double press = 0.5 - 0.5 * cos(i * 0.05);
        cal_trigger_sweep_sample(&capture, (uint16_t)lround(880 - press * 610), 0);
    }
    CHECK(cal_trigger_finish(&capture, &cal) == CAL_OK, "trigger sweep rejected");
    printf("trigger: rest %u full %u inner dz %.1f %%\n", cal.rest, cal.full,
           cal.inner_deadzone * 100.0 / CAL_STICK_MAX);
    CHECK(cal_trigger_is_sane(&cal), "trigger result not sane");
    for (i = 0; i < 100; i++) {
        value = cal_trigger_map(&cal, (uint16_t)(880 + noise(3)));
        CHECK(value == 0, "trigger rest reads %u", value);
    }
    CHECK(cal_trigger_map(&cal, 880 - 600) == CAL_TRIGGER_MAX,
          "a firm press does not read full");
    CHECK(cal_trigger_map(&cal, 0) == CAL_TRIGGER_MAX, "past full is not full");
    value = cal_trigger_map(&cal, 880 - 305);
    CHECK(value > 30000 && value < 36000, "half press reads %u", value);
    for (i = 1023; i >= 0; i--) {
        value = cal_trigger_map(&cal, (uint16_t)i);
        CHECK(value >= last, "trigger not monotonic at %d", i);
        last = value;
    }

    cal_trigger_capture_begin(&capture);
    for (i = 0; i < 100; i++) cal_trigger_rest_sample(&capture, 880);
    cal_trigger_rest_end(&capture);
    cal_trigger_sweep_sample(&capture, 800, 1);
    CHECK(cal_trigger_finish(&capture, &cal) == CAL_STOP_ENGAGED,
          "a sweep with the stop closed was accepted");

    cal_trigger_capture_begin(&capture);
    for (i = 0; i < 100; i++) cal_trigger_rest_sample(&capture, 880);
    cal_trigger_rest_end(&capture);
    cal_trigger_sweep_sample(&capture, 850, 0);
    CHECK(cal_trigger_finish(&capture, &cal) == CAL_SHORT_RANGE,
          "a trigger barely pressed was accepted");
}

static void test_isqrt_and_angle(void)
{
    uint32_t v;

    for (v = 0; v < 200000; v++) {
        uint32_t r = cal_isqrt(v);
        CHECK(r * r <= v && (r + 1) * (r + 1) > v, "isqrt(%u) = %u", v, r);
    }
    CHECK(cal_isqrt(4294967295U) == 65535U, "isqrt(max)");
    CHECK(cal_pseudo_angle(100, 0) == 0, "angle +x");
    CHECK(cal_pseudo_angle(100, 100) == CAL_ANGLE_FULL / 8, "angle diagonal");
    CHECK(cal_pseudo_angle(0, 100) == CAL_ANGLE_FULL / 4, "angle +y");
    CHECK(cal_pseudo_angle(-100, 0) == CAL_ANGLE_FULL / 2, "angle -x");
    CHECK(cal_pseudo_angle(0, -100) == 3 * CAL_ANGLE_FULL / 4, "angle -y");
    CHECK(cal_pseudo_angle(100, -1) < CAL_ANGLE_FULL, "angle wrap");
}

int main(void)
{
    static const struct sim_stick ideal = {
        0x4000, 0x4000, 0x2a00, 0x2a00, 0x2a00, 0x2a00, 0.0};
    /* Off centre, unequal sides and axes, short diagonals. */
    static const struct sim_stick worn = {
        0x4180, 0x3e60, 0x2800, 0x2650, 0x2480, 0x2900, 0.08};
    /* The gate's corners reach further than its cardinals. */
    static const struct sim_stick squarish = {
        0x3f00, 0x4090, 0x2a00, 0x2b00, 0x2700, 0x2a40, -0.10};

    srand(1);
    test_isqrt_and_angle();
    test_default_stick();
    test_stick("ideal", &ideal);
    test_stick("worn", &worn);
    test_stick("squarish", &squarish);
    test_stick_rejections();
    test_trigger();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("calibration: all checks passed\n");
    return 0;
}
