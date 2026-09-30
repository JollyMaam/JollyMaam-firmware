#include <system/includes.h>
#include <btstack/avctp_user.h>

#include "app_comm_bt.h"
#include "edr_hid_user.h"
#include "btstack_jl.h"
#include "btstack_jl_hid_ext.h"
#include "config.h"
#include "controller.h"
#include "gyro.h"
#include "imu.h"
#include "input.h"
#include "profile.h"
#include "profile_classic.h"
#include "timing.h"

/* A Bluetooth Classic keyboard and mouse, for a desktop or a game that
 * wants one.  stickmouse aims with a stick; gyromouse adds the gyro on top
 * of it.  What every control does is the slot's layout
 * (profile.#.variant), one of the tables below. */

#define MOUSE_REPORT_KEYBOARD   0x01U
#define MOUSE_REPORT_MOUSE      0x02U
#define MOUSE_KEYBOARD_SIZE     8U
#define MOUSE_MOUSE_SIZE        7U
#define MOUSE_KEYS              6U

/* HID keyboard usages (HID Usage Tables, page 0x07).  The SDK already has
 * KEY_* names, hence KB_. */
enum mouse_key {
    KB_NONE = 0x00U,
    KB_A = 0x04U, KB_D = 0x07U, KB_E = 0x08U, KB_F = 0x09U,
    KB_Q = 0x14U, KB_R = 0x15U, KB_S = 0x16U, KB_W = 0x1aU,
    KB_ENTER = 0x28U, KB_ESCAPE = 0x29U, KB_BACKSPACE = 0x2aU,
    KB_TAB = 0x2bU, KB_SPACE = 0x2cU, KB_PRINT_SCREEN = 0x46U,
    KB_RIGHT = 0x4fU, KB_LEFT = 0x50U, KB_DOWN = 0x51U, KB_UP = 0x52U,
    /* 0xe0..0xe7 are the modifiers, sent as bits rather than keys. */
    KB_LEFT_CTRL = 0xe0U, KB_LEFT_SHIFT = 0xe1U, KB_LEFT_ALT = 0xe2U,
    KB_LEFT_GUI = 0xe3U,
};

enum mouse_button {
    MOUSE_LEFT = 0x01U,
    MOUSE_RIGHT = 0x02U,
    MOUSE_MIDDLE = 0x04U,
    MOUSE_BACK = 0x08U,
    MOUSE_FORWARD = 0x10U,
};

/* Controls: the report's buttons, then the triggers and the d-pad. */
#define CONTROL_L2              (1UL << 16)
#define CONTROL_R2              (1UL << 17)
#define CONTROL_UP              (1UL << 18)
#define CONTROL_RIGHT           (1UL << 19)
#define CONTROL_DOWN            (1UL << 20)
#define CONTROL_LEFT            (1UL << 21)

/* What a stick does. */
enum mouse_stick {
    STICK_NONE,
    STICK_POINTER,
    STICK_SCROLL,       /* wheel up/down, pan left/right */
    STICK_WASD,
    STICK_ARROWS,
};

/* While a control with one of these is held. */
#define HOLD_SCROLL             0x01U   /* the pointer stick scrolls */
#define HOLD_GYRO_OFF           0x02U   /* the gyro lets go: put the pad back */
#define HOLD_SLOW               0x04U   /* the pointer moves at a quarter */

struct mouse_binding {
    u32 control;
    u8 key;
    u8 mouse;
    u8 hold;
};

struct mouse_layout {
    u8 left_stick;
    u8 right_stick;
    const struct mouse_binding *bindings;
    u8 count;
};

/* Home is in no layout: its chords select slots, and a tap of it would
 * reach the host as a key press after every one of them. */

/* profile.#.variant=default: games.  WASD on the left stick, aim on the
 * right, triggers fire and aim down sights. */
static const struct mouse_binding game_bindings[] = {
    { CYCLONE2_REPORT_BUTTON_A,          .key = KB_SPACE },
    { CYCLONE2_REPORT_BUTTON_B,          .key = KB_LEFT_CTRL },
    { CYCLONE2_REPORT_BUTTON_X,          .key = KB_R },
    { CYCLONE2_REPORT_BUTTON_Y,          .key = KB_E },
    { CYCLONE2_REPORT_BUTTON_L1,         .key = KB_Q },
    { CYCLONE2_REPORT_BUTTON_R1,         .key = KB_F },
    { CYCLONE2_REPORT_BUTTON_L3,         .key = KB_LEFT_SHIFT },
    { CYCLONE2_REPORT_BUTTON_R3,         .mouse = MOUSE_MIDDLE },
    { CONTROL_L2,                        .mouse = MOUSE_RIGHT },
    { CONTROL_R2,                        .mouse = MOUSE_LEFT },
    { CYCLONE2_REPORT_BUTTON_SELECT,     .key = KB_TAB },
    { CYCLONE2_REPORT_BUTTON_START,      .key = KB_ESCAPE },
    { CYCLONE2_REPORT_BUTTON_SCREENSHOT, .key = KB_PRINT_SCREEN },
    { CYCLONE2_REPORT_BUTTON_L4,         .mouse = MOUSE_BACK },
    { CYCLONE2_REPORT_BUTTON_R4,         .mouse = MOUSE_FORWARD },
    { CYCLONE2_REPORT_BUTTON_M,          .hold = HOLD_SCROLL | HOLD_GYRO_OFF },
    { CONTROL_UP,                        .key = KB_UP },
    { CONTROL_RIGHT,                     .key = KB_RIGHT },
    { CONTROL_DOWN,                      .key = KB_DOWN },
    { CONTROL_LEFT,                      .key = KB_LEFT },
};

/* profile.#.variant=desktop: point with the left stick, scroll with the
 * right, click with A and B.  L4 is Alt and R4 Ctrl, to hold for shortcuts
 * (L4 then Select is Alt+Tab). */
static const struct mouse_binding desktop_bindings[] = {
    { CYCLONE2_REPORT_BUTTON_A,          .mouse = MOUSE_LEFT },
    { CYCLONE2_REPORT_BUTTON_B,          .mouse = MOUSE_RIGHT },
    { CYCLONE2_REPORT_BUTTON_X,          .mouse = MOUSE_MIDDLE },
    { CYCLONE2_REPORT_BUTTON_Y,          .key = KB_ENTER },
    { CYCLONE2_REPORT_BUTTON_L1,         .mouse = MOUSE_BACK },
    { CYCLONE2_REPORT_BUTTON_R1,         .mouse = MOUSE_FORWARD },
    { CYCLONE2_REPORT_BUTTON_L3,         .key = KB_LEFT_GUI },
    { CYCLONE2_REPORT_BUTTON_R3,         .key = KB_LEFT_SHIFT },
    { CONTROL_L2,                        .mouse = MOUSE_RIGHT },
    { CONTROL_R2,                        .mouse = MOUSE_LEFT },
    { CYCLONE2_REPORT_BUTTON_SELECT,     .key = KB_TAB },
    { CYCLONE2_REPORT_BUTTON_START,      .key = KB_ESCAPE },
    { CYCLONE2_REPORT_BUTTON_SCREENSHOT, .key = KB_PRINT_SCREEN },
    { CYCLONE2_REPORT_BUTTON_L4,         .key = KB_LEFT_ALT },
    { CYCLONE2_REPORT_BUTTON_R4,         .key = KB_LEFT_CTRL },
    { CYCLONE2_REPORT_BUTTON_M,          .hold = HOLD_SLOW | HOLD_GYRO_OFF },
    { CONTROL_UP,                        .key = KB_UP },
    { CONTROL_RIGHT,                     .key = KB_RIGHT },
    { CONTROL_DOWN,                      .key = KB_DOWN },
    { CONTROL_LEFT,                      .key = KB_LEFT },
};

static const struct mouse_layout mouse_layouts[] = {
    [CONFIG_VARIANT_DEFAULT] = {
        .left_stick = STICK_WASD,
        .right_stick = STICK_POINTER,
        .bindings = game_bindings,
        .count = ARRAY_SIZE(game_bindings),
    },
    [CONFIG_VARIANT_DESKTOP] = {
        .left_stick = STICK_POINTER,
        .right_stick = STICK_SCROLL,
        .bindings = desktop_bindings,
        .count = ARRAY_SIZE(desktop_bindings),
    },
};

/* The triggers are pressed past this much of their travel. */
#define MOUSE_TRIGGER_PRESS     (CYCLONE2_TRIGGER_MAX / 4U)

/* A stick used as keys presses a direction past MOUSE_MOVE_PRESS of its
 * reach and lets go below MOUSE_MOVE_RELEASE. */
#define MOUSE_MOVE_PRESS        (CYCLONE2_STICK_MAX / 2)
#define MOUSE_MOVE_RELEASE      (CYCLONE2_STICK_MAX * 2 / 5)
static const u8 wasd_keys[4] = { KB_W, KB_D, KB_S, KB_A };
static const u8 arrow_keys[4] = { KB_UP, KB_RIGHT, KB_DOWN, KB_LEFT };

/* A scrolling stick: nothing within MOUSE_SCROLL_DEADZONE of the centre on
 * an axis (so an up/down push does not also pan, and a resting stick never
 * creeps a notch), then notches per second rising with the square of the
 * rest of the travel, to MOUSE_SCROLL_PER_SECOND all the way over. */
#define MOUSE_SCROLL_DEADZONE   (CYCLONE2_STICK_MAX * 35 / 100)
#define MOUSE_SCROLL_PER_SECOND 15

/* A report later than this is integrated as if it were this late. */
#define MOUSE_MAX_DT_US         50000U
/* A mouse report the link has not taken within this is replaced anyway,
 * losing its motion: the link has gone, and must not hold the pointer. */
#define MOUSE_TAKE_MAX_MS       100U

static const u8 mouse_report_descriptor[] = {
    /* Keyboard, report 1: modifiers, reserved, six keys; LEDs out. */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, MOUSE_REPORT_KEYBOARD,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x75, 0x01, 0x95, 0x05, 0x91, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x91, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, MOUSE_KEYS, 0x81, 0x00,
    0xc0,
    /* Mouse, report 2: five buttons, 16-bit x and y, wheel, AC Pan. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, MOUSE_REPORT_MOUSE,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0,
};

static const edr_sniff_par_t mouse_sniff_param = {
    .sniff_mode = SNIFF_MODE_DEF,
    .cnt_time = 1,
    .max_interval_slots = 24,
    .min_interval_slots = 24,
    .attempt_slots = 1,
    .timeout_slots = 0,
    .check_timer_period = 1000,
};

static const edr_init_cfg_t mouse_edr_config = {
    .class_type = 0x0025c0U,    /* peripheral: keyboard and pointing device */
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = sizeof(mouse_report_descriptor),
    .report_map = mouse_report_descriptor,
    .sniff_param = &mouse_sniff_param,
};

static u8 use_gyro;
static u8 keyboard_last[MOUSE_KEYBOARD_SIZE];
static u8 keyboard_sent;
static u8 buttons_last;
static u8 stick_keys_held[2];   /* bit per direction: up, right, down, left */
static u32 last_step_us;
static u8 have_step;
static s64 pointer_rest[2];     /* sub-count stick motion */
static s64 scroll_rest[2];
static s32 pending[2];          /* counts not yet in a report */
static s32 pending_wheel[2];
static u32 posted_seq;
static u32 posted_ms;
static u8 posted;

static u32 isqrt(u32 value)
{
    u32 root = 0U;
    u32 bit = 1U << 30;

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

static const struct mouse_layout *active_layout(void)
{
    u8 variant = config.profile[get_cur_bt_idx() % CONFIG_PROFILES].variant;

    return variant < ARRAY_SIZE(mouse_layouts) ? &mouse_layouts[variant] :
           &mouse_layouts[CONFIG_VARIANT_DEFAULT];
}

static u32 controls_of(const struct cyclone2_input_report *input)
{
    static const u32 hat_controls[CYCLONE2_HAT_NEUTRAL] = {
        CONTROL_UP, CONTROL_UP | CONTROL_RIGHT, CONTROL_RIGHT,
        CONTROL_DOWN | CONTROL_RIGHT, CONTROL_DOWN,
        CONTROL_DOWN | CONTROL_LEFT, CONTROL_LEFT, CONTROL_UP | CONTROL_LEFT,
    };
    u32 controls = input->buttons;

    if (input->left_trigger >= MOUSE_TRIGGER_PRESS) controls |= CONTROL_L2;
    if (input->right_trigger >= MOUSE_TRIGGER_PRESS) controls |= CONTROL_R2;
    if (input->hat < CYCLONE2_HAT_NEUTRAL) controls |= hat_controls[input->hat];
    return controls;
}

static void press_key(u8 report[MOUSE_KEYBOARD_SIZE], u8 key)
{
    u8 index;

    if (key == KB_NONE) return;
    if (key >= KB_LEFT_CTRL) {
        report[0] |= (u8)(1U << (key - KB_LEFT_CTRL));
        return;
    }
    for (index = 2U; index < MOUSE_KEYBOARD_SIZE; index++) {
        if (report[index] == key) return;
        if (report[index] == KB_NONE) {
            report[index] = key;
            return;
        }
    }
}

/* A stick as four keys, with hysteresis per direction. */
static void stick_keys(u8 report[MOUSE_KEYBOARD_SIZE], u8 *held, s16 x, s16 y,
                       const u8 keys[4])
{
    const s32 value[4] = { y, x, -(s32)y, -(s32)x };
    u8 direction;

    for (direction = 0U; direction < 4U; direction++) {
        u8 bit = (u8)(1U << direction);

        if (value[direction] >= MOUSE_MOVE_PRESS) *held |= bit;
        else if (value[direction] < MOUSE_MOVE_RELEASE) *held &= (u8)~bit;
        if (*held & bit) press_key(report, keys[direction]);
    }
}

/* The stick's motion over dt_us at `speed` counts per second all the way
 * over: speed follows the square of the deflection, for fine aim near the
 * centre.  The stick's y is up; the pointer's is down. */
static void stick_motion(s16 x, s16 y, u32 dt_us, u32 speed, s32 out[2])
{
    const s32 axis[2] = { x, -(s32)y };
    u32 reach = isqrt((u32)(axis[0] * axis[0]) + (u32)(axis[1] * axis[1]));
    const s64 per_count = (s64)CYCLONE2_STICK_MAX * CYCLONE2_STICK_MAX *
                          1000000;
    int index;

    if (reach > CYCLONE2_STICK_MAX) reach = CYCLONE2_STICK_MAX;
    for (index = 0; index < 2; index++) {
        s64 total = (s64)axis[index] * (s32)reach * speed * dt_us +
                    pointer_rest[index];

        out[index] = (s32)(total / per_count);
        pointer_rest[index] = total - (s64)out[index] * per_count;
    }
}

/* Wheel notches (up positive) and pan (right positive). */
static void scroll_motion(s16 x, s16 y, u32 dt_us, s32 out[2])
{
    const s32 axis[2] = { y, x };
    const s32 travel = CYCLONE2_STICK_MAX - MOUSE_SCROLL_DEADZONE;
    const s64 per_notch = (s64)travel * travel * 1000000;
    int index;

    for (index = 0; index < 2; index++) {
        s32 past = (axis[index] < 0 ? -axis[index] : axis[index]) -
                   MOUSE_SCROLL_DEADZONE;
        s64 rate;

        out[index] = 0;
        if (past <= 0) {
            scroll_rest[index] = 0;
            continue;
        }
        if (past > travel) past = travel;
        rate = (s64)past * past * MOUSE_SCROLL_PER_SECOND;
        scroll_rest[index] += (axis[index] < 0 ? -rate : rate) * dt_us;
        out[index] = (s32)(scroll_rest[index] / per_notch);
        scroll_rest[index] -= (s64)out[index] * per_notch;
    }
}

static s32 clamp(s32 value, s32 limit)
{
    return value > limit ? limit : value < -limit ? -limit : value;
}

static void mouse_output(u8 *packet, u16 size, u16 channel)
{
    /* Keyboard LEDs: nothing to show them on. */
    (void)packet;
    (void)size;
    (void)channel;
}

static void mouse_session_start(void)
{
    keyboard_sent = 0U;
    buttons_last = 0U;
    stick_keys_held[0] = stick_keys_held[1] = 0U;
    have_step = 0U;
    posted = 0U;
    pointer_rest[0] = pointer_rest[1] = 0;
    scroll_rest[0] = scroll_rest[1] = 0;
    pending[0] = pending[1] = 0;
    pending_wheel[0] = pending_wheel[1] = 0;
    cyclone2_gyro_reset();
    if (use_gyro) cyclone2_imu_enable_gyro();
}

static void mouse_session_exit(void)
{
    if (use_gyro) cyclone2_imu_disable();
}

static const struct cyclone2_classic_profile mouse_classic = {
    .local_name = "Cyclone 2 Keyboard Mouse",
    .edr = &mouse_edr_config,
    /* pid.codes' test product: no host driver claims it. */
    .vendor_id = 0x1209U,
    .product_id = 0x0001U,
    .version = 0x0100U,
    .output = mouse_output,
    .session_start = mouse_session_start,
    .link_up = mouse_session_start,
    .session_exit = mouse_session_exit,
};

void cyclone2_stickmouse_start(u8 slot, u8 pairing)
{
    use_gyro = 0U;
    cyclone2_classic_start(&mouse_classic, slot, pairing);
}

void cyclone2_gyromouse_start(u8 slot, u8 pairing)
{
    use_gyro = 1U;
    cyclone2_classic_start(&mouse_classic, slot, pairing);
}

/* One stick's part: keys into the keyboard report, motion into pending. */
static void apply_stick(u8 role, u8 index, s16 x, s16 y, u8 hold, u32 dt_us,
                        u32 speed, u8 keyboard[MOUSE_KEYBOARD_SIZE])
{
    s32 motion[2];

    if (role == STICK_POINTER && (hold & HOLD_SCROLL)) role = STICK_SCROLL;
    switch (role) {
    case STICK_POINTER:
        stick_motion(x, y, dt_us, hold & HOLD_SLOW ? speed / 4U : speed,
                     motion);
        pending[0] += motion[0];
        pending[1] += motion[1];
        break;
    case STICK_SCROLL:
        scroll_motion(x, y, dt_us, motion);
        pending_wheel[0] += motion[0];
        pending_wheel[1] += motion[1];
        break;
    case STICK_WASD:
        stick_keys(keyboard, &stick_keys_held[index], x, y, wasd_keys);
        break;
    case STICK_ARROWS:
        stick_keys(keyboard, &stick_keys_held[index], x, y, arrow_keys);
        break;
    }
}

/* The keyboard report goes in the queue when it changes: key presses are
 * events and must all arrive.  The mouse report carries relative motion, so
 * it may not replace one the link has not taken yet; motion is gathered
 * until the last one is gone.  Returns as cyclone2_profile_send()
 * documents. */
int cyclone2_mouse_send(const struct cyclone2_input_report *input)
{
    const struct config_profile *settings =
        &config.profile[get_cur_bt_idx() % CONFIG_PROFILES];
    const struct mouse_layout *layout = active_layout();
    u8 keyboard[MOUSE_KEYBOARD_SIZE];
    u8 mouse[MOUSE_MOUSE_SIZE];
    u8 buttons = 0U;
    u8 hold = 0U;
    u32 controls = controls_of(input);
    u32 now = sys_timer_get_ms();
    u32 now_us = cyclone2_time_us();
    u32 dt_us = 0U;
    u32 index;
    s32 x;
    s32 y;
    int result;

    if (!edr_hid_is_connected()) {
        keyboard_sent = 0U;
        posted = 0U;
        return 0;
    }
    if (have_step) dt_us = now_us - last_step_us;
    if (dt_us > MOUSE_MAX_DT_US) dt_us = MOUSE_MAX_DT_US;
    last_step_us = now_us;
    have_step = 1U;

    memset(keyboard, 0, sizeof(keyboard));
    for (index = 0U; index < layout->count; index++) {
        const struct mouse_binding *binding = &layout->bindings[index];

        if (!(controls & binding->control)) continue;
        press_key(keyboard, binding->key);
        buttons |= binding->mouse;
        hold |= binding->hold;
    }
    apply_stick(layout->left_stick, 0U, input->left_x, input->left_y, hold,
                dt_us, settings->stick_speed, keyboard);
    apply_stick(layout->right_stick, 1U, input->right_x, input->right_y, hold,
                dt_us, settings->stick_speed, keyboard);
    if (use_gyro) {
        /* Always taken, so turns made while the gyro is let go are
         * dropped. */
        cyclone2_gyro_take(settings->gyro_speed, settings->gyro_accel, &x, &y);
        if (!(hold & HOLD_GYRO_OFF)) {
            pending[0] += x;
            pending[1] += y;
        }
    }

    if (!keyboard_sent || memcmp(keyboard, keyboard_last, sizeof(keyboard))) {
        result = edr_hid_data_send(MOUSE_REPORT_KEYBOARD, keyboard,
                                   sizeof(keyboard));
        if (result == 3) return -1;
        if (result != 0) return -2;
        memcpy(keyboard_last, keyboard, sizeof(keyboard));
        keyboard_sent = 1U;
    }

    if (!pending[0] && !pending[1] && !pending_wheel[0] &&
        !pending_wheel[1] && buttons == buttons_last)
        return 0;
    if (posted &&
        (s32)(btstack_jl_core_input_taken() - posted_seq) < 0 &&
        now - posted_ms < MOUSE_TAKE_MAX_MS)
        return 0;

    x = clamp(pending[0], 32767);
    y = clamp(pending[1], 32767);
    mouse[0] = buttons;
    mouse[1] = (u8)x;
    mouse[2] = (u8)((u16)x >> 8);
    mouse[3] = (u8)y;
    mouse[4] = (u8)((u16)y >> 8);
    mouse[5] = (u8)(s8)clamp(pending_wheel[0], 127);
    mouse[6] = (u8)(s8)clamp(pending_wheel[1], 127);
    result = edr_hid_input_send(MOUSE_REPORT_MOUSE, mouse, sizeof(mouse));
    if (result == 3) return -1;
    if (result != 0) return -2;
    pending[0] -= x;
    pending[1] -= y;
    pending_wheel[0] -= (s8)mouse[5];
    pending_wheel[1] -= (s8)mouse[6];
    buttons_last = buttons;
    posted_seq = btstack_jl_core_input_posted();
    posted_ms = now;
    posted = 1U;
    return 1;
}
