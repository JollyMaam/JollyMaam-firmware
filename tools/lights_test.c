/* Host check for the light renderer (src/lights_render.c): what each
 * controller state shows, and the overlays on top. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "lights.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static const struct cyclone2_rgb blue = { 0x00, 0x00, 0xff };
static const struct cyclone2_rgb red = { 0xff, 0x00, 0x00 };
static const struct cyclone2_rgb purple = { 0x80, 0x00, 0xff };
static const struct cyclone2_rgb green = { 0x00, 0xff, 0x00 };
static const struct cyclone2_rgb dim_white = { 5, 5, 5 };
static const struct cyclone2_rgb dim_orange = { 5, 2, 0 };
static const struct cyclone2_rgb dark = { 0, 0, 0 };

static struct cyclone2_controller_state state;
static struct cyclone2_lights_input in;
static struct cyclone2_light_frame frame;

static int same(struct cyclone2_rgb a, struct cyclone2_rgb b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static struct cyclone2_rgb light(enum cyclone2_light which)
{
    return frame.light[which];
}

static struct cyclone2_rgb player(unsigned index)
{
    return frame.light[CYCLONE2_LIGHT_PLAYER_0 + index];
}

/* A fresh input, 10 s after boot, `t` ms into a state entered at 1000. */
static void reset(enum cyclone2_controller_kind kind, u8 slot)
{
    struct cyclone2_session session = { .slot = slot,
                                        .type = CYCLONE2_PROFILE_SWITCH };

    memset(&state, 0, sizeof(state));
    state.kind = kind;
    state.entered_ms = 1000U;
    switch (kind) {
    case CYCLONE2_CONTROLLER_CONNECTING: state.as.connecting.session = session; break;
    case CYCLONE2_CONTROLLER_PAIRING: state.as.pairing.session = session; break;
    case CYCLONE2_CONTROLLER_CONNECTED: state.as.connected.session = session; break;
    case CYCLONE2_CONTROLLER_DISCONNECTED: state.as.disconnected.session = session; break;
    case CYCLONE2_CONTROLLER_WAKING: state.as.waking.slot = slot; break;
    default: break;
    }
    memset(&in, 0, sizeof(in));
    in.state = &state;
    in.since_boot_ms = 10000U;
    in.slot_colour[0] = blue;
    in.slot_colour[1] = red;
    in.slot_colour[2] = purple;
    in.slot_colour[3] = green;
    in.idle_colour = dim_white;
    in.battery_colour = dim_orange;
    in.grip_mode = CONFIG_GRIPS_LIGHTBAR;
}

static void render_at(u32 t)
{
    in.now_ms = state.entered_ms + t;
    memset(&frame, 0xa5, sizeof(frame));
    cyclone2_lights_render(&in, &frame);
}

static void test_connecting(void)
{
    reset(CYCLONE2_CONTROLLER_CONNECTING, 0U);
    render_at(0U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), blue));      /* blinks, starting lit */
    CHECK(same(player(0), blue));                       /* slot A: top light */
    CHECK(same(player(1), dark) && same(player(2), dark) && same(player(3), dark));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark));
    CHECK(same(light(CYCLONE2_LIGHT_UNUSED), dark));
    render_at(600U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));
    CHECK(same(player(0), blue));

    reset(CYCLONE2_CONTROLLER_CONNECTING, 3U);
    render_at(0U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), green));
    CHECK(same(player(3), green) && same(player(0), dark));
}

static void test_pairing_sweeps(void)
{
    unsigned step;
    const unsigned expected[] = { 0, 1, 2, 3, 2, 1, 0, 1 };

    reset(CYCLONE2_CONTROLLER_PAIRING, 1U);
    for (step = 0U; step < sizeof(expected) / sizeof(expected[0]); step++) {
        unsigned index;

        render_at(step * 120U + 10U);
        for (index = 0U; index < 4U; index++)
            CHECK(same(player(index), index == expected[step] ? red : dark));
    }
}

static void test_connected(void)
{
    reset(CYCLONE2_CONTROLLER_CONNECTED, 2U);
    render_at(5000U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), purple));
    /* No player number from the host yet: the slot's own light. */
    CHECK(same(player(2), purple) && same(player(0), dark));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark));

    in.host.has_players = 1U;
    in.host.players_on = 0x3U;                          /* player 2 */
    render_at(5000U);
    CHECK(same(player(0), purple) && same(player(1), purple));
    CHECK(same(player(2), dark) && same(player(3), dark));

    /* Flashing lights follow the clock, 250 ms on and 250 ms off. */
    in.host.players_on = 0x1U;
    in.host.players_flash = 0x8U;
    render_at(500U);
    CHECK(same(player(3), purple));
    render_at(750U);
    CHECK(same(player(3), dark) && same(player(0), purple));
}

static void test_grips(void)
{
    const struct cyclone2_rgb orange = { 0xff, 0x80, 0x00 };

    reset(CYCLONE2_CONTROLLER_CONNECTED, 0U);
    in.rumble[0] = 255U;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark)); /* no host lightbar */
    in.grip_mode = CONFIG_GRIPS_BOTH;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), blue));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT), dark));
    in.rumble[0] = 0U;
    in.reach[1] = 128U;
    render_at(100U);
    CHECK(light(CYCLONE2_LIGHT_GRIP_RIGHT).blue == 128U);
    in.grip_mode = CONFIG_GRIPS_RUMBLE;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT), dark));

    in.host.has_lightbar = 1U;
    in.host.lightbar = orange;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark)); /* mode ignores host */
    in.grip_mode = CONFIG_GRIPS_LIGHTBAR;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), orange));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT), orange));
    CHECK(same(light(CYCLONE2_LIGHT_HOME), blue));      /* Home stays the slot's */
    in.grip_mode = CONFIG_GRIPS_NONE;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT), dark));
    in.grip_mode = CONFIG_GRIPS_LIGHTBAR;
    in.host.lightbar = dark;                            /* host switched it off */
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark));
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT), dark));

    /* Rumble and input only while connected. */
    reset(CYCLONE2_CONTROLLER_CONNECTING, 0U);
    in.grip_mode = CONFIG_GRIPS_BOTH;
    in.rumble[0] = in.reach[0] = 255U;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_LEFT), dark));
}

static void test_colour_values(void)
{
    reset(CYCLONE2_CONTROLLER_CONNECTED, 2U);
    in.slot_colour[2] = (struct cyclone2_rgb){ 3U, 0U, 5U };
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), in.slot_colour[2]));
    CHECK(same(player(2), in.slot_colour[2]));
    in.grip_mode = CONFIG_GRIPS_INPUT;
    in.reach[1] = 128U;
    render_at(100U);
    CHECK(same(light(CYCLONE2_LIGHT_GRIP_RIGHT),
               (struct cyclone2_rgb){ 2U, 0U, 3U }));
}

static void test_boot_idle_off(void)
{
    struct cyclone2_rgb home;

    reset(CYCLONE2_CONTROLLER_IDLE, 0U);
    in.since_boot_ms = 100U;
    render_at(0U);
    CHECK(cyclone2_scene_for(&state, 100U) == CYCLONE2_SCENE_BOOT);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dim_white));
    in.since_boot_ms = 10000U;
    render_at(0U);                                      /* breathing, from lit */
    home = light(CYCLONE2_LIGHT_HOME);
    CHECK(same(home, dim_white));
    render_at(2000U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));

    reset(CYCLONE2_CONTROLLER_STOPPING, 0U);
    state.as.stopping.after = CYCLONE2_AFTER_STOP_SLEEP;
    in.low_battery = 0U;
    render_at(0U);
    for (unsigned index = 0U; index < CYCLONE2_LIGHT_COUNT; index++)
        CHECK(same(frame.light[index], dark));
}

static void test_switching_shows_next_slot(void)
{
    reset(CYCLONE2_CONTROLLER_STOPPING, 0U);
    state.as.stopping.has_session = 1U;
    state.as.stopping.session.slot = 0U;
    state.as.stopping.after = CYCLONE2_AFTER_STOP_START;
    state.as.stopping.then.start.slot = 3U;
    render_at(10U);
    CHECK(same(player(3), green) && same(player(0), dark));
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));
}

static void test_overlays(void)
{
    const struct cyclone2_rgb calibration = { 0, 0, 12 };

    reset(CYCLONE2_CONTROLLER_CONNECTED, 0U);
    in.low_battery = 1U;
    in.now_ms = 3000U;
    memset(&frame, 0, sizeof(frame));
    cyclone2_lights_render(&in, &frame);                /* flash: orange */
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dim_orange));
    in.now_ms = 3500U;
    cyclone2_lights_render(&in, &frame);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), blue));

    in.has_calibration = 1U;
    in.calibration = calibration;
    cyclone2_lights_render(&in, &frame);                /* not scaled again */
    CHECK(same(light(CYCLONE2_LIGHT_HOME), calibration));
}

static void test_usb_overlays(void)
{
    reset(CYCLONE2_CONTROLLER_DISCONNECTED, 0U);
    in.low_battery = 1U;
    render_at(1400U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));

    in.usb_powered = 1U;
    in.low_battery = 0U;
    render_at(1400U);                              /* charging breath at peak */
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dim_orange));
    in.usb_plug_flash = 1U;
    render_at(1400U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), blue));
    in.usb_plug_flash = 2U;
    render_at(1400U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));

    reset(CYCLONE2_CONTROLLER_CONNECTING, 1U);
    in.usb_powered = 1U;
    render_at(600U);                               /* Home's usual blink is off */
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));
    in.usb_plug_flash = 1U;
    render_at(600U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), red));
    in.usb_plug_flash = 2U;
    render_at(600U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));

    reset(CYCLONE2_CONTROLLER_PAIRING, 1U);
    in.usb_powered = 1U;
    render_at(0U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), red));
    reset(CYCLONE2_CONTROLLER_CONNECTED, 1U);
    in.usb_powered = 1U;
    render_at(0U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), red));
    in.usb_plug_flash = 2U;
    render_at(0U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dark));

    reset(CYCLONE2_CONTROLLER_IDLE, 0U);
    in.usb_powered = 1U;
    render_at(1400U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dim_orange));
    in.usb_plug_flash = 1U;                     /* no slot: use idle colour */
    render_at(1400U);
    CHECK(same(light(CYCLONE2_LIGHT_HOME), dim_white));
}

static void test_disconnected_sleep_deadline(void)
{
    reset(CYCLONE2_CONTROLLER_DISCONNECTED, 0U);
    CHECK(!cyclone2_disconnected_sleep_due(&state, 15999U, 1000U, 0));
    CHECK(cyclone2_disconnected_sleep_due(&state, 16000U, 1000U, 0));
    CHECK(!cyclone2_disconnected_sleep_due(&state, 30000U, 1000U, 1));
    CHECK(!cyclone2_disconnected_sleep_due(&state, 24999U, 20000U, 0));
    CHECK(cyclone2_disconnected_sleep_due(&state, 25000U, 20000U, 0));
    state.kind = CYCLONE2_CONTROLLER_CONNECTED;
    CHECK(!cyclone2_disconnected_sleep_due(&state, 30000U, 1000U, 0));
}

/* Every kind has a scene, and nothing ever lights the unused position. */
static void test_every_state(void)
{
    enum cyclone2_controller_kind kind;
    u32 t;

    for (kind = 0; kind < CYCLONE2_CONTROLLER_KIND_COUNT; kind++) {
        reset(kind, 1U);
        CHECK(cyclone2_scenes[cyclone2_scene_for(&state, 10000U)].name != NULL);
        for (t = 0U; t < 5000U; t += 37U) {
            render_at(t);
            CHECK(same(light(CYCLONE2_LIGHT_UNUSED), dark));
        }
    }
}

int main(void)
{
    test_connecting();
    test_pairing_sweeps();
    test_connected();
    test_grips();
    test_colour_values();
    test_boot_idle_off();
    test_switching_shows_next_slot();
    test_overlays();
    test_usb_overlays();
    test_disconnected_sleep_deadline();
    test_every_state();
    if (failures) {
        printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("lights: all checks passed\n");
    return 0;
}
